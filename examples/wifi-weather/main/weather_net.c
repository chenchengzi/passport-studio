// examples/wifi-weather/main/weather_net.c
#include "weather_net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "wx_net";

#define BIT_GOT_IP      BIT0
#define BIT_DISCONNECT  BIT1
#define HTTP_BUFFER     4096

static EventGroupHandle_t s_events;
static volatile bool s_connected;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        ESP_LOGW(TAG, "disconnected, reason %d", d->reason);
        s_connected = false;
        xEventGroupSetBits(s_events, BIT_DISCONNECT);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&e->ip_info.ip));
        s_connected = true;
        xEventGroupSetBits(s_events, BIT_GOT_IP);
    }
}

esp_err_t wx_net_init(bool nvs_ok, wx_network_t *driver)
{
    memset(driver, 0, sizeof(*driver));
    s_events = xEventGroupCreate();
    if (!s_events) return ESP_ERR_NO_MEM;

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.nvs_enable = nvs_ok;
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL), TAG, "wifi events");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL), TAG, "ip events");

    // With Flash storage the driver has just loaded nvs.net80211.
    wifi_config_t loaded = {0};
    if (nvs_ok && esp_wifi_get_config(WIFI_IF_STA, &loaded) == ESP_OK) {
        memcpy(driver->ssid, loaded.sta.ssid, sizeof(loaded.sta.ssid));
        memcpy(driver->password, loaded.sta.password, sizeof(loaded.sta.password));
        memset(&loaded, 0, sizeof(loaded));
    }
    // Keep that setting intact for the next boot: later changes stay in RAM.
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "mode");
    return esp_wifi_start();
}

bool wx_net_connect(const wx_network_t *net, int timeout_ms)
{
    wx_net_disconnect();
    wifi_config_t cfg = {0};
    memcpy(cfg.sta.ssid, net->ssid, strnlen(net->ssid, sizeof(cfg.sta.ssid)));
    memcpy(cfg.sta.password, net->password, strnlen(net->password, sizeof(cfg.sta.password)));
    cfg.sta.threshold.authmode = net->password[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    cfg.sta.pmf_cfg.capable = true;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    memset(&cfg, 0, sizeof(cfg));
    if (err != ESP_OK) return false;

    // A first attempt often fails while the AP is still waking; allow three.
    for (int attempt = 0; attempt < 3; attempt++) {
        xEventGroupClearBits(s_events, BIT_GOT_IP | BIT_DISCONNECT);
        if (esp_wifi_connect() != ESP_OK) return false;
        EventBits_t bits = xEventGroupWaitBits(s_events, BIT_GOT_IP | BIT_DISCONNECT, pdTRUE, pdFALSE,
                                               pdMS_TO_TICKS(timeout_ms));
        if (bits & BIT_GOT_IP) return true;
        if (!(bits & BIT_DISCONNECT)) break;   // timeout
    }
    wx_net_disconnect();
    return false;
}

bool wx_net_connected(void)
{
    return s_connected;
}

void wx_net_disconnect(void)
{
    if (esp_wifi_disconnect() == ESP_OK) {
        xEventGroupWaitBits(s_events, BIT_DISCONNECT, pdTRUE, pdFALSE, pdMS_TO_TICKS(500));
    }
    s_connected = false;
}

// GET url into a NUL-terminated buffer; returns the body length or -1.
static int http_get(const char *url, char *buf, int cap)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    ESP_LOGI(TAG, "heap before request: %u free, largest block %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return -1;
    int total = -1;
    if (esp_http_client_open(client, 0) == ESP_OK && esp_http_client_fetch_headers(client) >= 0) {
        total = 0;
        int n;
        while (total < cap - 1 && (n = esp_http_client_read(client, buf + total, cap - 1 - total)) > 0) {
            total += n;
        }
        int status = esp_http_client_get_status_code(client);
        if (status != 200) {
            ESP_LOGE(TAG, "%s: HTTP %d", url, status);
            total = -1;
        }
    } else {
        ESP_LOGE(TAG, "%s: request failed", url);
    }
    esp_http_client_cleanup(client);
    if (total >= 0) buf[total] = '\0';
    return total;
}

static double number(const cJSON *obj, const char *key, double fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsArray(item)) item = cJSON_GetArrayItem(item, 0);
    return cJSON_IsNumber(item) ? item->valuedouble : fallback;
}

