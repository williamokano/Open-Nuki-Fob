// ESP32 (M5Stack Atom Lite) test harness for the Open-Nuki-Fob protocol code.
// Button handling and the pairing / lock-action flow mirror src/main.c; the protocol itself
// (src/nuki*.c) runs unmodified.
//
//   1 / 2 / 3 presses -> Fob Action 1 / 2 / 3
//   hold >= 5 s       -> pair with a Nuki in pairing mode
//
// Speed-ups over the nRF flow: the BLE connection starts on the first press (while the number of
// presses is still being counted), and the IO characteristic handles are cached like the nRF
// handle cache, with one fallback to full discovery if the cached handles don't work.
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "ble_central.h"
#include "board.h"
#include "bt_comm_esp32.h"
#include "fob_store.h"
#include "nuki.h"

#define LOOP_PERIOD_MS 10
#define DEBOUNCE_SAMPLES 5          // 50 ms, like BUTTON_DETECTION_DELAY
#define MULTI_PRESS_GAP_MS 300      // decide once no new press follows the last release within this gap
#define PAIRING_HOLD_MS 5000
#define INPUT_ABORT_MS 10000        // the nRF fob enters DFU here; we just ignore the input
#define OPERATION_TIMEOUT_MS 30000  // SHUTDOWN_TICKS in src/main.c
#define RESULT_LED_MS 1500
#define CACHED_REPLY_TIMEOUT_MS 3000 // no reply with cached handles -> rediscover once

static const char* TAG = "fob";

enum fob_action { ACTION_NONE = 0, ACTION_FOB_1 = 1, ACTION_FOB_2 = 2, ACTION_FOB_3 = 3, ACTION_PAIRING = 4 };
enum app_state {
    STATE_IDLE,
    STATE_PRECONNECT,    // button pressed, presses still being counted, BLE already connecting
    STATE_CONNECTING,    // action decided, waiting for the BLE link to be ready
    STATE_COMMUNICATING, // Nuki protocol running
};

static QueueHandle_t ble_events;
static enum app_state state = STATE_IDLE;
static enum fob_action current_action = ACTION_NONE;
static int64_t operation_started_ms;
static int64_t protocol_started_ms;
static volatile bool operation_done;
static bool ble_ready;
static bool using_cached_handles;
static bool cache_fallback_done;

static nuki_key stored_key;
static bool has_key;
static nuki_pairing_context pairing_ctx;
static nuki_lock_action_context lock_action_ctx;
static void* active_ctx;

static int64_t led_result_until_ms;

static int64_t now_ms(void) {
    return esp_timer_get_time() / 1000;
}

// ---------------------------------------------------------------------------------------------
// LED

static void show_result(bool success) {
    if(success) { board_led(0, 40, 0); } else { board_led(40, 0, 0); }
    led_result_until_ms = now_ms() + RESULT_LED_MS;
}

static void update_led(bool pairing_hold_hint) {
    int64_t now = now_ms();
    if(now < led_result_until_ms) { return; }

    if(state == STATE_CONNECTING || state == STATE_COMMUNICATING) {
        if(current_action == ACTION_PAIRING) {
            board_led(30, 0, 30);                                   // solid purple: pairing
        } else {
            bool on = ((now - operation_started_ms) / 100) % 12 < 2; // BLINK_PATTERN_UNLOCK, starts on
            if(on) { board_led(0, 0, 40); } else { board_led(0, 0, 0); }
        }
    } else if(pairing_hold_hint) {
        board_led(30, 0, 30);
    } else {
        board_led(0, 0, 0);
    }
}

// ---------------------------------------------------------------------------------------------
// Timing report: when each phase finished, in ms relative to the moment the action was decided
// (negative = already done during the press counting thanks to the early connection)

static void format_offset(char* out, size_t size, int64_t timestamp_ms) {
    if(timestamp_ms == 0) { snprintf(out, size, "n/a"); return; }
    snprintf(out, size, "%+lld", timestamp_ms - operation_started_ms);
}

