#include "varta_reader.h"

#include <furi_hal.h>
#include <stm32wbxx_ll_tim.h>

#include "varta.h"

/** Symbols a burst needs before it counts as a frame at all.
 *
 * Six is the shortest ADDRESSED read: opcode, lock and block. A reset or a regular read is
 * two bits and will not be counted, which is the right trade.
 * Below that a burst is one or two stray stalls with quiet either side - which a carrier
 * with a little ripple produces constantly. Counting those as frames made a Flipper doing
 * an EM4100 read, and a Proxmark3, both report an undecodable command where the truth is
 * that neither of them says anything at all.
 */
#define VARTA_READER_FRAME_MIN 6

/** Symbols in the longest burst below which nothing structured was heard.
 *
 * Not a measurement of ripple - a Flipper reading a tag scored 2 but an EM4100-only reader
 * scored 5. It is the length below which a burst tells us nothing either way, and what matters
 * is the direction of its error: at marginal coupling a missed gap stretches an interval past
 * PAUSE_US and chops a real 38-symbol frame into sub-6 pieces, so a protected reader measures 5
 * with 132 in-band gaps. Catching that as "could not decode" is honest; letting it fall through
 * to a verdict that says nothing was said calls an interrogating reader silent.
 */
#define VARTA_READER_QUIET_BURST 3
static_assert(
    VARTA_READER_QUIET_BURST < VARTA_READER_FRAME_MIN,
    "a burst of FRAME_MIN already counted a frame, so this guard would never run");

/** A stall longer than this is a gap rather than the time between two carrier edges.
 *
 * One carrier period is 8 us and a T55x7 gap is ten to twelve of them, so anything from about
 * 16 us up separates them. 20 leaves room for a carrier that is off frequency.
 */
#define VARTA_READER_THRESHOLD_US 20

/** The width a downlink gap falls in. Spec says 80-96 us; measured 40-199 on a real reader. */
#define VARTA_READER_BAND_MIN_US 40
#define VARTA_READER_BAND_MAX_US 200

/** Two stalls closer than this are one gap counted twice.
 *
 * The carrier does not return cleanly - on the way up it wobbles enough to stall the counter a
 * second time, which showed up as roughly one spurious gap per bit. The shortest real symbol is
 * several times this, so folding them cannot swallow a bit.
 */
#define VARTA_READER_DEBOUNCE_US 50

/** Symbols kept per frame. Past the frame length means the boundary is not where we
 * think it is, so there is nothing to gain by storing more of it.
 */
#define VARTA_READER_FRAME_MAX 48

/** A silence this long ends a frame. Bit periods stop below 1 ms and the reader's silent
 * window measured 13.76 ms, so this sits in empty space with room either side.
 */
#define VARTA_READER_PAUSE_US 2000

struct VartaReader {
    // Volatile throughout, including the fields only the handler touches. Narrowing it to the
    // fields that cross the boundary is defensible on paper and was tried; the detector stopped
    // behaving and reading the code did not say why, so it stays as the version that decoded a
    // real reader six frames out of six.
    volatile uint32_t gap_began_at; // raw DWT count when the stall was declared
    volatile uint32_t last_began_at; // raw DWT count of the previous kept gap
    volatile bool have_previous;
    volatile bool in_gap;
    volatile bool running;
    // Set when the field stayed up longer than anything inside a frame, which a floating pin
    // cannot fake: a stray edge resets the timer once and it overflows 20 us later, while a real
    // carrier holds it reset for milliseconds. A plain "a trigger happened" was tried and is
    // wrong - open air produces 120 to 260 stray edges a second, so it latched immediately.
    volatile bool carrier_seen;
    volatile uint32_t carrier_at; // raw DWT count of the edge that ended the last stall
    volatile bool have_carrier_at;

    volatile uint16_t period_us[VARTA_READER_FRAME_MAX];
    volatile uint8_t periods;
    // Set at the first frame boundary: everything before it is the tail of a command already in
    // flight when listening began, and a partial frame decoded as whole is how a wrong model
    // gets confirmed.
    volatile bool aligned;

    // The previous full-length frame, kept to compare the next one against; 32 bits would
    // drop the opcode and the password's top four. ISR-private - what gets reported is the
    // pair below, because the latest frame need not be the best one.
    volatile uint64_t bits_decoded;
    volatile uint8_t agreed; // the run in progress, which a junk frame resets
    volatile uint8_t last_frame_bits; // evidence only: how long the last whole frame was

