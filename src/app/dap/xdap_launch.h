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
    char *name,*source_name;
    uint32_t first, instructions, parameters, values,origin,module;
} XdapCodeFunction;

typedef struct XdapCodeModule {
    char *identity,*path;
    size_t identity_length,path_length,name_offset;
    uint32_t query_module;
    XrFingerprint fingerprint;
} XdapCodeModule;
typedef struct XdapLaunch {
    XrCompileResources *resources;
    XrXirSourceProduct *source;
    char *absolute_file;
    XdapCurrent *control;
    XdapCodeFunction *functions;
    XdapCodeModule *modules;
    XrXirInstruction *instructions;
    uint32_t entry, function_count, instruction_count,module_count;
} XdapLaunch;
typedef struct XdapLaunchRequest {
    XrSourceCompiler *compiler;
    const char *absolute_file;
    const XrSourceCompilerText *text;
    XrXirDomain *observer_metadata;
    XrXirInstanceConfig instance;
} XdapLaunchRequest;
typedef struct XdapFunctionSelection {
    const char *name;
    size_t length;
    XrXirVmDebugRule rule;
} XdapFunctionSelection;
/* File or explicit text uses the single SourceCompiler and original ledger.
 * The published Closed packet receives full fresh decoding and lowering before
 * debug Program sealing. No AST/VM fallback or product-private pointer escapes.
 * Success owns queries, immutable display code and the controlled Instance.
 * Failure preserves empty output and retains an owned source diagnostic. */
XR_FUNC XrXirStatus xdap_launch_build(const XdapLaunchRequest *, XdapLaunch **,
    XrSourceCompileDiagnostic *);
/* Consumes except on BUSY; preserves the exact execution cleanup status. */
XR_FUNC XrXirCallStatus xdap_launch_free(XdapLaunch *);
/* One verified source declaration may have several closed VM instances.
 * Ambiguous declarations or an overfull bounded result preserve outputs. */
XR_FUNC XrXirStatus xdap_launch_function_rules(const XdapLaunch *,const XdapFunctionSelection *,
    XrXirVmDebugRule *,uint32_t,uint32_t *);
/* Copies one authentic source site without driving the target. Unmapped
 * synthetic instructions return UNRESOLVED and preserve the output. */
XR_FUNC XrXirStatus xdap_launch_source_point(const XdapLaunch *,uint32_t,uint32_t,XrXirSourceDebugPoint *);
#endif // XDAP_LAUNCH_H
