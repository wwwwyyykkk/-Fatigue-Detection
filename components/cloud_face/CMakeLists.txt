idf_component_register(
    SRCS "cloud_face.cpp"
    INCLUDE_DIRS "."
    REQUIRES esp_http_client json nvs_flash esp_timer mbedtls espressif__esp_new_jpeg
)