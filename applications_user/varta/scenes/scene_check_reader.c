#include "../varta_app.h"

#define TAG "VartaReader"

/** How long to listen before giving up, in milliseconds.
 *
 * A reader interrogating with no tag present repeats about thirty times a second, so
 * VARTA_READER_FRAMES_AGREE agreeing frames is a fifth of a second once the coupling is right.
 * The rest of this is time to find the coupling.
 */
#define VARTA_CHECK_TIMEOUT_MS 12000

/** How often the screen is refreshed while listening. */
#define VARTA_CHECK_TICK_MS 100

static void varta_app_scene_check_reader_doing(VartaApp* app, const VartaReaderResult* result) {
    // 91 px in FontPrimary against the 92 px the Loading label centres in - measured, and the
    // longest this screen may say.
    const char* stage = "Hold on the reader";

    if(result->frames) stage = "Decoding";
    if(result->agreed) stage = "Confirming";

    loading_set_text(app->loading, stage);
    // Agreement, not elapsed time, so the bar is positioning feedback rather than a countdown.
    // It only climbs: `agreed` is the longest run reached, so losing the coupling stalls the bar
    // instead of retracting progress the detector really made.
    loading_set_progress(app->loading, (float)result->agreed / (float)VARTA_READER_FRAMES_AGREE);
}

// The one record of a run. For VartaReaderOther the password is a finding about someone else's
// installation and it lives only until Back, so the log is the copy that outlasts it.
static bool varta_app_scene_check_reader_conclude(VartaApp* app) {
    const VartaReaderResult* result = &app->reader_result;

    // Unguarded: varta_reader_read() zeroes the result and fills the command fields only after
    // the opcode and lock gates, so a verdict that did not decode already carries zeroes here.
    FURI_LOG_I(
        TAG,
        "verdict %u pwd %08lX blk %u page %u | %lu stalls %lu gaps %lu frames "
        "%u symbols%s | %u last-frame bits %u agreed | %lu / %lu us",
        result->verdict,
        (unsigned long)result->password,
        result->block,
        result->page1 ? 1 : 0,
        (unsigned long)result->stalls,
        (unsigned long)result->gaps,
        (unsigned long)result->frames,
        result->symbol_burst,
        result->symbol_burst_capped ? "+" : "",
        result->bits,
        result->agreed,
        (unsigned long)result->short_us,
        (unsigned long)result->long_us);

    scene_manager_next_scene(app->scene_manager, VartaAppSceneCheckReaderResult);

    return true;
}

void varta_app_scene_check_reader_on_enter(void* context) {
    VartaApp* app = context;

    // A fresh look deserves its own result screen, chime and all.
    scene_manager_set_scene_state(app->scene_manager, VartaAppSceneCheckReaderResult, 0);

    VartaReaderResult empty = {0};
    varta_app_scene_check_reader_doing(app, &empty);
    view_dispatcher_switch_to_view(app->view_dispatcher, VartaAppViewLoading);

    notification_message(app->notifications, &sequence_blink_start_cyan);

    // Defensively, as the read and emulate scenes both do: the worker holds TIM2 and its
    // interrupt slot, and furi_hal_bus_enable() furi_checks rather than failing. Unreachable
    // today - this scene is only entered from the menu - but the guarantee should not rest on
    // one menu edge never being added.
    varta_app_worker_stop(app);
    varta_reader_start(app->reader);

    varta_app_deadline_later(app, VARTA_CHECK_TIMEOUT_MS);
    varta_app_step_later(app, VARTA_CHECK_TICK_MS);
}

bool varta_app_scene_check_reader_on_event(void* context, SceneManagerEvent event) {
    VartaApp* app = context;

    if(event.type != SceneManagerEventTypeCustom) return false;

    if(event.event == VartaAppCustomEventDeadline) {
        // Out of time. Whatever was heard is the answer, including nothing at all - and the
        // detector is stopped before it is read, so no frame can land between the two.
        varta_reader_stop(app->reader);
        varta_reader_read(app->reader, &app->reader_result);

        return varta_app_scene_check_reader_conclude(app);
    }

    if(event.event == VartaAppCustomEventStep) {
        VartaReaderResult result;
        varta_reader_read(app->reader, &result);

        // Six agreeing frames is as sure as this gets, and listening on could only spoil it -
        // a frozen screen is what a user reads off a door. The result just proved confirmed is
        // the one shown, rather than a second read taken after the stop: the detector is live
        // across those few instructions, and a malformed frame landing in them would report
        // "Can't decode" for a run this function has already decided was confirmed.
        // No deadline_stop: scene_manager runs this scene's on_exit before the next scene's
        // on_enter, and that stops both timers.
        if(varta_reader_confirmed(&result)) {
            varta_reader_stop(app->reader);
            app->reader_result = result;

            return varta_app_scene_check_reader_conclude(app);
        }

        varta_app_scene_check_reader_doing(app, &result);
        varta_app_step_later(app, VARTA_CHECK_TICK_MS);

        return true;
    }

    return false;
}

void varta_app_scene_check_reader_on_exit(void* context) {
    VartaApp* app = context;

    varta_app_step_stop(app);
    varta_app_deadline_stop(app);
    varta_reader_stop(app->reader);

    loading_reset_progress(app->loading);
    notification_message(app->notifications, &sequence_blink_stop);
}
