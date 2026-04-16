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
#include "xchecks.h"
#include "xmalloc.h"
#include <string.h>
#include <stdlib.h>

/* ========== Internal Functions ========== */

// Parse content into line array
static void parse_lines(XrSourceFile *file) {
    if (!file->content) {
        file->lines = NULL;
        file->line_count = 0;
        return;
    }
    
    // Count lines
    int count = 1;
    for (char *p = file->content; *p; p++) {
        if (*p == '\n') count++;
    }
    
    // Allocate line pointer array
    file->lines = (char**)xr_malloc(sizeof(char*) * count);
    if (!file->lines) {
        file->line_count = 0;
        return;
    }
    
    // Fill line pointers
    file->line_count = 0;
    char *start = file->content;
    for (char *p = file->content; ; p++) {
        if (*p == '\n' || *p == '\0') {
            file->lines[file->line_count++] = start;
            if (*p == '\0') break;
            start = p + 1;
        }
    }
}

// Find file by path
static XrSourceFile* find_file(XrSourceCache *cache, const char *path) {
    if (!cache || !path) return NULL;
    
    for (int i = 0; i < cache->file_count; i++) {
        if (cache->files[i].path && strcmp(cache->files[i].path, path) == 0) {
            return &cache->files[i];
        }
    }
    return NULL;
}

/* ========== API Implementation ========== */

XrSourceCache* xr_source_cache_new(void) {
    XrSourceCache *cache = (XrSourceCache*)xr_malloc(sizeof(XrSourceCache));
    if (!cache) return NULL;
    
    cache->files = NULL;
    cache->file_count = 0;
    cache->file_capacity = 0;
    
    return cache;
}

void xr_source_cache_free(XrSourceCache *cache) {
    if (!cache) return;
    
    for (int i = 0; i < cache->file_count; i++) {
        xr_free(cache->files[i].path);
        xr_free(cache->files[i].content);
        xr_free(cache->files[i].lines);
    }
    xr_free(cache->files);
    xr_free(cache);
}

bool xr_source_cache_add(XrSourceCache *cache, const char *path, const char *content) {
    if (!cache || !path || !content) return false;
    
    // Check if already exists
    if (find_file(cache, path)) return true;
    
    // Expand capacity
    if (cache->file_count >= cache->file_capacity) {
        int new_cap = cache->file_capacity == 0 ? 4 : cache->file_capacity * 2;
        size_t new_size = sizeof(XrSourceFile) * new_cap;
        XrSourceFile *new_files = (XrSourceFile*)xr_realloc(cache->files, new_size);
        if (!new_files) return false;
        cache->files = new_files;
        cache->file_capacity = new_cap;
    }
    
    // Add new file
    XrSourceFile *file = &cache->files[cache->file_count];
    size_t path_len = strlen(path);
    file->path = (char*)xr_malloc(path_len + 1);
    if (file->path) memcpy(file->path, path, path_len + 1);

    size_t content_len = strlen(content);
    file->content = (char*)xr_malloc(content_len + 1);
    if (file->content) memcpy(file->content, content, content_len + 1);
    
    if (!file->path || !file->content) {
        xr_free(file->path);
        xr_free(file->content);
        return false;
    }
    
    // Parse lines
    parse_lines(file);
    
    cache->file_count++;
    return true;
}

const char* xr_source_cache_get_line(XrSourceCache *cache, const char *path, int line) {
    XrSourceFile *file = find_file(cache, path);
    if (!file || !file->lines) return NULL;
    
    // Line number is 1-indexed
    if (line < 1 || line > file->line_count) return NULL;
    
    return file->lines[line - 1];
}

int xr_source_cache_get_line_length(XrSourceCache *cache, const char *path, int line) {
    XrSourceFile *file = find_file(cache, path);
    if (!file || !file->lines) return 0;
    
    // Line number is 1-indexed
    if (line < 1 || line > file->line_count) return 0;
    
    const char *start = file->lines[line - 1];
    const char *end = start;
    while (*end && *end != '\n' && *end != '\r') end++;
    
    return (int)(end - start);
}
