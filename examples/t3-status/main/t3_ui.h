// examples/t3-status/main/t3_ui.h
// The status screen. Every function takes bsp_lvgl_lock() itself and may be
// called from any task.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "t3_logic.h"

bool t3_ui_init(void);
void t3_ui_screen(bool on);
void t3_ui_flash(t3_flash_kind_t kind);   // border color, or hidden for T3_FLASH_NONE
void t3_ui_battery(int percent);          // -1 hides it
void t3_ui_link(bool connected);          // phone connected over Passport Link
void t3_ui_passkey(uint32_t passkey);     // 0 hides the pairing card

// Setup states and the bridge result below show the home screen.
// Setup states before any status is available (Wi-Fi, missing address):
// a neutral status line, dashes in the cards, and a hint in the footer.
void t3_ui_message(const char *title, const char *footer);

// Bridge result. s is the last good status (NULL when none); its counts are
// shown dimmed unless view is T3_VIEW_LIVE. done is t3_format_done() text;
// just_done highlights it after a run finished.
void t3_ui_status(t3_view_t view, const t3_status_t *s, const char *done, const char *footer, bool just_done);

// Task page `page` of `count`: state, project, title, elapsed text, and the
// request or latest reply from d (NULL while it loads). t NULL shows an empty
// page with message instead.
void t3_ui_task(int page, int count, const t3_task_t *t, const t3_detail_t *d, const char *elapsed,
                const char *message);
