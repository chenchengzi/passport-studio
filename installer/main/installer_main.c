// installer/main/installer_main.c
// Resident Bluetooth installer. Two tasks own the work:
//   worker  - receives CONTROL/DATA packets in order and drives inst_session;
//             the only task that writes Flash.
//   main    - connection events, keys, battery; never touches Flash.
// The NimBLE host task only copies packets and events into their queues.
#include <stdio.h>
#include <string.h>

#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_pins.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/message_buffer.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "inst_boot.h"
#include "inst_proto.h"
#include "inst_session.h"
#include "installer_ble.h"
#include "installer_flash.h"
#include "installer_ui.h"
#include "installer_wifi.h"

static const char *TAG = "installer";

#define RX_BUFFER_BYTES    (24 * 1024)   // > INST_WINDOW_BYTES plus per-message overhead
#define PACKET_MAX         520
#define BATTERY_PERIOD_MS  30000
#define RESYNC_GAP_US      (100 * 1000)
#define PROGRESS_GAP_US    (250 * 1000)

typedef enum {
    EV_KEY,
    EV_CONNECTED,
    EV_DISCONNECTED,
    EV_PASSKEY,
    EV_SECURED,
} event_kind_t;

typedef struct {
    event_kind_t kind;
    uint32_t value;
    bsp_btn_t btn;
    bsp_btn_ev_t key_event;
} event_t;

static inst_session_t s_session;          // worker task only
static MessageBufferHandle_t s_rx;
static QueueHandle_t s_events;
static uint32_t s_status_seq;
static volatile bool s_busy;              // receiving, verifying, or restarting
static volatile bool s_secured;

// INFO snapshot, read on the NimBLE host task.
static portMUX_TYPE s_info_lock = portMUX_INITIALIZER_UNLOCKED;
static inst_state_t s_record;
static bool s_record_valid;
static bool s_interrupted;
static bool s_app_bootable;
static uint8_t s_wifi_char[192];
static size_t s_wifi_char_len;

static inst_wifi_store_t s_wifi;          // worker task only, after startup
static uint32_t s_wifi_ops;

// ---------------------------------------------------------------------------
// INFO and STATUS
// ---------------------------------------------------------------------------
static void put32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static size_t read_info(uint8_t *out, size_t cap)
{
    const char *version = esp_app_get_description()->version;
    size_t ver_len = strlen(version);
    taskENTER_CRITICAL(&s_info_lock);
    inst_state_t record = s_record;
    uint8_t flags = (s_record_valid ? INST_INFO_HAS_RECORD : 0) |
                    (s_interrupted ? INST_INFO_PENDING : 0) |
                    (s_app_bootable ? INST_INFO_APP_PRESENT : 0);
    taskEXIT_CRITICAL(&s_info_lock);
    if (!s_record_valid) record.name_len = 0;

    size_t need = 16 + 32 + 1 + record.name_len + 1 + ver_len;
    if (need > cap) return 0;
    uint8_t *p = out;
    *p++ = INST_PROTOCOL_VERSION;
    *p++ = flags;
    *p++ = 0;
    *p++ = 0;
    put32(p, INST_STREAM_START); p += 4;
    put32(p, INST_PLAY_LIMIT); p += 4;
    put32(p, s_record_valid ? record.image_end : 0); p += 4;
    if (s_record_valid) memcpy(p, record.sha256, 32);
    else memset(p, 0, 32);
    p += 32;
    *p++ = record.name_len;
    memcpy(p, record.name, record.name_len); p += record.name_len;
    *p++ = (uint8_t)ver_len;
    memcpy(p, version, ver_len); p += ver_len;
    return (size_t)(p - out);
}

static void refresh_info(void)
{
    inst_state_t record;
    bool ok = inst_session_read_state(installer_flash_ops(), &record);
    bool interrupted = !ok || inst_state_requires_installer(&record);
    bool valid = ok && inst_state_check(&record) == INST_STATE_VALID && !interrupted;
    bool bootable = !interrupted && installer_flash_app_bootable();
    taskENTER_CRITICAL(&s_info_lock);
    s_record = record;
    s_record_valid = valid;
    s_interrupted = interrupted;
    s_app_bootable = bootable;
    taskEXIT_CRITICAL(&s_info_lock);
}

