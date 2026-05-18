/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_server.h - LSP server main interface
 *
 * KEY CONCEPT:
 *   Main LSP server that handles requests and notifications.
 */

#ifndef XLSP_SERVER_H
#define XLSP_SERVER_H

#include "xlsp_transport.h"
#include "../../base/xjson.h"
#include "xlsp_types.h"
#include "xlsp_async.h"
#include "xray_isolate.h"
#include "../../base/xarena.h"
#include <stdbool.h>
#include <stdio.h>  // FILE *log_file in XrLspServer

// Forward declarations
typedef struct XrLspDocument XrLspDocument;
typedef struct XrLspServer XrLspServer;
typedef struct AstNode AstNode;
typedef struct XaAnalyzer XaAnalyzer;

// Document in the workspace
struct XrLspDocument {
    char *uri;
    char *content;  // Separately managed (needs incremental update)
    size_t length;
    int version;

    // Back-pointer to owning server (for accessing per-server state)
    struct XrLspServer *server;

    // Diagnostic debounce: timer-based (no background thread sleep)
    uint64_t last_change_time;
    uint64_t diagnostic_deadline;  // 0 = no pending diagnostic
    bool diag_pending;             // true while queued in pending_diag[]

    // Line index for position conversion
    uint32_t *line_offsets;
    int line_count;

    // Arena for document-owned allocations (AST, diagnostics, etc.)
    // Reset on each parse, destroyed on document close
    XrArena arena;

    // Analysis cache (allocated from arena)
    bool dirty;
    AstNode *ast;  // Arena allocated
    bool parse_error;
    char *error_message;  // Arena allocated
    int error_line;

    // Cached diagnostics from parser (XrJsonValue*, arena allocated)
    void *cached_diagnostics;

    // Incremental parsing support
    uint64_t content_hash;   // Hash for quick change detection
    int last_change_line;    // Line where last change occurred
    int last_change_offset;  // Offset where last change occurred

    // Semantic tokens cache (for delta encoding)
    uint32_t *prev_sem_tokens;     // Previous encoded token data
    int prev_sem_token_count;      // Number of uint32_t values
    uint32_t sem_token_result_id;  // Monotonically increasing result ID

    // Import cache (invalidated on content change)
    // Forward declared, defined in xlsp_imports.h
    struct XlspImportInfo *cached_imports;
    uint64_t imports_content_hash;  // Hash when imports were parsed

    struct XrLspDocument *next;
};

// Document hash table bucket
typedef struct XrLspDocBucket {
    XrLspDocument *doc;
    struct XrLspDocBucket *next;
} XrLspDocBucket;

// Document hash table for O(1) lookup
typedef struct XrLspDocTable {
    XrLspDocBucket **buckets;
    int bucket_count;
    int doc_count;
} XrLspDocTable;

// Forward declaration for method hash table
typedef struct MethodHashEntry MethodHashEntry;

// Forward declaration for file exports cache
typedef struct XlspFileExports XlspFileExports;

// Forward declaration for index pool
typedef struct XrLspIndexPool XrLspIndexPool;

// Method hash table size
#define METHOD_HASH_SIZE 64

// Maximum workspace folders
#define MAX_WORKSPACE_FOLDERS 16

// Per-tick time budget (ms) for draining pending background analysis
#define ANALYSIS_DRAIN_BUDGET_MS 5

// Pending analysis entry (file waiting for main-thread re-analysis)
typedef struct XlspPendingAnalysis {
    char *uri;
    char *path;
} XlspPendingAnalysis;

// Maximum pending requests to track (for cancellation support)
#define MAX_PENDING_REQUESTS 64

// JSON-RPC 2.0 request id. The spec allows either a number OR a string
// (null is reserved and we treat it as "notification"). Existing LSP
// clients like VS Code always use integers, but correctness-wise we
// MUST echo back the exact shape the client sent us.
typedef enum {
    XLSP_ID_NONE = 0,  // Notification (no id field / id == null)
    XLSP_ID_NUMBER,    // Integer or real id
    XLSP_ID_STRING,    // String id (owned, xr_free on clear)
} XlspRequestIdKind;

