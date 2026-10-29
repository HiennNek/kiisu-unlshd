#include "varta_app.h"

#define TAG "VartaApp"

static bool varta_app_custom_event_callback(void* context, uint32_t event) {
    furi_assert(context);
    VartaApp* app = context;
    return scene_manager_handle_custom_event(app->scene_manager, event);
}

static bool varta_app_back_event_callback(void* context) {
    furi_assert(context);
    VartaApp* app = context;
    return scene_manager_handle_back_event(app->scene_manager);
}

// loading.c's own input callback returns true for every key, Back included, so work in progress
// could not be left until it reached a verdict. This one releases Back to the view dispatcher's
// navigation callback - which navigates on a short or long press and drops the rest - and keeps
// the other keys inert as the module does.
//
// Leaving early is as safe as leaving a result screen: both scenes that show this view already
// undo in their on_exit everything they started. VartaAppViewLoading is where that contract is
// written down.
//
// The wait differs by scene. Check Reader's step only snapshots the detector, so Back lands on
// the next pass of the loop. Read blocks the loop inside a step, so the press waits that step
// out - see VartaApp::step_timer for how long one is.
static bool varta_app_loading_input_callback(InputEvent* event, void* context) {
    UNUSED(context);
    return event->key != InputKeyBack;
}

static void varta_app_step_timer_callback(void* context) {
    furi_assert(context);
    VartaApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, VartaAppCustomEventStep);
}

void varta_app_step_later(VartaApp* app, uint32_t delay_ms) {
    furi_timer_start(app->step_timer, furi_ms_to_ticks(delay_ms));
}

void varta_app_step_stop(VartaApp* app) {
    furi_timer_stop(app->step_timer);
}

static void varta_app_deadline_timer_callback(void* context) {
    furi_assert(context);
    VartaApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, VartaAppCustomEventDeadline);
}

void varta_app_deadline_later(VartaApp* app, uint32_t delay_ms) {
    furi_timer_start(app->deadline_timer, furi_ms_to_ticks(delay_ms));
}

void varta_app_deadline_stop(VartaApp* app) {
    furi_timer_stop(app->deadline_timer);
}

void varta_app_worker_stop(VartaApp* app) {
    if(!app->worker_running) return;

    app->worker_running = false;
    lfrfid_worker_stop(app->worker);
    lfrfid_worker_stop_thread(app->worker);
}

void varta_app_say(VartaApp* app, const char* header, const char* text) {
    DialogMessage* message = dialog_message_alloc();
    dialog_message_set_header(message, header, 64, 10, AlignCenter, AlignTop);
    // y=20, the same origin varta_app_warn() uses: at 32 a third line lands at y=54, under
    // the button box, which dialog_ex draws after the text and so paints straight over.
    dialog_message_set_text(message, text, 64, 20, AlignCenter, AlignTop);
    dialog_message_set_buttons(message, NULL, "OK", NULL);
    dialog_message_show(app->dialogs, message);
    dialog_message_free(message);
}

bool varta_app_warn(VartaApp* app, const char* header, const char* text, const char* accept) {
    DialogMessage* message = dialog_message_alloc();

    dialog_message_set_header(message, header, 64, 4, AlignCenter, AlignTop);
    dialog_message_set_text(message, text, 64, 20, AlignCenter, AlignTop);
    dialog_message_set_buttons(message, "Edit", NULL, accept);

    const DialogMessageButton pressed = dialog_message_show(app->dialogs, message);
    dialog_message_free(message);

    // Logged because it is the one decision that changes what later screens may claim, and
    // the modal is gone by the time anyone asks why.
    const bool accepted = pressed == DialogMessageButtonRight;
    FURI_LOG_I(TAG, "%s: %s", header, accepted ? "accepted" : "declined");

    return accepted;
}

void varta_app_clear_key(VartaApp* app) {
    app->id_read = false;
    memset(app->id, 0, sizeof(app->id));
    memset(&app->broadcast, 0, sizeof(app->broadcast));
}

bool varta_app_set_key(VartaApp* app, const uint8_t* id) {
    memcpy(app->id, id, VARTA_ID_SIZE);
    app->id_read = true;

    // Written even for a prefix Varta does not issue, so the group is never half a key.
    // Costs nothing: EM4100 encodes any five bytes, so neither furi_check in varta_broadcast()
    // can fire on junk, and the dict is the app's own.
    varta_broadcast(app->dict, app->id, &app->broadcast);

    return varta_id_known(app->id);
}

bool varta_app_take_read_id(VartaApp* app) {
    varta_app_clear_key(app);

    const ProtocolId protocol = app->read_protocol;

    // Any of the three EM4100 clock variants. The ID is the same five bytes whichever rate it
    // arrived at, and a key is built at the one rate the reader follows regardless.
    if(protocol != LFRFIDProtocolEM4100 && protocol != LFRFIDProtocolEM4100_32 &&
       protocol != LFRFIDProtocolEM4100_16) {
        return false;
    }

    uint8_t id[VARTA_ID_SIZE];
    protocol_dict_get_data(app->dict, protocol, id, sizeof(id));

    return varta_app_set_key(app, id);
}

