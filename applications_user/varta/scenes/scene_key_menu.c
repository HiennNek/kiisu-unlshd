#include "../varta_app.h"

typedef enum {
    KeyIndexEmulate,
    KeyIndexSave,
    KeyIndexInfo,
} KeyIndex;

static void varta_app_scene_key_menu_callback(void* context, uint32_t index) {
    VartaApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void varta_app_scene_key_menu_on_enter(void* context) {
    VartaApp* app = context;

    FuriString* header = furi_string_alloc();
    varta_app_id_text(app, header);

    submenu_reset(app->submenu);
    submenu_set_header(app->submenu, furi_string_get_cstr(header));
    submenu_add_item(
        app->submenu, "Emulate", KeyIndexEmulate, varta_app_scene_key_menu_callback, app);
    submenu_add_item(
        app->submenu, "Save T5577 Dump", KeyIndexSave, varta_app_scene_key_menu_callback, app);
    submenu_add_item(app->submenu, "Info", KeyIndexInfo, varta_app_scene_key_menu_callback, app);
    submenu_set_selected_item(
        app->submenu, scene_manager_get_scene_state(app->scene_manager, VartaAppSceneKeyMenu));

    furi_string_free(header);

    view_dispatcher_switch_to_view(app->view_dispatcher, VartaAppViewSubmenu);
}

bool varta_app_scene_key_menu_on_event(void* context, SceneManagerEvent event) {
    VartaApp* app = context;

    if(event.type != SceneManagerEventTypeCustom) return false;
    if(event.event > KeyIndexInfo) return false;

    scene_manager_set_scene_state(app->scene_manager, VartaAppSceneKeyMenu, event.event);

    if(event.event == KeyIndexEmulate) {
        scene_manager_next_scene(app->scene_manager, VartaAppSceneEmulate);
    } else if(event.event == KeyIndexSave) {
        scene_manager_next_scene(app->scene_manager, VartaAppSceneSaveName);
    } else {
        scene_manager_next_scene(app->scene_manager, VartaAppSceneInfo);
    }

    return true;
}

void varta_app_scene_key_menu_on_exit(void* context) {
    VartaApp* app = context;
    submenu_reset(app->submenu);
}
