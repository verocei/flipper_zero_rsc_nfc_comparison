/*
 * RSC NFC Comparison  (v0.1)
 * ------------------------------------------------------------------
 * Reads an NFC card, then reads a card a second time and checks that the
 * second read matches the first one 100% (protocol, type, UID and every
 * byte of data the card gives out without keys).
 *
 * Main screen (same look as the other RSC apps): a big Start button, then
 * "Compare with 1st" (when a first read is kept) and Settings. The version is
 * the last settings row.
 *
 * Flow:
 *   Start          -> 1st read -> "1st read OK" (type + UID)
 *   2nd read       -> result: 100% MATCH / DIFFERENT, with Type / UID / Data
 *                     rows; Up/Down shows both reads side by side.
 *   Next           -> read another card against the same 1st read
 *   New            -> start over with a new 1st read
 *
 * Reading uses the firmware's own NFC scanner and protocol pollers (the same
 * read the built-in NFC app does), but never tries keys or passwords:
 *   - Mifare Ultralight / NTAG: all pages readable without a password, plus
 *     version, signature and counters.
 *   - ISO15693 / SLIX, FeliCa, ST25TB, DESFire, Mifare Plus, NTAG4xx,
 *     Type 4 Tag, ISO14443-3A/3B/4A/4B: whatever the poller reads openly.
 *   - Mifare Classic: sector data needs keys, so only the ISO14443-3A level
 *     (UID, ATQA, SAK) is compared; the type is marked with '*'.
 *   - Bank cards (EMV): only the ISO14443-4A level (UID, ATQA, SAK, ATS).
 * The comparison itself is nfc_device_is_equal(): every field of the read data.
 *
 * Language: English (default), Magyar, Deutsch -- first settings row, labelled
 * "Language" in every language so it can be found.
 *
 * Settings: /ext/apps_data/rsc_nfc_comparison/settings.bin
 */

#include <stddef.h>
#include <furi.h>
#include <gui/gui.h>
#include <gui/view.h>
#include <gui/view_dispatcher.h>
#include <gui/elements.h>
#include <gui/modules/variable_item_list.h>
#include <input/input.h>
#include <storage/storage.h>
#include <notification/notification_messages.h>

#include <nfc/nfc.h>
#include <nfc/nfc_device.h>
#include <nfc/nfc_scanner.h>
#include <nfc/nfc_poller.h>
#include <nfc/protocols/iso14443_3a/iso14443_3a_poller.h>
#include <nfc/protocols/iso14443_3b/iso14443_3b_poller.h>
#include <nfc/protocols/iso14443_4a/iso14443_4a_poller.h>
#include <nfc/protocols/iso14443_4b/iso14443_4b_poller.h>
#include <nfc/protocols/iso15693_3/iso15693_3_poller.h>
#include <nfc/protocols/felica/felica_poller.h>
#include <nfc/protocols/mf_ultralight/mf_ultralight_poller.h>
#include <nfc/protocols/mf_desfire/mf_desfire_poller.h>
#include <nfc/protocols/mf_plus/mf_plus_poller.h>
#include <nfc/protocols/slix/slix_poller.h>
#include <nfc/protocols/st25tb/st25tb_poller.h>
#include <nfc/protocols/ntag4xx/ntag4xx_poller.h>
#include <nfc/protocols/type_4_tag/type_4_tag_poller.h>

#define TAG         "RscNfcCmp"
#define APP_NAME    "RSC NFC Comparison"
#define APP_VERSION "0.2"

#define BASE_DIR      EXT_PATH("apps_data/rsc_nfc_comparison")
#define SETTINGS_PATH EXT_PATH("apps_data/rsc_nfc_comparison/settings.bin")

#define SETTINGS_MAGIC      0x52 // 'R'
#define SETTINGS_VERSION    2
#define SETTINGS_VERSION_V1 1 // same blob without auto_second; still loaded

// "Auto 2nd read": seconds the "1st read OK" screen stays before the 2nd read
// starts by itself (0 = off, wait for the button). The pause leaves time to
// swap cards, so the 1st card is not simply read again at once.
static const uint8_t auto_second_choices[] = {0, 1, 2, 3, 5};
#define AUTO_SECOND_COUNT (sizeof(auto_second_choices) / sizeof(auto_second_choices[0]))

#define TICK_MS        100 // redraw / animation rate while reading
#define READ_ERROR_MS  1500 // how long "read failed" stays on screen
#define NAME_LEN       40
#define UID_STR_LEN    48
#define NOTE_LEN       24

// UI language: index into the L() triples below. English is the default after
// install. No accented letters on screen (the Flipper fonts lack them), so
// German uses ae/oe/ue/ss.
#define LANG_EN    0
#define LANG_HU    1
#define LANG_DE    2
#define LANG_COUNT 3
static uint8_t g_lang = LANG_EN;
#define L(en, hu, de) (g_lang == LANG_HU ? (hu) : g_lang == LANG_DE ? (de) : (en))

static const char* const language_names[LANG_COUNT] = {"English", "Magyar", "Deutsch"};

static const char* const on_off_label[LANG_COUNT][2] = {
    {"Off", "On"},
    {"Ki", "Be"},
    {"Aus", "An"},
};

typedef enum {
    ViewIdMenu = 0,
    ViewIdSettings,
    ViewIdWork,
} ViewId;

// Main screen entries. Start is the big button; the rest are small rows, and
// "Compare with 1st" is only listed while a first read is kept.
typedef enum {
    MenuStart = 0,
    MenuCompare,
    MenuSettings,
    MenuItemCount,
} MenuItem;

typedef enum {
    StateReading = 0, // waiting for / reading a card (step 1 or 2)
    StateRef, // first read done, waiting for the user to start the 2nd read
    StateResult, // comparison shown
} WorkState;

typedef enum {
    DataOk = 0,
    DataDiff,
    DataNa, // not comparable (different card type)
} DataState;

typedef enum {
    PollContinue = 0,
    PollOk,
    PollFail,
} PollResult;

typedef enum {
    CustomEventTick = 1,
    CustomEventDetected,
    CustomEventReadOk,
    CustomEventReadFail,
    CustomEventLanguage,
} CustomEvent;

typedef struct App App;

// Shared model for the custom views: just a back-reference to App.
typedef struct {
    App* app;
} ViewModel;

// One read, ready to show: the data itself plus display strings.
typedef struct {
    NfcDevice* dev;
    char name[NAME_LEN]; // card type, '*' = only the ISO14443 level was read
    char uid[UID_STR_LEN];
    bool uid_level_only; // card type needs keys for its content (Mifare Classic)
} ReadInfo;

