// examples/t3-status/main/t3_main.c
// T3 Code status display. Joins the first reachable network from
//   1. the installer's saved list (components/passport_wifi), in priority order;
//   2. the station settings the installer wrote into this app's NVS;
// then polls the bridge on the Mac (bridge/bridge.mjs) every POLL_MS and shows
// how many T3 Code threads are running or waiting for approval.
// The bridge address and screen timeout come from the iPhone app's Device Link
// page. Keys (a press that wakes the screen does nothing else):
//   home       DOWN: task pages; OK: poll now
//   task page  UP/DOWN: previous/next (UP on the first: home); OK: home
// Task pages return home after TASK_IDLE_MS without a key. A task page shows
// what a waiting task asks for, or the running task's latest reply.
// When a run finishes the home screen highlights it for JUST_DONE_MS (until a
// key press). A new approval request or a finished run wakes the screen,
// unless that is turned off.
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "passport_link.h"
#include "passport_wifi.h"

#include "t3_logic.h"
#include "t3_net.h"
#include "t3_text.h"
#include "t3_ui.h"

static const char *TAG = "t3_status";

#define POLL_MS             2000
#define HTTP_TIMEOUT_MS     1500
#define CONNECT_TIMEOUT_MS  15000
#define RETRY_MS            (60 * 1000)
#define NO_ADDRESS_MS       5000
#define BATTERY_MS          30000
#define TASK_IDLE_MS        (60 * 1000)
#define JUST_DONE_MS        (10 * 60 * 1000)
#define ADDRESS_MAX         64

typedef enum {
    EV_KEY,           // a key went down
    EV_SETTINGS,      // the phone changed a setting
} event_kind_t;

typedef struct {
    event_kind_t kind;
    bsp_btn_t btn;
} event_t;

// What ended wait_event().
typedef enum {
    WAKE_TIMEOUT,
    WAKE_POLL,        // poll now: OK on home, a settings change, opening the task pages
} wake_t;

static QueueHandle_t s_events;
static bool s_nvs_ok;

// Screen timeout state, status task only.
static int64_t s_last_activity_us;
static int64_t s_next_battery_us;
static bool s_screen_on = true;
static bool s_battery;

// Task pages, status task only. s_tasks is the last list the bridge sent.
static t3_nav_t s_nav;
static t3_task_t s_tasks[T3_MAX_TASKS];
static int s_task_count;
static enum { TASKS_NONE, TASKS_OK, TASKS_FAILED } s_tasks_state;
static int64_t s_nav_us;    // last key on a task page
static bool s_live;         // the last poll found T3 Code: task pages may open
// Detail of the selected task; s_detail_id names the task it belongs to.
static t3_detail_t s_detail;
static char s_detail_id[T3_TASK_ID_LEN + 1];
static int64_t s_just_done_until_us;   // home highlights the finished run until then

static const passport_link_setting_t SETTINGS[] = {
    PASSPORT_LINK_TEXT("bridge", "桥接地址（Mac 的 IP，可加 :端口）", "", ADDRESS_MAX),
    PASSPORT_LINK_INT("screen", "自动关屏（分钟，0 为常亮）", 10, 0, 60),
    PASSPORT_LINK_BOOL_SETTING("wake", "有新的待批准或任务完成时亮屏", 1),
};
static const char *const CHANNELS[] = {"time"};

static void post(event_kind_t kind, bsp_btn_t btn)
{
    event_t ev = {.kind = kind, .btn = btn};
    (void)xQueueSend(s_events, &ev, 0);
}

// Runs on the button driver's timer task: enqueue only. Presses, not clicks:
// a click waits out the double-click window, and quick presses must all count.
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    if (ev == BSP_BTN_PRESS) post(EV_KEY, btn);
}

// ---------------------------------------------------------------------------
// Passport Link callbacks (component task)
// ---------------------------------------------------------------------------
static void on_settings(void *user)
{
    (void)user;
    post(EV_SETTINGS, BSP_BTN_OK);
}

static void on_passkey(uint32_t passkey, void *user)
{
    (void)user;
    t3_ui_passkey(passkey);
}

static void on_connection(bool connected, void *user)
{
    (void)user;
    t3_ui_link(connected);
}

