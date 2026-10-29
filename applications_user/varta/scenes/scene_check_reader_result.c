#include "../varta_app.h"
#include <varta_icons.h>

typedef struct {
    const char* title;
    // Empty where the title, the password and the icon already say it. Otherwise what to do
    // where the user can act, and what the verdict deliberately does not claim where they
    // cannot.
    const char* detail;
    // In this table rather than beside it: the icon is the loudest security claim on the screen,
    // so a verdict added later should not be able to get a warning for its words and the default
    // picture in silence.
    const Icon* icon;
} VartaReaderText;

// Nothing was concluded, so no padlock may appear. Spelled once because it is both the
// Unknown row and the fallback, and a screen described in two places drifts in one of them.
static const VartaReaderText varta_result_undecodable = {
    "Can't decode",
    "See More",
    &I_WarningDolphinFlip_45x42};

// Every case named, so a verdict added later is a -Wswitch warning rather than a row that
// silently drifts apart.
static VartaReaderText varta_app_scene_check_reader_result_text(VartaReaderVerdict verdict) {
    switch(verdict) {
    case VartaReaderVarta:
        return (VartaReaderText){"Varta reader", "", &I_VartaFob_33x42};
    case VartaReaderOther:
        // The length cannot tell a passworded read from a plain write - both are 38 bits, and
        // the lock bit this checks is a data bit in a write. The password below may be a word
        // that reader writes into tags, which matters because this app writes passwords.
        return (VartaReaderText){"Other scheme", "May be a write", &I_VartaLocked_31x40};
    case VartaReaderQuiet:
        // No open padlock anywhere in this table: the verdict that would have earned one was
        // withdrawn because nothing here can measure it. The words claim as little as the
        // picture: not that a reader said nothing - a frame chopped by poor coupling lands
        // here - and not that a carrier was present, since one stray in-band stall is enough
        // to rule out "No reader".
        return (VartaReaderText){
            "Nothing read", "No command this\napp could parse", &I_WarningDolphinFlip_45x42};
    case VartaReaderUnknown:
        return varta_result_undecodable;
    case VartaReaderNoField:
        return (VartaReaderText){"No reader", "Hold it on a reader", &I_WarningDolphinFlip_45x42};
    }

    return varta_result_undecodable;
}

static void
    varta_app_scene_check_reader_result_button(GuiButtonType type, InputType input, void* context) {
    VartaApp* app = context;

    if(input != InputTypeShort) return;

    if(type == GuiButtonTypeLeft) {
        view_dispatcher_send_custom_event(app->view_dispatcher, VartaAppCustomEventExit);
    } else if(type == GuiButtonTypeCenter) {
        view_dispatcher_send_custom_event(app->view_dispatcher, VartaAppCustomEventRetry);
    } else if(type == GuiButtonTypeRight) {
        view_dispatcher_send_custom_event(app->view_dispatcher, VartaAppCustomEventMore);
    }
}

void varta_app_scene_check_reader_result_on_enter(void* context) {
    VartaApp* app = context;

    const bool first =
        scene_manager_get_scene_state(app->scene_manager, VartaAppSceneCheckReaderResult) == 0;
    scene_manager_set_scene_state(app->scene_manager, VartaAppSceneCheckReaderResult, 1);

    const VartaReaderResult* result = &app->reader_result;
    const VartaReaderText text = varta_app_scene_check_reader_result_text(result->verdict);

    widget_reset(app->widget);

    widget_add_icon_element(
        app->widget, VARTA_ICON_X(text.icon), VARTA_ICON_Y(text.icon), text.icon);
    widget_add_string_element(app->widget, 0, 2, AlignLeft, AlignTop, FontPrimary, text.title);

    FuriString* line = furi_string_alloc();

    // The bold line is the find - the scheme's name for a Varta reader, the password for
    // anything else - and the detail starts under it. Only the verdicts that decoded have one.
    uint8_t detail_y = 18;

    const bool decoded = varta_reader_decoded(result);

    // Said plainly, because nothing else on this screen would show it: without agreement the
    // line below could be one lucky frame of noise that happened to carry a legal opcode.
    if(decoded && !varta_reader_confirmed(result)) {
        furi_string_printf(line, "Unconfirmed: %u/%u", result->agreed, VARTA_READER_FRAMES_AGREE);
        widget_add_string_element(
            app->widget, 0, 40, AlignLeft, AlignTop, FontSecondary, furi_string_get_cstr(line));
    } else if(!decoded && result->verdict != VartaReaderNoField) {
        // The figure this verdict was settled on, against VARTA_READER_QUIET_BURST, so someone
        // who disbelieves it has the deciding number. Only where nothing decoded: a confirmed
        // read has its answer on the line above. Not for NoField either, where gaps == 0 is one
        // of the gates and so this is 0 by construction - a number that cannot vary is not
        // evidence.
        furi_string_printf(
            line,
            "In-band run: %u%s",
            result->symbol_burst,
            result->symbol_burst_capped ? "+" : "");
        widget_add_string_element(
            app->widget, 0, 40, AlignLeft, AlignTop, FontSecondary, furi_string_get_cstr(line));
    }

    if(decoded) {
        // Varta's password is the same on every reader and every key, so printing it is not a
        // finding - naming the scheme is. A password worth reading is one that is NOT Varta's,
        // and that is the case this prints. Both are on the More screen either way.
        if(result->verdict == VartaReaderVarta) {
            furi_string_set(line, "Protected Mode");
        } else {
            furi_string_printf(line, "%08lX", (unsigned long)result->password);
        }

        widget_add_string_element(
            app->widget, 0, 16, AlignLeft, AlignTop, FontPrimary, furi_string_get_cstr(line));
        detail_y = 30;
    }

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

    furi_string_free(line);

    widget_add_button_element(
        app->widget, GuiButtonTypeLeft, "Back", varta_app_scene_check_reader_result_button, app);
    widget_add_button_element(
        app->widget, GuiButtonTypeCenter, "Again", varta_app_scene_check_reader_result_button, app);
    widget_add_button_element(
        app->widget, GuiButtonTypeRight, "More", varta_app_scene_check_reader_result_button, app);

    if(first) {
        // The success chime only for a verdict that earned it. A single frame that happened to
        // decode sounds exactly like six agreeing ones otherwise.
        const bool sure = result->verdict == VartaReaderVarta && varta_reader_confirmed(result);

        notification_message(
            app->notifications, sure ? &sequence_success : &sequence_blink_blue_100);
    }

    view_dispatcher_switch_to_view(app->view_dispatcher, VartaAppViewWidget);
}

bool varta_app_scene_check_reader_result_on_event(void* context, SceneManagerEvent event) {
    VartaApp* app = context;

    // Past the listening scene, not into it: it starts a run on entry, so the plain back key
    // would otherwise be indistinguishable from Again.
    const bool leaving =
        event.type == SceneManagerEventTypeBack ||
        (event.type == SceneManagerEventTypeCustom && event.event == VartaAppCustomEventExit);

    if(leaving) {
        scene_manager_search_and_switch_to_previous_scene(app->scene_manager, VartaAppSceneStart);
        return true;
    }

    if(event.type != SceneManagerEventTypeCustom) return false;

    if(event.event == VartaAppCustomEventRetry) {
        scene_manager_previous_scene(app->scene_manager);
        return true;
    }

    if(event.event == VartaAppCustomEventMore) {
        scene_manager_next_scene(app->scene_manager, VartaAppSceneCheckReaderFrame);
        return true;
    }

    return false;
}

void varta_app_scene_check_reader_result_on_exit(void* context) {
    VartaApp* app = context;
    widget_reset(app->widget);
}
