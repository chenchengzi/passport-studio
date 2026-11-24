// examples/t3-status/main/t3_net.h
// Wi-Fi station and the bridge poll. Called from the status task only.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "t3_logic.h"

// netif and Wi-Fi driver; nvs_ok says whether nvs_flash_init() succeeded.
// *driver receives the station settings the driver loaded from this app's NVS
// (what the installer injected), or an empty SSID.
esp_err_t t3_net_init(bool nvs_ok, t3_network_t *driver);

// Connect and wait for an IP address; false on timeout or rejection.
bool t3_net_connect(const t3_network_t *net, int timeout_ms);
bool t3_net_connected(void);

// GET http://host:port/status and parse it. Plain HTTP on the LAN: the reply
// carries only counts. ESP_FAIL on a network or HTTP error,
// ESP_ERR_INVALID_RESPONSE when the body is not the bridge's JSON.
esp_err_t t3_net_poll(const char *host, uint16_t port, int timeout_ms, t3_status_t *out);

// GET http://host:port/tasks: the running and waiting threads with titles.
// On success *count tasks are in out; otherwise out is unspecified.
esp_err_t t3_net_tasks(const char *host, uint16_t port, int timeout_ms, t3_task_t *out, int max, int *count);

// GET http://host:port/task/<id>: one listed task's request, latest reply, and
// background work. ESP_FAIL also when the task has just left the list (404).
esp_err_t t3_net_detail(const char *host, uint16_t port, const char *id, int timeout_ms, t3_detail_t *out);
