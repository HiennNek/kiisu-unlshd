#include "../varta_app.h"
#include <varta_icons.h>

static void varta_app_scene_info_button(GuiButtonType type, InputType input, void* context) {
    VartaApp* app = context;

    if(input != InputTypeShort) return;
    if(type != GuiButtonTypeRight) return;

    view_dispatcher_send_custom_event(app->view_dispatcher, VartaAppCustomEventUse);
}

void varta_app_scene_info_on_enter(void* context) {
    VartaApp* app = context;

    widget_reset(app->widget);

    FuriString* line = furi_string_alloc();

    // Not "Varta key" unless it is one: an ID accepted past the prefix warning is the case
    // this screen gets opened to explain, so it may not be the screen that hides it.
    widget_add_string_element(
        app->widget,
        0,
        2,
        AlignLeft,
        AlignTop,
        FontSecondary,
        varta_app_has_id(app) ? "Varta key" : "Unusual prefix");

    // The ID is the whole of what varies between keys, so it is the one thing this screen is
    // about and the only thing on it in bold.
    varta_app_id_text(app, line);
    widget_add_string_element(
        app->widget, 0, 14, AlignLeft, AlignTop, FontPrimary, furi_string_get_cstr(line));

    widget_add_string_element(
        app->widget, 0, 31, AlignLeft, AlignTop, FontSecondary, "EM4100, RF/64");

    widget_add_icon_element(
        app->widget,
        VARTA_ICON_X(&I_VartaFob_33x42),
        VARTA_ICON_Y(&I_VartaFob_33x42),
        &I_VartaFob_33x42);

    furi_string_free(line);

    // The chip's blocks are a level down, the way NFC puts a card's raw pages behind "More":
    // true of the key but not what anyone opens this screen to find out.
    widget_add_button_element(
        app->widget, GuiButtonTypeRight, "More", varta_app_scene_info_button, app);

    view_dispatcher_switch_to_view(app->view_dispatcher, VartaAppViewWidget);
}

bool varta_app_scene_info_on_event(void* context, SceneManagerEvent event) {
    VartaApp* app = context;

    if(event.type == SceneManagerEventTypeCustom && event.event == VartaAppCustomEventUse) {
        scene_manager_next_scene(app->scene_manager, VartaAppSceneBlocks);
        return true;
    }

    return false;
}

void varta_app_scene_info_on_exit(void* context) {
    VartaApp* app = context;
    widget_reset(app->widget);
}