// ---------------------------------------------------------------------------
// Status task
// ---------------------------------------------------------------------------
static void screen_wake(void)
{
    s_last_activity_us = esp_timer_get_time();
    if (!s_screen_on) {
        t3_ui_screen(true);
        s_screen_on = true;
    }
}

static void screen_check(void)
{
    int32_t minutes = passport_link_get_int("screen");   // 0: always on
    if (minutes > 0 && s_screen_on &&
        esp_timer_get_time() - s_last_activity_us >= (int64_t)minutes * 60 * 1000000) {
        t3_ui_screen(false);
        s_screen_on = false;
    }
}

static void show_task_page(void)
{
    if (s_task_count == 0 || s_nav.page > s_task_count) {
        const char *message = s_tasks_state == TASKS_NONE ? T3_TXT_TASKS_LOADING
                              : s_tasks_state == TASKS_FAILED ? T3_TXT_TASKS_FAILED
                              : T3_TXT_TASKS_NONE;
        t3_ui_task(1, 0, NULL, NULL, "", message);
        return;
    }
    const t3_task_t *t = &s_tasks[s_nav.page - 1];
    const t3_detail_t *d = strcmp(s_detail_id, t->id) ? NULL : &s_detail;
    char elapsed[48] = "";
    int64_t min = t3_elapsed_min(t->started, (int64_t)time(NULL));
    if (min == 0) snprintf(elapsed, sizeof(elapsed), "%s", T3_TXT_ELAPSED_NEW);
    else if (min > 0 && min < 60) snprintf(elapsed, sizeof(elapsed), T3_TXT_ELAPSED_MIN, (int)min);
    else if (min >= 60) snprintf(elapsed, sizeof(elapsed), T3_TXT_ELAPSED_HOUR, (int)(min / 60), (int)(min % 60));
    t3_ui_task(s_nav.page, s_task_count, t, d, elapsed, NULL);
}

// A key press on a lit screen. Returns true when the bridge should be polled
// now; otherwise any page change is already drawn.
static bool on_key_press(bsp_btn_t btn)
{
    s_just_done_until_us = 0;   // seen
    if (s_nav.page == 0 && btn == BSP_BTN_OK) return true;
    if (s_nav.page == 0 && !s_live) return false;
    bool opening = s_nav.page == 0;
    t3_key_t key = btn == BSP_BTN_UP ? T3_KEY_UP : btn == BSP_BTN_DOWN ? T3_KEY_DOWN : T3_KEY_OK;
    if (!t3_nav_key(&s_nav, key, s_tasks, s_task_count)) return false;
    s_nav_us = esp_timer_get_time();
    if (opening) s_tasks_state = TASKS_NONE;   // show "loading" until this poll answers
    if (s_nav.page == 0) return true;          // redraw home from fresh status
    show_task_page();
    return true;                               // fetch the new page's detail now
}

// Wait until the timeout or something that needs a poll, handling keys, the
// screen timeout, the task page timeout, and battery reading meanwhile.
static wake_t wait_event(int64_t timeout_ms)
{
    int64_t deadline = esp_timer_get_time() + timeout_ms * 1000;
    for (;;) {
        int64_t now = esp_timer_get_time();
        if (now >= deadline) return WAKE_TIMEOUT;
        int64_t step = deadline - now < 1000000 ? deadline - now : 1000000;
        event_t ev;
        if (xQueueReceive(s_events, &ev, pdMS_TO_TICKS(step / 1000 + 1)) == pdTRUE) {
            bool was_off = !s_screen_on;
            screen_wake();
            if (ev.kind == EV_SETTINGS) return WAKE_POLL;
            if (!was_off && on_key_press(ev.btn)) return WAKE_POLL;
        }
        screen_check();
        if (s_nav.page > 0 && esp_timer_get_time() - s_nav_us >= (int64_t)TASK_IDLE_MS * 1000) {
            s_nav = (t3_nav_t){0};
            return WAKE_POLL;
        }
        if (s_battery && esp_timer_get_time() >= s_next_battery_us) {
            t3_ui_battery(bsp_battery_soc());
            s_next_battery_us = esp_timer_get_time() + (int64_t)BATTERY_MS * 1000;
        }
    }
}

