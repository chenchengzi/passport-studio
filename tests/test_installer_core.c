// Host tests for the resident installer core: boot decisions, journal, partition
// table planning, and a complete install stream against a simulated 8 MB Flash.
// Optional argv[1]: a real merged image to install through the same path.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "inst_boot.h"
#include "inst_layout.h"
#include "inst_session.h"
#include "inst_state.h"
#include "inst_table.h"
#include "inst_wifi.h"
#include "test_hash.h"

// ---------------------------------------------------------------------------
// Simulated Flash with NOR semantics: writes may only clear bits.
// ---------------------------------------------------------------------------
typedef struct {
    uint8_t *mem;
    uint32_t erase_lo, erase_hi;     // play-area erase bounds seen this run
    unsigned erase_calls;
    int fail_write_at;               // fail the Nth write (0 = never)
    unsigned writes;
    test_sha256_t sha;
    bool app_ok;
} flash_t;

static bool f_erase(void *ctx, uint32_t addr, uint32_t len)
{
    flash_t *f = ctx;
    assert(addr % 0x1000 == 0 && len % 0x1000 == 0 && addr + len <= INST_FLASH_SIZE);
    assert(addr >= INST_TABLE_OFFSET);   // never the bootloader
    memset(f->mem + addr, 0xFF, len);
    if (addr >= INST_PLAY_START && addr < INST_PLAY_LIMIT) {
        if (addr < f->erase_lo) f->erase_lo = addr;
        if (addr + len > f->erase_hi) f->erase_hi = addr + len;
    }
    f->erase_calls++;
    return true;
}

static bool f_write(void *ctx, uint32_t addr, const void *data, uint32_t len)
{
    flash_t *f = ctx;
    assert(addr >= INST_TABLE_OFFSET && addr + len <= INST_FLASH_SIZE);
    assert(addr + len <= INST_APP_OFFSET);   // never the installer image
    f->writes++;
    if (f->fail_write_at && (int)f->writes == f->fail_write_at) return false;
    const uint8_t *p = data;
    for (uint32_t i = 0; i < len; i++) {
        assert((f->mem[addr + i] & p[i]) == p[i]);   // programming an unerased bit
        f->mem[addr + i] &= p[i];
    }
    return true;
}

static bool f_read(void *ctx, uint32_t addr, void *data, uint32_t len)
{
    flash_t *f = ctx;
    assert(addr + len <= INST_FLASH_SIZE);
    memcpy(data, f->mem + addr, len);
    return true;
}

static void f_sha(void *ctx, int step, const void *data, size_t len, uint8_t out[32])
{
    flash_t *f = ctx;
    if (step == INST_SHA_START) test_sha256_start(&f->sha);
    else if (step == INST_SHA_UPDATE) test_sha256_update(&f->sha, data, len);
    else test_sha256_finish(&f->sha, out);
}

static bool f_verify_app(void *ctx, uint32_t offset, uint32_t max_size)
{
    flash_t *f = ctx;
    return f->app_ok && f->mem[offset] == INST_IMAGE_MAGIC && max_size > 0;
}

static flash_t *flash_new(uint8_t fill)
{
    flash_t *f = calloc(1, sizeof(*f));
    f->mem = malloc(INST_FLASH_SIZE);
    for (uint32_t i = 0; i < INST_FLASH_SIZE; i++) f->mem[i] = (uint8_t)(fill ^ (i * 31));
    f->erase_lo = UINT32_MAX;
    f->app_ok = true;
    return f;
}

static void flash_free(flash_t *f)
{
    free(f->mem);
    free(f);
}

static inst_ops_t ops_for(flash_t *f)
{
    return (inst_ops_t){.ctx = f, .erase = f_erase, .write = f_write, .read = f_read,
                        .md5 = test_md5, .sha256 = f_sha, .verify_app = f_verify_app};
}

// ---------------------------------------------------------------------------
// Partition-table helpers
// ---------------------------------------------------------------------------
static void entry(uint8_t *table, int index, uint8_t type, uint8_t subtype, uint32_t offset,
                  uint32_t size, const char *label)
{
    uint8_t *e = table + index * 32;
    memset(e, 0, 32);
    e[0] = 0xAA;
    e[1] = 0x50;
    e[2] = type;
    e[3] = subtype;
    for (int i = 0; i < 4; i++) {
        e[4 + i] = (uint8_t)(offset >> (8 * i));
        e[8 + i] = (uint8_t)(size >> (8 * i));
    }
    memcpy(e + 12, label, strlen(label));
}

