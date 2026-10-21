idf_component_register(
    SRCS "main.cpp" "ScreenDriver.cpp" "lvgl_ui.cpp" "lv_font_chinese_16.c" "lv_font_chinese_16b.c" "lv_font_chinese_18b.c" "lv_font_fancy_44.c" "gps_uart.cpp" "mic_test.cpp" "esp_sr_task.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        usb
        usb_host_uvc
        esp_new_jpeg
        esp-dl
        human_face_detect
        eye_classifier
        face_recognizer
        freertos
        driver
        esp_timer
        heap
        log
        esp_lcd
        esp_wifi
        esp_event
        esp_netif
        nvs_flash
        esp_now_link
        cloud_face
        usb_host_uac
        esp-sr
)
