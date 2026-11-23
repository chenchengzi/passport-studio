// components/passport_link/passport_link.c
// NimBLE peripheral for Passport Link. The NimBLE host task only copies frames
// and events into a message buffer; the link task reassembles messages,
// validates and stores settings, and runs the application's callbacks.
#include "passport_link.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/message_buffer.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "passport_link";

// Declared without a public header in ESP-IDF 5.5's NimBLE port.
void ble_store_config_init(void);

#define NVS_NAMESPACE   "plink"
#define NVS_KEY_ADDR    "_addr"   // leading underscore: never a setting key
#define NVS_KEY_TZ      "_tz"
#define RX_BUFFER       (LINK_MSG_MAX + 1024)
#define EVENT_MARK      0xE0      // first byte of an internal event; not a valid frame flag
#define TASK_STACK      6144
#define NOTIFY_RETRIES  20

typedef enum { EV_CONNECTED, EV_DISCONNECTED, EV_PASSKEY, EV_SECURED } event_kind_t;

// UUIDs in NimBLE's little-endian order; byte 12 selects the characteristic.
#define LINK_UUID(n) BLE_UUID128_INIT(0x8A, 0x91, 0xF6, 0xE0, 0x8D, 0x26, 0x0F, 0x89, \
                                      0xC6, 0x47, 0x5A, 0xDD, (n), 0x7C, 0xDE, 0xCD)
static const ble_uuid128_t UUID_SERVICE = LINK_UUID(0x00);
static const ble_uuid128_t UUID_INFO = LINK_UUID(0x01);
static const ble_uuid128_t UUID_RX = LINK_UUID(0x02);
static const ble_uuid128_t UUID_TX = LINK_UUID(0x03);

static const passport_link_config_t *s_cfg;
static nvs_handle_t s_nvs;
static char s_info[LINK_INFO_MAX + 1];
static size_t s_info_len;
static MessageBufferHandle_t s_rx;
static SemaphoreHandle_t s_tx_lock;
static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static volatile uint16_t s_mtu = BLE_ATT_MTU_DFLT;
static volatile bool s_secured;
static volatile bool s_subscribed;
static uint16_t s_tx_handle;
static int32_t s_tz_min;

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------
static const passport_link_setting_t *setting(const char *key)
{
    if (!s_cfg) return NULL;   // not started: callers fall back to 0 / ""
    int i = link_setting_find(s_cfg->settings, s_cfg->setting_count, key);
    return i < 0 ? NULL : &s_cfg->settings[i];
}

int32_t passport_link_get_int(const char *key)
{
    const passport_link_setting_t *s = setting(key);
    if (!s || (s->type != PASSPORT_LINK_INT && s->type != PASSPORT_LINK_BOOL)) return 0;
    int32_t v;
    if (nvs_get_i32(s_nvs, key, &v) == ESP_OK && link_setting_int_ok(s, v)) return v;
    return s->def_int;
}

bool passport_link_get_bool(const char *key)
{
    return passport_link_get_int(key) != 0;
}

void passport_link_get_str(const char *key, char *out, size_t cap)
{
    if (cap == 0) return;
    out[0] = '\0';
    const passport_link_setting_t *s = setting(key);
    if (!s || (s->type != PASSPORT_LINK_ENUM && s->type != PASSPORT_LINK_TEXT)) return;
    char value[LINK_INFO_MAX];
    size_t len = sizeof(value);
    const char *src = s->def_str ? s->def_str : "";
    if (nvs_get_str(s_nvs, key, value, &len) == ESP_OK && link_setting_str_ok(s, value)) src = value;
    snprintf(out, cap, "%s", src);
}

int32_t passport_link_tz_offset_min(void)
{
    return s_tz_min;
}

static cJSON *settings_json(void)
{
    cJSON *v = cJSON_CreateObject();
    char text[LINK_INFO_MAX];
    for (size_t i = 0; i < s_cfg->setting_count; i++) {
        const passport_link_setting_t *s = &s_cfg->settings[i];
        switch (s->type) {
        case PASSPORT_LINK_BOOL:
            cJSON_AddBoolToObject(v, s->key, passport_link_get_bool(s->key));
            break;
        case PASSPORT_LINK_INT:
            cJSON_AddNumberToObject(v, s->key, passport_link_get_int(s->key));
            break;
        default:
            passport_link_get_str(s->key, text, sizeof(text));
            cJSON_AddStringToObject(v, s->key, text);
            break;
        }
    }
    return v;
}

