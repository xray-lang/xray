/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_class_access_compile_faults.h - Actual resource allocator failure suffixes
 */
#ifndef XIR_CLASS_ACCESS_COMPILE_FAULTS_H
#define XIR_CLASS_ACCESS_COMPILE_FAULTS_H
static size_t access_compile_faults[32768],access_compile_depth,access_compile_hits;
static size_t access_compile_site_bytes[32768],access_compile_site_count;
static bool access_compile_recording;
static void access_runtime_before_free(const void *pointer);
#define XR_CELL_RUNTIME_BEFORE_FREE(pointer) access_runtime_before_free(pointer)
/* The existing observer remains the sole physical compiler allocator hook. */
static void access_compile_before(size_t bytes);
#define XR_INSTANCE_COMPILE_BEFORE_ALLOCATION(bytes) access_compile_before(bytes)
#include "xir_instance_compile_observer.h"
#undef XR_INSTANCE_COMPILE_BEFORE_ALLOCATION
static void access_compile_before(size_t bytes) {
    if (!access_compile_recording) return;
    CHECK(access_compile_site_count==instance_compile_attempts && access_compile_site_count<32768);
    access_compile_site_bytes[access_compile_site_count++]=bytes;
    bool fail=access_compile_hits<access_compile_depth &&
        instance_compile_attempts==access_compile_faults[access_compile_hits];
    instance_compile_fail_at=fail?instance_compile_attempts:SIZE_MAX;
    if (fail) ++access_compile_hits;
}
#endif // XIR_CLASS_ACCESS_COMPILE_FAULTS_H
