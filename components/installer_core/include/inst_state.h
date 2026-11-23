// components/installer_core/include/inst_state.h
// Install journal stored in the INST_STATE_OFFSET sector. Header-only so the
// bootloader hook can use it without linking the installer component.
//
// Power-loss rules:
//   - The record is written with INST_STATE_PENDING before the first byte of the
//     play area is erased, and rewritten without it only after the new partition
//     table has been written and read back.
//   - An erased sector means "no installer record": the play area was last
//     written by USB or by a completed install, so it boots normally.
//   - A record whose magic matches but whose CRC fails is treated as pending,
//     because a torn rewrite can only happen around an install.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define INST_STATE_MAGIC     0x31534950u   // "PIS1"
#define INST_STATE_PENDING   0x00000001u
#define INST_NAME_MAX        63u

typedef struct {
    uint32_t magic;
    uint32_t flags;
    uint32_t image_end;          // end offset of the last installed image
    uint8_t sha256[32];          // SHA-256 of image bytes [0x8000, image_end)
    uint8_t name_len;            // UTF-8 bytes in name, not NUL-terminated
    uint8_t name[INST_NAME_MAX];
    uint32_t crc32;              // over every preceding byte
} inst_state_t;

typedef enum {
    INST_STATE_BLANK = 0,        // erased sector
    INST_STATE_VALID,
    INST_STATE_CORRUPT,
} inst_state_status_t;

static inline uint32_t inst_crc32(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

static inline void inst_state_seal(inst_state_t *state)
{
    state->magic = INST_STATE_MAGIC;
    state->crc32 = inst_crc32(state, offsetof(inst_state_t, crc32));
}

static inline inst_state_status_t inst_state_check(const inst_state_t *state)
{
    if (state->magic != INST_STATE_MAGIC) {
        const uint8_t *raw = (const uint8_t *)state;
        for (size_t i = 0; i < sizeof(*state); i++) {
            if (raw[i] != 0xFF) return INST_STATE_CORRUPT;
        }
        return INST_STATE_BLANK;
    }
    if (state->crc32 != inst_crc32(state, offsetof(inst_state_t, crc32)) ||
        state->name_len > INST_NAME_MAX) {
        return INST_STATE_CORRUPT;
    }
    return INST_STATE_VALID;
}

// True when the bootloader must start the installer instead of the play.
static inline bool inst_state_requires_installer(const inst_state_t *state)
{
    inst_state_status_t status = inst_state_check(state);
    if (status == INST_STATE_BLANK) return false;
    if (status == INST_STATE_CORRUPT) return true;
    return (state->flags & INST_STATE_PENDING) != 0;
}
