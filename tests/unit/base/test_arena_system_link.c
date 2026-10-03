/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_arena_system_link.c - Runtime arena links without compiler resources
 */
#include "base/xarena.h"
#include <string.h>

int main(void) {
    XrArena arena = {0};
    XrArenaBacking backing = xr_arena_system_backing();
    if (xr_arena_open(&arena, 16, &backing) != XR_ARENA_OK) return 1;
    char *copy = xr_arena_strdup(&arena, "runtime");
    int result = !copy || strcmp(copy, "runtime");
    xr_arena_destroy(&arena);
    return result;
}
