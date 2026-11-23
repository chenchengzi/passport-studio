idf_component_register(
    SRCS "link_core.c"
         "passport_link.c"
    INCLUDE_DIRS "include"
    REQUIRES bt json nvs_flash
    PRIV_REQUIRES esp_app_format esp_hw_support
)
