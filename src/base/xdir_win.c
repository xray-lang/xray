/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdir_win.c - Windows implementation of xdir.h.
 *
 * FindFirstFileA / FindNextFileA wrap-and-skip-dot-entries
 * iterator. The first FindFirstFile call already returns the
 * first entry, so we need a "primed" flag to defer that hand-off
 * to the first xr_dir_next.
 *
 * UTF-8 note: callers pass UTF-8 paths. The narrow FindFirstFileA
 * uses the ANSI code page, which is not UTF-8 on most Windows
 * deployments, so non-ASCII paths are not round-trip-safe in this
 * implementation. A wide-char (-W) variant with MultiByteToWideChar
 * conversion is the right long-term fix; tracked separately.
 */

#include "xdir.h"

#ifdef _WIN32

#include "xmalloc.h"
#include <string.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

struct XrDirIter {
    HANDLE h;
    WIN32_FIND_DATAA data;
    bool primed;  // true if `data` already holds the next entry
};

XrDirIter *xr_dir_open(const char *path) {
    if (!path || !*path)
        return NULL;

    // FindFirstFile expects a glob pattern; append "\\*".
    char pattern[XR_DIR_ENTRY_NAME_MAX * 4];
    size_t plen = strlen(path);
    if (plen + 3 >= sizeof(pattern))
        return NULL;
    memcpy(pattern, path, plen);
    if (plen > 0 && pattern[plen - 1] != '\\' && pattern[plen - 1] != '/')
        pattern[plen++] = '\\';
    pattern[plen++] = '*';
    pattern[plen] = '\0';

    XrDirIter *it = (XrDirIter *) xr_malloc(sizeof(*it));
    if (!it)
        return NULL;

    it->h = FindFirstFileA(pattern, &it->data);
    if (it->h == INVALID_HANDLE_VALUE) {
        xr_free(it);
        return NULL;
    }
    it->primed = true;
    return it;
}

bool xr_dir_next(XrDirIter *it, XrDirEntry *out) {
    if (!it || !out)
        return false;

    for (;;) {
        if (!it->primed) {
            if (!FindNextFileA(it->h, &it->data))
                return false;
        }
        it->primed = false;

        const char *n = it->data.cFileName;
        if (n[0] == '.' && (n[1] == '\0' || (n[1] == '.' && n[2] == '\0')))
            continue;

        size_t nlen = strlen(n);
        if (nlen >= XR_DIR_ENTRY_NAME_MAX)
            continue;
        memcpy(out->name, n, nlen + 1);
        out->is_dir = (it->data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        return true;
    }
}

void xr_dir_close(XrDirIter *it) {
    if (!it)
        return;
    if (it->h != INVALID_HANDLE_VALUE)
        FindClose(it->h);
    xr_free(it);
}

#endif  // _WIN32
