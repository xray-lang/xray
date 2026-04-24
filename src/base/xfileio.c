/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xfileio.c - File I/O and path utilities implementation
 */

#include "xfileio.h"
#include "xmalloc.h"
#include "xchecks.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char* xr_file_read_all(const char *path, const char *mode, size_t *out_size) {
    if (!path || !mode) return NULL;

    FILE *f = fopen(path, mode);
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    XR_DCHECK(size >= 0, "xr_file_read_all: ftell returned negative");
    fseek(f, 0, SEEK_SET);

    if (size < 0) {
        fclose(f);
        return NULL;
    }

    char *buf = (char*)xr_malloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }

    size_t n = fread(buf, 1, (size_t)size, f);
    buf[n] = '\0';
    fclose(f);

    if (out_size) *out_size = n;
    return buf;
}

char* xr_path_dirname(const char *path) {
    if (!path) return xr_strdup(".");

    const char *last_slash = strrchr(path, '/');
    if (!last_slash) return xr_strdup(".");

    /* Handle root "/" */
    if (last_slash == path) return xr_strdup("/");

    size_t len = (size_t)(last_slash - path);
    char *dir = (char*)xr_malloc(len + 1);
    if (!dir) return NULL;

    memcpy(dir, path, len);
    dir[len] = '\0';
    return dir;
}

char* xr_path_join(const char *dir, const char *name) {
    if (!dir || !name) return NULL;

    size_t dir_len = strlen(dir);
    size_t name_len = strlen(name);

    /* Strip trailing slashes from dir */
    while (dir_len > 0 && dir[dir_len - 1] == '/') {
        dir_len--;
    }

    /* Handle empty dir after stripping */
    if (dir_len == 0) return xr_strdup(name);

    char *result = (char*)xr_malloc(dir_len + 1 + name_len + 1);
    if (!result) return NULL;

    memcpy(result, dir, dir_len);
    result[dir_len] = '/';
    memcpy(result + dir_len + 1, name, name_len);
    result[dir_len + 1 + name_len] = '\0';
    return result;
}

char* xr_path_basename(const char *path) {
    if (!path || !*path) return xr_strdup(".");

    size_t len = strlen(path);

    /* Strip trailing slashes */
    while (len > 1 && path[len - 1] == '/') {
        len--;
    }

    /* Root "/" */
    if (len == 1 && path[0] == '/') return xr_strdup("/");

    const char *end = path + len;
    const char *start = end;
    while (start > path && *(start - 1) != '/') {
        start--;
    }

    size_t blen = (size_t)(end - start);
    char *result = (char*)xr_malloc(blen + 1);
    if (!result) return NULL;

    memcpy(result, start, blen);
    result[blen] = '\0';
    return result;
}

char* xr_realpath(const char *path) {
    if (!path) return NULL;

    char *rp = realpath(path, NULL);
    if (!rp) return NULL;

    char *dup = xr_strdup(rp);
    free(rp);  /* realpath uses system malloc */
    return dup;
}
