// examples/t3-status/main/t3_ui.c
// 240x320 portrait with rounded corners: content keeps a 24 px margin.
//
//   ┌──────────────────────────┐
//   │ T3 CODE          BT 84%  │  BT: phone connected over Passport Link
//   │ ● 在线                    │  status line; dot color = state
//   │ ┌──────────────────────┐ │
//   │ │ 正在执行           2 │ │  green when > 0
//   │ └──────────────────────┘ │
//   │ ┌──────────────────────┐ │
//   │ │ 等待批准           1 │ │  amber outline when > 0
//   │ └──────────────────────┘ │
//   │ ┌──────────────────────┐ │
//   │ │ 最近完成       17:12 │ │  blue outline right after a run finishes
//   │ └──────────────────────┘ │
//   │     192.168.1.10:8787    │  footer: bridge, hint, or the finished task
//   └──────────────────────────┘
// DOWN opens the task pages: one card per running or waiting thread.
//
//   │ ● 任务 1/2                │
//   │ ┌──────────────────────┐ │
//   │ │ 运行命令      12 分钟 │ │  state or request kind, colored; elapsed
//   │ │ passport-studio      │ │  project
//   │ │ T3 Code Status       │ │  title, up to two lines
//   │ │ Display App          │ │
//   │ │ git push origin main │ │  waiting: what is asked (amber);
//   │ │ …                    │ │  running: the latest reply
//   │ │ 后台任务 2 个 · npm…  │ │  only with background work
//   │ └──────────────────────┘ │
//   │   上下键切换 · OK 返回    │
// A pairing card with the six-digit code covers the middle while pairing.
// Chinese uses Noto Sans SC subsets (fonts/generate_fonts.py) with Montserrat
// as the fallback for LVGL symbols.
#include "t3_ui.h"

#include <stdio.h>

#include "bsp_display.h"
#include "lvgl.h"
#include "t3_text.h"

#define COLOR_BG      0x0E1116
#define COLOR_CARD    0x1A1F27
#define COLOR_TEXT    0xF2F4F7
#define COLOR_MUTED   0x7D8696
#define COLOR_FAINT   0x4A5260
#define COLOR_RUN     0x3DDC84
#define COLOR_WAIT    0xFFB020
#define COLOR_DONE    0x6EA8FE
#define COLOR_ERROR   0xFF5C5C
#define LOCK_MS       1000
#define BACKLIGHT     70

LV_FONT_DECLARE(t3_font_zh_14);
LV_FONT_DECLARE(t3_font_zh_20);

// Writable copies so the fallback can be set; the generated fonts are const.
static lv_font_t s_zh_14, s_zh_20;

typedef struct {
    lv_obj_t *card, *label, *value;
} card_t;

static lv_obj_t *s_battery, *s_link, *s_dot, *s_state, *s_footer;
static card_t s_run, s_wait, s_done;
static lv_obj_t *s_pair, *s_pair_code;
static lv_obj_t *s_task, *s_task_state, *s_task_project, *s_task_title, *s_task_elapsed, *s_task_empty;
static lv_obj_t *s_task_detail, *s_task_bg;
static int32_t s_detail_y, s_line_14;

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, "");
    return l;
}

