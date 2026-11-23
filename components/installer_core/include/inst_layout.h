// components/installer_core/include/inst_layout.h
// Fixed Flash layout shared by the bootloader hook, the resident installer, the
// host tests, and (mirrored in Swift) the iOS app. Changing any value breaks
// devices that already carry an installer; bump INST_PROTOCOL_VERSION as well.
#pragma once

#include <stdint.h>

#define INST_FLASH_SIZE        0x800000u

// Installed images are standard merged files written from 0x0. The installer
// never writes the bootloader region, so a transfer starts at the partition table.
#define INST_TABLE_OFFSET      0x8000u
#define INST_TABLE_SECTOR      0x1000u
#define INST_TABLE_MAX         0xC00u     // ESP-IDF partition-table size limit
#define INST_STREAM_START      INST_TABLE_OFFSET
#define INST_PLAY_START        0x9000u    // first byte written to Flash

// Everything from INST_PLAY_LIMIT to the end of Flash belongs to the installer.
#define INST_PLAY_LIMIT        0x6FF000u
#define INST_STATE_OFFSET      0x6FF000u  // one sector: install journal and record
#define INST_STATE_SIZE        0x1000u
#define INST_APP_OFFSET        0x700000u  // installer application image
#define INST_APP_SIZE          0xFF000u
#define INST_WIFI_OFFSET       0x7FF000u  // one sector: saved Wi-Fi networks
#define INST_WIFI_SIZE         0x1000u

#define INST_STATE_LABEL       "inst_state"
#define INST_APP_LABEL         "installer"
#define INST_WIFI_LABEL        "inst_wifi"
#define INST_STATE_TYPE        0x01u      // data
#define INST_STATE_SUBTYPE     0x40u      // custom data subtype, never auto-mounted
#define INST_WIFI_TYPE         0x01u      // data
#define INST_WIFI_SUBTYPE      0x41u      // custom data subtype, read by passport_wifi
#define INST_APP_TYPE          0x00u      // app
#define INST_APP_SUBTYPE       0x20u      // test: ignored by the normal boot path

#define INST_PROTOCOL_VERSION  2u