// ---------------------------------------------------------------------------
// Sending
// ---------------------------------------------------------------------------
static esp_err_t send_json(cJSON *msg)
{
    char *text = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);
    if (!text) return ESP_ERR_NO_MEM;
    esp_err_t err = ESP_OK;
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    uint16_t conn = s_conn;
    if (conn == BLE_HS_CONN_HANDLE_NONE || !s_secured || !s_subscribed) {
        err = ESP_ERR_INVALID_STATE;
    } else {
        size_t len = strlen(text), offset = 0, n;
        size_t max_frame = s_mtu > 3 ? s_mtu - 3 : 20;
        uint8_t frame[BLE_ATT_ATTR_MAX_LEN];
        if (max_frame > sizeof(frame)) max_frame = sizeof(frame);
        while (err == ESP_OK && (n = link_tx_next((const uint8_t *)text, len, &offset, max_frame, frame)) > 0) {
            int rc = BLE_HS_ENOMEM;
            for (int i = 0; i < NOTIFY_RETRIES && rc == BLE_HS_ENOMEM; i++) {
                struct os_mbuf *om = ble_hs_mbuf_from_flat(frame, n);
                rc = om ? ble_gatts_notify_custom(conn, s_tx_handle, om) : BLE_HS_ENOMEM;
                if (rc == BLE_HS_ENOMEM) vTaskDelay(pdMS_TO_TICKS(20));
            }
            if (rc != 0) err = ESP_FAIL;
        }
    }
    xSemaphoreGive(s_tx_lock);
    cJSON_free(text);
    return err;
}

static void send_settings(void)
{
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "t", "settings");
    cJSON_AddItemToObject(msg, "v", settings_json());
    send_json(msg);
}

static void send_error(const char *why, const char *key)
{
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "t", "err");
    cJSON_AddStringToObject(msg, "why", why);
    if (key) cJSON_AddStringToObject(msg, "k", key);
    send_json(msg);
}

esp_err_t passport_link_request(const char *channel)
{
    if (!s_cfg) return ESP_ERR_INVALID_STATE;
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "t", "req");
    cJSON_AddStringToObject(msg, "ch", channel);
    return send_json(msg);
}

bool passport_link_connected(void)
{
    return s_conn != BLE_HS_CONN_HANDLE_NONE && s_secured;
}

// ---------------------------------------------------------------------------
// Incoming messages (link task)
// ---------------------------------------------------------------------------
static bool value_ok(const passport_link_setting_t *s, const cJSON *v)
{
    switch (s->type) {
    case PASSPORT_LINK_BOOL:
        return cJSON_IsBool(v);
    case PASSPORT_LINK_INT:
        return cJSON_IsNumber(v) && v->valuedouble == (double)(int32_t)v->valuedouble &&
               link_setting_int_ok(s, (int32_t)v->valuedouble);
    default:
        return cJSON_IsString(v) && link_setting_str_ok(s, v->valuestring);
    }
}

static void handle_set(const cJSON *values)
{
    if (!cJSON_IsObject(values)) {
        send_error("bad set", NULL);
        return;
    }
    // Validate everything first: a rejected set changes nothing.
    const cJSON *v;
    cJSON_ArrayForEach(v, values) {
        const passport_link_setting_t *s = setting(v->string);
        if (!s) {
            send_error("unknown setting", v->string);
            return;
        }
        if (!value_ok(s, v)) {
            send_error("invalid value", v->string);
            return;
        }
    }
    esp_err_t err = ESP_OK;
    cJSON_ArrayForEach(v, values) {
        const passport_link_setting_t *s = setting(v->string);
        if (s->type == PASSPORT_LINK_BOOL) err = nvs_set_i32(s_nvs, s->key, cJSON_IsTrue(v));
        else if (s->type == PASSPORT_LINK_INT) err = nvs_set_i32(s_nvs, s->key, (int32_t)v->valuedouble);
        else err = nvs_set_str(s_nvs, s->key, v->valuestring);
        if (err != ESP_OK) break;
    }
    if (err == ESP_OK) err = nvs_commit(s_nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "saving settings failed: %s", esp_err_to_name(err));
        send_error("storage", NULL);
        return;
    }
    send_settings();
    if (s_cfg->on_settings) s_cfg->on_settings(s_cfg->user);
}

static void apply_tz(void)
{
    // POSIX TZ counts hours west of UTC, so the sign is reversed.
    int32_t m = s_tz_min < 0 ? -s_tz_min : s_tz_min;
    char tz[24];
    snprintf(tz, sizeof(tz), "UTC%c%02ld:%02ld", s_tz_min >= 0 ? '-' : '+', (long)(m / 60), (long)(m % 60));
    setenv("TZ", tz, 1);
    tzset();
}