static void finish_table(uint8_t *table, int count)
{
    uint8_t *e = table + count * 32;
    memset(e, 0xFF, 32);
    e[0] = 0xEB;
    e[1] = 0xEB;
    test_md5(table, (size_t)count * 32, e + 16);
}

// The repository's original default: nvs, phy_init, factory spanning the rest.
static void default_table(uint8_t table[INST_TABLE_MAX])
{
    memset(table, 0xFF, INST_TABLE_MAX);
    entry(table, 0, 1, 0x02, 0x9000, 0x6000, "nvs");
    entry(table, 1, 1, 0x01, 0xF000, 0x1000, "phy_init");
    entry(table, 2, 0, 0x00, 0x10000, 0x7F0000, "factory");
    finish_table(table, 3);
}

static uint32_t entry_u32(const uint8_t *table, int index, int field)
{
    return inst_le32(table + index * 32 + field);
}

// ---------------------------------------------------------------------------
static void test_hold(void)
{
    inst_hold_t h;
    inst_hold_init(&h, 1000);
    assert(inst_hold_sample(&h, 1000, false) == INST_HOLD_SAMPLING);
    assert(inst_hold_sample(&h, 1099, false) == INST_HOLD_SAMPLING);
    assert(inst_hold_sample(&h, 1100, false) == INST_HOLD_BOOT);

    inst_hold_init(&h, 0);
    assert(inst_hold_sample(&h, 0, true) == INST_HOLD_SAMPLING);
    assert(inst_hold_sample(&h, 150, true) == INST_HOLD_SAMPLING);   // past window: still held
    assert(inst_hold_sample(&h, 160, false) == INST_HOLD_SAMPLING);  // bounce
    assert(inst_hold_sample(&h, 170, true) == INST_HOLD_SAMPLING);
    assert(inst_hold_sample(&h, 300, true) == INST_HOLD_ENTER);

    inst_hold_init(&h, 0);
    assert(inst_hold_sample(&h, 10, true) == INST_HOLD_SAMPLING);
    assert(inst_hold_sample(&h, 100, false) == INST_HOLD_SAMPLING);
    assert(inst_hold_sample(&h, 130, false) == INST_HOLD_BOOT);      // released early
    puts("  hold: PASS");
}

static void test_state(void)
{
    inst_state_t s;
    memset(&s, 0xFF, sizeof(s));
    assert(inst_state_check(&s) == INST_STATE_BLANK);
    assert(!inst_state_requires_installer(&s));

    memset(&s, 0, sizeof(s));
    s.flags = INST_STATE_PENDING;
    inst_state_seal(&s);
    assert(inst_state_check(&s) == INST_STATE_VALID);
    assert(inst_state_requires_installer(&s));

    s.flags = 0;
    inst_state_seal(&s);
    assert(!inst_state_requires_installer(&s));

    s.image_end ^= 1;   // torn rewrite
    assert(inst_state_check(&s) == INST_STATE_CORRUPT);
    assert(inst_state_requires_installer(&s));

    memset(&s, 0xFF, sizeof(s));
    s.flags = 0;         // partly programmed sector
    assert(inst_state_requires_installer(&s));
    puts("  journal: PASS");
}

static void test_boot_offset(void)
{
    uint8_t t[INST_TABLE_MAX];
    default_table(t);
    assert(inst_boot_app_offset(t, INST_TABLE_MAX) == 0x10000);

    memset(t, 0xFF, sizeof(t));
    entry(t, 0, 1, 0x02, 0x9000, 0x4000, "nvs");
    entry(t, 1, 1, 0x00, 0xD000, 0x2000, "otadata");
    entry(t, 2, 0, 0x10, 0x20000, 0x200000, "ota_0");
    entry(t, 3, 0, 0x11, 0x220000, 0x200000, "ota_1");
    assert(inst_boot_app_offset(t, INST_TABLE_MAX) == 0x20000);

    memset(t, 0xFF, sizeof(t));
    entry(t, 0, 1, 0x02, 0x9000, 0x4000, "nvs");
    assert(inst_boot_app_offset(t, INST_TABLE_MAX) == 0);
    memset(t, 0xFF, sizeof(t));
    assert(inst_boot_app_offset(t, INST_TABLE_MAX) == 0);   // erased table
    puts("  boot partition: PASS");
}

