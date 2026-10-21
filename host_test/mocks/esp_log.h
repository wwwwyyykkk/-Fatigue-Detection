#pragma once

// Minimal ESP-IDF logging stubs for host-side tests.
// The production code includes esp_log.h, but the PERCLOS logic itself
// does not depend on the ESP-IDF runtime.

#define ESP_LOGE(tag, fmt, ...) ((void)0)
#define ESP_LOGW(tag, fmt, ...) ((void)0)
#define ESP_LOGI(tag, fmt, ...) ((void)0)
#define ESP_LOGD(tag, fmt, ...) ((void)0)
#define ESP_LOGV(tag, fmt, ...) ((void)0)
