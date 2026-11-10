#include "ble_central.h"
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "bt_comm_esp32.h"
#include "nuki.h"

#define SCAN_DURATION_MS 30000
#define CONNECT_TIMEOUT_MS 10000
#define CCCD_UUID 0x2902
#define MAX_LOGGED_ADVERTISERS 8

static const char* TAG = "ble";

static const uint8_t keyturner_service_uuid[16] = { NUKI_KEYTURNER_SERVICE_BASE_UUID };
static const uint8_t pairing_service_uuid[16] = { NUKI_PAIRING_SERVICE_BASE_UUID };

// The nRF firmware allows 20-200 ms; every GATT round trip costs at least one interval,
// so ask for a short one to keep the whole exchange fast.
static const struct ble_gap_conn_params conn_params = {
    .scan_itvl = 0x0018,
    .scan_window = 0x0018,
    .itvl_min = 6,              // 7.5 ms
    .itvl_max = 12,             // 15 ms
    .latency = 0,
    .supervision_timeout = 400, // 4 s
};

static QueueHandle_t events;
static volatile bool synced;
static uint8_t own_addr_type;

static ble_central_mode mode;
static uint8_t target_address[6];
static ble_addr_t peer;
static volatile bool active;
static volatile bool connecting;
static volatile uint16_t conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t service_start, service_end;
static volatile uint16_t io_handle;
static uint16_t cccd_handle;
static bool use_cached_handles;

// Incremented by every ble_central_start(); passed as GAP callback argument so events of an
// earlier, stopped session (e.g. its late disconnect) are not reported to the new one.
static uintptr_t session;
#define SESSION_ARG ((void*)session)
#define IS_CURRENT_SESSION(arg) ((uintptr_t)(arg) == session)

static ble_central_timing timing;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static uint8_t logged_advertisers[MAX_LOGGED_ADVERTISERS][6];
static int n_logged_advertisers;

static int gap_event(struct ble_gap_event* event, void* arg);

static void post(ble_central_evt_type type, const char* reason) {
    if(!active) { return; }
    ble_central_evt evt = { .type = type, .reason = reason };
    xQueueSend(events, &evt, 0);
}

static void fail(const char* reason) {
    ESP_LOGE(TAG, "%s", reason);
    post(BLE_CENTRAL_EVT_FAILED, reason);
}

// Verbatim logic of advertises_pairing() / advertises_keyturner() in src/main.c
static bool advertises_pairing(const uint8_t* data, uint8_t length) {
    if(length < 21) { return false; }
    for(int i = 0; i < 16; i++) {
        if(data[5 + i] != pairing_service_uuid[i]) { return false; }
    }
    return true;
}

static bool advertises_keyturner(const uint8_t* data, uint8_t length) {
    if(length < 25) { return false; }
    for(int i = 0; i < 16; i++) {
        if(data[24 - i] != keyturner_service_uuid[i]) { return false; }
    }
    return true;
}

static void log_advertiser_once(const ble_addr_t* addr, int8_t rssi, const char* kind) {
    for(int i = 0; i < n_logged_advertisers; i++) {
        if(memcmp(logged_advertisers[i], addr->val, 6) == 0) { return; }
    }
    if(n_logged_advertisers < MAX_LOGGED_ADVERTISERS) {
        memcpy(logged_advertisers[n_logged_advertisers++], addr->val, 6);
    }
    const uint8_t* a = addr->val;
    ESP_LOGI(TAG, "Found Nuki (%s) %02X:%02X:%02X:%02X:%02X:%02X rssi %d",
        kind, a[5], a[4], a[3], a[2], a[1], a[0], rssi);
}

static ble_uuid128_t nuki_uuid(const uint8_t base[16], uint16_t short_uuid) {
    ble_uuid128_t uuid = { .u = { .type = BLE_UUID_TYPE_128 } };
    memcpy(uuid.value, base, 16);
    uuid.value[12] = short_uuid & 0xFF;
    uuid.value[13] = short_uuid >> 8;
    return uuid;
}

