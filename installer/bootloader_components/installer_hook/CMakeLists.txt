idf_component_register(
    SRCS "installer_hook.c"
    INCLUDE_DIRS "../../../components/installer_core/include"
    PRIV_REQUIRES bootloader_support hal esp_rom
)