    // The best run and the frame it agreed on, written together so they cannot describe two
    // different runs. Reporting the high-water agreement beside the latest frame's bits was a
    // verdict flip waiting to happen: six agreements and then one malformed full-length frame.
    volatile uint64_t best_bits;
    volatile uint8_t best_agreed;
    volatile bool have_decode; // best_bits holds a frame rather than its zero value

    volatile uint32_t frames;
    volatile uint32_t gaps;
    // Every completed stall, in band or not. Evidence, never a verdict: a reader whose gaps fall
    // outside the band left the whole screen reading zero, and ripple has to be visible without
    // being counted as something said.
    volatile uint32_t stalls;
    volatile uint8_t symbol_burst; // longest run of in-band symbols between two pauses

    volatile uint32_t short_count;
    volatile uint32_t short_sum_us;
    volatile uint32_t long_count;
    volatile uint32_t long_sum_us;

    uint32_t cycles_per_us;
};

// ---------------------------------------------------------------- the measurement

// A frame is 38 symbols; anything else is a boundary this did not find cleanly. Packed MSB
// first, which is the order a T5577 downlink sends.
static void varta_reader_frame_done(VartaReader* reader) {
    // One load apiece: every field here is volatile, so each mention is a fresh fetch from SRAM.
    const uint8_t periods = reader->periods;

    // Kept even for bursts thrown away below - see VARTA_READER_QUIET_BURST.
    if(periods > reader->symbol_burst) reader->symbol_burst = periods;

    // Too short to be any command, so it is a stall and some quiet rather than a frame. Not
    // counted, because "frames seen" is what separates a reader that talks from one that only
    // energises, and a ripple in a carrier is not talking.
    if(periods < VARTA_READER_FRAME_MIN) {
        reader->periods = 0;
        return;
    }

    reader->frames++;

    if(periods == VARTA_READER_FRAME_BITS) {
        uint64_t bits = 0;

        for(uint8_t at = 0; at < VARTA_READER_FRAME_BITS; at++) {
            // Long means one - see VARTA_READER_SPLIT_US for why the threshold is where it is.
            bits = (bits << 1) | (reader->period_us[at] > VARTA_READER_SPLIT_US ? 1u : 0u);
        }

        // have_decode, not a length: on the first frame bits_decoded is still its zero value,
        // and an all-short frame decodes to zero too, so the two would agree with each other.
        const uint8_t agreed =
            (reader->have_decode && bits == reader->bits_decoded) ? reader->agreed + 1 : 1;

        reader->agreed = agreed;
        reader->bits_decoded = bits;
        reader->last_frame_bits = VARTA_READER_FRAME_BITS;

        if(agreed > reader->best_agreed) {
            reader->best_agreed = agreed;
            reader->best_bits = bits;
            reader->have_decode = true;
        }
    } else {
        // A frame of the wrong length breaks the run: agreement has to mean consecutive. It does
        // not erase what was decoded - best_bits and best_agreed are left alone, so one junk
        // burst at the deadline cannot turn three hundred agreeing reads into nothing.
        reader->agreed = 0;
        reader->last_frame_bits = periods;
    }

    reader->periods = 0;
}

static void varta_reader_account(VartaReader* reader, uint32_t began_at, uint32_t ended_at) {
    const uint32_t cycles_per_us = reader->cycles_per_us;

    // Before the band test, so a reader this cannot measure is still visible as something.
    reader->stalls++;

    // Clustering - the longest run of stalls closer together than a frame gap - was measured
    // here and has been removed. It does not discriminate: open air clustered at 6, a Flipper
    // doing an LF read at 81, an EM4100-only reader at 22, which is bursty noise against even
    // noise rather than speech against silence. See the clustering fields in commit a374bd0 if
    // it is ever worth re-measuring.

    // Plus the threshold: the stall was already that old when the timer reached reload and told
    // us about it, so without it every width would read 20 us short of the band it is tested on.
    const uint32_t length_us = ((ended_at - began_at) / cycles_per_us) + VARTA_READER_THRESHOLD_US;

    if(length_us < VARTA_READER_BAND_MIN_US || length_us > VARTA_READER_BAND_MAX_US) return;

    reader->gaps++;

    // The first gap has no predecessor, so there is no period to measure from it yet.
    if(!reader->have_previous) {
        reader->last_began_at = began_at;
        reader->have_previous = true;
        return;
    }

    const uint32_t since_previous = began_at - reader->last_began_at;

    // A second stall inside the debounce is the same gap: it must not become a symbol and must
    // not move the anchor, since the next real period is still measured from the gap that began.
    // Tested on raw cycles, before the divide: about half of all in-band stalls are these, and
    // `delta / cpu < 50` is exactly `delta < 50 * cpu` for unsigned truncating division.
    if(since_previous < VARTA_READER_DEBOUNCE_US * cycles_per_us) return;

    reader->last_began_at = began_at;

    const uint32_t period_us = since_previous / cycles_per_us;

    if(period_us >= VARTA_READER_PAUSE_US) {
        if(reader->aligned) {
            varta_reader_frame_done(reader);
        } else {
            reader->aligned = true;
            reader->periods = 0;
        }
        return;
    }

    if(period_us < VARTA_READER_SPLIT_US) {
        reader->short_count++;
        reader->short_sum_us += period_us;
    } else {
        reader->long_count++;
        reader->long_sum_us += period_us;
    }

    if(!reader->aligned) return;

    // Longer than any frame this understands is simply not stored; the count stops at the cap,
    // so the next boundary is decoded against a buffer that did not overflow.
    const uint8_t periods = reader->periods;

    if(periods < VARTA_READER_FRAME_MAX) {
        reader->period_us[periods] = (uint16_t)period_us;
        reader->periods = periods + 1;
    }
}

