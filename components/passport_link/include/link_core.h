// components/passport_link/include/link_core.h
// Pure C parts of Passport Link: frame reassembly, frame splitting, and the
// settings schema with its validation. No ESP-IDF headers; host-tested by
// tests/test_passport_link.c and mirrored in
// ios/PassportKit/Sources/PassportKit/LinkProtocol.swift.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LINK_PROTOCOL_VERSION  1
#define LINK_MSG_MAX           4096u
#define LINK_INFO_MAX          512u
#define LINK_KEY_MAX           15u      // NVS key length
#define LINK_FLAG_FIRST        0x01u
#define LINK_FLAG_LAST         0x02u

// ---------------------------------------------------------------------------
// Frames
// ---------------------------------------------------------------------------
typedef struct {
    uint8_t *buf;     // cap bytes; one more than LINK_MSG_MAX for the terminator
    size_t cap;
    size_t len;
    bool active;      // a first frame was seen and no last frame yet
    bool overflow;    // the current message exceeded cap - 1 and is being skipped
} link_rx_t;

typedef enum {
    LINK_RX_PARTIAL = 0,   // keep feeding
    LINK_RX_COMPLETE,      // buf[0..len) holds a message, NUL-terminated
    LINK_RX_DROPPED,       // malformed, out of order, or too large; state reset
} link_rx_result_t;

void link_rx_init(link_rx_t *rx, uint8_t *buf, size_t cap);
link_rx_result_t link_rx_feed(link_rx_t *rx, const uint8_t *frame, size_t len);

// Write the frame of `msg` that starts at *offset into out (max_frame bytes,
// at least 2) and advance *offset. Returns the frame length, or 0 when the
// whole message has been framed. Messages are never empty.
size_t link_tx_next(const uint8_t *msg, size_t len, size_t *offset, size_t max_frame, uint8_t *out);

// ---------------------------------------------------------------------------
// Settings schema
// ---------------------------------------------------------------------------
typedef enum {
    PASSPORT_LINK_BOOL = 0,
    PASSPORT_LINK_INT,
    PASSPORT_LINK_ENUM,
    PASSPORT_LINK_TEXT,
} passport_link_type_t;

typedef struct {
    const char *key;               // at most LINK_KEY_MAX characters
    const char *label;             // shown on the phone, any language
    passport_link_type_t type;
    int32_t def_int;               // bool / int default
    int32_t min, max;              // int: range; text: max is the byte limit
    const char *def_str;           // enum / text default
    const char *const *options;    // enum choices
    uint8_t option_count;
} passport_link_setting_t;

#define PASSPORT_LINK_BOOL_SETTING(k, l, def) \
    {.key = (k), .label = (l), .type = PASSPORT_LINK_BOOL, .def_int = (def), .min = 0, .max = 1}
#define PASSPORT_LINK_INT(k, l, def, lo, hi) \
    {.key = (k), .label = (l), .type = PASSPORT_LINK_INT, .def_int = (def), .min = (lo), .max = (hi)}
#define PASSPORT_LINK_TEXT(k, l, def, max_bytes) \
    {.key = (k), .label = (l), .type = PASSPORT_LINK_TEXT, .def_str = (def), .max = (max_bytes)}
#define PASSPORT_LINK_ENUM(k, l, def, ...)                                              \
    {.key = (k), .label = (l), .type = PASSPORT_LINK_ENUM, .def_str = (def),           \
     .options = (const char *const[]){__VA_ARGS__},                                     \
     .option_count = sizeof((const char *const[]){__VA_ARGS__}) / sizeof(const char *)}

int link_setting_find(const passport_link_setting_t *settings, size_t count, const char *key);
bool link_setting_int_ok(const passport_link_setting_t *s, int32_t value);
bool link_setting_str_ok(const passport_link_setting_t *s, const char *value);

// Keys valid and unique, defaults pass their own checks.
bool link_schema_valid(const passport_link_setting_t *settings, size_t count);
