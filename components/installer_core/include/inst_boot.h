// components/installer_core/include/inst_boot.h
// Pure boot decisions used by the bootloader hook. Header-only and free of
// ESP-IDF dependencies so the host tests can exercise them directly.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define INST_ENTRY_SIZE        32u
#define INST_ENTRY_MAGIC_0     0xAAu
#define INST_ENTRY_MAGIC_1     0x50u
#define INST_IMAGE_MAGIC       0xE9u

// Key hold detection at power-on. The three function keys share GPIO0 through
// a resistor ladder and all read as a digital low, so "any key" is detected.
// A key must already be down within the first window and then stay down for
// the hold time; a released pad boots the play without delay.
#define INST_HOLD_WINDOW_MS    100u
#define INST_HOLD_TIME_MS      300u
#define INST_HOLD_RELEASE_MS   30u    // contact-bounce tolerance while held

typedef enum {
    INST_HOLD_SAMPLING = 0,
    INST_HOLD_ENTER,
    INST_HOLD_BOOT,
} inst_hold_result_t;

typedef struct {
    uint32_t started;
    uint32_t pressed_at;
    uint32_t released_at;
    bool pressing;
    bool releasing;
} inst_hold_t;

static inline void inst_hold_init(inst_hold_t *h, uint32_t now_ms)
{
    *h = (inst_hold_t){ .started = now_ms };
}

static inline inst_hold_result_t inst_hold_sample(inst_hold_t *h, uint32_t now_ms, bool pressed)
{
    if (pressed) {
        if (!h->pressing) {
            h->pressing = true;
            h->pressed_at = now_ms;
        }
        h->releasing = false;
        if (now_ms - h->pressed_at >= INST_HOLD_TIME_MS) return INST_HOLD_ENTER;
        return INST_HOLD_SAMPLING;
    }
    if (h->pressing) {
        if (!h->releasing) {
            h->releasing = true;
            h->released_at = now_ms;
        }
        if (now_ms - h->released_at < INST_HOLD_RELEASE_MS) return INST_HOLD_SAMPLING;
        return INST_HOLD_BOOT;
    }
    return now_ms - h->started >= INST_HOLD_WINDOW_MS ? INST_HOLD_BOOT : INST_HOLD_SAMPLING;
}

static inline uint32_t inst_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

// Offset of the partition the stock bootloader will try first: the factory app,
// or ota_0 when there is no factory app. Returns 0 when the table has neither.
// The table is not authenticated here; the bootloader verifies the image anyway.
static inline uint32_t inst_boot_app_offset(const uint8_t *table, uint32_t table_size)
{
    uint32_t ota0 = 0;
    for (uint32_t at = 0; at + INST_ENTRY_SIZE <= table_size; at += INST_ENTRY_SIZE) {
        const uint8_t *e = table + at;
        if (e[0] != INST_ENTRY_MAGIC_0 || e[1] != INST_ENTRY_MAGIC_1) break;
        if (e[2] != 0x00) continue;
        if (e[3] == 0x00) return inst_le32(e + 4);
        if (e[3] == 0x10 && ota0 == 0) ota0 = inst_le32(e + 4);
    }
    return ota0;
}
