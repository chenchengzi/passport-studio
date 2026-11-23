// examples/wifi-weather/main/weather_net.h
// Wi-Fi station and the two HTTP lookups. Called from the weather task only.
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "weather_logic.h"

typedef struct {
    char city[48];
    double lat, lon;
} wx_place_t;

typedef struct {
    double temp, temp_max, temp_min, wind_kmh;
    int humidity, wmo;
    char time[6];          // "HH:MM", local time of the observation
} wx_report_t;

// netif and Wi-Fi driver; nvs_ok says whether nvs_flash_init() succeeded.
// *driver receives the station settings the driver loaded from this app's NVS
// (what the installer injected), or an empty SSID.
esp_err_t wx_net_init(bool nvs_ok, wx_network_t *driver);

// Connect and wait for an IP address; false on timeout or rejection.
bool wx_net_connect(const wx_network_t *net, int timeout_ms);
bool wx_net_connected(void);
void wx_net_disconnect(void);

// Location from the public IP address, then the current weather.
esp_err_t wx_fetch_place(wx_place_t *place);
esp_err_t wx_fetch_report(const wx_place_t *place, bool fahrenheit, wx_report_t *report);
