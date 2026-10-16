/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_builder_timing.c - Original builder timing without observation hooks
 */
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#define XRT_IMPL
#define XRT_MALLOC xr_malloc
#define XRT_CALLOC xr_calloc
#define XRT_REALLOC xr_realloc
#define XRT_FREE xr_free
#include "aot/xrt_coll.h"
#include "aot/xrt_arith.h"
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
static double timing_wall(void) {
    struct timespec value; CHECK(timespec_get(&value, TIME_UTC) == TIME_UTC);
    return (double)value.tv_sec + (double)value.tv_nsec / 1000000000.0;
}
static void timing_text(XrValue value, size_t length, bool digits) {
    CHECK(xr_str_len(value) >= 0 && (size_t)xr_str_len(value) == length);
    const char *bytes = xr_str_data(value); CHECK(bytes[length] == 0);
    for (size_t i = 0; i < length; ++i) CHECK(bytes[i] == (digits ? (char)('0' + i % 10) : 'x'));
}
int main(int argc, char **argv) {
    CHECK(argc == 2); const char *name = argv[1];
    bool digits = !strcmp(name, "digits"), chunks = !strcmp(name, "chunks");
    bool join = !strcmp(name, "join"), snapshot = !strcmp(name, "snapshot");
    CHECK(digits || chunks || join || snapshot || !strcmp(name, "short"));
    size_t count = chunks ? 64 : join ? 1000 : snapshot ? 4096 : 1000000;
    size_t piece_length = chunks ? 16384 : join ? 1000 : 1, length = count * piece_length + (snapshot ? 64 : 0);
    void *arena = xrt_execution_arena_new(); CHECK(arena);
    void *previous = xrt_execution_arena_enter(arena);
    XrValue piece = xrt_str_alloc(piece_length); memset(xr_str_buf(piece), 'x', piece_length);
    clock_t start = clock(); double wall_start = timing_wall();
    XrValue builder = xrt_strbuf_new(), old = {0};
    if (join) xrt_strbuf_reserve_extra_exact(builder, (int64_t)length);
    for (size_t i = 0; i < count; ++i) xrt_strbuf_append(builder, digits ? XR_FROM_INT(i % 10) : piece);
    if (snapshot) {
        old = xrt_strbuf_finish(builder);
        for (size_t i = 0; i < 64; ++i) xrt_strbuf_append(builder, piece);
    }
    XrValue result = xrt_strbuf_finish(builder);
    double wall = timing_wall() - wall_start, cpu = (double)(clock() - start) / CLOCKS_PER_SEC;
    timing_text(result, length, digits);
    if (snapshot) timing_text(old, 4096, false);
    xrt_release(result); if (snapshot) xrt_release(old);
    xrt_release(builder); xrt_release(piece);
    xrt_execution_arena_restore(previous); xrt_execution_arena_destroy(arena);
    printf("{\"scenario\":\"%s\",\"result_bytes\":%zu,\"wall_seconds\":%.9f,"
        "\"cpu_seconds\":%.9f,\"copy_observer\":false,\"allocator_observer\":false,"
        "\"qualification\":\"TIMING_AND_CONTENT_ONLY\"}\n", name, length, wall, cpu);
    return 0;
}
