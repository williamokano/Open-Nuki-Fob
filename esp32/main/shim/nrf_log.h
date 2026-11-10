// Shim: maps the nRF5 SDK logger used by the shared sources onto ESP-IDF logging.
#pragma once
#include "esp_log.h"

#define NRF_LOG_ERROR(...)   ESP_LOGE("nuki", __VA_ARGS__)
#define NRF_LOG_WARNING(...) ESP_LOGW("nuki", __VA_ARGS__)
#define NRF_LOG_INFO(...)    ESP_LOGI("nuki", __VA_ARGS__)
#define NRF_LOG_DEBUG(...)   ESP_LOGD("nuki", __VA_ARGS__)