static int on_cccd_written(uint16_t conn, const struct ble_gatt_error* error, struct ble_gatt_attr* attr, void* arg) {
    if(!IS_CURRENT_SESSION(arg)) { return 0; }
    if(error->status != 0) {
        fail("Enabling indications failed");
        return 0;
    }
    ESP_LOGI(TAG, "Indications enabled");
    timing.ready_ms = now_ms();
    post(BLE_CENTRAL_EVT_READY, NULL);
    return 0;
}

static void enable_indications(uint16_t conn) {
    static const uint8_t value[2] = { 0x02, 0x00 }; // BLE_GATT_HVX_INDICATION
    if(ble_gattc_write_flat(conn, cccd_handle, value, sizeof(value), on_cccd_written, SESSION_ARG) != 0) {
        fail("Writing CCCD failed");
    }
}

static int on_descriptor(uint16_t conn, const struct ble_gatt_error* error, uint16_t chr_val_handle,
    const struct ble_gatt_dsc* dsc, void* arg) {
    if(!IS_CURRENT_SESSION(arg)) { return 0; }
    if(error->status == 0) {
        if(cccd_handle == 0 && dsc->uuid.u.type == BLE_UUID_TYPE_16 && ble_uuid_u16(&dsc->uuid.u) == CCCD_UUID) {
            cccd_handle = dsc->handle;
        }
        return 0;
    }
    if(error->status != BLE_HS_EDONE) { fail("Descriptor discovery failed"); return 0; }
    if(cccd_handle == 0) { fail("CCCD not found"); return 0; }

    ESP_LOGI(TAG, "IO handle 0x%04X, CCCD handle 0x%04X", io_handle, cccd_handle);
    enable_indications(conn);
    return 0;
}

static int on_characteristic(uint16_t conn, const struct ble_gatt_error* error, const struct ble_gatt_chr* chr, void* arg) {
    if(!IS_CURRENT_SESSION(arg)) { return 0; }
    if(error->status == 0) {
        io_handle = chr->val_handle;
        return 0;
    }
    if(error->status != BLE_HS_EDONE) { fail("Characteristic discovery failed"); return 0; }
    if(io_handle == 0) { fail("IO characteristic not found"); return 0; }

    if(ble_gattc_disc_all_dscs(conn, io_handle, service_end, on_descriptor, SESSION_ARG) != 0) {
        fail("Starting descriptor discovery failed");
    }
    return 0;
}

static int on_service(uint16_t conn, const struct ble_gatt_error* error, const struct ble_gatt_svc* service, void* arg) {
    if(!IS_CURRENT_SESSION(arg)) { return 0; }
    if(error->status == 0) {
        service_start = service->start_handle;
        service_end = service->end_handle;
        return 0;
    }
    if(error->status != BLE_HS_EDONE) { fail("Service discovery failed"); return 0; }
    if(service_start == 0) { fail("Nuki service not found"); return 0; }

    const bool pairing = mode == BLE_CENTRAL_PAIRING;
    ble_uuid128_t chr_uuid = pairing ? nuki_uuid(pairing_service_uuid, GDIO_CHAR_UUID)
                                     : nuki_uuid(keyturner_service_uuid, USDIO_CHAR_UUID);
    if(ble_gattc_disc_chrs_by_uuid(conn, service_start, service_end, &chr_uuid.u, on_characteristic, SESSION_ARG) != 0) {
        fail("Starting characteristic discovery failed");
    }
    return 0;
}

