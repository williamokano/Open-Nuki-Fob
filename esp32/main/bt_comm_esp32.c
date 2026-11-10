#include "bt_comm_esp32.h"
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "host/ble_hs.h"

#define BUFFER_SIZE 200
#define NUKI_ERROR_REPORT_CMD 0x0012

static const char* TAG = "bt_comm";
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;

uint8_t send_buffer[BUFFER_SIZE];

static uint8_t out_buffer[BUFFER_SIZE];
static uint16_t out_length;
static bool write_pending;
static volatile bool write_failed;

static bt_callback response_callback;
static uint8_t response_buffer[BUFFER_SIZE];
static uint16_t expected_response_length;
static volatile uint16_t response_progress;

void send_with_response(uint8_t* message_out, uint16_t message_out_length,
    uint16_t expected_length, bt_callback callback) {
    portENTER_CRITICAL(&lock);
    if(message_out_length <= BUFFER_SIZE) {
        memcpy(out_buffer, message_out, message_out_length);
    }
    out_length = message_out_length;
    write_pending = true;
    response_callback = callback;
    expected_response_length = expected_length;
    response_progress = 0;
    portEXIT_CRITICAL(&lock);
}

void bt_comm_reset(void) {
    portENTER_CRITICAL(&lock);
    out_length = 0;
    write_pending = false;
    write_failed = false;
    response_callback = NULL;
    expected_response_length = 0;
    response_progress = 0;
    portEXIT_CRITICAL(&lock);
}

void bt_comm_on_indication(const uint8_t* data, uint16_t length) {
    ESP_LOGI(TAG, "<- %u bytes", length);
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, data, length, ESP_LOG_INFO);

    // Unencrypted error report from the pairing service: cmd 0x0012, error code, failed cmd
    if(length >= 3 && (data[0] | (data[1] << 8)) == NUKI_ERROR_REPORT_CMD) {
        ESP_LOGE(TAG, "Nuki reported error 0x%02X", data[2]);
    }

    portENTER_CRITICAL(&lock);
    if(expected_response_length > 0 && response_progress < expected_response_length &&
        response_progress + length <= BUFFER_SIZE) {
        memcpy(&response_buffer[response_progress], data, length);
        response_progress += length;
    }
    portEXIT_CRITICAL(&lock);
}

static int on_write_done(uint16_t conn_handle, const struct ble_gatt_error* error,
    struct ble_gatt_attr* attr, void* arg) {
    if(error->status != 0) {
        ESP_LOGE(TAG, "Long write failed, status %d", error->status);
        write_failed = true;
    }
    return 0;
}

static bool start_write(uint16_t connection_handle, uint16_t attribute_handle) {
    if(out_length == 0 || out_length > BUFFER_SIZE) {
        // The protocol code returns length 0 when building a payload fails (e.g. challenge decryption)
        ESP_LOGE(TAG, "No valid payload to send (length %u)", out_length);
        return false;
    }
    ESP_LOGI(TAG, "-> %u bytes (expecting %u)", out_length, expected_response_length);
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, out_buffer, out_length, ESP_LOG_INFO);

    // Prepared writes + execute write, like gattc_long_write() in src/bt_comm.c
    struct os_mbuf* om = ble_hs_mbuf_from_flat(out_buffer, out_length);
    if(om == NULL) { return false; }
    int rc = ble_gattc_write_long(connection_handle, attribute_handle, 0, om, on_write_done, NULL);
    if(rc != 0) {
        ESP_LOGE(TAG, "ble_gattc_write_long failed: %d", rc);
        return false;
    }
    return true;
}

bool bt_comm_process(void* context, uint16_t connection_handle, uint16_t attribute_handle) {
    if(write_failed) { return false; }

    if(write_pending) {
        write_pending = false;
        if(!start_write(connection_handle, attribute_handle)) { return false; }
    }

    bt_callback callback = NULL;
    uint16_t length = 0;
    portENTER_CRITICAL(&lock);
    if(response_callback != NULL && expected_response_length > 0 && response_progress >= expected_response_length) {
        callback = response_callback;
        length = expected_response_length;
        response_callback = NULL;
        expected_response_length = 0;
        response_progress = 0;
    }
    portEXIT_CRITICAL(&lock);

    // The callback may queue the next message via send_with_response()
    if(callback != NULL) {
        callback(context, response_buffer, length);
    }
    return true;
}