static void varta_reader_isr(void* context) {
    VartaReader* reader = context;

    // The carrier stopped: the reload was reached with no edge to reset it.
    if(LL_TIM_IsActiveFlag_UPDATE(TIM2) && LL_TIM_IsEnabledIT_UPDATE(TIM2)) {
        LL_TIM_ClearFlag_UPDATE(TIM2);

        const uint32_t now = DWT->CYCCNT;

        // The field was up from the trigger that ended the last stall until now. Long enough
        // and there is a carrier, whatever else this run fails to decode.
        if(reader->have_carrier_at &&
           (now - reader->carrier_at) > VARTA_READER_PAUSE_US * reader->cycles_per_us) {
            reader->carrier_seen = true;
        }

        reader->gap_began_at = now;
        reader->in_gap = true;

        // Hand over to the trigger, which is the edge that ends this gap. Both armed at once
        // would be an interrupt per carrier edge - 125,000 a second.
        LL_TIM_DisableIT_UPDATE(TIM2);
        LL_TIM_ClearFlag_TRIG(TIM2);
        LL_TIM_EnableIT_TRIG(TIM2);
    }

    // The carrier came back, resetting the timer and raising the trigger flag.
    if(LL_TIM_IsActiveFlag_TRIG(TIM2) && LL_TIM_IsEnabledIT_TRIG(TIM2)) {
        LL_TIM_ClearFlag_TRIG(TIM2);

        const uint32_t now = DWT->CYCCNT;

        if(reader->in_gap) {
            varta_reader_account(reader, reader->gap_began_at, now);
            reader->in_gap = false;
        }

        // Where the field came back up. What it is worth is decided at the next stall, by how
        // long it stayed.
        reader->carrier_at = now;
        reader->have_carrier_at = true;

        LL_TIM_DisableIT_TRIG(TIM2);
        // The reset that raised the trigger can raise the update flag with it; starting the next
        // stall from a flag this gap set would report a gap that never happened.
        LL_TIM_ClearFlag_UPDATE(TIM2);
        LL_TIM_EnableIT_UPDATE(TIM2);
    }
}

// Carrier onto TIM2's external trigger, and everything that could drive the antenna held low -
// the pin setup a field detect uses, which is not exported to an app.
static void varta_reader_pins(void) {
    furi_hal_ibutton_pin_configure();
    furi_hal_ibutton_pin_write(false);

    furi_hal_gpio_init(&gpio_nfc_irq_rfid_pull, GpioModeOutputPushPull, GpioPullNo, GpioSpeedLow);
    furi_hal_gpio_write(&gpio_nfc_irq_rfid_pull, false);

    furi_hal_gpio_init(&gpio_rfid_carrier_out, GpioModeOutputPushPull, GpioPullNo, GpioSpeedLow);
    furi_hal_gpio_write(&gpio_rfid_carrier_out, false);

    furi_hal_gpio_init_ex(
        &gpio_rfid_carrier, GpioModeAltFunctionPushPull, GpioPullNo, GpioSpeedLow, GpioAltFn2TIM2);
}

// ---------------------------------------------------------------- the handle

VartaReader* varta_reader_alloc(void) {
    VartaReader* reader = malloc(sizeof(VartaReader));

    // Zeroed rather than left to the allocator: the heap only zeroes because this target happens
    // to configure it to, and read() before the first start() would otherwise assemble a verdict
    // - password and all - out of whatever the last owner of this memory left behind.
    memset(reader, 0, sizeof(*reader));
    reader->cycles_per_us = furi_hal_cortex_instructions_per_microsecond();

    return reader;
}

