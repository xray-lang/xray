/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmemstream.h - Cross-platform in-memory stdio stream
 *
 * KEY CONCEPT:
 *   Stand-in for POSIX open_memstream(3), which is unavailable on
 *   Windows / MSVC. Caller writes through a FILE* and gets back a
 *   contiguous xr_malloc'd buffer at close time.
 *
 *   Unlike POSIX open_memstream(), the returned buffer is finalized
 *   only after xr_close_memstream() returns. Reading *outbuf while
 *   the stream is still open is NOT supported on Windows; callers
 *   must treat the buffer as write-only until close.
 *
 *   The output buffer is always xr_malloc-owned. Caller releases
 *   it with xr_free() on every platform.
 *
 * USAGE:
 *   char *buf = NULL;
 *   size_t bufsz = 0;
 *   FILE *mem = xr_open_memstream(&buf, &bufsz);
 *   if (!mem) return error;
 *   fprintf(mem, "hello %s", "world");
 *   if (xr_close_memstream(mem, &buf, &bufsz) != 0) return error;
 *   // use buf, bufsz
 *   xr_free(buf);
 */

#ifndef XMEMSTREAM_H
#define XMEMSTREAM_H

#include <stddef.h>
#include <stdio.h>

#include "xdefs.h"

/* Open an in-memory stdio stream. *outbuf / *outsize are populated
 * after xr_close_memstream() succeeds. Returns NULL on failure. */
XR_FUNC FILE *xr_open_memstream(char **outbuf, size_t *outsize);

/* Finalize the stream opened by xr_open_memstream(). Writes the final
 * buffer pointer and size into *outbuf / *outsize. The buffer is
 * xr_malloc-owned; caller releases with xr_free(). Returns 0 on
 * success, -1 on failure (in which case *outbuf is set to NULL). */
XR_FUNC int xr_close_memstream(FILE *stream, char **outbuf, size_t *outsize);

#endif  // XMEMSTREAM_H
