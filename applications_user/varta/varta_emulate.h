/**
 * @file varta_emulate.h
 * @brief Emulating a Varta key as the reader expects to find one: ID and marker, in bursts.
 *
 * A Varta reader does not read an ID and decide. It addresses page 0 block 6 and compares the
 * word, so emulation of the EM4100 ID alone - which is all lfrfid_worker_emulate_start() can
 * send - never opens one. What the reader needs is the word to APPEAR, not an answer in the gap;
 * that is why the rest of this file works without ever hearing the question.
 *
 * This is the other way at it. Rather than hearing the question and answering it - the gap
 * detector and the reply both want TIM2, and the device has exactly one - it never listens.
 * Both halves go on the coil in turn, so the reader finds whichever one it is asking for.
 *
 * Why bursts, and not the two simply appended. Appending fails twice over, and the same change
 * answers both:
 *
 * One marker word takes 16.4 ms at RF/64 and the reader's listening window, measured with no tag
 * present, is 13.76 ms. No phase lands a whole word inside it. What rescues this is the other
 * half of that measurement: with a real key present the reader STOPS interrogating - it reads,
 * then locks out. The field then stays up continuously and the window is no longer 13.76 ms. So
 * a marker needs the reader already latched, and latching needs a clean ID.
 *
 * And VARTA_MARKER begins with nine 1s, which is a valid EM4100 header. A marker spliced into
 * every ID frame gives the ID decoder a false frame start once per spliced frame, every 49 ms -
 * it should fail parity and resync, but it never gets a clean run to latch on either.
 *
 * Hence a long stretch of nothing but ID frames, then a stretch of nothing but markers. The
 * stretches are not synchronised to anything and do not need to be: the whole loop runs at a
 * different period from the reader's frame, so every alignment of burst against window gets
 * tried in turn.
 *
 * How long that takes is worth knowing before holding a Flipper to a door: the phase advances
 * by (loop mod frame) each time round, and two of the three mixes are near-resonant with the
 * reader, so they crawl. ID 8 + mark 1 sweeps in about 2 s; the default and ID 4 + mark 8
 * take 5 to 12 s depending on which estimate of the reader's frame you take. Give each mix
 * fifteen seconds before concluding anything.
 *
 * Measured, with the gap probe at applications_user/lfgap, on a real reader with no tag present:
 * it repeats a 38 bit frame 30 times a second, talking for 19.1 ms and listening for 13.76 ms.
 *
 * A Proxmark3 demodulates the marker stream bit-for-bit, so the word, the RF/64 rate, the
 * Manchester polarity and the modulation depth were all confirmed correct long before any of this
 * worked. What was never right was where the halves sat in time.
 *
 * Validated at a real Varta reader on 2026-10-26: it opens on ID 8 + mark 1, ID 8 + mark 4 and
 * ID 4 + mark 8, and does not open on the bare ID. Three stretch lengths working rules out a
 * lucky alignment; the control failing shows it is the marker and not the ID that opens it.
 * Neither separates bursts from the spliced design, which was never run at a door - the case
 * against splicing is the arithmetic above, not an experiment.
 */
#pragma once

#include <furi.h>
#include <toolbox/protocols/protocol_dict.h>

#include "varta.h"

/** Carrier cycles in half a bit at RF/64, which is the rate every Varta key is issued at. */
#define VARTA_EMULATE_HALFBIT 32

/** Cycles in one EM4100 frame: 64 bits, each of two half-bits. */
#define VARTA_EMULATE_EM4100_CYCLES (64UL * 2UL * VARTA_EMULATE_HALFBIT)

/** Longest burst of either half.
 *
 * Eight ID frames is 262 ms - eight reader frames, so the reader gets many clean looks at the
 * ID before the marker takes the coil back. Eight marker words is load-bearing too: the ID 4 +
 * mark 8 mix that opened a door uses all of them.
 *
 * Raise either and VARTA_EMULATE_MAX_RUNS follows on its own - it is derived from both, and the
 * derivation is what keeps it exactly tight. varta_emulate_start() furi_checks these two, so a
 * burst past them is a crash at the check rather than a silently truncated stream. The cost is
 * RAM, marginal rather than averaged: 1.5 KB per extra ID frame, 0.75 KB per extra marker word.
 */
#define VARTA_EMULATE_MAX_ID_FRAMES    8
#define VARTA_EMULATE_MAX_MARKER_WORDS 8

/** Most runs of one level the longest stream can hold: every half-bit could be its own run. */
#define VARTA_EMULATE_MAX_RUNS \
    (VARTA_EMULATE_MAX_ID_FRAMES * 128 + VARTA_EMULATE_MAX_MARKER_WORDS * 64)

/** Most high/low pairs, which is half the runs because each pair takes one of each. */
#define VARTA_EMULATE_MAX_PULSES (VARTA_EMULATE_MAX_RUNS / 2)

/** One mix of the two halves, as the screen offers them.
 *
 * Still a choice rather than one constant because the stretch lengths that work are a fact about
 * the reader - how long it needs to latch an ID, how long it then stays latched - and a different
 * installation may want the ID longer or the marker thicker.
 */
typedef struct {
    const char* name; // "Mode 1", as the settings list and README.md both call it
    uint8_t id_frames;
    uint8_t marker_words;
} VartaEmulateMix;

/** How many modes there are, for the settings list that offers them. */
size_t varta_emulate_mix_count(void);

const VartaEmulateMix* varta_emulate_mix(size_t index);

/** The mode chosen last time the app ran, or 0 for the default - no file yet, or a bad one. */
uint8_t varta_emulate_mix_load(void);

/** Remember the chosen mode.
 *
 * @return false if it did not reach the card, in which case the mode works for this run and is
 *         gone at the next launch - which the caller has to say, because nothing else will
 */
bool varta_emulate_mix_save(uint8_t mix);

typedef struct VartaEmulate VartaEmulate;

VartaEmulate* varta_emulate_alloc(void);

void varta_emulate_free(VartaEmulate* emulate);

/** Build the stream for an ID and put it on the coil.
 *
 * @param id_frames    EM4100 frames per loop, up to VARTA_EMULATE_MAX_ID_FRAMES
 * @param marker_words marker repeats per loop, up to VARTA_EMULATE_MAX_MARKER_WORDS
 * @return false if the mix could not be rendered, in which case nothing was started
 */
bool varta_emulate_start(
    VartaEmulate* emulate,
    ProtocolDict* dict,
    const uint8_t* id,
    uint8_t id_frames,
    uint8_t marker_words);

void varta_emulate_stop(VartaEmulate* emulate);
