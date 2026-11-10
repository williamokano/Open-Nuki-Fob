// NimBLE central: finds a Nuki, connects, discovers the IO characteristic and enables indications.
// Port of the BLE parts of src/main.c (scan, connect, discover_services, enable_indications).
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

typedef enum {
    BLE_CENTRAL_EVT_READY,         // connected, IO characteristic found, indications enabled
    BLE_CENTRAL_EVT_FAILED,        // scan timeout, connect or discovery failure
    BLE_CENTRAL_EVT_DISCONNECTED,
} ble_central_evt_type;

typedef struct {
    ble_central_evt_type type;
    const char* reason;
} ble_central_evt;

typedef enum {
    BLE_CENTRAL_PAIRING,   // connect to the first lock in pairing mode
    BLE_CENTRAL_KEYTURNER, // connect to the paired lock with the given address
} ble_central_mode;

// GATT handles of the Nuki IO characteristic, cached to skip service discovery (like the nRF handle cache)
typedef struct {
    uint16_t io_handle;
    uint16_t cccd_handle;
} ble_central_handles;

void ble_central_init(QueueHandle_t event_queue);
bool ble_central_synced(void);
// cached_handles may be NULL: then the IO characteristic is discovered
bool ble_central_start(ble_central_mode mode, const uint8_t lock_address[6], const ble_central_handles* cached_handles);
void ble_central_stop(void);

uint16_t ble_central_conn_handle(void);
uint16_t ble_central_io_handle(void);
ble_central_handles ble_central_get_handles(void);
void ble_central_peer_address(uint8_t out[6]);

// Timestamps (ms since boot, 0 = not reached) of the last ble_central_start() run
typedef struct {
    int64_t lock_found_ms;
    int64_t connected_ms;
    int64_t ready_ms;
    uint16_t conn_interval_x1250us; // connection interval in 1.25 ms units
} ble_central_timing;
ble_central_timing ble_central_get_timing(void);
