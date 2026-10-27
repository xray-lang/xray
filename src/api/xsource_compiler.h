/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xsource_compiler.h - Request owner for ordinary source compilation
 *
 * KEY CONCEPT:
 *   A request owns its active parser and borrows short-lived control. The
 *   immutable Catalog can move into separately owned semantic history.
 */
#ifndef XSOURCE_COMPILER_H
#define XSOURCE_COMPILER_H
#include "xsource_compile.h"
#include "xsource_authority.h"
#include "../xir/xxir_source_generation.h"

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
typedef struct XrSourceCompilerFile {
    XrSourceAuthority *authority;
    uint8_t *bytes;
    size_t length;
} XrSourceCompilerFile;
/* Reads one exact regular-file snapshot under this request's controlled-memory
 * I/O policy and genuine FILE authority. Relative paths use the real process
 * working directory under that same policy. Empty output is preserved on failure;
 * successful path/bytes stay owned until clear, independently of the caller. */
XR_FUNC XrSourceCompileStatus xr_source_compiler_read_file(XrSourceCompiler *, const char *path,
    XrSourceCompilerFile *, XrSourceCompileDiagnostic *);
XR_FUNC void xr_source_compiler_file_clear(XrSourceCompilerFile *);

/* Config is borrowed only during construction. Every allocation pins its
 * memory owner; releasing the caller's resource reference is allowed.
 * Builds and all borrowed parser/state accesses require caller exclusion.
 * Control is borrowed through this request only and must outlive the compiler.
 * Memory owners and returned products retain no cancellation/deadline control. */
XR_FUNC XrSourceCompileStatus xr_source_compiler_new(const XrSourceCompilerConfig *, XrCompileRequestControl *,
    XrSourceCompiler **output, XrSourceCompileDiagnostic *diagnostic);
XR_FUNC void xr_source_compiler_free(XrSourceCompiler *compiler);
/* Incremental semantic history owns its real Catalog after the constructor
 * request ends. This move consumes the request and parser session without
 * creating a cache or retaining request state. Both owned outputs are empty. */
XR_FUNC XrSourceCompileStatus xr_source_compiler_take_catalog(XrSourceCompiler **,
    XrSourceCompilerConfig *, struct XrXirLibraryCatalog **, char **stdlib_path);
/* One short-lived request borrows the previously admitted immutable Catalog
 * and its exact configuration. That semantic owner outlives the request. */
XR_FUNC XrSourceCompileStatus xr_source_compiler_catalog_request(const XrSourceCompilerConfig *,
    struct XrXirLibraryCatalog *, XrCompileRequestControl *, XrSourceCompiler **);

/* These are genuine owners, borrowed until compiler_free, never VM accessors. */
XR_FUNC const XrXirCompileContext *xr_source_compiler_context(const XrSourceCompiler *);
XR_FUNC XrCompilerSession *xr_source_compiler_session(const XrSourceCompiler *);
XR_FUNC XrCompileState *xr_source_compiler_state(const XrSourceCompiler *);
XR_FUNC XrCompileResources *xr_source_compiler_resources(const XrSourceCompiler *);
/* Copies the admitted lowering target; this observation grants no authority. */
XR_FUNC XrXirTarget xr_source_compiler_target(const XrSourceCompiler *);
/* Borrows the actual immutable Catalog and copies memory-only configuration.
 * Generation construction copies namespace and locator text. Its semantic
 * owner keeps the Catalog alive; take_catalog can move that ownership before
 * the compiler dies. This observation creates no parser or request control. */
XR_FUNC XrSourceCompileStatus xr_source_compiler_generation_config(const XrSourceCompiler *,
    const char *namespace_id, XrXirSourceGenerationConfig *output);
/* Discovers authority with this exact Compiler's Catalog, manifest limits and
 * controlled memory owner. The caller closes a successful owner after graph capture;
 * failure preserves empty output and latches the original resource cause. */
XR_FUNC XrSourceCompileStatus xr_source_compiler_open_input(XrSourceCompiler *, const XrSourceEntryInput *,
    XrSourceAuthority **, XrSourceCompileDiagnostic *);
XR_FUNC XrSourceCompileStatus xr_source_compiler_build_file(XrSourceCompiler *, const char *absolute_path,
    XrXirSourceProduct **output, XrSourceCompileDiagnostic *diagnostic);
/* Rooted text derives exact project/script authority and supports ordinary
 * imports without creating an entry file. MEMORY forbids relative imports.
 * Returned products and diagnostics outlive the compiler and all source bytes. */
XR_FUNC XrSourceCompileStatus xr_source_compiler_build_text(XrSourceCompiler *, const XrSourceCompilerText *,
    XrXirSourceProduct **output, XrSourceCompileDiagnostic *diagnostic);
/* The actual owning tree is immutable borrowed input. A foreign parser state
 * must be detached from all live sessions. The caller pins the complete tree
 * and source through return; checking never reparses or modifies that entry.
 * Imports, metadata and ready output are owned by this compilation request. */
XR_FUNC XrSourceCompileStatus xr_source_compiler_build_ast(XrSourceCompiler *, const XrSourceCompilerText *,
    struct AstNode *program, XrXirSourceProduct **output, XrSourceCompileDiagnostic *diagnostic);
#endif // XSOURCE_COMPILER_H
