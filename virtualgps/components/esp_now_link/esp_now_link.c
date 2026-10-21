#include <string.h>
#include <stdlib.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_now.h>
#include "esp_now_link.h"

static const char *TAG = "esp_now_link";

static void (*s_recv_cb)(const uint8_t *mac, esp_now_msg_t *msg) = NULL;
static uint8_t s_peer_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static void esp_now_recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (data == NULL || len < sizeof(uint8_t) + sizeof(uint8_t)) {
        return;
    }
    esp_now_msg_t *msg = (esp_now_msg_t *)data;
    int msg_len = sizeof(msg->cmd) + sizeof(msg->data_len) + msg->data_len;
    if (len < msg_len) {
        return;
    }
    if (s_recv_cb) {
        s_recv_cb(info->src_addr, msg);
    }
}

static void esp_now_send_cb(const esp_now_send_info_t *tx_info, esp_now_send_status_t status)
{
    if (status != ESP_NOW_SEND_SUCCESS) {
        ESP_LOGW(TAG, "Send failed, status: %d", status);
    }
}

esp_err_t esp_now_link_init(void)
{
    uint8_t self_mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, self_mac);
    ESP_LOGI(TAG, "Self MAC: %02x:%02x:%02x:%02x:%02x:%02x",
             self_mac[0], self_mac[1], self_mac[2], self_mac[3], self_mac[4], self_mac[5]);

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(esp_now_recv_cb));
    ESP_ERROR_CHECK(esp_now_register_send_cb(esp_now_send_cb));

    uint8_t peer_mac[6] = ESP_NOW_LINK_PEER_MAC;
    memcpy(s_peer_mac, peer_mac, 6);
    esp_now_peer_info_t peer = {};
    peer.channel = 0;  /* 0 = use current WiFi channel */
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    memcpy(peer.peer_addr, s_peer_mac, 6);
    esp_err_t ret = esp_now_add_peer(&peer);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to add peer: %s", esp_err_to_name(ret));
    }

    ESP_LOGI(TAG, "ESP-NOW link initialized");
    return ESP_OK;
}

esp_err_t esp_now_link_send(uint8_t cmd, uint8_t *data, uint8_t len)
{
    if (len > ESPNOW_DATA_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_now_msg_t msg;
    msg.cmd = cmd;
    msg.data_len = len;
    if (len > 0 && data != NULL) {
        memcpy(msg.data, data, len);
    }
    esp_err_t ret = esp_now_send(s_peer_mac, (uint8_t *)&msg, sizeof(msg.cmd) + sizeof(msg.data_len) + len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_send failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

void esp_now_link_set_recv_cb(void (*cb)(const uint8_t *mac, esp_now_msg_t *msg))
{
    s_recv_cb = cb;
}
