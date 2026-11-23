// components/installer_core/inst_wifi.c
#include "inst_wifi.h"

#include <string.h>

#include "inst_state.h"

void inst_wifi_seal(inst_wifi_store_t *store)
{
    store->magic = INST_WIFI_MAGIC;
    memset(store->reserved, 0, sizeof(store->reserved));
    memset(store->pad, 0, sizeof(store->pad));
    store->crc32 = inst_crc32(store, offsetof(inst_wifi_store_t, crc32));
}

static bool hex(const uint8_t *p, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        uint8_t c = p[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}

static bool net_valid(const inst_wifi_net_t *net)
{
    if (net->ssid_len == 0 || net->ssid_len > INST_WIFI_SSID_MAX) return false;
    if (net->pass_len == 0) return true;
    if (net->pass_len < INST_WIFI_PASS_MIN || net->pass_len > INST_WIFI_PASS_MAX) return false;
    // The driver reads the password as a C string; 64 characters means a raw PSK.
    if (memchr(net->pass, 0, net->pass_len)) return false;
    return net->pass_len < INST_WIFI_PASS_MAX || hex(net->pass, net->pass_len);
}

bool inst_wifi_valid(const inst_wifi_store_t *store)
{
    if (store->magic != INST_WIFI_MAGIC || store->count > INST_WIFI_MAX_NETWORKS ||
        store->crc32 != inst_crc32(store, offsetof(inst_wifi_store_t, crc32))) {
        return false;
    }
    for (unsigned i = 0; i < store->count; i++) {
        if (!net_valid(&store->nets[i])) return false;
    }
    return true;
}

bool inst_wifi_parse(const uint8_t *payload, size_t len, inst_wifi_store_t *out)
{
    memset(out->nets, 0, sizeof(out->nets));
    if (len < 1 || payload[0] > INST_WIFI_MAX_NETWORKS) return false;
    out->count = payload[0];
    size_t at = 1;
    for (unsigned i = 0; i < out->count; i++) {
        inst_wifi_net_t *net = &out->nets[i];
        if (at >= len || payload[at] > INST_WIFI_SSID_MAX) return false;
        net->ssid_len = payload[at++];
        if (len - at < net->ssid_len) return false;
        memcpy(net->ssid, payload + at, net->ssid_len);
        at += net->ssid_len;
        if (at >= len || payload[at] > INST_WIFI_PASS_MAX) return false;
        net->pass_len = payload[at++];
        if (len - at < net->pass_len) return false;
        memcpy(net->pass, payload + at, net->pass_len);
        at += net->pass_len;
        if (!net_valid(net)) return false;
    }
    return at == len;
}

void inst_wifi_nvs_values(const inst_wifi_net_t *net, uint8_t ssid[INST_WIFI_NVS_SSID_LEN],
                          uint8_t pass[INST_WIFI_NVS_PASS_LEN])
{
    memset(ssid, 0, INST_WIFI_NVS_SSID_LEN);
    memset(pass, 0, INST_WIFI_NVS_PASS_LEN);
    ssid[0] = net->ssid_len;   // little-endian u32 length
    memcpy(ssid + 4, net->ssid, net->ssid_len);
    memcpy(pass, net->pass, net->pass_len);
}

size_t inst_wifi_encode_list(const inst_wifi_store_t *store, uint8_t result, uint8_t flags,
                             uint32_t ops, uint8_t *out, size_t cap)
{
    unsigned count = store ? store->count : 0;
    size_t need = 8;
    for (unsigned i = 0; i < count; i++) need += 1u + store->nets[i].ssid_len;
    if (need > cap) return 0;
    uint8_t *p = out;
    *p++ = 1;   // format
    *p++ = result;
    *p++ = flags;
    *p++ = (uint8_t)count;
    for (unsigned i = 0; i < 4; i++) *p++ = (uint8_t)(ops >> (8 * i));
    for (unsigned i = 0; i < count; i++) {
        *p++ = store->nets[i].ssid_len;
        memcpy(p, store->nets[i].ssid, store->nets[i].ssid_len);
        p += store->nets[i].ssid_len;
    }
    return (size_t)(p - out);
}
