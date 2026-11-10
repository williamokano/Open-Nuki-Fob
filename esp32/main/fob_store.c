#include "fob_store.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char* TAG = "fob_store";
static const char* NVS_NAMESPACE = "fob";
static const char* NVS_KEY = "key";
static const char* NVS_NOTE = "note";
static const char* NVS_HANDLES = "handles";

typedef struct {
    uint8_t lock_address[6];
    uint16_t io_handle;
    uint16_t cccd_handle;
} cached_handles;

void fob_store_init(void) {
    esp_err_t err = nvs_flash_init();
    if(err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

bool fob_store_load_key(nuki_key* out) {
    nvs_handle_t handle;
    if(nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) { return false; }
    size_t length = sizeof(nuki_key);
    esp_err_t err = nvs_get_blob(handle, NVS_KEY, out, &length);
    nvs_close(handle);
    return err == ESP_OK && length == sizeof(nuki_key);
}

esp_err_t fob_store_save_key(const nuki_key* key) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if(err != ESP_OK) { return err; }
    err = nvs_set_blob(handle, NVS_KEY, key, sizeof(nuki_key));
    if(err == ESP_OK) { err = nvs_commit(handle); }
    nvs_close(handle);
    if(err != ESP_OK) { ESP_LOGE(TAG, "Saving pairing failed: %s", esp_err_to_name(err)); }
    return err;
}

bool fob_store_load_handles(const uint8_t lock_address[6], uint16_t* io_handle, uint16_t* cccd_handle) {
    nvs_handle_t handle;
    if(nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) { return false; }
    cached_handles cache;
    size_t length = sizeof(cache);
    esp_err_t err = nvs_get_blob(handle, NVS_HANDLES, &cache, &length);
    nvs_close(handle);
    if(err != ESP_OK || length != sizeof(cache) || memcmp(cache.lock_address, lock_address, 6) != 0) { return false; }
    *io_handle = cache.io_handle;
    *cccd_handle = cache.cccd_handle;
    return true;
}

void fob_store_save_handles(const uint8_t lock_address[6], uint16_t io_handle, uint16_t cccd_handle) {
    nvs_handle_t handle;
    if(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) { return; }
    cached_handles cache = { .io_handle = io_handle, .cccd_handle = cccd_handle };
    memcpy(cache.lock_address, lock_address, 6);
    if(nvs_set_blob(handle, NVS_HANDLES, &cache, sizeof(cache)) == ESP_OK) { nvs_commit(handle); }
    nvs_close(handle);
}

void fob_store_clear_handles(void) {
    nvs_handle_t handle;
    if(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) { return; }
    if(nvs_erase_key(handle, NVS_HANDLES) == ESP_OK) { nvs_commit(handle); }
    nvs_close(handle);
}

void fob_store_save_note(const char* text) {
    nvs_handle_t handle;
    if(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) { return; }
    if(nvs_set_str(handle, NVS_NOTE, text) == ESP_OK) { nvs_commit(handle); }
    nvs_close(handle);
}

bool fob_store_load_note(char* out, size_t out_size) {
    nvs_handle_t handle;
    if(nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) { return false; }
    esp_err_t err = nvs_get_str(handle, NVS_NOTE, out, &out_size);
    nvs_close(handle);
    return err == ESP_OK;
}

void fob_store_identity(char fob_name[32], uint32_t* app_id) {
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_efuse_mac_get_default(mac));
    uint32_t device_address = ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16) | ((uint32_t)mac[4] << 8) | mac[5];
    memset(fob_name, 0, 32);
    snprintf(fob_name, 32, "Open Nuki Fob %08lX", (unsigned long)device_address);
    *app_id = device_address;
}
