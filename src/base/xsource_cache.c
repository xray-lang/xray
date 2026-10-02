/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xsource_cache.c - Source code cache implementation
 *
 * KEY CONCEPT:
 *   Caches source files for runtime error display with line-by-line access.
 */

#include "xsource_cache.h"
#include <limits.h>

static void source_file_close(XrSourceFile *file) {
    xr_compile_state_free(file->path);
    xr_compile_state_free(file->content);
    xr_compile_state_free(file->lines);
}

static bool cache_work(const XrSourceCache *cache, bool compiling, uint64_t amount) {
    return !compiling || xr_compile_state_work(cache->state, amount) == XR_COMPILE_RESOURCE_OK;
}

/* One lookup algorithm with an explicit execution domain, never a NULL policy. */
static const XrSourceFile *find_file(const XrSourceCache *cache, const char *path, bool compiling) {
    if (!cache || !path) return NULL;
    for (int i = 0; i < cache->file_count; ++i) {
        if (!cache_work(cache, compiling, 1)) return NULL;
        const char *a = cache->files[i].path, *b = path;
        for (;;) {
            if (!cache_work(cache, compiling, 2)) return NULL;
            unsigned char left = (unsigned char) *a++, right = (unsigned char) *b++;
            if (left != right) break;
            if (!left) return &cache->files[i];
        }
    }
    return NULL;
}

static XrCompileResourceStatus parse_lines(XrCompileState *state, XrSourceFile *file) {
    int count = 1;
    for (const char *p = file->content;; ++p) {
        if (xr_compile_state_work(state, 1) != XR_COMPILE_RESOURCE_OK) return xr_compile_state_status(state);
        char c = *p;
        if (!c) break;
        if (c == '\n') {
            if (count == INT_MAX) return xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BUDGET);
            ++count;
        }
    }
    void *memory = NULL;
    if ((size_t) count > SIZE_MAX / sizeof(char *)) return xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BUDGET);
    XrCompileResourceStatus status = xr_compile_state_alloc(state, (size_t) count * sizeof(char *), &memory);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    file->lines = memory;
    const char *start = file->content;
    for (const char *p = start;; ++p) {
        if (xr_compile_state_work(state, 1) != XR_COMPILE_RESOURCE_OK) return xr_compile_state_status(state);
        char c = *p;
        if (c == '\n' || !c) {
            if (xr_compile_state_work(state, sizeof(char *)) != XR_COMPILE_RESOURCE_OK) return xr_compile_state_status(state);
            file->lines[file->line_count++] = (char *) start;
            if (!c) break;
            start = p + 1;
        }
    }
    return XR_COMPILE_RESOURCE_OK;
}

XrCompileResourceStatus xr_compile_source_cache_open(XrCompileState *state, XrSourceCache **output) {
    XrCompileResourceStatus status = xr_compile_state_status(state);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    if (!output || *output) return xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    status = xr_compile_state_retain(state);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    void *memory = NULL;
    status = xr_compile_state_calloc(state, 1, sizeof(XrSourceCache), &memory);
    if (status != XR_COMPILE_RESOURCE_OK) {
        xr_compile_state_release(state);
        return status;
    }
    XrSourceCache *cache = memory;
    cache->state = state;
    *output = cache;
    return XR_COMPILE_RESOURCE_OK;
}

void xr_owned_source_cache_close(XrSourceCache *cache) {
    if (!cache) return;
    XrCompileState *state = cache->state;
    for (int i = 0; i < cache->file_count; ++i) source_file_close(&cache->files[i]);
    xr_compile_state_free(cache->files);
    xr_compile_state_free(cache);
    xr_compile_state_release(state);
}

XrCompileResourceStatus xr_compile_source_cache_add(XrSourceCache *cache, const char *path, const char *content) {
    if (!cache) return XR_COMPILE_RESOURCE_BAD_ARGUMENT;
    XrCompileState *state = cache->state;
    XrCompileResourceStatus status = xr_compile_state_status(state);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    if (!path || !content) return xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    if (find_file(cache, path, true)) return XR_COMPILE_RESOURCE_OK;
    if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK) return xr_compile_state_status(state);
    XrSourceFile file = {0};
    status = xr_compile_state_strdup(state, path, &file.path);
    if (status != XR_COMPILE_RESOURCE_OK) goto failure;
    status = xr_compile_state_strdup(state, content, &file.content);
    if (status != XR_COMPILE_RESOURCE_OK) goto failure;
    status = parse_lines(state, &file);
    if (status != XR_COMPILE_RESOURCE_OK) goto failure;
    if (cache->file_count == cache->file_capacity) {
        if (cache->file_capacity > INT_MAX / 2) {
            status = xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BUDGET);
            goto failure;
        }
        int capacity = cache->file_capacity ? cache->file_capacity * 2 : 4;
        if ((size_t) capacity > SIZE_MAX / sizeof(XrSourceFile)) {
            status = xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BUDGET);
            goto failure;
        }
        void *memory = cache->files;
        status = xr_compile_state_resize(state, &memory, (size_t) capacity * sizeof(XrSourceFile));
        if (status != XR_COMPILE_RESOURCE_OK) goto failure;
        cache->files = memory;
        cache->file_capacity = capacity;
    }
    status = xr_compile_state_copy(state, &cache->files[cache->file_count], &file, sizeof(file));
    if (status != XR_COMPILE_RESOURCE_OK) goto failure;
    ++cache->file_count;
    return XR_COMPILE_RESOURCE_OK;
failure:
    source_file_close(&file);
    return status;
}

const char *xr_runtime_source_cache_get_line(const XrSourceCache *cache, const char *path, int line) {
    const XrSourceFile *file = find_file(cache, path, false);
    return file && line >= 1 && line <= file->line_count ? file->lines[line - 1] : NULL;
}

int xr_runtime_source_cache_get_line_length(const XrSourceCache *cache, const char *path, int line) {
    const char *start = xr_runtime_source_cache_get_line(cache, path, line);
    if (!start) return 0;
    int length = 0;
    while (start[length] && start[length] != '\n' && start[length] != '\r') {
        if (length == INT_MAX) return INT_MAX;
        ++length;
    }
    return length;
}