static void test_table(void)
{
    uint8_t t[INST_TABLE_MAX];
    static inst_table_plan_t plan;

    default_table(t);
    assert(inst_table_plan(t, 0x200000, test_md5, &plan) == INST_TABLE_OK);
    assert(plan.entries == 3 && plan.clipped == 1);
    assert(plan.boot_offset == 0x10000 && plan.boot_size == INST_PLAY_LIMIT - 0x10000);
    assert(entry_u32(plan.table, 2, 8) == INST_PLAY_LIMIT - 0x10000);
    assert(!memcmp(plan.table + 3 * 32 + 12, INST_STATE_LABEL, sizeof(INST_STATE_LABEL)));
    assert(entry_u32(plan.table, 3, 4) == INST_STATE_OFFSET);
    assert(!memcmp(plan.table + 4 * 32 + 12, INST_APP_LABEL, sizeof(INST_APP_LABEL)));
    assert(entry_u32(plan.table, 4, 4) == INST_APP_OFFSET && plan.table[4 * 32 + 3] == 0x20);
    assert(entry_u32(plan.table, 4, 8) == INST_APP_SIZE);
    assert(!memcmp(plan.table + 5 * 32 + 12, INST_WIFI_LABEL, sizeof(INST_WIFI_LABEL)));
    assert(entry_u32(plan.table, 5, 4) == INST_WIFI_OFFSET && plan.table[5 * 32 + 3] == 0x41);
    uint8_t digest[16];
    test_md5(plan.table, 6 * 32, digest);
    assert(plan.table[6 * 32] == 0xEB && !memcmp(plan.table + 6 * 32 + 16, digest, 16));
    assert(plan.table[7 * 32] == 0xFF);

    // The patched table is accepted again unchanged (re-installing this template).
    static inst_table_plan_t again;
    assert(inst_table_plan(plan.table, 0x200000, test_md5, &again) == INST_TABLE_OK);
    assert(!memcmp(again.table, plan.table, INST_TABLE_MAX) && again.clipped == 0);

    // A table from an earlier template revision (1 MB installer, no Wi-Fi
    // sector) is rewritten to the current installer entries.
    memset(t, 0xFF, sizeof(t));
    entry(t, 0, 1, 0x02, 0x9000, 0x6000, "nvs");
    entry(t, 1, 0, 0x00, 0x10000, INST_PLAY_LIMIT - 0x10000, "factory");
    entry(t, 2, 1, 0x40, INST_STATE_OFFSET, INST_STATE_SIZE, "inst_state");
    entry(t, 3, 0, 0x20, INST_APP_OFFSET, 0x100000, "installer");
    finish_table(t, 4);
    assert(inst_table_plan(t, 0x200000, test_md5, &again) == INST_TABLE_OK);
    assert(again.entries == 2 && entry_u32(again.table, 3, 8) == INST_APP_SIZE);
    assert(!memcmp(again.table + 4 * 32 + 12, INST_WIFI_LABEL, sizeof(INST_WIFI_LABEL)));
    default_table(t);

    // Image data reaching the installer area.
    assert(inst_table_plan(t, INST_PLAY_LIMIT + 1, test_md5, &plan) == INST_TABLE_ERR_IMAGE_END);
    assert(inst_table_plan(t, INST_PLAY_LIMIT, test_md5, &plan) == INST_TABLE_OK);

    // Corrupted MD5.
    default_table(t);
    t[3 * 32 + 20] ^= 0xFF;
    assert(inst_table_plan(t, 0x200000, test_md5, &plan) == INST_TABLE_ERR_FORMAT);

    // A data partition inside the installer area.
    memset(t, 0xFF, sizeof(t));
    entry(t, 0, 1, 0x02, 0x9000, 0x6000, "nvs");
    entry(t, 1, 0, 0x00, 0x10000, 0x300000, "factory");
    entry(t, 2, 1, 0x82, 0x700000, 0x100000, "storage");
    finish_table(t, 3);
    assert(inst_table_plan(t, 0x200000, test_md5, &plan) == INST_TABLE_ERR_RESERVED);

    // A data partition crossing the limit: clipped only without prebuilt content.
    memset(t, 0xFF, sizeof(t));
    entry(t, 0, 1, 0x02, 0x9000, 0x6000, "nvs");
    entry(t, 1, 0, 0x00, 0x10000, 0x300000, "factory");
    entry(t, 2, 1, 0x82, 0x310000, 0x4F0000, "storage");
    finish_table(t, 3);
    assert(inst_table_plan(t, 0x200000, test_md5, &plan) == INST_TABLE_OK);
    assert(entry_u32(plan.table, 2, 8) == INST_PLAY_LIMIT - 0x310000);
    assert(inst_table_plan(t, 0x400000, test_md5, &plan) == INST_TABLE_ERR_RESERVED);

    // Reserved labels with a different definition, or a second test app.
    memset(t, 0xFF, sizeof(t));
    entry(t, 0, 0, 0x00, 0x10000, 0x300000, "factory");
    entry(t, 1, 1, 0x82, 0x310000, 0x1000, "installer");
    finish_table(t, 2);
    assert(inst_table_plan(t, 0x200000, test_md5, &plan) == INST_TABLE_ERR_RESERVED);
    memset(t, 0xFF, sizeof(t));
    entry(t, 0, 0, 0x00, 0x10000, 0x300000, "factory");
    entry(t, 1, 1, 0x82, 0x310000, 0x1000, "inst_wifi");
    finish_table(t, 2);
    assert(inst_table_plan(t, 0x200000, test_md5, &plan) == INST_TABLE_ERR_RESERVED);
    memset(t, 0xFF, sizeof(t));
    entry(t, 0, 0, 0x00, 0x10000, 0x300000, "factory");
    entry(t, 1, 0, 0x20, 0x310000, 0x100000, "test");
    finish_table(t, 2);
    assert(inst_table_plan(t, 0x200000, test_md5, &plan) == INST_TABLE_ERR_RESERVED);

    // Overlap, duplicate label, encryption, misalignment, no application.
    memset(t, 0xFF, sizeof(t));
    entry(t, 0, 0, 0x00, 0x10000, 0x300000, "factory");
    entry(t, 1, 1, 0x82, 0x300000, 0x10000, "storage");
    finish_table(t, 2);
    assert(inst_table_plan(t, 0x200000, test_md5, &plan) == INST_TABLE_ERR_OVERLAP);
    memset(t, 0xFF, sizeof(t));
    entry(t, 0, 0, 0x00, 0x10000, 0x300000, "factory");
    entry(t, 1, 1, 0x82, 0x310000, 0x10000, "factory");
    finish_table(t, 2);
    assert(inst_table_plan(t, 0x200000, test_md5, &plan) == INST_TABLE_ERR_OVERLAP);
    memset(t, 0xFF, sizeof(t));
    entry(t, 0, 0, 0x00, 0x10000, 0x300000, "factory");
    t[28] = 1;
    finish_table(t, 1);
    assert(inst_table_plan(t, 0x200000, test_md5, &plan) == INST_TABLE_ERR_ENTRY);
    memset(t, 0xFF, sizeof(t));
    entry(t, 0, 0, 0x00, 0x18000, 0x300000, "factory");
    finish_table(t, 1);
    assert(inst_table_plan(t, 0x200000, test_md5, &plan) == INST_TABLE_ERR_ENTRY);
    memset(t, 0xFF, sizeof(t));
    entry(t, 0, 1, 0x02, 0x9000, 0x6000, "nvs");
    finish_table(t, 1);
    assert(inst_table_plan(t, 0x200000, test_md5, &plan) == INST_TABLE_ERR_NO_APP);
    default_table(t);
    assert(inst_table_plan(t, 0xC000, test_md5, &plan) == INST_TABLE_ERR_NO_APP);

    // OTA layouts keep ota_0 as the boot partition.
    memset(t, 0xFF, sizeof(t));
    entry(t, 0, 1, 0x02, 0x9000, 0x4000, "nvs");
    entry(t, 1, 1, 0x00, 0xD000, 0x2000, "otadata");
    entry(t, 2, 0, 0x10, 0x20000, 0x300000, "ota_0");
    entry(t, 3, 0, 0x11, 0x320000, 0x300000, "ota_1");
    finish_table(t, 4);
    assert(inst_table_plan(t, 0x200000, test_md5, &plan) == INST_TABLE_OK);
    assert(plan.boot_offset == 0x20000 && plan.clipped == 0);

    // A table without an MD5 entry still gets one.
    memset(t, 0xFF, sizeof(t));
    entry(t, 0, 0, 0x00, 0x10000, 0x300000, "factory");
    assert(inst_table_plan(t, 0x200000, test_md5, &plan) == INST_TABLE_OK);
    assert(plan.table[4 * 32] == 0xEB);

    // Garbage instead of a table.
    memset(t, 0x00, sizeof(t));
    assert(inst_table_plan(t, 0x200000, test_md5, &plan) == INST_TABLE_ERR_FORMAT);
    puts("  partition table: PASS");
}

