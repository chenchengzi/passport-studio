// installer/bootloader_components/installer_hook/installer_hook.c
// Decides, before the normal boot flow, whether to start the resident installer
// at INST_APP_OFFSET instead of the installed play:
//   1. the install journal is pending or torn (an install was interrupted);
//   2. the partition table names no factory/ota_0 app, or that slot holds no image;
//   3. a function key is held at power-on (skipped on deep-sleep wake, so plays
//      that wake on a key press do not open the installer).
// Otherwise it returns and the stock bootloader boots the play as usual.
#include <stdbool.h>
#include <stdint.h>

#include "bootloader_common.h"
#include "bootloader_utility.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_gpio.h"
#include "esp_rom_sys.h"
#include "hal/gpio_ll.h"
#include "soc/reset_reasons.h"

#include "inst_boot.h"
#include "inst_layout.h"
#include "inst_state.h"

// ESP-IDF 5.5.3 declares this in bootloader_flash_priv.h, which also pulls in
// application-only SPI Flash headers that the bootloader build cannot see.
extern esp_err_t bootloader_flash_read(size_t src_addr, void *dest, size_t size, bool allow_decrypt);

// Same pin as BSP_BTN_GPIO; bsp_pins.h cannot be included in the bootloader.
#define KEY_GPIO 0

static const char *TAG = "installer";

// bootloader_flash_read() rejects sizes and destinations that are not 4-byte
// aligned, so every read below uses whole words.
static bool image_at(uint32_t offset)
{
    uint32_t word = 0;
    return bootloader_flash_read(offset, &word, sizeof(word), false) == ESP_OK &&
           (word & 0xFF) == INST_IMAGE_MAGIC;
}

static void boot_installer(const char *why)
{
    // A USB flash of an ordinary merged image may have replaced the installer.
    // Fall through to the stock boot flow instead of jumping into erased Flash.
    if (!image_at(INST_APP_OFFSET)) {
        ESP_LOGW(TAG, "%s, but no installer at 0x%x", why, INST_APP_OFFSET);
        return;
    }
    ESP_LOGI(TAG, "%s: starting installer at 0x%x", why, INST_APP_OFFSET);
    bootloader_state_t state = {0};
    state.factory.offset = INST_APP_OFFSET;
    state.factory.size = INST_APP_SIZE;
    bootloader_utility_load_boot_image(&state, FACTORY_INDEX);   // does not return on success
}

static bool play_present(void)
{
    static uint32_t table[INST_TABLE_MAX / 4];
    if (bootloader_flash_read(INST_TABLE_OFFSET, table, sizeof(table), false) != ESP_OK) return false;
    uint32_t offset = inst_boot_app_offset((const uint8_t *)table, sizeof(table));
    return offset != 0 && image_at(offset);
}

static bool key_held(void)
{
    // GPIO0 has an external 10 k pull-up; every function key pulls it low.
    esp_rom_gpio_pad_select_gpio(KEY_GPIO);
    gpio_ll_input_enable(&GPIO, KEY_GPIO);
    gpio_ll_pullup_dis(&GPIO, KEY_GPIO);
    gpio_ll_pulldown_dis(&GPIO, KEY_GPIO);
    inst_hold_t hold;
    inst_hold_init(&hold, esp_log_early_timestamp());
    for (;;) {
        inst_hold_result_t r = inst_hold_sample(&hold, esp_log_early_timestamp(),
                                                gpio_ll_get_level(&GPIO, KEY_GPIO) == 0);
        if (r != INST_HOLD_SAMPLING) return r == INST_HOLD_ENTER;
        esp_rom_delay_us(5000);
    }
}

void bootloader_hooks_include(void)
{
}

void bootloader_after_init(void)
{
    inst_state_t state;
    if (bootloader_flash_read(INST_STATE_OFFSET, &state, sizeof(state), false) == ESP_OK &&
        inst_state_requires_installer(&state)) {
        boot_installer("unfinished install");
    }
    if (!play_present()) {
        boot_installer("no application");
    }
    if (esp_rom_get_reset_reason(0) != RESET_REASON_CORE_DEEP_SLEEP && key_held()) {
        boot_installer("key held");
    }
}
