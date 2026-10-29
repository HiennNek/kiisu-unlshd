#include "../varta_app.h"
#include <dolphin/dolphin.h>

// Label and destination in one row, because they used to be in two places: a menu built by one
// list of submenu_add_item() calls and dispatched by an if/else chain whose final else MEANT
// Settings. Reorder that and the compiler says nothing - the wrong scene just opens.
static const struct {
    const char* label;
    VartaAppScene scene;
} varta_start_items[] = {
    {"Read", VartaAppSceneRead},
    {"Saved", VartaAppSceneSelectKey},
    {"Add Manually", VartaAppSceneEnterId},
    {"Check Reader", VartaAppSceneCheckReader},
    {"Settings", VartaAppSceneSettings},
};

static void varta_app_scene_start_callback(void* context, uint32_t index) {
    VartaApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void varta_app_scene_start_on_enter(void* context) {
    VartaApp* app = context;

    submenu_reset(app->submenu);
    submenu_set_header(app->submenu, "Varta");

    for(size_t i = 0; i < COUNT_OF(varta_start_items); i++) {
        submenu_add_item(
            app->submenu, varta_start_items[i].label, i, varta_app_scene_start_callback, app);
    }

    submenu_set_selected_item(
        app->submenu, scene_manager_get_scene_state(app->scene_manager, VartaAppSceneStart));

    view_dispatcher_switch_to_view(app->view_dispatcher, VartaAppViewSubmenu);
}

bool varta_app_scene_start_on_event(void* context, SceneManagerEvent event) {
    VartaApp* app = context;

    if(event.type != SceneManagerEventTypeCustom) return false;
    if(event.event >= COUNT_OF(varta_start_items)) return false;

    scene_manager_set_scene_state(app->scene_manager, VartaAppSceneStart, event.event);

    const VartaAppScene scene = varta_start_items[event.event].scene;

    // The one entry that is more than a scene change.
    if(scene == VartaAppSceneRead) dolphin_deed(DolphinDeedRfidRead);

    scene_manager_next_scene(app->scene_manager, scene);

    return true;
}

void varta_app_scene_start_on_exit(void* context) {
    VartaApp* app = context;
    submenu_reset(app->submenu);
}
