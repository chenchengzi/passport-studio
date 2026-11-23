// examples/wifi-weather/main/weather_i18n.h
// Screen text in English and Chinese. Pure C; tests/test_weather_logic.c.
// fonts/generate_fonts.sh takes every non-ASCII character of weather_i18n.c
// into the 14 px and 20 px Chinese fonts, so add Chinese text here only.
#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef enum { WX_LANG_EN = 0, WX_LANG_ZH } wx_lang_t;

typedef enum {
    WX_S_HEADER = 0,
    WX_S_STARTING,
    WX_S_WIFI_FAILED,
    WX_S_RESTART,
    WX_S_NO_WIFI,
    WX_S_NO_WIFI_HINT,
    WX_S_HOLD_UP,
    WX_S_CONNECTING,       // "%u/%u"
    WX_S_FROM_LIST,        // " (app list #%u)"
    WX_S_FROM_NVS,
    WX_S_CANT_CONNECT,
    WX_S_CANT_CONNECT_HINT,
    WX_S_TRIED,            // "%u"
    WX_S_UPDATING,
    WX_S_WEATHER_FAILED,
    WX_S_PLACE_FAILED,
    WX_S_REQUEST_FAILED,
    WX_S_DETAIL,           // high, low, humidity, wind
    WX_S_PAIR_TITLE,
    WX_S_PAIR_HINT,
    WX_S_COUNT,
} wx_str_t;

const char *wx_str(wx_lang_t lang, wx_str_t id);
const char *wx_text_lang(wx_lang_t lang, int wmo);

// Copy at most cap - 1 bytes of UTF-8 without splitting a character.
void wx_utf8_copy(char *out, size_t cap, const char *in);
