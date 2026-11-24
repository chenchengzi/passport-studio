idf_component_register(
    SRCS "t3_main.c"
         "t3_logic.c"
         "t3_text.c"
         "fonts/t3_font_zh_14.c"
         "fonts/t3_font_zh_20.c"
         "t3_net.c"
         "t3_ui.c"
    INCLUDE_DIRS "."
    REQUIRES bsp passport_link passport_wifi esp_event esp_http_client esp_netif esp_timer esp_wifi nvs_flash
)
