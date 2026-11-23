// installer/main/installer_ui.c
// One screen whose widgets are reconfigured per state. 240x320 portrait with
// rounded corners, so content stays inside a 24 px margin.
//
//   ┌──────────────────────────┐
//   │ INSTALLER          84%   │  header
//   │                          │
//   │        ╭──────╮          │  ring: progress, or a static frame
//   │        │  42% │          │  centre: symbol, passkey, or percentage
//   │        ╰──────╯          │
//   │       Installing         │  title
//   │      1.2 / 3.4 MB        │  detail
//   │                          │
//   │  Keep the device on      │  footer hint
//   └──────────────────────────┘
#include "installer_ui.h"

#include <stdio.h>

#include "bsp_display.h"
#include "lvgl.h"

#define COLOR_BG      0x0E1726
#define COLOR_RING    0x1F2D44
#define COLOR_TEXT    0xF2F5FA
#define COLOR_MUTED   0x8494AD
#define COLOR_ACCENT  0x3DDC97
#define COLOR_WARN    0xFFB547
#define COLOR_ERROR   0xFF6B6B
#define LOCK_MS       1000

static lv_obj_t *s_battery;
static lv_obj_t *s_ring;
static lv_obj_t *s_center;
static lv_obj_t *s_title;
static lv_obj_t *s_detail;
static lv_obj_t *s_name;
static lv_obj_t *s_footer;

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, int y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_width(l, 200);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
    lv_label_set_text(l, "");
    return l;
}

