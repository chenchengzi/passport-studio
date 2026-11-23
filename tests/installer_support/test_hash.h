// tests/installer_support/test_hash.h
// Small reference MD5 and SHA-256 for host tests only (RFC 1321, FIPS 180-4).
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static inline uint32_t th_rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
static inline uint32_t th_ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static inline void test_md5(const uint8_t *data, size_t len, uint8_t out[16])
{
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    static const int R[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                              5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
                              4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                              6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    uint32_t h[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    size_t total = ((len + 8) / 64 + 1) * 64;
    for (size_t off = 0; off < total; off += 64) {
        uint8_t block[64];
        for (size_t i = 0; i < 64; i++) {
            size_t at = off + i;
            block[i] = at < len ? data[at] : (at == len ? 0x80 : 0);
        }
        if (off + 64 == total) {
            uint64_t bits = (uint64_t)len * 8;
            for (int i = 0; i < 8; i++) block[56 + i] = (uint8_t)(bits >> (8 * i));
        }
        uint32_t w[16];
        for (int i = 0; i < 16; i++) {
            w[i] = (uint32_t)block[i * 4] | (uint32_t)block[i * 4 + 1] << 8 |
                   (uint32_t)block[i * 4 + 2] << 16 | (uint32_t)block[i * 4 + 3] << 24;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        for (int i = 0; i < 64; i++) {
            uint32_t f;
            int g;
            if (i < 16) { f = (b & c) | (~b & d); g = i; }
            else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
            else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) % 16; }
            else { f = c ^ (b | ~d); g = (7 * i) % 16; }
            uint32_t tmp = d;
            d = c;
            c = b;
            b = b + th_rol(a + f + K[i] + w[g], R[i]);
            a = tmp;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    }
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) out[i * 4 + j] = (uint8_t)(h[i] >> (8 * j));
    }
}

typedef struct {
    uint32_t h[8];
    uint64_t total;
    uint8_t buf[64];
    size_t used;
} test_sha256_t;

static inline void th_sha_block(test_sha256_t *c, const uint8_t *p)
{
    static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 |
               (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = th_ror(w[i - 15], 7) ^ th_ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = th_ror(w[i - 2], 17) ^ th_ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3];
    uint32_t e = c->h[4], f = c->h[5], g = c->h[6], h = c->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (th_ror(e, 6) ^ th_ror(e, 11) ^ th_ror(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint32_t t2 = (th_ror(a, 2) ^ th_ror(a, 13) ^ th_ror(a, 22)) + ((a & b) ^ (a & cc) ^ (b & cc));
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

static inline void test_sha256_start(test_sha256_t *c)
{
    static const uint32_t init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(c->h, init, sizeof(init));
    c->total = 0;
    c->used = 0;
}

static inline void test_sha256_update(test_sha256_t *c, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    c->total += len;
    while (len) {
        size_t n = 64 - c->used < len ? 64 - c->used : len;
        memcpy(c->buf + c->used, p, n);
        c->used += n;
        p += n;
        len -= n;
        if (c->used == 64) {
            th_sha_block(c, c->buf);
            c->used = 0;
        }
    }
}

static inline void test_sha256_finish(test_sha256_t *c, uint8_t out[32])
{
    uint64_t bits = c->total * 8;
    uint8_t pad = 0x80;
    test_sha256_update(c, &pad, 1);
    pad = 0;
    while (c->used != 56) test_sha256_update(c, &pad, 1);
    uint8_t len_be[8];
    for (int i = 0; i < 8; i++) len_be[i] = (uint8_t)(bits >> (56 - 8 * i));
    test_sha256_update(c, len_be, 8);
    for (int i = 0; i < 8; i++) {
        out[i * 4] = (uint8_t)(c->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(c->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(c->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)c->h[i];
    }
}

static inline void test_sha256(const void *data, size_t len, uint8_t out[32])
{
    test_sha256_t c;
    test_sha256_start(&c);
    test_sha256_update(&c, data, len);
    test_sha256_finish(&c, out);
}
