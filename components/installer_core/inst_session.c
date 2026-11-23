// components/installer_core/inst_session.c
#include "inst_session.h"

#include <string.h>

#define SECTOR      0x1000u
#define BLOCK       0x10000u
#define READ_CHUNK  0x400u

static uint32_t round_up(uint32_t v, uint32_t align)
{
    return (v + align - 1) / align * align;
}

static void fail(inst_session_t *s, inst_err_t err)
{
    s->phase = INST_FAILED;
    s->error = err;
}

bool inst_session_read_state(const inst_ops_t *ops, inst_state_t *state)
{
    return ops->read(ops->ctx, INST_STATE_OFFSET, state, sizeof(*state));
}

bool inst_session_write_state(const inst_ops_t *ops, const inst_state_t *state)
{
    inst_state_t check;
    return ops->erase(ops->ctx, INST_STATE_OFFSET, INST_STATE_SIZE) &&
           ops->write(ops->ctx, INST_STATE_OFFSET, state, sizeof(*state)) &&
           ops->read(ops->ctx, INST_STATE_OFFSET, &check, sizeof(check)) &&
           memcmp(&check, state, sizeof(check)) == 0;
}

static void fill_state(const inst_session_t *s, inst_state_t *state, uint32_t flags)
{
    memset(state, 0, sizeof(*state));
    state->flags = flags;
    state->image_end = s->end;
    memcpy(state->sha256, s->sha256, sizeof(state->sha256));
    state->name_len = s->name_len;
    memcpy(state->name, s->name, s->name_len);
    inst_state_seal(state);
}

// Erase lazily in front of the write position. 64 KB blocks are used only when
// the whole block lies inside the image, so data after the image is preserved
// exactly as a USB flash of the same merged file would preserve it.
static bool ensure_erased(inst_session_t *s, uint32_t addr)
{
    uint32_t limit = round_up(s->end, SECTOR);
    if (addr > limit) addr = limit;
    while (s->erased_to < addr) {
        uint32_t len = (s->erased_to % BLOCK == 0 && s->erased_to + BLOCK <= limit) ? BLOCK : SECTOR;
        if (!s->ops.erase(s->ops.ctx, s->erased_to, len)) return false;
        s->erased_to += len;
    }
    return true;
}

static bool flush_page(inst_session_t *s)
{
    if (s->page_len == 0) return true;
    if (!ensure_erased(s, s->page_base + s->page_len) ||
        !s->ops.write(s->ops.ctx, s->page_base, s->page, s->page_len)) {
        return false;
    }
    s->page_len = 0;
    return true;
}

// Called once the table sector is complete and before any play-area write.
static inst_err_t plan_and_journal(inst_session_t *s)
{
    inst_table_err_t err = inst_table_plan(s->table_sector, s->end, s->ops.md5, &s->plan);
    if (err != INST_TABLE_OK) {
        s->table_err = err;
        return INST_ERR_TABLE;
    }
    s->planned = true;
    if (!s->journal_set) {
        inst_state_t state;
        fill_state(s, &state, INST_STATE_PENDING);
        if (!inst_session_write_state(&s->ops, &state)) return INST_ERR_FLASH;
        s->journal_set = true;
    }
    return INST_OK;
}

static void reset_stream(inst_session_t *s)
{
    s->pos = INST_STREAM_START;
    s->erased_to = INST_PLAY_START;
    s->planned = false;
    s->page_len = 0;
    s->page_base = INST_PLAY_START;
    s->table_err = INST_TABLE_OK;
    memset(s->table_sector, 0xFF, sizeof(s->table_sector));
}

void inst_session_init(inst_session_t *s, const inst_ops_t *ops)
{
    memset(s, 0, sizeof(*s));
    s->ops = *ops;
    s->phase = INST_IDLE;
    reset_stream(s);
}

uint32_t inst_session_position(const inst_session_t *s)
{
    return s->pos;
}

inst_err_t inst_session_begin(inst_session_t *s, uint32_t end, const uint8_t sha256[32],
                              const uint8_t *name, size_t name_len, bool *resumed)
{
    *resumed = false;
    if (end <= INST_PLAY_START || end > INST_PLAY_LIMIT) return INST_ERR_SIZE;
    if (name_len > INST_NAME_MAX) name_len = INST_NAME_MAX;
    if (s->phase == INST_RECEIVING && s->end == end && !memcmp(s->sha256, sha256, 32)) {
        *resumed = true;
        return INST_OK;
    }
    // Keep journal_set: an earlier session may already have touched the play area.
    s->phase = INST_RECEIVING;
    s->error = INST_OK;
    s->end = end;
    memcpy(s->sha256, sha256, 32);
    s->name_len = (uint8_t)name_len;
    memcpy(s->name, name, name_len);
    reset_stream(s);
    return INST_OK;
}

