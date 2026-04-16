/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcli_utils.c - Common utility functions for CLI commands
 *
 * KEY CONCEPT:
 *   Provides file I/O and string utilities shared across CLI subcommands.
 */

#include "xcli_utils.h"
#include "xray_isolate.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <limits.h>
#include <errno.h>
#include <time.h>
#include "../../base/xmalloc.h"

char* cli_read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size < 0) {
        fclose(f);
        return NULL;
    }
    fseek(f, 0, SEEK_SET);
    
    char *content = (char*)xr_malloc((size_t)size + 1);
    if (!content) {
        fclose(f);
        return NULL;
    }
    
    size_t read = fread(content, 1, size, f);
    content[read] = '\0';
    fclose(f);
    
    return content;
}

int cli_write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    
    size_t len = strlen(content);
    size_t written = fwrite(content, 1, len, f);
    fclose(f);
    
    return written == len ? 0 : -1;
}

bool cli_file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

bool cli_is_xr_file(const char *filename) {
    size_t len = strlen(filename);
    if (len < 4) return false;
    return strcmp(filename + len - 3, ".xr") == 0;
}

bool cli_is_directory(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return false;
    return S_ISDIR(st.st_mode);
}

// Levenshtein edit distance (single-row DP, O(min(m,n)) space).
// Command names are short (<16 chars) so this is effectively free.
int cli_string_distance(const char *s1, const char *s2) {
    int len1 = (int)strlen(s1);
    int len2 = (int)strlen(s2);
    
    if (len1 == 0) return len2;
    if (len2 == 0) return len1;
    
    // Ensure len1 <= len2 to minimize row allocation
    if (len1 > len2) {
        const char *tmp_s = s1; s1 = s2; s2 = tmp_s;
        int tmp_n = len1; len1 = len2; len2 = tmp_n;
    }
    
    // Stack-allocate row (command names never exceed 32 chars)
    int row[33];
    for (int j = 0; j <= len1; j++) row[j] = j;
    
    for (int i = 1; i <= len2; i++) {
        int prev = row[0];
        row[0] = i;
        for (int j = 1; j <= len1; j++) {
            int cost = (s1[j - 1] == s2[i - 1]) ? 0 : 1;
            int val = prev + cost;                         // substitution
            if (row[j] + 1 < val) val = row[j] + 1;      // deletion
            if (row[j - 1] + 1 < val) val = row[j - 1] + 1; // insertion
            prev = row[j];
            row[j] = val;
        }
    }
    return row[len1];
}

char* cli_read_stdin(void) {
    size_t capacity = 4096;
    size_t length = 0;
    char *buf = (char*)xr_malloc(capacity);
    if (!buf) return NULL;

    size_t n;
    while ((n = fread(buf + length, 1, capacity - length - 1, stdin)) > 0) {
        length += n;
        if (length + 1 >= capacity) {
            capacity *= 2;
            char *newbuf = (char*)xr_realloc(buf, capacity);
            if (!newbuf) {
                xr_free(buf);
                return NULL;
            }
            buf = newbuf;
        }
    }
    buf[length] = '\0';
    return buf;
}

bool cli_parse_int(const char *str, int *out) {
    if (!str || !*str) return false;
    
    char *endptr;
    long val = strtol(str, &endptr, 10);
    
    // Check for conversion errors
    if (endptr == str || *endptr != '\0') {
        return false;
    }
    
    // Check for overflow
    if (val < INT_MIN || val > INT_MAX) {
        return false;
    }
    
    *out = (int)val;
    return true;
}

bool cli_parse_port(const char *str, int *out) {
    int val;
    if (!cli_parse_int(str, &val)) {
        return false;
    }
    if (val < 0 || val > 65535) {
        return false;
    }
    *out = val;
    return true;
}

double cli_get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

XrayIsolate* cli_create_isolate(void) {
    XrayIsolateParams params;
    xray_isolate_params_init(&params);
    xray_isolate_setup_full(&params);
    return xray_isolate_new(&params);
}
