// components/passport_link/include/passport_link.h
// Bluetooth channel from the iOS app to a running application: settings
// generated from a schema, and pushed content such as location and time.
// See docs/development/engineering/passport-link.md.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"
#include "link_core.h"

typedef struct {
    const char *app_id;                  // stable identifier, e.g. "wifi-weather"
    const char *display_name;            // advertised name, ASCII, at most 20 bytes
    const char *version;                 // optional; defaults to the app description
    const char *const *channels;         // channels the phone may push
    size_t channel_count;
    const passport_link_setting_t *settings;
    size_t setting_count;

    // All callbacks run on the component's task. They may block briefly but
    // must take bsp_lvgl_lock() before touching LVGL objects.
    void (*on_push)(const char *channel, const cJSON *data, void *user);
    void (*on_settings)(void *user);                  // one or more values changed
    void (*on_passkey)(uint32_t passkey, void *user); // show it; 0 = pairing finished
    void (*on_connection)(bool connected, void *user);
    void *user;
} passport_link_config_t;

// Requires nvs_flash_init() to have succeeded and Bluetooth (NimBLE,
// peripheral role, NVS persistence) enabled in sdkconfig. Starts advertising.
// The config and everything it points to must stay valid.
esp_err_t passport_link_start(const passport_link_config_t *config);

// True while a phone is connected over an encrypted link.
bool passport_link_connected(void);

// Ask the connected phone to push a channel now. ESP_ERR_INVALID_STATE when
// no phone is connected or it did not subscribe.
esp_err_t passport_link_request(const char *channel);

// Current setting values (stored in NVS), or the schema default.
int32_t passport_link_get_int(const char *key);
bool passport_link_get_bool(const char *key);
void passport_link_get_str(const char *key, char *out, size_t cap);

// Offset of local time from UTC in minutes, from the last "time" push
// (persisted); 0 before the first one.
int32_t passport_link_tz_offset_min(void);
