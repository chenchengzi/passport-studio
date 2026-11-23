// installer/main/installer_wifi.h
// Saved Wi-Fi networks: the INST_WIFI_OFFSET sector, and a copy of the first
// network in the installed play's Wi-Fi driver settings. Worker task only.
#pragma once

#include <stdbool.h>

#include "inst_wifi.h"

// Read the sector; an erased or damaged sector yields an empty list.
void installer_wifi_load(inst_wifi_store_t *store);

// Seal (incrementing seq), erase, write, and read back the sector.
bool installer_wifi_save(inst_wifi_store_t *store);

// Write the first saved network into namespace "nvs.net80211" of the "nvs"
// partition named by the current partition table, so a play that uses the
// ESP-IDF default (Flash) Wi-Fi storage connects without its own provisioning.
// An empty list removes the station credentials instead. Best effort: an
// unreadable NVS partition is left untouched, never erased.
bool installer_wifi_apply(const inst_wifi_store_t *store);
