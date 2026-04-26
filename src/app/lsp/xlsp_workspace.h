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
 *   workspace_analyzer is the SOLE authoritative semantic state source.
 *   This module manages background scanning, parallel indexing, and
 *   pending analysis queue — no separate symbol index layer.
 */

#ifndef XLSP_WORKSPACE_H
#define XLSP_WORKSPACE_H

#include "xlsp_server.h"
#include <stdbool.h>

typedef struct XrLspServer XrLspServer;

// Data for background indexing task
typedef struct XrLspIndexTaskData {
    XrLspServer *server;
    char *root_path;
    char **files;
    int file_count;
    int current_file;
} XrLspIndexTaskData;

// Start background workspace indexing
XR_FUNC void xlsp_workspace_start_background_index(XrLspServer *server, const char *root_path);

// Start background indexing for multiple roots (all scanned into one batch)
XR_FUNC void xlsp_workspace_start_background_index_roots(XrLspServer *server, const char **roots,
                                                         int root_count);

// Background task execute function (runs in worker thread)
XR_FUNC void xlsp_workspace_index_task_execute(void *data);

// Background task completion (runs in main thread)
XR_FUNC void xlsp_workspace_index_task_complete(void *data, void *result);

// Index a single file by path (for file watcher updates)
XR_FUNC void xlsp_workspace_index_file(XrLspServer *server, const char *uri, const char *path);

// ============================================================================
// Multi-Isolate Parallel Indexing API
// ============================================================================

// Forward declaration
typedef struct XrLspIndexResult XrLspIndexResult;

// Poll index pool and process results (call from main loop)
XR_FUNC void xlsp_workspace_poll_index_results(XrLspServer *server);

// Get index pool notify fd (for select/poll)
XR_FUNC int xlsp_workspace_get_index_notify_fd(XrLspServer *server);

// Merge index results into workspace analyzer
XR_FUNC void xlsp_workspace_merge_index_results(XrLspServer *server, XrLspIndexResult *results);

// Purge all analyzer/cache state for files under a path prefix
XR_FUNC void xlsp_workspace_purge_prefix(XrLspServer *server, const char *path_prefix);

// Pending analysis queue helpers
XR_FUNC void xlsp_enqueue_analysis(XrLspServer *server, const char *uri, const char *path);
XR_FUNC int xlsp_drain_pending_analysis(XrLspServer *server);
XR_FUNC void xlsp_free_pending_analysis(XrLspServer *server);

#endif  // XLSP_WORKSPACE_H
