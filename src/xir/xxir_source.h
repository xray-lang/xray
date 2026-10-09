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
/* Failure preserves output. Optional failure_path must point to NULL. A
 * semantic failure with a known location transfers its already-owned UTF-8
 * module path without allocating or consuming work during failure cleanup.
 * Release that path with xr_compile_resources_free. Graph and parse failures
 * without an exact semantic location leave failure_path unchanged. */
XR_FUNC XrXirStatus xr_xir_compile_source_check(const XrXirSourceRequest *request,
    XrXirSourceResult *output, XrXirSourceDiagnostic *diagnostic, char **failure_path);
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
 * failure_path contract above. */
XR_FUNC XrXirStatus xr_xir_compile_source_check_text(const XrXirSourceRequest *request,
    const XrXirSourceText *text,
    XrXirSourceResult *output, XrXirSourceDiagnostic *diagnostic, char **failure_path);
/* Synchronously check a rooted entry with an immutable set of unsaved sources.
 * Entries are deep copied to the request ledger before discovery. The entry
 * may itself be absent on disk. Relative imports select each .xr or /index.xr
 * candidate from the overlay before probing disk; ordinary catalog/stdlib/
 * package admission still applies. Catalog identity collisions are invalid.
 * A NULL set with count zero is the ordinary file input. Result and failure
 * publication follow source_check; all input borrows end on return. */
XR_FUNC XrXirStatus xr_xir_compile_source_check_overlay(const XrXirSourceRequest *request,
    const XrModuleOverlayInput *overlays, size_t count, XrXirSourceResult *output,
    XrXirSourceDiagnostic *diagnostic, char **failure_path);
#endif // XXIR_SOURCE_H
