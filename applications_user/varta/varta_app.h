/**
 * @file varta_app.h
 * @brief Reading Varta-M keys, and building one from an EM4100 ID.
 */
#pragma once

#include <furi.h>
#include <gui/gui.h>
#include <gui/scene_manager.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/loading.h>
#include <gui/modules/submenu.h>
#include <gui/modules/text_input.h>
#include <gui/modules/byte_input.h>
#include <gui/modules/widget.h>
#include <gui/modules/variable_item_list.h>
#include <dialogs/dialogs.h>
#include <notification/notification_messages.h>
#include <storage/storage.h>

#include <lfrfid/lfrfid_worker.h>
#include <lfrfid/lfrfid_dict_file.h>

#include "varta.h"
#include "varta_emulate.h"
#include "varta_reader.h"
#include "scenes/scenes.h"

// The lfrfid folder, not one beside it: what this app saves is a T5577 dump, and that is where
// the T5577 app and the 125 kHz saved list both look for one.
#define VARTA_APP_FOLDER EXT_PATH("lfrfid")
#define VARTA_NAME_MAX   32

/** Top of the button row. elements_button_left/center/right - the three a widget button can
 * draw - all anchor at canvas_height() - 12, so this is a firmware fact rather than a taste: a
 * button box covers rows 52..63 and is drawn after the text, so anything sharing those rows
 * under a button is painted over.
 */
#define VARTA_BUTTON_ROW_Y 52

/** Where an icon goes on a screen that also has a button row: right-aligned, and
 * bottom-aligned a few rows above the buttons.
 *
 * Derived from the icon rather than picked per screen, for the screens that can take it: the
 * Check Reader verdict and Info. The Read verdict keeps its own placement, because its icon is
 * deliberately overlapped with the ID row beside it. 127 is what the widest icon was already
 * hand-placed against, so this adopts the existing convention rather than a fourth one.
 *
 * Without the gap an icon ends on the last row before the buttons and the two read as one
 * shape - the buttons are drawn last, so there is no overlap to see, only no air.
 */
#define VARTA_ICON_GAP     4
#define VARTA_ICON_RIGHT   127
#define VARTA_ICON_X(icon) (VARTA_ICON_RIGHT - icon_get_width(icon))
#define VARTA_ICON_Y(icon) (VARTA_BUTTON_ROW_Y - VARTA_ICON_GAP - icon_get_height(icon))

/** Plain reads before concluding a tag is protected.
 *
 * The one judgement that decides whether VARTA_PASSWORD goes on the wire, so it is not left to a
 * single read: a badly coupled tag refuses a block read exactly as a protected one does. Same
 * count, used the same way round, as LFRFID_WORKER_SET_PASS_OPEN_TRIES in
 * lib/lfrfid/lfrfid_worker_modes.c - a private define there, so nothing enforces the match.
 */
#define VARTA_OPEN_TRIES 3

/** Marker reads before giving up on a tag that keeps answering with its own broadcast.
 *
 * An addressed read that does not land looks nothing like a wrong marker, but one attempt
 * cannot tell them apart either.
 */
#define VARTA_MARKER_TRIES 3

/** How long to listen for a key's EM4100 broadcast before giving up on it. */
#define VARTA_READ_TIMEOUT_MS 5000

/** What a read made of the tag on the coil.
 *
 * Not ordered by anything: the first six are reasons a tag never reached the marker read, and
 * the last three are what the marker read came back with. Nothing compares these.
 */
typedef enum {
    VartaVerdictNoTag,
    VartaVerdictNotEm4100, // something was read, and it was not an EM4100 ID at all
    VartaVerdictNotVarta, // an EM4100 ID whose prefix is not one Varta issues
    VartaVerdictOtherRate, // a Varta ID, but not at RF/64, so it cannot be a key
    VartaVerdictOpen, // right prefix, but the tag answers plain reads: an unprotected clone
    VartaVerdictLost, // stopped answering between the ID read and the marker read
    VartaVerdictGenuine, // block 6 holds the marker
    VartaVerdictOtherMarker, // answered the addressed read, with a different word
    VartaVerdictUnanswered, // only its own broadcast came back: the read never landed
} VartaVerdict;

