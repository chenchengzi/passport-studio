// components/installer_core/include/inst_proto.h
// BLE protocol between the resident installer and the iOS app. Mirrored in
// ios/PassportKit/Sources/PassportKit/InstallerProtocol.swift; keep both in sync.
// All integers are little-endian. Every characteristic requires an encrypted,
// MITM-authenticated link (LE Secure Connections, passkey shown on the device).
#pragma once

#include <stdint.h>

// 128-bit UUIDs, written in the usual text order. NimBLE wants them reversed.
#define INST_UUID_SERVICE  "CDDE6C00-DD5A-47C6-890F-268DE0F6918A"
#define INST_UUID_INFO     "CDDE6C01-DD5A-47C6-890F-268DE0F6918A"   // read
#define INST_UUID_CONTROL  "CDDE6C02-DD5A-47C6-890F-268DE0F6918A"   // write
#define INST_UUID_DATA     "CDDE6C03-DD5A-47C6-890F-268DE0F6918A"   // write without response
#define INST_UUID_STATUS   "CDDE6C04-DD5A-47C6-890F-268DE0F6918A"   // read, notify
#define INST_UUID_WIFI     "CDDE6C05-DD5A-47C6-890F-268DE0F6918A"   // read (protocol 2)

// CONTROL opcodes
#define INST_OP_BEGIN      0x01   // u32 end, u8[32] sha256, u8 name_len, name
#define INST_OP_COMMIT     0x02
#define INST_OP_ABORT      0x03
#define INST_OP_REBOOT     0x04   // leave the installer and start the play
#define INST_OP_WIFI_SET   0x20   // u8 count, {u8 ssid_len, ssid, u8 pass_len, pass}
                                  // replaces the saved list; count 0 clears it

// DATA opcodes
#define INST_OP_DATA       0x10   // u32 offset, payload
#define INST_OP_SKIP       0x11   // u32 offset, u32 to (bytes are 0xFF)

#define INST_BEGIN_LEN     (1 + 4 + 32 + 1)
#define INST_DATA_HEADER   5
#define INST_SKIP_LEN      9

// STATUS notification, 16 bytes:
//   u8 phase, u8 error (inst_err_t), u8 table_err (inst_table_err_t), u8 flags,
//   u32 position (next byte the device expects), u32 end, u32 sequence
#define INST_STATUS_LEN    16
#define INST_PHASE_IDLE      0
#define INST_PHASE_RECEIVING 1
#define INST_PHASE_VERIFYING 2
#define INST_PHASE_DONE      3
#define INST_PHASE_FAILED    4
#define INST_STATUS_RESUMED  0x01   // flags: BEGIN continued an earlier session
#define INST_STATUS_RESYNC   0x02   // flags: a DATA/SKIP packet was out of sequence

// INFO, read:
//   u8 protocol, u8 flags, u16 reserved, u32 stream_start, u32 play_limit,
//   u32 installed_end, u8[32] installed_sha256, u8 name_len, name, u8 ver_len, version
#define INST_INFO_HAS_RECORD   0x01   // installed_* describe the current play
#define INST_INFO_PENDING      0x02   // an install was interrupted; play is not usable
#define INST_INFO_APP_PRESENT  0x04   // a bootable application header exists

// WIFI, read: u8 format (1), u8 result, u8 flags, u8 count, u32 ops,
//   then per saved network in priority order: u8 ssid_len, ssid. No passwords.
// ops counts processed WIFI_SET requests since the installer started; the app
// waits for it to change, then checks result.
#define INST_WIFI_RESULT_NONE     0
#define INST_WIFI_RESULT_SAVED    1
#define INST_WIFI_RESULT_INVALID  2   // malformed list
#define INST_WIFI_RESULT_FAILED   3   // Flash write failed
#define INST_WIFI_APPLIED      0x01   // flags: first network copied to the play's Wi-Fi settings

// Sender guidance: keep at most this many unacknowledged bytes in flight.
// The device acknowledges at least every INST_ACK_INTERVAL bytes.
#define INST_WINDOW_BYTES  (16 * 1024)
#define INST_ACK_INTERVAL  4096
