/**
 * main.c - 虚拟GPS模拟板主程序
 *
 * 功能：通过6个按键模拟GPS/驾驶数据，OLED实时显示，UART发送给主控板
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "driver/i2s_std.h"

#include "config.h"
#include "gps_state.h"
#include "buttons_gpio.h"
#include "uart_protocol.h"
#include "oled_display.h"
// #include "voice_prompter.hpp"     /* 不用 MAX98357 I2S */
// #include "audio_resources.h"
#include "syn6288.h"
#include "syn6288_test.h"
#include "alcohol_sensor.h"
#include "app_priv.h"
#include "esp_sr_task.h"

/* ESP-NOW / WiFi / RainMaker */
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_log.h"

#include <esp_rmaker_core.h>
#include <esp_rmaker_standard_types.h>
#include <esp_rmaker_standard_params.h>
#include <esp_rmaker_standard_devices.h>
#include <esp_rmaker_schedule.h>
#include <esp_rmaker_scenes.h>
#include <esp_rmaker_console.h>
#include <esp_rmaker_ota.h>
#include <esp_rmaker_common_events.h>
#include <app_network.h>
#include <app_insights.h>

static const char *TAG = "MAIN";

/* ========== GPS 模块测试任务 (GY-GPS6MV2, UART1, 9600bps) ========== */
#include "driver/uart.h"
#include <stdlib.h>

/* 把 NMEA DDMM.MMMMM 格式转为十进制度数 */
static float nmea_to_deg(const char *ddmm, char dir)
{
    // NMEA 格式: DDMM.MMMMM (纬度) 或 DDDMM.MMMMM (经度)
    // 小数点前最后2位始终是"分"的整数部分
    // 例: 2602.39067 → 26° + 02.39067′
    // 例: 11911.56321 → 119° + 11.56321′
    char *dot = strchr(ddmm, '.');
    if (!dot) return 0.0f;

    int int_len = dot - ddmm;  // 小数点前长度
    if (int_len < 3) return 0.0f;

    // 分 = 最后2位整数 + 小数点 + 小数部分
    char min_str[16];
    strncpy(min_str, ddmm + int_len - 2, 2);
    min_str[2] = '.';
    strncpy(min_str + 3, dot + 1, 12);
    min_str[15] = '\0';

    // 度 = 小数点前去掉最后2位
    char deg_str[8] = {0};
    strncpy(deg_str, ddmm, int_len - 2);

    float deg = atof(deg_str);
    float minutes = atof(min_str);
    float result = deg + minutes / 60.0f;

    if (dir == 'S' || dir == 'W') result = -result;
    return result;
}

