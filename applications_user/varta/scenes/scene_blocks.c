#include "../varta_app.h"

// A title with a rule under it, then one row per block. Five rows at a pitch of 9 reach y=60,
// which is the last baseline a 64 pixel screen has room for.
#define VARTA_BLOCKS_TITLE_Y 10
#define VARTA_BLOCKS_RULE_Y  13
#define VARTA_BLOCKS_ROW_Y   24
#define VARTA_BLOCKS_PITCH   9
#define VARTA_BLOCKS_COL_NUM 2
#define VARTA_BLOCKS_COL_HEX 14
#define VARTA_BLOCKS_COL_USE 72

// The five blocks a Varta key is made of, in the order the chip holds them. Blocks 3 to 5 and
// page 1 are not here because a composed key puts nothing in them - see varta_dump_from_id(),
// whose block count is what keeps this list honest.
//
// The "Use" column is the point. Without it block 6 looks no more important than block 3, when
// it is the one word the reader actually checks - and two of the five are just the ID, which is
// on the screen behind this one.
static const struct {
    uint8_t block;
    const char* use;
} varta_blocks_rows[] = {
    {0, "Config"},
    {1, "ID"},
    {2, "ID"},
    {VARTA_MARKER_BLOCK, "Marker"},
    {LFRFID_T5577_PASSWORD_BLOCK, "Password"},
};

static_assert(COUNT_OF(varta_blocks_rows) == VARTA_KEY_BLOCK_COUNT, "a row per block of a key");

void varta_app_scene_blocks_on_enter(void* context) {
    VartaApp* app = context;

    // Composed, not read: this screen shows what the key WOULD be written as, which is the same
    // for every key with this ID. Where the ID came from is not a property of these words.
    T5577Dump dump;
    varta_dump_from_id(app->dict, app->id, &dump);

    widget_reset(app->widget);
    widget_add_string_element(
        app->widget,
        VARTA_BLOCKS_COL_NUM,
        VARTA_BLOCKS_TITLE_Y,
        AlignLeft,
        AlignBottom,
        FontPrimary,
        varta_app_has_id(app) ? "Varta Key" : "Unusual Key");
    widget_add_line_element(app->widget, 0, VARTA_BLOCKS_RULE_Y, 127, VARTA_BLOCKS_RULE_Y);

    for(size_t row = 0; row < COUNT_OF(varta_blocks_rows); row++) {
        const uint8_t block = varta_blocks_rows[row].block;
        const uint8_t y = VARTA_BLOCKS_ROW_Y + row * VARTA_BLOCKS_PITCH;
        char text[16];

        snprintf(text, sizeof(text), "%u", block);
        widget_add_string_element(
            app->widget, VARTA_BLOCKS_COL_NUM, y, AlignLeft, AlignBottom, FontSecondary, text);

        snprintf(text, sizeof(text), "%08lX", (unsigned long)dump.block[0][block]);
        widget_add_string_element(
            app->widget, VARTA_BLOCKS_COL_HEX, y, AlignLeft, AlignBottom, FontSecondary, text);

        widget_add_string_element(
            app->widget,
            VARTA_BLOCKS_COL_USE,
            y,
            AlignLeft,
            AlignBottom,
            FontSecondary,
            varta_blocks_rows[row].use);
    }

    view_dispatcher_switch_to_view(app->view_dispatcher, VartaAppViewWidget);
}

bool varta_app_scene_blocks_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);

    return false;
}

void varta_app_scene_blocks_on_exit(void* context) {
    VartaApp* app = context;
    widget_reset(app->widget);
}