// ---------------------------------------------------------------------------
// Install streaming
// ---------------------------------------------------------------------------
typedef struct {
    uint8_t *bytes;   // full merged image, from 0x0
    uint32_t end;     // trimmed end
} image_t;

static image_t synthetic_image(uint32_t end, unsigned seed)
{
    image_t img = {.bytes = malloc(end), .end = end};
    memset(img.bytes, 0xFF, end);
    default_table(img.bytes + INST_TABLE_OFFSET);
    for (uint32_t i = 0x10000; i < end; i++) img.bytes[i] = (uint8_t)(i * 7 + seed);
    img.bytes[0x10000] = INST_IMAGE_MAGIC;
    // A 0xFF hole the sender can skip, and one that is not sector aligned.
    memset(img.bytes + 0x20000, 0xFF, 0x3000);
    memset(img.bytes + 0x28100, 0xFF, 0x500);
    img.bytes[end - 1] = 0x5A;
    return img;
}

static void image_sha(const image_t *img, uint8_t out[32])
{
    test_sha256(img->bytes + INST_STREAM_START, img->end - INST_STREAM_START, out);
}

// Sends the image like the iOS app: data in MTU-sized pieces, aligned all-0xFF
// sectors as SKIP. drop_every > 0 loses that packet and recovers from SEQ.
static inst_err_t stream(inst_session_t *s, const image_t *img, uint32_t from, uint32_t until,
                         unsigned chunk, unsigned drop_every)
{
    static uint8_t ff[0x1000];
    memset(ff, 0xFF, sizeof(ff));
    uint32_t pos = from;
    unsigned packet = 0, drops = 0;
    while (pos < until) {
        if (pos % 0x1000 == 0 && pos + 0x1000 <= until && !memcmp(img->bytes + pos, ff, 0x1000)) {
            uint32_t to = pos;
            while (to + 0x1000 <= until && !memcmp(img->bytes + to, ff, 0x1000)) to += 0x1000;
            inst_err_t err = inst_session_skip(s, pos, to);
            if (err != INST_OK) return err;
            pos = to;
            continue;
        }
        uint32_t n = until - pos < chunk ? until - pos : chunk;
        if (pos % 0x1000 + n > 0x1000) n = 0x1000 - pos % 0x1000;   // stop at skip candidates
        packet++;
        if (drop_every && packet % drop_every == 0) {
            // Lost in the air; the next one arrives out of sequence.
            drops++;
            uint32_t next = pos + n;
            if (next < until) {
                uint32_t m = until - next < chunk ? until - next : chunk;
                if (next % 0x1000 + m > 0x1000) m = 0x1000 - next % 0x1000;
                assert(inst_session_data(s, next, img->bytes + next, m) == INST_ERR_SEQ);
                pos = inst_session_position(s);
            }
            continue;
        }
        inst_err_t err = inst_session_data(s, pos, img->bytes + pos, n);
        if (err != INST_OK) return err;
        pos += n;
    }
    assert(!drop_every || drops > 0);
    return INST_OK;
}