static int on_mtu(uint16_t conn, const struct ble_gatt_error* error, uint16_t mtu, void* arg) {
    if(!IS_CURRENT_SESSION(arg)) { return 0; }
    if(error->status == 0) {
        ESP_LOGI(TAG, "MTU %u", mtu);
    } else {
        ESP_LOGW(TAG, "MTU exchange failed (%d), continuing with default", error->status);
    }

    if(use_cached_handles) {
        ESP_LOGI(TAG, "Using cached handles: IO 0x%04X, CCCD 0x%04X", io_handle, cccd_handle);
        enable_indications(conn);
        return 0;
    }

    const bool pairing = mode == BLE_CENTRAL_PAIRING;
    ble_uuid128_t service_uuid = pairing ? nuki_uuid(pairing_service_uuid, NUKI_PAIRING_SERVICE_VENDOR_UUID)
                                         : nuki_uuid(keyturner_service_uuid, NUKI_KEYTURNER_SERVICE_VENDOR_UUID);
    if(ble_gattc_disc_svc_by_uuid(conn, &service_uuid.u, on_service, SESSION_ARG) != 0) {
        fail("Starting service discovery failed");
    }
    return 0;
}

static void connect_to(const ble_addr_t* addr) {
    ble_gap_disc_cancel();
    timing.lock_found_ms = now_ms();
    peer = *addr;
    connecting = true;
    int rc = ble_gap_connect(own_addr_type, addr, CONNECT_TIMEOUT_MS, &conn_params, gap_event, SESSION_ARG);
    if(rc != 0) {
        connecting = false;
        fail("ble_gap_connect failed");
    }
}

static void on_advertisement(const struct ble_gap_disc_desc* disc) {
    if(connecting || conn_handle != BLE_HS_CONN_HANDLE_NONE) { return; }

    if(mode == BLE_CENTRAL_PAIRING) {
        if(advertises_pairing(disc->data, disc->length_data)) {
            log_advertiser_once(&disc->addr, disc->rssi, "pairing mode");
            connect_to(&disc->addr);
        }
        return;
    }

    if(advertises_keyturner(disc->data, disc->length_data)) {
        bool is_paired_lock = memcmp(disc->addr.val, target_address, 6) == 0;
        log_advertiser_once(&disc->addr, disc->rssi, is_paired_lock ? "paired" : "not paired with this fob");
        if(is_paired_lock) { connect_to(&disc->addr); }
    }
}

// Events belonging to a stopped session: close its connection, never report to the app
static void handle_stale_event(struct ble_gap_event* event) {
    if(event->type == BLE_GAP_EVENT_CONNECT && event->connect.status == 0) {
        ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    } else if(event->type == BLE_GAP_EVENT_DISCONNECT && event->disconnect.conn.conn_handle == conn_handle) {
        conn_handle = BLE_HS_CONN_HANDLE_NONE;
    }
}

