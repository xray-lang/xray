/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * random_unix.c - POSIX implementation of os_random.h.
 *
 * macOS / BSD route to arc4random_buf (always non-blocking,
 * never fails). Linux uses getrandom(); pre-3.17 kernels lacking
 * the syscall fall back to reading /dev/urandom.
 */

#include "../os_random.h"

#ifndef _WIN32

#include "../../base/xplatform.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>  // abort

#ifdef XR_OS_MACOS
// arc4random_buf lives in <stdlib.h>, already included above.
#elif defined(XR_OS_BSD)
// arc4random_buf lives in <stdlib.h>, already included above.
#else
#include <sys/random.h>  // getrandom
#endif

static void xr_random_die(const char *what) {
    fprintf(stderr, "xray: CSPRNG failure (%s); aborting to avoid biased output\n", what);
    abort();
}

void xr_random_bytes(unsigned char *buf, size_t len) {
#if defined(XR_OS_MACOS) || defined(XR_OS_BSD)
    arc4random_buf(buf, len);
#else
    size_t off = 0;
    while (off < len) {
        ssize_t n = getrandom(buf + off, len - off, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            // Older kernel without the syscall, or transient
            // failure: fall back to /dev/urandom for the
            // remaining bytes. Abort on any fallback failure —
            // crypto callers must never proceed with partial data.
            FILE *f = fopen("/dev/urandom", "rb");
            if (!f)
                xr_random_die("/dev/urandom open");
            size_t got = fread(buf + off, 1, len - off, f);
            fclose(f);
            if (got != len - off)
                xr_random_die("/dev/urandom short read");
            return;
        }
        off += (size_t) n;
    }
#endif
}

#endif  // !_WIN32
