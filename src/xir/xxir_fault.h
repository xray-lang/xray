/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_fault.h - Allocation-free runtime fault detail
 *
 * KEY CONCEPT:
 *   A fault retains observed operands independently of every runtime owner.
 */
#ifndef XXIR_FAULT_H
#define XXIR_FAULT_H
#include <stdbool.h>
#include <stdint.h>

typedef struct XrXirFaultDetail {
    uint32_t code, reserved;
    int64_t index, length;
} XrXirFaultDetail;

static inline bool xr_xir_fault_empty(XrXirFaultDetail fault) {
    return !fault.code && !fault.reserved && !fault.index && !fault.length;
}
static inline bool xr_xir_fault_bounds_valid(XrXirFaultDetail fault) {
    return fault.code == 430 && !fault.reserved && fault.length >= 0 &&
        (fault.index < 0 || fault.index >= fault.length);
}
#endif // XXIR_FAULT_H