static void handle_time(const cJSON *d)
{
    const cJSON *unix_time = cJSON_GetObjectItemCaseSensitive(d, "unix");
    const cJSON *tz = cJSON_GetObjectItemCaseSensitive(d, "tz");
    if (!cJSON_IsNumber(unix_time) || unix_time->valuedouble < 1.6e9) return;
    struct timeval tv = {.tv_sec = (time_t)unix_time->valuedouble};
    settimeofday(&tv, NULL);
    if (cJSON_IsNumber(tz) && tz->valuedouble >= -720 && tz->valuedouble <= 840) {
        s_tz_min = (int32_t)tz->valuedouble;
        nvs_set_i32(s_nvs, NVS_KEY_TZ, s_tz_min);
        nvs_commit(s_nvs);
        apply_tz();
    }
}

static bool declared(const char *channel)
{
    for (size_t i = 0; i < s_cfg->channel_count; i++) {
        if (!strcmp(s_cfg->channels[i], channel)) return true;
    }
    return false;
}

static void handle_message(const char *text)
{
    cJSON *msg = cJSON_Parse(text);
    const cJSON *t = msg ? cJSON_GetObjectItemCaseSensitive(msg, "t") : NULL;
    if (!cJSON_IsString(t)) {
        send_error("bad message", NULL);
        cJSON_Delete(msg);
        return;
    }
    if (!strcmp(t->valuestring, "get")) {
        send_settings();
    } else if (!strcmp(t->valuestring, "set")) {
        handle_set(cJSON_GetObjectItemCaseSensitive(msg, "v"));
    } else if (!strcmp(t->valuestring, "push")) {
        const cJSON *ch = cJSON_GetObjectItemCaseSensitive(msg, "ch");
        const cJSON *d = cJSON_GetObjectItemCaseSensitive(msg, "d");
        if (!cJSON_IsString(ch) || !d || !declared(ch->valuestring)) {
            send_error("unknown channel", cJSON_IsString(ch) ? ch->valuestring : NULL);
        } else {
            if (!strcmp(ch->valuestring, "time")) handle_time(d);
            if (s_cfg->on_push) s_cfg->on_push(ch->valuestring, d, s_cfg->user);
        }
    } else {
        send_error("unknown type", NULL);
    }
    cJSON_Delete(msg);
}

static void handle_event(event_kind_t kind, uint32_t value)
{
    switch (kind) {
    case EV_PASSKEY:
        if (s_cfg->on_passkey) s_cfg->on_passkey(value, s_cfg->user);
        break;
    case EV_SECURED:
        if (s_cfg->on_passkey) s_cfg->on_passkey(0, s_cfg->user);
        if (value && s_cfg->on_connection) s_cfg->on_connection(true, s_cfg->user);
        break;
    case EV_DISCONNECTED:
        if (s_cfg->on_passkey) s_cfg->on_passkey(0, s_cfg->user);
        if (s_cfg->on_connection) s_cfg->on_connection(false, s_cfg->user);
        break;
    case EV_CONNECTED:
        break;
    }
}

static void link_task(void *arg)
{
    (void)arg;
    static uint8_t packet[BLE_ATT_ATTR_MAX_LEN + 1];
    static uint8_t message[LINK_MSG_MAX + 1];
    link_rx_t rx;
    link_rx_init(&rx, message, sizeof(message));
    for (;;) {
        size_t len = xMessageBufferReceive(s_rx, packet, sizeof(packet), portMAX_DELAY);
        if (len == 0) continue;
        if (packet[0] == EVENT_MARK && len == 6) {
            uint32_t value;
            memcpy(&value, packet + 2, sizeof(value));
            if (packet[1] == EV_DISCONNECTED) link_rx_init(&rx, message, sizeof(message));
            handle_event((event_kind_t)packet[1], value);
            continue;
        }
        link_rx_result_t r = link_rx_feed(&rx, packet, len);
        if (r == LINK_RX_COMPLETE) handle_message((const char *)message);
        else if (r == LINK_RX_DROPPED) ESP_LOGW(TAG, "dropped a malformed or oversized message");
    }
}

// ---------------------------------------------------------------------------
// NimBLE host task: copy and return
// ---------------------------------------------------------------------------
static void post_event(event_kind_t kind, uint32_t value)
{
    uint8_t ev[6] = {EVENT_MARK, (uint8_t)kind};
    memcpy(ev + 2, &value, sizeof(value));
    (void)xMessageBufferSend(s_rx, ev, sizeof(ev), 0);
}

