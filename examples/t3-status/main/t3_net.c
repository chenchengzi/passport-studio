// examples/t3-status/main/t3_net.c
#include "t3_net.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "t3_net";

#define BIT_GOT_IP      BIT0
#define BIT_DISCONNECT  BIT1
#define STATUS_MAX      384    // /status: about 90 bytes plus a 40-character title
#define TASKS_MAX       2560   // /tasks: 8 lines of at most ~280 bytes
#define DETAIL_MAX      1024   // /task/<id>: three lines, at most ~800 bytes

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

esp_err_t t3_net_init(bool nvs_ok, t3_network_t *driver)
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

static void disconnect(void)
{
    if (esp_wifi_disconnect() == ESP_OK) {
        xEventGroupWaitBits(s_events, BIT_DISCONNECT, pdTRUE, pdFALSE, pdMS_TO_TICKS(500));
    }
    s_connected = false;
}

bool t3_net_connect(const t3_network_t *net, int timeout_ms)
{
    disconnect();
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
    disconnect();
    return false;
}

bool t3_net_connected(void)
{
    return s_connected;
}

// GET http://host:port<path> into body (NUL-terminated). ESP_FAIL on a network
// error, an HTTP status other than 200, or a reply that does not fit.
static esp_err_t http_get(const char *host, uint16_t port, const char *path, int timeout_ms,
                          char *body, size_t cap)
{
    esp_http_client_config_t cfg = {
        .host = host,
        .port = port,
        .path = path,
        .transport_type = HTTP_TRANSPORT_OVER_TCP,
        .timeout_ms = timeout_ms,
        .buffer_size = 512,
        .buffer_size_tx = 256,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_ERR_NO_MEM;

    esp_err_t err = ESP_FAIL;
    if (esp_http_client_open(client, 0) == ESP_OK && esp_http_client_fetch_headers(client) >= 0) {
        int total = 0, n;
        while (total < (int)cap - 1 && (n = esp_http_client_read(client, body + total, cap - 1 - total)) > 0) {
            total += n;
        }
        body[total] = '\0';
        int status = esp_http_client_get_status_code(client);
        if (status != 200) {
            ESP_LOGW(TAG, "bridge %s: HTTP %d", path, status);
        } else if (total == (int)cap - 1 && !esp_http_client_is_complete_data_received(client)) {
            ESP_LOGW(TAG, "bridge %s: reply over %u bytes", path, (unsigned)cap - 1);
        } else {
            err = ESP_OK;
        }
    }
    esp_http_client_cleanup(client);
    return err;
}

esp_err_t t3_net_poll(const char *host, uint16_t port, int timeout_ms, t3_status_t *out)
{
    char body[STATUS_MAX];
    esp_err_t err = http_get(host, port, "/status", timeout_ms, body, sizeof(body));
    if (err != ESP_OK) return err;
    if (!t3_parse_status(body, out)) {
        ESP_LOGW(TAG, "bridge: unexpected /status reply");
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

esp_err_t t3_net_tasks(const char *host, uint16_t port, int timeout_ms, t3_task_t *out, int max, int *count)
{
    static char body[TASKS_MAX];   // status task only
    esp_err_t err = http_get(host, port, "/tasks", timeout_ms, body, sizeof(body));
    if (err != ESP_OK) return err;
    int n = t3_parse_tasks(body, out, max);
    if (n < 0) {
        ESP_LOGW(TAG, "bridge: unexpected /tasks reply");
        return ESP_ERR_INVALID_RESPONSE;
    }
    *count = n;
    return ESP_OK;
}

esp_err_t t3_net_detail(const char *host, uint16_t port, const char *id, int timeout_ms, t3_detail_t *out)
{
    static char body[DETAIL_MAX];   // status task only
    char path[24];
    snprintf(path, sizeof(path), "/task/%s", id);
    esp_err_t err = http_get(host, port, path, timeout_ms, body, sizeof(body));
    if (err != ESP_OK) return err;
    if (!t3_parse_detail(body, out)) {
        ESP_LOGW(TAG, "bridge: unexpected %s reply", path);
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}
