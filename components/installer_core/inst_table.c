// components/installer_core/inst_table.c
#include "inst_table.h"

#include <stdbool.h>
#include <string.h>

#include "inst_boot.h"

#define MAX_ENTRIES     (INST_TABLE_MAX / INST_ENTRY_SIZE - 1)   // one slot for MD5
#define LABEL_SIZE      16u
#define FLAG_ENCRYPTED  0x1u

typedef struct {
    uint8_t raw[INST_ENTRY_SIZE];
    uint8_t type, subtype;
    uint32_t offset, size, flags;
    char label[LABEL_SIZE + 1];
} entry_t;

static void put32(uint8_t *p, uint32_t v)
{
    for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static bool overlaps(uint32_t a, uint32_t a_size, uint32_t b, uint32_t b_size)
{
    return a < b + b_size && b < a + a_size;
}

static void encode(uint8_t *out, uint8_t type, uint8_t subtype, uint32_t offset,
                   uint32_t size, const char *label)
{
    memset(out, 0, INST_ENTRY_SIZE);
    out[0] = INST_ENTRY_MAGIC_0;
    out[1] = INST_ENTRY_MAGIC_1;
    out[2] = type;
    out[3] = subtype;
    put32(out + 4, offset);
    put32(out + 8, size);
    memcpy(out + 12, label, strlen(label));
}

static bool is_reserved_label(const char *label)
{
    return !strcmp(label, INST_STATE_LABEL) || !strcmp(label, INST_APP_LABEL) ||
           !strcmp(label, INST_WIFI_LABEL);
}

// Images built from this template (any revision) already describe the
// installer area; those entries are re-appended below in canonical form.
static bool is_installer_entry(const entry_t *e)
{
    return is_reserved_label(e->label) && e->offset >= INST_PLAY_LIMIT;
}

static bool decode(const uint8_t *raw, entry_t *e)
{
    memcpy(e->raw, raw, INST_ENTRY_SIZE);
    e->type = raw[2];
    e->subtype = raw[3];
    e->offset = inst_le32(raw + 4);
    e->size = inst_le32(raw + 8);
    e->flags = inst_le32(raw + 28);
    if (!memchr(raw + 12, 0, LABEL_SIZE)) return false;   // label must be NUL-terminated
    memcpy(e->label, raw + 12, LABEL_SIZE);
    e->label[LABEL_SIZE] = '\0';
    if (!e->label[0] || (e->flags & FLAG_ENCRYPTED)) return false;
    if (e->offset < INST_PLAY_START || e->offset % 0x1000 || e->size == 0 ||
        e->size % 0x1000 || e->size > INST_FLASH_SIZE - e->offset) {
        return false;
    }
    if (e->type == 0x00 && e->offset % 0x10000) return false;
    return true;
}

inst_table_err_t inst_table_plan(const uint8_t *image_table, uint32_t image_end,
                                 inst_md5_fn md5, inst_table_plan_t *plan)
{
    // Static: about 6 KB, too large for a task stack. Only the installer worker calls this.
    static entry_t entries[MAX_ENTRIES];
    unsigned count = 0;

    memset(plan, 0, sizeof(*plan));
    memset(plan->table, 0xFF, sizeof(plan->table));
    if (image_end > INST_PLAY_LIMIT) return INST_TABLE_ERR_IMAGE_END;

    for (uint32_t at = 0; at + INST_ENTRY_SIZE <= INST_TABLE_MAX; at += INST_ENTRY_SIZE) {
        const uint8_t *raw = image_table + at;
        if (raw[0] == 0xEB && raw[1] == 0xEB) {
            uint8_t digest[16];
            md5(image_table, at, digest);
            if (memcmp(digest, raw + 16, sizeof(digest))) return INST_TABLE_ERR_FORMAT;
            break;
        }
        if (raw[0] == 0xFF && raw[1] == 0xFF) break;
        if (raw[0] != INST_ENTRY_MAGIC_0 || raw[1] != INST_ENTRY_MAGIC_1) {
            return INST_TABLE_ERR_FORMAT;
        }
        if (count == MAX_ENTRIES) return INST_TABLE_ERR_FULL;
        if (!decode(raw, &entries[count])) return INST_TABLE_ERR_ENTRY;
        count++;
    }
    // Tables built without an MD5 entry are accepted; the output always has one.
    if (count == 0) return INST_TABLE_ERR_FORMAT;

    for (unsigned i = 0; i < count; i++) {
        for (unsigned j = 0; j < i; j++) {
            if (!strcmp(entries[i].label, entries[j].label) ||
                overlaps(entries[i].offset, entries[i].size, entries[j].offset, entries[j].size)) {
                return INST_TABLE_ERR_OVERLAP;
            }
        }
    }

    unsigned out = 0;
    uint32_t ota0_offset = 0, ota0_size = 0;
    bool factory = false;
    for (unsigned i = 0; i < count; i++) {
        entry_t *e = &entries[i];
        if (is_installer_entry(e)) continue;
        if (is_reserved_label(e->label)) return INST_TABLE_ERR_RESERVED;
        if (e->type == INST_APP_TYPE && e->subtype == INST_APP_SUBTYPE) {
            return INST_TABLE_ERR_RESERVED;
        }
        uint32_t size = e->size;
        if (e->offset + size > INST_PLAY_LIMIT) {
            if (e->offset >= INST_PLAY_LIMIT) return INST_TABLE_ERR_RESERVED;
            // Applications are verified against their real length before the
            // table is committed. A data partition may only shrink when the
            // image carries no prebuilt content for it, since filesystem images
            // record their own size.
            if (e->type != 0x00 && image_end > e->offset) return INST_TABLE_ERR_RESERVED;
            size = INST_PLAY_LIMIT - e->offset;
            plan->clipped++;
        }
        if (e->type == 0x00 && e->subtype == 0x00) {
            factory = true;
            plan->boot_offset = e->offset;
            plan->boot_size = size;
        } else if (e->type == 0x00 && e->subtype == 0x10) {
            ota0_offset = e->offset;
            ota0_size = size;
        }
        memcpy(plan->table + out * INST_ENTRY_SIZE, e->raw, INST_ENTRY_SIZE);
        put32(plan->table + out * INST_ENTRY_SIZE + 8, size);
        out++;
    }
    if (!factory) {
        if (!ota0_offset) return INST_TABLE_ERR_NO_APP;
        plan->boot_offset = ota0_offset;
        plan->boot_size = ota0_size;
    }
    if (image_end <= plan->boot_offset) return INST_TABLE_ERR_NO_APP;
    if (out + 3 > MAX_ENTRIES) return INST_TABLE_ERR_FULL;
    plan->entries = (uint8_t)out;

    encode(plan->table + out++ * INST_ENTRY_SIZE, INST_STATE_TYPE, INST_STATE_SUBTYPE,
           INST_STATE_OFFSET, INST_STATE_SIZE, INST_STATE_LABEL);
    encode(plan->table + out++ * INST_ENTRY_SIZE, INST_APP_TYPE, INST_APP_SUBTYPE,
           INST_APP_OFFSET, INST_APP_SIZE, INST_APP_LABEL);
    encode(plan->table + out++ * INST_ENTRY_SIZE, INST_WIFI_TYPE, INST_WIFI_SUBTYPE,
           INST_WIFI_OFFSET, INST_WIFI_SIZE, INST_WIFI_LABEL);

    uint8_t *md5_entry = plan->table + out * INST_ENTRY_SIZE;
    memset(md5_entry, 0xFF, INST_ENTRY_SIZE);
    md5_entry[0] = 0xEB;
    md5_entry[1] = 0xEB;
    md5(plan->table, out * INST_ENTRY_SIZE, md5_entry + 16);
    return INST_TABLE_OK;
}

const char *inst_table_err_name(inst_table_err_t err)
{
    switch (err) {
    case INST_TABLE_OK: return "ok";
    case INST_TABLE_ERR_FORMAT: return "bad partition table";
    case INST_TABLE_ERR_ENTRY: return "invalid partition entry";
    case INST_TABLE_ERR_OVERLAP: return "overlapping partitions";
    case INST_TABLE_ERR_RESERVED: return "needs installer area";
    case INST_TABLE_ERR_NO_APP: return "no application";
    case INST_TABLE_ERR_IMAGE_END: return "image too large";
    case INST_TABLE_ERR_FULL: return "partition table full";
    }
    return "unknown";
}
