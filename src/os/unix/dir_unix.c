/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdir_unix.c - POSIX implementation of xdir.h.
 *
 * Wraps DIR* / dirent. The iterator caches the directory path so
 * the is_dir fallback (when d_type is DT_UNKNOWN, e.g. some
 * network filesystems) can stat the full child path without the
 * caller having to plumb that path through.
 */

#include "../os_dir.h"

#ifndef _WIN32

#include "xmalloc.h"
#include <dirent.h>
#include <string.h>
#include <sys/stat.h>

struct XrDirIter {
    DIR *d;
    // Path-prefix buffer used to build the full path of each
    // entry when d_type is DT_UNKNOWN and we must stat() to
    // resolve directory-ness.
    char prefix[XR_DIR_ENTRY_NAME_MAX * 4];
    size_t prefix_len;
};

XrDirIter *xr_dir_open(const char *path) {
    if (!path || !*path)
        return NULL;
    DIR *d = opendir(path);
    if (!d)
        return NULL;
    XrDirIter *it = (XrDirIter *) xr_malloc(sizeof(*it));
    if (!it) {
        closedir(d);
        return NULL;
    }
    it->d = d;
    size_t plen = strlen(path);
    if (plen >= sizeof(it->prefix) - 1) {
        // Path too long to use for stat fallback. Truncating
        // would silently produce wrong is_dir; instead disable
        // the fallback by leaving prefix empty, which forces
        // is_dir = false when d_type is DT_UNKNOWN.
        it->prefix[0] = '\0';
        it->prefix_len = 0;
    } else {
        memcpy(it->prefix, path, plen);
        // Append trailing slash if missing.
        if (plen > 0 && it->prefix[plen - 1] != '/') {
            it->prefix[plen++] = '/';
        }
        it->prefix[plen] = '\0';
        it->prefix_len = plen;
    }
    return it;
}

bool xr_dir_next(XrDirIter *it, XrDirEntry *out) {
    if (!it || !out)
        return false;
    for (;;) {
        struct dirent *e = readdir(it->d);
        if (!e)
            return false;
        const char *n = e->d_name;
        if (n[0] == '.' && (n[1] == '\0' || (n[1] == '.' && n[2] == '\0')))
            continue;

        size_t nlen = strlen(n);
        if (nlen >= XR_DIR_ENTRY_NAME_MAX) {
            // Filename too long for our fixed buffer. Skip
            // rather than truncate to avoid surprising callers
            // with a partial name they will then mishandle.
            continue;
        }
        memcpy(out->name, n, nlen + 1);

        // is_dir resolution. DT_UNKNOWN happens on some FSes
        // (older XFS, some NFS mounts); fall back to stat() on
        // the joined path.
        out->is_dir = false;
#ifdef DT_DIR
        if (e->d_type == DT_DIR) {
            out->is_dir = true;
        } else if (e->d_type == DT_UNKNOWN && it->prefix_len > 0) {
#else
        if (it->prefix_len > 0) {
#endif
            char full[XR_DIR_ENTRY_NAME_MAX * 4];
            if (it->prefix_len + nlen < sizeof(full)) {
                memcpy(full, it->prefix, it->prefix_len);
                memcpy(full + it->prefix_len, n, nlen + 1);
                struct stat st;
                if (stat(full, &st) == 0 && S_ISDIR(st.st_mode))
                    out->is_dir = true;
            }
        }
        return true;
    }
}

void xr_dir_close(XrDirIter *it) {
    if (!it)
        return;
    if (it->d)
        closedir(it->d);
    xr_free(it);
}

#endif  // !_WIN32