// Coordinates may be numbers, numeric strings, or ipinfo's "lat,lon" string.
static bool coordinate(const cJSON *obj, const char *key, double *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(item)) {
        *out = item->valuedouble;
        return true;
    }
    if (cJSON_IsString(item) && item->valuestring[0]) {
        *out = strtod(item->valuestring, NULL);
        return true;
    }
    return false;
}

static bool parse_place(const cJSON *root, const char *lat_key, const char *lon_key, wx_place_t *place)
{
    const cJSON *city = cJSON_GetObjectItemCaseSensitive(root, "city");
    if (!lon_key) {   // ipinfo.io: "loc": "39.9075,116.3972"
        const cJSON *loc = cJSON_GetObjectItemCaseSensitive(root, lat_key);
        const char *comma = cJSON_IsString(loc) ? strchr(loc->valuestring, ',') : NULL;
        if (!comma) return false;
        place->lat = strtod(loc->valuestring, NULL);
        place->lon = strtod(comma + 1, NULL);
    } else if (!coordinate(root, lat_key, &place->lat) || !coordinate(root, lon_key, &place->lon)) {
        return false;
    }
    snprintf(place->city, sizeof(place->city), "%s",
             cJSON_IsString(city) && city->valuestring[0] ? city->valuestring : "Here");
    return true;
}

// Several free IP-location services, since any one may be unreachable from a
// given network (ip-api.com, for example, times out from some Chinese ISPs).
esp_err_t wx_fetch_place(wx_place_t *place)
{
    static const struct {
        const char *url, *lat, *lon;
    } providers[] = {
        {"https://ipinfo.io/json", "loc", NULL},
        {"https://get.geojs.io/v1/ip/geo.json", "latitude", "longitude"},
        {"https://ipwho.is/", "latitude", "longitude"},
    };
    static char buf[HTTP_BUFFER];
    for (size_t i = 0; i < sizeof(providers) / sizeof(providers[0]); i++) {
        if (http_get(providers[i].url, buf, sizeof(buf)) < 0) continue;
        cJSON *root = cJSON_Parse(buf);
        bool ok = root && parse_place(root, providers[i].lat, providers[i].lon, place);
        cJSON_Delete(root);
        if (ok) {
            ESP_LOGI(TAG, "location %s (%.3f, %.3f) from %s", place->city, place->lat, place->lon, providers[i].url);
            return ESP_OK;
        }
        ESP_LOGW(TAG, "%s: unexpected response", providers[i].url);
    }
    return ESP_FAIL;
}

esp_err_t wx_fetch_report(const wx_place_t *place, bool fahrenheit, wx_report_t *report)
{
    static char buf[HTTP_BUFFER];
    char url[320];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%.3f&longitude=%.3f"
             "&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m"
             "&daily=temperature_2m_max,temperature_2m_min&timezone=auto&forecast_days=1%s",
             place->lat, place->lon, fahrenheit ? "&temperature_unit=fahrenheit" : "");
    if (http_get(url, buf, sizeof(buf)) < 0) return ESP_FAIL;
    cJSON *root = cJSON_Parse(buf);
    if (!root) return ESP_ERR_INVALID_RESPONSE;
    const cJSON *current = cJSON_GetObjectItemCaseSensitive(root, "current");
    const cJSON *daily = cJSON_GetObjectItemCaseSensitive(root, "daily");
    esp_err_t err = ESP_ERR_INVALID_RESPONSE;
    if (cJSON_IsObject(current) && cJSON_IsObject(daily)) {
        report->temp = number(current, "temperature_2m", 0);
        report->humidity = (int)number(current, "relative_humidity_2m", 0);
        report->wmo = (int)number(current, "weather_code", -1);
        report->wind_kmh = number(current, "wind_speed_10m", 0);
        report->temp_max = number(daily, "temperature_2m_max", report->temp);
        report->temp_min = number(daily, "temperature_2m_min", report->temp);
        const cJSON *time = cJSON_GetObjectItemCaseSensitive(current, "time");   // "2026-11-20T19:30"
        const char *t = cJSON_IsString(time) ? strchr(time->valuestring, 'T') : NULL;
        snprintf(report->time, sizeof(report->time), "%.5s", t ? t + 1 : "--:--");
        err = ESP_OK;
    }
    cJSON_Delete(root);
    return err;
}
