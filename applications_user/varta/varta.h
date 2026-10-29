/**
 * @file varta.h
 * @brief Varta-M keys: what makes one, and how to build one from its public half.
 *
 * A key is a T5577 broadcasting an EM4100 ID, protected by one password shared by every key and
 * carrying one marker word in block 6. Neither is per-card, so the public ID is enough to build a
 * complete key.
 */
#pragma once

#include <lfrfid/tools/t5577.h>
#include <lfrfid/tools/t5577_file.h>
#include <toolbox/protocols/protocol_dict.h>

/** The word every Varta-M key is protected with.
 *
 * @warning Must never reach a tag that has no password. A passworded read and a plain write are
 * both 38 bits - see t5577_build_frame() - and the chip tells them apart by length alone, so an
 * unprotected tag parses the read as a write of `password << 1` into *the addressed block*, with
 * the password's bit 31 as that block's lock bit. Bit 31 is set here, and a T5577 lock bit is
 * one-way: the block can never be written again. Addressed to VARTA_MARKER_BLOCK, that is the
 * marker block locked with rubbish in it, and no wipe recovers it. Block 0 is untouched, so the
 * tag goes on working as a plain key - it just can never become a Varta one.
 */
#define VARTA_PASSWORD 0xFFA94A50UL

/** The word block 6 holds, which is what the reader asks for and compares. */
#define VARTA_MARKER 0xFFAD2D00UL

// Neither 0 nor 0xFFFFFFFF, so a captured word can be told from its own inverse - polarity is
// unknowable without a frame marker, so a self-complementary word could not be matched at all.
static_assert(
    VARTA_MARKER != 0 && VARTA_MARKER != 0xFFFFFFFFUL,
    "a marker that cannot be told from its own inverse cannot be verified");

#define VARTA_MARKER_BLOCK 6

/** Blocks varta_dump_from_id() fills. Checked there, so a screen listing them can assert
 * against this rather than trusting a comment to stay true.
 */
#define VARTA_KEY_BLOCK_COUNT 5

/** Block 0 as a key is found with: what an EM4100 at RF/64 asks for, plus the protection bit. */
#define VARTA_CONFIG (LFRFID_T5577_CONFIG_OPEN | LFRFID_T5577_PWD)

// The configuration read off a real key. The literal is where it came from; what this checks is
// that the named fields still add up to it, so retuning either is caught here and not on a tag.
static_assert(VARTA_CONFIG == 0x00148050UL, "block 0 of a Varta-M key");

// Where the marker sits decides whether any of this works, and getting it wrong is silent:
// t5577_dump_set() checks only that a block exists, so a marker block of 7 or 0 would be
// overwritten by the password or the config in varta_dump_from_id(), and one of 1 or 2 would
// overwrite the ID. Above the blocks VARTA_CONFIG broadcasts is also why the marker takes an
// addressed read at all, and why putting it there leaves the EM4100 frame alone. The max-block
// field is unpacked by hand because t5577_config_max_block() is a call, and a call is not an
// integer constant expression.
static_assert(
    VARTA_MARKER_BLOCK >
            ((VARTA_CONFIG >> LFRFID_T5577_MAXBLOCK_SHIFT) & LFRFID_T5577_MAXBLOCK_MASK) &&
        VARTA_MARKER_BLOCK < LFRFID_T5577_PASSWORD_BLOCK,
    "the marker belongs above the broadcast blocks and below the password");

/** Bytes of EM4100 ID, which is the only part of a key that varies.
 *
 * Coupled to EM4100's own decoded size through protocol_dict_get_data(), which furi_checks the
 * buffer against it - so shrinking this crashes a read rather than failing a build.
 */
#define VARTA_ID_SIZE 5

/** The prefixes seen on real keys, for a screen that has to name them. */
#define VARTA_PREFIX_TEXT "0300 or 0400"

/** What a screen says about an ID that is not one. Spelled once: it had already drifted
 * into a second wording across the three screens that have to say it.
 */
#define VARTA_PREFIX_WRONG  "It starts with\n" VARTA_PREFIX_TEXT
#define VARTA_PREFIX_NOT_EM "Only EM4100 keys\ncarry Varta IDs"
#define VARTA_PREFIX_WARN   "Varta keys start\n" VARTA_PREFIX_TEXT ".\nThis one does not."

/** Whether an EM4100 ID carries a prefix a Varta-M key is issued with.
 *
 * One of the two gates in front of VARTA_PASSWORD, and the only check an ID typed in or loaded
 * from a file can be given at all.
 */
bool varta_id_known(const uint8_t* id);

/** The two words a key broadcasts.
 *
 * What lets a read tell the tag's own ID frame from an answer to a command: a tag that ignored an
 * addressed read does not go quiet, it goes on modulating these.
 */
typedef struct {
    uint32_t block1;
    uint32_t block2;
} VartaBroadcast;

/** Work out what an ID broadcasts, once, for the reads that have to recognise it.
 *
 * @param dict  one holding every lfrfid protocol - the caller's, so nothing is allocated here
 */
void varta_broadcast(ProtocolDict* dict, const uint8_t* id, VartaBroadcast* broadcast);

/** Recover the ID a pair of broadcast blocks carries, the other way round from varta_broadcast().
 *
 * What a saved .t5577 dump holds is blocks, not an ID, so opening one means reading the EM4100
 * frame back out of blocks 1 and 2.
 *
 * Self-checking by construction: the five bytes are pulled out by EM4100's bit layout and then
 * re-encoded, and the result has to match the blocks exactly. The encoder fixes the header, both
 * parity rows and the stop bit, so a match proves the frame is a well-formed EM4100 one carrying
 * this ID - and a mistake in the layout here cannot come back as a plausible wrong ID, only as a
 * refusal.
 *
 * @param      dict as varta_broadcast()
 * @param[out] id   VARTA_ID_SIZE bytes, untouched unless this returns true
 * @return false if the blocks are not an EM4100 frame
 */
