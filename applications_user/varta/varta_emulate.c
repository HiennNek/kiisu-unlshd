#include "varta_emulate.h"

#include <furi_hal.h>
#include <lfrfid/protocols/lfrfid_protocols.h>
#include <storage/storage.h>
#include <toolbox/level_duration.h>
#include <toolbox/saved_struct.h>

#define TAG "VartaEmulate"

// Why there is more than one of these is on VartaEmulateMix. Which is default: the best
// compromise on both halves - ID 4 + mark 8 carries more marker, ID 8 + mark 1 as much ID.
//
// The row ORDER is user-visible API, and this is where a reorder would be typed: position is
// what "Mode 2" means, what README.md documents, and what the saved setting indexes. Add rows at
// the end; never reorder them. Burst lengths are capped - see VARTA_EMULATE_MAX_ID_FRAMES.
//
// A bare-ID entry sat here as the control until it had done its job. Do not re-add it: it never
// opened anything, which is the whole of what it was for.
static const VartaEmulateMix varta_emulate_mixes[] = {
    {"Mode 1", 8, 4},
    {"Mode 2", 8, 1},
    {"Mode 3", 4, 8},
};

size_t varta_emulate_mix_count(void) {
    return COUNT_OF(varta_emulate_mixes);
}

const VartaEmulateMix* varta_emulate_mix(size_t index) {
    furi_check(index < COUNT_OF(varta_emulate_mixes));
    return &varta_emulate_mixes[index];
}

/** The chosen mix, kept across runs.
 *
 * Which mix a door wants is a property of that door, so it is worth remembering: without this,
 * testing two readers means re-pressing the button at each one, and the result of a sweep is lost
 * the moment the app closes.
 */
#define VARTA_EMULATE_SETTINGS_PATH    APP_DATA_PATH("emulate.settings")
#define VARTA_EMULATE_SETTINGS_MAGIC   0x56
#define VARTA_EMULATE_SETTINGS_VERSION 1

typedef struct {
    uint8_t mix;
} VartaEmulateSettings;

uint8_t varta_emulate_mix_load(void) {
    VartaEmulateSettings settings = {0};

    // Asked before loading, because saved_struct_load() logs a missing file at E - and on a
    // first run there is nothing wrong with not having one. The record is opened here rather
    // than taken as a parameter, the way lfrfid_settings.c does it: it is refcounted, and a
    // module that saves without a handle should not need one to load.
    Storage* storage = furi_record_open(RECORD_STORAGE);
    const FS_Error stat = storage_common_stat(storage, VARTA_EMULATE_SETTINGS_PATH, NULL);
    furi_record_close(RECORD_STORAGE);

    // No file is the normal state until a mode is chosen; anything else means the card is not
    // answering, which is worth a word because the setting silently reverts to the default.
    if(stat == FSE_NOT_EXIST) return 0;
    if(stat != FSE_OK) {
        FURI_LOG_W(TAG, "cannot reach the settings file, using the default mode");
        return 0;
    }

    if(!saved_struct_load(
           VARTA_EMULATE_SETTINGS_PATH,
           &settings,
           sizeof(settings),
           VARTA_EMULATE_SETTINGS_MAGIC,
           VARTA_EMULATE_SETTINGS_VERSION)) {
        FURI_LOG_W(TAG, "settings unreadable, falling back to the default mode");
        return 0;
    }

    // A file from a build with more mixes in it would index off the end of the table. Logged
    // because it is the one case here that means something happened: a sweep's result silently
    // became mix 0.
    if(settings.mix >= COUNT_OF(varta_emulate_mixes)) {
        FURI_LOG_W(TAG, "saved mix %u is past the table, using the default", settings.mix);
        return 0;
    }

    return settings.mix;
}

bool varta_emulate_mix_save(uint8_t mix) {
    const VartaEmulateSettings settings = {.mix = mix};

    if(saved_struct_save(
           VARTA_EMULATE_SETTINGS_PATH,
           &settings,
           sizeof(settings),
           VARTA_EMULATE_SETTINGS_MAGIC,
           VARTA_EMULATE_SETTINGS_VERSION)) {
        return true;
    }

    // E, not W: saved_struct_save() has already logged the storage error at E, and a warning
    // beside it reads as the lesser of the two when it is the one with a user consequence.
    FURI_LOG_E(TAG, "could not save mix %u", mix);
    return false;
}

typedef struct {
    bool level;
    uint32_t cycles;
} VartaRun;

struct VartaEmulate {
    // Read by the DMA for as long as the stream runs, so it outlives every call that builds it.
    uint32_t duration[VARTA_EMULATE_MAX_PULSES];
    uint32_t pulse[VARTA_EMULATE_MAX_PULSES];
    size_t pulses;
    uint32_t cycles;

    VartaRun run[VARTA_EMULATE_MAX_RUNS];
    size_t runs;

    bool running;
};

VartaEmulate* varta_emulate_alloc(void) {
    VartaEmulate* emulate = malloc(sizeof(VartaEmulate));
    emulate->running = false;
    return emulate;
}

void varta_emulate_free(VartaEmulate* emulate) {
    furi_check(emulate);
    varta_emulate_stop(emulate);
    free(emulate);
}

// Adjacent equal levels are one run: the pulse the timer makes is "high for a while, then low",
// so what matters is where the level changes and not how the encoder happened to chop it up.
static bool varta_emulate_push(VartaEmulate* emulate, bool level, uint32_t cycles) {
    if(cycles == 0) return true;

    if(emulate->runs && emulate->run[emulate->runs - 1].level == level) {
        emulate->run[emulate->runs - 1].cycles += cycles;
        return true;
    }

    if(emulate->runs == VARTA_EMULATE_MAX_RUNS) return false;

    emulate->run[emulate->runs].level = level;
    emulate->run[emulate->runs].cycles = cycles;
    emulate->runs++;
    return true;
}

