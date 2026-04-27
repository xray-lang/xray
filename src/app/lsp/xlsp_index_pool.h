/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_index_pool.h - Multi-isolate parallel indexing pool
 *
 * DESIGN:
 *   Each worker thread has its own XrayIsolate, allowing true parallel parsing.
 *   Results are lightweight metadata (symbols, types, locations) that can be
 *   safely transferred to the main thread for merging.
 *
 *   Main Thread                Worker Pool (N threads)
 *   ───────────                ───────────────────────
 *   Submit files ───────────▶  Worker 1 (Isolate A) → Parse → Extract symbols
 *                              Worker 2 (Isolate B) → Parse → Extract symbols
 *                              Worker 3 (Isolate C) → Parse → Extract symbols
 *                                           │
 *                                           ▼
 *   Merge results ◀─────────  Lightweight symbol metadata (no GC objects)
 */

#ifndef XLSP_INDEX_POOL_H
#define XLSP_INDEX_POOL_H

#include <stdint.h>
#include <stdbool.h>
#include "../../base/xthread.h"
#include <stdatomic.h>
#include "../../base/xdefs.h"
#include "xlsp_types.h"  // For XrLspSymbolKind

#include "../../base/xforward_decl.h"
typedef struct XrLspServer XrLspServer;

// ============================================================================
// Index Result Structures (lightweight, no GC objects)
// ============================================================================

// Lightweight symbol info (can be passed between threads)
typedef struct XrLspIndexSymbol {
    char *name;                     // Symbol name (owned, must free)
    char *type_str;                 // Type as string (owned, may be NULL)
    XrLspSymbolKind kind;           // Symbol kind
    int line;                       // Definition line (0-based)
    int column;                     // Definition column (0-based)
    int end_line;                   // End line
    int end_column;                 // End column
    bool is_exported;               // Is exported?
    bool is_definition;             // Is definition (vs reference)?
    struct XrLspIndexSymbol *next;  // Linked list
} XrLspIndexSymbol;

// Index result for a single file
typedef struct XrLspIndexResult {
    char *uri;                      // File URI (owned)
    char *path;                     // File path (owned)
    XrLspIndexSymbol *symbols;      // Linked list of symbols
    int symbol_count;               // Number of symbols
    bool success;                   // Parse succeeded?
    char *error_message;            // Error message if failed (owned, may be NULL)
    struct XrLspIndexResult *next;  // Linked list for batch results
} XrLspIndexResult;

// ============================================================================
// Index Worker Pool
// ============================================================================

#define XLSP_INDEX_POOL_SIZE 4  // Number of worker threads

// Work item for the pool
typedef struct XrLspIndexWork {
    char *path;                   // File path to index (owned)
    char *uri;                    // File URI (owned)
    struct XrLspIndexWork *next;  // Linked list
} XrLspIndexWork;

// Worker thread state
typedef struct XrLspIndexWorker {
    xr_thread_t thread;           // Thread handle
    XrayIsolate *isolate;         // Per-worker Isolate
    int worker_id;                // Worker ID (0 to N-1)
    struct XrLspIndexPool *pool;  // Back-pointer to pool
} XrLspIndexWorker;

// Index pool state
typedef struct XrLspIndexPool {
    // Workers
    XrLspIndexWorker workers[XLSP_INDEX_POOL_SIZE];
    int worker_count;

    // Work queue (MPMC with mutex for simplicity)
    XrLspIndexWork *work_head;
    XrLspIndexWork *work_tail;
    int work_count;
    xr_mutex_t work_mutex;
    xr_cond_t work_cond;

    // Result queue
    XrLspIndexResult *result_head;
    XrLspIndexResult *result_tail;
    int result_count;
    xr_mutex_t result_mutex;

    // Notification pipe (to wake main thread)
    int notify_fd[2];

    // Control
    atomic_bool running;
    atomic_int active_workers;  // Number of workers currently processing

    // Progress tracking
    atomic_int files_submitted;
    atomic_int files_completed;

    // Server reference (for result callback)
    XrLspServer *server;
} XrLspIndexPool;

// ============================================================================
// API Functions
// ============================================================================

// Create index pool (starts worker threads)
XR_FUNC XrLspIndexPool *xlsp_index_pool_new(XrLspServer *server);

// Destroy index pool (stops workers, frees resources)
XR_FUNC void xlsp_index_pool_free(XrLspIndexPool *pool);

// Submit file for indexing
XR_FUNC void xlsp_index_pool_submit(XrLspIndexPool *pool, const char *path, const char *uri);

// Submit multiple files for indexing
XR_FUNC void xlsp_index_pool_submit_batch(XrLspIndexPool *pool, char **paths, int count);

// Get notification fd (for select/poll in main loop)
XR_FUNC int xlsp_index_pool_get_notify_fd(XrLspIndexPool *pool);

// Poll for completed results (call from main thread)
// Returns list of results, caller must free with xlsp_index_result_free_list
XR_FUNC XrLspIndexResult *xlsp_index_pool_poll(XrLspIndexPool *pool);

// Check if pool is idle (no pending or in-progress work)
XR_FUNC bool xlsp_index_pool_is_idle(XrLspIndexPool *pool);

// Get progress info
XR_FUNC void xlsp_index_pool_get_progress(XrLspIndexPool *pool, int *submitted, int *completed);

// ============================================================================
// Result Memory Management
// ============================================================================

// Free a single index result
XR_FUNC void xlsp_index_result_free(XrLspIndexResult *result);

// Free a list of index results
XR_FUNC void xlsp_index_result_free_list(XrLspIndexResult *list);

// Free a single symbol
XR_FUNC void xlsp_index_symbol_free(XrLspIndexSymbol *sym);

#endif  // XLSP_INDEX_POOL_H