static void report_timing(bool success) {
    ble_central_timing t = ble_central_get_timing();
    char found[16], connected[16], ready[16], replied[16];
    format_offset(found, sizeof(found), t.lock_found_ms);
    format_offset(connected, sizeof(connected), t.connected_ms);
    format_offset(ready, sizeof(ready), t.ready_ms);
    format_offset(replied, sizeof(replied), success ? now_ms() : 0);
    const char* handles = using_cached_handles ? "cached" : (cache_fallback_done ? "rediscovered" : "discovered");

    char line[200];
    snprintf(line, sizeof(line),
        "%s action %d: lock found %s, connected %s (interval %u.%02u ms), link ready %s, Nuki replied %s ms; handles %s",
        success ? "OK" : "FAILED", current_action, found, connected,
        t.conn_interval_x1250us * 5 / 4, (t.conn_interval_x1250us * 125) % 100, ready, replied, handles);
    ESP_LOGI(TAG, "Timing: %s", line);
    fob_store_save_note(line);
}

// ---------------------------------------------------------------------------------------------
// BLE session for the paired lock (used for the early connection and for fob actions)

static bool start_keyturner_session(void) {
    xQueueReset(ble_events);
    bt_comm_reset();
    ble_ready = false;

    ble_central_handles cached;
    using_cached_handles = fob_store_load_handles(stored_key.lock_uuid, &cached.io_handle, &cached.cccd_handle);
    return ble_central_start(BLE_CENTRAL_KEYTURNER, stored_key.lock_uuid, using_cached_handles ? &cached : NULL);
}

static void start_preconnect(void) {
    if(!has_key || !ble_central_synced()) { return; }
    if(start_keyturner_session()) {
        state = STATE_PRECONNECT;
    }
}

static void cancel_preconnect(void) {
    ble_central_stop();
    bt_comm_reset();
    state = STATE_IDLE;
}

// ---------------------------------------------------------------------------------------------
// Operation lifecycle

static void finish_operation(bool success, const char* reason) {
    if(state == STATE_IDLE) { return; }
    report_timing(success);
    if(success) {
        ESP_LOGI(TAG, "SUCCESS: %s", current_action == ACTION_PAIRING ? "paired" : "lock action sent");
    } else {
        ESP_LOGE(TAG, "FAILED: %s", reason ? reason : "unknown");
    }
    state = STATE_IDLE;
    ble_central_stop();
    bt_comm_reset();
    show_result(success);
}

// Like the nRF fob (update_handles): if the cached handles fail, rediscover once and retry
static void attempt_failed(const char* reason) {
    if(current_action != ACTION_PAIRING && using_cached_handles && !cache_fallback_done) {
        ESP_LOGW(TAG, "Attempt with cached handles failed (%s), retrying with service discovery", reason);
        fob_store_clear_handles();
        cache_fallback_done = true;
        ble_central_stop();
        state = STATE_CONNECTING;
        if(!start_keyturner_session()) { finish_operation(false, "Could not restart scanning"); }
        return;
    }
    finish_operation(false, reason);
}

static void protocol_done(void) {
    operation_done = true;
}

// Same context setup as AS_INDICATIONS_ENABLED / connect_to_nearest_lock() in src/main.c
static void begin_protocol(void) {
    if(current_action == ACTION_PAIRING) {
        memset(&pairing_ctx, 0, sizeof(pairing_ctx));
        ble_central_peer_address(pairing_ctx.key.lock_uuid);
        fob_store_identity(pairing_ctx.fob_name, &pairing_ctx.app_id);
        pairing_ctx.pairing_done_callback = protocol_done;
        active_ctx = &pairing_ctx;
        start_pairing(&pairing_ctx);
    } else {
        memset(&lock_action_ctx, 0, sizeof(lock_action_ctx));
        lock_action_ctx.lock_action_done_callback = protocol_done;
        lock_action_ctx.lock_action = current_action + 0x80; // Fob Action 1/2/3 = 0x81/0x82/0x83
        lock_action_ctx.key = stored_key;
        // app_id intentionally left 0: the nRF firmware never sets it either
        active_ctx = &lock_action_ctx;
        perform_lock_action(&lock_action_ctx);
    }
    protocol_started_ms = now_ms();
    state = STATE_COMMUNICATING;
}

