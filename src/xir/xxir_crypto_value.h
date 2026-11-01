/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_crypto_value.h - Synchronous typed byte operations inside the owning Value implementation
 *
 * KEY CONCEPT:
 *   Typed observations stay inside the actual storage owner. Entropy filling
 *   completes every fallible preparation before changing any published bytes.
 */
#ifndef XXIR_CRYPTO_VALUE_H
#define XXIR_CRYPTO_VALUE_H
#include "xxir_cell_owner_internal.h"
typedef struct XirCryptoCommit {
    bool (*ready)(void *);
    void *context;
} XirCryptoCommit;
/* The callback is borrowed only until return, after all fallible preparation.
 * It observes execution status, never supplies entropy or write authority. */
XR_FUNC XrXirValueStatus xir_cell_crypto_fill(const XrXirValue *,const XrXirCellAuthority *,
    XrXirValueAdmission *,XrXirFaultDetail *,const XirCryptoCommit *);
XR_FUNC XrXirValueStatus xir_crypto_timing_equal(const XrXirValue *,const XrXirValue *,
    XrXirValueAdmission *,bool *);
#endif // XXIR_CRYPTO_VALUE_H
