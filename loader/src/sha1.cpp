#include "sha1.h"

#include <cstring>

namespace {

uint32_t rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

void block(Sha1Ctx *c, const uint8_t *p)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = c->state[0], b = c->state[1], cc = c->state[2], d = c->state[3], e = c->state[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & cc) | (~b & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ cc ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & cc) | (b & d) | (cc & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ cc ^ d;
            k = 0xCA62C1D6;
        }
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d;
        d = cc;
        cc = rol(b, 30);
        b = a;
        a = t;
    }
    c->state[0] += a;
    c->state[1] += b;
    c->state[2] += cc;
    c->state[3] += d;
    c->state[4] += e;
}

} // namespace

void sha1_init(Sha1Ctx *c)
{
    c->state[0] = 0x67452301;
    c->state[1] = 0xEFCDAB89;
    c->state[2] = 0x98BADCFE;
    c->state[3] = 0x10325476;
    c->state[4] = 0xC3D2E1F0;
    c->count_lo = c->count_hi = 0;
}

void sha1_update(Sha1Ctx *c, const uint8_t *data, size_t len)
{
    size_t used = c->count_lo & 63;
    uint32_t old = c->count_lo;
    c->count_lo += (uint32_t)len;
    if (c->count_lo < old) c->count_hi++;
    while (len) {
        size_t n = 64 - used < len ? 64 - used : len;
        memcpy(c->buf + used, data, n);
        used += n;
        data += n;
        len -= n;
        if (used == 64) {
            block(c, c->buf);
            used = 0;
        }
    }
}

void sha1_final(Sha1Ctx *c, uint8_t digest[20])
{
    uint64_t bits = ((uint64_t)c->count_hi << 32 | c->count_lo) * 8;
    uint8_t pad = 0x80;
    sha1_update(c, &pad, 1);
    uint8_t zero = 0;
    while ((c->count_lo & 63) != 56) sha1_update(c, &zero, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha1_update(c, len, 8);
    for (int i = 0; i < 5; i++) {
        digest[i * 4] = (uint8_t)(c->state[i] >> 24);
        digest[i * 4 + 1] = (uint8_t)(c->state[i] >> 16);
        digest[i * 4 + 2] = (uint8_t)(c->state[i] >> 8);
        digest[i * 4 + 3] = (uint8_t)c->state[i];
    }
}

void hmac_sha1(const uint8_t *key, size_t key_len, const uint8_t *d1, size_t l1, const uint8_t *d2, size_t l2, uint8_t out[20])
{
    uint8_t k[64] = {};
    Sha1Ctx c;
    if (key_len > 64) {
        sha1_init(&c);
        sha1_update(&c, key, key_len);
        sha1_final(&c, k);
    } else if (key_len) {
        memcpy(k, key, key_len);
    }
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; i++) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5C;
    }
    uint8_t inner[20];
    sha1_init(&c);
    sha1_update(&c, ipad, 64);
    if (d1 && l1) sha1_update(&c, d1, l1);
    if (d2 && l2) sha1_update(&c, d2, l2);
    sha1_final(&c, inner);
    sha1_init(&c);
    sha1_update(&c, opad, 64);
    sha1_update(&c, inner, 20);
    sha1_final(&c, out);
}
