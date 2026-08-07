/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * cluster_auth.c - Backend-neutral cluster authentication kernel
 *
 * KEY CONCEPT:
 *   Every transport backend uses the same HMAC proof and constant-time
 *   comparison. Authentication never depends on a VM value representation.
 */

#include "cluster_internal.h"
#include "../crypto/crypto.h"
#include "../../src/base/xchecks.h"

#include <string.h>

void cluster_compute_proof(const char *secret, const uint8_t *nonce, uint8_t *proof_out) {
    XR_DCHECK(secret != NULL, "cluster proof requires a secret");
    XR_DCHECK(nonce != NULL, "cluster proof requires a nonce");
    XR_DCHECK(proof_out != NULL, "cluster proof requires an output buffer");
    if (!secret || !nonce || !proof_out)
        return;
    xr_hmac_sha256((const uint8_t *) secret, strlen(secret), nonce, XR_NONCE_SIZE, proof_out);
}

bool cluster_proof_equal(const uint8_t *a, const uint8_t *b) {
    if (!a || !b)
        return false;
    uint8_t diff = 0;
    for (size_t i = 0; i < XR_PROOF_SIZE; i++)
        diff |= (uint8_t) (a[i] ^ b[i]);
    return diff == 0;
}
