#include "../varta_app.h"
#include <toolbox/name_generator.h>
#include <dolphin/dolphin.h>

static void varta_app_scene_save_name_callback(void* context) {
    VartaApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, VartaAppCustomEventConfirm);
}

void varta_app_scene_save_name_on_enter(void* context) {
    VartaApp* app = context;

    name_generator_make_auto(app->name, sizeof(app->name), "Varta");

    text_input_set_header_text(app->text_input, "Name the dump");
    text_input_set_result_callback(
        app->text_input,
        varta_app_scene_save_name_callback,
        app,
        app->name,
        sizeof(app->name),
        false);

    ValidatorIsFile* validator =
        validator_is_file_alloc_init(VARTA_APP_FOLDER, T5577_FILE_EXTENSION, app->name);
    text_input_set_validator(app->text_input, validator_is_file_callback, validator);

    view_dispatcher_switch_to_view(app->view_dispatcher, VartaAppViewTextInput);
}

bool varta_app_scene_save_name_on_event(void* context, SceneManagerEvent event) {
    VartaApp* app = context;

    if(event.type != SceneManagerEventTypeCustom) return false;
    if(event.event != VartaAppCustomEventConfirm) return false;

    T5577Dump dump;
    varta_dump_from_id(app->dict, app->id, &dump);

    FuriString* path =
        furi_string_alloc_printf("%s/%s%s", VARTA_APP_FOLDER, app->name, T5577_FILE_EXTENSION);

    const bool saved = t5577_file_save(&dump, furi_string_get_cstr(path));

    if(!saved) {
        dialog_message_show_storage_error(app->dialogs, "Could not write\nthe dump");
    } else {
        // app->path is deliberately left alone. It is where the browser reopens, and that should
        // be the user's last choice of file rather than wherever a save happened to land - a save
        // is not a selection. The browser lists .t5577 now, so it could hold one either way.
        notification_message(app->notifications, &sequence_success);
        dolphin_deed(DolphinDeedRfidSave);
        varta_app_say(app, "Saved", "Write to a blank tag\nwith the T5577 app");
        scene_manager_search_and_switch_to_previous_scene(app->scene_manager, VartaAppSceneStart);
    }

    furi_string_free(path);

    return true;
}

void varta_app_scene_save_name_on_exit(void* context) {
    VartaApp* app = context;

    void* validator = text_input_get_validator_callback_context(app->text_input);
    text_input_set_validator(app->text_input, NULL, NULL);
    if(validator) validator_is_file_free(validator);

    text_input_reset(app->text_input);
}
