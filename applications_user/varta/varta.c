#include "varta.h"

#include <furi.h>
#include <lfrfid/protocols/lfrfid_protocols.h>
#include <bit_lib.h>

#define TAG "Varta"

bool varta_id_known(const uint8_t* id) {
    // Both bytes, not just the first: 03 and 04 have each been seen, always followed by 00.
    static const uint8_t known[][2] = {
        {0x03, 0x00},
        {0x04, 0x00},
    };

    for(size_t i = 0; i < COUNT_OF(known); i++) {
        if(id[0] == known[i][0] && id[1] == known[i][1]) return true;
    }

    return false;
}

void varta_broadcast(ProtocolDict* dict, const uint8_t* id, VartaBroadcast* broadcast) {
    // EM4100's own encoder rather than a second copy of its parity layout here. The base variant
    // is the RF/64 one, which is the rate VARTA_CONFIG names - a key issued at any other rate
    // would not be read by the reader at all.
    protocol_dict_set_data(dict, LFRFIDProtocolEM4100, id, VARTA_ID_SIZE);

    LFRFIDWriteRequest request = {.write_type = LFRFIDWriteTypeT5577};
    const bool encoded = protocol_dict_get_write_data(dict, LFRFIDProtocolEM4100, &request);

    furi_check(encoded);

    // The encoder fills block 0 as well, with VARTA_CONFIG minus the protection bit - so the two
    // files' idea of a key's configuration is checked against each other here for free.
    furi_check((request.t5577.block[0] | LFRFID_T5577_PWD) == VARTA_CONFIG);

    broadcast->block1 = request.t5577.block[1];
    broadcast->block2 = request.t5577.block[2];
}

bool varta_id_from_broadcast(ProtocolDict* dict, const VartaBroadcast* broadcast, uint8_t* id) {
    const uint64_t frame = ((uint64_t)broadcast->block1 << 32) | broadcast->block2;

    // EM4100's frame, MSB first: nine header bits, then ten rows of four data bits and a row
    // parity, then four column parity bits and a stop bit. Two rows make a byte, high nibble
    // first. Only the data bits are taken; everything else the re-encode below checks for free.
    uint8_t decoded[VARTA_ID_SIZE] = {0};

    for(uint8_t row = 0; row < 10; row++) {
        const uint8_t shift = 51 - row * 5;
        const uint8_t nibble = (uint8_t)((frame >> shift) & 0xF);

        decoded[row / 2] = (uint8_t)((decoded[row / 2] << 4) | nibble);
    }

    VartaBroadcast check = {0};
    varta_broadcast(dict, decoded, &check);

    if(check.block1 != broadcast->block1 || check.block2 != broadcast->block2) {
        // The words themselves, because they are the whole evidence and appear on no screen:
        // without them "my dump of a real key says there is no ID in it" has nothing to go on.
        FURI_LOG_W(
            TAG,
            "blocks %08lX %08lX are not an EM4100 frame",
            (unsigned long)broadcast->block1,
            (unsigned long)broadcast->block2);
        return false;
    }

    memcpy(id, decoded, VARTA_ID_SIZE);
    return true;
}

static bool varta_dump_holds(const T5577Dump* dump, uint8_t block, uint32_t word) {
    return t5577_dump_was_read(dump, 0, block) && dump->block[0][block] == word;
}

// Either form counts: a dump composed from an ID carries the word in block 7 while deliberately
// claiming no working password - see varta_dump_from_id(). Same rule as t5577_restore_password(),
// which also lets block 7 outrank the Password line.
static bool varta_dump_holds_password(const T5577Dump* dump) {
    if(dump->has_password && dump->password == VARTA_PASSWORD) return true;

    return varta_dump_holds(dump, LFRFID_T5577_PASSWORD_BLOCK, VARTA_PASSWORD);
}

VartaDumpResult varta_dump_read(ProtocolDict* dict, const T5577Dump* dump) {
    VartaDumpResult result = {0};

    if(!t5577_dump_was_read(dump, 0, 1) || !t5577_dump_was_read(dump, 0, 2)) {
        result.verdict = VartaDumpNoBlocks;
        return result;
    }

    const VartaBroadcast broadcast = {
        .block1 = dump->block[0][1],
        .block2 = dump->block[0][2],
    };

    uint8_t decoded[VARTA_ID_SIZE] = {0};

    if(!varta_id_from_broadcast(dict, &broadcast, decoded)) {
        result.verdict = VartaDumpNotEm4100;
        return result;
    }

    // Filled before the prefix is judged: the frame decoded and re-encoded, so the ID is
    // sound whatever its first bytes say, and the caller may choose to use it anyway.
    memcpy(result.id, decoded, VARTA_ID_SIZE);

    // Before the verdict, like the ID: these describe the FILE, and the file holds what it
    // holds whatever its prefix turns out to be. Filling them only on Ok made the provenance
    // log report an empty dump for the one path it was added for.
    result.parts.config = varta_dump_holds(dump, 0, VARTA_CONFIG);
    result.parts.marker = varta_dump_holds(dump, VARTA_MARKER_BLOCK, VARTA_MARKER);
    result.parts.password = varta_dump_holds_password(dump);

    result.verdict = varta_id_known(decoded) ? VartaDumpOk : VartaDumpOtherId;

    return result;
}