static size_t read_wifi(uint8_t *out, size_t cap)
{
    taskENTER_CRITICAL(&s_info_lock);
    size_t len = s_wifi_char_len <= cap ? s_wifi_char_len : 0;
    memcpy(out, s_wifi_char, len);
    taskEXIT_CRITICAL(&s_info_lock);
    return len;
}

static void publish_wifi(uint8_t result, uint8_t flags)
{
    uint8_t buf[sizeof(s_wifi_char)];
    size_t len = inst_wifi_encode_list(&s_wifi, result, flags, s_wifi_ops, buf, sizeof(buf));
    taskENTER_CRITICAL(&s_info_lock);
    memcpy(s_wifi_char, buf, len);
    s_wifi_char_len = len;
    taskEXIT_CRITICAL(&s_info_lock);
}

static void show_ready(void)
{
    installer_ui_ready_t ready = {
        .app_bootable = s_app_bootable,
        .interrupted = s_interrupted,
        .installed_size = s_record_valid ? s_record.image_end : 0,
    };
    installer_ui_ready(&ready);
}

static void send_status(uint8_t phase, uint8_t flags)
{
    uint8_t status[INST_STATUS_LEN];
    status[0] = phase;
    status[1] = (uint8_t)s_session.error;
    status[2] = (uint8_t)s_session.table_err;
    status[3] = flags;
    put32(status + 4, inst_session_position(&s_session));
    put32(status + 8, s_session.end);
    put32(status + 12, ++s_status_seq);
    installer_ble_set_status(status);
}

static uint8_t phase_code(void)
{
    switch (s_session.phase) {
    case INST_RECEIVING: return INST_PHASE_RECEIVING;
    case INST_DONE: return INST_PHASE_DONE;
    case INST_FAILED: return INST_PHASE_FAILED;
    default: return INST_PHASE_IDLE;
    }
}

// ---------------------------------------------------------------------------
// Worker: the only Flash writer
// ---------------------------------------------------------------------------
static void report_failure(void)
{
    char detail[64];
    if (s_session.error == INST_ERR_TABLE) {
        snprintf(detail, sizeof(detail), "Firmware not compatible:\n%s",
                 inst_table_err_name(s_session.table_err));
    } else {
        snprintf(detail, sizeof(detail), "%s", inst_err_name(s_session.error));
    }
    ESP_LOGE(TAG, "install failed: %s", detail);
    installer_ui_error(detail);
    send_status(INST_PHASE_FAILED, 0);
    s_busy = false;
    refresh_info();
}

static void handle_begin(const uint8_t *p, size_t len)
{
    if (len < INST_BEGIN_LEN || len < (size_t)INST_BEGIN_LEN + p[37]) {
        send_status(phase_code(), 0);
        return;
    }
    uint32_t end = inst_le32(p + 1);
    bool resumed = false;
    inst_err_t err = inst_session_begin(&s_session, end, p + 5, p + 38, p[37], &resumed);
    if (err != INST_OK) {
        s_session.error = err;
        send_status(INST_PHASE_FAILED, 0);
        installer_ui_error(inst_err_name(err));
        return;
    }
    ESP_LOGI(TAG, "%s install: %lu bytes", resumed ? "resuming" : "starting", end);
    s_busy = true;
    installer_ui_progress(inst_session_position(&s_session) - INST_STREAM_START,
                          end - INST_STREAM_START);
    send_status(INST_PHASE_RECEIVING, resumed ? INST_STATUS_RESUMED : 0);
}

static void handle_commit(void)
{
    if (s_session.phase != INST_RECEIVING) {
        send_status(phase_code(), 0);
        return;
    }
    installer_ui_verifying();
    send_status(INST_PHASE_VERIFYING, 0);
    if (inst_session_commit(&s_session) != INST_OK) {
        report_failure();
        return;
    }
    ESP_LOGI(TAG, "install committed");
    if (s_wifi.count > 0) installer_wifi_apply(&s_wifi);
    send_status(INST_PHASE_DONE, 0);
    installer_ui_done();
    vTaskDelay(pdMS_TO_TICKS(1500));   // let the notification and screen go out
    esp_restart();
}

