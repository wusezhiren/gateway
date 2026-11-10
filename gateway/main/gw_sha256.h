#pragma once

#include <stdint.h>
#include "psa/crypto.h"

typedef psa_hash_operation_t gw_sha256_ctx_t;

static inline void gw_sha256_init(gw_sha256_ctx_t *ctx)
{
    const psa_hash_operation_t initial = PSA_HASH_OPERATION_INIT;
    *ctx = initial;
}

static inline int gw_sha256_starts(gw_sha256_ctx_t *ctx)
{
    return psa_hash_setup(ctx, PSA_ALG_SHA_256) == PSA_SUCCESS ? 0 : -1;
}

static inline int gw_sha256_update(gw_sha256_ctx_t *ctx, const uint8_t *data, size_t len)
{
    return psa_hash_update(ctx, data, len) == PSA_SUCCESS ? 0 : -1;
}

static inline int gw_sha256_finish(gw_sha256_ctx_t *ctx, uint8_t out[32])
{
    size_t len = 0;
    return psa_hash_finish(ctx, out, 32, &len) == PSA_SUCCESS && len == 32 ? 0 : -1;
}

static inline void gw_sha256_free(gw_sha256_ctx_t *ctx)
{
    psa_hash_abort(ctx);
}
