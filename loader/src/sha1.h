#pragma once

#include <cstddef>
#include <cstdint>

// SHA-1 for the kernel's XcSHA* / XcHMAC exports. The context must fit in the caller's XcSHA buffer.
struct Sha1Ctx {
    uint32_t state[5];
    uint32_t count_lo, count_hi;
    uint8_t buf[64];
};
static_assert(sizeof(Sha1Ctx) <= 96, "must fit the Xbox SHA context buffer");

void sha1_init(Sha1Ctx *c);
void sha1_update(Sha1Ctx *c, const uint8_t *data, size_t len);
void sha1_final(Sha1Ctx *c, uint8_t digest[20]);
void hmac_sha1(const uint8_t *key, size_t key_len, const uint8_t *d1, size_t l1, const uint8_t *d2, size_t l2, uint8_t out[20]);