static int access_info(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn; (void)attr; (void)arg;
    return os_mbuf_append(ctxt->om, s_info, s_info_len) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int access_rx(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn; (void)attr; (void)arg;
    static uint8_t buf[BLE_ATT_ATTR_MAX_LEN];   // host task only
    uint16_t len = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len) != 0 || len == 0 || buf[0] == EVENT_MARK) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    return xMessageBufferSend(s_rx, buf, len, 0) == len ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int access_tx(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn; (void)attr; (void)ctxt; (void)arg;
    return BLE_ATT_ERR_READ_NOT_PERMITTED;
}

#define SECURE_READ  (BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_READ_AUTHEN)
#define SECURE_WRITE (BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | \
                      BLE_GATT_CHR_F_WRITE_ENC | BLE_GATT_CHR_F_WRITE_AUTHEN)

static const struct ble_gatt_svc_def SERVICES[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &UUID_SERVICE.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = &UUID_INFO.u, .access_cb = access_info, .flags = SECURE_READ },
            { .uuid = &UUID_RX.u, .access_cb = access_rx, .flags = SECURE_WRITE },
            { .uuid = &UUID_TX.u, .access_cb = access_tx, .val_handle = &s_tx_handle,
              .flags = BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_READ_AUTHEN },
            { 0 },
        },
    },
    { 0 },
};

static int gap_event(struct ble_gap_event *event, void *arg);

static void advertise(void)
{
    struct ble_hs_adv_fields adv = {0};
    adv.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    adv.uuids128 = &UUID_SERVICE;
    adv.num_uuids128 = 1;
    adv.uuids128_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&adv);
    if (rc == 0) {
        struct ble_hs_adv_fields rsp = {0};
        const char *name = ble_svc_gap_device_name();
        rsp.name = (const uint8_t *)name;
        rsp.name_len = strlen(name);
        rsp.name_is_complete = 1;
        rc = ble_gap_adv_rsp_set_fields(&rsp);
    }
    if (rc == 0) {
        struct ble_gap_adv_params params = {
            .conn_mode = BLE_GAP_CONN_MODE_UND,
            .disc_mode = BLE_GAP_DISC_MODE_GEN,
        };
        rc = ble_gap_adv_start(BLE_OWN_ADDR_RANDOM, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
    }
    if (rc != 0 && rc != BLE_HS_EALREADY) ESP_LOGE(TAG, "advertising failed: %d", rc);
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            advertise();
            break;
        }
        s_conn = event->connect.conn_handle;
        s_secured = false;
        s_subscribed = false;
        s_mtu = BLE_ATT_MTU_DFLT;
        post_event(EV_CONNECTED, 0);
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_secured = false;
        s_subscribed = false;
        post_event(EV_DISCONNECTED, 0);
        advertise();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        break;
    case BLE_GAP_EVENT_PASSKEY_ACTION:
        if (event->passkey.params.action == BLE_SM_IOACT_DISP) {
            struct ble_sm_io io = {
                .action = BLE_SM_IOACT_DISP,
                .passkey = 100000 + esp_random() % 900000,
            };
            post_event(EV_PASSKEY, io.passkey);
            ble_sm_inject_io(event->passkey.conn_handle, &io);
        }
        break;
    case BLE_GAP_EVENT_ENC_CHANGE:
        s_secured = event->enc_change.status == 0;
        post_event(EV_SECURED, s_secured);
        break;
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        // The phone lost its bond: forget ours and pair again.
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_tx_handle) s_subscribed = event->subscribe.cur_notify;
        break;
    case BLE_GAP_EVENT_MTU:
        s_mtu = event->mtu.value;
        break;
    default:
        break;
    }
    return 0;
}

// A static random address kept in this application's NVS. An install through
// the installer erases NVS and with it the bonds, and the new address keeps
// iOS from reusing a bond the device no longer has.
static void on_sync(void)
{
    ble_addr_t addr;
    size_t len = sizeof(addr.val);
    if (nvs_get_blob(s_nvs, NVS_KEY_ADDR, addr.val, &len) != ESP_OK || len != sizeof(addr.val)) {
        if (ble_hs_id_gen_rnd(0, &addr) != 0) {
            ESP_LOGE(TAG, "cannot generate an address");
            return;
        }
        nvs_set_blob(s_nvs, NVS_KEY_ADDR, addr.val, sizeof(addr.val));
        nvs_commit(s_nvs);
    }
    int rc = ble_hs_id_set_rnd(addr.val);
    if (rc != 0) {
        ESP_LOGE(TAG, "address rejected: %d", rc);
        return;
    }
    advertise();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "host reset: %d", reason);
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