// Try every saved network in order; true once one is joined.
static bool join(const t3_network_t *driver)
{
    static passport_wifi_network_t saved[PASSPORT_WIFI_MAX_NETWORKS];
    static t3_network_t list[PASSPORT_WIFI_MAX_NETWORKS];
    static t3_candidate_t cands[T3_MAX_CANDIDATES];

    size_t count = passport_wifi_list(saved, PASSPORT_WIFI_MAX_NETWORKS);
    for (size_t i = 0; i < count; i++) {
        memcpy(list[i].ssid, saved[i].ssid, sizeof(list[i].ssid));
        memcpy(list[i].password, saved[i].password, sizeof(list[i].password));
    }
    memset(saved, 0, sizeof(saved));
    size_t n = t3_candidates(list, count, driver, cands, T3_MAX_CANDIDATES);
    memset(list, 0, sizeof(list));
    ESP_LOGI(TAG, "%u saved network(s), driver setting %s", (unsigned)count, driver->ssid[0] ? "yes" : "none");
    if (n == 0) {
        t3_ui_message(T3_TXT_NO_WIFI, T3_TXT_NO_WIFI_HINT);
        return false;
    }

    char line[64];
    bool joined = false;
    for (size_t i = 0; i < n && !joined; i++) {
        snprintf(line, sizeof(line), T3_TXT_JOINING, cands[i].net.ssid);
        t3_ui_message(line, "");
        ESP_LOGI(TAG, "trying %s", cands[i].net.ssid);
        joined = t3_net_connect(&cands[i].net, CONNECT_TIMEOUT_MS);
        if (!joined) ESP_LOGW(TAG, "could not join %s", cands[i].net.ssid);
    }
    memset(cands, 0, sizeof(cands));
    if (!joined) t3_ui_message(T3_TXT_CANT_JOIN, T3_TXT_CANT_JOIN_HINT);
    return joined;
}

