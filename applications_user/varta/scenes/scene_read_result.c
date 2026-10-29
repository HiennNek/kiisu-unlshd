#include "../varta_app.h"
#include <varta_icons.h>
#include <dolphin/dolphin.h>

static void
    varta_app_scene_read_result_button(GuiButtonType type, InputType input, void* context) {
    VartaApp* app = context;

    if(input != InputTypeShort) return;

    if(type == GuiButtonTypeLeft) {
        view_dispatcher_send_custom_event(app->view_dispatcher, VartaAppCustomEventRetry);
    } else if(type == GuiButtonTypeCenter) {
        view_dispatcher_send_custom_event(app->view_dispatcher, VartaAppCustomEventUse);
    }
}

// What each verdict means, in the terms that say what to do about it. Title and detail together in
// one switch with every case named, so a verdict added later is a -Wswitch warning rather than a
// pair that drifts apart or a string that silently reads as another verdict's.
typedef struct {
    const char* title;
    const char* detail; // empty where the title already says the whole of it
} VartaVerdictText;

static VartaVerdictText varta_app_scene_read_result_text(VartaVerdict verdict) {
    switch(verdict) {
    // Titles are bound by nothing: they sit at y=2, above both the icon and the button row. What
    // bounds the details is the text box below, not a character count - but it bounds them by
    // dropping what does not fit, silently, so a line over 80 px still costs its tail.
    case VartaVerdictGenuine:
        return (VartaVerdictText){"Genuine key", ""};
    case VartaVerdictOtherMarker:
        return (VartaVerdictText){"Not a Varta key", "Block 6 holds\nanother word"};
    case VartaVerdictUnanswered:
        return (VartaVerdictText){"No answer yet", "Only its own ID\ncame back. Retry"};
    case VartaVerdictLost:
        return (VartaVerdictText){"Tag went away", "It stopped\nanswering. Retry"};
    case VartaVerdictOpen:
        return (VartaVerdictText){"Unprotected clone", "A Varta ID with\nno password"};
    case VartaVerdictOtherRate:
        return (VartaVerdictText){"Wrong bit rate", "A Varta ID, but\nnot at RF/64"};
    case VartaVerdictNotVarta:
        return (VartaVerdictText){"Not a Varta ID", VARTA_PREFIX_WRONG};
    case VartaVerdictNotEm4100:
        return (VartaVerdictText){"Not an EM4100", VARTA_PREFIX_NOT_EM};
    case VartaVerdictNoTag:
        return (VartaVerdictText){"Nothing found", "Hold the key on\nthe back of the\nFlipper"};
    }

    return (VartaVerdictText){"Nothing found", ""};
}

void varta_app_scene_read_result_on_enter(void* context) {
    VartaApp* app = context;

    // Once, not every time the screen is come back to.
    const bool first =
        scene_manager_get_scene_state(app->scene_manager, VartaAppSceneReadResult) == 0;
    scene_manager_set_scene_state(app->scene_manager, VartaAppSceneReadResult, 1);

    widget_reset(app->widget);

    const VartaVerdictText text = varta_app_scene_read_result_text(app->verdict);

    // A keyfob for the one verdict that found a key - which is the thing in the user's hand -
    // and the warning dolphin for the eight that did not. Right-hand third, below the title: the
    // button row only ever uses the left two thirds, so nothing collides.
    const bool genuine = app->verdict == VartaVerdictGenuine;

    widget_add_icon_element(
        app->widget,
        genuine ? 91 : 82,
        20,
        genuine ? &I_VartaFob_33x42 : &I_WarningDolphinFlip_45x42);

    widget_add_string_element(app->widget, 0, 2, AlignLeft, AlignTop, FontPrimary, text.title);

    // Bold, because on a screen about one key this IS the key - and an ID read off a tag whose
    // prefix is not one of ours is still worth printing, as the one thing the user can look up.
    //
    // Full width, and it overlaps: the widest hex ID is 91 px in FontPrimary (every glyph an
    // 'A'), against the warning dolphin at x=82, sharing rows 20..27. The fob is clear - its ink
    // starts at x=94 - so only the eight failure verdicts are affected, and the dolphin's own
    // top-left corner is blank there.
    if(app->id_read) {
        FuriString* line = furi_string_alloc();
        varta_app_id_text(app, line);
        widget_add_string_element(
            app->widget, 0, 16, AlignLeft, AlignTop, FontPrimary, furi_string_get_cstr(line));
        furi_string_free(line);
    }

    // A text box rather than a multiline string: it word-wraps inside the width the icon
    // leaves and stops at the height the button row leaves, so both budgets are enforced by the
    // element instead of by counting characters in a comment.
    const uint8_t detail_y = app->id_read ? 30 : 18;

    widget_add_text_box_element(
        app->widget,
        0,
        detail_y,
        80,
        VARTA_BUTTON_ROW_Y - detail_y,
        AlignLeft,
        AlignTop,
        text.detail,
        false);

    widget_add_button_element(
        app->widget, GuiButtonTypeLeft, "Retry", varta_app_scene_read_result_button, app);

    // Offered for any tag carrying an ID Varta issues, genuine or not: the ID is the whole of
    // what a key is built from, so a clone's is worth as much as a key's.
    if(varta_app_has_id(app)) {
        widget_add_button_element(
            app->widget, GuiButtonTypeCenter, "Use", varta_app_scene_read_result_button, app);
    }

    if(first) {
        notification_message(
            app->notifications, genuine ? &sequence_success : &sequence_blink_blue_100);
        if(app->id_read) dolphin_deed(DolphinDeedRfidReadSuccess);
    }

    view_dispatcher_switch_to_view(app->view_dispatcher, VartaAppViewWidget);
}

bool varta_app_scene_read_result_on_event(void* context, SceneManagerEvent event) {
    VartaApp* app = context;

    // Back goes past the read rather than into it.
    if(event.type == SceneManagerEventTypeBack) {
        scene_manager_search_and_switch_to_previous_scene(app->scene_manager, VartaAppSceneStart);
        return true;
    }

    if(event.type != SceneManagerEventTypeCustom) return false;

    if(event.event == VartaAppCustomEventRetry) {
        scene_manager_search_and_switch_to_previous_scene(app->scene_manager, VartaAppSceneRead);
        return true;
    }

    if(event.event == VartaAppCustomEventUse) {
        scene_manager_next_scene(app->scene_manager, VartaAppSceneKeyMenu);
        return true;
    }

    return false;
}

void varta_app_scene_read_result_on_exit(void* context) {
    VartaApp* app = context;
    widget_reset(app->widget);
}
