// components/installer_core/include/inst_table.h
// Validate the partition table of an incoming merged image and produce the
// table that is actually written: partitions are clipped to the play area and
// the installer's own entries are appended. Pure C; MD5 comes from the caller.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "inst_layout.h"

typedef void (*inst_md5_fn)(const uint8_t *data, size_t len, uint8_t out[16]);

typedef enum {
    INST_TABLE_OK = 0,
    INST_TABLE_ERR_FORMAT,       // missing/garbled entries or MD5 mismatch
    INST_TABLE_ERR_ENTRY,        // invalid offset, size, alignment, label, or flags
    INST_TABLE_ERR_OVERLAP,      // duplicate label or overlapping partitions
    INST_TABLE_ERR_RESERVED,     // a partition needs the installer's Flash area
    INST_TABLE_ERR_NO_APP,       // no factory or ota_0 application partition
    INST_TABLE_ERR_IMAGE_END,    // image data reaches into the installer area
    INST_TABLE_ERR_FULL,         // no room to append the installer entries
} inst_table_err_t;

typedef struct {
    uint8_t table[INST_TABLE_MAX];   // patched table, 0xFF padded
    uint32_t boot_offset;            // factory (or ota_0) partition offset
    uint32_t boot_size;              // its size after clipping
    uint8_t entries;                 // entries copied from the image
    uint8_t clipped;                 // entries shrunk to fit the play area
} inst_table_plan_t;

// image_table: bytes [0x8000, 0x8000 + INST_TABLE_MAX) of the merged image.
// image_end:   end offset of meaningful image data (trailing 0xFF trimmed).
inst_table_err_t inst_table_plan(const uint8_t *image_table, uint32_t image_end,
                                 inst_md5_fn md5, inst_table_plan_t *plan);

const char *inst_table_err_name(inst_table_err_t err);