struct App {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    View* menu_view;
    View* work_view;
    VariableItemList* settings_list;
    Storage* storage;
    NotificationApp* notif;
    FuriTimer* timer;
    uint32_t tick_freq;
    ViewId current_view;
    uint8_t menu_sel; // index into the visible main-screen items

    // settings
    bool led;
    bool vibro;
    bool beep;
    uint8_t auto_second; // seconds before the 2nd read starts by itself, 0 = off

    // nfc (all owned by the app thread; the callbacks only read these pointers)
    Nfc* nfc;
    NfcScanner* scanner;
    NfcPoller* poller;
    NfcProtocol poll_protocol; // protocol actually read
    volatile NfcProtocol detected; // protocol picked by the scanner callback
    volatile NfcProtocol detected_raw; // what the card really is (for the name)
    volatile bool scan_done; // scanner callback already fired
    volatile bool poll_done; // poller callback already finished

    // work screen
    WorkState state;
    uint8_t step; // 1 or 2
    uint32_t error_until; // tick until which "read failed" is shown
    uint32_t auto_at; // tick at which the 2nd read starts by itself (0 = none)
    uint32_t anim;
    bool blinking; // cyan "reading" blink running
    bool result_led; // green / red result LED lit
    bool backlight_held;

    ReadInfo ref; // 1st read
    ReadInfo cur; // 2nd read
    bool has_ref;

    // result
    bool match;
    bool type_ok;
    bool uid_ok;
    DataState data_state;
    char data_note[NOTE_LEN];
    uint8_t result_page; // 0 = summary, 1 = both reads
    uint32_t compared; // 2nd reads against the current 1st read
    uint32_t matched;
};

/* ---------------- helpers ---------------- */

static uint32_t ms_to_ticks(App* app, uint32_t ms) {
    return (uint32_t)(((uint64_t)ms * app->tick_freq) / 1000u);
}

static void uid_to_str(const uint8_t* uid, size_t len, char* out, size_t out_len) {
    out[0] = '\0';
    size_t pos = 0;
    for(size_t i = 0; i < len && pos + 3 < out_len; i++) {
        pos += snprintf(out + pos, out_len - pos, (i == 0) ? "%02X" : " %02X", uid[i]);
    }
}

/* ---------------- settings persistence ---------------- */

typedef struct {
    uint8_t magic;
    uint8_t version;
    uint8_t language;
    uint8_t led;
    uint8_t vibro;
    uint8_t beep;
    uint8_t auto_second; // v2; a v1 file ends before it
} SettingsBlob;

static void settings_defaults(App* app) {
    app->led = true;
    app->vibro = true;
    app->beep = false;
    app->auto_second = 0;
    g_lang = LANG_EN;
}

static bool auto_second_valid(uint8_t v) {
    for(size_t i = 0; i < AUTO_SECOND_COUNT; i++)
        if(auto_second_choices[i] == v) return true;
    return false;
}