static void gps_reader_task(void *arg)
{
    uart_config_t uart_config = {
        .baud_rate  = GPS_UART_BAUD_RATE,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
    };
    ESP_ERROR_CHECK(uart_driver_install(GPS_UART_PORT, GPS_UART_BUF_SIZE, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(GPS_UART_PORT, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(GPS_UART_PORT, GPS_UART_TX_PIN, GPS_UART_RX_PIN,
                                  UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_LOGI(TAG, "GPS task started (UART1, GPIO16 RX, 9600bps)");

    TickType_t last_fix_tick = 0;
    bool had_fix = false;
    char buf[256];
    while (1) {
        int len = uart_read_bytes(GPS_UART_PORT, (uint8_t*)buf, sizeof(buf) - 1,
                                  pdMS_TO_TICKS(200));
        if (len > 0) {
            buf[len] = '\0';
            char *line = buf;
            while (*line) {
                char *end = strchr(line, '\n');
                if (end) {
                    *end = '\0';
                    if (strlen(line) > 6 && line[0] == '$' && line[3] == 'R' && line[4] == 'M' && line[5] == 'C') {
                        // $GPRMC,time,status,lat,N/S,lon,E/W,speed,course,date,...*cs
                        char *save;
                        char *tok = strtok_r(line, ",", &save);
                        int field = 0;
                        char lat_str[16] = {0}, ns = 0, lon_str[16] = {0}, ew = 0, status = 0;
                        while (tok) {
                            switch (field) {
                                case 1: /* time */ break;
                                case 2: status = tok[0]; break;       // A=Valid V=Invalid
                                case 3: strncpy(lat_str, tok, 15); break;
                                case 4: ns = tok[0]; break;
                                case 5: strncpy(lon_str, tok, 15); break;
                                case 6: ew = tok[0]; break;
                                case 7: /* speed */ break;
                                case 8: /* course */ break;
                                case 9: /* date */ break;
                            }
                            tok = strtok_r(NULL, ",", &save);
                            field++;
                        }
                        if (status == 'A' && ns && ew && strlen(lat_str) > 0 && strlen(lon_str) > 0) {
                            float lat = nmea_to_deg(lat_str, ns);
                            float lng = nmea_to_deg(lon_str, ew);
                            ESP_LOGI(TAG, "📍 lat=%.6f  lng=%.6f  (valid=%c)", lat, lng, status);
                            // 真实GPS模式 → 更新全局状态
                            if (gps_state_is_real_gps()) {
                                gps_state_set_gps(lat, lng);
                            }
                            // 更新信号状态
                            last_fix_tick = xTaskGetTickCount();
                            if (!had_fix) {
                                had_fix = true;
                                gps_state_set_fix_status(true);
                            }
                        }
                    }
                    line = end + 1;
                } else {
                    break;
                }
            }
        }
        // 超时检测：超过5秒没有有效定位 → 信号丢失
        if (had_fix && (xTaskGetTickCount() - last_fix_tick > pdMS_TO_TICKS(5000))) {
            had_fix = false;
            gps_state_set_fix_status(false);
        }
    }
}

/* ========== 自动走时任务 ========== */
static void time_tick_task(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(TIME_TICK_INTERVAL_MS));
        gps_state_tick_second();
    }
}

/* ── 疲劳等级变化回调 ── */
static void on_fatigue_change(int old_level, int new_level)
{
    ESP_LOGI(TAG, "Fatigue changed: %d - %d", old_level, new_level);
    /* 只在疲劳加重（等级升高）时提醒；等级下降是好转（如严重→轻度、严重→30% 附近的严重），不提醒 */
    if (new_level <= old_level) return;
    if (new_level == 2)      syn6288_speak(SYN_VOICE_SEVERE_FATIGUE);
    else if (new_level == 1) syn6288_speak(SYN_VOICE_MILD_FATIGUE);
}

/* ═══════════════════════════════════════════════════════
 *  WiFi + RainMaker 初始化 (替代硬编码 SSID/密码)
 *  使用 RainMaker APP 扫码配网 (BLE Provisioning)
 * ═══════════════════════════════════════════════════════ */
static esp_err_t wifi_rainmaker_init(void)
{
    /* Step 1: NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Step 2: WiFi hardware init only (no connect yet — RainMaker needs MAC first) */
    app_network_init();

    /* Step 3: Initialize RainMaker node (WiFi HW ready, MAC available) */
    ESP_ERROR_CHECK(rainmaker_init());
    rainmaker_register_event_handlers();

    /* Step 4: Start WiFi provisioning (blocks until connected or timeout) */
    ESP_LOGI(TAG, "Starting WiFi provisioning (BLE QR code)...");
    ret = app_network_start(POP_TYPE_RANDOM);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi provisioning failed! Aborting...");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "WiFi connected via RainMaker provisioning");

    /* Step 5: Disable power save for low-latency ESP-NOW */
    esp_wifi_set_ps(WIFI_PS_NONE);

    uint8_t self_mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, self_mac);
    ESP_LOGI(TAG, "Self MAC: %02x:%02x:%02x:%02x:%02x:%02x",
             self_mac[0], self_mac[1], self_mac[2], self_mac[3], self_mac[4], self_mac[5]);

    ESP_LOGI(TAG, "WiFi + RainMaker ready");
    return ESP_OK;
}

/* INMP441 I2S 麦克风采集已移入 esp_sr_task.cpp (ESP-SR) — 2026-08-06 */

void app_main(void)
{
    esp_rmaker_console_init();

    ESP_LOGI(TAG, "============================================");
    ESP_LOGI(TAG, "  Virtual GPS Simulator Board + RainMaker");
    ESP_LOGI(TAG, "  ESP32-S3 | OLED + SYN6288 + ESP-NOW + Cloud");
    ESP_LOGI(TAG, "============================================");

    /* 1. 初始化全局状态 */
    gps_state_init();

    /* 2. 初始化 WiFi + RainMaker + ESP-NOW */
    ESP_LOGI(TAG, "Init WiFi (RainMaker provisioning) + ESP-NOW...");
    if (wifi_rainmaker_init() == ESP_OK) {
        ESP_ERROR_CHECK(esp_now_protocol_init_wifi());
        /* Start periodic GPS state report to RainMaker cloud */
        rainmaker_start_periodic_report();
    } else {
        ESP_LOGE(TAG, "WiFi/RainMaker init failed! VirtualGPS comm unavailable.");
    }

    /* 2. 初始化硬件驱动 */
    buttons_init();
    uart_protocol_init();       /* ESP-NOW 模式：UART2 硬件已禁用 */
    alcohol_sensor_init();      /* MQ3 酒精传感器 */
    oled_display_init();
    // voice_prompter_init();   /* 不用 MAX98357 I2S */
    syn6288_init();

    /* ── 主控板疲劳等级变化 → SYN6288 语音联动 ── */
    uart_set_fatigue_change_callback(on_fatigue_change);

    /* 3. 启动 RainMaker 云服务 (调度 / Insights)
     * 2026-08-06: OTA 已去掉（分区表去 ota_0/ota_1 加 model 分区给 ESP-SR） */
    // esp_rmaker_ota_enable_default();
    esp_rmaker_timezone_service_enable();
    esp_rmaker_schedule_enable();
    esp_rmaker_scenes_enable();
    app_insights_enable();
    esp_rmaker_start();
    ESP_LOGI(TAG, "RainMaker cloud services started");

    /* 4. 启动任务 */
    buttons_start_scan_task();      // 按键扫描任务 (优先级5)
    uart_start_heartbeat_task();    // UART心跳任务 (优先级3)
    oled_start_refresh_task();      // OLED刷新任务 (优先级2)
    xTaskCreate(time_tick_task, "time_tick", 2048, NULL, 1, NULL);  // 自动走时 (优先级1)

    /* 4. 启动 GPS 测试任务 (UART1, GY-GPS6MV2) */
    xTaskCreate(gps_reader_task, "gps_reader", 3072, NULL, 3, NULL);

    /* 5. ESP-SR 语音识别 (INMP441 I2S 麦克风, 唤醒词"你好小智"+命令) */
    if (esp_sr_init()) {
        ESP_LOGI(TAG, "ESP-SR started");
    } else {
        ESP_LOGE(TAG, "ESP-SR init failed!");
    }

    ESP_LOGI(TAG, "All tasks started. System ready.");

    /* ── 内存诊断 (2026-08-03)：看 SRAM 剩余 + PSRAM 是否可用 ── */
    ESP_LOGI(TAG, "MEM: free_heap=%luKB internal=%luKB psram_total=%luKB psram_free=%luKB",
             esp_get_free_heap_size() / 1024,
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024,
             heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024,
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);

    /* 播放开机提示音 */
    vTaskDelay(pdMS_TO_TICKS(200));
    // voice_prompter_play(boot_wav, boot_wav_len, NULL, NULL);  /* 不用 MAX98357 I2S */
    syn6288_speak(SYN_VOICE_SYS_BOOT);
    /* "开始进行身份验证" 不再在 VirtualGPS 开机时播（主板可能还没启动）。
     * 改为收到主板 IDENTIFY_EVENT_START（摄像头打开/开始人脸检测）时播，
     * 见 uart_protocol.c 的 identify_event 分发。 */

    ESP_LOGI(TAG, "----------------------------------------");
    ESP_LOGI(TAG, "Button mapping (ADC GPIO1):");
    ESP_LOGI(TAG, "  BTN1(1k)   -> Cycle speed");
    ESP_LOGI(TAG, "  BTN2(2k)   -> Cycle location");
    ESP_LOGI(TAG, "  BTN3(3.9k) -> (not used)");
    ESP_LOGI(TAG, "  BTN4(8.2k) -> Toggle real/virtual GPS");
    ESP_LOGI(TAG, "  BTN5(16k)  -> Toggle driver");
    ESP_LOGI(TAG, "  BTN6(33k)  -> Add 30min drive time");
    ESP_LOGI(TAG, "----------------------------------------");
    ESP_LOGI(TAG, "TTS: SYN6288 (GPIO13 soft-UART, 9600bps)");
    ESP_LOGI(TAG, "MQ3: Alcohol sensor (ADC=GPIO2 DO=GPIO6)");
    ESP_LOGI(TAG, "ESP-NOW: WiFi 双向通信 (替换 UART2 有线)");
    ESP_LOGI(TAG, "  → Main: GPS state data (cmd 0x10)");
    ESP_LOGI(TAG, "  ← Main: Fatigue/face/sys state (cmd 0x20)");
    ESP_LOGI(TAG, "----------------------------------------");
    ESP_LOGI(TAG, "Commands: test_syn | scan_baud | say_hi");
    ESP_LOGI(TAG, "----------------------------------------");

    /* 主任务：监听串口命令 + 接收F-prefix数据 */
    char cmd_buf[32];
    int cmd_idx = 0;

    while (1) {
        uart_poll_receive();  /* Check for F-prefix from main board */

        /* MQ3 alcohol check */
        if (alcohol_sensor_check()) {
            syn6288_speak(SYN_VOICE_ALCOHOL_WARNING);
        }

        int c = getchar();
        if (c != EOF) {
            if (c == '\n' || c == '\r') {
                if (cmd_idx > 0) {
                    cmd_buf[cmd_idx] = '\0';

                    // 处理命令
                    if (strcmp(cmd_buf, "test_syn") == 0) {
                        ESP_LOGI(TAG, ">>> Running SYN6288 diagnostics...");
                        syn6288_run_diagnostics();
                    } else if (strcmp(cmd_buf, "scan_baud") == 0) {
                        ESP_LOGI(TAG, ">>> Scanning SYN6288 baud rates...");
                        syn6288_scan_baud_rate();
                    } else if (strcmp(cmd_buf, "say_hi") == 0) {
                        ESP_LOGI(TAG, ">>> Sending test speech...");
                        syn6288_speak(SYN_VOICE_SYS_BOOT);
                    } else {
                        ESP_LOGI(TAG, "Unknown command: %s", cmd_buf);
                    }

                    cmd_idx = 0;
                }
            } else if (cmd_idx < sizeof(cmd_buf) - 1) {
                cmd_buf[cmd_idx++] = (char)c;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