void varta_reader_free(VartaReader* reader) {
    furi_check(reader);

    varta_reader_stop(reader);
    free(reader);
}

void varta_reader_start(VartaReader* reader) {
    furi_check(reader);
    furi_check(!reader->running);

    varta_reader_pins();
    furi_hal_bus_enable(FuriHalBusTIM2);

    // Microseconds from the system clock, reloading at the threshold.
    LL_TIM_SetPrescaler(TIM2, reader->cycles_per_us - 1);
    LL_TIM_SetCounterMode(TIM2, LL_TIM_COUNTERMODE_UP);
    LL_TIM_SetAutoReload(TIM2, VARTA_READER_THRESHOLD_US - 1);
    LL_TIM_SetClockSource(TIM2, LL_TIM_CLOCKSOURCE_INTERNAL);

    // Only an overflow counts as an update. The reset default also raises UPDATE whenever the
    // slave mode controller reloads the counter - which here is every carrier edge, so the
    // detector read a 125 kHz carrier as 113k stalls a second. The flag that must mean "the
    // carrier stopped" cannot also mean "the carrier arrived".
    LL_TIM_SetUpdateSource(TIM2, LL_TIM_UPDATESOURCE_COUNTER);

    // The carrier resets the count. Inverted polarity and no filter, exactly as the field detect
    // configures the same pin - a filter would swallow the edges this depends on.
    LL_TIM_ConfigETR(
        TIM2, LL_TIM_ETR_POLARITY_INVERTED, LL_TIM_ETR_PRESCALER_DIV1, LL_TIM_ETR_FILTER_FDIV1);
    LL_TIM_SetTriggerInput(TIM2, LL_TIM_TS_ETRF);
    LL_TIM_SetSlaveMode(TIM2, LL_TIM_SLAVEMODE_RESET);

    // Field by field, under a critical section. A compound literal naming only what survives
    // is tidier and was tried; it went back because the detector misbehaved after the tidy pass
    // and this is the form that was validated on hardware.
    FURI_CRITICAL_ENTER();
    reader->in_gap = false;
    reader->carrier_seen = false;
    reader->have_carrier_at = false;
    reader->have_previous = false;
    reader->aligned = false;
    reader->periods = 0;
    reader->bits_decoded = 0;
    reader->agreed = 0;
    reader->last_frame_bits = 0;
    reader->best_bits = 0;
    reader->best_agreed = 0;
    reader->have_decode = false;
    reader->frames = 0;
    reader->gaps = 0;
    reader->stalls = 0;
    reader->symbol_burst = 0;
    reader->short_count = 0;
    reader->short_sum_us = 0;
    reader->long_count = 0;
    reader->long_sum_us = 0;
    FURI_CRITICAL_EXIT();

    furi_hal_interrupt_set_isr(FuriHalInterruptIdTIM2, varta_reader_isr, reader);

    LL_TIM_ClearFlag_UPDATE(TIM2);
    LL_TIM_ClearFlag_TRIG(TIM2);
    LL_TIM_EnableIT_UPDATE(TIM2);
    LL_TIM_EnableCounter(TIM2);

    reader->running = true;
}

void varta_reader_stop(VartaReader* reader) {
    furi_check(reader);
    if(!reader->running) return;

    LL_TIM_DisableIT_UPDATE(TIM2);
    LL_TIM_DisableIT_TRIG(TIM2);
    LL_TIM_DisableCounter(TIM2);

    // Close a run the window interrupted. frame_done() fires at the NEXT pause, so a complete
    // frame still waiting for one was left in period_us[] and reported as "could not decode".
    // Safe here: the counter and both interrupts are off, so nothing else touches the state. The
    // exact-length gate means this can only add a decode - never move a count or break a run.
    if(reader->aligned && reader->periods == VARTA_READER_FRAME_BITS) {
        varta_reader_frame_done(reader);
    }

    furi_hal_interrupt_set_isr(FuriHalInterruptIdTIM2, NULL, NULL);
    furi_hal_bus_disable(FuriHalBusTIM2);

    // All four pins, not just the carrier input: start() drives the iButton pad, the NFC pull and
    // the carrier output low, and leaving the scene used to leave them that way - a 1-Wire pad
    // shorted to ground for as long as the app stayed open.
    furi_hal_rfid_pins_reset();

    reader->running = false;
}