static void settings_load(App* app) {
    settings_defaults(app);
    File* f = storage_file_alloc(app->storage);
    if(storage_file_open(f, SETTINGS_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        SettingsBlob b;
        memset(&b, 0, sizeof(b));
        size_t got = storage_file_read(f, &b, sizeof(b));
        // A v1 file (before Auto 2nd read) keeps its settings, auto stays off.
        bool v2 = b.version == SETTINGS_VERSION && got == sizeof(b);
        bool v1 = b.version == SETTINGS_VERSION_V1 && got >= offsetof(SettingsBlob, auto_second);
        if(b.magic == SETTINGS_MAGIC && (v2 || v1)) {
            if(b.language < LANG_COUNT) g_lang = b.language;
            app->led = b.led ? true : false;
            app->vibro = b.vibro ? true : false;
            app->beep = b.beep ? true : false;
            if(v2 && auto_second_valid(b.auto_second)) app->auto_second = b.auto_second;
        }
    }
    storage_file_close(f);
    storage_file_free(f);
}

static void settings_save(App* app) {
    SettingsBlob b;
    memset(&b, 0, sizeof(b));
    b.magic = SETTINGS_MAGIC;
    b.version = SETTINGS_VERSION;
    b.language = g_lang;
    b.led = app->led ? 1 : 0;
    b.vibro = app->vibro ? 1 : 0;
    b.beep = app->beep ? 1 : 0;
    b.auto_second = app->auto_second;

    storage_common_mkdir(app->storage, BASE_DIR);
    File* f = storage_file_alloc(app->storage);
    if(storage_file_open(f, SETTINGS_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        storage_file_write(f, &b, sizeof(b));
    }
    storage_file_close(f);
    storage_file_free(f);
}

/* ---------------- feedback (LED / vibro / beep) ---------------- */

static const NotificationSequence seq_led_green = {&message_green_255, &message_do_not_reset, NULL};
static const NotificationSequence seq_led_red = {&message_red_255, &message_do_not_reset, NULL};

// Beeps built from raw sound messages, so they never reset the result LED.
static const NotificationMessage m_beep_hi = {
    .type = NotificationMessageTypeSoundOn,
    .data.sound = {.frequency = 2000.0f, .volume = 1.0f},
};
static const NotificationMessage m_beep_lo = {
    .type = NotificationMessageTypeSoundOn,
    .data.sound = {.frequency = 400.0f, .volume = 1.0f},
};
static const NotificationSequence seq_beep_read = {
    &m_beep_hi,
    &message_delay_50,
    &message_sound_off,
    NULL,
};
static const NotificationSequence seq_beep_match = {
    &m_beep_hi,
    &message_delay_50,
    &message_sound_off,
    &message_delay_50,
    &m_beep_hi,
    &message_delay_50,
    &message_sound_off,
    NULL,
};
static const NotificationSequence seq_beep_diff = {
    &m_beep_lo,
    &message_delay_500,
    &message_sound_off,
    NULL,
};

static void blink_start(App* app) {
    if(!app->led || app->blinking) return;
    notification_message(app->notif, &sequence_blink_start_cyan);
    app->blinking = true;
}

static void blink_stop(App* app) {
    if(!app->blinking) return;
    notification_message(app->notif, &sequence_blink_stop);
    app->blinking = false;
}

static void result_led_off(App* app) {
    if(!app->result_led) return;
    notification_message(app->notif, &sequence_reset_rgb);
    app->result_led = false;
}

static void feedback_first_read(App* app) {
    if(app->vibro) notification_message(app->notif, &sequence_single_vibro);
    if(app->beep) notification_message(app->notif, &seq_beep_read);
}

static void feedback_result(App* app) {
    if(app->vibro)
        notification_message(
            app->notif, app->match ? &sequence_single_vibro : &sequence_double_vibro);
    // (the two beep sequences differ in length, so they are different array types)
    if(app->beep) {
        if(app->match)
            notification_message(app->notif, &seq_beep_match);
        else
            notification_message(app->notif, &seq_beep_diff);
    }
    if(app->led) {
        notification_message(app->notif, app->match ? &seq_led_green : &seq_led_red);
        app->result_led = true;
    }
}

// Keep the display lit while the work screen is open. The firmware counts
// enforce_on calls, so hold/release must pair.
static void backlight_hold(App* app) {
    if(app->backlight_held) return;
    notification_message(app->notif, &sequence_display_backlight_enforce_on);
    app->backlight_held = true;
}

static void backlight_release(App* app) {
    if(!app->backlight_held) return;
    notification_message(app->notif, &sequence_display_backlight_enforce_auto);
    app->backlight_held = false;
}

/* ---------------- reading ---------------- */

// Protocols read with their own poller. Mifare Classic (needs keys for its
// sectors) and EMV (bank cards) are read at their parent level instead.
static bool protocol_supported(NfcProtocol p) {
    switch(p) {
    case NfcProtocolIso14443_3a:
    case NfcProtocolIso14443_3b:
    case NfcProtocolIso14443_4a:
    case NfcProtocolIso14443_4b:
    case NfcProtocolIso15693_3:
    case NfcProtocolFelica:
    case NfcProtocolMfUltralight:
    case NfcProtocolMfDesfire:
    case NfcProtocolMfPlus:
    case NfcProtocolSlix:
    case NfcProtocolSt25tb:
    case NfcProtocolNtag4xx:
    case NfcProtocolType4Tag:
        return true;
    default:
        return false;
    }
}

static NfcProtocol protocol_readable(NfcProtocol p) {
    while(p < NfcProtocolNum && !protocol_supported(p)) p = nfc_protocol_get_parent(p);
    return (p < NfcProtocolNum) ? p : NfcProtocolInvalid;
}

static int protocol_depth(NfcProtocol p) {
    int d = 0;
    while(p < NfcProtocolNum) {
        p = nfc_protocol_get_parent(p);
        d++;
    }
    return d;
}

// Scanner callback (NFC worker thread): pick the most specific readable
// protocol and hand over to the app thread, which starts the poller.
static void scanner_cb(NfcScannerEvent event, void* context) {
    App* app = context;
    if(event.type != NfcScannerEventTypeDetected || app->scan_done) return;

    NfcProtocol best = NfcProtocolInvalid;
    NfcProtocol best_raw = NfcProtocolInvalid;
    int best_depth = -1;
    for(size_t i = 0; i < event.data.protocol_num; i++) {
        NfcProtocol raw = event.data.protocols[i];
        NfcProtocol p = protocol_readable(raw);
        if(p == NfcProtocolInvalid) continue;
        int d = protocol_depth(p);
        if(d > best_depth) {
            best_depth = d;
            best = p;
            best_raw = raw;
        }
    }
    app->detected = best;
    app->detected_raw = best_raw;
    app->scan_done = true;
    view_dispatcher_send_custom_event(app->view_dispatcher, CustomEventDetected);
}

// Interpret one protocol-specific poller event. Requests for a mode get "read";
// requests for keys / passwords are declined (nothing is guessed or tried).
static PollResult poll_event_result(NfcGenericEvent event) {
    switch(event.protocol) {
    case NfcProtocolIso14443_3a: {
        const Iso14443_3aPollerEvent* e = event.event_data;
        if(e->type == Iso14443_3aPollerEventTypeReady) return PollOk;
        if(e->type == Iso14443_3aPollerEventTypeError) return PollFail;
        return PollContinue;
    }
    case NfcProtocolIso14443_3b: {
        const Iso14443_3bPollerEvent* e = event.event_data;
        if(e->type == Iso14443_3bPollerEventTypeReady) return PollOk;
        if(e->type == Iso14443_3bPollerEventTypeError) return PollFail;
        return PollContinue;
    }
    case NfcProtocolIso14443_4a: {
        const Iso14443_4aPollerEvent* e = event.event_data;
        if(e->type == Iso14443_4aPollerEventTypeReady) return PollOk;
        if(e->type == Iso14443_4aPollerEventTypeError) return PollFail;
        return PollContinue;
    }
    case NfcProtocolIso14443_4b: {
        const Iso14443_4bPollerEvent* e = event.event_data;
        if(e->type == Iso14443_4bPollerEventTypeReady) return PollOk;
        if(e->type == Iso14443_4bPollerEventTypeError) return PollFail;
        return PollContinue;
    }
    case NfcProtocolIso15693_3: {
        const Iso15693_3PollerEvent* e = event.event_data;
        if(e->type == Iso15693_3PollerEventTypeReady) return PollOk;
        if(e->type == Iso15693_3PollerEventTypeError) return PollFail;
        return PollContinue;
    }
    case NfcProtocolFelica: {
        FelicaPollerEvent* e = event.event_data;
        if(e->type == FelicaPollerEventTypeRequestAuthContext) {
            e->data->auth_context->skip_auth = true;
            return PollContinue;
        }
        if(e->type == FelicaPollerEventTypeReady || e->type == FelicaPollerEventTypeIncomplete)
            return PollOk;
        if(e->type == FelicaPollerEventTypeError) return PollFail;
        return PollContinue;
    }
    case NfcProtocolMfUltralight: {
        MfUltralightPollerEvent* e = event.event_data;
        switch(e->type) {
        case MfUltralightPollerEventTypeRequestMode:
            e->data->poller_mode = MfUltralightPollerModeRead;
            return PollContinue;
        case MfUltralightPollerEventTypeAuthRequest:
            e->data->auth_context.skip_auth = true;
            return PollContinue;
        case MfUltralightPollerEventTypeRequestKey:
            e->data->key_request_data.key_provided = false;
            return PollContinue;
        case MfUltralightPollerEventTypeReadSuccess:
            return PollOk;
        case MfUltralightPollerEventTypeReadFailed:
            return PollFail;
        default:
            return PollContinue;
        }
    }
    case NfcProtocolMfDesfire: {
        const MfDesfirePollerEvent* e = event.event_data;
        if(e->type == MfDesfirePollerEventTypeReadSuccess) return PollOk;
        if(e->type == MfDesfirePollerEventTypeReadFailed) return PollFail;
        return PollContinue;
    }
    case NfcProtocolMfPlus: {
        const MfPlusPollerEvent* e = event.event_data;
        if(e->type == MfPlusPollerEventTypeReadSuccess) return PollOk;
        if(e->type == MfPlusPollerEventTypeReadFailed) return PollFail;
        return PollContinue;
    }
    case NfcProtocolSlix: {
        SlixPollerEvent* e = event.event_data;
        if(e->type == SlixPollerEventTypePrivacyUnlockRequest) {
            e->data->privacy_password.password_set = false;
            return PollContinue;
        }
        if(e->type == SlixPollerEventTypeReady) return PollOk;
        if(e->type == SlixPollerEventTypeError) return PollFail;
        return PollContinue;
    }
    case NfcProtocolSt25tb: {
        St25tbPollerEvent* e = event.event_data;
        if(e->type == St25tbPollerEventTypeRequestMode) {
            e->data->mode_request.mode = St25tbPollerModeRead;
            return PollContinue;
        }
        if(e->type == St25tbPollerEventTypeSuccess) return PollOk;
        if(e->type == St25tbPollerEventTypeFailure) return PollFail;
        return PollContinue;
    }
    case NfcProtocolNtag4xx: {
        const Ntag4xxPollerEvent* e = event.event_data;
        if(e->type == Ntag4xxPollerEventTypeReadSuccess) return PollOk;
        if(e->type == Ntag4xxPollerEventTypeReadFailed) return PollFail;
        return PollContinue;
    }
    case NfcProtocolType4Tag: {
        Type4TagPollerEvent* e = event.event_data;
        if(e->type == Type4TagPollerEventTypeRequestMode) {
            e->data->poller_mode.mode = Type4TagPollerModeRead;
            return PollContinue;
        }
        if(e->type == Type4TagPollerEventTypeReadSuccess) return PollOk;
        if(e->type == Type4TagPollerEventTypeReadFailed) return PollFail;
        return PollContinue;
    }
    default:
        return PollFail;
    }
}

static ReadInfo* read_target(App* app) {
    return (app->step == 1) ? &app->ref : &app->cur;
}

// Poller callback (NFC worker thread): on success copy the read data into the
// target NfcDevice, then stop and let the app thread clean up.
static NfcCommand poller_cb(NfcGenericEvent event, void* context) {
    App* app = context;
    if(app->poll_done) return NfcCommandStop;
    if(event.protocol != app->poll_protocol) return NfcCommandContinue;

    PollResult r = poll_event_result(event);
    if(r == PollContinue) return NfcCommandContinue;

    app->poll_done = true;
    if(r == PollOk) {
        nfc_device_set_data(
            read_target(app)->dev, app->poll_protocol, nfc_poller_get_data(app->poller));
        view_dispatcher_send_custom_event(app->view_dispatcher, CustomEventReadOk);
    } else {
        view_dispatcher_send_custom_event(app->view_dispatcher, CustomEventReadFail);
    }
    return NfcCommandStop;
}

static void scanner_start(App* app) {
    app->scan_done = false;
    app->scanner = nfc_scanner_alloc(app->nfc);
    nfc_scanner_start(app->scanner, scanner_cb, app);
}

static void scanner_stop(App* app) {
    if(!app->scanner) return;
    nfc_scanner_stop(app->scanner);
    nfc_scanner_free(app->scanner);
    app->scanner = NULL;
}

static void poller_stop(App* app) {
    if(!app->poller) return;
    nfc_poller_stop(app->poller);
    nfc_poller_free(app->poller);
    app->poller = NULL;
}

// Stop every NFC activity and release the hardware.
static void read_abort(App* app) {
    furi_timer_stop(app->timer);
    scanner_stop(app);
    poller_stop(app);
    if(app->nfc) {
        nfc_free(app->nfc);
        app->nfc = NULL;
    }
    blink_stop(app);
}

static void switch_view(App* app, ViewId id) {
    app->current_view = id;
    view_dispatcher_switch_to_view(app->view_dispatcher, id);
}

static void work_redraw(App* app) {
    with_view_model(app->work_view, ViewModel * m, { UNUSED(m); }, true);
}

// Begin a read: step 1 = a new first read, step 2 = a read to compare with it.
static void read_begin(App* app, uint8_t step) {
    read_abort(app);
    result_led_off(app);
    app->step = step;
    app->state = StateReading;
    app->error_until = 0;
    app->auto_at = 0;
    app->anim = 0;
    if(step == 1) {
        app->has_ref = false;
        app->compared = 0;
        app->matched = 0;
    }
    app->nfc = nfc_alloc();
    scanner_start(app);
    blink_start(app);
    furi_timer_start(app->timer, ms_to_ticks(app, TICK_MS));
    backlight_hold(app);
    switch_view(app, ViewIdWork);
    work_redraw(app);
}

static void leave_work(App* app) {
    read_abort(app);
    app->auto_at = 0;
    result_led_off(app);
    backlight_release(app);
    app->menu_sel = 0;
    switch_view(app, ViewIdMenu);
}

/* ---------------- comparison ---------------- */

// Fill the display strings of a finished read.
static void read_info_fill(App* app, ReadInfo* ri) {
    NfcProtocol p = nfc_device_get_protocol(ri->dev);
    ri->uid_level_only = (app->detected_raw != p);
    if(ri->uid_level_only) {
        // e.g. "Mifare Classic*": the card type, read only at ISO14443 level.
        snprintf(ri->name, sizeof(ri->name), "%s*", nfc_device_get_protocol_name(app->detected_raw));
    } else {
        const char* n = nfc_device_get_name(ri->dev, NfcDeviceNameTypeFull);
        strncpy(ri->name, n ? n : "?", sizeof(ri->name) - 1);
        ri->name[sizeof(ri->name) - 1] = '\0';
    }
    size_t uid_len = 0;
    const uint8_t* uid = nfc_device_get_uid(ri->dev, &uid_len);
    if(uid && uid_len)
        uid_to_str(uid, uid_len, ri->uid, sizeof(ri->uid));
    else
        strncpy(ri->uid, "-", sizeof(ri->uid));
}

// Mifare Ultralight / NTAG: say what differs (pages, counter, signature, ...).
static void ultralight_note(App* app, const NfcDevice* a_dev, const NfcDevice* b_dev) {
    const MfUltralightData* a = nfc_device_get_data(a_dev, NfcProtocolMfUltralight);
    const MfUltralightData* b = nfc_device_get_data(b_dev, NfcProtocolMfUltralight);
    if(!a || !b) return;
    uint16_t n = a->pages_read > b->pages_read ? a->pages_read : b->pages_read;
    if(n > MF_ULTRALIGHT_MAX_PAGE_NUM) n = MF_ULTRALIGHT_MAX_PAGE_NUM;
    uint16_t diff = 0;
    for(uint16_t i = 0; i < n; i++) {
        if(i >= a->pages_read || i >= b->pages_read ||
           memcmp(a->page[i].data, b->page[i].data, MF_ULTRALIGHT_PAGE_SIZE) != 0)
            diff++;
    }
    if(diff) {
        snprintf(
            app->data_note,
            sizeof(app->data_note),
            L("%u pages", "%u lap", "%u Seiten"),
            (unsigned)diff);
    } else if(memcmp(a->counter, b->counter, sizeof(a->counter)) != 0) {
        strncpy(app->data_note, L("counter", "szamlalo", "Zaehler"), NOTE_LEN - 1);
    } else if(memcmp(&a->signature, &b->signature, sizeof(a->signature)) != 0) {
        strncpy(app->data_note, L("signature", "alairas", "Signatur"), NOTE_LEN - 1);
    } else if(memcmp(&a->version, &b->version, sizeof(a->version)) != 0) {
        strncpy(app->data_note, L("version", "verzio", "Version"), NOTE_LEN - 1);
    } else if(memcmp(a->tearing_flag, b->tearing_flag, sizeof(a->tearing_flag)) != 0) {
        strncpy(app->data_note, L("tearing", "tearing", "Tearing"), NOTE_LEN - 1);
    }
    app->data_note[NOTE_LEN - 1] = '\0';
}

static void compare_reads(App* app) {
    NfcDevice* a = app->ref.dev;
    NfcDevice* b = app->cur.dev;
    NfcProtocol pa = nfc_device_get_protocol(a);
    NfcProtocol pb = nfc_device_get_protocol(b);

    app->type_ok = (pa == pb) && strcmp(app->ref.name, app->cur.name) == 0;

    size_t la = 0, lb = 0;
    const uint8_t* ua = nfc_device_get_uid(a, &la);
    const uint8_t* ub = nfc_device_get_uid(b, &lb);
    app->uid_ok = ua && ub && la == lb && memcmp(ua, ub, la) == 0;

    // 100% = every field of the read data is equal.
    app->match = app->type_ok && nfc_device_is_equal(a, b);
    app->data_note[0] = '\0';

    if(app->match) {
        app->data_state = DataOk;
    } else if(!app->type_ok) {
        app->data_state = DataNa;
    } else if(app->uid_ok) {
        app->data_state = DataDiff;
        if(pa == NfcProtocolMfUltralight) ultralight_note(app, a, b);
    } else {
        // Different UID: compare the rest by giving a copy of the 2nd read the
        // 1st read's UID, so the Data row tells whether the content is the same.
        NfcDevice* tmp = nfc_device_alloc();
        nfc_device_set_data(tmp, pb, nfc_device_get_data(b, pb));
        if(ua && nfc_device_set_uid(tmp, ua, la)) {
            app->data_state = nfc_device_is_equal(a, tmp) ? DataOk : DataDiff;
            if(app->data_state == DataDiff && pa == NfcProtocolMfUltralight)
                ultralight_note(app, a, tmp);
        } else {
            app->data_state = DataNa;
        }
        nfc_device_free(tmp);
    }

    app->compared++;
    if(app->match) app->matched++;
    app->result_page = 0;
}

/* ---------------- shared drawing ---------------- */

static void draw_centered_trunc_w(Canvas* canvas, int y, const char* text, int max_w) {
    char buf[64];
    strncpy(buf, text, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    while(strlen(buf) > 1 && (int)canvas_string_width(canvas, buf) > max_w) {
        buf[strlen(buf) - 1] = '\0';
    }
    int w = canvas_string_width(canvas, buf);
    int x = (128 - w) / 2;
    if(x < 1) x = 1;
    canvas_draw_str(canvas, x, y, buf);
}

static void draw_centered_trunc(Canvas* canvas, int y, const char* text) {
    draw_centered_trunc_w(canvas, y, text, 126);
}

static void draw_left_trunc(Canvas* canvas, int x, int y, const char* text) {
    char buf[64];
    strncpy(buf, text, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    while(strlen(buf) > 1 && (int)canvas_string_width(canvas, buf) > 126 - x) {
        buf[strlen(buf) - 1] = '\0';
    }
    canvas_draw_str(canvas, x, y, buf);
}

// Inverted banner, like the Remote Analyzer's call-to-action box.
static void draw_banner(Canvas* canvas, int y, const char* text, bool filled) {
    canvas_set_font(canvas, FontPrimary);
    if(filled) {
        canvas_draw_rbox(canvas, 10, y, 108, 16, 3);
        canvas_set_color(canvas, ColorWhite);
    } else {
        canvas_draw_rframe(canvas, 10, y, 108, 16, 3);
        canvas_draw_rframe(canvas, 11, y + 1, 106, 14, 2);
    }
    canvas_draw_str_aligned(canvas, 64, y + 8, AlignCenter, AlignCenter, text);
    canvas_set_color(canvas, ColorBlack);
}

/* ---------------- main screen ---------------- */

static uint8_t menu_items(App* app, MenuItem* out) {
    uint8_t n = 0;
    out[n++] = MenuStart;
    if(app->has_ref) out[n++] = MenuCompare;
    out[n++] = MenuSettings;
    return n;
}

static const char* menu_item_label(MenuItem item) {
    switch(item) {
    case MenuCompare:
        return L("Compare with 1st", "Osszevetes az 1.-vel", "Mit 1. vergleichen");
    case MenuSettings:
        return L("Settings", "Beallitasok", "Einstellungen");
    default:
        return "";
    }
}

static void menu_draw_cb(Canvas* canvas, void* model) {
    App* app = ((ViewModel*)model)->app;
    MenuItem items[MenuItemCount];
    uint8_t count = menu_items(app, items);
    if(app->menu_sel >= count) app->menu_sel = 0;

    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 0, AlignCenter, AlignTop, APP_NAME);

    // Big Start button: filled when selected, double frame otherwise.
    if(app->menu_sel == 0) {
        canvas_draw_rbox(canvas, 3, 12, 122, 23, 3);
        canvas_set_color(canvas, ColorWhite);
    } else {
        canvas_draw_rframe(canvas, 3, 12, 122, 23, 3);
        canvas_draw_rframe(canvas, 4, 13, 120, 21, 2);
    }
    canvas_draw_str_aligned(canvas, 64, 15, AlignCenter, AlignTop, L("Start", "Inditas", "Start"));
    // Second line: what Start does.
    canvas_set_font(canvas, FontSecondary);
    draw_centered_trunc_w(
        canvas,
        32,
        L("Read twice, compare", "2 olvasas, osszevetes", "2x lesen, vergleichen"),
        116);
    canvas_set_color(canvas, ColorBlack);

    for(uint8_t i = 1; i < count; i++) {
        uint8_t y = 37 + (i - 1) * 9;
        if(app->menu_sel == i) {
            canvas_draw_box(canvas, 0, y, 128, 9);
            canvas_set_color(canvas, ColorWhite);
        }
        canvas_draw_str_aligned(canvas, 64, y + 1, AlignCenter, AlignTop, menu_item_label(items[i]));
        canvas_set_color(canvas, ColorBlack);
    }

    // The kept 1st read, so it is clear what "Compare with 1st" compares to.
    if(app->has_ref) {
        char line[64];
        snprintf(line, sizeof(line), L("1st: %s", "1.: %s", "1.: %s"), app->ref.name);
        draw_centered_trunc(canvas, 63, line);
    }
}

static void menu_select(App* app, MenuItem item) {
    switch(item) {
    case MenuStart:
        read_begin(app, 1);
        break;
    case MenuCompare:
        read_begin(app, 2);
        break;
    case MenuSettings:
        switch_view(app, ViewIdSettings);
        break;
    default:
        break;
    }
}

static bool menu_input_cb(InputEvent* event, void* context) {
    App* app = context;
    if(event->type != InputTypeShort && event->type != InputTypeRepeat) return false;
    MenuItem items[MenuItemCount];
    uint8_t count = menu_items(app, items);
    if(app->menu_sel >= count) app->menu_sel = 0;
    if(event->key == InputKeyUp || event->key == InputKeyDown) {
        if(event->key == InputKeyUp)
            app->menu_sel = (app->menu_sel + count - 1) % count;
        else
            app->menu_sel = (app->menu_sel + 1) % count;
        with_view_model(app->menu_view, ViewModel * m, { UNUSED(m); }, true);
        return true;
    }
    if(event->key == InputKeyOk && event->type == InputTypeShort) {
        menu_select(app, items[app->menu_sel]);
        return true;
    }
    return false; // Back reaches navigation_cb, which exits the app
}

/* ---------------- work view (reading / 1st read / result) ---------------- */

static void draw_reading(Canvas* canvas, App* app) {
    // Header (bold, as on the other RSC run screens): which read, and the step.
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(
        canvas,
        0,
        9,
        app->step == 1 ? L("1st read", "1. olvasas", "1. Lesung") :
                         L("2nd read", "2. olvasas", "2. Lesung"));
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(
        canvas, 127, 9, AlignRight, AlignBottom, app->step == 1 ? "1/2" : "2/2");

    // Tile: framed while waiting for a card, filled while it is being read.
    canvas_set_font(canvas, FontPrimary);
    bool reading = app->poller != NULL;
    if(reading) {
        canvas_draw_rbox(canvas, 0, 12, 128, 16, 3);
        canvas_set_color(canvas, ColorWhite);
        draw_centered_trunc(canvas, 24, L("Reading...", "Olvasas...", "Lese..."));
    } else {
        canvas_draw_rframe(canvas, 0, 12, 128, 16, 3);
        char wait[40];
        static const char* const dots[4] = {"", ".", "..", "..."};
        snprintf(
            wait,
            sizeof(wait),
            "%s%s",
            L("Waiting for card", "Kartyara var", "Warte auf Karte"),
            dots[(app->anim / 4) % 4]);
        draw_centered_trunc(canvas, 24, wait);
    }
    canvas_set_color(canvas, ColorBlack);

    canvas_set_font(canvas, FontSecondary);
    bool error_shown = app->error_until && (int32_t)(app->error_until - furi_get_tick()) > 0;
    if(error_shown) {
        draw_centered_trunc(
            canvas,
            38,
            L("!! Read failed - hold still", "!! Olvasasi hiba - tartsd", "!! Lesefehler - ruhig"));
        draw_centered_trunc(
            canvas, 47, L("retrying", "stabilan, ujra probal", "halten, neuer Versuch"));
    } else if(app->step == 2) {
        char line[64];
        snprintf(line, sizeof(line), L("1st: %s", "1.: %s", "1.: %s"), app->ref.name);
        draw_centered_trunc(canvas, 38, line);
        draw_centered_trunc(canvas, 47, app->ref.uid);
    } else {
        draw_centered_trunc(
            canvas, 38, L("Hold the card to the", "Tartsd a kartyat a", "Karte an die Rueckseite"));
        draw_centered_trunc(
            canvas, 47, L("back of the Flipper", "Flipper hatuljahoz", "des Flippers halten"));
    }

    elements_button_center(canvas, L("Cancel", "Megse", "Abbruch"));
}

static void draw_ref(Canvas* canvas, App* app) {
    draw_banner(canvas, 3, L("1st read OK", "1. olvasas OK", "1. Lesung OK"), true);
    canvas_set_font(canvas, FontSecondary);
    draw_centered_trunc(canvas, 30, app->ref.name);
    char uid[64];
    snprintf(uid, sizeof(uid), "UID: %s", app->ref.uid);
    draw_centered_trunc(canvas, 39, uid);
    if(app->auto_at) {
        // Auto 2nd read countdown (the '*' note gives way to it; '*' stays on the name).
        int32_t left = (int32_t)(app->auto_at - furi_get_tick());
        if(left < 0) left = 0;
        uint32_t left_ms = (uint32_t)((uint64_t)left * 1000u / app->tick_freq);
        int secs = (int)((left_ms + 999u) / 1000u);
        char line[40];
        snprintf(
            line,
            sizeof(line),
            L("2nd read in %d s", "2. olvasas: %d mp", "2. Lesung in %d s"),
            secs);
        canvas_set_font(canvas, FontPrimary);
        draw_centered_trunc(canvas, 49, line);
        canvas_set_font(canvas, FontSecondary);
    } else if(app->ref.uid_level_only) {
        draw_centered_trunc(
            canvas, 48, L("* UID/ATQA/SAK only", "* csak UID/ATQA/SAK", "* nur UID/ATQA/SAK"));
    }
    elements_button_left(canvas, L("Menu", "Menu", "Menue"));
    elements_button_right(canvas, L("2nd read", "2. olvasas", "2. Lesung"));
}

static void draw_result_row(Canvas* canvas, int y, const char* label, bool ok, const char* extra) {
    canvas_draw_str(canvas, 6, y, label);
    char val[40];
    if(extra && extra[0])
        snprintf(val, sizeof(val), "%s (%s)", ok ? "OK" : L("DIFF", "ELTER", "ABW."), extra);
    else
        snprintf(val, sizeof(val), "%s", ok ? "OK" : L("DIFF", "ELTER", "ABW."));
    draw_left_trunc(canvas, 44, y, val);
}

static void draw_result(Canvas* canvas, App* app) {
    if(app->result_page == 0) {
        draw_banner(
            canvas,
            2,
            app->match ? L("100% MATCH", "100% EGYEZIK", "100% GLEICH") :
                         L("DIFFERENT", "ELTERO", "ABWEICHEND"),
            app->match);
        canvas_set_font(canvas, FontSecondary);
        draw_result_row(canvas, 28, L("Type", "Tipus", "Typ"), app->type_ok, NULL);
        draw_result_row(canvas, 37, "UID", app->uid_ok, NULL);
        if(app->data_state == DataNa) {
            canvas_draw_str(canvas, 6, 46, L("Data", "Adat", "Daten"));
            canvas_draw_str(canvas, 44, 46, L("n/a", "nem vetheto", "k.A."));
        } else {
            draw_result_row(
                canvas, 46, L("Data", "Adat", "Daten"), app->data_state == DataOk, app->data_note);
        }
    } else {
        // Both reads side by side, and the tally against this 1st read.
        canvas_set_font(canvas, FontSecondary);
        char line[64];
        snprintf(line, sizeof(line), L("1st: %s", "1.: %s", "1.: %s"), app->ref.name);
        draw_left_trunc(canvas, 0, 8, line);
        draw_left_trunc(canvas, 8, 17, app->ref.uid);
        snprintf(line, sizeof(line), L("2nd: %s", "2.: %s", "2.: %s"), app->cur.name);
        draw_left_trunc(canvas, 0, 27, line);
        draw_left_trunc(canvas, 8, 36, app->cur.uid);
        snprintf(
            line,
            sizeof(line),
            L("Matched %lu of %lu", "Egyezett: %lu / %lu", "Gleich: %lu von %lu"),
            (unsigned long)app->matched,
            (unsigned long)app->compared);
        draw_left_trunc(canvas, 0, 46, line);
    }
    elements_scrollbar_pos(canvas, 128, 0, 50, app->result_page, 2);
    elements_button_left(canvas, L("Menu", "Menu", "Menue"));
    elements_button_center(canvas, L("New", "Uj", "Neu"));
    elements_button_right(canvas, L("Next", "Tovabb", "Weiter"));
}

static void work_draw_cb(Canvas* canvas, void* model) {
    App* app = ((ViewModel*)model)->app;
    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);
    switch(app->state) {
    case StateReading:
        draw_reading(canvas, app);
        break;
    case StateRef:
        draw_ref(canvas, app);
        break;
    case StateResult:
        draw_result(canvas, app);
        break;
    }
}

static bool work_input_cb(InputEvent* event, void* context) {
    App* app = context;
    if(event->type != InputTypeShort) {
        // Swallow repeats so held keys don't leak to the dispatcher.
        return event->type == InputTypeRepeat;
    }
    switch(app->state) {
    case StateReading:
        if(event->key == InputKeyOk || event->key == InputKeyBack) {
            leave_work(app);
            return true;
        }
        break;
    case StateRef:
        if(event->key == InputKeyRight || event->key == InputKeyOk) {
            read_begin(app, 2);
            return true;
        }
        if(event->key == InputKeyLeft || event->key == InputKeyBack) {
            leave_work(app);
            return true;
        }
        break;
    case StateResult:
        if(event->key == InputKeyRight) {
            read_begin(app, 2); // another card against the same 1st read
            return true;
        }
        if(event->key == InputKeyOk) {
            read_begin(app, 1); // start over with a new 1st read
            return true;
        }
        if(event->key == InputKeyUp || event->key == InputKeyDown) {
            app->result_page ^= 1;
            work_redraw(app);
            return true;
        }
        if(event->key == InputKeyLeft || event->key == InputKeyBack) {
            leave_work(app);
            return true;
        }
        break;
    }
    return true;
}

/* ---------------- events ---------------- */

static void on_detected(App* app) {
    if(!app->scanner || app->state != StateReading) return; // cancelled meanwhile
    scanner_stop(app);
    NfcProtocol p = app->detected;
    if(p == NfcProtocolInvalid) {
        // Nothing we can read: show the error and keep looking.
        app->error_until = furi_get_tick() + ms_to_ticks(app, READ_ERROR_MS);
        scanner_start(app);
        return;
    }
    app->poll_protocol = p;
    app->poll_done = false;
    app->poller = nfc_poller_alloc(app->nfc, p);
    nfc_poller_start(app->poller, poller_cb, app);
}

static void on_read_ok(App* app) {
    if(!app->poller || app->state != StateReading) return;
    poller_stop(app);
    read_abort(app); // releases the NFC hardware and stops the blink
    ReadInfo* ri = read_target(app);
    read_info_fill(app, ri);
    if(app->step == 1) {
        app->has_ref = true;
        app->state = StateRef;
        feedback_first_read(app);
        if(app->auto_second > 0) {
            // Auto 2nd read: count down on the "1st read OK" screen, then start.
            app->auto_at =
                furi_get_tick() + ms_to_ticks(app, (uint32_t)app->auto_second * 1000u);
            furi_timer_start(app->timer, ms_to_ticks(app, TICK_MS));
        }
    } else {
        compare_reads(app);
        app->state = StateResult;
        feedback_result(app);
    }
}

static void on_read_fail(App* app) {
    if(!app->poller || app->state != StateReading) return;
    poller_stop(app);
    app->error_until = furi_get_tick() + ms_to_ticks(app, READ_ERROR_MS);
    scanner_start(app); // try again with whatever card is presented
}

/* ---------------- settings ---------------- */

static void settings_populate(App* app); // rebuilt when the language changes

static void language_changed_cb(VariableItem* item) {
    App* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    if(idx >= LANG_COUNT) idx = LANG_EN;
    g_lang = idx;
    variable_item_set_current_value_text(item, language_names[idx]);
    // Every label changes: rebuild the list (deferred, as the list can't be
    // rebuilt from inside its own change callback).
    view_dispatcher_send_custom_event(app->view_dispatcher, CustomEventLanguage);
}

static void led_changed_cb(VariableItem* item) {
    App* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item) ? 1 : 0;
    app->led = idx ? true : false;
    variable_item_set_current_value_text(item, on_off_label[g_lang][idx]);
}

static void vibro_changed_cb(VariableItem* item) {
    App* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item) ? 1 : 0;
    app->vibro = idx ? true : false;
    variable_item_set_current_value_text(item, on_off_label[g_lang][idx]);
}

