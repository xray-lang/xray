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
typedef struct XrSourceCompilerAst {
    const char *absolute_path; /* NULL requires a MEMORY namespace. */
    const char *namespace_id;
    struct AstNode *program;
} XrSourceCompilerAst;
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
/* Borrows this compiler's actual Catalog, locator and cumulative context.
 * The compiler must outlive the generation owner; namespace is copied by that
 * owner's ordinary admission. This observation creates no parser or ledger. */
XR_FUNC XrSourceCompileStatus xr_source_compiler_generation_config(const XrSourceCompiler *,
    const char *namespace_id, XrXirSourceGenerationConfig *output);
XR_FUNC XrSourceCompileStatus xr_source_compiler_build_file(XrSourceCompiler *, const char *absolute_path,
    XrXirSourceProduct **output, XrSourceCompileDiagnostic *diagnostic);
/* Rooted text derives exact project/script authority and supports ordinary
 * imports without creating an entry file. MEMORY forbids relative imports.
 * Returned products and diagnostics outlive the compiler and all source bytes. */
XR_FUNC XrSourceCompileStatus xr_source_compiler_build_text(XrSourceCompiler *, const XrSourceCompilerText *,
    XrXirSourceProduct **output, XrSourceCompileDiagnostic *diagnostic);
/* Borrow a complete owning parser AST on this compiler's actual ledger.
 * Rooted authority/lockfile discovery and public Program admission are shared
 * with other inputs. The entry may be absent on disk. Its producing session
 * may be dead; AST and all request fields remain caller-owned on every return.
 * The returned product owns its data independently of the AST and compiler. */
XR_FUNC XrSourceCompileStatus xr_source_compiler_build_ast(XrSourceCompiler *, const XrSourceCompilerAst *,
    XrXirSourceProduct **output, XrSourceCompileDiagnostic *diagnostic);
#endif // XSOURCE_COMPILER_H