void varta_app_id_text(const VartaApp* app, FuriString* text) {
    furi_string_reset(text);

    for(size_t i = 0; i < sizeof(app->id); i++) {
        furi_string_cat_printf(text, i ? " %02X" : "%02X", app->id[i]);
    }
}

static VartaApp* varta_app_alloc(void) {
    VartaApp* app = malloc(sizeof(VartaApp));

    app->gui = furi_record_open(RECORD_GUI);
    app->storage = furi_record_open(RECORD_STORAGE);
    app->dialogs = furi_record_open(RECORD_DIALOGS);
    app->notifications = furi_record_open(RECORD_NOTIFICATION);

    app->scene_manager = scene_manager_alloc(&varta_app_scene_handlers, app);
    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(
        app->view_dispatcher, varta_app_custom_event_callback);
    view_dispatcher_set_navigation_event_callback(
        app->view_dispatcher, varta_app_back_event_callback);
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    // Loading rather than a Popup: its animation is driven by the GUI thread, so it keeps moving
    // while this one is blocked inside a read, and it copies the label instead of keeping it by
    // pointer. A popup cannot do either, and a read blocks for most of a second at a time.
    app->loading = loading_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, VartaAppViewLoading, loading_get_view(app->loading));
    view_set_input_callback(loading_get_view(app->loading), varta_app_loading_input_callback);

    app->submenu = submenu_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, VartaAppViewSubmenu, submenu_get_view(app->submenu));

    app->text_input = text_input_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, VartaAppViewTextInput, text_input_get_view(app->text_input));

    app->byte_input = byte_input_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, VartaAppViewByteInput, byte_input_get_view(app->byte_input));

    app->widget = widget_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, VartaAppViewWidget, widget_get_view(app->widget));

    app->var_item_list = variable_item_list_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher,
        VartaAppViewVarItemList,
        variable_item_list_get_view(app->var_item_list));

    // The whole table: the worker feeds every decoder a tag's envelope could belong to, and
    // lfrfid_dict_file_load() can come back with any protocol in it. Indexing alone would be
    // satisfied by the three EM4100 variants.
    app->dict = protocol_dict_alloc(lfrfid_protocols, LFRFIDProtocolMax);
    app->worker = lfrfid_worker_alloc(app->dict);
    // No app->emulate here: zero-fill gives the emulate scene the NULL it expects, and the
    // 18 KB belongs to the scene that uses it. See VartaApp::emulate.
    app->emulate_mix = varta_emulate_mix_load();
    app->reader = varta_reader_alloc();

    // One shot, re-armed by whichever scene wants another step: a periodic timer queues ticks
    // behind steps that take most of a second, and the backlog outlives the scene.
    app->step_timer = furi_timer_alloc(varta_app_step_timer_callback, FuriTimerTypeOnce, app);
    app->deadline_timer =
        furi_timer_alloc(varta_app_deadline_timer_callback, FuriTimerTypeOnce, app);

    app->path = furi_string_alloc();
    // The one field zero-fill gets wrong: PROTOCOL_NO is -1, and zero is a valid protocol index.
    app->read_protocol = PROTOCOL_NO;

    return app;
}

static void varta_app_free(VartaApp* app) {
    furi_timer_free(app->step_timer);
    furi_timer_free(app->deadline_timer);

    // Normally already freed by the scene's on_exit, which Back always runs. Guarded rather
    // than trusted, because leaving it to scene bookkeeping is how a handle survives its
    // owner.
    if(app->emulate) varta_emulate_free(app->emulate);
    varta_reader_free(app->reader);
    lfrfid_worker_free(app->worker);
    protocol_dict_free(app->dict);

    view_dispatcher_remove_view(app->view_dispatcher, VartaAppViewLoading);
    view_dispatcher_remove_view(app->view_dispatcher, VartaAppViewSubmenu);
    view_dispatcher_remove_view(app->view_dispatcher, VartaAppViewTextInput);
    view_dispatcher_remove_view(app->view_dispatcher, VartaAppViewByteInput);
    view_dispatcher_remove_view(app->view_dispatcher, VartaAppViewWidget);
    view_dispatcher_remove_view(app->view_dispatcher, VartaAppViewVarItemList);

    loading_free(app->loading);
    submenu_free(app->submenu);
    text_input_free(app->text_input);
    byte_input_free(app->byte_input);
    widget_free(app->widget);
    variable_item_list_free(app->var_item_list);

    view_dispatcher_free(app->view_dispatcher);
    scene_manager_free(app->scene_manager);

    furi_string_free(app->path);

    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_DIALOGS);
    furi_record_close(RECORD_STORAGE);
    furi_record_close(RECORD_GUI);

    free(app);
}

int32_t varta_app(void* p) {
    UNUSED(p);

    VartaApp* app = varta_app_alloc();

    if(!storage_simply_mkdir(app->storage, VARTA_APP_FOLDER)) {
        FURI_LOG_W(TAG, "Could not make %s", VARTA_APP_FOLDER);
    }

    scene_manager_next_scene(app->scene_manager, VartaAppSceneStart);

    view_dispatcher_run(app->view_dispatcher);

    varta_app_free(app);

    return 0;
}