static inst_err_t append(inst_session_t *s, const uint8_t *data, uint32_t len)
{
    while (len) {
        if (s->pos < INST_PLAY_START) {
            uint32_t n = INST_PLAY_START - s->pos;
            if (n > len) n = len;
            memcpy(s->table_sector + (s->pos - INST_TABLE_OFFSET), data, n);
            s->pos += n;
            data += n;
            len -= n;
            continue;
        }
        if (!s->planned) {
            inst_err_t err = plan_and_journal(s);
            if (err != INST_OK) return err;
        }
        if (s->page_len == 0) s->page_base = s->pos;
        uint32_t room = SECTOR - (s->pos % SECTOR);
        uint32_t n = room < len ? room : len;
        memcpy(s->page + s->page_len, data, n);
        s->page_len += n;
        s->pos += n;
        data += n;
        len -= n;
        if (s->pos % SECTOR == 0 && !flush_page(s)) return INST_ERR_FLASH;
    }
    return INST_OK;
}

inst_err_t inst_session_data(inst_session_t *s, uint32_t offset, const uint8_t *data, size_t len)
{
    if (s->phase != INST_RECEIVING) return INST_ERR_STATE;
    if (offset != s->pos) return INST_ERR_SEQ;
    if (len == 0 || len > s->end - s->pos) return INST_ERR_SIZE;
    inst_err_t err = append(s, data, (uint32_t)len);
    if (err != INST_OK) fail(s, err);
    return err;
}

inst_err_t inst_session_skip(inst_session_t *s, uint32_t offset, uint32_t to)
{
    if (s->phase != INST_RECEIVING) return INST_ERR_STATE;
    if (offset != s->pos) return INST_ERR_SEQ;
    if (to <= s->pos || to > s->end) return INST_ERR_SIZE;
    inst_err_t err = INST_OK;
    if (s->pos < INST_PLAY_START) {
        // The table buffer is already 0xFF; just move past it.
        s->pos = to < INST_PLAY_START ? to : INST_PLAY_START;
    }
    if (s->pos < to) {
        if (!s->planned) err = plan_and_journal(s);
        if (err == INST_OK && !flush_page(s)) err = INST_ERR_FLASH;
        if (err == INST_OK && !ensure_erased(s, to)) err = INST_ERR_FLASH;
        if (err == INST_OK) s->pos = to;
    }
    if (err != INST_OK) fail(s, err);
    return err;
}

static bool readback_matches(inst_session_t *s)
{
    uint8_t buf[READ_CHUNK];
    uint8_t digest[32];
    s->ops.sha256(s->ops.ctx, INST_SHA_START, NULL, 0, NULL);
    s->ops.sha256(s->ops.ctx, INST_SHA_UPDATE, s->table_sector, sizeof(s->table_sector), NULL);
    for (uint32_t at = INST_PLAY_START; at < s->end; at += READ_CHUNK) {
        uint32_t n = s->end - at < READ_CHUNK ? s->end - at : READ_CHUNK;
        if (!s->ops.read(s->ops.ctx, at, buf, n)) return false;
        s->ops.sha256(s->ops.ctx, INST_SHA_UPDATE, buf, n, NULL);
    }
    s->ops.sha256(s->ops.ctx, INST_SHA_FINISH, NULL, 0, digest);
    return memcmp(digest, s->sha256, sizeof(digest)) == 0;
}

static bool write_table(inst_session_t *s)
{
    uint8_t check[INST_TABLE_MAX];
    return s->ops.erase(s->ops.ctx, INST_TABLE_OFFSET, INST_TABLE_SECTOR) &&
           s->ops.write(s->ops.ctx, INST_TABLE_OFFSET, s->plan.table, INST_TABLE_MAX) &&
           s->ops.read(s->ops.ctx, INST_TABLE_OFFSET, check, sizeof(check)) &&
           memcmp(check, s->plan.table, sizeof(check)) == 0;
}

inst_err_t inst_session_commit(inst_session_t *s)
{
    if (s->phase != INST_RECEIVING) return INST_ERR_STATE;
    if (s->pos != s->end || !s->planned) return INST_ERR_SIZE;
    inst_err_t err = INST_OK;
    if (!flush_page(s) || !ensure_erased(s, s->end)) {
        err = INST_ERR_FLASH;
    } else if (!readback_matches(s)) {
        err = INST_ERR_VERIFY;
    } else if (s->ops.verify_app &&
               !s->ops.verify_app(s->ops.ctx, s->plan.boot_offset, s->plan.boot_size)) {
        err = INST_ERR_IMAGE;
    } else if (!write_table(s)) {
        err = INST_ERR_FLASH;
    } else {
        inst_state_t state;
        fill_state(s, &state, 0);
        // The table is already committed; a failed record rewrite leaves the
        // pending journal, which only makes the next boot open the installer.
        if (!inst_session_write_state(&s->ops, &state)) err = INST_ERR_FLASH;
        else s->journal_set = false;
    }
    if (err != INST_OK) {
        fail(s, err);
        return err;
    }
    s->phase = INST_DONE;
    return INST_OK;
}

void inst_session_abort(inst_session_t *s)
{
    if (s->phase == INST_RECEIVING) s->phase = INST_IDLE;
    reset_stream(s);
}

const char *inst_err_name(inst_err_t err)
{
    switch (err) {
    case INST_OK: return "ok";
    case INST_ERR_STATE: return "wrong state";
    case INST_ERR_SIZE: return "bad size";
    case INST_ERR_SEQ: return "out of sequence";
    case INST_ERR_TABLE: return "partition table rejected";
    case INST_ERR_FLASH: return "flash error";
    case INST_ERR_VERIFY: return "verification failed";
    case INST_ERR_IMAGE: return "invalid application";
    }
    return "unknown";
}
