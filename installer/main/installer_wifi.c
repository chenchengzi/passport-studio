// installer/main/installer_wifi.c
#include "installer_wifi.h"

#include <string.h>

#include "esp_flash.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "inst_boot.h"
#include "inst_layout.h"

static const char *TAG = "inst_wifi";

#define WIFI_NAMESPACE  "nvs.net80211"
#define NVS_SUBTYPE     0x02

void installer_wifi_load(inst_wifi_store_t *store)
{
    if (esp_flash_read(NULL, store, INST_WIFI_OFFSET, sizeof(*store)) != ESP_OK ||
        !inst_wifi_valid(store)) {
        memset(store, 0, sizeof(*store));
    }
}

bool installer_wifi_save(inst_wifi_store_t *store)
{
    static inst_wifi_store_t check;
    store->seq++;
    inst_wifi_seal(store);
    esp_err_t err = esp_flash_erase_region(NULL, INST_WIFI_OFFSET, INST_WIFI_SIZE);
    if (err == ESP_OK) err = esp_flash_write(NULL, store, INST_WIFI_OFFSET, sizeof(*store));
    if (err == ESP_OK) err = esp_flash_read(NULL, &check, INST_WIFI_OFFSET, sizeof(check));
    if (err != ESP_OK || memcmp(&check, store, sizeof(check))) {
        ESP_LOGE(TAG, "saving networks failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "saved %u network(s)", store->count);
    return true;
}

// The play's NVS partition from the table on Flash. The table may have been
// rewritten since boot, so the cached esp_partition list cannot be used.
static bool find_nvs(esp_partition_t *part)
{
    static uint8_t table[INST_TABLE_MAX];
    if (esp_flash_read(NULL, table, INST_TABLE_OFFSET, sizeof(table)) != ESP_OK) return false;
    for (uint32_t at = 0; at + INST_ENTRY_SIZE <= sizeof(table); at += INST_ENTRY_SIZE) {
        const uint8_t *e = table + at;
        if (e[0] != INST_ENTRY_MAGIC_0 || e[1] != INST_ENTRY_MAGIC_1) break;
        if (e[2] != ESP_PARTITION_TYPE_DATA || e[3] != NVS_SUBTYPE) continue;
        if (strncmp((const char *)e + 12, "nvs", 16)) continue;
        uint32_t offset = inst_le32(e + 4), size = inst_le32(e + 8);
        if (offset < INST_PLAY_START || size > INST_PLAY_LIMIT - offset || (inst_le32(e + 28) & 1)) {
            return false;   // out of the play area, or encrypted
        }
        *part = (esp_partition_t){
            .flash_chip = esp_flash_default_chip,
            .type = ESP_PARTITION_TYPE_DATA,
            .subtype = ESP_PARTITION_SUBTYPE_DATA_NVS,
            .address = offset,
            .size = size,
            .erase_size = 0x1000,
            .label = "nvs",
        };
        return true;
    }
    return false;
}

static void erase_key(nvs_handle_t h, const char *key)
{
    esp_err_t err = nvs_erase_key(h, key);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) ESP_LOGW(TAG, "erase %s: %s", key, esp_err_to_name(err));
}

bool installer_wifi_apply(const inst_wifi_store_t *store)
{
    static esp_partition_t part;   // NVS keeps the pointer until deinit
    if (!find_nvs(&part)) {
        ESP_LOGW(TAG, "no usable nvs partition; Wi-Fi settings not applied");
        return false;
    }
    esp_err_t err = nvs_flash_init_partition_ptr(&part);
    if (err != ESP_OK) {
        // NO_FREE_PAGES / NEW_VERSION_FOUND: the play owns that recovery.
        ESP_LOGW(TAG, "nvs at 0x%lx unusable (%s); not applied", part.address, esp_err_to_name(err));
        return false;
    }
    nvs_handle_t h;
    err = nvs_open_from_partition(part.label, WIFI_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        if (store->count > 0) {
            uint8_t ssid[INST_WIFI_NVS_SSID_LEN], pass[INST_WIFI_NVS_PASS_LEN];
            inst_wifi_nvs_values(&store->nets[0], ssid, pass);
            err = nvs_set_blob(h, "sta.ssid", ssid, sizeof(ssid));
            if (err == ESP_OK) err = nvs_set_blob(h, "sta.pswd", pass, sizeof(pass));
            memset(pass, 0, sizeof(pass));
        } else {
            erase_key(h, "sta.ssid");
            erase_key(h, "sta.pswd");
        }
        // Cached key material and a pinned BSSID belong to the previous network.
        erase_key(h, "sta.pmk");
        erase_key(h, "sta.apinfo");
        erase_key(h, "bssid.set");
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    nvs_flash_deinit_partition(part.label);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "writing Wi-Fi settings failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "Wi-Fi settings %s in the play's nvs", store->count ? "applied" : "cleared");
    return true;
}