typedef struct {
    Gui* gui;
    Storage* storage;
    DialogsApp* dialogs;
    NotificationApp* notifications;
    SceneManager* scene_manager;
    ViewDispatcher* view_dispatcher;

    Loading* loading;
    Submenu* submenu;
    TextInput* text_input;
    ByteInput* byte_input;
    Widget* widget;
    VariableItemList* var_item_list;

    // The worker's thread is started and stopped per scene; both outlive every scene.
    ProtocolDict* dict;
    LFRFIDWorker* worker;
    bool worker_running;
    // The emulate scene owns the coil directly rather than through the worker, so the worker
    // must be stopped before it starts. Which mix of ID and marker is on it is emulate_mix.
    //
    // NULL outside that scene: this is 18 KB, and nothing else reaches it.
    VartaEmulate* emulate;
    // Which emulation mode the Settings scene has chosen. App-wide rather than the emulate
    // scene's, because Settings writes it and Emulate reads it.
    uint8_t emulate_mix;
    // Listening to a reader's downlink. Claims TIM2, which the emulator also wants, so the
    // two can never run at once - they are different scenes and neither is reachable from
    // the other.
    VartaReader* reader;
    VartaReaderResult reader_result;

    // The read scene holds the field up across its own step ticks, so the tag keeps power
    // between tries. Tracked because Back can leave the scene with it up.
    bool field_up;

    // The key in hand, however it was come by: read off a tag, typed in, or loaded from a key
    // file. Everything else about a Varta key is constant, so this is the whole of it.
    uint8_t id[VARTA_ID_SIZE];
    // Whether five bytes of ID were decoded at all, which is not the same question as whether
    // they carry a prefix Varta issues - varta_app_has_id() answers that one from the ID itself.
    // A screen that can tell them apart is the difference between "not a Varta ID" and "not an
    // EM4100 key". Not derivable: a typed-in ID has no protocol, and all-zero is a legal read.
    bool id_read;
    // Edited in place by the byte input, and deliberately not app->id: a half-typed ID must not
    // destroy the key in hand when the user backs out.
    uint8_t id_bytes[VARTA_ID_SIZE];
    // What an ID broadcasts, worked out when the ID is taken in hand. Every later read needs it
    // to tell the tag's own frame from an answer; caching it keeps one owner of that answer
    // rather than leaving each read to derive it mid-sequence.
    VartaBroadcast broadcast;

    // What put an ID in the dict: a read, where the worker thread writes it and the event it
    // posts carries the ordering, or a key file loaded on this thread.
    ProtocolId read_protocol;
    VartaVerdict verdict;

    FuriString* path;
    char name[VARTA_NAME_MAX];

    // One step per timer tick so Back is answerable between them, which is as much as can be
    // had: a step is a whole t5577_detect(), and a failing one sweeps three rates at about half
    // a second each, so the Loading view's own animation is what says the app is still alive.
    FuriTimer* step_timer;
    // The listening window, on a timer of its own rather than sharing the step tick. Re-arming a
    // one-shot cancels the timer but not an event it has already posted, so one timer behind one
    // event cannot tell a stale deadline from a live step - and that stale tick would spend a
    // step on a read taken the instant the carrier came up, which is one fewer chance for the
    // tag to answer before a password is sent on the strength of its silence.
    FuriTimer* deadline_timer;
    uint8_t step; // where the read has got to, its own meaning per scene
    uint8_t tries; // reads spent on the current step
} VartaApp;

