// examples/wifi-weather/main/weather_ui.c
// 240x320 portrait, rounded corners: content keeps a 24 px margin.
//
//   ┌──────────────────────────┐
//   │ WEATHER        BT  84%   │  BT: phone connected over Passport Link
//   │        Shanghai          │  place
//   │       ☁  (drawn)         │  icon, built from shapes
//   │          18°             │  temperature or status symbol
//   │       Light rain         │  title
//   │  H 21°  L 12°            │  detail (two lines)
//   │  Humidity 60%  Wind 9    │
//   │  Home #1 · 19:30         │  footer: network used, observation time
//   └──────────────────────────┘
// A pairing card with the six-digit code covers the middle while pairing.
// Chinese uses Noto Sans SC subsets (fonts/generate_fonts.py) with Montserrat
// as the fallback for LVGL symbols.
#include "weather_ui.h"

#include <stdio.h>

#include "bsp_display.h"
#include "lvgl.h"

#define COLOR_BG      0x10213A
#define COLOR_TEXT    0xF4F7FB
#define COLOR_MUTED   0x8FA3BF
#define COLOR_SUN     0xFFC94A
#define COLOR_CLOUD   0xD5DDEA
#define COLOR_DARK    0x7D8BA3
#define COLOR_RAIN    0x5AB0FF
#define LOCK_MS       1000
#define BACKLIGHT     80

LV_FONT_DECLARE(wx_font_zh_14);
LV_FONT_DECLARE(wx_font_zh_20);

// Writable copies so the fallback can be set; the generated fonts are const.
static lv_font_t s_zh_14, s_zh_20;
static wx_lang_t s_lang = WX_LANG_EN;

static lv_obj_t *s_battery, *s_link, *s_place, *s_icon, *s_big, *s_title, *s_detail, *s_footer;
static lv_obj_t *s_pair, *s_pair_code, *s_header, *s_pair_title, *s_pair_hint;

static lv_obj_t *text(lv_obj_t *parent, const lv_font_t *font, uint32_t color, int y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_width(l, 192);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
    lv_label_set_text(l, "");
    return l;
}

static lv_obj_t *shape(int x, int y, int w, int h, int radius, uint32_t color)
{
    lv_obj_t *o = lv_obj_create(s_icon);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    return o;
}

static void cloud(int x, int y, uint32_t color)
{
    shape(x + 8, y + 22, 72, 28, 14, color);
    shape(x + 18, y + 8, 32, 32, LV_RADIUS_CIRCLE, color);
    shape(x + 40, y, 36, 36, LV_RADIUS_CIRCLE, color);
}

