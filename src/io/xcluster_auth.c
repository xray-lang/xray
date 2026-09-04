/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcluster_auth.c - Native handshake proof projection
 *
 * KEY CONCEPT:
 *   Every transport backend uses the same HMAC proof and constant-time
 *   comparison. Authentication never depends on a VM value representation.
 */

#include "xcluster_auth.h"
#include "xcluster_wire.h"
#include "../base/xchecks.h"
#include "../shared/xr_crypto_core.h"

#include <string.h>

void xr_cluster_auth_compute_proof(const char *secret, const uint8_t *nonce, uint8_t *proof_out) {
    XR_DCHECK(secret != NULL, "cluster proof requires a secret");
    XR_DCHECK(nonce != NULL, "cluster proof requires a nonce");
    XR_DCHECK(proof_out != NULL, "cluster proof requires an output buffer");
    if (!secret || !nonce || !proof_out)
        return;
    xr_hmac_sha256((const uint8_t *) secret, strlen(secret), nonce, XR_NONCE_SIZE, proof_out);
}

bool xr_cluster_auth_proof_equal(const uint8_t *a, const uint8_t *b) {
    if (!a || !b)
        return false;
    uint8_t diff = 0;
    for (size_t i = 0; i < XR_PROOF_SIZE; i++)
        diff |= (uint8_t) (a[i] ^ b[i]);
    return diff == 0;
}