static void test_install(void)
{
    flash_t *f = flash_new(0x3C);
    uint8_t old_after[0x2000];
    image_t img = synthetic_image(0x41234, 3);
    uint32_t tail = (img.end + 0xFFF) & ~0xFFFu;
    memcpy(old_after, f->mem + tail, sizeof(old_after));
    uint8_t old_table[INST_TABLE_MAX];
    memcpy(old_table, f->mem + INST_TABLE_OFFSET, sizeof(old_table));

    static inst_session_t s;
    inst_ops_t ops = ops_for(f);
    inst_session_init(&s, &ops);
    uint8_t sha[32];
    image_sha(&img, sha);
    bool resumed;
    const char *name = "Pocket Pomodoro";
    assert(inst_session_commit(&s) == INST_ERR_STATE);
    assert(inst_session_begin(&s, img.end, sha, (const uint8_t *)name, strlen(name), &resumed) == INST_OK);
    assert(!resumed);

    // Partial transfer, then a disconnect: the journal is pending, table untouched.
    assert(stream(&s, &img, INST_STREAM_START, 0x22000, 244, 0) == INST_OK);
    inst_state_t st;
    assert(inst_session_read_state(&ops, &st));
    assert(inst_state_requires_installer(&st));
    assert(!memcmp(f->mem + INST_TABLE_OFFSET, old_table, sizeof(old_table)));

    // Reconnect and resume where the device stopped, losing packets on the way.
    assert(inst_session_begin(&s, img.end, sha, (const uint8_t *)name, strlen(name), &resumed) == INST_OK);
    assert(resumed && inst_session_position(&s) == 0x22000);
    assert(inst_session_data(&s, 0x9000, img.bytes + 0x9000, 16) == INST_ERR_SEQ);
    assert(stream(&s, &img, inst_session_position(&s), img.end, 495, 7) == INST_OK);
    assert(inst_session_data(&s, img.end, img.bytes, 1) == INST_ERR_SIZE);
    assert(inst_session_commit(&s) == INST_OK);
    assert(s.phase == INST_DONE);

    // Flash content matches the image; data after it is untouched.
    assert(!memcmp(f->mem + INST_PLAY_START, img.bytes + INST_PLAY_START, img.end - INST_PLAY_START));
    for (uint32_t i = img.end; i < tail; i++) assert(f->mem[i] == 0xFF);
    assert(!memcmp(f->mem + tail, old_after, sizeof(old_after)));
    assert(f->erase_lo == INST_PLAY_START && f->erase_hi == tail);

    // Table committed, journal cleared, record kept.
    static inst_table_plan_t plan;
    assert(inst_table_plan(img.bytes + INST_TABLE_OFFSET, img.end, test_md5, &plan) == INST_TABLE_OK);
    assert(!memcmp(f->mem + INST_TABLE_OFFSET, plan.table, INST_TABLE_MAX));
    assert(inst_boot_app_offset(f->mem + INST_TABLE_OFFSET, INST_TABLE_MAX) == 0x10000);
    assert(inst_session_read_state(&ops, &st));
    assert(inst_state_check(&st) == INST_STATE_VALID && !inst_state_requires_installer(&st));
    assert(st.image_end == img.end && !memcmp(st.sha256, sha, 32));
    assert(st.name_len == strlen(name) && !memcmp(st.name, name, st.name_len));
    free(img.bytes);
    flash_free(f);
    puts("  install + resume + packet loss: PASS");
}

