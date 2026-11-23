// installer/main/installer_flash.c
#include "installer_flash.h"

#include "esp_flash.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_rom_md5.h"
#include "mbedtls/sha256.h"

#include "inst_boot.h"

static const char *TAG = "inst_flash";

static mbedtls_sha256_context s_sha;

static bool flash_erase(void *ctx, uint32_t addr, uint32_t len)
{
    (void)ctx;
    esp_err_t err = esp_flash_erase_region(NULL, addr, len);
    if (err != ESP_OK) ESP_LOGE(TAG, "erase 0x%lx+0x%lx: %s", addr, len, esp_err_to_name(err));
    return err == ESP_OK;
}

static bool flash_write(void *ctx, uint32_t addr, const void *data, uint32_t len)
{
    (void)ctx;
    esp_err_t err = esp_flash_write(NULL, data, addr, len);
    if (err != ESP_OK) ESP_LOGE(TAG, "write 0x%lx+0x%lx: %s", addr, len, esp_err_to_name(err));
    return err == ESP_OK;
}

static bool flash_read(void *ctx, uint32_t addr, void *data, uint32_t len)
{
    (void)ctx;
    return esp_flash_read(NULL, data, addr, len) == ESP_OK;
}

static void md5(const uint8_t *data, size_t len, uint8_t out[16])
{
    md5_context_t ctx;
    esp_rom_md5_init(&ctx);
    esp_rom_md5_update(&ctx, data, len);
    esp_rom_md5_final(out, &ctx);
}

static void sha256(void *ctx, int step, const void *data, size_t len, uint8_t out[32])
{
    (void)ctx;
    switch (step) {
    case INST_SHA_START:
        mbedtls_sha256_init(&s_sha);
        mbedtls_sha256_starts(&s_sha, 0);
        break;
    case INST_SHA_UPDATE:
        mbedtls_sha256_update(&s_sha, data, len);
        break;
    default:
        mbedtls_sha256_finish(&s_sha, out);
        mbedtls_sha256_free(&s_sha);
        break;
    }
}

static bool verify_app(void *ctx, uint32_t offset, uint32_t max_size)
{
    (void)ctx;
    const esp_partition_pos_t pos = {.offset = offset, .size = max_size};
    esp_image_metadata_t meta;
    esp_err_t err = esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &pos, &meta);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "application at 0x%lx failed verification: %s", offset, esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "application at 0x%lx: %lu bytes", offset, meta.image_len);
    return true;
}

static const inst_ops_t s_ops = {
    .erase = flash_erase,
    .write = flash_write,
    .read = flash_read,
    .md5 = md5,
    .sha256 = sha256,
    .verify_app = verify_app,
};

const inst_ops_t *installer_flash_ops(void)
{
    return &s_ops;
}

bool installer_flash_app_bootable(void)
{
    static uint8_t table[INST_TABLE_MAX];
    if (!flash_read(NULL, INST_TABLE_OFFSET, table, sizeof(table))) return false;
    uint32_t offset = inst_boot_app_offset(table, sizeof(table));
    if (offset == 0 || offset >= INST_PLAY_LIMIT) return false;
    return verify_app(NULL, offset, INST_PLAY_LIMIT - offset);
}