// EM4100 frames, taken from the protocol the rest of the app already uses rather than spelled out
// again here - its header, parity rows and stop bit are not this file's business. The encoder
// repeats forever, so one start covers however many frames the burst wants.
static bool varta_emulate_push_em4100(VartaEmulate* emulate, ProtocolDict* dict, uint8_t frames) {
    if(frames == 0) return true;

    protocol_dict_encoder_start(dict, LFRFIDProtocolEM4100);

    const uint32_t target = frames * VARTA_EMULATE_EM4100_CYCLES;
    uint32_t total = 0;

    while(total < target) {
        const LevelDuration ld = protocol_dict_encoder_yield(dict, LFRFIDProtocolEM4100);
        const uint32_t cycles = level_duration_get_duration(ld);

        // Unreachable as the rates stand - every yield is half a bit and target is a whole
        // number of frames - which is the point: retune VARTA_EMULATE_HALFBIT or the encoder's
        // rate and this fails loudly instead of handing the marker a torn frame.
        if(cycles == 0 || total + cycles > target) return false;

        if(!varta_emulate_push(emulate, level_duration_get_level(ld), cycles)) return false;
        total += cycles;
    }

    return true;
}

// The marker, in the same Manchester the ID is in: every bit is its own level then the opposite,
// which is what keeps the stream free of DC and what the reader's receiver expects. Repeated
// back to back, so where a burst holds more than one word a receiver that opens part-way
// through gets a whole next one. ID 8 + mark 1 deliberately forgoes that.
static bool varta_emulate_push_marker(VartaEmulate* emulate, uint8_t words) {
    for(uint8_t word = 0; word < words; word++) {
        for(size_t bit = 0; bit < LFRFID_T5577_WORD_BITS; bit++) {
            const bool value = (VARTA_MARKER >> (LFRFID_T5577_WORD_BITS - 1 - bit)) & 1UL;

            if(!varta_emulate_push(emulate, value, VARTA_EMULATE_HALFBIT)) return false;
            if(!varta_emulate_push(emulate, !value, VARTA_EMULATE_HALFBIT)) return false;
        }
    }

    return true;
}

/** Turn the runs into the high/low pairs the emulation DMA takes.
 *
 * Each entry is one high run followed by one low run, because that is the only shape a PWM period
 * can make: duration goes to ARR, which counts from zero, and pulse is how much of it is high.
 *
 * The stream is read circularly, so the ends meet - two runs of the same level there are really
 * one run, and the pairing has to begin at a high run or the first entry would be a low one with
 * no high to open it. Which high run it begins at does not matter: the loop has no beginning once
 * it is running, and nothing downstream is synchronised to it.
 */
static bool varta_emulate_render(VartaEmulate* emulate) {
    if(emulate->runs < 2) return false;

    // Where the buffer wraps is not a transition if both sides hold the same level.
    if(emulate->run[0].level == emulate->run[emulate->runs - 1].level) {
        emulate->run[0].cycles += emulate->run[emulate->runs - 1].cycles;
        emulate->runs--;
    }

    // Levels now alternate the whole way round, so an odd count cannot happen - and if it did,
    // the last pair would borrow a run that the first pair also used.
    if(emulate->runs % 2) return false;

    size_t start = 0;
    while(start < emulate->runs && !emulate->run[start].level) {
        start++;
    }

    // Manchester puts a transition inside every bit, so a stream with no high run is impossible.
    if(start == emulate->runs) return false;

    emulate->pulses = 0;
    emulate->cycles = 0;

    for(size_t i = 0; i < emulate->runs; i += 2) {
        const VartaRun* high = &emulate->run[(start + i) % emulate->runs];
        const VartaRun* low = &emulate->run[(start + i + 1) % emulate->runs];

        if(emulate->pulses == VARTA_EMULATE_MAX_PULSES) return false;

        emulate->duration[emulate->pulses] = high->cycles + low->cycles - 1;
        emulate->pulse[emulate->pulses] = high->cycles;
        emulate->pulses++;
        emulate->cycles += high->cycles + low->cycles;
    }

    return true;
}

static void varta_emulate_dma_isr(bool half, void* context) {
    UNUSED(half);
    UNUSED(context);
    // The stream is fixed and circular, so there is nothing to refill. The callback exists only
    // because the HAL takes one.
}

bool varta_emulate_start(
    VartaEmulate* emulate,
    ProtocolDict* dict,
    const uint8_t* id,
    uint8_t id_frames,
    uint8_t marker_words) {
    furi_check(emulate);
    furi_check(!emulate->running);
    furi_check(id_frames <= VARTA_EMULATE_MAX_ID_FRAMES);
    furi_check(marker_words <= VARTA_EMULATE_MAX_MARKER_WORDS);

    if(id_frames == 0 && marker_words == 0) return false;

    protocol_dict_set_data(dict, LFRFIDProtocolEM4100, id, VARTA_ID_SIZE);

    emulate->runs = 0;

    if(!varta_emulate_push_em4100(emulate, dict, id_frames)) return false;
    if(!varta_emulate_push_marker(emulate, marker_words)) return false;
    if(!varta_emulate_render(emulate)) return false;

    furi_hal_rfid_tim_emulate_dma_start(
        emulate->duration, emulate->pulse, emulate->pulses, varta_emulate_dma_isr, emulate);

    emulate->running = true;
    return true;
}

void varta_emulate_stop(VartaEmulate* emulate) {
    furi_check(emulate);
    if(!emulate->running) return;

    furi_hal_rfid_tim_emulate_dma_stop();
    furi_hal_rfid_pins_reset();

    emulate->running = false;
}
