/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source.h - Source declarations to owned Checked XIR
 *
 * KEY CONCEPT:
 *   Parsing and module resolution precede direct typed XIR construction.
 */
#ifndef XXIR_SOURCE_H
#define XXIR_SOURCE_H
#include "xxir_source_query.h"
#include "../module/xmodule_identity.h"
#include "../module/xmodule_overlay.h"
struct XrCompilerSession;
struct XrLockfile;
struct XrXirLibraryCatalog;
typedef struct XrXirSourceRequest {
    struct XrCompilerSession *session;
    const char *entry_path;
    const XrModuleIdentityAuthority *authority;
    const XrXirCompileContext *context;
    const char *stdlib_path;
    /* Borrowed for this synchronous check; package imports require exact entries. */
    struct XrLockfile *lockfile;
    XrXirLinkageKind linkage_kind;
    const struct XrXirLibraryCatalog *libraries;
} XrXirSourceRequest;
/* Failure preserves result output. Optional failure must be empty. A located
 * parser or semantic failure transfers its owned UTF-8 path without allocation
 * or work during cleanup. Parser failures also carry independently owned
 * origin and exact UTF-16 display scalars. Diagnostic line/column remain
 * one-based compiler bytes. Release failure with source_failure_free. */
XR_FUNC XrXirStatus xr_xir_compile_source_check(const XrXirSourceRequest *request,
    XrXirSourceResult *output, XrXirSourceDiagnostic *diagnostic, XrXirSourceFailure *failure);
/* Borrowed only for one synchronous check; the result owns all published data. */
typedef struct XrXirSourceText {
    const char *logical_path;
    const char *text;
    size_t length;
} XrXirSourceText;
/* Explicit entry bytes use the same checking and owned-query pipeline. The
 * buffer is borrowed only for this synchronous call and need not be terminated;
 * embedded NUL is rejected. Length excludes any caller-owned terminator. Empty
 * text is allowed with a non-NULL buffer. No entry file is read or written.
 * Logical identity and entry_path must agree under request->authority. Rooted
 * entries may be absent on disk. MEMORY authority requires a NULL entry_path
 * and an empty or NULL logical_path; relative imports remain unavailable there.
 * Imports still use the ordinary resolver; this is not a multi-file overlay.
 * Results own their data independently of text, authority, path and session.
 * A NULL input struct is invalid. Failure preserves output and uses the same
 * failure contract above. */
XR_FUNC XrXirStatus xr_xir_compile_source_check_text(const XrXirSourceRequest *request,
    const XrXirSourceText *text,
    XrXirSourceResult *output, XrXirSourceDiagnostic *diagnostic, XrXirSourceFailure *failure);
typedef struct XrXirSourceAst {
    XrXirSourceText source;
    struct AstNode *program;
} XrXirSourceAst;
/* Check the actual caller AST, without reparsing the entry or taking ownership.
 * Its arena must belong to request->session's exact state. Tree, bytes and
 * authority are borrowed exclusively for this call; results and diagnostics
 * own all published data. Source text supplies diagnostic provenance, not an
 * alternative syntax input. Imports use the ordinary graph and checker. */
XR_FUNC XrXirStatus xr_xir_compile_source_check_ast(const XrXirSourceRequest *request,
    const XrXirSourceAst *input, XrXirSourceResult *output,
    XrXirSourceDiagnostic *diagnostic, XrXirSourceFailure *failure);

/* Synchronously check a rooted entry with an immutable set of unsaved sources.
 * Entries are deep copied to the request ledger before discovery. The entry
 * may itself be absent on disk. Relative imports select each .xr or /index.xr
 * candidate from the overlay before probing disk; ordinary catalog/stdlib/
 * package admission still applies. Catalog identity collisions are invalid.
 * A NULL set with count zero is the ordinary file input. Result and failure
 * publication follow source_check; all input borrows end on return. */
XR_FUNC XrXirStatus xr_xir_compile_source_check_overlay(const XrXirSourceRequest *request,
    const XrModuleOverlayInput *overlays, size_t count, XrXirSourceResult *output,
    XrXirSourceDiagnostic *diagnostic, XrXirSourceFailure *failure);
#endif // XXIR_SOURCE_H