void varta_reader_read(const VartaReader* reader, VartaReaderResult* result) {
    furi_check(reader);
    furi_check(result);

    memset(result, 0, sizeof(*result));

    // One copy under a critical section: a verdict assembled from fields read either side of an
    // interrupt would describe two different moments.
    FURI_CRITICAL_ENTER();
    const uint64_t bits = reader->best_bits;
    const bool have_decode = reader->have_decode;
    const bool carrier_seen = reader->carrier_seen;
    result->bits = reader->last_frame_bits;
    result->agreed = reader->best_agreed;
    result->frames = reader->frames;
    result->gaps = reader->gaps;
    result->stalls = reader->stalls;
    result->symbol_burst = reader->symbol_burst;
    const uint8_t pending = reader->periods;
    const uint32_t short_count = reader->short_count;
    const uint32_t short_sum = reader->short_sum_us;
    const uint32_t long_count = reader->long_count;
    const uint32_t long_sum = reader->long_sum_us;
    const bool stalled = reader->in_gap;
    FURI_CRITICAL_EXIT();

    // The run in flight when the window closed never reached frame_done(), which only fires at
    // the NEXT pause - so without this a reader heard for 300 ms reports zero. Through a local
    // because the critical exit is an opaque call, so the field would be re-read across it.
    // Note `frames` is NOT folded: it stays the count frame_done() actually reached.
    const uint8_t burst = pending > result->symbol_burst ? pending : result->symbol_burst;

    result->symbol_burst = burst;
    result->symbol_burst_capped = burst >= VARTA_READER_FRAME_MAX;
    result->short_us = short_count ? short_sum / short_count : 0;
    result->long_us = long_count ? long_sum / long_count : 0;

    if(result->frames == 0) {
        // Something was being said in runs, and none of it resolved. Calling that "plain" would
        // be a claim about someone else's security drawn from a failure to parse - including a
        // reader whose gaps fall outside the band this was measured against.
        if(result->symbol_burst >= VARTA_READER_QUIET_BURST) {
            result->verdict = VartaReaderUnknown;
            return;
        }

        // No field. All three terms are required and each keeps a different reader out of
        // here: `stalled` a clean continuous carrier, which generates no interrupts at all and
        // would otherwise read as absent; `!carrier_seen` a reader whose gaps miss the band;
        // `gaps == 0` a field too marginal to hold up for 2 ms.
        if(stalled && !carrier_seen && result->gaps == 0) {
            result->verdict = VartaReaderNoField;
            return;
        }

        // Nothing this could use - and deliberately NOT a finding that the reader is
        // unprotected, nor that a carrier was there at all. A verdict for the first was built
        // and withdrawn: no threshold survived the next reader, because a read-only one from
        // another system outscored a Flipper doing the same job on symbol burst while
        // undercutting it on clustering. As for the second, one stray in-band stall is enough
        // to disqualify NoField above, so this is also where an empty coil can land.
        result->verdict = VartaReaderQuiet;
        return;
    }

    // Whether a frame ever decoded, not whether the last one did. last_frame_bits is the last
    // frame's length and belongs on the evidence screen; using it here threw away a decode that
    // had agreed hundreds of times because one malformed burst arrived before the deadline.
    if(!have_decode) {
        result->verdict = VartaReaderUnknown;
        return;
    }

    // opcode(2) password(32) lock(1) block(3), most significant first. 0b10 is page 0 and 0b11
    // page 1; anything else is not a read of a block, so the rest of the frame is not those
    // fields and must not be reported as if it were. A plain write is 38 bits too, so a read
    // opcode and a clear lock bit is as far as the length alone can tell the two apart.
    const uint32_t opcode = (uint32_t)((bits >> (VARTA_READER_FRAME_BITS - 2)) & 0x3);

    if(opcode != 0x2 && opcode != 0x3) {
        result->verdict = VartaReaderUnknown;
        return;
    }

    // A read never sets the lock bit, so a frame that has it set was read out of alignment and
    // the password beside it is shifted. Checked before anything is filled in, so "the command
    // fields mean something exactly when the verdict decoded" holds by construction rather than
    // by remembering to clear them.
    if((bits >> 3) & 0x1) {
        result->verdict = VartaReaderUnknown;
        return;
    }

    result->password = (uint32_t)((bits >> 4) & 0xFFFFFFFFULL); // block(3) + lock(1) below it
    result->block = (uint8_t)(bits & 0x7);
    result->page1 = opcode == 0x3;

    result->verdict = (!result->page1 && result->password == VARTA_PASSWORD &&
                       result->block == VARTA_MARKER_BLOCK) ?
                          VartaReaderVarta :
                          VartaReaderOther;
}