static void beep_changed_cb(VariableItem* item) {
    App* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item) ? 1 : 0;
    app->beep = idx ? true : false;
    variable_item_set_current_value_text(item, on_off_label[g_lang][idx]);
}

static void auto_second_text(VariableItem* item, uint8_t secs) {
    if(secs == 0) {
        variable_item_set_current_value_text(item, on_off_label[g_lang][0]);
    } else {
        char text[8];
        snprintf(text, sizeof(text), "%d s", secs);
        variable_item_set_current_value_text(item, text);
    }
}

static void auto_second_changed_cb(VariableItem* item) {
    App* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    if(idx >= AUTO_SECOND_COUNT) idx = 0;
    app->auto_second = auto_second_choices[idx];
    auto_second_text(item, app->auto_second);
}

static void add_on_off(App* app, const char* label, bool value, VariableItemChangeCallback cb) {
    VariableItem* it = variable_item_list_add(app->settings_list, label, 2, cb, app);
    variable_item_set_current_value_index(it, value ? 1 : 0);
    variable_item_set_current_value_text(it, on_off_label[g_lang][value ? 1 : 0]);
}

static void settings_populate(App* app) {
    VariableItemList* l = app->settings_list;
    variable_item_list_reset(l);
    variable_item_list_set_header(l, L("Settings", "Beallitasok", "Einstellungen"));

    // First row, and "Language" in every language (as in the other RSC apps),
    // so it can be found after picking a language you don't read.
    VariableItem* it_lang =
        variable_item_list_add(l, "Language", LANG_COUNT, language_changed_cb, app);
    variable_item_set_current_value_index(it_lang, g_lang);
    variable_item_set_current_value_text(it_lang, language_names[g_lang]);

    // Auto 2nd read: Off, or the seconds to wait on "1st read OK" before it starts.
    uint8_t ai = 0;
    for(uint8_t i = 0; i < AUTO_SECOND_COUNT; i++)
        if(auto_second_choices[i] == app->auto_second) ai = i;
    VariableItem* it_auto = variable_item_list_add(
        l,
        L("Auto 2nd read", "Auto 2. olv.", "Auto 2. Lesung"),
        AUTO_SECOND_COUNT,
        auto_second_changed_cb,
        app);
    variable_item_set_current_value_index(it_auto, ai);
    auto_second_text(it_auto, app->auto_second);

    add_on_off(app, "LED", app->led, led_changed_cb);
    add_on_off(app, L("Vibro", "Rezges", "Vibration"), app->vibro, vibro_changed_cb);
    add_on_off(app, L("Beep", "Hang", "Ton"), app->beep, beep_changed_cb);

    // Last row: the version (as in the other RSC apps).
    VariableItem* it_ver =
        variable_item_list_add(l, L("Version", "Verzio", "Version"), 1, NULL, app);
    variable_item_set_current_value_text(it_ver, APP_VERSION);
}

