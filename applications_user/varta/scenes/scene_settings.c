#include "../varta_app.h"
#include "../varta_emulate.h"

// Which burst mix the emulate scene sends. The value column has room for the mode number and no
// more, so what each one sends is shown on the emulate screen itself and spelled out in README.md.
static void varta_app_scene_settings_mode_changed(VariableItem* item) {
    VartaApp* app = variable_item_get_context(item);
    const uint8_t index = variable_item_get_current_value_index(item);

    variable_item_set_current_value_text(item, varta_emulate_mix(index)->name);

    // Applied here, saved from the event: variable_item_list fires this callback inside its own
    // view model lock, and a save is four storage round trips with a dialog behind them that
    // waits on the user. Neither belongs under a mutex the GUI thread wants back.
    app->emulate_mix = index;
    view_dispatcher_send_custom_event(app->view_dispatcher, VartaAppCustomEventConfirm);
}

void varta_app_scene_settings_on_enter(void* context) {
    VartaApp* app = context;

    scene_manager_set_scene_state(app->scene_manager, VartaAppSceneSettings, 0);
    variable_item_list_reset(app->var_item_list);

    VariableItem* item = variable_item_list_add(
        app->var_item_list,
        "Emulation",
        (uint8_t)varta_emulate_mix_count(),
        varta_app_scene_settings_mode_changed,
        app);

    variable_item_set_current_value_index(item, app->emulate_mix);
    variable_item_set_current_value_text(item, varta_emulate_mix(app->emulate_mix)->name);

    view_dispatcher_switch_to_view(app->view_dispatcher, VartaAppViewVarItemList);
}

bool varta_app_scene_settings_on_event(void* context, SceneManagerEvent event) {
    VartaApp* app = context;

    // Saved as it changes rather than on the way out, so a mode chosen at a door survives the app
    // being killed there - which is where it is chosen.
    if(event.type != SceneManagerEventTypeCustom) return false;
    if(event.event != VartaAppCustomEventConfirm) return false;

    if(varta_emulate_mix_save(app->emulate_mix)) return true;

    // Said once per visit. Scrolling three modes with no card would otherwise raise a modal per
    // step, and the second says nothing the first did not.
    if(scene_manager_get_scene_state(app->scene_manager, VartaAppSceneSettings)) return true;

    scene_manager_set_scene_state(app->scene_manager, VartaAppSceneSettings, 1);
    varta_app_say(app, "Settings", "Could not save.\nThis mode works until\nyou close the app.");

    return true;
}

void varta_app_scene_settings_on_exit(void* context) {
    VartaApp* app = context;
    variable_item_list_reset(app->var_item_list);
}