static void test_install_failures(void)
{
    static inst_session_t s;
    uint8_t sha[32];
    bool resumed;

    // Wrong hash: nothing committed, journal stays pending.
    flash_t *f = flash_new(0x11);
    inst_ops_t ops = ops_for(f);
    image_t img = synthetic_image(0x30000, 9);
    uint8_t old_table[INST_TABLE_MAX];
    memcpy(old_table, f->mem + INST_TABLE_OFFSET, sizeof(old_table));
    image_sha(&img, sha);
    sha[0] ^= 1;
    inst_session_init(&s, &ops);
    assert(inst_session_begin(&s, img.end, sha, NULL, 0, &resumed) == INST_OK);
    assert(stream(&s, &img, INST_STREAM_START, img.end, 300, 0) == INST_OK);
    assert(inst_session_commit(&s) == INST_ERR_VERIFY && s.phase == INST_FAILED);
    assert(!memcmp(f->mem + INST_TABLE_OFFSET, old_table, sizeof(old_table)));
    inst_state_t st;
    assert(inst_session_read_state(&ops, &st) && inst_state_requires_installer(&st));

    // A fresh begin after failure restarts; the right hash then succeeds.
    sha[0] ^= 1;
    assert(inst_session_begin(&s, img.end, sha, NULL, 0, &resumed) == INST_OK && !resumed);
    assert(inst_session_position(&s) == INST_STREAM_START);
    assert(stream(&s, &img, INST_STREAM_START, img.end, 300, 0) == INST_OK);
    assert(inst_session_commit(&s) == INST_OK);
    assert(inst_session_read_state(&ops, &st) && !inst_state_requires_installer(&st));
    flash_free(f);

    // Invalid application image: rejected before the table is written.
    f = flash_new(0x22);
    ops = ops_for(f);
    f->app_ok = false;
    memcpy(old_table, f->mem + INST_TABLE_OFFSET, sizeof(old_table));
    inst_session_init(&s, &ops);
    assert(inst_session_begin(&s, img.end, sha, NULL, 0, &resumed) == INST_OK);
    assert(stream(&s, &img, INST_STREAM_START, img.end, 300, 0) == INST_OK);
    assert(inst_session_commit(&s) == INST_ERR_IMAGE);
    assert(!memcmp(f->mem + INST_TABLE_OFFSET, old_table, sizeof(old_table)));
    flash_free(f);

    // Rejected table: no play-area byte is erased and the journal is not set.
    f = flash_new(0x33);
    ops = ops_for(f);
    uint8_t *bad = img.bytes + INST_TABLE_OFFSET;
    memset(bad, 0xFF, INST_TABLE_MAX);
    entry(bad, 0, 1, 0x02, 0x9000, 0x6000, "nvs");
    finish_table(bad, 1);
    image_sha(&img, sha);
    inst_session_init(&s, &ops);
    assert(inst_session_begin(&s, img.end, sha, NULL, 0, &resumed) == INST_OK);
    assert(stream(&s, &img, INST_STREAM_START, img.end, 300, 0) == INST_ERR_TABLE);
    assert(s.phase == INST_FAILED && s.table_err == INST_TABLE_ERR_NO_APP);
    assert(f->erase_calls == 0);
    assert(inst_session_read_state(&ops, &st) && inst_state_check(&st) != INST_STATE_VALID);

    // Out-of-range sizes and commands.
    inst_session_init(&s, &ops);
    assert(inst_session_begin(&s, INST_PLAY_LIMIT + 1, sha, NULL, 0, &resumed) == INST_ERR_SIZE);
    assert(inst_session_begin(&s, INST_PLAY_START, sha, NULL, 0, &resumed) == INST_ERR_SIZE);
    assert(inst_session_data(&s, INST_STREAM_START, sha, 1) == INST_ERR_STATE);
    assert(inst_session_begin(&s, 0x20000, sha, NULL, 0, &resumed) == INST_OK);
    assert(inst_session_data(&s, INST_STREAM_START, sha, 0) == INST_ERR_SIZE);
    assert(inst_session_skip(&s, INST_STREAM_START, 0x20001) == INST_ERR_SIZE);
    assert(inst_session_commit(&s) == INST_ERR_SIZE);

    // Flash write failure fails the session.
    free(img.bytes);
    img = synthetic_image(0x30000, 4);
    image_sha(&img, sha);
    flash_free(f);
    f = flash_new(0x44);
    ops = ops_for(f);
    f->fail_write_at = 5;
    inst_session_init(&s, &ops);
    assert(inst_session_begin(&s, img.end, sha, NULL, 0, &resumed) == INST_OK);
    assert(stream(&s, &img, INST_STREAM_START, img.end, 300, 0) == INST_ERR_FLASH);
    assert(s.phase == INST_FAILED);
    free(img.bytes);
    flash_free(f);
    puts("  failure paths: PASS");
}