static void handle_wifi_set(const uint8_t *p, size_t len)
{
    static inst_wifi_store_t next;
    uint8_t result, flags = 0;
    next = s_wifi;
    if (!inst_wifi_parse(p + 1, len - 1, &next)) {
        result = INST_WIFI_RESULT_INVALID;
    } else if (!installer_wifi_save(&next)) {
        result = INST_WIFI_RESULT_FAILED;
        installer_wifi_load(&s_wifi);   // the sector may now be erased
    } else {
        s_wifi = next;
        result = INST_WIFI_RESULT_SAVED;
        // Never write the play's NVS while an install owns the play area.
        if (!s_busy && !s_interrupted && s_app_bootable && installer_wifi_apply(&s_wifi)) {
            flags |= INST_WIFI_APPLIED;
        }
    }
    memset(&next, 0, sizeof(next));
    s_wifi_ops++;
    publish_wifi(result, flags);
    if (result == INST_WIFI_RESULT_SAVED && !s_busy) {
        char notice[32];
        snprintf(notice, sizeof(notice), "Wi-Fi saved (%u)", s_wifi.count);
        installer_ui_notice(notice);
    }
}

static void worker_task(void *arg)
{
    (void)arg;
    static uint8_t packet[PACKET_MAX];
    uint32_t last_ack = 0;
    uint32_t last_resync_pos = UINT32_MAX;
    int64_t last_resync_us = 0, last_ui_us = 0;

    for (;;) {
        size_t len = xMessageBufferReceive(s_rx, packet, sizeof(packet), portMAX_DELAY);
        if (len == 0) continue;
        switch (packet[0]) {
        case INST_OP_BEGIN:
            handle_begin(packet, len);
            last_ack = inst_session_position(&s_session);
            break;
        case INST_OP_COMMIT:
            handle_commit();
            break;
        case INST_OP_ABORT:
            inst_session_abort(&s_session);
            s_busy = false;
            send_status(INST_PHASE_IDLE, 0);
            refresh_info();
            if (s_secured) installer_ui_connected();
            break;
        case INST_OP_WIFI_SET:
            handle_wifi_set(packet, len);
            break;
        case INST_OP_REBOOT:
            if (!s_busy && s_app_bootable) esp_restart();
            send_status(phase_code(), 0);
            break;
        case INST_OP_DATA:
        case INST_OP_SKIP: {
            if (len < INST_DATA_HEADER || (packet[0] == INST_OP_SKIP && len < INST_SKIP_LEN)) break;
            uint32_t offset = inst_le32(packet + 1);
            inst_err_t err = packet[0] == INST_OP_DATA
                ? inst_session_data(&s_session, offset, packet + INST_DATA_HEADER, len - INST_DATA_HEADER)
                : inst_session_skip(&s_session, offset, inst_le32(packet + 5));
            uint32_t pos = inst_session_position(&s_session);
            int64_t now = esp_timer_get_time();
            if (err == INST_ERR_SEQ || err == INST_ERR_STATE || err == INST_ERR_SIZE) {
                // Tell the sender where to continue, without flooding it.
                if (pos != last_resync_pos || now - last_resync_us > RESYNC_GAP_US) {
                    send_status(phase_code(), INST_STATUS_RESYNC);
                    last_resync_pos = pos;
                    last_resync_us = now;
                }
                break;
            }
            if (err != INST_OK) {
                report_failure();
                break;
            }
            if (pos - last_ack >= INST_ACK_INTERVAL || pos == s_session.end) {
                send_status(INST_PHASE_RECEIVING, 0);
                last_ack = pos;
            }
            if (now - last_ui_us > PROGRESS_GAP_US || pos == s_session.end) {
                installer_ui_progress(pos - INST_STREAM_START, s_session.end - INST_STREAM_START);
                last_ui_us = now;
            }
            break;
        }
        default:
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// NimBLE host-task callbacks: copy and return
// ---------------------------------------------------------------------------
static bool on_packet(const uint8_t *data, size_t len)
{
    if (len > PACKET_MAX) return false;
    return xMessageBufferSend(s_rx, data, len, 0) == len;
}

static void post(event_kind_t kind, uint32_t value)
{
    event_t ev = {.kind = kind, .value = value};
    (void)xQueueSend(s_events, &ev, 0);
}

static void on_connection(bool connected) { post(connected ? EV_CONNECTED : EV_DISCONNECTED, 0); }
static void on_passkey(uint32_t passkey) { post(EV_PASSKEY, passkey); }
static void on_secured(bool ok) { post(EV_SECURED, ok); }

// Button callbacks run on the esp_timer task: enqueue only.
static void on_key(bsp_btn_t btn, bsp_btn_ev_t key_event, void *user)
{
    (void)user;
    event_t ev = {.kind = EV_KEY, .btn = btn, .key_event = key_event};
    (void)xQueueSend(s_events, &ev, 0);
}

// ---------------------------------------------------------------------------
// Main task: events, keys, battery
// ---------------------------------------------------------------------------
static void handle_event(const event_t *ev, bool *keys_live, bool *exit_requested)
{
    switch (ev->kind) {
    case EV_KEY:
        // The key held at power-on to open the installer is still down when the
        // driver starts. Ignore everything until it has been released once.
        if (!*keys_live) {
            if (ev->key_event == BSP_BTN_RELEASE) *keys_live = true;
            break;
        }
        if (*exit_requested && ev->key_event == BSP_BTN_RELEASE) {
            // Restart only after release, or the bootloader sees a held key
            // and opens the installer again.
            esp_restart();
        }
        if (ev->btn == BSP_BTN_OK && ev->key_event == BSP_BTN_LONG && !s_busy) {
            if (s_app_bootable) {
                *exit_requested = true;
                installer_ui_notice("Release OK to start the app");
            } else {
                installer_ui_notice(s_interrupted ? "Finish the install first" : "No app to start");
            }
        }
        break;
    case EV_PASSKEY:
        installer_ui_pairing(ev->value);
        break;
    case EV_SECURED:
        s_secured = ev->value;
        if (s_busy) break;
        if (ev->value) installer_ui_connected();
        else installer_ui_error("Pairing failed");
        break;
    case EV_CONNECTED:
        if (!s_busy) installer_ui_notice("Connecting...");
        break;
    case EV_DISCONNECTED:
        s_secured = false;
        if (s_session.phase == INST_DONE) break;   // restarting
        // A receiving session stays in memory so the phone can resume it.
        s_busy = false;
        refresh_info();
        show_ready();
        if (s_session.phase == INST_RECEIVING) installer_ui_notice("Disconnected - reconnect to resume");
        break;
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Passport installer %s", esp_app_get_description()->version);
    s_rx = xMessageBufferCreate(RX_BUFFER_BYTES);
    s_events = xQueueCreate(16, sizeof(event_t));
    if (!s_rx || !s_events) {
        ESP_LOGE(TAG, "out of memory");
        return;
    }
    inst_session_init(&s_session, installer_flash_ops());

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BT);
    char name[20];
    snprintf(name, sizeof(name), "Passport-%02X%02X", mac[4], mac[5]);

    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init() || !installer_ui_init(name)) {
        ESP_LOGE(TAG, "display unavailable; continuing without screen");
    } else {
        bsp_display_backlight(80);
    }
    refresh_info();
    show_ready();
    installer_wifi_load(&s_wifi);
    publish_wifi(INST_WIFI_RESULT_NONE, 0);

    bool keys_live = false, exit_requested = false;
    if (bsp_button_init(on_key, NULL) == ESP_OK) {
        keys_live = bsp_button_read_mv() >= BSP_BTN_MV_RELEASED_MIN;
    }
    bool battery = bsp_battery_init() == ESP_OK;
    installer_ui_battery(battery ? bsp_battery_soc() : -1);

    if (xTaskCreate(worker_task, "inst_worker", 6144, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "worker task failed");
        return;
    }
    const installer_ble_callbacks_t callbacks = {
        .on_packet = on_packet,
        .on_connection = on_connection,
        .on_passkey = on_passkey,
        .on_secured = on_secured,
        .read_info = read_info,
        .read_wifi = read_wifi,
    };
    esp_err_t err = installer_ble_start(name, &callbacks);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Bluetooth failed: %s", esp_err_to_name(err));
        installer_ui_error("Bluetooth failed to start");
    }
    ESP_LOGI(TAG, "advertising as %s; app %s, journal %s", name,
             s_app_bootable ? "bootable" : "absent", s_interrupted ? "pending" : "clear");

    int64_t next_battery = esp_timer_get_time() + BATTERY_PERIOD_MS * 1000LL;
    for (;;) {
        event_t ev;
        if (xQueueReceive(s_events, &ev, pdMS_TO_TICKS(1000)) == pdTRUE) {
            handle_event(&ev, &keys_live, &exit_requested);
        }
        if (battery && esp_timer_get_time() >= next_battery) {
            installer_ui_battery(bsp_battery_soc());
            next_battery = esp_timer_get_time() + BATTERY_PERIOD_MS * 1000LL;
        }
    }
}
