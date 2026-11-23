// components/passport_wifi/include/passport_wifi.h
// Wi-Fi networks saved from the iPhone app through the resident Bluetooth
// installer. Any application installed by the installer can read them; the
// list lives in the "inst_wifi" partition that the installer appends to every
// partition table. See docs/development/engineering/bluetooth-installer.md.
#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "esp_wifi_types.h"

#define PASSPORT_WIFI_MAX_NETWORKS 5

typedef struct {
    char ssid[33];       // NUL-terminated
    char password[65];   // NUL-terminated; empty for an open network
} passport_wifi_network_t;

// Copy up to max saved networks, highest priority first. Returns how many were
// copied: 0 when the device has no installer, no list, or a damaged list.
size_t passport_wifi_list(passport_wifi_network_t *out, size_t max);

// Fill the station part of a wifi_config_t from network `index` of the list.
// Returns ESP_ERR_NOT_FOUND when that entry does not exist.
esp_err_t passport_wifi_sta_config(size_t index, wifi_config_t *config);
