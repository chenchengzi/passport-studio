// examples/wifi-weather/main/weather_ui.h
// One screen, two modes: a status card (connecting, errors) and the weather
// report. Callable from any task; every function takes the LVGL lock.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "weather_i18n.h"
#include "weather_net.h"

bool wx_ui_init(void);
void wx_ui_status(const char *symbol, const char *title, const char *detail, const char *footer);
void wx_ui_report(const char *place, const wx_report_t *report, bool fahrenheit, const char *footer);
void wx_ui_set_lang(wx_lang_t lang);    // fonts and fixed text; call before status/report
void wx_ui_screen(bool on);             // backlight
void wx_ui_passkey(uint32_t passkey);   // 0 hides the pairing card
void wx_ui_link(bool connected);        // phone indicator in the header
void wx_ui_battery(int percent);   // < 0 hides it
