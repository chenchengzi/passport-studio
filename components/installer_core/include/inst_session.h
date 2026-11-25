// components/installer_core/include/inst_session.h
// One install: receive the byte stream of a merged image starting at 0x8000,
// write it to the play area, verify it, then commit the patched partition table
// and the journal. Single-threaded: call every function from one worker task.
//
// Stream rules shared with the iOS app:
//   - Bytes arrive strictly in order. DATA and SKIP carry the offset they start
//     at; anything else than the current position is rejected with
//     INST_ERR_SEQ and changes nothing, so the sender rewinds to
//     inst_session_position().
//   - SKIP advances over bytes the sender knows are 0xFF.
//   - SHA-256 covers image bytes [0x8000, end) and is checked by reading the
//     written Flash back before anything is committed.
//
// Reinstalling the same application keeps its data: the "nvs" partition is
// left as it is when the installed and the incoming table place it at the same
// offset with the same size, the incoming image has only 0xFF for it, both
// applications carry the same ESP-IDF project name, and the last install was
// completed. Otherwise it is erased like the rest of the image.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "inst_layout.h"
#include "inst_state.h"
#include "inst_table.h"

typedef struct {
    void *ctx;
    bool (*erase)(void *ctx, uint32_t addr, uint32_t len);   // 4 KB aligned
    bool (*write)(void *ctx, uint32_t addr, const void *data, uint32_t len);
    bool (*read)(void *ctx, uint32_t addr, void *data, uint32_t len);
    inst_md5_fn md5;
    void (*sha256)(void *ctx, int step, const void *data, size_t len, uint8_t out[32]);
    // Optional: check the application image at offset fits max_size and is intact.
    bool (*verify_app)(void *ctx, uint32_t offset, uint32_t max_size);
} inst_ops_t;

// esp_app_desc_t inside an application image: after the 24-byte image header
// and the first 8-byte segment header.
#define INST_APP_DESC_OFFSET   0x20u
#define INST_APP_DESC_MAGIC    0xABCD5432u
#define INST_APP_PROJECT_AT    0x30u      // project_name within esp_app_desc_t
#define INST_APP_PROJECT_MAX   32u

// sha256 steps
#define INST_SHA_START  0
#define INST_SHA_UPDATE 1
#define INST_SHA_FINISH 2

typedef enum {
    INST_IDLE = 0,
    INST_RECEIVING,
    INST_DONE,
    INST_FAILED,
} inst_phase_t;

typedef enum {
    INST_OK = 0,
    INST_ERR_STATE,     // command not valid in the current phase
    INST_ERR_SIZE,      // image end outside the play area, or data past the end
    INST_ERR_SEQ,       // offset differs from the current position (recoverable)
    INST_ERR_TABLE,     // partition table rejected; see table_err
    INST_ERR_FLASH,     // erase/write/read failed
    INST_ERR_VERIFY,    // SHA-256 of written data differs
    INST_ERR_IMAGE,     // application image invalid or larger than its partition
} inst_err_t;

typedef struct {
    inst_ops_t ops;
    inst_phase_t phase;
    inst_err_t error;
    inst_table_err_t table_err;
    uint32_t end;
    uint32_t pos;
    uint32_t erased_to;
    bool planned;
    bool journal_set;
    uint8_t sha256[32];
    uint8_t name_len;
    uint8_t name[INST_NAME_MAX];
    uint32_t page_base;          // Flash sector being assembled
    uint32_t page_len;
    uint8_t page[0x1000];
    uint8_t table_sector[INST_TABLE_SECTOR];
    inst_table_plan_t plan;
    // The installed application's data, read before the first play-area write.
    bool old_known;              // old_nvs* still hold that application's data
    uint32_t old_nvs, old_nvs_size;
    char old_project[INST_APP_PROJECT_MAX + 1];
    bool keep;                   // this stream leaves old_nvs untouched so far
    bool kept;                   // the committed install kept it
} inst_session_t;

void inst_session_init(inst_session_t *s, const inst_ops_t *ops);

// Start, or resume when an unfinished session has the same end and SHA-256.
inst_err_t inst_session_begin(inst_session_t *s, uint32_t end, const uint8_t sha256[32],
                              const uint8_t *name, size_t name_len, bool *resumed);
inst_err_t inst_session_data(inst_session_t *s, uint32_t offset, const uint8_t *data, size_t len);
inst_err_t inst_session_skip(inst_session_t *s, uint32_t offset, uint32_t to);
inst_err_t inst_session_commit(inst_session_t *s);
void inst_session_abort(inst_session_t *s);

uint32_t inst_session_position(const inst_session_t *s);

// Journal helpers, also used by the installer UI at startup.
bool inst_session_read_state(const inst_ops_t *ops, inst_state_t *state);
bool inst_session_write_state(const inst_ops_t *ops, const inst_state_t *state);

const char *inst_err_name(inst_err_t err);
