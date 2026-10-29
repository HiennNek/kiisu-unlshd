#include "../varta_app.h"

static void varta_app_scene_enter_id_callback(void* context) {
    VartaApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, VartaAppCustomEventConfirm);
}

void varta_app_scene_enter_id_on_enter(void* context) {
    VartaApp* app = context;

    // Blank every time, with a prefix Varta issues as the one byte that has a conventional
    // answer. It used to start from the key in hand, which meant arriving here after reading a
    // tag or opening a saved one found that key already typed in - surprising, because this is
    // the screen for adding a DIFFERENT key.
    //
    // Correcting a near miss still works: declining the prefix warning returns to the input
    // without re-entering the scene, so the bytes just typed are still there.
    memset(app->id_bytes, 0, sizeof(app->id_bytes));
    app->id_bytes[0] = 0x03;

    byte_input_set_header_text(app->byte_input, "Enter the EM4100 ID");
    byte_input_set_result_callback(
        app->byte_input,
        varta_app_scene_enter_id_callback,
        NULL,
        app,
        app->id_bytes,
        sizeof(app->id_bytes));

    view_dispatcher_switch_to_view(app->view_dispatcher, VartaAppViewByteInput);
}

bool varta_app_scene_enter_id_on_event(void* context, SceneManagerEvent event) {
    VartaApp* app = context;

    if(event.type != SceneManagerEventTypeCustom) return false;
    if(event.event != VartaAppCustomEventConfirm) return false;

    // A warning rather than a refusal: typing is deliberate, and nothing downstream needs the
    // prefix. Composing a key from an ID is arithmetic, and the prefix gate that keeps
    // VARTA_PASSWORD off the WIRE is on the read path, where the ID comes off a tag rather
    // than a keyboard. A saved dump still carries the password in block 7, as every key does.
    if(!varta_id_known(app->id_bytes) &&
       !varta_app_warn(app, "Unusual prefix", VARTA_PREFIX_WARN, "Use anyway")) {
        // Declined: back to the input with the bytes still in it, which is where they get fixed.
        view_dispatcher_switch_to_view(app->view_dispatcher, VartaAppViewByteInput);
        return true;
    }

    varta_app_set_key(app, app->id_bytes);

    scene_manager_next_scene(app->scene_manager, VartaAppSceneKeyMenu);

    return true;
}

void varta_app_scene_enter_id_on_exit(void* context) {
    VartaApp* app = context;
    byte_input_set_result_callback(app->byte_input, NULL, NULL, NULL, NULL, 0);
    byte_input_set_header_text(app->byte_input, "");
}
