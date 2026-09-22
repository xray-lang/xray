/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_program_source_allocation_harness.c - Execute source output through every allocation failure
 */

#include <stdio.h>
#include <stdlib.h>

static void *live[128];
static size_t attempts, allocated, released, fail_at;
static int invalid_free;

static void *observed_malloc(size_t size) {
    if (++attempts == fail_at)
        return NULL;
    void *pointer = malloc(size);
    if (!pointer)
        return NULL;
    for (size_t index = 0u; index < 128u; ++index) {
        if (live[index])
            continue;
        live[index] = pointer;
        ++allocated;
        return pointer;
    }
    free(pointer);
    abort();
}

static void observed_free(void *pointer) {
    if (!pointer)
        return;
    for (size_t index = 0u; index < 128u; ++index) {
        if (live[index] != pointer)
            continue;
        live[index] = NULL;
        ++released;
        free(pointer);
        return;
    }
    invalid_free = 1;
}

#define malloc observed_malloc
#define free observed_free
#define main source_program_main
#include XR_SOURCE_NATIVE_FILE
#undef main
#undef free
#undef malloc

int main(void) {
    for (size_t failure = 1u; failure < 128u; ++failure) {
        fail_at = failure;
        attempts = allocated = released = 0u;
        invalid_free = 0;
        int result = source_program_main();
        int expected = attempts < failure ? XR_SOURCE_EXPECTED_EXIT : 240;
        if (result != expected || allocated != released || invalid_free) {
            fprintf(stderr, "allocation failure=%zu result=%d expected=%d live=%zu invalid=%d\n",
                    failure, result, expected, allocated - released, invalid_free);
            return 1;
        }
        if (attempts < failure) {
            printf("Native source allocation failures: %zu; every allocation physically freed\n",
                   attempts);
            return attempts == 0u;
        }
    }
    return 1;
}