typedef struct XlspRequestId {
    XlspRequestIdKind kind;
    union {
        double number;
        char *string;  // xr_strdup'd when kind == XLSP_ID_STRING
    } as;
} XlspRequestId;

// Pending request entry (for $/cancelRequest support)
typedef struct XlspPendingRequest {
    XlspRequestId id;     // Request id (number or string, owned)
    const char *method;   // Method name (static, for logging)
    bool cancelled;       // Whether this request was cancelled
    uint64_t start_time;  // Start time (monotonic ms)
} XlspPendingRequest;

// Pending requests tracker (ring buffer)
typedef struct XlspPendingRequests {
    XlspPendingRequest requests[MAX_PENDING_REQUESTS];
    int head;   // Next write position
    int count;  // Number of active entries
} XlspPendingRequests;

// Workspace folder with lifecycle state
typedef struct XlspWorkspaceFolder {
    char *uri;
    char *name;
    char *path;            // Derived from uri
    bool config_loaded;    // xray.toml loaded for this folder
    bool index_requested;  // Background indexing requested
    bool index_completed;  // Background indexing finished
} XlspWorkspaceFolder;

// Ignore pattern for workspace scanning
typedef struct XlspIgnorePattern {
    char *pattern;     // Pattern string (e.g., "build", "build-*", "*.test.xr")
    bool is_glob;      // true if contains glob wildcards (* or ?)
    bool is_dir_only;  // true if pattern applies only to directories
} XlspIgnorePattern;

// Server configuration (from workspace/configuration and xray.toml)
typedef struct XlspConfig {
    // Diagnostic settings
    int diagnostic_debounce_ms;
    bool diagnostics_enabled;

    // Completion settings
    int completion_max_items;

    // Formatting settings
    int format_tab_size;
    int format_max_line_length;
    bool format_insert_spaces;
    bool format_align_match_arms;
    bool format_align_enum_values;
    bool format_align_struct_fields;
    bool format_align_trailing_comments;
    bool format_wrap_long_lines;
    bool format_multiline_trailing_comma;

    // Inlay hints settings
    bool inlay_hints_type_annotations;
    bool inlay_hints_parameter_names;

    // Workspace ignore patterns (from xray.toml [lsp] section)
    XlspIgnorePattern *ignore_patterns;
    int ignore_pattern_count;
    int ignore_pattern_capacity;

    // Logging settings
    char *log_path;      // Log file path (NULL = no file logging, "" = default)
    bool log_to_stderr;  // Also log to stderr (default: true)
} XlspConfig;

// Ignore pattern API
XR_FUNC void xlsp_config_add_ignore(XlspConfig *config, const char *pattern, bool is_dir_only);
XR_FUNC void xlsp_config_free_ignores(XlspConfig *config);
XR_FUNC bool xlsp_config_should_ignore(XlspConfig *config, const char *name, bool is_dir);
XR_FUNC void xlsp_config_load_defaults(XlspConfig *config);
XR_FUNC bool xlsp_config_load_from_toml(XlspConfig *config, const char *root_path);

// LSP Server state
struct XrLspServer {
    XrLspTransport *transport;

    // Parser context
    XrayIsolate *isolate;

    // Documents (hash table for O(1) lookup by URI)
    XrLspDocTable *doc_table;

    // Workspace-level static analyzer (unified index for all cross-file features)
    XaAnalyzer *workspace_analyzer;

    // Server state — the LSP lifecycle is a strict state machine:
    //   (pre-init) -> initialized -> shutdown_received -> exit_received
    // After `shutdown` we must keep the process alive and only reply
    // to `shutdown`/`exit`; all other requests are rejected with
    // -32002 (InvalidRequest). Only `exit` tears down the event loop.
    bool initialized;
    bool shutdown_received;
    bool exit_received;

    // Workspace folders (unified — root_path/root_uri folded in at initialize)
    // root_path() accessor returns workspace_folders[0].path or NULL.
    XlspWorkspaceFolder workspace_folders[MAX_WORKSPACE_FOLDERS];
    int workspace_folder_count;

    // Server configuration
    XlspConfig config;

