/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcli_graph_authority.h - Exact source graph authority for CLI commands
 *
 * KEY CONCEPT:
 *   A source command owns its project, lockfile and exact entry authority.
 *   The Source producer owns the resolver and borrows this authority only
 *   while constructing its module graph.
 */

#ifndef XCLI_GRAPH_AUTHORITY_H
#define XCLI_GRAPH_AUTHORITY_H

#include "../../module/xlockfile.h"
#include "../../module/xmodule_overlay.h"
#include "../../module/xproject.h"
#include "../../xir/xxir.h"
struct XrXirLibraryCatalog;
typedef struct XrCliGraphAuthority XrCliGraphAuthority;
/* Inputs are explicit and borrowed synchronously. Catalog remains borrowed and
 * must outlive this authority. Failure preserves an initially empty output. */
XR_FUNC XrManifestStatus xr_cli_compile_graph_authority_open(
    const XrXirCompileContext *context, const char *absolute_entry_path,
    const struct XrXirLibraryCatalog *libraries, const XrTomlParseLimits *limits,
    XrCliGraphAuthority **output, XrManifestDiagnostic *diagnostic);
typedef enum XrCliGraphEntryKind {
    XR_CLI_GRAPH_ENTRY_FILE, XR_CLI_GRAPH_ENTRY_TEXT
} XrCliGraphEntryKind;
typedef struct XrCliGraphEntryInput {
    XrCliGraphEntryKind kind;
    const char *absolute_path;
    const char *text; /* TEXT only: length bytes, no terminator required. */
    size_t length;
} XrCliGraphEntryInput;
/* Same authority discovery as the file entry. TEXT may be absent on disk;
 * its nearest existing parent is canonicalized and missing path components
 * are validated, never normalized into a different authority. The owner
 * retains the canonical entry path and a real immutable overlay entry.
 * Invalid nearer projects/lockfiles are errors; no SCRIPT fallback on errors.
 * Catalog remains borrowed under the original authority lifetime contract. */
XR_FUNC XrManifestStatus xr_cli_compile_graph_authority_open_input(
    const XrXirCompileContext *context, const XrCliGraphEntryInput *input,
    const struct XrXirLibraryCatalog *libraries, const XrTomlParseLimits *limits,
    XrCliGraphAuthority **output, XrManifestDiagnostic *diagnostic);
XR_FUNC const char *xr_cli_compile_graph_authority_source_path(const XrCliGraphAuthority *);
/* NULL for FILE input; an immutable owner-backed entry for TEXT input. */
XR_FUNC const XrModuleOverlayInput *xr_cli_compile_graph_authority_overlay(const XrCliGraphAuthority *);
XR_FUNC void xr_cli_compile_graph_authority_close(XrCliGraphAuthority *authority);
XR_FUNC const XrModuleIdentityAuthority *xr_cli_compile_graph_authority_entry(const XrCliGraphAuthority *);
XR_FUNC const XrProject *xr_cli_compile_graph_authority_project(const XrCliGraphAuthority *);
XR_FUNC XrLockfile *xr_cli_compile_graph_authority_lockfile(const XrCliGraphAuthority *);
XR_FUNC const struct XrXirLibraryCatalog *xr_cli_compile_graph_authority_catalog(const XrCliGraphAuthority *);
XR_FUNC const XrXirCompileContext *xr_cli_compile_graph_authority_context(const XrCliGraphAuthority *);
#endif /* XCLI_GRAPH_AUTHORITY_H */
