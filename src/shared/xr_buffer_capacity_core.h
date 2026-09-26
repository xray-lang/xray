/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_buffer_capacity_core.h - Overflow-safe bounded geometric buffer growth
 */
#ifndef XR_BUFFER_CAPACITY_CORE_H
#define XR_BUFFER_CAPACITY_CORE_H
#include <stdbool.h>
#include <stddef.h>

/* Plan before allocating or adding lengths. A false result never publishes a
 * capacity and leaves the caller's buffer untouched. */
static inline bool xr_buffer_capacity_plan(size_t length, size_t capacity,
                                                 size_t additional, size_t minimum,
                                                 size_t maximum, size_t *result) {
    if (!result)
        return false;
    *result = 0u;
    if (length > capacity || capacity > maximum || additional > maximum - length)
        return false;
    size_t required = length + additional;
    size_t grown = capacity;
    if (required > grown && grown == 0u) {
        grown = minimum ? minimum : 1u;
        if (grown > maximum)
            grown = maximum;
    }
    while (grown < required) {
        if (grown > maximum / 2u) {
            grown = maximum;
            break;
        }
        grown *= 2u;
    }
    *result = grown;
    return true;
}

#endif // XR_BUFFER_CAPACITY_CORE_H