static card_t card(lv_obj_t *scr, int y, const char *title, const lv_font_t *value_font)
{
    card_t c;
    c.card = lv_obj_create(scr);
    lv_obj_remove_style_all(c.card);
    lv_obj_set_pos(c.card, 24, y);
    lv_obj_set_size(c.card, 192, 64);
    lv_obj_set_style_radius(c.card, 14, 0);
    lv_obj_set_style_bg_color(c.card, lv_color_hex(COLOR_CARD), 0);
    lv_obj_set_style_bg_opa(c.card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(c.card, lv_color_hex(COLOR_WAIT), 0);
    lv_obj_set_style_border_width(c.card, 0, 0);
    lv_obj_set_scrollable(c.card, false);

    c.label = label(c.card, &s_zh_20, COLOR_MUTED);
    lv_label_set_text(c.label, title);
    lv_obj_align(c.label, LV_ALIGN_LEFT_MID, 16, 0);
    c.value = label(c.card, value_font, COLOR_FAINT);
    lv_label_set_text(c.value, "-");
    lv_obj_align(c.value, LV_ALIGN_RIGHT_MID, -16, 0);
    return c;
}

static void card_set(card_t *c, const char *value, uint32_t color, bool outline)
{
    lv_label_set_text(c->value, value);
    lv_obj_set_style_text_color(c->value, lv_color_hex(color), 0);
    lv_obj_set_style_border_width(c->card, outline ? 2 : 0, 0);
    lv_obj_align(c->value, LV_ALIGN_RIGHT_MID, -16, 0);
}

static void state_set(uint32_t dot, const char *text)
{
    lv_obj_set_style_bg_color(s_dot, lv_color_hex(dot), 0);
    lv_label_set_text(s_state, text);
}

static void show_home(bool home)
{
    lv_obj_set_hidden(s_run.card, !home);
    lv_obj_set_hidden(s_wait.card, !home);
    lv_obj_set_hidden(s_done.card, !home);
    lv_obj_set_hidden(s_task, home);
}

static void task_card(lv_obj_t *scr)
{
    s_task = lv_obj_create(scr);
    lv_obj_remove_style_all(s_task);
    lv_obj_set_pos(s_task, 24, 84);
    lv_obj_set_size(s_task, 192, 208);
    lv_obj_set_style_radius(s_task, 14, 0);
    lv_obj_set_style_bg_color(s_task, lv_color_hex(COLOR_CARD), 0);
    lv_obj_set_style_bg_opa(s_task, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_task, lv_color_hex(COLOR_WAIT), 0);
    lv_obj_set_scrollable(s_task, false);

    s_task_state = label(s_task, &s_zh_14, COLOR_RUN);
    lv_obj_set_pos(s_task_state, 16, 12);
    s_task_elapsed = label(s_task, &s_zh_14, COLOR_MUTED);
    lv_obj_align(s_task_elapsed, LV_ALIGN_TOP_RIGHT, -16, 12);

    s_task_project = label(s_task, &s_zh_14, COLOR_MUTED);
    lv_obj_set_width(s_task_project, 160);
    lv_label_set_long_mode(s_task_project, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_task_project, 16, 32);

    // Two lines; a longer title ends in an ellipsis.
    int32_t line = lv_font_get_line_height(&s_zh_20);
    s_task_title = label(s_task, &s_zh_20, COLOR_TEXT);
    lv_obj_set_size(s_task_title, 160, line * 2);
    lv_label_set_long_mode(s_task_title, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_task_title, 16, 52);

    // The request or reply fills the rest; its height depends on the
    // background line (t3_ui_task).
    s_line_14 = lv_font_get_line_height(&s_zh_14);
    s_detail_y = 52 + line * 2 + 6;
    s_task_detail = label(s_task, &s_zh_14, COLOR_MUTED);
    lv_obj_set_width(s_task_detail, 160);
    lv_label_set_long_mode(s_task_detail, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_task_detail, 16, s_detail_y);

    s_task_bg = label(s_task, &s_zh_14, COLOR_DONE);
    lv_obj_set_width(s_task_bg, 160);
    lv_label_set_long_mode(s_task_bg, LV_LABEL_LONG_DOT);
    lv_obj_align(s_task_bg, LV_ALIGN_BOTTOM_LEFT, 16, -12);

    s_task_empty = label(s_task, &s_zh_20, COLOR_MUTED);
    lv_obj_set_width(s_task_empty, 160);
    lv_label_set_long_mode(s_task_empty, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_task_empty, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_task_empty, LV_ALIGN_CENTER, 0, 0);

    lv_obj_set_hidden(s_task, true);
}

bool t3_ui_init(void)
{
    s_zh_14 = t3_font_zh_14;
    s_zh_14.fallback = &lv_font_montserrat_14;
    s_zh_20 = t3_font_zh_20;
    s_zh_20.fallback = &lv_font_montserrat_20;
    if (!bsp_lvgl_lock(LOCK_MS)) return false;
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(scr, false);

    lv_obj_t *header = label(scr, &lv_font_montserrat_14, COLOR_MUTED);
    lv_obj_set_style_text_letter_space(header, 3, 0);
    lv_label_set_text(header, T3_TXT_HEADER);
    lv_obj_align(header, LV_ALIGN_TOP_LEFT, 28, 20);

    s_battery = label(scr, &lv_font_montserrat_14, COLOR_MUTED);
    lv_obj_align(s_battery, LV_ALIGN_TOP_RIGHT, -28, 20);
    s_link = label(scr, &lv_font_montserrat_14, COLOR_DONE);
    lv_obj_align(s_link, LV_ALIGN_TOP_RIGHT, -70, 20);

    s_dot = lv_obj_create(scr);
    lv_obj_remove_style_all(s_dot);
    lv_obj_set_size(s_dot, 10, 10);
    lv_obj_set_style_radius(s_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_dot, lv_color_hex(COLOR_FAINT), 0);
    lv_obj_set_pos(s_dot, 30, 55);
    s_state = label(scr, &s_zh_20, COLOR_TEXT);
    lv_obj_set_width(s_state, 160);
    lv_label_set_long_mode(s_state, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_state, 48, 46);

    s_run = card(scr, 84, T3_TXT_RUN, &lv_font_montserrat_48);
    s_wait = card(scr, 156, T3_TXT_WAIT, &lv_font_montserrat_48);
    s_done = card(scr, 228, T3_TXT_DONE, &lv_font_montserrat_20);

    task_card(scr);

    s_footer = label(scr, &s_zh_14, COLOR_MUTED);
    lv_obj_set_width(s_footer, 192);
    lv_obj_set_style_text_align(s_footer, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_footer, LV_LABEL_LONG_DOT);
    lv_obj_align(s_footer, LV_ALIGN_TOP_MID, 0, 298);

    s_pair = lv_obj_create(scr);
    lv_obj_set_size(s_pair, 200, 132);
    lv_obj_align(s_pair, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_pair, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_border_color(s_pair, lv_color_hex(COLOR_DONE), 0);
    lv_obj_set_style_border_width(s_pair, 2, 0);
    lv_obj_set_style_radius(s_pair, 16, 0);
    lv_obj_set_scrollable(s_pair, false);
    lv_obj_t *pair_title = label(s_pair, &s_zh_14, COLOR_MUTED);
    lv_label_set_text_fmt(pair_title, LV_SYMBOL_BLUETOOTH "  %s", T3_TXT_PAIR_TITLE);
    lv_obj_align(pair_title, LV_ALIGN_TOP_MID, 0, 4);
    s_pair_code = label(s_pair, &lv_font_montserrat_48, COLOR_TEXT);
    lv_obj_align(s_pair_code, LV_ALIGN_CENTER, 0, 4);
    lv_obj_t *pair_hint = label(s_pair, &s_zh_14, COLOR_MUTED);
    lv_label_set_text(pair_hint, T3_TXT_PAIR_HINT);
    lv_obj_align(pair_hint, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_set_hidden(s_pair, true);

    lv_screen_load(scr);
    bsp_lvgl_unlock();
    return true;
}

void t3_ui_screen(bool on)
{
    bsp_display_backlight(on ? BACKLIGHT : 0);
}

void t3_ui_message(const char *title, const char *footer)
{
    if (!bsp_lvgl_lock(LOCK_MS)) return;
    show_home(true);
    state_set(COLOR_FAINT, title);
    card_set(&s_run, "-", COLOR_FAINT, false);
    card_set(&s_wait, "-", COLOR_FAINT, false);
    card_set(&s_done, "--:--", COLOR_FAINT, false);
    lv_label_set_text(s_footer, footer);
    bsp_lvgl_unlock();
}

void t3_ui_status(t3_view_t view, const t3_status_t *s, const char *done, const char *footer, bool just_done)
{
    if (!bsp_lvgl_lock(LOCK_MS)) return;
    bool live = view == T3_VIEW_LIVE;
    show_home(true);
    switch (view) {
    case T3_VIEW_LIVE:
        state_set(s->wait > 0 ? COLOR_WAIT : COLOR_RUN, s->run || s->wait ? T3_TXT_LIVE : T3_TXT_IDLE);
        break;
    case T3_VIEW_T3_OFFLINE:
        state_set(COLOR_WAIT, T3_TXT_T3_OFFLINE);
        break;
    case T3_VIEW_UNREACHABLE:
        state_set(COLOR_ERROR, T3_TXT_UNREACHABLE);
        break;
    case T3_VIEW_CONNECTING:
        state_set(COLOR_FAINT, T3_TXT_CONNECTING);
        break;
    }
    if (s) {
        char n[8];
        snprintf(n, sizeof(n), "%d", s->run);
        card_set(&s_run, n, live && s->run ? COLOR_RUN : (live ? COLOR_TEXT : COLOR_FAINT), false);
        snprintf(n, sizeof(n), "%d", s->wait);
        card_set(&s_wait, n, live && s->wait ? COLOR_WAIT : (live ? COLOR_TEXT : COLOR_FAINT), live && s->wait);
        card_set(&s_done, done, live ? (just_done ? COLOR_DONE : COLOR_TEXT) : COLOR_FAINT, false);
        lv_obj_set_style_border_color(s_done.card, lv_color_hex(COLOR_DONE), 0);
        lv_obj_set_style_border_width(s_done.card, live && just_done ? 2 : 0, 0);
    } else {
        card_set(&s_run, "-", COLOR_FAINT, false);
        card_set(&s_wait, "-", COLOR_FAINT, false);
        card_set(&s_done, "--:--", COLOR_FAINT, false);
    }
    lv_label_set_text(s_footer, footer);
    bsp_lvgl_unlock();
}

static const char *request_label(t3_req_kind_t kind)
{
    switch (kind) {
    case T3_REQ_COMMAND: return T3_TXT_REQ_COMMAND;
    case T3_REQ_FILE_READ: return T3_TXT_REQ_FILE_READ;
    case T3_REQ_FILE_CHANGE: return T3_TXT_REQ_FILE_CHANGE;
    case T3_REQ_PERMISSION: return T3_TXT_REQ_PERMISSION;
    case T3_REQ_QUESTION: return T3_TXT_REQ_QUESTION;
    case T3_REQ_MCP: return T3_TXT_REQ_MCP;
    case T3_REQ_TOOL: return T3_TXT_REQ_TOOL;
    case T3_REQ_AUTH: return T3_TXT_REQ_AUTH;
    case T3_REQ_OTHER: break;
    }
    return T3_TXT_WAIT;
}

void t3_ui_task(int page, int count, const t3_task_t *t, const t3_detail_t *d, const char *elapsed,
                const char *message)
{
    if (!bsp_lvgl_lock(LOCK_MS)) return;
    show_home(false);
    bool show = t != NULL;
    lv_obj_t *const parts[] = {s_task_state, s_task_project, s_task_title, s_task_elapsed, s_task_detail};
    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) lv_obj_set_hidden(parts[i], !show);
    lv_obj_set_hidden(s_task_empty, show);
    if (show) {
        uint32_t color = t->waiting ? COLOR_WAIT : COLOR_RUN;
        char line[24];
        snprintf(line, sizeof(line), T3_TXT_TASK_PAGE, page, count);
        state_set(color, line);
        const char *state = T3_TXT_RUN;
        if (t->waiting) state = d && d->has_request ? request_label(d->request_kind) : T3_TXT_WAIT;
        lv_label_set_text(s_task_state, state);
        lv_obj_set_style_text_color(s_task_state, lv_color_hex(color), 0);
        lv_label_set_text(s_task_elapsed, elapsed);
        lv_label_set_text(s_task_project, t->project);
        lv_label_set_text(s_task_title, t->title[0] ? t->title : "-");
        lv_obj_set_style_border_width(s_task, t->waiting ? 2 : 0, 0);

        // Waiting: what is asked; otherwise the latest reply.
        const char *detail = T3_TXT_DETAIL_LOADING;
        uint32_t detail_color = COLOR_FAINT;
        if (d && t->waiting && d->has_request && d->request[0]) {
            detail = d->request;
            detail_color = COLOR_WAIT;
        } else if (d && d->message[0]) {
            detail = d->message;
            detail_color = COLOR_MUTED;
        } else if (d) {
            detail = T3_TXT_NO_REPLY;
        }
        lv_label_set_text(s_task_detail, detail);
        lv_obj_set_style_text_color(s_task_detail, lv_color_hex(detail_color), 0);

        bool bg = d && d->background > 0;
        lv_obj_set_hidden(s_task_bg, !bg);
        if (bg) {
            char text[sizeof(d->background_text) + 32];
            int n = snprintf(text, sizeof(text), T3_TXT_BACKGROUND, d->background);
            if (d->background_text[0] && n > 0 && (size_t)n < sizeof(text)) {
                snprintf(text + n, sizeof(text) - n, " · %s", d->background_text);
            }
            lv_label_set_text(s_task_bg, text);
        }
        // Whole lines only, above the background line when it is shown.
        int32_t bottom = 208 - 12 - (bg ? s_line_14 + 4 : 0);
        int32_t lines = (bottom - s_detail_y) / s_line_14;
        lv_obj_set_height(s_task_detail, (lines > 0 ? lines : 1) * s_line_14);
    } else {
        lv_obj_set_hidden(s_task_bg, true);
        state_set(COLOR_FAINT, T3_TXT_TASKS);
        lv_label_set_text(s_task_empty, message);
        lv_obj_set_style_border_width(s_task, 0, 0);
    }
    lv_label_set_text(s_footer, T3_TXT_TASK_HINT);
    bsp_lvgl_unlock();
}

void t3_ui_battery(int percent)
{
    if (!bsp_lvgl_lock(LOCK_MS)) return;
    if (percent < 0) lv_label_set_text(s_battery, "");
    else lv_label_set_text_fmt(s_battery, "%d%%", percent);
    bsp_lvgl_unlock();
}

void t3_ui_passkey(uint32_t passkey)
{
    if (!bsp_lvgl_lock(LOCK_MS)) return;
    if (passkey) {
        lv_label_set_text_fmt(s_pair_code, "%06lu", (unsigned long)passkey);
        lv_obj_set_hidden(s_pair, false);
    } else {
        lv_obj_set_hidden(s_pair, true);
    }
    bsp_lvgl_unlock();
}

void t3_ui_link(bool connected)
{
    if (!bsp_lvgl_lock(LOCK_MS)) return;
    lv_label_set_text(s_link, connected ? LV_SYMBOL_BLUETOOTH : "");
    bsp_lvgl_unlock();
}