static void status_task(void *arg)
{
    (void)arg;
    static t3_network_t driver;
    s_battery = bsp_battery_init() == ESP_OK;
    t3_ui_battery(s_battery ? bsp_battery_soc() : -1);
    s_last_activity_us = esp_timer_get_time();
    s_next_battery_us = s_last_activity_us + (int64_t)BATTERY_MS * 1000;

    t3_ui_message(T3_TXT_STARTING, "");
    esp_err_t err = t3_net_init(s_nvs_ok, &driver);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi init failed: %s", esp_err_to_name(err));
        t3_ui_message(T3_TXT_WIFI_FAILED, esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    t3_health_t health;
    t3_health_reset(&health);
    char address[ADDRESS_MAX + 1] = "", previous[sizeof(address)] = "";
    char host[ADDRESS_MAX + 1], footer[160], done[6];
    int last_wait = 0;
    int64_t last_done = 0;

    for (;;) {
        if (!t3_net_connected()) {
            t3_health_reset(&health);
            s_live = false;
            s_nav = (t3_nav_t){0};
            if (!join(&driver)) {
                wait_event(RETRY_MS);
                continue;
            }
        }

        passport_link_get_str("bridge", address, sizeof(address));
        if (strcmp(address, previous)) {
            t3_health_reset(&health);   // a new bridge: forget the old one's status
            last_wait = 0;
            last_done = 0;
            s_just_done_until_us = 0;
            memcpy(previous, address, sizeof(previous));
        }
        uint16_t port;
        if (!t3_parse_address(address, host, sizeof(host), &port)) {
            s_live = false;
            s_nav = (t3_nav_t){0};
            t3_ui_message(address[0] ? T3_TXT_BAD_ADDRESS : T3_TXT_NO_ADDRESS, T3_TXT_NO_ADDRESS_HINT);
            wait_event(NO_ADDRESS_MS);
            continue;
        }

        t3_status_t s;
        if (t3_net_poll(host, port, HTTP_TIMEOUT_MS, &s) == ESP_OK) t3_health_ok(&health, &s);
        else t3_health_fail(&health);

        t3_view_t view = t3_health_view(&health);
        s_live = view == T3_VIEW_LIVE;
        if (!s_live) s_nav = (t3_nav_t){0};
        if (view == T3_VIEW_UNREACHABLE) {
            char where[ADDRESS_MAX + 8];
            snprintf(where, sizeof(where), "%s:%u", host, (unsigned)port);
            snprintf(footer, sizeof(footer), T3_TXT_UNREACHABLE_HINT, where);
        } else if (view == T3_VIEW_T3_OFFLINE) {
            snprintf(footer, sizeof(footer), "%s", T3_TXT_OFFLINE_HINT);
        } else {
            snprintf(footer, sizeof(footer), "%s:%u", host, (unsigned)port);
        }

        bool woke = false;
        if (view == T3_VIEW_LIVE) {
            bool wake = passport_link_get_bool("wake");
            if (t3_done_changed(last_done, health.last.done_at)) {
                s_just_done_until_us = esp_timer_get_time() + (int64_t)JUST_DONE_MS * 1000;
                woke = wake;
            }
            if (health.last.wait > last_wait) woke = woke || wake;
            if (health.last.done_at > 0) last_done = health.last.done_at;
            last_wait = health.last.wait;
        }
        bool just_done = s_just_done_until_us > esp_timer_get_time();
        if (view == T3_VIEW_LIVE && just_done && health.last.done_title[0]) {
            snprintf(footer, sizeof(footer), T3_TXT_JUST_DONE, health.last.done_title);
        }

        if (s_nav.page > 0) {
            static t3_task_t fetched[T3_MAX_TASKS];
            int count;
            if (t3_net_tasks(host, port, HTTP_TIMEOUT_MS, fetched, T3_MAX_TASKS, &count) == ESP_OK) {
                memcpy(s_tasks, fetched, sizeof(s_tasks));
                s_task_count = count;
                s_tasks_state = TASKS_OK;
                t3_nav_sync(&s_nav, s_tasks, s_task_count);
            } else if (s_tasks_state != TASKS_OK) {
                s_tasks_state = TASKS_FAILED;   // otherwise keep showing the last list
            }
            if (s_nav.page > 0 && s_nav.page <= s_task_count) {
                const char *id = s_tasks[s_nav.page - 1].id;
                static t3_detail_t fresh;
                if (t3_net_detail(host, port, id, HTTP_TIMEOUT_MS, &fresh) == ESP_OK) {
                    s_detail = fresh;
                    memcpy(s_detail_id, id, sizeof(s_detail_id));
                }   // on failure keep the last detail of this task, if any
            }
            show_task_page();
        } else {
            t3_format_done(health.last.done_at, (int64_t)time(NULL), passport_link_tz_offset_min(), done);
            t3_ui_status(view, health.have_last ? &health.last : NULL, done, footer, just_done);
        }

        if (woke) screen_wake();
        wait_event(POLL_MS);
    }
}

void app_main(void)
{
    s_events = xQueueCreate(8, sizeof(event_t));
    if (!s_events || bsp_display_init() != ESP_OK || !bsp_lvgl_init() || !t3_ui_init()) {
        ESP_LOGE(TAG, "display unavailable");
        return;
    }
    t3_ui_screen(true);
    if (bsp_button_init(on_key, NULL) != ESP_OK) ESP_LOGW(TAG, "buttons unavailable");

    // Never erase NVS here: it holds the Wi-Fi settings the installer wrote.
    esp_err_t err = nvs_flash_init();
    s_nvs_ok = err == ESP_OK;
    if (!s_nvs_ok) ESP_LOGE(TAG, "NVS unavailable (%s); Wi-Fi list only, no phone link", esp_err_to_name(err));

    static const passport_link_config_t link = {
        .app_id = "t3-status",
        .display_name = "T3 Status",
        .channels = CHANNELS,
        .channel_count = sizeof(CHANNELS) / sizeof(CHANNELS[0]),
        .settings = SETTINGS,
        .setting_count = sizeof(SETTINGS) / sizeof(SETTINGS[0]),
        .on_settings = on_settings,
        .on_passkey = on_passkey,
        .on_connection = on_connection,
    };
    if (s_nvs_ok) {
        err = passport_link_start(&link);
        if (err != ESP_OK) ESP_LOGE(TAG, "Passport Link failed: %s", esp_err_to_name(err));
    }

    if (xTaskCreate(status_task, "t3_status", 6144, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "status task failed");
    }
}
