// Shim: the shared utils.c gets its random bytes from the SoftDevice RNG.
// On ESP32 use the hardware RNG (true random while the radio is enabled).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_random.h"

static inline uint32_t sd_rand_application_vector_get(uint8_t* buffer, size_t length) {
    esp_fill_random(buffer, length);
    return 0;
}
