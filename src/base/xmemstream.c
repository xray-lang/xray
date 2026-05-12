/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmemstream.c - Cross-platform in-memory stdio stream
 *
 * IMPLEMENTATION:
 *   POSIX: Thin wrapper over open_memstream(3). The libc-malloc'd
 *          buffer is copied into an xr_malloc'd region at close so
 *          callers always release with xr_free().
 *   Windows: open_memstream() is missing from the MSVC CRT and there
 *          is no fopencookie/funopen equivalent to hook fclose. The
 *          backing store is tmpfile() (auto-deleted on close); at
 *          close time the file is rewound and read back into an
 *          xr_malloc'd buffer.
 */

#include "xmemstream.h"

#include <stdlib.h>
#include <string.h>

#include "xmalloc.h"

#if defined(_WIN32) || defined(XR_OS_WINDOWS)
#define XR_MEMSTREAM_WINDOWS 1
#endif

#ifdef XR_MEMSTREAM_WINDOWS

XR_FUNC FILE *xr_open_memstream(char **outbuf, size_t *outsize) {
    if (!outbuf || !outsize)
        return NULL;
    *outbuf = NULL;
    *outsize = 0;
    /* tmpfile() opens a binary read-write file that is deleted on
     * close. Sufficient as a backing store; only the final readback
     * matters for correctness. */
    return tmpfile();
}

XR_FUNC int xr_close_memstream(FILE *stream, char **outbuf, size_t *outsize) {
    if (!stream || !outbuf || !outsize) {
        if (stream)
            fclose(stream);
        if (outbuf)
            *outbuf = NULL;
        if (outsize)
            *outsize = 0;
        return -1;
    }
    *outbuf = NULL;
    *outsize = 0;

    if (fflush(stream) != 0) {
        fclose(stream);
        return -1;
    }
    if (fseek(stream, 0, SEEK_END) != 0) {
        fclose(stream);
        return -1;
    }
    long end = ftell(stream);
    if (end < 0) {
        fclose(stream);
        return -1;
    }
    rewind(stream);

    size_t size = (size_t) end;
    char *buf = (char *) xr_malloc(size + 1);
    if (!buf) {
        fclose(stream);
        return -1;
    }
    size_t nread = (size > 0) ? fread(buf, 1, size, stream) : 0;
    buf[nread] = '\0';

    /* fclose may report a deferred IO error; we still own the buffer. */
    int rc = fclose(stream);
    *outbuf = buf;
    *outsize = nread;
    return (rc == 0) ? 0 : -1;
}

#else /* POSIX */

XR_FUNC FILE *xr_open_memstream(char **outbuf, size_t *outsize) {
    if (!outbuf || !outsize)
        return NULL;
    return open_memstream(outbuf, outsize);
}

XR_FUNC int xr_close_memstream(FILE *stream, char **outbuf, size_t *outsize) {
    if (!stream || !outbuf || !outsize) {
        if (stream)
            fclose(stream);
        if (outbuf)
            *outbuf = NULL;
        if (outsize)
            *outsize = 0;
        return -1;
    }
    /* fclose finalizes the open_memstream buffer pointed to by
     * *outbuf / *outsize (libc-malloc'd). */
    if (fclose(stream) != 0) {
        if (*outbuf) {
            free(*outbuf);
            *outbuf = NULL;
        }
        *outsize = 0;
        return -1;
    }
    if (!*outbuf) {
        *outsize = 0;
        return -1;
    }
    /* Copy into xr_malloc territory so callers can xr_free() uniformly
     * across platforms. */
    size_t size = *outsize;
    char *owned = (char *) xr_malloc(size + 1);
    if (!owned) {
        free(*outbuf);
        *outbuf = NULL;
        *outsize = 0;
        return -1;
    }
    memcpy(owned, *outbuf, size);
    owned[size] = '\0';
    free(*outbuf);
    *outbuf = owned;
    *outsize = size;
    return 0;
}

#endif /* XR_MEMSTREAM_WINDOWS */
