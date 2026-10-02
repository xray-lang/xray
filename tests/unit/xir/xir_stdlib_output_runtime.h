/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_stdlib_output_runtime.h - Independent standard output publication checks
 *
 * KEY CONCEPT: Both execution engines compare streams against fixed bytes.
 */
#ifndef XIR_STDLIB_OUTPUT_RUNTIME_H
#define XIR_STDLIB_OUTPUT_RUNTIME_H
XR_FUNC size_t *xr_test_stdlib_output_counter(unsigned index);
XR_FUNC void xr_test_stdlib_output_execute(XrXirArtifact *checked, const char *generated);
#ifdef XR_STDLIB_OUTPUT_SOURCE
#define runtime_attempts (*xr_test_stdlib_output_counter(0))
#define runtime_fail_at (*xr_test_stdlib_output_counter(1))
#define runtime_live (*xr_test_stdlib_output_counter(2))
#define runtime_bytes (*xr_test_stdlib_output_counter(3))
#endif
#endif
