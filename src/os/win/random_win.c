/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * random_win.c - Windows implementation of os_random.h.
 *
 * BCryptGenRandom with BCRYPT_USE_SYSTEM_PREFERRED_RNG is the
 * recommended modern API — fast, never blocks, FIPS-validatable.
 */

#include "../os_random.h"

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>

#pragma comment(lib, "bcrypt.lib")

#include <stdio.h>
#include <stdlib.h>

void xr_random_bytes(unsigned char *buf, size_t len) {
    // Truncate len to ULONG range; len > 4GiB is nonsensical for
    // a single CSPRNG call. STATUS_SUCCESS is 0; any non-zero
    // NTSTATUS means failure and we must not proceed with biased
    // output (crypto IVs / keys would silently weaken).
    if (BCryptGenRandom(NULL, buf, (ULONG) len, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        fprintf(stderr,
                "xray: CSPRNG failure (BCryptGenRandom); aborting to avoid biased output\n");
        abort();
    }
}

#endif  // _WIN32
