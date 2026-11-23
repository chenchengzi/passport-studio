// components/installer_core/include/inst_wifi.h
// Saved Wi-Fi networks, written by the installer to the INST_WIFI_OFFSET sector
// and read by applications through components/passport_wifi. Pure C.
//
// The list is ordered by priority (index 0 first). Passwords are stored in
// plain text, like ESP-IDF's own Wi-Fi configuration in NVS.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define INST_WIFI_MAGIC         0x31574950u   // "PIW1"
#define INST_WIFI_MAX_NETWORKS  5u
#define INST_WIFI_SSID_MAX      32u
#define INST_WIFI_PASS_MIN      8u            // WPA passphrase; 0 means an open network
#define INST_WIFI_PASS_MAX      64u           // 64 characters: a hexadecimal PSK

typedef struct {
    uint8_t ssid_len;
    uint8_t ssid[INST_WIFI_SSID_MAX];         // not NUL-terminated
    uint8_t pass_len;
    uint8_t pass[INST_WIFI_PASS_MAX];         // not NUL-terminated
} inst_wifi_net_t;

typedef struct {
    uint32_t magic;
    uint32_t seq;                             // incremented by every save
    uint8_t count;
    uint8_t reserved[3];
    inst_wifi_net_t nets[INST_WIFI_MAX_NETWORKS];
    uint8_t pad[2];
    uint32_t crc32;                           // over every preceding byte
} inst_wifi_store_t;

_Static_assert(sizeof(inst_wifi_store_t) == 508, "inst_wifi_store_t layout changed");

void inst_wifi_seal(inst_wifi_store_t *store);

// True for a sealed, well-formed store. An erased sector is not valid.
bool inst_wifi_valid(const inst_wifi_store_t *store);

// Parse a WIFI_SET payload (after the opcode): u8 count, then per network
// u8 ssid_len, ssid, u8 pass_len, pass. Fills count and nets only.
bool inst_wifi_parse(const uint8_t *payload, size_t len, inst_wifi_store_t *out);

// ESP-IDF Wi-Fi driver values for namespace "nvs.net80211": "sta.ssid" is a
// 36-byte blob (u32 length, 32 bytes) and "sta.pswd" a 65-byte blob
// (NUL-padded). Verified against ESP-IDF 5.5.3 libnet80211.
#define INST_WIFI_NVS_SSID_LEN  36u
#define INST_WIFI_NVS_PASS_LEN  65u
void inst_wifi_nvs_values(const inst_wifi_net_t *net, uint8_t ssid[INST_WIFI_NVS_SSID_LEN],
                          uint8_t pass[INST_WIFI_NVS_PASS_LEN]);

// Encode the WIFI characteristic (see inst_proto.h). Passwords are never sent.
size_t inst_wifi_encode_list(const inst_wifi_store_t *store, uint8_t result, uint8_t flags,
                             uint32_t ops, uint8_t *out, size_t cap);
