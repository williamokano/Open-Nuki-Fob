// Persistent fob data on ESP32 (NVS), replacing the nRF flash pages of src/fob_data.c.
// The test harness keeps a single pairing.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "nuki.h"

void fob_store_init(void);
bool fob_store_load_key(nuki_key* out);
esp_err_t fob_store_save_key(const nuki_key* key);
// GATT handle cache for the paired lock (replaces handle_cache in src/fob_data.c)
bool fob_store_load_handles(const uint8_t lock_address[6], uint16_t* io_handle, uint16_t* cccd_handle);
void fob_store_save_handles(const uint8_t lock_address[6], uint16_t io_handle, uint16_t cccd_handle);
void fob_store_clear_handles(void);

// Diagnostics: short text (e.g. last operation timing) that survives power loss
void fob_store_save_note(const char* text);
bool fob_store_load_note(char* out, size_t out_size);

// Same derivation as get_fob_settings() in src/fob_data.c, using the ESP32 MAC instead of FICR->DEVICEADDR.
void fob_store_identity(char fob_name[32], uint32_t* app_id);