static void start_operation(enum fob_action action) {
    ESP_LOGI(TAG, "Action to execute: %d", action);
    if(action != ACTION_PAIRING && !has_key) {
        ESP_LOGE(TAG, "Not paired yet: hold the button for 5 s while the Nuki is in pairing mode");
        show_result(false);
        return;
    }
    if(!ble_central_synced()) {
        ESP_LOGE(TAG, "BLE not ready");
        show_result(false);
        return;
    }

    bool preconnected = state == STATE_PRECONNECT;
    if(action == ACTION_PAIRING && preconnected) {
        cancel_preconnect();
        preconnected = false;
    }

    operation_done = false;
    cache_fallback_done = false;
    current_action = action;
    operation_started_ms = now_ms();
    state = STATE_CONNECTING;

    if(action == ACTION_PAIRING) {
        xQueueReset(ble_events);
        bt_comm_reset();
        ble_ready = false;
        using_cached_handles = false;
        if(!ble_central_start(BLE_CENTRAL_PAIRING, NULL, NULL)) { finish_operation(false, "Could not start scanning"); }
        return;
    }

    if(!preconnected && !start_keyturner_session()) {
        finish_operation(false, "Could not start scanning");
        return;
    }
    if(ble_ready) { begin_protocol(); }
}

static void on_protocol_complete(void) {
    if(current_action == ACTION_PAIRING) {
        nuki_key key = pairing_ctx.key;
        if(fob_store_save_key(&key) != ESP_OK) {
            finish_operation(false, "Pairing succeeded but saving it failed");
            return;
        }
        fob_store_clear_handles();
        stored_key = key;
        has_key = true;
        ESP_LOGI(TAG, "Saved pairing, authorization id %lu", (unsigned long)key.authorization_id);
    } else if(!using_cached_handles) {
        ble_central_handles handles = ble_central_get_handles();
        fob_store_save_handles(stored_key.lock_uuid, handles.io_handle, handles.cccd_handle);
        ESP_LOGI(TAG, "Cached handles IO 0x%04X, CCCD 0x%04X for next time", handles.io_handle, handles.cccd_handle);
    }
    finish_operation(true, NULL);
}

static void handle_operation(void) {
    ble_central_evt evt;
    while(xQueueReceive(ble_events, &evt, 0) == pdTRUE) {
        switch(state) {
            case STATE_IDLE:
                break;
            case STATE_PRECONNECT:
                if(evt.type == BLE_CENTRAL_EVT_READY) {
                    ble_ready = true;
                } else {
                    // let the action decision start a fresh attempt
                    ESP_LOGW(TAG, "Early connection failed: %s", evt.reason ? evt.reason : "unknown");
                    cancel_preconnect();
                }
                break;
            case STATE_CONNECTING:
            case STATE_COMMUNICATING:
                if(evt.type == BLE_CENTRAL_EVT_READY) {
                    ble_ready = true;
                    if(state == STATE_CONNECTING) { begin_protocol(); }
                } else if(!operation_done) {
                    attempt_failed(evt.reason);
                }
                break;
        }
    }

    if(state == STATE_COMMUNICATING) {
        if(!bt_comm_process(active_ctx, ble_central_conn_handle(), ble_central_io_handle())) {
            attempt_failed("Transport or protocol error");
        } else if(operation_done) {
            on_protocol_complete();
        } else if(using_cached_handles && now_ms() - protocol_started_ms > CACHED_REPLY_TIMEOUT_MS) {
            attempt_failed("No reply from the Nuki");
        }
    }

    if((state == STATE_CONNECTING || state == STATE_COMMUNICATING) &&
        now_ms() - operation_started_ms > OPERATION_TIMEOUT_MS) {
        finish_operation(false, "Timeout");
    }
}

// ---------------------------------------------------------------------------------------------
// Button input. Like input_timer_handler() in src/main.c, but instead of a fixed 2 s window the
// action is decided MULTI_PRESS_GAP_MS after the last release, so fast presses trigger sooner.

