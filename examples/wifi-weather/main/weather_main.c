// examples/wifi-weather/main/weather_main.c
// Wi-Fi weather: a test application for the Bluetooth installer's Wi-Fi
// provisioning and for Passport Link. It joins the first reachable network from
//   1. the installer's saved list (components/passport_wifi), in priority order;
//   2. the station settings the installer wrote into this app's NVS;
// and shows the weather for the location pushed by the iPhone app over
// Passport Link, falling back to the public IP's location.
// Settings (units, refresh interval, language, screen timeout) come from the
// iPhone app's Device Link page. OK: refresh now; any key wakes the screen.
#include <stdio.h>
#include <string.h>

#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "passport_link.h"
#include "passport_wifi.h"

#include "weather_i18n.h"
#include "weather_logic.h"
#include "weather_net.h"
#include "weather_ui.h"

static const char *TAG = "weather";

#define CONNECT_TIMEOUT_MS  15000
#define RETRY_MS            (60 * 1000)
#define BATTERY_MS          30000
#define PLACE_NAMESPACE     "weather"

typedef enum {
    EV_KEY_PRESS,     // any key went down: wakes the screen
    EV_KEY_OK,        // OK clicked: refresh
    EV_SETTINGS,      // the phone changed a setting
    EV_LOCATION,      // the phone pushed a location (already saved to NVS)
} event_kind_t;

typedef struct {
    event_kind_t kind;
    bool ok_key;
} event_t;

static QueueHandle_t s_events;
static bool s_nvs_ok;

// Screen timeout state, weather task only.
static int64_t s_last_activity_us;
static bool s_screen_on = true;
static bool s_swallow_ok;   // the OK press that woke the screen does not refresh

static const passport_link_setting_t SETTINGS[] = {
    PASSPORT_LINK_ENUM("lang", "显示语言", "English", "English", "中文"),
    PASSPORT_LINK_ENUM("units", "温度单位", "C", "C", "F"),
    PASSPORT_LINK_INT("refresh", "刷新间隔（分钟）", 30, 5, 180),
    PASSPORT_LINK_INT("screen", "自动关屏（分钟，0 为常亮）", 0, 0, 60),
};
static const char *const CHANNELS[] = {"location", "time"};

static void post(event_kind_t kind, bool ok_key)
{
    event_t ev = {.kind = kind, .ok_key = ok_key};
    (void)xQueueSend(s_events, &ev, 0);
}

// Runs on the button driver's timer task: enqueue only.
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    if (ev == BSP_BTN_PRESS) post(EV_KEY_PRESS, btn == BSP_BTN_OK);
    else if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) post(EV_KEY_OK, true);
}

static wx_lang_t current_lang(void)
{
    char lang[16];
    passport_link_get_str("lang", lang, sizeof(lang));
    return strcmp(lang, "English") && lang[0] ? WX_LANG_ZH : WX_LANG_EN;
}

