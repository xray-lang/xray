/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xsource_api.h - Internal ownership transfer at the public Program boundary
 */
#ifndef XSOURCE_API_H
#define XSOURCE_API_H
#include "../../include/xray_source.h"
#include "../program/xr_xir_source_product.h"
/* Accept the actual product on its original ledger. Success consumes product;
 * failure preserves both owner pointers. No code or metadata is synthesized. */
XR_FUNC XrSourceStatus xr_source_program_take_product(XrXirSourceProduct **product,
    XrSourceProgram **output);
/* Transfers the genuine VM projection to a native/common host consumer. The
 * public wrapper is consumed only on success; no execution representation is
 * copied or converted. Failure preserves the wrapper and empty output. */
XR_FUNC XrSourceStatus xr_source_program_take_vm(XrSourceProgram **program,
    XrXirProgram **output, uint32_t *entry);
#endif // XSOURCE_API_H
