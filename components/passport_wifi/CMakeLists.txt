idf_component_register(
    SRCS "passport_wifi.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi
    PRIV_REQUIRES esp_partition installer_core
)
