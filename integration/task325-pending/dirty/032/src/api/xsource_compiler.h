/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xsource_compiler.h - Cumulative owner for ordinary source compilation
 *
 * KEY CONCEPT:
 *   One compiler owns the parser, immutable Catalog and the caller's ledger.
 */
#ifndef XSOURCE_COMPILER_H
#define XSOURCE_COMPILER_H
#include "xsource_compile.h"

typedef struct XrSourceCompiler XrSourceCompiler;
typedef struct XrSourceCompilerConfig {
    XrXirCompileContext context;
    XrXirTarget target;
    const char *absolute_stdlib_path;
    XrTomlParseLimits manifest_limits;
} XrSourceCompilerConfig;
typedef struct XrSourceCompilerText {
    const char *absolute_path; /* NULL requires a MEMORY namespace. */
    const char *namespace_id; /* Rooted text requires NULL. */
    const char *text;
    size_t length;
} XrSourceCompilerText;
/* Config is borrowed only during construction. Every allocation pins its
 * original ledger; releasing the caller's resource reference is allowed.
 * Builds and all borrowed parser/state accesses require caller exclusion.
 * No operation resets sticky resource failure or grants a fresh allowance. */
XR_FUNC XrSourceCompileStatus xr_source_compiler_new(const XrSourceCompilerConfig *,
    XrSourceCompiler **output, XrSourceCompileDiagnostic *diagnostic);
XR_FUNC void xr_source_compiler_free(XrSourceCompiler *compiler);
/* These are genuine owners, borrowed until compiler_free, never VM accessors. */
XR_FUNC const XrXirCompileContext *xr_source_compiler_context(const XrSourceCompiler *);
XR_FUNC XrCompilerSession *xr_source_compiler_session(const XrSourceCompiler *);
XR_FUNC XrCompileState *xr_source_compiler_state(const XrSourceCompiler *);
XR_FUNC XrCompileResources *xr_source_compiler_resources(const XrSourceCompiler *);
XR_FUNC const struct XrXirLibraryCatalog *xr_source_compiler_libraries(const XrSourceCompiler *);
XR_FUNC XrSourceCompileStatus xr_source_compiler_build_file(XrSourceCompiler *, const char *absolute_path,
    XrXirSourceProduct **output, XrSourceCompileDiagnostic *diagnostic);
/* Rooted text derives exact project/script authority and supports ordinary
 * imports without creating an entry file. MEMORY forbids relative imports.
 * Returned products and diagnostics outlive the compiler and all source bytes. */
XR_FUNC XrSourceCompileStatus xr_source_compiler_build_text(XrSourceCompiler *, const XrSourceCompilerText *,
    XrXirSourceProduct **output, XrSourceCompileDiagnostic *diagnostic);
/* Receive a closed AST graph built with this owner's borrowed session and
 * Catalog. Its resolver pin policy is borrowed for the synchronous call.
 * Ordinary public admission and sticky failure use this owner's sole ledger;
 * neither the product nor its VM retains the compiler or caller syntax. */
XR_FUNC XrSourceCompileStatus xr_source_compiler_build_graph(XrSourceCompiler *,
    const struct XrModuleGraph *, XrXirSourceProduct **output, XrSourceCompileDiagnostic *diagnostic);
#endif // XSOURCE_COMPILER_H