// ---------------------------------------------------------------------------
// Start
// ---------------------------------------------------------------------------
static const char *type_name(passport_link_type_t type)
{
    switch (type) {
    case PASSPORT_LINK_BOOL: return "bool";
    case PASSPORT_LINK_INT: return "int";
    case PASSPORT_LINK_ENUM: return "enum";
    default: return "text";
    }
}

static esp_err_t build_info(void)
{
    cJSON *info = cJSON_CreateObject();
    cJSON_AddNumberToObject(info, "v", LINK_PROTOCOL_VERSION);
    cJSON_AddStringToObject(info, "id", s_cfg->app_id);
    cJSON_AddStringToObject(info, "name", s_cfg->display_name);
    cJSON_AddStringToObject(info, "ver", s_cfg->version ? s_cfg->version : esp_app_get_description()->version);
    cJSON *ch = cJSON_AddArrayToObject(info, "ch");
    for (size_t i = 0; i < s_cfg->channel_count; i++) {
        cJSON_AddItemToArray(ch, cJSON_CreateString(s_cfg->channels[i]));
    }
    cJSON *set = cJSON_AddArrayToObject(info, "set");
    for (size_t i = 0; i < s_cfg->setting_count; i++) {
        const passport_link_setting_t *s = &s_cfg->settings[i];
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "k", s->key);
        cJSON_AddStringToObject(item, "n", s->label);
        cJSON_AddStringToObject(item, "type", type_name(s->type));
        switch (s->type) {
        case PASSPORT_LINK_BOOL:
            cJSON_AddBoolToObject(item, "def", s->def_int != 0);
            break;
        case PASSPORT_LINK_INT:
            cJSON_AddNumberToObject(item, "def", s->def_int);
            cJSON_AddNumberToObject(item, "min", s->min);
            cJSON_AddNumberToObject(item, "max", s->max);
            break;
        case PASSPORT_LINK_ENUM: {
            cJSON_AddStringToObject(item, "def", s->def_str);
            cJSON *opt = cJSON_AddArrayToObject(item, "opt");
            for (uint8_t j = 0; j < s->option_count; j++) cJSON_AddItemToArray(opt, cJSON_CreateString(s->options[j]));
            break;
        }
        case PASSPORT_LINK_TEXT:
            cJSON_AddStringToObject(item, "def", s->def_str ? s->def_str : "");
            cJSON_AddNumberToObject(item, "max", s->max);
            break;
        }
        cJSON_AddItemToArray(set, item);
    }
    char *text = cJSON_PrintUnformatted(info);
    cJSON_Delete(info);
    if (!text) return ESP_ERR_NO_MEM;
    size_t len = strlen(text);
    esp_err_t err = ESP_OK;
    if (len > LINK_INFO_MAX) {
        ESP_LOGE(TAG, "INFO is %u bytes; shorten labels or settings (limit %u)", (unsigned)len, LINK_INFO_MAX);
        err = ESP_ERR_INVALID_SIZE;
    } else {
        memcpy(s_info, text, len + 1);
        s_info_len = len;
    }
    cJSON_free(text);
    return err;
}

esp_err_t passport_link_start(const passport_link_config_t *config)
{
    if (s_cfg) return ESP_ERR_INVALID_STATE;
    if (!config || !config->app_id || !config->display_name ||
        !link_schema_valid(config->settings, config->setting_count)) {
        return ESP_ERR_INVALID_ARG;
    }
    s_cfg = config;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &s_nvs);
    if (err == ESP_OK) err = build_info();
    if (err != ESP_OK) {
        s_cfg = NULL;
        return err;
    }
    if (nvs_get_i32(s_nvs, NVS_KEY_TZ, &s_tz_min) == ESP_OK) apply_tz();

    s_rx = xMessageBufferCreate(RX_BUFFER);
    s_tx_lock = xSemaphoreCreateMutex();
    if (!s_rx || !s_tx_lock || xTaskCreate(link_task, "passport_link", TASK_STACK, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    err = nimble_port_init();
    if (err != ESP_OK) return err;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    // Passkey shown by the application, LE Secure Connections, bonded.
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_DISP_ONLY;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(SERVICES);
    if (rc == 0) rc = ble_gatts_add_svcs(SERVICES);
    if (rc == 0) rc = ble_svc_gap_device_name_set(s_cfg->display_name);
    if (rc != 0) {
        ESP_LOGE(TAG, "GATT setup failed: %d", rc);
        return ESP_FAIL;
    }
    ble_store_config_init();
    nimble_port_freertos_init(host_task);
    ESP_LOGI(TAG, "advertising as %s", s_cfg->display_name);
    return ESP_OK;
}
