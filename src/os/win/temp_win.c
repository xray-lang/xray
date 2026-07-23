/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * temp_win.c - Windows secure private temporary directory creation
 */

#include "../os_temp.h"
#include "../os_random.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <string.h>

static int temp_prefix_valid(const char *prefix) {
    if (!prefix || !prefix[0])
        return 0;
    for (const char *p = prefix; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
              *p == '-' || *p == '_'))
            return 0;
    }
    return 1;
}

int xr_temp_dir_create(const char *prefix, char *out, size_t out_size) {
    char base[MAX_PATH];
    DWORD base_len;
    if (!temp_prefix_valid(prefix) || !out || out_size == 0)
        return -1;
    base_len = GetTempPathA((DWORD) sizeof(base), base);
    if (base_len == 0 || base_len >= sizeof(base))
        return -1;
    for (int attempt = 0; attempt < 32; attempt++) {
        unsigned char random[16];
        char token[33];
        static const char hex[] = "0123456789abcdef";
        xr_random_bytes(random, sizeof(random));
        for (size_t i = 0; i < sizeof(random); i++) {
            token[i * 2] = hex[random[i] >> 4];
            token[i * 2 + 1] = hex[random[i] & 0x0f];
        }
        token[32] = '\0';
        int written = snprintf(out, out_size, "%s%s-%s", base, prefix, token);
        if (written < 0 || (size_t) written >= out_size)
            return -1;
        if (CreateDirectoryA(out, NULL))
            return 0;
        if (GetLastError() != ERROR_ALREADY_EXISTS)
            return -1;
    }
    return -1;
}
