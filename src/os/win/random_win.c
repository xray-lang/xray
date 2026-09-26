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

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>

#pragma comment(lib, "bcrypt.lib")

#include <stdio.h>
#include <stdlib.h>
#include <limits.h>

void xr_random_bytes(unsigned char *buf, size_t len) {
    /* The public length is size_t; each OS request is bounded by ULONG.
     * Never report success after filling only a truncated prefix. */
    while (len != 0u) {
        ULONG chunk = len > ULONG_MAX ? ULONG_MAX : (ULONG)len;
        if (BCryptGenRandom(NULL, buf, chunk, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
            fprintf(stderr,
                    "xray: CSPRNG failure (BCryptGenRandom); aborting to avoid biased output\n");
            abort();
        }
        buf += chunk;
        len -= chunk;
    }
}