/* ---------------- dispatcher callbacks ---------------- */

static bool navigation_cb(void* context) {
    App* app = context;
    switch(app->current_view) {
    case ViewIdWork:
        leave_work(app);
        return true;
    case ViewIdSettings:
        settings_save(app);
        switch_view(app, ViewIdMenu);
        return true;
    case ViewIdMenu:
    default:
        return false;
    }
}

static bool custom_event_cb(void* context, uint32_t event) {
    App* app = context;
    switch(event) {
    case CustomEventTick:
        app->anim++;
        if(app->state == StateRef && app->auto_at &&
           (int32_t)(furi_get_tick() - app->auto_at) >= 0) {
            app->auto_at = 0;
            read_begin(app, 2); // Auto 2nd read
            return true;
        }
        work_redraw(app);
        return true;
    case CustomEventDetected:
        on_detected(app);
        work_redraw(app);
        return true;
    case CustomEventReadOk:
        on_read_ok(app);
        work_redraw(app);
        return true;
    case CustomEventReadFail:
        on_read_fail(app);
        work_redraw(app);
        return true;
    case CustomEventLanguage:
        settings_populate(app);
        variable_item_list_set_selected_item(app->settings_list, 0);
        return true;
    default:
        return false;
    }
}

static void timer_cb(void* ctx) {
    App* app = ctx;
    view_dispatcher_send_custom_event(app->view_dispatcher, CustomEventTick);
}

