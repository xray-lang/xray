/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * common_io.h - Shared blocking file helpers for stdlib parseFile /
 * writeFile bindings (json / yaml / toml / xml / csv).
 *
 * KEY CONCEPT:
 *   Today these helpers are synchronous (`fopen / fread / fwrite /
 *   fclose`). They run on the calling coroutine's worker thread and
 *   will block it on slow storage. That is a deliberate, documented
 *   trade-off: full `XrAsyncPool` integration would require turning
 *   every parseFile / writeFile binding into a yieldable C function
 *   with XrAsyncJob continuations and rewriting the per-coro IO state
 *   in `src/coro/*`.
 *
 *   By funnelling all four modules through this single helper we
 *   guarantee:
 *     - identical error handling + NUL-termination semantics;
 *     - identical buffer ownership (xr_malloc so callers xr_free);
 *     - a single swap-point when the async migration finally lands.
 *       Replacing `xrs_file_read_all_sync` with an async version and
 *       flipping each binding to yieldable C functions will not
 *       require touching the parse / serialise logic itself.
 *
 * USAGE:
 *     char *buf = NULL;
 *     size_t len = 0;
 *     if (xrs_file_read_all_sync(path, &buf, &len)) {
 *         // Use buf[0..len), NUL-terminated at buf[len].
 *         ...
 *         xr_free(buf);
 *     }
 *
 *     bool ok = xrs_file_write_all_sync(path, data, data_len);
 */

#ifndef XR_STDLIB_COMMON_IO_H
#define XR_STDLIB_COMMON_IO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include "../src/base/xmalloc.h"

// Read the entire file at `path` into a freshly-xr_malloc'd, NUL-
// terminated buffer. On success, `*out_data` points at the contents
// (ownership transferred to the caller — free with `xr_free`) and
// `*out_len` is the file length in bytes.
//
// Returns true on success, false on any I/O failure. Failure modes
// leave `*out_data = NULL` and `*out_len = 0`. No partial buffer is
// returned.
//
// NOTE: synchronous. Blocks the worker thread for large files. See the
// header comment for the P9 plan.
static inline bool xrs_file_read_all_sync(const char *path,
                                          char **out_data,
                                          size_t *out_len)
{
    if (!path || !out_data || !out_len) return false;
    *out_data = NULL;
    *out_len = 0;

    FILE *f = fopen(path, "rb");
    if (!f) return false;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
    long raw = ftell(f);
    if (raw < 0) { fclose(f); return false; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return false; }

    size_t size = (size_t)raw;
    char *buf = (char *)xr_malloc(size + 1);
    if (!buf) { fclose(f); return false; }

    size_t got = fread(buf, 1, size, f);
    int read_err = ferror(f);
    fclose(f);

    if (got != size || read_err) {
        xr_free(buf);
        return false;
    }

    buf[size] = '\0';
    *out_data = buf;
    *out_len = size;
    return true;
}

// Write `len` bytes from `data` to `path`, truncating / creating the
// file. Returns true if the full payload was flushed and the file was
// closed cleanly. On any failure mid-write the file may remain in a
// partial state — this matches Python's `open(path,"wb").write()`
// semantics and avoids a temp-file dance the stdlib does not need for
// its current use cases.
//
// NOTE: synchronous. See the header comment for the P9 plan.
static inline bool xrs_file_write_all_sync(const char *path,
                                           const char *data, size_t len)
{
    if (!path) return false;
    // Allow data==NULL only when len==0 (empty file create).
    if (!data && len != 0) return false;

    FILE *f = fopen(path, "wb");
    if (!f) return false;

    bool ok = true;
    if (len > 0) {
        size_t wrote = fwrite(data, 1, len, f);
        if (wrote != len) ok = false;
    }
    if (fclose(f) != 0) ok = false;
    return ok;
}

#endif // XR_STDLIB_COMMON_IO_H
