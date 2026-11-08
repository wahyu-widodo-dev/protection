idf_component_register(
    SRCS "protection.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_common esp_driver_gpio esp_timer log
)
