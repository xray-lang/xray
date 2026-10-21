/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * reference.c - Ordinary C references for execution benchmarks.
 *
 * KEY CONCEPT:
 *   Match the source workloads and print an observable result. Iteration
 *   counts arrive at runtime; the compiler may optimize ordinary C freely.
 */

#include "base/xmalloc.h"
#include "xray_build_identity.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#ifdef XR_OS_WINDOWS
#include <fcntl.h>
#include <io.h>
#endif

static uint64_t fib(uint64_t n) {
    return n < 2 ? n : fib(n - 1) + fib(n - 2);
}

static uint64_t integer_loop(uint64_t n) {
    uint64_t sum = 0;
    for (uint64_t i = 0; i < n; ++i)
        sum += i % 97;
    return sum;
}

static bool push_sum(uint64_t n, uint64_t *result) {
    uint64_t *values = NULL;
    size_t capacity = 0, length = 0;
    if (n > SIZE_MAX / sizeof(uint64_t))
        return false;
    for (uint64_t i = 0; i < n; ++i) {
        if (length == capacity) {
            size_t next = capacity ? capacity * 2 : 8;
            if (next < capacity || next > SIZE_MAX / sizeof(uint64_t)) {
                xr_free(values);
                return false;
            }
            uint64_t *grown = xr_realloc(values, next * sizeof(uint64_t));
            if (!grown) {
                xr_free(values);
                return false;
            }
            values = grown;
            capacity = next;
        }
        values[length++] = i;
    }
    uint64_t sum = 0;
    for (size_t i = 0; i < length; ++i)
        sum += values[i];
    xr_free(values);
    *result = sum;
    return true;
}

int main(int argc, char **argv) {
#ifdef XR_OS_WINDOWS
    if (_setmode(_fileno(stdout), _O_BINARY) == -1)
        return 3;
#endif
    if (argc == 2 && strcmp(argv[1], "--identity") == 0) {
        printf("compiler=%s\ncommit=%s\ndirty=%d\n", XR_BENCH_COMPILER, XRAY_BUILD_COMMIT,
               XRAY_BUILD_DIRTY);
        return 0;
    }
    if (argc != 3 || !argv[2][0] || argv[2][0] == '-')
        return 2;
    errno = 0;
    char *end = NULL;
    uint64_t n = strtoull(argv[2], &end, 10), result;
    if (errno || !end || *end)
        return 2;
    if (strcmp(argv[1], "fib") == 0) {
        if (n > 46)
            return 2;
        result = fib(n);
    } else if (strcmp(argv[1], "integer_loop") == 0) {
        result = integer_loop(n);
    } else if (strcmp(argv[1], "push_sum") == 0) {
        if (!push_sum(n, &result))
            return 3;
    } else {
        return 2;
    }
    printf("%" PRIu64 "\n", result);
    return 0;
}
