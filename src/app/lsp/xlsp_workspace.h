/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_workspace.h - Workspace background indexing and file scanning
 *
 * KEY CONCEPT:
 *   Background display indexing owns immutable Source syntax facts on the
 *   caller ledger. It never creates VM/compiler semantic authority.
 */

#ifndef XLSP_WORKSPACE_H
#define XLSP_WORKSPACE_H

#include "xlsp_server.h"
#include <stdbool.h>

typedef struct XrLspServer XrLspServer;

// Start background workspace indexing
XR_FUNC void xlsp_workspace_start_background_index(XrLspServer *server, const char *root_path);

// Start background indexing for multiple roots (all scanned into one batch)
XR_FUNC void xlsp_workspace_start_background_index_roots(XrLspServer *server, const char **roots,
                                                         int root_count);

// Index a single file by path (for file watcher updates)
XR_FUNC void xlsp_workspace_index_file(XrLspServer *server, const char *uri, const char *path);

// ============================================================================
// Multi-Isolate Parallel Indexing API
// ============================================================================

// Forward declaration
typedef struct XrLspIndexResult XrLspIndexResult;

// Poll index pool and process results (call from main loop)
XR_FUNC void xlsp_workspace_poll_index_results(XrLspServer *server);

// Merge index results into workspace analyzer
XR_FUNC void xlsp_workspace_merge_index_results(XrLspServer *server, XrLspIndexResult *results);

// Purge all analyzer/cache state for files under a path prefix
XR_FUNC void xlsp_workspace_purge_prefix(XrLspServer *server, const char *path_prefix);

#endif  // XLSP_WORKSPACE_H