// ---------------------------------------------------------------------------
// Location pushed by the phone, kept in NVS so it survives restarts
// ---------------------------------------------------------------------------
// city receives the name for the current language (English when no Chinese
// name was pushed).
static bool load_phone_place(wx_place_t *place, wx_lang_t lang)
{
    nvs_handle_t h;
    if (!s_nvs_ok || nvs_open(PLACE_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
    int32_t lat, lon;
    size_t len = sizeof(place->city);
    bool ok = nvs_get_i32(h, "lat", &lat) == ESP_OK && nvs_get_i32(h, "lon", &lon) == ESP_OK &&
              nvs_get_str(h, "name", place->city, &len) == ESP_OK;
    len = sizeof(place->city);
    if (ok && lang == WX_LANG_ZH) (void)nvs_get_str(h, "name_zh", place->city, &len);
    nvs_close(h);
    if (ok) {
        place->lat = lat / 1e6;
        place->lon = lon / 1e6;
    }
    return ok;
}

static void save_phone_place(double lat, double lon, const char *name, const char *name_zh)
{
    nvs_handle_t h;
    if (!s_nvs_ok || nvs_open(PLACE_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    char city[sizeof(((wx_place_t *)0)->city)], city_zh[sizeof(city)];
    wx_utf8_copy(city, sizeof(city), name && name[0] ? name : "Here");
    wx_utf8_copy(city_zh, sizeof(city_zh), name_zh && name_zh[0] ? name_zh : city);
    if (nvs_set_i32(h, "lat", (int32_t)(lat * 1e6)) == ESP_OK &&
        nvs_set_i32(h, "lon", (int32_t)(lon * 1e6)) == ESP_OK &&
        nvs_set_str(h, "name", city) == ESP_OK && nvs_set_str(h, "name_zh", city_zh) == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
}

// ---------------------------------------------------------------------------
// Passport Link callbacks (component task)
// ---------------------------------------------------------------------------
static void on_push(const char *channel, const cJSON *d, void *user)
{
    (void)user;
    if (strcmp(channel, "location")) return;   // "time" is handled by the component
    const cJSON *lat = cJSON_GetObjectItemCaseSensitive(d, "lat");
    const cJSON *lon = cJSON_GetObjectItemCaseSensitive(d, "lon");
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(d, "name");
    const cJSON *name_zh = cJSON_GetObjectItemCaseSensitive(d, "name_zh");
    if (!cJSON_IsNumber(lat) || !cJSON_IsNumber(lon) || lat->valuedouble < -90 || lat->valuedouble > 90 ||
        lon->valuedouble < -180 || lon->valuedouble > 180) {
        return;
    }
    ESP_LOGI(TAG, "location from phone: %.3f, %.3f", lat->valuedouble, lon->valuedouble);
    save_phone_place(lat->valuedouble, lon->valuedouble, cJSON_IsString(name) ? name->valuestring : NULL,
                     cJSON_IsString(name_zh) ? name_zh->valuestring : NULL);
    post(EV_LOCATION, false);
}

static void on_settings(void *user)
{
    (void)user;
    post(EV_SETTINGS, false);
}

static void on_passkey(uint32_t passkey, void *user)
{
    (void)user;
    wx_ui_passkey(passkey);
}

static void on_connection(bool connected, void *user)
{
    (void)user;
    wx_ui_link(connected);
}

// ---------------------------------------------------------------------------
// Weather task
// ---------------------------------------------------------------------------
static void screen_wake(void)
{
    s_last_activity_us = esp_timer_get_time();
    if (!s_screen_on) {
        wx_ui_screen(true);
        s_screen_on = true;
    }
}

static void screen_check(void)
{
    int32_t minutes = passport_link_get_int("screen");   // 0: always on
    if (minutes > 0 && s_screen_on &&
        esp_timer_get_time() - s_last_activity_us >= (int64_t)minutes * 60 * 1000000) {
        wx_ui_screen(false);
        s_screen_on = false;
    }
}

// Wait until the timeout or an event that needs a refresh, handling the
// screen timeout and battery reading meanwhile.
static void wait_event(int64_t timeout_ms, bool battery, bool *location_changed)
{
    int64_t deadline = esp_timer_get_time() + timeout_ms * 1000;
    int64_t next_battery = esp_timer_get_time() + (int64_t)BATTERY_MS * 1000;
    for (;;) {
        int64_t now = esp_timer_get_time();
        if (now >= deadline) return;
        int64_t step = deadline - now < 1000000 ? deadline - now : 1000000;
        event_t ev;
        if (xQueueReceive(s_events, &ev, pdMS_TO_TICKS(step / 1000 + 1)) == pdTRUE) {
            switch (ev.kind) {
            case EV_KEY_PRESS: {
                bool was_off = !s_screen_on;
                screen_wake();
                if (was_off && ev.ok_key) s_swallow_ok = true;
                break;
            }
            case EV_KEY_OK:
                if (s_swallow_ok) {
                    s_swallow_ok = false;
                    break;
                }
                screen_wake();
                return;
            case EV_LOCATION:
                *location_changed = true;
                screen_wake();
                return;
            case EV_SETTINGS:
                wx_ui_set_lang(current_lang());
                screen_wake();
                return;
            }
        }
        screen_check();
        if (battery && esp_timer_get_time() >= next_battery) {
            wx_ui_battery(bsp_battery_soc());
            next_battery = esp_timer_get_time() + (int64_t)BATTERY_MS * 1000;
        }
    }
}

static void describe(wx_lang_t lang, const wx_candidate_t *c, char *out, size_t cap)
{
    char where[48];
    if (c->source == WX_SOURCE_LIST) snprintf(where, sizeof(where), wx_str(lang, WX_S_FROM_LIST), c->rank);
    else snprintf(where, sizeof(where), "%s", wx_str(lang, WX_S_FROM_NVS));
    snprintf(out, cap, "%s%s", c->net.ssid, where);
}

// Try every candidate in order; returns the index joined or -1.
static int join(wx_lang_t lang, const wx_candidate_t *cands, size_t n)
{
    char line[96], title[48];
    for (size_t i = 0; i < n; i++) {
        describe(lang, &cands[i], line, sizeof(line));
        snprintf(title, sizeof(title), wx_str(lang, WX_S_CONNECTING), (unsigned)(i + 1), (unsigned)n);
        wx_ui_status(LV_SYMBOL_WIFI, title, line, "");
        ESP_LOGI(TAG, "trying %s", line);
        if (wx_net_connect(&cands[i].net, CONNECT_TIMEOUT_MS)) return (int)i;
        ESP_LOGW(TAG, "could not join %s", cands[i].net.ssid);
    }
    return -1;
}

static void weather_task(void *arg)
{
    (void)arg;
    static wx_network_t driver;
    static passport_wifi_network_t saved[PASSPORT_WIFI_MAX_NETWORKS];
    static wx_network_t list[PASSPORT_WIFI_MAX_NETWORKS];
    static wx_candidate_t cands[WX_MAX_CANDIDATES];
    bool battery = bsp_battery_init() == ESP_OK;
    wx_ui_battery(battery ? bsp_battery_soc() : -1);

    wx_lang_t lang = current_lang();
    wx_ui_set_lang(lang);
    s_last_activity_us = esp_timer_get_time();
    wx_ui_status(LV_SYMBOL_WIFI, wx_str(lang, WX_S_STARTING), "", "");
    esp_err_t err = wx_net_init(s_nvs_ok, &driver);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi init failed: %s", esp_err_to_name(err));
        wx_ui_status(LV_SYMBOL_WARNING, wx_str(lang, WX_S_WIFI_FAILED), esp_err_to_name(err),
                     wx_str(lang, WX_S_RESTART));
        vTaskDelete(NULL);
        return;
    }

    wx_place_t place = {0};
    bool have_place = load_phone_place(&place, lang), from_phone = have_place;
    int joined = -1;
    size_t n = 0;
    char footer[64];

    for (;;) {
        lang = current_lang();
        if (!wx_net_connected()) {
            size_t count = passport_wifi_list(saved, PASSPORT_WIFI_MAX_NETWORKS);
            for (size_t i = 0; i < count; i++) {
                memcpy(list[i].ssid, saved[i].ssid, sizeof(list[i].ssid));
                memcpy(list[i].password, saved[i].password, sizeof(list[i].password));
            }
            memset(saved, 0, sizeof(saved));
            n = wx_candidates(list, count, &driver, cands, WX_MAX_CANDIDATES);
            memset(list, 0, sizeof(list));
            ESP_LOGI(TAG, "%u saved network(s), driver setting %s", (unsigned)count,
                     driver.ssid[0] ? driver.ssid : "(none)");
            if (n == 0) {
                wx_ui_status(LV_SYMBOL_WIFI, wx_str(lang, WX_S_NO_WIFI), wx_str(lang, WX_S_NO_WIFI_HINT),
                             wx_str(lang, WX_S_HOLD_UP));
                wait_event(RETRY_MS, battery, &from_phone);
                continue;
            }
            joined = join(lang, cands, n);
            if (joined < 0) {
                snprintf(footer, sizeof(footer), wx_str(lang, WX_S_TRIED), (unsigned)n);
                wx_ui_status(LV_SYMBOL_WARNING, wx_str(lang, WX_S_CANT_CONNECT),
                             wx_str(lang, WX_S_CANT_CONNECT_HINT), footer);
                wait_event(RETRY_MS, battery, &from_phone);
                continue;
            }
        }

        // A pushed location replaces the IP-based guess for good.
        bool location_changed = false;
        if (from_phone && load_phone_place(&place, lang)) have_place = true;
        wx_ui_status(LV_SYMBOL_REFRESH, wx_str(lang, WX_S_UPDATING), cands[joined].net.ssid, "");
        if (!have_place) have_place = wx_fetch_place(&place) == ESP_OK;

        char units[4];
        passport_link_get_str("units", units, sizeof(units));
        bool fahrenheit = units[0] == 'F';
        wx_report_t report;
        if (!have_place || wx_fetch_report(&place, fahrenheit, &report) != ESP_OK) {
            wx_ui_status(LV_SYMBOL_WARNING, wx_str(lang, have_place ? WX_S_WEATHER_FAILED : WX_S_PLACE_FAILED),
                         wx_str(lang, WX_S_REQUEST_FAILED), cands[joined].net.ssid);
            wait_event(RETRY_MS, battery, &location_changed);
            if (location_changed) from_phone = true;
            continue;
        }
        ESP_LOGI(TAG, "%s: %.1f, code %d", place.city, report.temp, report.wmo);
        // Montserrat has U+2022 but not U+00B7.
        if (cands[joined].source == WX_SOURCE_LIST) {
            snprintf(footer, sizeof(footer), "%s #%u  \xE2\x80\xA2  %s", cands[joined].net.ssid,
                     cands[joined].rank, report.time);
        } else {
            snprintf(footer, sizeof(footer), "%s (NVS)  \xE2\x80\xA2  %s", cands[joined].net.ssid, report.time);
        }
        wx_ui_report(place.city, &report, fahrenheit, footer);
        int32_t refresh_min = passport_link_get_int("refresh");   // 0 if the link did not start
        int64_t refresh_ms = (int64_t)(refresh_min >= 5 ? refresh_min : 30) * 60 * 1000;
        wait_event(refresh_ms, battery, &location_changed);
        if (location_changed) from_phone = true;
    }
}

void app_main(void)
{
    s_events = xQueueCreate(8, sizeof(event_t));
    if (!s_events || bsp_display_init() != ESP_OK || !bsp_lvgl_init() || !wx_ui_init()) {
        ESP_LOGE(TAG, "display unavailable");
        return;
    }
    bsp_display_backlight(80);
    if (bsp_button_init(on_key, NULL) != ESP_OK) ESP_LOGW(TAG, "buttons unavailable");

    // Never erase NVS here: it holds the Wi-Fi settings the installer wrote.
    esp_err_t err = nvs_flash_init();
    s_nvs_ok = err == ESP_OK;
    if (!s_nvs_ok) ESP_LOGE(TAG, "NVS unavailable (%s); Wi-Fi list only, no phone link", esp_err_to_name(err));

    static const passport_link_config_t link = {
        .app_id = "wifi-weather",
        .display_name = "Passport Weather",
        .channels = CHANNELS,
        .channel_count = sizeof(CHANNELS) / sizeof(CHANNELS[0]),
        .settings = SETTINGS,
        .setting_count = sizeof(SETTINGS) / sizeof(SETTINGS[0]),
        .on_push = on_push,
        .on_settings = on_settings,
        .on_passkey = on_passkey,
        .on_connection = on_connection,
    };
    if (s_nvs_ok) {
        err = passport_link_start(&link);
        if (err != ESP_OK) ESP_LOGE(TAG, "Passport Link failed: %s", esp_err_to_name(err));
    }

    // TLS and JSON need more stack than app_main has.
    if (xTaskCreate(weather_task, "weather", 8192, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "weather task failed");
    }
}
