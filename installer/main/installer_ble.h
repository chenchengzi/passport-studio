// installer/main/installer_ble.h
// NimBLE peripheral exposing the installer service (see inst_proto.h).
// Callbacks run on the NimBLE host task: they must only copy or enqueue.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "inst_proto.h"

typedef struct {
    // CONTROL and DATA writes from an encrypted link, opcode first. Return
    // false when the packet could not be queued: CONTROL writes then fail
    // visibly, while lost DATA is recovered through the sequence check.
    bool (*on_packet)(const uint8_t *data, size_t len);
    void (*on_connection)(bool connected);
    void (*on_passkey)(uint32_t passkey);
    void (*on_secured)(bool ok);
    // Fill the INFO characteristic; returns the length written.
    size_t (*read_info)(uint8_t *out, size_t cap);
    // Fill the WIFI characteristic; returns the length written.
    size_t (*read_wifi)(uint8_t *out, size_t cap);
} installer_ble_callbacks_t;

esp_err_t installer_ble_start(const char *device_name, const installer_ble_callbacks_t *callbacks);

// Store the latest STATUS value and notify it when the client subscribed.
// Safe to call from any task.
void installer_ble_set_status(const uint8_t status[INST_STATUS_LEN]);
