# ── I2S 语音播报封装（MAX98357） ──
# 基于 ESP-IDF I2S 驱动

idf_component_register(
    SRCS "voice_prompter.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        freertos
        heap
        log
        esp_timer
        driver
)
