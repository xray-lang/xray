/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcli_lir.h - Shared command compilation and ordinary native publication
 */
#ifndef XCLI_LIR_H
#define XCLI_LIR_H
#include "lir/xlir_source.h"

/* Both outputs must be empty. Success transfers independent owners. */
XR_FUNC int xr_cli_lir_compile(const char *input, XrCompileResources **resources,
    XrLirLowered *output);
XR_FUNC int xr_cli_lir_publish(XrCompileResources *resources, const char *path,
    const void *bytes, size_t length, bool executable);
typedef struct XrCliNativeOptions {
    const char *compiler, *provider;
    bool verbose;
} XrCliNativeOptions;
XR_FUNC int xr_cli_lir_native(XrCompileResources *resources, const XrCliNativeOptions *options,
    const void *source, size_t length, const char *output);
#endif // XCLI_LIR_H