static void install_file(const char *path)
{
    FILE *fp = fopen(path, "rb");
    assert(fp);
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    image_t img = {.bytes = malloc((size_t)size)};
    assert(fread(img.bytes, 1, (size_t)size, fp) == (size_t)size);
    fclose(fp);
    uint32_t end = (uint32_t)size;
    while (end && img.bytes[end - 1] == 0xFF) end--;
    img.end = end;

    static inst_table_plan_t plan;
    inst_table_err_t terr = inst_table_plan(img.bytes + INST_TABLE_OFFSET, end, test_md5, &plan);
    printf("  %s: %ld bytes, data end 0x%x, table: %s\n", path, size, end, inst_table_err_name(terr));
    if (terr != INST_TABLE_OK) {
        free(img.bytes);
        return;
    }
    flash_t *f = flash_new(0x5A);
    inst_ops_t ops = ops_for(f);
    static inst_session_t s;
    uint8_t sha[32];
    bool resumed;
    image_sha(&img, sha);
    inst_session_init(&s, &ops);
    assert(inst_session_begin(&s, end, sha, NULL, 0, &resumed) == INST_OK);
    assert(stream(&s, &img, INST_STREAM_START, end, 495, 0) == INST_OK);
    assert(inst_session_commit(&s) == INST_OK);
    assert(!memcmp(f->mem + INST_PLAY_START, img.bytes + INST_PLAY_START, end - INST_PLAY_START));
    printf("  %s: installed, boot partition 0x%x (%u KiB), %u erase calls\n", path,
           plan.boot_offset, plan.boot_size / 1024, f->erase_calls);
    free(img.bytes);
    flash_free(f);
}