// Icon area is 120 x 70.
static void draw_icon(wx_kind_t kind)
{
    lv_obj_clean(s_icon);
    switch (kind) {
    case WX_KIND_CLEAR:
        shape(38, 4, 44, 44, LV_RADIUS_CIRCLE, COLOR_SUN);
        break;
    case WX_KIND_PARTLY:
        shape(58, 0, 38, 38, LV_RADIUS_CIRCLE, COLOR_SUN);
        cloud(14, 14, COLOR_CLOUD);
        break;
    case WX_KIND_CLOUDY:
    case WX_KIND_UNKNOWN:
        cloud(16, 6, COLOR_CLOUD);
        break;
    case WX_KIND_FOG:
        for (int i = 0; i < 4; i++) shape(20 + (i % 2) * 8, 10 + i * 14, 72, 6, 3, COLOR_CLOUD);
        break;
    case WX_KIND_RAIN:
        cloud(16, 0, COLOR_CLOUD);
        for (int i = 0; i < 3; i++) shape(38 + i * 18, 56, 5, 13, 3, COLOR_RAIN);
        break;
    case WX_KIND_SNOW:
        cloud(16, 0, COLOR_CLOUD);
        for (int i = 0; i < 3; i++) shape(38 + i * 18, 58, 8, 8, LV_RADIUS_CIRCLE, COLOR_TEXT);
        break;
    case WX_KIND_STORM: {
        cloud(16, 0, COLOR_DARK);
        lv_obj_t *bolt = lv_label_create(s_icon);
        lv_obj_set_style_text_font(bolt, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(bolt, lv_color_hex(COLOR_SUN), 0);
        lv_label_set_text(bolt, LV_SYMBOL_CHARGE);
        lv_obj_set_pos(bolt, 52, 48);
        break;
    }
    }
}

bool wx_ui_init(void)
{
    s_zh_14 = wx_font_zh_14;
    s_zh_14.fallback = &lv_font_montserrat_14;
    s_zh_20 = wx_font_zh_20;
    s_zh_20.fallback = &lv_font_montserrat_20;
    if (!bsp_lvgl_lock(LOCK_MS)) return false;
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_grad_color(scr, lv_color_hex(0x1D3F66), 0);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    s_header = lv_label_create(scr);
    lv_obj_set_style_text_color(s_header, lv_color_hex(COLOR_MUTED), 0);
    lv_obj_set_style_text_letter_space(s_header, 2, 0);
    lv_obj_align(s_header, LV_ALIGN_TOP_LEFT, 30, 20);

    s_battery = lv_label_create(scr);
    lv_obj_set_style_text_font(s_battery, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_battery, lv_color_hex(COLOR_MUTED), 0);
    lv_label_set_text(s_battery, "");
    lv_obj_align(s_battery, LV_ALIGN_TOP_RIGHT, -30, 20);

    s_link = lv_label_create(scr);
    lv_obj_set_style_text_font(s_link, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_link, lv_color_hex(COLOR_RAIN), 0);
    lv_label_set_text(s_link, "");
    lv_obj_align(s_link, LV_ALIGN_TOP_RIGHT, -72, 20);

    s_place = text(scr, &lv_font_montserrat_20, COLOR_TEXT, 46);
    s_icon = lv_obj_create(scr);
    lv_obj_remove_style_all(s_icon);
    lv_obj_set_size(s_icon, 120, 70);
    lv_obj_align(s_icon, LV_ALIGN_TOP_MID, 0, 78);
    s_big = text(scr, &lv_font_montserrat_48, COLOR_TEXT, 150);
    s_title = text(scr, &lv_font_montserrat_20, COLOR_TEXT, 206);
    s_detail = text(scr, &lv_font_montserrat_14, COLOR_MUTED, 236);
    lv_label_set_long_mode(s_detail, LV_LABEL_LONG_WRAP);
    s_footer = text(scr, &lv_font_montserrat_14, COLOR_MUTED, 284);

    s_pair = lv_obj_create(scr);
    lv_obj_set_size(s_pair, 200, 132);
    lv_obj_align(s_pair, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_pair, lv_color_hex(0x0A1626), 0);
    lv_obj_set_style_border_color(s_pair, lv_color_hex(COLOR_RAIN), 0);
    lv_obj_set_style_border_width(s_pair, 2, 0);
    lv_obj_set_style_radius(s_pair, 16, 0);
    lv_obj_remove_flag(s_pair, LV_OBJ_FLAG_SCROLLABLE);
    s_pair_title = lv_label_create(s_pair);
    lv_obj_set_style_text_color(s_pair_title, lv_color_hex(COLOR_MUTED), 0);
    lv_obj_align(s_pair_title, LV_ALIGN_TOP_MID, 0, 4);
    s_pair_code = lv_label_create(s_pair);
    lv_obj_set_style_text_font(s_pair_code, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_pair_code, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_align(s_pair_code, LV_ALIGN_CENTER, 0, 4);
    s_pair_hint = lv_label_create(s_pair);
    lv_obj_set_style_text_color(s_pair_hint, lv_color_hex(COLOR_MUTED), 0);
    lv_obj_align(s_pair_hint, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_add_flag(s_pair, LV_OBJ_FLAG_HIDDEN);

    lv_screen_load(scr);
    bsp_lvgl_unlock();
    wx_ui_set_lang(WX_LANG_EN);
    return true;
}

void wx_ui_set_lang(wx_lang_t lang)
{
    if (!bsp_lvgl_lock(LOCK_MS)) return;
    s_lang = lang;
    bool zh = lang == WX_LANG_ZH;
    const lv_font_t *small = zh ? &s_zh_14 : &lv_font_montserrat_14;
    const lv_font_t *large = zh ? &s_zh_20 : &lv_font_montserrat_20;
    lv_obj_set_style_text_font(s_header, small, 0);
    lv_obj_set_style_text_font(s_place, large, 0);
    lv_obj_set_style_text_font(s_title, large, 0);
    lv_obj_set_style_text_font(s_detail, small, 0);
    lv_obj_set_style_text_font(s_footer, small, 0);
    lv_obj_set_style_text_font(s_pair_title, small, 0);
    lv_obj_set_style_text_font(s_pair_hint, small, 0);
    lv_label_set_text(s_header, wx_str(lang, WX_S_HEADER));
    char title[48];
    snprintf(title, sizeof(title), LV_SYMBOL_BLUETOOTH "  %s", wx_str(lang, WX_S_PAIR_TITLE));
    lv_label_set_text(s_pair_title, title);
    lv_label_set_text(s_pair_hint, wx_str(lang, WX_S_PAIR_HINT));
    bsp_lvgl_unlock();
}

void wx_ui_screen(bool on)
{
    bsp_display_backlight(on ? BACKLIGHT : 0);
}

void wx_ui_status(const char *symbol, const char *title, const char *detail, const char *footer)
{
    if (!bsp_lvgl_lock(LOCK_MS)) return;
    lv_obj_clean(s_icon);
    lv_label_set_text(s_place, "");
    lv_obj_align(s_big, LV_ALIGN_TOP_MID, 0, 100);
    lv_label_set_text(s_big, symbol);
    lv_label_set_text(s_title, title);
    lv_label_set_text(s_detail, detail);
    lv_label_set_text(s_footer, footer);
    bsp_lvgl_unlock();
}

void wx_ui_report(const char *place, const wx_report_t *r, bool fahrenheit, const char *footer)
{
    if (!bsp_lvgl_lock(LOCK_MS)) return;
    lv_label_set_text(s_place, place);
    draw_icon(wx_kind(r->wmo));
    lv_obj_align(s_big, LV_ALIGN_TOP_MID, 0, 150);
    // LVGL's own formatter has no %f; format with newlib instead.
    char buf[96];
    snprintf(buf, sizeof(buf), "%.0f\xC2\xB0%c", r->temp, fahrenheit ? 'F' : 'C');
    lv_label_set_text(s_big, buf);
    lv_label_set_text(s_title, wx_text_lang(s_lang, r->wmo));
    snprintf(buf, sizeof(buf), wx_str(s_lang, WX_S_DETAIL), r->temp_max, r->temp_min, r->humidity, r->wind_kmh);
    lv_label_set_text(s_detail, buf);
    lv_label_set_text(s_footer, footer);
    bsp_lvgl_unlock();
}

void wx_ui_battery(int percent)
{
    if (!bsp_lvgl_lock(LOCK_MS)) return;
    if (percent < 0) lv_label_set_text(s_battery, "");
    else lv_label_set_text_fmt(s_battery, "%d%%", percent);
    bsp_lvgl_unlock();
}

void wx_ui_passkey(uint32_t passkey)
{
    if (!bsp_lvgl_lock(LOCK_MS)) return;
    if (passkey) {
        char code[12];
        snprintf(code, sizeof(code), "%06lu", (unsigned long)passkey);
        lv_label_set_text(s_pair_code, code);
        lv_obj_remove_flag(s_pair, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_pair, LV_OBJ_FLAG_HIDDEN);
    }
    bsp_lvgl_unlock();
}

void wx_ui_link(bool connected)
{
    if (!bsp_lvgl_lock(LOCK_MS)) return;
    lv_label_set_text(s_link, connected ? LV_SYMBOL_BLUETOOTH : "");
    bsp_lvgl_unlock();
}
