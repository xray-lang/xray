/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xsource_authority.h - Exact source graph authority for source consumers
 *
 * KEY CONCEPT:
 *   A source command owns its project, lockfile and exact entry authority.
 *   The Source producer owns the resolver and borrows this authority only
 *   while constructing its module graph.
 */

#ifndef XSOURCE_AUTHORITY_H
#define XSOURCE_AUTHORITY_H

#include "../module/xlockfile.h"
#include "../module/xmodule_overlay.h"
#include "../module/xproject.h"
#include "../xir/xxir.h"
#include "../xir/xxir_source_query.h"
/* Nearest manifest ancestor of an absolute directory. NOT_FOUND is normal
 * absence; the returned path is owned by the supplied policy. */
XR_FUNC XrOsIoStatus xr_source_find_project_root(const XrOsIoPolicy *policy,
    const char *absolute_start_directory, char **output);
struct XrXirLibraryCatalog;
typedef struct XrSourceAuthority XrSourceAuthority;
/* Inputs are explicit and borrowed synchronously. Catalog remains borrowed and
 * must outlive this authority. Failure preserves an initially empty output. */
XR_FUNC XrManifestStatus xr_source_authority_open(
    const XrXirCompileContext *context, const char *absolute_entry_path,
    const struct XrXirLibraryCatalog *libraries, const XrTomlParseLimits *limits,
    XrSourceAuthority **output, XrManifestDiagnostic *diagnostic);
typedef enum XrSourceEntryKind {
    XR_SOURCE_ENTRY_FILE, XR_SOURCE_ENTRY_TEXT, XR_SOURCE_ENTRY_AST
} XrSourceEntryKind;
typedef struct XrSourceEntryInput {
    XrSourceEntryKind kind;
    const char *absolute_path;
    const char *text; /* TEXT only: length bytes, no terminator required. */
    size_t length;
} XrSourceEntryInput;
/* Diagnostic receivers borrow only during open. failure_path must be empty;
 * an actual input rejection transfers the canonical owned entry path. */
typedef struct XrSourceAuthorityDiagnostics {
    XrManifestDiagnostic *manifest;
    XrXirSourceDiagnostic *source;
    char **failure_path;
} XrSourceAuthorityDiagnostics;
/* Same authority discovery as the file entry. TEXT and AST may be absent on disk;
 * its nearest existing parent is canonicalized and missing path components
 * are validated, never normalized into a different authority. The owner
 * retains the canonical entry path. TEXT additionally owns an immutable
 * overlay; AST owns the exact logical locator without manufacturing bytes.
 * Invalid nearer projects/lockfiles are errors; no SCRIPT fallback on errors.
 * Catalog remains borrowed under the original authority lifetime contract. */
XR_FUNC XrManifestStatus xr_source_authority_open_input(
    const XrXirCompileContext *context, const XrSourceEntryInput *input,
    const struct XrXirLibraryCatalog *libraries, const XrTomlParseLimits *limits,
    XrSourceAuthority **output, const XrSourceAuthorityDiagnostics *diagnostics);
XR_FUNC const char *xr_source_authority_source_path(const XrSourceAuthority *);
/* FILE/AST's exact authority-relative locator; TEXT uses its owned overlay. */
XR_FUNC const char *xr_source_authority_logical_path(const XrSourceAuthority *);
/* NULL for FILE input; an immutable owner-backed entry for TEXT input. */
XR_FUNC const XrModuleOverlayInput *xr_source_authority_overlay(const XrSourceAuthority *);
XR_FUNC void xr_source_authority_close(XrSourceAuthority *authority);
XR_FUNC const XrModuleIdentityAuthority *xr_source_authority_entry(const XrSourceAuthority *);
XR_FUNC const XrProject *xr_source_authority_project(const XrSourceAuthority *);
XR_FUNC XrLockfile *xr_source_authority_lockfile(const XrSourceAuthority *);
XR_FUNC const struct XrXirLibraryCatalog *xr_source_authority_catalog(const XrSourceAuthority *);
XR_FUNC const XrXirCompileContext *xr_source_authority_context(const XrSourceAuthority *);
#endif /* XSOURCE_AUTHORITY_H */