static size_t put_net(uint8_t *p, const char *ssid, const char *pass)
{
    size_t n = 0;
    p[n++] = (uint8_t)strlen(ssid);
    memcpy(p + n, ssid, strlen(ssid));
    n += strlen(ssid);
    p[n++] = (uint8_t)strlen(pass);
    memcpy(p + n, pass, strlen(pass));
    return n + strlen(pass);
}

static void test_wifi(void)
{
    static inst_wifi_store_t store;
    uint8_t msg[600];
    size_t n = 0;
    msg[n++] = 3;
    n += put_net(msg + n, "Home", "password1");
    n += put_net(msg + n, "Cafe Open", "");
    n += put_net(msg + n, "Office", "0123456789abcdef0123456789ABCDEF0123456789abcdef0123456789abcdef");
    assert(inst_wifi_parse(msg, n, &store));
    assert(store.count == 3 && store.nets[1].pass_len == 0 && store.nets[2].pass_len == 64);
    assert(!inst_wifi_parse(msg, n - 1, &store));   // truncated
    msg[n] = 0;
    assert(!inst_wifi_parse(msg, n + 1, &store));   // trailing byte

    // Seal and verify; any bit flip is caught; erased Flash is not a store.
    assert(inst_wifi_parse(msg, n, &store));
    store.seq = 7;
    inst_wifi_seal(&store);
    assert(inst_wifi_valid(&store));
    store.nets[0].ssid[0] ^= 1;
    assert(!inst_wifi_valid(&store));
    memset(&store, 0xFF, sizeof(store));
    assert(!inst_wifi_valid(&store));

    // Invalid networks.
    const char *bad[][2] = {
        {"", "password1"},                     // empty SSID
        {"Home", "short"},                     // passphrase under 8
        {"Home", "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdeg"},   // 64, not hex
        {"123456789012345678901234567890123", "password1"},   // SSID over 32
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        n = 0;
        msg[n++] = 1;
        n += put_net(msg + n, bad[i][0], bad[i][1]);
        assert(!inst_wifi_parse(msg, n, &store));
    }
    msg[0] = INST_WIFI_MAX_NETWORKS + 1;
    assert(!inst_wifi_parse(msg, 1, &store));
    msg[0] = 0;
    assert(inst_wifi_parse(msg, 1, &store) && store.count == 0);   // clear

    // Driver NVS values and the passwordless list.
    n = 0;
    msg[n++] = 2;
    n += put_net(msg + n, "Home", "password1");
    n += put_net(msg + n, "Lab", "");
    assert(inst_wifi_parse(msg, n, &store));
    uint8_t ssid[INST_WIFI_NVS_SSID_LEN], pass[INST_WIFI_NVS_PASS_LEN];
    inst_wifi_nvs_values(&store.nets[0], ssid, pass);
    assert(inst_le32(ssid) == 4 && !memcmp(ssid + 4, "Home", 4) && ssid[8] == 0);
    assert(!strcmp((const char *)pass, "password1") && pass[64] == 0);
    uint8_t list[64];
    size_t len = inst_wifi_encode_list(&store, 1, 0x01, 0x01020304, list, sizeof(list));
    const uint8_t want[] = {1, 1, 0x01, 2, 4, 3, 2, 1, 4, 'H', 'o', 'm', 'e', 3, 'L', 'a', 'b'};
    assert(len == sizeof(want) && !memcmp(list, want, len));
    assert(inst_wifi_encode_list(&store, 1, 0, 0, list, 10) == 0);
    assert(inst_wifi_encode_list(NULL, 0, 0, 0, list, sizeof(list)) == 8 && list[3] == 0);
    puts("  wifi list: PASS");
}

int main(int argc, char **argv)
{
    test_hold();
    test_state();
    test_boot_offset();
    test_table();
    test_install();
    test_install_failures();
    test_wifi();
    for (int i = 1; i < argc; i++) install_file(argv[i]);
    puts("installer core tests: PASS");
    return 0;
}
