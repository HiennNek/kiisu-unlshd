#include "../varta_app.h"

typedef enum {
    ReadStepId, // listening for the key's EM4100 broadcast
    ReadStepOpen, // finding out whether the tag takes a plain block read
    ReadStepPresent, // and whether it is still on the coil, before any password goes out
    ReadStepMarker,
    ReadStepCount, // not a step: what makes the table below checkable
} ReadStep;

/** What a step tells the user, and how many reads it may spend.
 *
 * The stage line is phrased as what is happening to their key, not as the question the code is
 * asking the tag - "Protected?" and "Still there?" were the latter. ReadStepPresent's says what
 * to do rather than what is being checked, because it is the step where the fix is prevention:
 * the others can be retried, a tag lifted mid-read cannot be un-lifted.
 *
 * "Checking protection" is broken by hand because it has to be: loading.c centres the label in a
 * 92 pixel budget in FontPrimary (helvB08) and hyphenates anything wider, so the unbroken string
 * at 98 px would draw as "Checking pro-". "Looking for a key" is 85 px. Characters do not
 * predict pixels here - rewording needs measuring.
 *
 * The read counts are what the progress bar is a fraction of. Counted in reads rather than steps
 * because the steps are nothing like equal: a failing detect sweeps three bit rates at about
 * 600 ms each, so the protection check alone is some five seconds. A bar that moved only between
 * steps would sit still for all of it and look hung.
 */
typedef struct {
    const char* stage;
    uint8_t reads;
} VartaReadStep;

static const VartaReadStep varta_read_steps[] = {
    [ReadStepId] = {"Looking for a key", 1},
    [ReadStepOpen] = {"Checking\nprotection", VARTA_OPEN_TRIES},
    [ReadStepPresent] = {"Hold it still", 1},
    [ReadStepMarker] = {"Verifying key", VARTA_MARKER_TRIES},
};

// Anchored to the enum's extent, not to its last member: ReadStepMarker + 1 would still hold
// after a step was appended and its row forgotten, which is the case worth catching.
static_assert(COUNT_OF(varta_read_steps) == ReadStepCount, "a row per read step");

static void varta_app_scene_read_callback(
    LFRFIDWorkerReadResult result,
    ProtocolId protocol,
    void* context) {
    VartaApp* app = context;

    if(result != LFRFIDWorkerReadDone) return;

    // Written on the worker's thread and read on the loop's; the queue the event goes through is
    // what orders the two.
    app->read_protocol = protocol;
    view_dispatcher_send_custom_event(app->view_dispatcher, VartaAppCustomEventRead);
}

// Name the work in progress and say how much of it is done. The animation is the GUI thread's,
// so it keeps moving while this thread is inside a read; these say which read, and how far in.
static void varta_app_scene_read_doing(VartaApp* app) {
    uint32_t done = app->tries;
    uint32_t total = 0;

    for(size_t step = 0; step < COUNT_OF(varta_read_steps); step++) {
        if(step < app->step) done += varta_read_steps[step].reads;
        total += varta_read_steps[step].reads;
    }

    loading_set_text(app->loading, varta_read_steps[app->step].stage);
    loading_set_progress(app->loading, (float)done / (float)total);
}

// Done: show the verdict.
static bool varta_app_scene_read_verdict(VartaApp* app, VartaVerdict verdict) {
    app->verdict = verdict;
    scene_manager_next_scene(app->scene_manager, VartaAppSceneReadResult);

    return true;
}

// Another go at the same step, keeping the step and the try count: the tries are what the
// progress is made of, so a retry that showed nothing is the one that looks hung.
static bool varta_app_scene_read_retry(VartaApp* app) {
    varta_app_scene_read_doing(app);
    varta_app_step_later(app, 10);

    return true;
}

// On to the next step, with its own count of tries and its own line on screen.
static bool varta_app_scene_read_go(VartaApp* app, ReadStep step) {
    app->step = step;
    app->tries = 0;

    return varta_app_scene_read_retry(app);
}

void varta_app_scene_read_on_enter(void* context) {
    VartaApp* app = context;

    app->step = ReadStepId;
    app->tries = 0;
    app->verdict = VartaVerdictNoTag;
    app->read_protocol = PROTOCOL_NO;
    // A read that times out never reaches the gate, so the key in hand is dropped here instead.
    varta_app_clear_key(app);
    // A fresh read deserves its own result screen, chime and all.
    scene_manager_set_scene_state(app->scene_manager, VartaAppSceneReadResult, 0);

    varta_app_scene_read_doing(app);
    view_dispatcher_switch_to_view(app->view_dispatcher, VartaAppViewLoading);

    notification_message(app->notifications, &sequence_blink_start_cyan);

    lfrfid_worker_start_thread(app->worker);
    app->worker_running = true;
    lfrfid_worker_read_start(
        app->worker, LFRFIDWorkerReadTypeASKOnly, varta_app_scene_read_callback, app);

    varta_app_deadline_later(app, VARTA_READ_TIMEOUT_MS);
}

