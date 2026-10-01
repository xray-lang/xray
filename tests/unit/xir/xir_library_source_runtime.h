/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_library_source_runtime.h - Counted Checked runtime access
 *
 * KEY CONCEPT:
 *   Source allocations and Checked runtime allocations have independent fault indices.
 */
#ifndef XIR_LIBRARY_SOURCE_RUNTIME_H
#define XIR_LIBRARY_SOURCE_RUNTIME_H
#include "xir/xxir.h"
XR_FUNC size_t *xr_test_library_runtime_counter(unsigned index);
XR_FUNC void xr_test_library_source_run(XrXirArtifact *owned);
#define runtime_attempts (*xr_test_library_runtime_counter(0))
#define runtime_fail_at (*xr_test_library_runtime_counter(1))
#define runtime_live (*xr_test_library_runtime_counter(2))
#define runtime_bytes (*xr_test_library_runtime_counter(3))
#endif // XIR_LIBRARY_SOURCE_RUNTIME_H
