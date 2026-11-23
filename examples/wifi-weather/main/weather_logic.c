// examples/wifi-weather/main/weather_logic.c
#include "weather_logic.h"

#include <string.h>

size_t wx_candidates(const wx_network_t *list, size_t list_len, const wx_network_t *driver,
                     wx_candidate_t *out, size_t max)
{
    size_t n = 0;
    for (size_t i = 0; i < list_len && n < max; i++) {
        if (!list[i].ssid[0]) continue;
        out[n].net = list[i];
        out[n].source = WX_SOURCE_LIST;
        out[n].rank = (uint8_t)(i + 1);
        n++;
    }
    if (driver && driver->ssid[0] && n < max) {
        for (size_t i = 0; i < n; i++) {
            if (!strcmp(out[i].net.ssid, driver->ssid)) return n;
        }
        out[n].net = *driver;
        out[n].source = WX_SOURCE_DRIVER;
        out[n].rank = 0;
        n++;
    }
    return n;
}

wx_kind_t wx_kind(int wmo)
{
    switch (wmo) {
    case 0: return WX_KIND_CLEAR;
    case 1: case 2: return WX_KIND_PARTLY;
    case 3: return WX_KIND_CLOUDY;
    case 45: case 48: return WX_KIND_FOG;
    case 51: case 53: case 55: case 56: case 57:
    case 61: case 63: case 65: case 66: case 67:
    case 80: case 81: case 82: return WX_KIND_RAIN;
    case 71: case 73: case 75: case 77: case 85: case 86: return WX_KIND_SNOW;
    case 95: case 96: case 99: return WX_KIND_STORM;
    default: return WX_KIND_UNKNOWN;
    }
}

const char *wx_text(int wmo)
{
    switch (wmo) {
    case 0: return "Clear sky";
    case 1: return "Mainly clear";
    case 2: return "Partly cloudy";
    case 3: return "Overcast";
    case 45: case 48: return "Fog";
    case 51: case 53: case 55: return "Drizzle";
    case 56: case 57: return "Freezing drizzle";
    case 61: return "Light rain";
    case 63: return "Rain";
    case 65: return "Heavy rain";
    case 66: case 67: return "Freezing rain";
    case 71: return "Light snow";
    case 73: return "Snow";
    case 75: return "Heavy snow";
    case 77: return "Snow grains";
    case 80: case 81: case 82: return "Rain showers";
    case 85: case 86: return "Snow showers";
    case 95: return "Thunderstorm";
    case 96: case 99: return "Storm with hail";
    default: return "Unknown";
    }
}
