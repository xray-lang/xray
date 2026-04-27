/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * os_random.h - Cross-platform cryptographically secure random.
 *
 * KEY CONCEPT:
 *   Single API xr_random_bytes(buf, len) that pulls from the
 *   system CSPRNG: BCryptGenRandom on Windows, arc4random_buf on
 *   macOS / BSD, getrandom on Linux (with /dev/urandom fallback
 *   for ancient kernels).
 *
 *   This is the only hardened randomness source the runtime and
 *   stdlib should use. A non-CSPRNG path is intentionally absent.
 */

#ifndef XRAY_OS_RANDOM_H
#define XRAY_OS_RANDOM_H

#include "../base/xdefs.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Fill buf with len cryptographically-secure random bytes.
// On CSPRNG failure (BCryptGenRandom returning non-zero,
// /dev/urandom unavailable, etc.) the process aborts with a
// diagnostic on stderr — silently proceeding would let crypto
// callers emit biased keys / IVs. Same contract as Go's
// crypto/rand and Rust's getrandom crate.
XR_FUNC void xr_random_bytes(unsigned char *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif  // XRAY_OS_RANDOM_H