bool varta_id_from_broadcast(ProtocolDict* dict, const VartaBroadcast* broadcast, uint8_t* id);

/** Build a complete key from its public ID.
 *
 * Everything the ID does not give is constant, so this is the whole of what the protection costs
 * to reproduce. The result is a dump for t5577_file_save(), written back by the T5577 app rather
 * than here: t5577_restore_target() already puts the blocks in the order a protected key has to
 * be built in, with block 0 last.
 *
 * @param      dict  as varta_broadcast()
 * @param      id    VARTA_ID_SIZE bytes, any prefix - composing is arithmetic, and a user
 *                   who typed an unusual prefix has been warned and chose it
 * @param[out] dump  zeroed, then filled
 */
void varta_dump_from_id(ProtocolDict* dict, const uint8_t* id, T5577Dump* dump);

/** Which of a key's constant blocks a saved dump actually holds.
 *
 * Reported rather than required. A dump of an unprotected clone has the ID and none of the rest,
 * and that is precisely the dump worth opening - the app exists to turn a public ID back into a
 * complete key, so refusing to open one would be refusing the main use.
 *
 * @note These do NOT say the file came off a genuine key. varta_dump_from_id() fills blocks 0, 1,
 * 2, 6 and 7, so a dump this app saved reads back with all three set, exactly like one read off a
 * real tag. What they distinguish is a dump carrying a key's constant blocks from a source that
 * carries only an ID - a .rfid, a typed-in ID, or a clone.
 */
typedef struct {
    bool config; // block 0 is VARTA_CONFIG
    bool marker; // block 6 holds VARTA_MARKER
    bool password; // the file carries VARTA_PASSWORD, as block 7 or as its Password line
} VartaDumpParts;

/** Whether a saved dump yields a key at all. */
typedef enum {
    /** Blocks 1 and 2 gave an ID with a prefix Varta issues; VartaDumpParts says what else. */
    VartaDumpOk,
    /** The dump never held blocks 1 and 2.
     *
     * Kept apart from VartaDumpNotEm4100 because it is the likeliest bad file this app will
     * be handed and the only one with a remedy: a protected key refuses the plain read that a
     * detect opens with, so a dump taken without the password comes back empty rather than
     * wrong. "Read it again with the password" is worth saying; "this file is corrupt" is
     * both useless and false. It is the file-side twin of VartaVerdictNoTag.
     */
    VartaDumpNoBlocks,
    /** Blocks 1 and 2 are there, but together they are not an EM4100 frame. */
    VartaDumpNotEm4100,
    /** A well-formed EM4100 ID, but not a prefix Varta issues. `id` is still filled:
     * the ID is sound, only its prefix is unusual, and that is the user's call.
     */
    VartaDumpOtherId,
} VartaDumpVerdict;

/** What reading a saved dump came to: the verdict, and what it decided.
 *
 * One value rather than a verdict plus two out-params, so the three cannot be separated and
 * re-paired wrongly on the way through a call chain - the reason t5577_restore_target()
 * gives for the same shape.
 *
 * `id` and `parts` are filled whenever the blocks decoded, which includes VartaDumpOtherId:
 * an unusual prefix is a judgement about the ID, not a failure to read it. Both are zero on
 * the verdicts where there was nothing to read.
 */
typedef struct {
    VartaDumpVerdict verdict;
    uint8_t id[VARTA_ID_SIZE];
    VartaDumpParts parts;
} VartaDumpResult;

/** Read a saved T5577 dump as a Varta key.
 *
 * @param dict as varta_broadcast()
 */
VartaDumpResult varta_dump_read(ProtocolDict* dict, const T5577Dump* dump);

/** Whether a tag is still on the coil, asked without sending a password.
 *
 * A plain addressed read is six bits where a plain write is thirty-eight, so unlike a passworded
 * read it cannot be misparsed as a write by any tag - which makes this safe to send to anything.
 * What it looks for is the broadcast rather than an answer: a key goes on modulating its ID
 * whether or not it accepted the read, so this proves the tag is present and powered, and
 * nothing more.
 *
 * Asked between concluding "protected" and sending the password, because that conclusion is
 * drawn from silence and a tag that has been lifted off the coil is silent too.
 */
bool varta_tag_present(const VartaBroadcast* broadcast);

/** What asking a tag for its marker came to. */
typedef enum {
    /** Block 6 holds the marker: a genuine key. */
    VartaMarkerHeld,
    /** The tag answered the addressed read, with a different word. */
    VartaMarkerOther,
    /** Only the broadcast came back, so the read never landed and the password may well be
     * right. Retryable, and what a badly coupled key looks like. */
    VartaMarkerUnanswered,
    /** Nothing decodable at all: the tag has gone, or the field is dead. */
    VartaMarkerQuiet,
} VartaMarkerResult;

/** Ask a key for its marker, and say what came back.
 *
 * One addressed read. No detect first and no rate sweep: a Varta key's block 0 is known, so the
 * clock its answer arrives at is known too.
 *
 * Reads the stream rather than asking t5577_block_holds() to match the marker, because that
 * cannot tell an answer from the broadcast - VARTA_CONFIG leaves answer-on-request clear, so a
 * tag that refused the command keeps modulating its ID, and a plain match reports that as
 * "answered with a different word".
 *
 * @warning Sends VARTA_PASSWORD - see its warning. Only for a tag already shown to refuse a
 * plain read and still be on the coil.
 */
VartaMarkerResult varta_check_marker(const VartaBroadcast* broadcast);
