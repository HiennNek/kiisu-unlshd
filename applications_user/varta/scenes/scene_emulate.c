#include "../varta_app.h"
#include "../varta_emulate.h"
#include <varta_icons.h>
#include <dolphin/dolphin.h>

#define TAG "Varta"

// The shape every emulate screen in the firmware has - dolphin on the left, what is being sent on
// the right. Copied rather than shared: a FAP cannot reach the firmware's icons, so the asset
// lives in this app's own images/.
static void varta_app_scene_emulate_show(VartaApp* app, bool live) {
    const VartaEmulateMix* mix = varta_emulate_mix(app->emulate_mix);

    FuriString* id = furi_string_alloc();
    varta_app_id_text(app, id);

    FuriString* text = furi_string_alloc_set(live ? "\e#Emulating\e#\n" : "\e#Stopped\e#\n");

    furi_string_cat_printf(text, "[%s]\n%s", mix->name, furi_string_get_cstr(id));

    furi_string_free(id);

    widget_reset(app->widget);

    // The emulating dolphin is the firmware's "transmitting" icon and the loudest thing on the
    // screen, so it may not outlive the stream it stands for.
    if(live) {
        widget_add_icon_element(app->widget, 0, 0, &I_NFC_dolphin_emulation_51x64);
    } else {
        widget_add_icon_element(app->widget, 3, 12, &I_WarningDolphinFlip_45x42);
    }
    widget_add_text_box_element(
        app->widget, 51, 6, 79, 50, AlignCenter, AlignTop, furi_string_get_cstr(text), false);

    furi_string_free(text);

    view_dispatcher_switch_to_view(app->view_dispatcher, VartaAppViewWidget);
}

// The stream goes out through our own DMA - the lfrfid worker would send the same encoder's
// output through the same timer, and having one owner of the coil removes a hand-off.
static bool varta_app_scene_emulate_run(VartaApp* app) {
    const VartaEmulateMix* mix = varta_emulate_mix(app->emulate_mix);

    varta_app_worker_stop(app);
    varta_emulate_stop(app->emulate);

    if(varta_emulate_start(app->emulate, app->dict, app->id, mix->id_frames, mix->marker_words)) {
        return true;
    }

    // Rendering is pure arithmetic on fixed-length frames, so a failure here is a bug and not a
    // tag that moved. Say so rather than emulating nothing in silence.
    FURI_LOG_E(
        TAG,
        "could not render %s (%u ID frames, %u marker words)",
        mix->name,
        mix->id_frames,
        mix->marker_words);
    varta_app_say(app, "Emulate", "Could not build\nthis stream");
    return false;
}

void varta_app_scene_emulate_on_enter(void* context) {
    VartaApp* app = context;

    app->emulate = varta_emulate_alloc();

    // Everything that claims the coil is set from one answer, so a stream that fails to build
    // cannot leave the LED or the screen saying otherwise - the dialog reporting it is dismissed
    // before this screen is even visible, which would make the blink the user's last impression.
    const bool live = varta_app_scene_emulate_run(app);

    notification_message(
        app->notifications, live ? &sequence_blink_start_magenta : &sequence_blink_stop);
    varta_app_scene_emulate_show(app, live);

    if(live) dolphin_deed(DolphinDeedRfidEmulate);
}

bool varta_app_scene_emulate_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);

    // The mode is a setting now, so this scene sends one stream and offers no choices.
    return false;
}

void varta_app_scene_emulate_on_exit(void* context) {
    VartaApp* app = context;

    varta_app_worker_stop(app);
    varta_emulate_free(app->emulate); // stops the stream on its way out
    app->emulate = NULL;

    notification_message(app->notifications, &sequence_blink_stop);
    widget_reset(app->widget);
}
