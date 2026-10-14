/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdap_launch.h - Canonical source products received by the observed VM
 */
#ifndef XDAP_LAUNCH_H
#define XDAP_LAUNCH_H
#include "xdap_current.h"
#include "../../api/xsource_compiler.h"
typedef struct XdapCodeFunction {
    char *name;
    uint32_t first, instructions, parameters, values;
} XdapCodeFunction;
typedef struct XdapLaunch {
    XrCompileResources *resources;
    XrXirSourceProduct *source;
    char *absolute_file;
    XdapCurrent *control;
    XdapCodeFunction *functions;
    XrXirInstruction *instructions;
    uint32_t entry, function_count, instruction_count;
} XdapLaunch;
typedef struct XdapLaunchRequest {
    XrSourceCompiler *compiler;
    const char *absolute_file;
    const XrSourceCompilerText *text;
    XrXirDomain *observer_metadata;
    XrXirInstanceConfig instance;
} XdapLaunchRequest;
/* File or explicit text uses the single SourceCompiler and original ledger.
 * The published Closed packet receives full fresh decoding and lowering before
 * debug Program sealing. No AST/VM fallback or product-private pointer escapes.
 * Success owns queries, immutable display code and the controlled Instance.
 * Failure preserves empty output and retains an owned source diagnostic. */
XR_FUNC XrXirStatus xdap_launch_build(const XdapLaunchRequest *, XdapLaunch **,
    XrSourceCompileDiagnostic *);
/* Consumes except on BUSY; preserves the exact execution cleanup status. */
XR_FUNC XrXirCallStatus xdap_launch_free(XdapLaunch *);
#endif // XDAP_LAUNCH_H
