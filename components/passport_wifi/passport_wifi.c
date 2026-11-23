// components/passport_wifi/passport_wifi.c
#include "passport_wifi.h"

#include <string.h>

#include "esp_partition.h"

#include "inst_layout.h"
#include "inst_wifi.h"

_Static_assert(PASSPORT_WIFI_MAX_NETWORKS == INST_WIFI_MAX_NETWORKS, "list size mismatch");

static bool load(inst_wifi_store_t *store)
{
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)INST_WIFI_SUBTYPE, INST_WIFI_LABEL);
    return part && esp_partition_read(part, 0, store, sizeof(*store)) == ESP_OK &&
           inst_wifi_valid(store);
}

size_t passport_wifi_list(passport_wifi_network_t *out, size_t max)
{
    static inst_wifi_store_t store;   // 508 bytes; keep it off small task stacks
    if (!load(&store)) return 0;
    size_t count = store.count < max ? store.count : max;
    for (size_t i = 0; i < count; i++) {
        const inst_wifi_net_t *net = &store.nets[i];
        memset(&out[i], 0, sizeof(out[i]));
        memcpy(out[i].ssid, net->ssid, net->ssid_len);
        memcpy(out[i].password, net->pass, net->pass_len);
    }
    memset(&store, 0, sizeof(store));
    return count;
}

esp_err_t passport_wifi_sta_config(size_t index, wifi_config_t *config)
{
    passport_wifi_network_t nets[PASSPORT_WIFI_MAX_NETWORKS];
    size_t count = passport_wifi_list(nets, PASSPORT_WIFI_MAX_NETWORKS);
    esp_err_t err = ESP_ERR_NOT_FOUND;
    if (index < count) {
        memset(&config->sta, 0, sizeof(config->sta));
        memcpy(config->sta.ssid, nets[index].ssid, strlen(nets[index].ssid));
        memcpy(config->sta.password, nets[index].password, strlen(nets[index].password));
        config->sta.threshold.authmode = nets[index].password[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
        err = ESP_OK;
    }
    memset(nets, 0, sizeof(nets));
    return err;
}