    // Capabilities
    struct {
        bool completion;
        bool hover;
        bool definition;
        bool references;
        bool document_symbol;
        bool formatting;
        bool rename;
    } capabilities;

    // Background task system
    XrLspAsync *async;

    // Multi-isolate parallel indexing pool
    XrLspIndexPool *index_pool;

    // Workspace indexing state
    bool indexing_in_progress;
    int files_indexed;
    int files_total;
    void *index_task_data;  // For incremental indexing

    // Method dispatch table (per-server instead of global)
    MethodHashEntry *method_hash_table[METHOD_HASH_SIZE];
    bool method_table_initialized;

    // Logging (per-server instead of global)
    FILE *log_file;
    bool log_file_checked;  // Have we tried to open the log file?

    // File exports cache (hash table for O(1) lookup by file path)
    struct XlspExportsCache *exports_cache;

    // Pending diagnostics queue (growable, avoids full doc_table scan)
    XrLspDocument **pending_diag;
    int pending_diag_count;
    int pending_diag_capacity;

    // Pending background analysis queue (budgeted drain per tick)
    XlspPendingAnalysis *pending_analysis;
    int pending_analysis_count;
    int pending_analysis_capacity;

    // Request cancellation support ($/cancelRequest)
    XlspPendingRequests pending_requests;

    // Counter for server-initiated request IDs
    int64_t next_request_id;

    // Progress reporting token (for workspace indexing)
    char *index_progress_token;
    bool index_cancelled;

    // Monotonic counter for per-server progress token generation.
    // Kept here (not as a file-scope mutable global) so multiple servers —
    // including test harnesses — don't collide on "xlsp-progress-N".
    int progress_token_counter;
};

// Create and destroy server
XR_FUNC XrLspServer *xlsp_server_new(void);
XR_FUNC void xlsp_server_free(XrLspServer *server);

// Main event loop - runs until shutdown
XR_FUNC int xlsp_server_run(XrLspServer *server);

// Document management
XR_FUNC XrLspDocument *xlsp_document_open(XrLspServer *server, const char *uri, const char *text,
                                          int version);
XR_FUNC void xlsp_document_change(XrLspDocument *doc, XrLspRange *range, const char *text);
XR_FUNC void xlsp_document_close(XrLspServer *server, const char *uri);
XR_FUNC XrLspDocument *xlsp_document_get(XrLspServer *server, const char *uri);

// On-demand document loading (for unopened files)
XR_FUNC XrLspDocument *xlsp_document_get_or_load(XrLspServer *server, const char *uri);
XR_FUNC void xlsp_document_free_temp(XrLspDocument *doc);

// Position utilities
XR_FUNC uint32_t xlsp_position_to_offset(XrLspDocument *doc, XrLspPosition pos);
XR_FUNC XrLspPosition xlsp_offset_to_position(XrLspDocument *doc, uint32_t offset);

// Progress reporting (for workspace indexing)
XR_FUNC char *xlsp_progress_begin(XrLspServer *server, const char *title, const char *message,
                                  bool cancellable);
XR_FUNC void xlsp_progress_report(XrLspServer *server, const char *token, const char *message,
                                  int percentage);
XR_FUNC void xlsp_progress_end(XrLspServer *server, const char *token, const char *message);

// Logging
XR_FUNC void lsp_log(const char *fmt, ...);
XR_FUNC void xlsp_set_log_server(XrLspServer *server);

// ============================================================================
// Internal helpers (shared between handler files, not public API)
// ============================================================================

// JSON-RPC transport helpers
XR_FUNC void xlsp_send_notification(XrLspServer *server, const char *method, XrJsonValue *params);
XR_FUNC void xlsp_send_request(XrLspServer *server, const char *method, XrJsonValue *params);

// URI/path conversion (defined as static inline in xlsp_utils.h)

// Diagnostics scheduling
XR_FUNC void xlsp_publish_diagnostics(XrLspServer *server, XrLspDocument *doc);
XR_FUNC void xlsp_schedule_diagnostics(XrLspServer *server, XrLspDocument *doc);
XR_FUNC void xlsp_clear_all_diagnostics(XrLspServer *server);

#endif  // XLSP_SERVER_H
