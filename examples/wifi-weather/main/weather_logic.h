// examples/wifi-weather/main/weather_logic.h
// Pure logic for the Wi-Fi weather example: connection candidates and WMO
// weather codes. No ESP-IDF headers; covered by tests/test_weather_logic.c.
#pragma once

#include <stddef.h>
#include <stdint.h>

#define WX_MAX_CANDIDATES 6   // five saved networks plus the driver's own setting

typedef enum {
    WX_SOURCE_LIST = 0,   // the installer's saved list (passport_wifi)
    WX_SOURCE_DRIVER,     // ESP-IDF Wi-Fi settings in this app's NVS
} wx_source_t;

typedef struct {
    char ssid[33];
    char password[65];
} wx_network_t;

typedef struct {
    wx_network_t net;
    wx_source_t source;
    uint8_t rank;          // 1-based position in the saved list; 0 for the driver setting
} wx_candidate_t;

// Saved networks first, in priority order, then the driver's setting when it
// names a network the list does not contain. Empty SSIDs are skipped.
size_t wx_candidates(const wx_network_t *list, size_t list_len, const wx_network_t *driver,
                     wx_candidate_t *out, size_t max);

typedef enum {
    WX_KIND_CLEAR = 0,
    WX_KIND_PARTLY,
    WX_KIND_CLOUDY,
    WX_KIND_FOG,
    WX_KIND_RAIN,
    WX_KIND_SNOW,
    WX_KIND_STORM,
    WX_KIND_UNKNOWN,
} wx_kind_t;

// WMO weather interpretation codes as used by Open-Meteo.
wx_kind_t wx_kind(int wmo);
const char *wx_text(int wmo);