typedef enum {
    // Abortable by contract: varta_app_loading_input_callback() lets Back out of this view, so a
    // scene that shows it must undo in its on_exit everything it started, and must not show it
    // across anything that cannot be abandoned half-done - a T5577 write, say.
    VartaAppViewLoading,
    VartaAppViewSubmenu,
    VartaAppViewTextInput,
    VartaAppViewByteInput,
    VartaAppViewWidget,
    VartaAppViewVarItemList,
} VartaAppView;

// Submenu indices travel down the same uint32_t channel, so everything here has to stay above
// the largest of those; both submenu scenes also bound-check what they accept.
typedef enum {
    VartaAppCustomEventStep = 100, // the step timer fired
    VartaAppCustomEventDeadline, // the listening window closed
    VartaAppCustomEventRead, // the worker heard a key
    VartaAppCustomEventConfirm,
    VartaAppCustomEventUse,
    VartaAppCustomEventRetry,
    VartaAppCustomEventExit, // leave the result behind rather than re-enter what produced it
    VartaAppCustomEventMore, // show the evidence behind a verdict, not "adopt this key"
} VartaAppCustomEvent;

/** Arm the step timer for one more step of whatever the current scene is doing. */
void varta_app_step_later(VartaApp* app, uint32_t delay_ms);

/** Stop the step timer, for a scene being left while a step is still pending. */
void varta_app_step_stop(VartaApp* app);

/** Arm the listening window. */
void varta_app_deadline_later(VartaApp* app, uint32_t delay_ms);

/** Stop the listening window, for a read that got an answer before it closed. */
void varta_app_deadline_stop(VartaApp* app);

/** One dialog shape for everything the app has to say. */
void varta_app_say(VartaApp* app, const char* header, const char* text);

/** Say something the user may overrule, and report whether they did.
 *
 * Left goes back to whatever they were editing, right accepts - the way every confirm in
 * the firmware is laid out. Back counts as declining, so the quiet answer is the
 * cautious one.
 *
 * @param accept label for the right button, which should name the consequence
 * @return true when the user accepted
 */
bool varta_app_warn(VartaApp* app, const char* header, const char* text, const char* accept);

/** Release the coil, for any scene that started the worker.
 *
 * Both the worker and the T5577 driver claim TIM1 and TIM2, and the second claim on either is a
 * furi_check failure rather than merely bad timing - so this has to run before anything addresses
 * a tag, and before the app exits. Idempotent.
 */
void varta_app_worker_stop(VartaApp* app);

/** Whether the key in hand carries a prefix Varta issues.
 *
 * Derived rather than stored: app->id is zeroed when no ID was decoded, and an all-zero ID has no
 * known prefix, so the question is always answerable from the bytes themselves.
 */
static inline bool varta_app_has_id(const VartaApp* app) {
    return varta_id_known(app->id);
}

/** Nothing in hand.
 *
 * app->id, app->id_read and app->broadcast are one value and move together, so this and
 * varta_app_set_key() are the only two things that write them. Written as a pair because they
 * were not: a read that timed out once left the previous key's ID on screen under "Nothing
 * found", with a Use button, because the scene that cleared some of the group did not clear it
 * all.
 */
void varta_app_clear_key(VartaApp* app);

/** Put an ID in hand.
 *
 * Writes all three fields whatever the prefix, so the group is never half a key - a screen
 * reading app->broadcast after a rejected ID gets that ID's broadcast, not the last one's.
 *
 * @return true when the prefix is one Varta issues
 */
bool varta_app_set_key(VartaApp* app, const uint8_t* id);

/** Take the ID out of the dict, for whatever put one there - a read or a .rfid key file.
 *
 * The first of the two gates that stand between a tag and VARTA_PASSWORD; the second is a tag
 * refusing a plain read, which only a tag can answer.
 *
 * @return true when an ID with a known prefix is now in hand
 */
bool varta_app_take_read_id(VartaApp* app);

/** Write app->id into a line, as the dump and the reader both spell it. */
void varta_app_id_text(const VartaApp* app, FuriString* text);