void varta_dump_from_id(ProtocolDict* dict, const uint8_t* id, T5577Dump* dump) {
    memset(dump, 0, sizeof(*dump));

    VartaBroadcast broadcast = {0};
    varta_broadcast(dict, id, &broadcast);

    t5577_dump_set(dump, 0, 1, broadcast.block1);
    t5577_dump_set(dump, 0, 2, broadcast.block2);
    t5577_dump_set(dump, 0, VARTA_MARKER_BLOCK, VARTA_MARKER);
    t5577_dump_set(dump, 0, LFRFID_T5577_PASSWORD_BLOCK, VARTA_PASSWORD);
    t5577_dump_set(dump, 0, 0, VARTA_CONFIG);

    // Ties the block COUNT to the screen's row count, and only the count: add a block here
    // and this fires, bumping VARTA_KEY_BLOCK_COUNT then fails the row table's
    // static_assert. Swapping WHICH five blocks are written trips neither.
    furi_check(t5577_dump_blocks_read(dump) == VARTA_KEY_BLOCK_COUNT);

    // Blocks 3 to 5 and page 1 are left with no bit set, so a restore skips them: a key holds
    // zero in 3 to 5, page 1 blocks 1 and 2 are the traceability words a genuine chip refuses to
    // change, and nothing here knows what page 1 block 3 should be. A tag that was something else
    // before keeps whatever it had in those blocks, which the reader never looks at.

    // has_password is deliberately left false. What else it decides costs nothing here, because
    // block 7 carries the word: the saved file simply drops its Password line.
    //
    // For the write it is the only safe answer: t5577_restore_password() derives *with_pass from
    // this field for every write, and a passworded write is 70 bits where a blank tag expects 38
    // - not the 38-bit collision VARTA_PASSWORD warns about, but still not something to find out
    // on a key. False sends plain frames throughout, which is what a blank wants: blocks 1, 2, 6
    // and 7 go in while it is still open, and block 0 is the write that closes it.
    //
    // The read back afterwards is not this field's business: t5577_restore_password() derives
    // *with_pass there from the configuration block 0 just wrote, which is VARTA_CONFIG's PWD
    // bit, so it asks with the word block 7 carries and a key that landed verifies.
}

// Whether a 32 bit word turns up anywhere in a captured stream.
//
// Every alignment, because the answer repeats with no frame marker and where a word begins is
// unknown; both polarities, because which one the envelope detector settled on is unknowable for
// the same reason. Sliding a bit at a time can only delay a match, never invent one.
static bool varta_stream_holds(const uint8_t* bits, size_t count, uint32_t word) {
    for(size_t offset = 0; offset + LFRFID_T5577_WORD_BITS <= count; offset++) {
        const uint32_t window = bit_lib_get_bits_32(bits, offset, LFRFID_T5577_WORD_BITS);

        if(window == word || ~window == word) return true;
    }

    return false;
}

// How this app addresses a block, in one place so the two reads below cannot drift apart.
//
// NULL timings take the driver's defaults in t5577.c, which is the firmware's single real tuning
// point - the gaps and symbol lengths are antenna-dependent, which is why t5577.h takes them from
// the caller, so a key that will not answer is tuned by putting a T5577Timing here and nowhere
// else. settle_ms of zero likewise takes that default, which was chosen for RF/32's benefit, so a
// key read at RF/64 waits longer than it needs to; it is the other knob.
//
// The clock is derived from the configuration rather than named, so the rate a read listens at
// and the block 0 a composed key carries cannot disagree.
static T5577ReadRequest varta_request(uint8_t block, bool with_pass) {
    return (T5577ReadRequest){
        .downlink = T5577DownlinkFixedBitLength,
        .timing = NULL,
        .clock = t5577_read_clock_from_config(VARTA_CONFIG),
        .settle_ms = 0,
        .password = VARTA_PASSWORD,
        .page = 0,
        .block = block,
        .with_pass = with_pass,
    };
}

bool varta_tag_present(const VartaBroadcast* broadcast) {
    // Plain, so six bits on the wire where a plain write is thirty-eight: no tag can take this
    // for a write, which is what makes it safe to send before anything is known.
    const T5577ReadRequest request = varta_request(1, false);

    // Block 1's word is in the stream either way - an open tag answers with it, a protected tag
    // refuses and keeps broadcasting it - so a match means the tag is powered and nothing more.
    // That is exactly what t5577_block_holds() matches, and an EM4100 word can be neither 0 nor
    // 0xFFFFFFFF (every row carries a parity bit, so no row is all ones), so it never comes back
    // T5577BlockCannotVerify.
    const bool present = t5577_block_holds(&request, broadcast->block1) == T5577BlockHeld;

    FURI_LOG_I(TAG, "Presence check: %s", present ? "there" : "gone");

    return present;
}

VartaMarkerResult varta_check_marker(const VartaBroadcast* broadcast) {
    // On the stack as the library's own readers keep theirs; t5577_block_read_bits() puts its
    // capture on the heap, so this path is nowhere near the limit.
    uint8_t bits[LFRFID_T5577_READ_BITS_BYTES];

    FURI_LOG_I(TAG, "Sending %08lX to block %u", (uint32_t)VARTA_PASSWORD, VARTA_MARKER_BLOCK);

    const T5577ReadRequest request = varta_request(VARTA_MARKER_BLOCK, true);
    const size_t count = t5577_block_read_bits(&request, bits, LFRFID_T5577_READ_BITS_MAX, NULL);

    VartaMarkerResult result;

    if(count < LFRFID_T5577_WORD_BITS) {
        result = VartaMarkerQuiet;
    } else if(varta_stream_holds(bits, count, VARTA_MARKER)) {
        // Before the broadcast test, so a marker that happened to equal one of the ID words is
        // still read as the answer it is.
        result = VartaMarkerHeld;
    } else if(
        varta_stream_holds(bits, count, broadcast->block1) ||
        varta_stream_holds(bits, count, broadcast->block2)) {
        result = VartaMarkerUnanswered;
    } else {
        result = VartaMarkerOther;
    }

    FURI_LOG_I(TAG, "Marker read: %u bits -> %d", (unsigned)count, result);

    return result;
}
