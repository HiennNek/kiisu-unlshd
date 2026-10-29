/**
 * @file varta_reader.h
 * @brief What a reader is asking for, read off its own downlink.
 *
 * A 125 kHz reader sends commands by cutting gaps into its carrier, and the field-on time between
 * two gaps is the bit. Nothing in the firmware demodulates that and the envelope comparator
 * cannot see it at all - with no field of our own it self-oscillates near the threshold - so this
 * takes the digital route instead: TIM2's external trigger is wired to the carrier, so the
 * carrier RESETS the timer rather than clocking it. While the field runs, an edge every 8 us
 * keeps the timer from ever reaching its reload. It only fires when the carrier stops, which is
 * exactly a gap, with no threshold to fool.
 *
 * That detector is the one proven in applications_user/lfgap; this adds the decode.
 *
 * Measured on a Varta reader, 2026-10-27: symbols fall in two classes ending at 370 us and
 * starting at 607, with 237 us of empty space between them. A 12 second capture gave 15 frames
 * with a longest agreeing run of 7, class means 349 and 636 us against the 480 us split, and
 * every agreeing frame decoded to a page-0 passworded read of block 6 with password FFA94A50 -
 * the first direct confirmation of what the rest of this app had only inferred from which tags
 * open a door.
 *
 * @warning Listening only. Nothing here energises the coil or sends a frame, so no password can
 * reach a tag by this path - which is what makes pointing it at an unknown reader safe.
 */
#pragma once

#include <furi.h>

/** Bits in a passworded read: opcode, password, lock bit, block address. */
#define VARTA_READER_FRAME_BITS 38

/** Frames that must agree before a verdict is believed.
 *
 * One frame proves nothing about whether a command is fixed, and a reader at the edge of range
 * produces frames that differ. Agreement across several is what separates "decoded" from
 * "decoded something".
 */
#define VARTA_READER_FRAMES_AGREE 6

/** Where one symbol class ends and the other begins, in microseconds.
 *
 * Measured, not chosen: the two classes are 237 us apart and nothing lands between them. The gap
 * probe originally split at 350, which is inside the short class - about half the short symbols
 * counted as long, and the resulting histogram described a reader that does not exist.
 */
#define VARTA_READER_SPLIT_US 480

/** What the reader turned out to be. */
typedef enum {
    /** Nothing on the coil: no carrier at all. */
    VartaReaderNoField,
    /** Nothing this decoder could use - and not a claim that a carrier was there at all, since
     * one stray in-band stall is enough to rule out VartaReaderNoField. An observation, never a
     * verdict about the reader's security: one whose gaps fall outside the measured band is
     * silent to this and protected all the same. There is deliberately no verdict for
     * "unprotected" - see varta_reader_read() for why the measurements will not carry one.
     */
    VartaReaderQuiet,
    /** Gaps that never resolved into a frame this understands - including a whole frame that
     * resolved and then failed the opcode or lock-bit gate.
     */
    VartaReaderUnknown,
    // Not ordered by anything. Nothing compares these, and `>= VartaReaderOther` happening to
    // mean "decoded" today is the respelling varta_reader_decoded() exists to prevent.
    /** A 38-bit frame with a read opcode and a clear lock bit, addressed somewhere other than
     * VARTA_MARKER_BLOCK or asking a password other than VARTA_PASSWORD.
     *
     * The canonical statement of the ambiguity this app repeats elsewhere: a plain WRITE is also
     * 38 bits, and the lock bit checked here is a data bit in one, so `password` may be a word
     * this reader writes into tags rather than one it asks for.
     */
    VartaReaderOther,
    /** A page-0 passworded read of block 6 with the Varta password. */
    VartaReaderVarta,
} VartaReaderVerdict;

/** What was read off the reader, and the evidence for it. */
typedef struct {
    VartaReaderVerdict verdict;

    // What the reader asked for, filled exactly when varta_reader_decoded() is true -
    // printing bits where they would have been is how a failed decode gets believed.
    uint32_t password;
    uint8_t block;
    bool page1; // the opcode selects page 1 rather than page 0

    // The evidence, for a screen that has to explain a verdict it could not reach.
    uint8_t bits; // symbols in the last whole frame, which need not be the one decoded
    uint8_t agreed; // longest run of consecutive frames that decoded the same
    uint32_t frames; // whole frames seen
    uint32_t gaps; // carrier stalls in the band, decoded or not
    uint32_t stalls; // every completed stall, including the ones the band rejected
    uint8_t symbol_burst; // longest in-band run, frame or not and including the one still in
        // flight, up to the store's cap
    bool symbol_burst_capped; // it hit that cap, so the real run was at least that long
    uint32_t short_us; // mean of each symbol class, for checking the split still separates them
    uint32_t long_us;
} VartaReaderResult;

typedef struct VartaReader VartaReader;

VartaReader* varta_reader_alloc(void);

void varta_reader_free(VartaReader* reader);

/** Start listening. Claims TIM2, so nothing may emulate while this runs. */
void varta_reader_start(VartaReader* reader);

void varta_reader_stop(VartaReader* reader);

/** What has been heard so far. Safe to call while listening, and what supports a verdict is
 * never retracted by listening on: agreed, burst and the decoded command are the best of the
 * agreed, symbol_burst and the decoded command are the best of the run, so a caller can stop
 * as soon as varta_reader_confirmed() is true. bits is the last whole
 * frame's length, and short_us/long_us are running means over the whole window.
 */
void varta_reader_read(const VartaReader* reader, VartaReaderResult* result);

/** Whether a frame decoded, which is what makes password, block and page1 mean anything. */
static inline bool varta_reader_decoded(const VartaReaderResult* result) {
    return result->verdict == VartaReaderVarta || result->verdict == VartaReaderOther;
}

/** Whether enough frames agreed to believe the verdict.
 *
 * The listening scene stops as soon as this is true; its timeout path does not, so a
 * verdict can be shown on one frame. A screen that states a reader's security has to be
 * able to say which it is - the comparison lives here so it cannot be respelled.
 */
static inline bool varta_reader_confirmed(const VartaReaderResult* result) {
    return result->agreed >= VARTA_READER_FRAMES_AGREE;
}
