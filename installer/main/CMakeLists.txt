idf_component_register(
    SRCS "installer_main.c"
         "installer_flash.c"
         "installer_ble.c"
         "installer_ui.c"
         "installer_wifi.c"
    INCLUDE_DIRS "."
    REQUIRES bsp installer_core bt bootloader_support esp_app_format esp_rom esp_timer esp_partition mbedtls nvs_flash spi_flash
)
