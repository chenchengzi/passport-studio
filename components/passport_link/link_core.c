// components/passport_link/link_core.c
#include "link_core.h"

#include <string.h>

void link_rx_init(link_rx_t *rx, uint8_t *buf, size_t cap)
{
    rx->buf = buf;
    rx->cap = cap;
    rx->len = 0;
    rx->active = false;
    rx->overflow = false;
}

link_rx_result_t link_rx_feed(link_rx_t *rx, const uint8_t *frame, size_t len)
{
    if (len == 0 || (frame[0] & ~(LINK_FLAG_FIRST | LINK_FLAG_LAST))) {
        rx->active = false;
        return LINK_RX_DROPPED;
    }
    uint8_t flags = frame[0];
    if (flags & LINK_FLAG_FIRST) {
        rx->len = 0;
        rx->active = true;
        rx->overflow = false;
    } else if (!rx->active) {
        return LINK_RX_DROPPED;   // continuation without a start
    }
    size_t n = len - 1;
    if (!rx->overflow && rx->len + n > rx->cap - 1) rx->overflow = true;
    if (!rx->overflow) {
        memcpy(rx->buf + rx->len, frame + 1, n);
        rx->len += n;
    }
    if (!(flags & LINK_FLAG_LAST)) return LINK_RX_PARTIAL;
    rx->active = false;
    if (rx->overflow || rx->len == 0) {
        rx->len = 0;
        return LINK_RX_DROPPED;
    }
    rx->buf[rx->len] = '\0';
    return LINK_RX_COMPLETE;
}

size_t link_tx_next(const uint8_t *msg, size_t len, size_t *offset, size_t max_frame, uint8_t *out)
{
    if (max_frame < 2 || *offset >= len) return 0;
    size_t n = len - *offset;
    if (n > max_frame - 1) n = max_frame - 1;
    out[0] = (*offset == 0 ? LINK_FLAG_FIRST : 0) | (*offset + n == len ? LINK_FLAG_LAST : 0);
    memcpy(out + 1, msg + *offset, n);
    *offset += n;
    return n + 1;
}

int link_setting_find(const passport_link_setting_t *settings, size_t count, const char *key)
{
    for (size_t i = 0; i < count; i++) {
        if (!strcmp(settings[i].key, key)) return (int)i;
    }
    return -1;
}

bool link_setting_int_ok(const passport_link_setting_t *s, int32_t value)
{
    switch (s->type) {
    case PASSPORT_LINK_BOOL: return value == 0 || value == 1;
    case PASSPORT_LINK_INT: return value >= s->min && value <= s->max;
    default: return false;
    }
}

bool link_setting_str_ok(const passport_link_setting_t *s, const char *value)
{
    if (!value) return false;
    switch (s->type) {
    case PASSPORT_LINK_ENUM:
        for (uint8_t i = 0; i < s->option_count; i++) {
            if (!strcmp(s->options[i], value)) return true;
        }
        return false;
    case PASSPORT_LINK_TEXT:
        return strlen(value) <= (size_t)s->max;
    default:
        return false;
    }
}

bool link_schema_valid(const passport_link_setting_t *settings, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        const passport_link_setting_t *s = &settings[i];
        size_t key_len = s->key ? strlen(s->key) : 0;
        if (key_len == 0 || key_len > LINK_KEY_MAX || !s->label) return false;
        if (link_setting_find(settings, i, s->key) >= 0) return false;   // duplicate
        switch (s->type) {
        case PASSPORT_LINK_BOOL:
        case PASSPORT_LINK_INT:
            if (s->min > s->max || !link_setting_int_ok(s, s->def_int)) return false;
            break;
        case PASSPORT_LINK_ENUM:
            if (s->option_count == 0 || !link_setting_str_ok(s, s->def_str)) return false;
            break;
        case PASSPORT_LINK_TEXT:
            if (s->max <= 0 || !link_setting_str_ok(s, s->def_str ? s->def_str : "")) return false;
            break;
        default:
            return false;
        }
    }
    return true;
}
