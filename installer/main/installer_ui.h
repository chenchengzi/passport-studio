// installer/main/installer_ui.h
// Installer screens. Every function takes the LVGL lock itself and may be
// called from any task except the LVGL task.
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool app_bootable;       // OK long-press can start the installed application
    bool interrupted;        // an install did not finish
    uint32_t installed_size; // bytes of the last installed image, 0 when unknown
} installer_ui_ready_t;

bool installer_ui_init(const char *device_name);
void installer_ui_ready(const installer_ui_ready_t *ready);
void installer_ui_pairing(uint32_t passkey);
void installer_ui_connected(void);
void installer_ui_progress(uint32_t done, uint32_t total);
void installer_ui_verifying(void);
void installer_ui_done(void);
void installer_ui_error(const char *detail);
void installer_ui_notice(const char *text);   // transient footer message
void installer_ui_battery(int percent);       // -1 hides the reading