bool installer_ui_init(const char *device_name)
{
    if (!bsp_lvgl_lock(LOCK_MS)) return false;
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *header = lv_label_create(scr);
    lv_obj_set_style_text_font(header, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(header, lv_color_hex(COLOR_MUTED), 0);
    lv_obj_set_style_text_letter_space(header, 2, 0);
    lv_label_set_text(header, "INSTALLER");
    lv_obj_align(header, LV_ALIGN_TOP_LEFT, 30, 20);

    s_battery = lv_label_create(scr);
    lv_obj_set_style_text_font(s_battery, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_battery, lv_color_hex(COLOR_MUTED), 0);
    lv_label_set_text(s_battery, "");
    lv_obj_align(s_battery, LV_ALIGN_TOP_RIGHT, -30, 20);

    s_ring = lv_arc_create(scr);
    lv_obj_set_size(s_ring, 140, 140);
    lv_obj_align(s_ring, LV_ALIGN_TOP_MID, 0, 52);
    lv_arc_set_rotation(s_ring, 270);
    lv_arc_set_bg_angles(s_ring, 0, 360);
    lv_arc_set_range(s_ring, 0, 1000);
    lv_arc_set_value(s_ring, 0);
    lv_obj_remove_style(s_ring, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(s_ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_ring, 8, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_ring, 8, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_ring, lv_color_hex(COLOR_RING), LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_ring, lv_color_hex(COLOR_ACCENT), LV_PART_INDICATOR);

    s_center = lv_label_create(scr);
    lv_obj_set_style_text_font(s_center, &lv_font_montserrat_40, 0);
    lv_obj_set_style_text_color(s_center, lv_color_hex(COLOR_TEXT), 0);
    lv_label_set_text(s_center, "");
    lv_obj_align_to(s_center, s_ring, LV_ALIGN_CENTER, 0, 0);

    s_title = label(scr, &lv_font_montserrat_20, COLOR_TEXT, 206);
    s_detail = label(scr, &lv_font_montserrat_14, COLOR_MUTED, 236);
    s_name = label(scr, &lv_font_montserrat_14, COLOR_ACCENT, 274);
    lv_label_set_text(s_name, device_name);
    s_footer = label(scr, &lv_font_montserrat_14, COLOR_MUTED, 290);

    lv_screen_load(scr);
    bsp_lvgl_unlock();
    return true;
}

// Called with the lock held.
static void layout(const char *center, uint32_t center_color, int32_t ring_value,
                   uint32_t ring_color, const char *title, const char *detail, const char *footer)
{
    lv_label_set_text(s_center, center);
    lv_obj_set_style_text_color(s_center, lv_color_hex(center_color), 0);
    lv_obj_align_to(s_center, s_ring, LV_ALIGN_CENTER, 0, 0);
    lv_arc_set_value(s_ring, ring_value);
    lv_obj_set_style_arc_color(s_ring, lv_color_hex(ring_color), LV_PART_INDICATOR);
    lv_label_set_text(s_title, title);
    lv_label_set_text(s_detail, detail);
    lv_label_set_text(s_footer, footer ? footer : "");
    lv_obj_set_style_text_color(s_footer, lv_color_hex(COLOR_MUTED), 0);
    // The device name is only useful while the phone is looking for it.
    if (footer) lv_obj_add_flag(s_name, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(s_name, LV_OBJ_FLAG_HIDDEN);
}

void installer_ui_ready(const installer_ui_ready_t *ready)
{
    char detail[64];
    if (ready->interrupted) {
        snprintf(detail, sizeof(detail), "Last install did not finish.\nInstall again from iPhone.");
    } else if (ready->app_bootable && ready->installed_size) {
        snprintf(detail, sizeof(detail), "Installed app: %.1f MB\nHold OK to start it.",
                 ready->installed_size / 1048576.0);
    } else if (ready->app_bootable) {
        snprintf(detail, sizeof(detail), "Open Passport Library on iPhone.\nHold OK to start the app.");
    } else {
        snprintf(detail, sizeof(detail), "No app installed yet.\nOpen Passport Library on iPhone.");
    }
    if (!bsp_lvgl_lock(LOCK_MS)) return;
    layout(LV_SYMBOL_BLUETOOTH, ready->interrupted ? COLOR_WARN : COLOR_ACCENT, 0,
           COLOR_ACCENT, "Ready", detail, NULL);
    bsp_lvgl_unlock();
}

void installer_ui_pairing(uint32_t passkey)
{
    char code[16];
    snprintf(code, sizeof(code), "%03lu %03lu", passkey / 1000, passkey % 1000);
    if (!bsp_lvgl_lock(LOCK_MS)) return;
    layout(code, COLOR_TEXT, 0, COLOR_ACCENT, "Pairing code",
           "Enter this code on your iPhone.", "Code changes every connection");
    bsp_lvgl_unlock();
}

void installer_ui_connected(void)
{
    if (!bsp_lvgl_lock(LOCK_MS)) return;
    layout(LV_SYMBOL_OK, COLOR_ACCENT, 1000, COLOR_ACCENT, "Connected",
           "Choose firmware in the app.", "Waiting for iPhone");
    bsp_lvgl_unlock();
}

void installer_ui_progress(uint32_t done, uint32_t total)
{
    uint32_t permille = total ? (uint32_t)((uint64_t)done * 1000 / total) : 0;
    char center[16], detail[40];
    snprintf(center, sizeof(center), "%lu%%", permille / 10);
    snprintf(detail, sizeof(detail), "%.1f / %.1f MB", done / 1048576.0, total / 1048576.0);
    if (!bsp_lvgl_lock(LOCK_MS)) return;
    layout(center, COLOR_TEXT, (int32_t)permille, COLOR_ACCENT, "Installing", detail,
           "Keep the device on");
    bsp_lvgl_unlock();
}

void installer_ui_verifying(void)
{
    if (!bsp_lvgl_lock(LOCK_MS)) return;
    layout(LV_SYMBOL_REFRESH, COLOR_ACCENT, 1000, COLOR_ACCENT, "Verifying",
           "Checking what was written.", "Keep the device on");
    bsp_lvgl_unlock();
}

void installer_ui_done(void)
{
    if (!bsp_lvgl_lock(LOCK_MS)) return;
    layout(LV_SYMBOL_OK, COLOR_ACCENT, 1000, COLOR_ACCENT, "Installed",
           "Starting the new app...", " ");
    bsp_lvgl_unlock();
}

void installer_ui_error(const char *detail)
{
    if (!bsp_lvgl_lock(LOCK_MS)) return;
    layout(LV_SYMBOL_WARNING, COLOR_ERROR, 1000, COLOR_ERROR, "Install failed", detail,
           "Retry from the iPhone app");
    bsp_lvgl_unlock();
}

void installer_ui_notice(const char *text)
{
    if (!bsp_lvgl_lock(LOCK_MS)) return;
    lv_label_set_text(s_footer, text);
    lv_obj_set_style_text_color(s_footer, lv_color_hex(COLOR_WARN), 0);
    lv_obj_add_flag(s_name, LV_OBJ_FLAG_HIDDEN);
    bsp_lvgl_unlock();
}

void installer_ui_battery(int percent)
{
    if (!bsp_lvgl_lock(LOCK_MS)) return;
    if (percent < 0) lv_label_set_text(s_battery, "");
    else lv_label_set_text_fmt(s_battery, "%d%%", percent);
    bsp_lvgl_unlock();
}
