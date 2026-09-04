/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * crypto.h - Runtime cryptographic support used by private providers
 *
 * KEY CONCEPT:
 *   The Xray-facing algorithms are defined only by stdlib/crypto/crypto.xr.
 *   This header exposes runtime/toolchain primitives used by private native
 *   leaves and internal cluster authentication.
 */

#ifndef XR_STDLIB_CRYPTO_H
#define XR_STDLIB_CRYPTO_H

#include "../../src/shared/xr_crypto_core.h"

#endif  // XR_STDLIB_CRYPTO_H
