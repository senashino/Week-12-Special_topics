idf_component_register(
    SRCS "espnow_receiver.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi nvs_flash
)