typedef struct {
    bool stable_pressed;
    int same_samples;
    bool active;
    int64_t started_ms;
    int64_t first_release_ms;
    int64_t last_release_ms;
    int releases;
} input_state;

static enum fob_action poll_input(input_state* in, bool* pairing_hold_hint, bool* press_started) {
    bool raw = board_button_pressed();
    int64_t now = now_ms();
    *pairing_hold_hint = false;
    *press_started = false;

    // debounce
    if(raw != in->stable_pressed) {
        if(++in->same_samples >= DEBOUNCE_SAMPLES) {
            in->stable_pressed = raw;
            in->same_samples = 0;
            if(raw && !in->active) {
                in->active = true;
                in->started_ms = now;
                in->releases = 0;
                *press_started = true;
            } else if(!raw && in->active) {
                if(in->releases++ == 0) { in->first_release_ms = now; }
                in->last_release_ms = now;
            }
        }
    } else {
        in->same_samples = 0;
    }

    if(!in->active) { return ACTION_NONE; }
    int64_t elapsed = now - in->started_ms;

    if(in->releases == 0) {
        *pairing_hold_hint = elapsed >= PAIRING_HOLD_MS;
        if(elapsed >= INPUT_ABORT_MS) {
            ESP_LOGW(TAG, "Button held too long, ignoring");
            in->active = false;
        }
        return ACTION_NONE;
    }

    if(in->first_release_ms - in->started_ms >= PAIRING_HOLD_MS) {
        in->active = false;
        return ACTION_PAIRING;
    }

    if(!in->stable_pressed && now - in->last_release_ms >= MULTI_PRESS_GAP_MS) {
        in->active = false;
        switch(in->releases) {
            case 1: return ACTION_FOB_1;
            case 2: return ACTION_FOB_2;
            case 3: return ACTION_FOB_3;
            default:
                ESP_LOGW(TAG, "Invalid input (%d presses), ignoring", in->releases);
                return ACTION_NONE;
        }
    }
    return ACTION_NONE;
}

// ---------------------------------------------------------------------------------------------

void app_main(void) {
    board_init();
    // Boot cue: short white flash, so a working LED is visible right after flashing
    board_led(20, 20, 20);
    vTaskDelay(pdMS_TO_TICKS(300));
    board_led(0, 0, 0);
    fob_store_init();

    char fob_name[32];
    uint32_t app_id;
    fob_store_identity(fob_name, &app_id);
    has_key = fob_store_load_key(&stored_key);
    ESP_LOGI(TAG, "%s (app id %08lX)", fob_name, (unsigned long)app_id);
    if(has_key) {
        const uint8_t* a = stored_key.lock_uuid;
        ESP_LOGI(TAG, "Paired with %02X:%02X:%02X:%02X:%02X:%02X, authorization id %lu",
            a[5], a[4], a[3], a[2], a[1], a[0], (unsigned long)stored_key.authorization_id);
    } else {
        ESP_LOGI(TAG, "Not paired");
    }
    char last_timing[200];
    if(fob_store_load_note(last_timing, sizeof(last_timing))) {
        ESP_LOGI(TAG, "Last operation timing: %s", last_timing);
    }
    ESP_LOGI(TAG, "1/2/3 presses = Fob Action 1/2/3, hold 5 s = pair");

    ble_events = xQueueCreate(8, sizeof(ble_central_evt));
    ble_central_init(ble_events);

    input_state input = { 0 };
    while(true) {
        bool pairing_hold_hint = false;
        if(state == STATE_IDLE || state == STATE_PRECONNECT) {
            bool press_started = false;
            enum fob_action action = poll_input(&input, &pairing_hold_hint, &press_started);
            if(press_started && state == STATE_IDLE) { start_preconnect(); }
            if(state == STATE_PRECONNECT && (pairing_hold_hint || (!input.active && action == ACTION_NONE))) {
                cancel_preconnect(); // heading for pairing, or the input was invalid / aborted
            }
            if(action != ACTION_NONE) { start_operation(action); }
        }
        handle_operation();
        update_led(pairing_hold_hint);
        vTaskDelay(pdMS_TO_TICKS(LOOP_PERIOD_MS));
    }
}