/* ---------------- builders ---------------- */

static View* make_custom_view(App* app, ViewDrawCallback draw, ViewInputCallback input) {
    View* v = view_alloc();
    view_set_context(v, app);
    view_allocate_model(v, ViewModelTypeLocking, sizeof(ViewModel));
    with_view_model(v, ViewModel * m, { m->app = app; }, false);
    view_set_draw_callback(v, draw);
    view_set_input_callback(v, input);
    return v;
}

static App* app_alloc(void) {
    App* app = malloc(sizeof(App));
    memset(app, 0, sizeof(App));

    app->storage = furi_record_open(RECORD_STORAGE);
    app->notif = furi_record_open(RECORD_NOTIFICATION);
    app->timer = furi_timer_alloc(timer_cb, FuriTimerTypePeriodic, app);
    app->tick_freq = furi_kernel_get_tick_frequency();
    app->ref.dev = nfc_device_alloc();
    app->cur.dev = nfc_device_alloc();
    app->detected = NfcProtocolInvalid;
    app->detected_raw = NfcProtocolInvalid;
    app->step = 1;

    settings_load(app);

    app->gui = furi_record_open(RECORD_GUI);
    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->view_dispatcher, custom_event_cb);
    view_dispatcher_set_navigation_event_callback(app->view_dispatcher, navigation_cb);

    app->menu_view = make_custom_view(app, menu_draw_cb, menu_input_cb);
    view_dispatcher_add_view(app->view_dispatcher, ViewIdMenu, app->menu_view);

    app->settings_list = variable_item_list_alloc();
    settings_populate(app);
    view_dispatcher_add_view(
        app->view_dispatcher, ViewIdSettings, variable_item_list_get_view(app->settings_list));

    app->work_view = make_custom_view(app, work_draw_cb, work_input_cb);
    view_dispatcher_add_view(app->view_dispatcher, ViewIdWork, app->work_view);

    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    app->current_view = ViewIdMenu;
    return app;
}

static void app_free(App* app) {
    read_abort(app);
    result_led_off(app);
    backlight_release(app);

    view_dispatcher_remove_view(app->view_dispatcher, ViewIdMenu);
    view_dispatcher_remove_view(app->view_dispatcher, ViewIdSettings);
    view_dispatcher_remove_view(app->view_dispatcher, ViewIdWork);

    view_free(app->menu_view);
    variable_item_list_free(app->settings_list);
    view_free(app->work_view);

    view_dispatcher_free(app->view_dispatcher);
    furi_record_close(RECORD_GUI);

    furi_timer_free(app->timer);
    nfc_device_free(app->ref.dev);
    nfc_device_free(app->cur.dev);
    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_STORAGE);

    free(app);
}

int32_t rsc_nfc_comparison_app(void* p) {
    UNUSED(p);
    App* app = app_alloc();
    view_dispatcher_switch_to_view(app->view_dispatcher, ViewIdMenu);
    view_dispatcher_run(app->view_dispatcher);
    app_free(app);
    return 0;
}
