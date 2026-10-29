#include "../varta_app.h"
#include <varta_icons.h>

#define TAG "Varta"

// The lfrfid key extension, spelled here: the app's own define lives in a header a .fap cannot
// include. T5577_FILE_EXTENSION can be included, and is.
#define VARTA_KEY_EXTENSION ".rfid"

// The browser worker splits this on "|", so one browser covers both kinds of file - which is what
// the folder actually holds. A key this app composed is a .t5577 dump; anything read by the 125
// kHz app beside it is a .rfid.
#define VARTA_BROWSE_EXTENSIONS VARTA_KEY_EXTENSION "|" T5577_FILE_EXTENSION

// Only the .rfid path needs this: a key file's ID arrives in the dict, so it has to come back
// out through the gate. A dump has its five bytes in hand and goes straight to set_key().
static bool varta_app_scene_select_key_adopt(VartaApp* app, ProtocolId protocol) {
    // Snapshotted because adopting overwrites the key in hand before anyone has agreed to it:
    // take_read_id() clears and sets unconditionally, and only its return value says whether the
    // prefix was one Varta issues. The dump path warns before adopting, so without this the two
    // paths would leave different state behind on a decline - and Add Manually pre-fills from
    // whatever is in hand.
    const bool had_key = app->id_read;
    uint8_t held[VARTA_ID_SIZE];
    memcpy(held, app->id, sizeof(held));

    app->read_protocol = protocol;

    if(varta_app_take_read_id(app)) return true;

    // Nothing to offer if it was never an EM4100: there is no ID to accept. An unusual prefix is
    // a different matter - the ID is sound, only its first bytes are unconventional.
    if(!app->id_read) {
        varta_app_say(app, "Not a Varta ID", VARTA_PREFIX_NOT_EM);
        return false;
    }

    if(varta_app_warn(app, "Unusual prefix", VARTA_PREFIX_WARN, "Use anyway")) return true;

    if(had_key) {
        varta_app_set_key(app, held);
    } else {
        varta_app_clear_key(app);
    }

    return false;
}

static bool varta_app_scene_select_key_load_rfid(VartaApp* app) {
    const ProtocolId protocol = lfrfid_dict_file_load(app->dict, furi_string_get_cstr(app->path));

    if(protocol == PROTOCOL_NO) {
        // lfrfid_dict_file_load() logs nothing on any of its six failure paths, so without this
        // the only record of a refusal is a sentence that assumes it was a format problem.
        FURI_LOG_E(TAG, "could not load %s as a key", furi_string_get_cstr(app->path));
        varta_app_say(app, "Cannot open", "Not a key file this\nversion can read");
        return false;
    }

    return varta_app_scene_select_key_adopt(app, protocol);
}

static bool varta_app_scene_select_key_load_dump(VartaApp* app) {
    T5577Dump dump = {0};

    if(!t5577_file_load(&dump, furi_string_get_cstr(app->path))) {
        // t5577_file_load() logs the format and version cases itself, but not a file it could not
        // open at all - which is the one worth telling apart from a dump this build cannot read.
        FURI_LOG_E(TAG, "could not load %s as a dump", furi_string_get_cstr(app->path));
        varta_app_say(app, "Cannot open", "Not a T5577 dump this\nversion can read");
        return false;
    }

    const VartaDumpResult key = varta_dump_read(app->dict, &dump);

    switch(key.verdict) {
    case VartaDumpNoBlocks:
        varta_app_say(
            app, "Nothing in it", "No ID blocks. Read the\ntag again with its\npassword.");
        return false;

    case VartaDumpNotEm4100:
        varta_app_say(app, "No ID in it", "Blocks 1 and 2 are not\nan EM4100 frame");
        return false;

    case VartaDumpOtherId:
        // The ID decoded and re-encoded cleanly; only its prefix is unusual, which is the user's
        // call to make rather than the app's.
        if(!varta_app_warn(app, "Unusual prefix", VARTA_PREFIX_WARN, "Use anyway")) return false;
        break;

    case VartaDumpOk:
        break;
    }

    // All three, not an OR of them: "which of a key's constants was in that file" is a question
    // someone asks of a file that did not behave, and the answer is only useful whole.
    FURI_LOG_I(
        TAG,
        "dump held config=%u marker=%u password=%u",
        key.parts.config,
        key.parts.marker,
        key.parts.password);

    // Return ignored: an accepted unusual prefix lands here too, and set_key puts the ID in
    // hand either way - that is what makes the acceptance mean something.
    varta_app_set_key(app, key.id);
    return true;
}

static bool varta_app_scene_select_key_load(VartaApp* app) {
    // How applications/main/lfrfid tells these same two file types apart in this same folder.
    const bool dump = furi_string_end_withi(app->path, T5577_FILE_EXTENSION);

    return dump ? varta_app_scene_select_key_load_dump(app) :
                  varta_app_scene_select_key_load_rfid(app);
}

void varta_app_scene_select_key_on_enter(void* context) {
    VartaApp* app = context;

    if(furi_string_empty(app->path)) {
        furi_string_set(app->path, VARTA_APP_FOLDER);
    }

    DialogsFileBrowserOptions options;
    dialog_file_browser_set_basic_options(&options, VARTA_BROWSE_EXTENSIONS, &I_varta_10px);
    options.base_path = VARTA_APP_FOLDER;

    if(dialog_file_browser_show(app->dialogs, app->path, app->path, &options) &&
       varta_app_scene_select_key_load(app)) {
        scene_manager_next_scene(app->scene_manager, VartaAppSceneKeyMenu);
        return;
    }

    // Nothing was opened, so there is nothing for Back to come back to: this scene is the
    // browser, and re-entering it would open it again on the file just declined.
    scene_manager_search_and_switch_to_previous_scene(app->scene_manager, VartaAppSceneStart);
}

bool varta_app_scene_select_key_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);

    return false;
}

void varta_app_scene_select_key_on_exit(void* context) {
    UNUSED(context);
}