// The worker heard something. Everything below this point addresses the tag directly, so the
// worker has to let go of the coil first - see varta_app_worker_stop().
static bool varta_app_scene_read_on_id(VartaApp* app) {
    varta_app_deadline_stop(app);
    varta_app_worker_stop(app);

    if(!varta_app_take_read_id(app)) {
        // Which of the two it was decides what the result screen can honestly say: there is no
        // prefix to complain about on a tag that was never an EM4100.
        return varta_app_scene_read_verdict(
            app, app->id_read ? VartaVerdictNotVarta : VartaVerdictNotEm4100);
    }

    // A key is RF/64 by definition - VARTA_CONFIG says so - so an ID at another rate is already
    // known not to be one. Stopping here keeps the password off a tag that could never have
    // answered the RF/64 marker read anyway. The ID is still worth keeping: a dump composed from
    // it is a proper RF/64 key.
    if(app->read_protocol != LFRFIDProtocolEM4100) {
        return varta_app_scene_read_verdict(app, VartaVerdictOtherRate);
    }

    // No t5577_field_reset() here, tempting as a leaked nesting count is to guard against: it
    // ends in furi_hal_rfid_tim_read_stop(), which disables TIM1's bus, and furi_hal_bus_disable()
    // furi_checks that the peripheral is enabled. With the carrier down - which it is at this
    // point - that call is a crash, not a safety net.

    // Up once and held across the steps below, so the tag keeps power between them rather than
    // being re-powered for each, and the envelope detector stays settled.
    t5577_field_start();
    app->field_up = true;

    return varta_app_scene_read_go(app, ReadStepOpen);
}

// One step of whatever the read is on. Each step owns its own exit: another try, the next step,
// or a verdict.
static bool varta_app_scene_read_on_step(VartaApp* app) {
    switch((ReadStep)app->step) {
    case ReadStepId:
        // The step timer is never armed during the listening window, so nothing should arrive
        // here. Ignored rather than acted on.
        return true;

    case ReadStepOpen: {
        // The gate that decides whether VARTA_PASSWORD goes on the wire. A tag that answers a
        // plain block read has no password, so it is a clone rather than a key - and the one tag
        // the password must not reach; see its warning in varta.h for what it would do.
        T5577Config open = {0};

        if(t5577_detect(&open)) return varta_app_scene_read_verdict(app, VartaVerdictOpen);

        // Three refusals is still only absence of evidence, which is why the next step asks
        // whether the tag is even there before acting on it.
        if(++app->tries < VARTA_OPEN_TRIES) return varta_app_scene_read_retry(app);

        return varta_app_scene_read_go(app, ReadStepPresent);
    }

    case ReadStepPresent:
        // Asked without sending a password, so being sure costs nothing.
        if(!varta_tag_present(&app->broadcast)) {
            return varta_app_scene_read_verdict(app, VartaVerdictLost);
        }

        return varta_app_scene_read_go(app, ReadStepMarker);

    case ReadStepMarker:
        switch(varta_check_marker(&app->broadcast)) {
        case VartaMarkerHeld:
            return varta_app_scene_read_verdict(app, VartaVerdictGenuine);
        case VartaMarkerOther:
            return varta_app_scene_read_verdict(app, VartaVerdictOtherMarker);
        case VartaMarkerQuiet:
            return varta_app_scene_read_verdict(app, VartaVerdictLost);
        case VartaMarkerUnanswered:
            // Worth asking again, and the verdict if it never answers.
            if(++app->tries < VARTA_MARKER_TRIES) return varta_app_scene_read_retry(app);

            return varta_app_scene_read_verdict(app, VartaVerdictUnanswered);
        }

        return varta_app_scene_read_verdict(app, VartaVerdictUnanswered);

    case ReadStepCount:
        // Not a step. Listed rather than defaulted, so -Wswitch keeps guarding the real ones.
        break;
    }

    return true;
}

bool varta_app_scene_read_on_event(void* context, SceneManagerEvent event) {
    VartaApp* app = context;

    if(event.type != SceneManagerEventTypeCustom) return false;

    switch(event.event) {
    case VartaAppCustomEventDeadline:
        // Its own timer, so a tick already posted when the answer arrived is identifiable here.
        if(app->step != ReadStepId) return true;

        // Nothing decoded in the window. The carrier was up throughout, but no frame and no
        // password has been addressed to anything.
        varta_app_worker_stop(app);
        return varta_app_scene_read_verdict(app, VartaVerdictNoTag);

    case VartaAppCustomEventRead:
        return varta_app_scene_read_on_id(app);

    case VartaAppCustomEventStep:
        return varta_app_scene_read_on_step(app);

    default:
        return false;
    }
}

void varta_app_scene_read_on_exit(void* context) {
    VartaApp* app = context;

    varta_app_step_stop(app);
    varta_app_deadline_stop(app);
    varta_app_worker_stop(app);

    // The bar belongs to this read; whatever shows the Loading view next starts without one.
    loading_reset_progress(app->loading);

    // Kept rather than stopped unconditionally: an unbalanced t5577_field_stop() is logged and
    // ignored, not fatal, so this is log hygiene for the paths that never raised the field at all
    // - the listening deadline, and every verdict reached before the T5577 half.
    if(app->field_up) {
        app->field_up = false;
        t5577_field_stop();
    }

    notification_message(app->notifications, &sequence_blink_stop);
}
