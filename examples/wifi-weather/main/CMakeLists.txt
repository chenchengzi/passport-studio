idf_component_register(
    SRCS "weather_main.c"
         "weather_logic.c"
         "weather_i18n.c"
         "fonts/wx_font_zh_14.c"
         "fonts/wx_font_zh_20.c"
         "weather_net.c"
         "weather_ui.c"
    INCLUDE_DIRS "."
    REQUIRES bsp passport_link passport_wifi esp_event esp_http_client esp_netif esp_wifi json mbedtls nvs_flash
)
