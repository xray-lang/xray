/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_array_capacity_native_oracles.h - Independent native capacity entry names
 */
#ifndef XIR_ARRAY_CAPACITY_NATIVE_ORACLES_H
#define XIR_ARRAY_CAPACITY_NATIVE_ORACLES_H
enum {
    CAP_NATIVE_ENTRY, CAP_NATIVE_SHAPE, CAP_NATIVE_PATH, CAP_NATIVE_RESERVE,
    CAP_NATIVE_SNAPSHOT, CAP_NATIVE_TRACE, CAP_NATIVE_MUTATE, CAP_NATIVE_NEGATIVE_NEW,
    CAP_NATIVE_NEGATIVE_RESERVE, CAP_NATIVE_OVERFLOW, CAP_NATIVE_COUNT
};
static const char *const capacity_native_names[CAP_NATIVE_COUNT]={
    "main", "shape", "nestedPath", "reserveSnapshot", "snapshot", "traceValue",
    "mutate", "negativeNew", "negativeReserve", "overflow"
};
#endif // XIR_ARRAY_CAPACITY_NATIVE_ORACLES_H
