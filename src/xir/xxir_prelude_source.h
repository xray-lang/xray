/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 */
/* Internal compile owner for the generated canonical prelude Library. */
#ifndef XXIR_PRELUDE_SOURCE_H
#define XXIR_PRELUDE_SOURCE_H
#include "xxir_source.h"
/* Fixed compiler input only. Occupied output is rejected before resource work.
 * The ordinary Source/Checked owner publishes a fully owned Library result. */
XR_FUNC XrXirStatus xr_xir_compile_prelude_library(const XrXirCompileContext *context,
    XrXirSourceResult *output, XrXirSourceDiagnostic *diagnostic);
#endif
