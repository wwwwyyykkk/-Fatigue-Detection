#pragma once
#include <stdint.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ESPNOW_DATA_MAX_LEN 200

#ifndef ESP_NOW_LINK_PEER_MAC
#define ESP_NOW_LINK_PEER_MAC {0xe8, 0xf6, 0x0a, 0xa8, 0x9f, 0x1c}
#endif

typedef struct {
    uint8_t cmd;
    uint8_t data_len;
    uint8_t data[ESPNOW_DATA_MAX_LEN];
} esp_now_msg_t;

esp_err_t esp_now_link_init(void);

esp_err_t esp_now_link_send(uint8_t cmd, uint8_t *data, uint8_t len);

void esp_now_link_set_recv_cb(void (*cb)(const uint8_t *mac, esp_now_msg_t *msg));

#ifdef __cplusplus
}
#endif
