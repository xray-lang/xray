/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_generated_c_allocation_probe.h - Test-only allocation interception for emitted C
 */
#ifndef XR_GENERATED_C_ALLOCATION_PROBE_H
#define XR_GENERATED_C_ALLOCATION_PROBE_H
#include <stdlib.h>
void *xr_program_test_malloc(size_t size);
void *xr_program_test_calloc(size_t count, size_t size);
void *xr_program_test_realloc(void *pointer, size_t size);
void xr_program_test_free(void *pointer);
#define malloc(size) xr_program_test_malloc(size)
#define calloc(count, size) xr_program_test_calloc(count, size)
#define realloc(pointer, size) xr_program_test_realloc(pointer, size)
#define free(pointer) xr_program_test_free(pointer)
#endif