static int gap_event(struct ble_gap_event* event, void* arg) {
    if(!IS_CURRENT_SESSION(arg)) {
        handle_stale_event(event);
        return 0;
    }
    switch(event->type) {
        case BLE_GAP_EVENT_DISC:
            on_advertisement(&event->disc);
            break;

        case BLE_GAP_EVENT_DISC_COMPLETE:
            if(!connecting && conn_handle == BLE_HS_CONN_HANDLE_NONE) {
                fail(mode == BLE_CENTRAL_PAIRING ? "No Nuki in pairing mode found" : "Paired Nuki not found");
            }
            break;

        case BLE_GAP_EVENT_CONNECT:
            connecting = false;
            if(event->connect.status != 0) {
                fail("Connection failed");
                break;
            }
            conn_handle = event->connect.conn_handle;
            timing.connected_ms = now_ms();
            struct ble_gap_conn_desc desc;
            if(ble_gap_conn_find(conn_handle, &desc) == 0) {
                timing.conn_interval_x1250us = desc.conn_itvl;
                ESP_LOGI(TAG, "Connected to Nuki, interval %u.%02u ms", desc.conn_itvl * 5 / 4, (desc.conn_itvl * 125) % 100);
            }
            if(ble_gattc_exchange_mtu(conn_handle, on_mtu, SESSION_ARG) != 0) {
                fail("Starting MTU exchange failed");
            }
            break;

        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "Disconnected, reason 0x%03X", event->disconnect.reason);
            conn_handle = BLE_HS_CONN_HANDLE_NONE;
            post(BLE_CENTRAL_EVT_DISCONNECTED, "Disconnected");
            break;

        case BLE_GAP_EVENT_CONN_UPDATE: {
            struct ble_gap_conn_desc desc;
            if(event->conn_update.status == 0 && ble_gap_conn_find(event->conn_update.conn_handle, &desc) == 0) {
                ESP_LOGI(TAG, "Connection interval updated to %u.%02u ms", desc.conn_itvl * 5 / 4, (desc.conn_itvl * 125) % 100);
            }
            break;
        }

        case BLE_GAP_EVENT_NOTIFY_RX:
            if(event->notify_rx.attr_handle == io_handle) {
                uint8_t buffer[256];
                uint16_t length = OS_MBUF_PKTLEN(event->notify_rx.om);
                if(length > sizeof(buffer)) { length = sizeof(buffer); }
                os_mbuf_copydata(event->notify_rx.om, 0, length, buffer);
                bt_comm_on_indication(buffer, length);
            }
            break;

        default:
            break;
    }
    return 0;
}

static void on_sync(void) {
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &own_addr_type);
    synced = true;
    ESP_LOGI(TAG, "BLE ready");
}

static void on_reset(int reason) {
    synced = false;
    ESP_LOGE(TAG, "BLE host reset, reason %d", reason);
}

static void host_task(void* param) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ble_central_init(QueueHandle_t event_queue) {
    events = event_queue;
    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    nimble_port_freertos_init(host_task);
}

bool ble_central_synced(void) {
    return synced;
}

bool ble_central_start(ble_central_mode new_mode, const uint8_t lock_address[6], const ble_central_handles* cached_handles) {
    if(!synced) { return false; }
    session++;
    mode = new_mode;
    if(lock_address != NULL) { memcpy(target_address, lock_address, 6); }
    memset(&peer, 0, sizeof(peer));
    service_start = service_end = 0;
    io_handle = cccd_handle = 0;
    use_cached_handles = cached_handles != NULL;
    if(use_cached_handles) {
        io_handle = cached_handles->io_handle;
        cccd_handle = cached_handles->cccd_handle;
    }
    n_logged_advertisers = 0;
    memset(&timing, 0, sizeof(timing));
    connecting = false;
    active = true;

    // Passive scan, same as m_scan_params in src/main.c
    struct ble_gap_disc_params params = {
        .itvl = 0x0018,   // 15 ms
        .window = 0x0018, // 15 ms
        .passive = 1,
        .filter_duplicates = 0,
    };
    int rc = ble_gap_disc(own_addr_type, SCAN_DURATION_MS, &params, gap_event, SESSION_ARG);
    if(rc != 0) {
        ESP_LOGE(TAG, "ble_gap_disc failed: %d", rc);
        active = false;
        return false;
    }
    ESP_LOGI(TAG, "Scanning for %s", mode == BLE_CENTRAL_PAIRING ? "a Nuki in pairing mode" : "the paired Nuki");
    return true;
}

void ble_central_stop(void) {
    active = false;
    ble_gap_disc_cancel();
    if(connecting) {
        ble_gap_conn_cancel();
        connecting = false;
    }
    if(conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
}

ble_central_timing ble_central_get_timing(void) { return timing; }
uint16_t ble_central_conn_handle(void) { return conn_handle; }
uint16_t ble_central_io_handle(void) { return io_handle; }
ble_central_handles ble_central_get_handles(void) {
    return (ble_central_handles){ .io_handle = io_handle, .cccd_handle = cccd_handle };
}
void ble_central_peer_address(uint8_t out[6]) { memcpy(out, peer.val, 6); }
