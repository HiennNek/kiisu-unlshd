#include "../varta_app.h"

// The evidence behind the verdict, laid out like the key's block screen. This is what makes a
// "Can't decode" answerable: the counts say whether the detector heard anything, and the two
// symbol means say whether the split still separates the classes it was measured against.
#define VARTA_FRAME_TITLE_Y 10
#define VARTA_FRAME_RULE_Y  13
// Five rows at a pitch of 9, the same as the key's block screen. 24 + 4*9 = 60 is the last
// baseline a 64 row screen has room for: FontSecondary descends 2 below its baseline, and
// AlignBottom puts the baseline at y. Six rows at a pitch of 8 was tried and ran a row off the
// bottom, clipping the Command row's descenders.
#define VARTA_FRAME_ROW_Y   24
#define VARTA_FRAME_PITCH   9
#define VARTA_FRAME_COL_KEY 2
// 52, not 62: the widest value is the decoded command at 73 px in FontSecondary, and 62 ran
// it seven pixels off the right edge. The Bits row is 70, so there is room for one more digit
// of agreement and no more. The widest label is "Command" at 41 px, ending at 43.
#define VARTA_FRAME_COL_VAL 52

static void varta_app_scene_check_reader_frame_row(
    VartaApp* app,
    uint8_t row,
    const char* label,
    const char* value) {
    const uint8_t y = VARTA_FRAME_ROW_Y + row * VARTA_FRAME_PITCH;

    widget_add_string_element(
        app->widget, VARTA_FRAME_COL_KEY, y, AlignLeft, AlignBottom, FontSecondary, label);
    widget_add_string_element(
        app->widget, VARTA_FRAME_COL_VAL, y, AlignLeft, AlignBottom, FontSecondary, value);
}

void varta_app_scene_check_reader_frame_on_enter(void* context) {
    VartaApp* app = context;
    const VartaReaderResult* result = &app->reader_result;

    widget_reset(app->widget);
    widget_add_string_element(
        app->widget,
        VARTA_FRAME_COL_KEY,
        VARTA_FRAME_TITLE_Y,
        AlignLeft,
        AlignBottom,
        FontPrimary,
        "What was heard");
    widget_add_line_element(app->widget, 0, VARTA_FRAME_RULE_Y, 127, VARTA_FRAME_RULE_Y);

    FuriString* line = furi_string_alloc();

    // In band over all: a reader whose gaps fall outside 40-200 us reads 0 / several thousand
    // here, which says to look at the band rather than at the coupling. Without the second
    // figure that reader was indistinguishable from an empty coil on every row of this screen.
    furi_string_printf(
        line, "%lu / %lu", (unsigned long)result->gaps, (unsigned long)result->stalls);
    varta_app_scene_check_reader_frame_row(app, 0, "Gaps", furi_string_get_cstr(line));

    // "48+" rather than 48: the symbol store stops at the cap, so a longer burst - a 70 bit
    // passworded write, or noise - reads as exactly the cap and would look like a measurement.
    furi_string_printf(
        line,
        "%lu, max %u%s",
        (unsigned long)result->frames,
        result->symbol_burst,
        result->symbol_burst_capped ? "+" : "");
    varta_app_scene_check_reader_frame_row(app, 1, "Frames", furi_string_get_cstr(line));

    // Bits of the last whole frame - which need not be the frame that decoded. 38 is a
    // passworded read, or a plain write, which is the same length; anything else means the
    // boundary was not where the decoder thought it was.
    // Labelled, because the two are from different runs by design: the length is the last
    // frame's, the agreement is the best run's.
    furi_string_printf(line, "%u last, %u agreed", result->bits, result->agreed);
    varta_app_scene_check_reader_frame_row(app, 2, "Bits", furi_string_get_cstr(line));

    furi_string_printf(
        line, "%lu / %lu us", (unsigned long)result->short_us, (unsigned long)result->long_us);
    varta_app_scene_check_reader_frame_row(app, 3, "Symbols", furi_string_get_cstr(line));

    // A dash where nothing was decoded: a zero that means "never measured" and a zero that
    // means "measured zero" must not look alike on the screen built to explain a failure.
    if(varta_reader_decoded(result)) {
        // The whole command, password included. The verdict screen prints the password only
        // when it is not Varta's, so for a Varta reader this is the only place it is shown.
        furi_string_printf(
            line,
            "%08lX p%u b%u",
            (unsigned long)result->password,
            result->page1 ? 1 : 0,
            result->block);
    } else {
        furi_string_set(line, "-");
    }

    varta_app_scene_check_reader_frame_row(app, 4, "Command", furi_string_get_cstr(line));

    furi_string_free(line);

    view_dispatcher_switch_to_view(app->view_dispatcher, VartaAppViewWidget);
}

bool varta_app_scene_check_reader_frame_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);

    return false;
}

void varta_app_scene_check_reader_frame_on_exit(void* context) {
    VartaApp* app = context;
    widget_reset(app->widget);
}
