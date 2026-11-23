// Host tests for examples/wifi-weather/main/weather_logic.c.
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "weather_i18n.h"
#include "weather_logic.h"

static wx_network_t net(const char *ssid, const char *pass)
{
    wx_network_t n = {0};
    strcpy(n.ssid, ssid);
    strcpy(n.password, pass);
    return n;
}

int main(void)
{
    wx_candidate_t out[WX_MAX_CANDIDATES];
    wx_network_t list[3] = {net("Home", "password1"), net("", ""), net("Lab", "")};

    // List order is kept, empty entries skipped, a new driver SSID appended.
    wx_network_t driver = net("Office", "officepass");
    size_t n = wx_candidates(list, 3, &driver, out, WX_MAX_CANDIDATES);
    assert(n == 3);
    assert(!strcmp(out[0].net.ssid, "Home") && out[0].rank == 1 && out[0].source == WX_SOURCE_LIST);
    assert(!strcmp(out[1].net.ssid, "Lab") && out[1].rank == 3);
    assert(!strcmp(out[2].net.ssid, "Office") && out[2].source == WX_SOURCE_DRIVER && out[2].rank == 0);

    // The driver setting duplicates a saved network: not tried twice.
    driver = net("Lab", "");
    assert(wx_candidates(list, 3, &driver, out, WX_MAX_CANDIDATES) == 2);

    // Only the driver setting (installer wrote NVS, app list unreadable).
    driver = net("Office", "officepass");
    n = wx_candidates(NULL, 0, &driver, out, WX_MAX_CANDIDATES);
    assert(n == 1 && out[0].source == WX_SOURCE_DRIVER);
    assert(wx_candidates(NULL, 0, NULL, out, WX_MAX_CANDIDATES) == 0);
    assert(wx_candidates(list, 3, &driver, out, 1) == 1);   // capacity respected

    assert(wx_kind(0) == WX_KIND_CLEAR && wx_kind(2) == WX_KIND_PARTLY && wx_kind(3) == WX_KIND_CLOUDY);
    assert(wx_kind(48) == WX_KIND_FOG && wx_kind(63) == WX_KIND_RAIN && wx_kind(81) == WX_KIND_RAIN);
    assert(wx_kind(75) == WX_KIND_SNOW && wx_kind(95) == WX_KIND_STORM && wx_kind(4) == WX_KIND_UNKNOWN);
    assert(!strcmp(wx_text(65), "Heavy rain") && !strcmp(wx_text(-1), "Unknown"));

    // Every string exists in both languages; Chinese weather text for all codes.
    for (int id = 0; id < WX_S_COUNT; id++) {
        assert(wx_str(WX_LANG_EN, id)[0] && wx_str(WX_LANG_ZH, id)[0]);
    }
    assert(!strcmp(wx_text_lang(WX_LANG_ZH, 63), "\u4E2D\u96E8"));
    assert(!strcmp(wx_text_lang(WX_LANG_EN, 63), "Rain"));
    assert(!strcmp(wx_text_lang(WX_LANG_ZH, 1234), "\u672A\u77E5"));

    // UTF-8 truncation never splits a character.
    char text[8];
    wx_utf8_copy(text, sizeof(text), "\u6D77\u6DC0\u533A");   // 9 bytes into 7
    assert(!strcmp(text, "\u6D77\u6DC0"));
    wx_utf8_copy(text, sizeof(text), "Haidian");
    assert(!strcmp(text, "Haidian"));
    wx_utf8_copy(text, 1, "x");
    assert(text[0] == '\0');
    puts("weather logic tests: PASS");
    return 0;
}
