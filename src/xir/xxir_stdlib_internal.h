/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_stdlib_internal.h - Source-independent Checked standard library input
 *
 * KEY CONCEPT: Library bytes grant no native code execution authority.
 */
#ifndef XXIR_STDLIB_INTERNAL_H
#define XXIR_STDLIB_INTERNAL_H
#include "xxir_library_catalog.h"
XR_DATA const uint8_t xir_stdlib_output_checked[];
XR_DATA const size_t xir_stdlib_output_checked_length;
XR_DATA const uint8_t xir_stdlib_output_checked_sha256[32];
/* Construction copies the physical root and verifies the entire Checked input
 * under the caller's ledger. Failure leaves the output unchanged. */
XR_FUNC XrXirStatus xir_stdlib_library_catalog_new(const XrXirCompileContext *context,
    const char *physical_root, XrXirLibraryCatalog **output);
#endif // XXIR_STDLIB_INTERNAL_H
