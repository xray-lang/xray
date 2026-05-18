/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_server.c - LSP server implementation
 */

#include "xlsp_server.h"
#include "xlsp_analysis.h"
#include "xlsp_ast_utils.h"
#include "xlsp_workspace.h"
#include "xlsp_index_pool.h"
#include "../../base/xjson.h"
#include "xlsp_cache.h"
#include "../../frontend/analyzer/xanalyzer.h"
#include "xlsp_semantic_tokens.h"
#include "xlsp_inlay_hints.h"
#include "xlsp_imports.h"
#include "xlsp_folding.h"
#include "xlsp_code_action.h"
#include "xlsp_call_hierarchy.h"
#include "xlsp_extra_handlers.h"
#include "xlsp_rename.h"
#include "xlsp_utils.h"
#include "../../frontend/parser/xast_nodes.h"
#include "../../base/xhash.h"
#include "../../runtime/object/xutf8.h"
#include "../../os/os_poll.h"  // Cross-platform poll abstraction
#include "xray_version.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include "../../base/xmalloc.h"
#include "../../base/xchecks.h"
#include "../../os/os_time.h"

// Fallback diagnostic debounce interval (ms). The live value is
// server->config.diagnostic_debounce_ms, which is populated from
// xray.toml / workspace/didChangeConfiguration. This constant is used
// only before the server has finished initialising.
#define DIAGNOSTIC_DEBOUNCE_MS 300

// Get monotonic time in milliseconds
static uint64_t get_monotonic_ms(void) {
    return xr_time_monotonic_ms();
}

// Handler implementations extracted into separate files
#include "xlsp_handlers_lifecycle.h"
#include "xlsp_handlers_workspace.h"
#include "xlsp_handlers_textdoc.h"

static void free_method_table(XrLspServer *server);

// Thread-local server pointer for logging (set before each operation)
static _Thread_local XrLspServer *tls_server = NULL;

void xlsp_set_log_server(XrLspServer *server) {
    tls_server = server;
}

/* void(*)(void*) wrapper — avoids UB from calling through
 * an incompatible function pointer type. */
static void set_log_server_wrapper(void *ctx) {
    xlsp_set_log_server((XrLspServer *) ctx);
}

// Default log path (can be overridden by XRAY_LSP_LOG env var or xray.toml)
#define XLSP_DEFAULT_LOG_PATH "/tmp/xray_lsp.log"

// Get log path from configuration or environment
static const char *get_log_path(XrLspServer *server) {
    // 1. Environment variable takes highest priority
    const char *env_path = getenv("XRAY_LSP_LOG");
    if (env_path) {
        if (strcmp(env_path, "none") == 0 || strcmp(env_path, "0") == 0) {
            return NULL;  // Disable file logging
        }
        return env_path;
    }

    // 2. Config from xray.toml
    if (server && server->config.log_path) {
        if (server->config.log_path[0] == '\0') {
            return XLSP_DEFAULT_LOG_PATH;  // Empty string means default
        }
        if (strcmp(server->config.log_path, "none") == 0) {
            return NULL;  // Disable file logging
        }
        return server->config.log_path;
    }

    // 3. Default path
    return XLSP_DEFAULT_LOG_PATH;
}

void lsp_log(const char *fmt, ...) {
    XrLspServer *server = tls_server;

    // Open log file on first call (per-server)
    if (server && !server->log_file && !server->log_file_checked) {
        server->log_file_checked = true;
        const char *log_path = get_log_path(server);
        if (log_path) {
            server->log_file = fopen(log_path, "a");
            if (!server->log_file) {
                fprintf(stderr, "[xray-lsp] Warning: Cannot open log file: %s\n", log_path);
            }
        }
    }

    va_list args;
    va_start(args, fmt);

    // Print to stderr (unless disabled in config)
    bool log_to_stderr = !server || server->config.log_to_stderr;
    if (log_to_stderr) {
        va_list args_copy;
        va_copy(args_copy, args);
        fprintf(stderr, "[xray-lsp] ");
        vfprintf(stderr, fmt, args_copy);
        fprintf(stderr, "\n");
        fflush(stderr);
        va_end(args_copy);
    }

    // Also log to file with timestamp
    FILE *log_file = server ? server->log_file : NULL;
    if (log_file) {
        time_t now = time(NULL);
        struct tm *tm_info = localtime(&now);
        char time_buf[32];
        strftime(time_buf, sizeof(time_buf), "%H:%M:%S", tm_info);

        fprintf(log_file, "[%s] ", time_buf);
        vfprintf(log_file, fmt, args);
        fprintf(log_file, "\n");
        fflush(log_file);
    }

    va_end(args);
}

// ============================================================================
// Document Hash Table Operations (O(1) lookup by URI)
// ============================================================================

#define DOC_TABLE_INITIAL_SIZE 32
#define DOC_TABLE_LOAD_FACTOR_NUM 3   // Numerator of load factor (75% = 3/4)
#define DOC_TABLE_LOAD_FACTOR_DEN 4   // Denominator of load factor
#define DOC_TABLE_MAX_SIZE (1 << 16)  // Max 65536 buckets

static uint32_t hash_uri(const char *uri) {
    return xr_hash_bytes(uri, strlen(uri));
}

// Forward declaration for resize
static void doc_table_resize(XrLspDocTable *table, int new_size);

static XrLspDocTable *doc_table_new(void) {
    XrLspDocTable *table = xr_calloc(1, sizeof(XrLspDocTable));
    if (!table)
        return NULL;
    table->bucket_count = DOC_TABLE_INITIAL_SIZE;
    table->buckets = xr_calloc(table->bucket_count, sizeof(XrLspDocBucket *));
    if (!table->buckets) {
        xr_free(table);
        return NULL;
    }
    return table;
}

static void doc_table_free(XrLspDocTable *table) {
    if (!table)
        return;
    for (int i = 0; i < table->bucket_count; i++) {
        XrLspDocBucket *bucket = table->buckets[i];
        while (bucket) {
            XrLspDocBucket *next = bucket->next;
            XrLspDocument *doc = bucket->doc;
            if (doc) {
                xlsp_invalidate_import_cache(doc);
                xlsp_free_document_cache(doc);
                xr_arena_destroy(&doc->arena);
                xr_free(doc->uri);
                xr_free(doc->content);
                xr_free(doc->line_offsets);
                xr_free(doc);
            }
            xr_free(bucket);
            bucket = next;
        }
    }
    xr_free(table->buckets);
    xr_free(table);
}

static XrLspDocument *doc_table_get(XrLspDocTable *table, const char *uri) {
    if (!table || !uri)
        return NULL;
    uint32_t hash = hash_uri(uri) % table->bucket_count;
    XrLspDocBucket *bucket = table->buckets[hash];
    while (bucket) {
        if (bucket->doc && strcmp(bucket->doc->uri, uri) == 0) {
            return bucket->doc;
        }
        bucket = bucket->next;
    }
    return NULL;
}

// Resize hash table to new_size buckets (internal, called when load factor exceeded)
static void doc_table_resize(XrLspDocTable *table, int new_size) {
    if (!table || new_size <= table->bucket_count)
        return;
    if (new_size > DOC_TABLE_MAX_SIZE)
        new_size = DOC_TABLE_MAX_SIZE;

    // Allocate new bucket array
    XrLspDocBucket **new_buckets = xr_calloc(new_size, sizeof(XrLspDocBucket *));
    if (!new_buckets) {
        // Allocation failed, continue with current size (graceful degradation)
        lsp_log("Warning: Failed to resize doc table to %d buckets", new_size);
        return;
    }

    // Rehash all existing entries
    int rehashed = 0;
    for (int i = 0; i < table->bucket_count; i++) {
        XrLspDocBucket *bucket = table->buckets[i];
        while (bucket) {
            XrLspDocBucket *next = bucket->next;

            // Compute new hash with new bucket count
            uint32_t new_hash = hash_uri(bucket->doc->uri) % new_size;

            // Insert into new bucket array (prepend to chain)
            bucket->next = new_buckets[new_hash];
            new_buckets[new_hash] = bucket;
            rehashed++;

            bucket = next;
        }
    }

    // Replace old buckets with new ones
    xr_free(table->buckets);
    table->buckets = new_buckets;
    table->bucket_count = new_size;

    lsp_log("Doc table resized: %d buckets, %d documents", new_size, rehashed);
}

// Check if table needs resize (load factor > 75%)
static inline bool doc_table_needs_resize(XrLspDocTable *table) {
    // Using integer math: doc_count * 4 > bucket_count * 3
    return table->doc_count * DOC_TABLE_LOAD_FACTOR_DEN >
           table->bucket_count * DOC_TABLE_LOAD_FACTOR_NUM;
}

static void doc_table_put(XrLspDocTable *table, XrLspDocument *doc) {
    if (!table || !doc || !doc->uri)
        return;

    // Check if resize is needed before insertion
    if (doc_table_needs_resize(table) && table->bucket_count < DOC_TABLE_MAX_SIZE) {
        doc_table_resize(table, table->bucket_count * 2);
    }

    uint32_t hash = hash_uri(doc->uri) % table->bucket_count;
    XrLspDocBucket *bucket = xr_malloc(sizeof(XrLspDocBucket));
    if (!bucket) {
        lsp_log("Error: Failed to allocate doc bucket for %s", doc->uri);
        return;
    }
    bucket->doc = doc;
    bucket->next = table->buckets[hash];
    table->buckets[hash] = bucket;
    table->doc_count++;
}

static void doc_table_remove(XrLspDocTable *table, const char *uri) {
    if (!table || !uri)
        return;
    uint32_t hash = hash_uri(uri) % table->bucket_count;
    XrLspDocBucket **pp = &table->buckets[hash];
    while (*pp) {
        if ((*pp)->doc && strcmp((*pp)->doc->uri, uri) == 0) {
            XrLspDocBucket *to_free = *pp;
            *pp = to_free->next;
            XrLspDocument *doc = to_free->doc;
            if (doc) {
                // Free import cache
                xlsp_invalidate_import_cache(doc);
                // Free diagnostics cache
                xlsp_free_document_cache(doc);
                // Destroy arena first (frees AST, diagnostics, etc.)
                xr_arena_destroy(&doc->arena);
                // Free separately managed resources
                xr_free(doc->uri);
                xr_free(doc->content);
                xr_free(doc->line_offsets);
                xr_free(doc);
            }
            xr_free(to_free);
            table->doc_count--;
            return;
        }
        pp = &(*pp)->next;
    }
}

XrLspServer *xlsp_server_new(void) {
    XrLspServer *server = xr_calloc(1, sizeof(XrLspServer));
    if (!server)
        return NULL;

    server->transport = xlsp_transport_stdio();
    if (!server->transport) {
        xr_free(server);
        return NULL;
    }

    // Create isolate for parsing
    XrayIsolateParams params;
    xray_isolate_params_init(&params);
    xray_isolate_setup_full(&params);
    params.enable_gc = false;  // No need for GC in LSP
    server->isolate = xray_isolate_new(&params);
    if (!server->isolate) {
        lsp_log("Warning: Failed to create isolate, parser features limited");
    }

    // Enable all capabilities by default
    server->capabilities.completion = true;
    server->capabilities.hover = true;
    server->capabilities.definition = true;
    server->capabilities.references = true;
    server->capabilities.document_symbol = true;
    server->capabilities.formatting = true;
    server->capabilities.rename = true;

    // Create document hash table
    server->doc_table = doc_table_new();
    if (!server->doc_table) {
        xlsp_transport_free(server->transport);
        if (server->isolate)
            xray_isolate_delete(server->isolate);
        xr_free(server);
        return NULL;
    }

    // Create workspace-level analyzer (unified index for all cross-file features)
    server->workspace_analyzer = xa_analyzer_new(server->isolate);
    if (!server->workspace_analyzer) {
        lsp_log("Warning: Failed to create workspace analyzer");
    }

    // Create background task system.
    // Pass the log-server init hook so the worker thread shares the same
    // log file / stderr routing as the main loop. The hook is installed
    // before pthread_create inside xlsp_async_new to avoid a data race.
    server->async = xlsp_async_new(set_log_server_wrapper, server);
    if (!server->async) {
        lsp_log("Warning: Failed to create async worker, background tasks disabled");
    }

    // Default configuration
    server->config.diagnostic_debounce_ms = 300;
    server->config.diagnostics_enabled = true;
    server->config.completion_max_items = 100;
    server->config.format_tab_size = 4;
    server->config.format_max_line_length = 100;
    server->config.format_insert_spaces = true;
    server->config.format_align_match_arms = false;
    server->config.format_align_enum_values = false;
    server->config.format_align_struct_fields = false;
    server->config.format_align_trailing_comments = false;
    server->config.format_wrap_long_lines = false;
    server->config.format_multiline_trailing_comma = true;
    server->config.inlay_hints_type_annotations = true;
    server->config.inlay_hints_parameter_names = true;

    // Logging defaults
    server->config.log_path = NULL;       // NULL = use default path
    server->config.log_to_stderr = true;  // Always log to stderr by default

    // Load default ignore patterns
    xlsp_config_load_defaults(&server->config);

    return server;
}

// Forward decl — definition lives alongside the rest of the request-id
// helpers further down the file.
static void xlsp_request_id_free(XlspRequestId *id);

void xlsp_server_free(XrLspServer *server) {
    if (!server)
        return;

    // Free document hash table (frees all documents)
    if (server->doc_table) {
        doc_table_free(server->doc_table);
    }

    // Free workspace folders
    for (int i = 0; i < server->workspace_folder_count; i++) {
        xr_free(server->workspace_folders[i].uri);
        xr_free(server->workspace_folders[i].name);
        xr_free(server->workspace_folders[i].path);
    }

    xlsp_transport_free(server->transport);

    if (server->workspace_analyzer) {
        xa_analyzer_free(server->workspace_analyzer);
    }

    if (server->async) {
        xlsp_async_free(server->async);
    }

    // Free parallel index pool
    if (server->index_pool) {
        xlsp_index_pool_free(server->index_pool);
    }

    if (server->isolate) {
        xray_isolate_delete(server->isolate);
    }

    // Free method dispatch table
    free_method_table(server);

    // Close log file
    if (server->log_file) {
        fclose(server->log_file);
    }

    // Free exports cache (handled by xlsp_imports)
    xlsp_free_exports_cache(server);

    // Free ignore patterns
    xlsp_config_free_ignores(&server->config);

    // Free log path
    if (server->config.log_path) {
        xr_free(server->config.log_path);
    }

    if (server->index_progress_token) {
        xr_free(server->index_progress_token);
        server->index_progress_token = NULL;
    }

    // Free pending diagnostics queue
    xr_free(server->pending_diag);

    // Free pending analysis queue
    xlsp_free_pending_analysis(server);

    // Release any still-live string ids in the pending-request ring
    // buffer. After a clean shutdown the ring is usually empty, but
    // abrupt disconnects can leave entries behind; failing to free
    // their owned strings would leak on exit.
    for (int i = 0; i < MAX_PENDING_REQUESTS; i++) {
        xlsp_request_id_free(&server->pending_requests.requests[i].id);
    }

    xr_free(server);
}

// Build line offset index for a document
static bool build_line_index(XrLspDocument *doc) {
    xr_free(doc->line_offsets);
    doc->line_offsets = NULL;
    doc->line_count = 0;

    // Count lines
    int line_count = 1;
    for (size_t i = 0; i < doc->length; i++) {
        if (doc->content[i] == '\n')
            line_count++;
    }

    doc->line_offsets = xr_malloc(line_count * sizeof(uint32_t));
    if (!doc->line_offsets)
        return false;

    doc->line_count = line_count;

    // Build index
    doc->line_offsets[0] = 0;
    int line = 1;
    for (size_t i = 0; i < doc->length && line < line_count; i++) {
        if (doc->content[i] == '\n') {
            doc->line_offsets[line++] = i + 1;
        }
    }
    return true;
}

XrLspDocument *xlsp_document_open(XrLspServer *server, const char *uri, const char *text,
                                  int version) {
    XrLspDocument *doc = xr_calloc(1, sizeof(XrLspDocument));
    if (!doc)
        return NULL;

    doc->uri = xr_strdup(uri);
    if (!doc->uri) {
        xr_free(doc);
        return NULL;
    }

    doc->content = xr_strdup(text);
    if (!doc->content) {
        xr_free(doc->uri);
        xr_free(doc);
        return NULL;
    }

    doc->length = strlen(text);
    doc->version = version;
    doc->server = server;
    doc->dirty = true;
    doc->content_hash = XLSP_CONTENT_HASH_UNINITIALIZED;

    // Initialize document arena (64KB initial size)
    xr_arena_init(&doc->arena, 64 * 1024);

    if (!build_line_index(doc)) {
        xr_arena_destroy(&doc->arena);
        xr_free(doc->content);
        xr_free(doc->uri);
        xr_free(doc);
        return NULL;
    }

    // Add to document hash table (O(1) lookup)
    doc_table_put(server->doc_table, doc);

    lsp_log("Opened document: %s (%zu bytes)", uri, doc->length);

    return doc;
}

void xlsp_document_change(XrLspDocument *doc, XrLspRange *range, const char *text) {
    if (!doc || !text)
        return;

    if (!range) {
        // Full document sync
        char *new_content = xr_strdup(text);
        if (!new_content)
            return;  // Keep old content on failure

        xr_free(doc->content);
        doc->content = new_content;
        doc->length = strlen(text);
    } else {
        // Incremental sync
        uint32_t start = xlsp_position_to_offset(doc, range->start);
        uint32_t end = xlsp_position_to_offset(doc, range->end);
        size_t text_len = strlen(text);
        size_t new_len = doc->length - (end - start) + text_len;

        char *new_content = xr_malloc(new_len + 1);
        if (!new_content)
            return;  // Keep old content on failure

        memcpy(new_content, doc->content, start);
        memcpy(new_content + start, text, text_len);
        memcpy(new_content + start + text_len, doc->content + end, doc->length - end);
        new_content[new_len] = '\0';

        xr_free(doc->content);
        doc->content = new_content;
        doc->length = new_len;
    }

    // Hash-based change detection: rapid keystrokes often produce a
    // didChange that arrives with identical bytes (e.g. edit + undo
    // within the debounce window). Skipping the reparse on a hash
    // match keeps the analyzer idle and lets the main loop stay
    // responsive.
    uint64_t new_hash = xlsp_content_hash(doc->content, doc->length);
    if (new_hash != doc->content_hash) {
        doc->content_hash = new_hash;
        doc->dirty = true;
    }
    build_line_index(doc);  // Ignore failure - line_offsets may be NULL
}

void xlsp_document_close(XrLspServer *server, const char *uri) {
    lsp_log("Closed document: %s", uri);
    doc_table_remove(server->doc_table, uri);
}

XrLspDocument *xlsp_document_get(XrLspServer *server, const char *uri) {
    return doc_table_get(server->doc_table, uri);
}

// Upper bound on files we are willing to pull into memory for
// on-demand analysis (Go-to-Definition targets, import resolution,
// background indexing). LSP spec carries no such limit, but we enforce
// one to avoid accidentally loading huge generated artifacts (minified
// JS-in-text, massive test fixtures, binary blobs mistyped as .xr) —
// all of which would OOM the server long before they parsed.
#define XLSP_MAX_DOCUMENT_BYTES (50u * 1024u * 1024u)  // 50 MiB

// Get or load document on-demand (for unopened files like Go to Definition targets)
XrLspDocument *xlsp_document_get_or_load(XrLspServer *server, const char *uri) {
    // First check if already open
    XrLspDocument *doc = doc_table_get(server->doc_table, uri);
    if (doc)
        return doc;

    const char *path = xlsp_uri_to_path(uri);

    // Try to load from disk
    FILE *f = fopen(path, "r");
    if (!f)
        return NULL;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size < 0) {
        fclose(f);
        return NULL;
    }
    if ((size_t) size >= XLSP_MAX_DOCUMENT_BYTES) {
        lsp_log("Refusing to load %s: %ld bytes exceeds %u-byte limit", path, size,
                XLSP_MAX_DOCUMENT_BYTES);
        fclose(f);
        return NULL;
    }

    char *content = xr_malloc((size_t) size + 1);
    if (!content) {
        fclose(f);
        return NULL;
    }

    size_t read_size = fread(content, 1, (size_t) size, f);
    content[read_size] = '\0';
    fclose(f);

    // Create temporary document (not added to doc_table)
    doc = xr_calloc(1, sizeof(XrLspDocument));
    if (!doc) {
        xr_free(content);
        return NULL;
    }

    doc->uri = xr_strdup(uri);
    if (!doc->uri) {
        xr_free(content);
        xr_free(doc);
        return NULL;
    }

    doc->content = content;
    doc->length = read_size;
    doc->version = 0;
    doc->server = server;
    doc->dirty = true;
    doc->content_hash = XLSP_CONTENT_HASH_UNINITIALIZED;

    // Build line index (ignore failure - document still usable without it)
    build_line_index(doc);

    // Parse document
    xlsp_parse_document(doc, server);

    lsp_log("On-demand loaded: %s", path);
    return doc;
}

// Free a temporarily loaded document
void xlsp_document_free_temp(XrLspDocument *doc) {
    if (!doc)
        return;
    xlsp_invalidate_import_cache(doc);
    xlsp_free_document_cache(doc);
    xr_free(doc->uri);
    xr_free(doc->content);
    xr_free(doc->line_offsets);
    xr_arena_destroy(&doc->arena);
    xr_free(doc);
}

uint32_t xlsp_position_to_offset(XrLspDocument *doc, XrLspPosition pos) {
    if (!doc || !doc->line_offsets)
        return 0;
    if ((int) pos.line >= doc->line_count) {
        return doc->length;
    }

    uint32_t line_start = doc->line_offsets[pos.line];

    // Calculate line length (bytes until next line or end of document)
    uint32_t line_end;
    if ((int) pos.line + 1 < doc->line_count) {
        line_end = doc->line_offsets[pos.line + 1];
        // Exclude newline character from line content
        if (line_end > line_start && doc->content[line_end - 1] == '\n') {
            line_end--;
        }
    } else {
        line_end = doc->length;
    }
    uint32_t line_len = line_end - line_start;

    // Convert UTF-16 code unit offset to byte offset within the line
    size_t byte_offset_in_line =
        xr_utf8_utf16_to_byte_offset(doc->content + line_start, line_len, pos.character);

    uint32_t offset = line_start + (uint32_t) byte_offset_in_line;
    if (offset > doc->length)
        offset = doc->length;
    return offset;
}

XrLspPosition xlsp_offset_to_position(XrLspDocument *doc, uint32_t offset) {
    XrLspPosition pos = {0, 0};
    if (!doc || !doc->line_offsets)
        return pos;

    // Binary search for line
    int lo = 0, hi = doc->line_count - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (doc->line_offsets[mid] <= offset) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }

    pos.line = lo;

    // Calculate line length for UTF-16 conversion
    uint32_t line_start = doc->line_offsets[lo];
    uint32_t line_end;
    if (lo + 1 < doc->line_count) {
        line_end = doc->line_offsets[lo + 1];
        if (line_end > line_start && doc->content[line_end - 1] == '\n') {
            line_end--;
        }
    } else {
        line_end = doc->length;
    }
    uint32_t line_len = line_end - line_start;

    // Convert byte offset within line to UTF-16 code units
    uint32_t byte_offset_in_line = offset - line_start;
    pos.character = (uint32_t) xr_utf8_byte_to_utf16_offset(doc->content + line_start, line_len,
                                                            byte_offset_in_line);

    return pos;
}

// ---------------------------------------------------------------------------
// Request id (JSON-RPC 2.0: number | string)
// ---------------------------------------------------------------------------

// Parse the "id" field of an incoming message. Returns a value-typed
// XlspRequestId whose string variant (if any) is xr_strdup'd and must be
// released via xlsp_request_id_free. A missing / null id yields kind
// XLSP_ID_NONE, which the caller uses to distinguish notifications.
static XlspRequestId parse_request_id(XrJsonValue *msg) {
    XlspRequestId id = {.kind = XLSP_ID_NONE, .as.number = 0};
    XrJsonValue *v = xjson_get(msg, "id");
    if (!v)
        return id;
    if (v->type == XR_JSON_NUMBER) {
        id.kind = XLSP_ID_NUMBER;
        id.as.number = v->is_integer ? (double) v->as.integer : v->as.number;
    } else if (v->type == XR_JSON_STRING) {
        id.kind = XLSP_ID_STRING;
        id.as.string = xr_strdup(v->as.string ? v->as.string : "");
        if (!id.as.string) {
            // OOM — downgrade to NONE so the caller treats it as a
            // notification. The client will probably time out, but we
            // never crash or fabricate a wrong id.
            id.kind = XLSP_ID_NONE;
        }
    }
    // XR_JSON_NULL, bool, array, object → treated as NONE. Per spec null
    // id is reserved; clients should not use it. Logging would be noisy.
    return id;
}

static void xlsp_request_id_free(XlspRequestId *id) {
    if (!id)
        return;
    if (id->kind == XLSP_ID_STRING) {
        xr_free(id->as.string);
        id->as.string = NULL;
    }
    id->kind = XLSP_ID_NONE;
}

static XlspRequestId xlsp_request_id_clone(const XlspRequestId *src) {
    XlspRequestId dst = {.kind = XLSP_ID_NONE, .as.number = 0};
    if (!src)
        return dst;
    dst.kind = src->kind;
    if (src->kind == XLSP_ID_NUMBER) {
        dst.as.number = src->as.number;
    } else if (src->kind == XLSP_ID_STRING) {
        dst.as.string = xr_strdup(src->as.string ? src->as.string : "");
        if (!dst.as.string)
            dst.kind = XLSP_ID_NONE;
    }
    return dst;
}

static bool xlsp_request_id_equals(const XlspRequestId *a, const XlspRequestId *b) {
    if (!a || !b)
        return false;
    if (a->kind != b->kind)
        return false;
    switch (a->kind) {
        case XLSP_ID_NUMBER:
            return a->as.number == b->as.number;
        case XLSP_ID_STRING:
            return a->as.string && b->as.string && strcmp(a->as.string, b->as.string) == 0;
        case XLSP_ID_NONE:
            return true;  // both notifications (rare)
    }
    return false;
}

// Build the JSON id value for a response / request. Returns a JSON
// value (null for XLSP_ID_NONE — used only for error responses that
// have to be emitted before we could parse the id).
static XrJsonValue *xlsp_request_id_to_json(const XlspRequestId *id) {
    if (!id || id->kind == XLSP_ID_NONE)
        return xjson_new_null();
    if (id->kind == XLSP_ID_NUMBER)
        return xjson_new_number(id->as.number);
    return xjson_new_string(id->as.string ? id->as.string : "");
}

// Short debug label, used for log messages only. Returns a pointer into
// a static buffer (single-threaded caller assumed: lsp_log is only ever
// called from the main loop or a thread that already holds tls_server).
static const char *xlsp_request_id_debug(const XlspRequestId *id) {
    static _Thread_local char buf[64];
    if (!id || id->kind == XLSP_ID_NONE)
        return "<notif>";
    if (id->kind == XLSP_ID_NUMBER) {
        snprintf(buf, sizeof(buf), "%.0f", id->as.number);
    } else {
        snprintf(buf, sizeof(buf), "\"%s\"", id->as.string ? id->as.string : "");
    }
    return buf;
}

// ---------------------------------------------------------------------------
// Response / error emitters
// ---------------------------------------------------------------------------

// Send a JSON-RPC response echoing back the client's id exactly.
static void send_response(XrLspServer *server, const XlspRequestId *id, XrJsonValue *result) {
    XrJsonValue *resp = xjson_new_object();
    xjson_object_set(resp, "jsonrpc", xjson_new_string("2.0"));
    xjson_object_set(resp, "id", xlsp_request_id_to_json(id));
    xjson_object_set(resp, "result", result ? result : xjson_new_null());

    size_t len;
    char *json = xjson_stringify(resp, &len);
    xlsp_transport_write(server->transport, json, len);

    xr_free(json);
    xjson_free(resp);
}

// Send a JSON-RPC error response, mirroring the client's id shape.
static void send_error(XrLspServer *server, const XlspRequestId *id, int code,
                       const char *message) {
    XrJsonValue *resp = xjson_new_object();
    xjson_object_set(resp, "jsonrpc", xjson_new_string("2.0"));
    xjson_object_set(resp, "id", xlsp_request_id_to_json(id));

    XrJsonValue *error = xjson_new_object();
    xjson_object_set(error, "code", xjson_new_number(code));
    xjson_object_set(error, "message", xjson_new_string(message));
    xjson_object_set(resp, "error", error);

    size_t len;
    char *json = xjson_stringify(resp, &len);
    xlsp_transport_write(server->transport, json, len);

    xr_free(json);
    xjson_free(resp);
}

// Send a notification
void xlsp_send_notification(XrLspServer *server, const char *method, XrJsonValue *params) {
    XrJsonValue *notif = xjson_new_object();
    xjson_object_set(notif, "jsonrpc", xjson_new_string("2.0"));
    xjson_object_set(notif, "method", xjson_new_string(method));
    if (params) {
        xjson_object_set(notif, "params", params);
    }

    size_t len;
    char *json = xjson_stringify(notif, &len);
    xlsp_transport_write(server->transport, json, len);

    xr_free(json);
    xjson_free(notif);
}

// Send a server-initiated request (fire-and-forget, response ignored)
void xlsp_send_request(XrLspServer *server, const char *method, XrJsonValue *params) {
    XrJsonValue *req = xjson_new_object();
    xjson_object_set(req, "jsonrpc", xjson_new_string("2.0"));
    xjson_object_set(req, "id", xjson_new_number(++server->next_request_id));
    xjson_object_set(req, "method", xjson_new_string(method));
    if (params) {
        xjson_object_set(req, "params", params);
    }

    size_t len;
    char *json = xjson_stringify(req, &len);
    xlsp_transport_write(server->transport, json, len);

    xr_free(json);
    xjson_free(req);
}

// ============================================================================
// Request Cancellation Support ($/cancelRequest)
// ============================================================================

// Add a pending request to track (ring buffer: O(1) insert).
// Takes ownership of *id (moves it into the slot). `method` is a static
// string from the dispatch table and is stored by reference.
static void pending_request_add(XrLspServer *server, XlspRequestId id, const char *method) {
    XlspPendingRequests *pending = &server->pending_requests;

    // Write at head position (overwrites oldest if full). If we're
    // about to overwrite a still-live slot that holds an owned string
    // id, release it first so we don't leak.
    int slot = pending->head;
    if (pending->count == MAX_PENDING_REQUESTS) {
        xlsp_request_id_free(&pending->requests[slot].id);
    }
    pending->requests[slot].id = id;
    pending->requests[slot].method = method;
    pending->requests[slot].cancelled = false;
    pending->requests[slot].start_time = get_monotonic_ms();

    pending->head = (pending->head + 1) % MAX_PENDING_REQUESTS;
    if (pending->count < MAX_PENDING_REQUESTS) {
        pending->count++;
    }
}

// Remove a pending request (when completed).
static void pending_request_remove(XrLspServer *server, const XlspRequestId *id) {
    XlspPendingRequests *pending = &server->pending_requests;
    for (int i = 0; i < pending->count; i++) {
        if (xlsp_request_id_equals(&pending->requests[i].id, id)) {
            xlsp_request_id_free(&pending->requests[i].id);
            pending->requests[i].method = NULL;
            pending->requests[i].cancelled = false;
            break;
        }
    }
}

// Check if a request was cancelled.
static bool pending_request_is_cancelled(XrLspServer *server, const XlspRequestId *id) {
    XlspPendingRequests *pending = &server->pending_requests;
    for (int i = 0; i < pending->count; i++) {
        if (xlsp_request_id_equals(&pending->requests[i].id, id)) {
            return pending->requests[i].cancelled;
        }
    }
    return false;
}

// Mark a request as cancelled (also cancels associated background tasks).
// The async subsystem still keys tasks by int64_t request id — string
// ids cannot be propagated there without a wider refactor, so for now
// we only cancel async tasks for numeric ids. VS Code always uses
// numeric ids so this covers 100% of today's real traffic.
static bool pending_request_cancel(XrLspServer *server, const XlspRequestId *id) {
    XlspPendingRequests *pending = &server->pending_requests;
    bool found = false;

    for (int i = 0; i < pending->count; i++) {
        if (xlsp_request_id_equals(&pending->requests[i].id, id)) {
            pending->requests[i].cancelled = true;
            lsp_log("Request %s (%s) marked as cancelled", xlsp_request_id_debug(id),
                    pending->requests[i].method ? pending->requests[i].method : "unknown");
            found = true;
            break;
        }
    }

    if (server->async && id && id->kind == XLSP_ID_NUMBER) {
        int async_cancelled = xlsp_async_cancel_request(server->async, (int64_t) id->as.number);
        if (async_cancelled > 0) {
            lsp_log("Cancelled %d background tasks for request %s", async_cancelled,
                    xlsp_request_id_debug(id));
        }
    }

    if (!found) {
        lsp_log("Request %s not found for cancellation", xlsp_request_id_debug(id));
    }
    return found;
}

// Handle $/cancelRequest notification
static void handle_cancel_request(XrLspServer *server, XrJsonValue *params) {
    if (!params)
        return;

    // params itself carries the id to cancel, same number-or-string
    // shape as an original request's id.
    XlspRequestId target = parse_request_id(params);
    if (target.kind == XLSP_ID_NONE)
        return;

    pending_request_cancel(server, &target);
    xlsp_request_id_free(&target);
}

// LSP error codes
#define LSP_ERROR_REQUEST_CANCELLED -32800

// Publish diagnostics for a document
void xlsp_publish_diagnostics(XrLspServer *server, XrLspDocument *doc) {
    XrJsonValue *diagnostics = xlsp_analyze_diagnostics(doc);

    XrJsonValue *params = xjson_new_object();
    xjson_object_set(params, "uri", xjson_new_string(doc->uri));
    xjson_object_set(params, "version", xjson_new_number(doc->version));
    xjson_object_set(params, "diagnostics", diagnostics);

    xlsp_send_notification(server, "textDocument/publishDiagnostics", params);
}

// ============================================================================
// Progress Reporting
// ============================================================================

// Begin progress reporting (with optional cancellation support).
// The token counter lives on the server (see XrLspServer::progress_token_counter)
// so that multiple concurrent XrLspServer instances do not collide on token names.
static char *progress_begin_ex(XrLspServer *server, const char *title, const char *message,
                               bool cancellable) {
    XR_DCHECK(server != NULL, "progress_begin_ex: NULL server");
    char token[32];
    snprintf(token, sizeof(token), "xlsp-progress-%d", ++server->progress_token_counter);

    // Create progress token
    XrJsonValue *create_params = xjson_new_object();
    xjson_object_set(create_params, "token", xjson_new_string(token));
    xlsp_send_request(server, "window/workDoneProgress/create", create_params);

    // Send begin notification
    XrJsonValue *params = xjson_new_object();
    xjson_object_set(params, "token", xjson_new_string(token));

    XrJsonValue *value = xjson_new_object();
    xjson_object_set(value, "kind", xjson_new_string("begin"));
    xjson_object_set(value, "title", xjson_new_string(title));
    if (message) {
        xjson_object_set(value, "message", xjson_new_string(message));
    }
    xjson_object_set(value, "cancellable", xjson_new_bool(cancellable));
    xjson_object_set(value, "percentage", xjson_new_number(0));
    xjson_object_set(params, "value", value);

    xlsp_send_notification(server, "$/progress", params);

    return xr_strdup(token);
}

// Begin progress reporting (public API, with cancellation support)
char *xlsp_progress_begin(XrLspServer *server, const char *title, const char *message,
                          bool cancellable) {
    return progress_begin_ex(server, title, message, cancellable);
}

// Report progress (public API)
void xlsp_progress_report(XrLspServer *server, const char *token, const char *message,
                          int percentage) {
    if (!server || !token)
        return;

    XrJsonValue *params = xjson_new_object();
    xjson_object_set(params, "token", xjson_new_string(token));

    XrJsonValue *value = xjson_new_object();
    xjson_object_set(value, "kind", xjson_new_string("report"));
    if (message) {
        xjson_object_set(value, "message", xjson_new_string(message));
    }
    if (percentage >= 0) {
        xjson_object_set(value, "percentage", xjson_new_number(percentage));
    }
    xjson_object_set(params, "value", value);

    xlsp_send_notification(server, "$/progress", params);
}

// End progress reporting (public API)
void xlsp_progress_end(XrLspServer *server, const char *token, const char *message) {
    if (!server || !token)
        return;

    XrJsonValue *params = xjson_new_object();
    xjson_object_set(params, "token", xjson_new_string(token));

    XrJsonValue *value = xjson_new_object();
    xjson_object_set(value, "kind", xjson_new_string("end"));
    if (message) {
        xjson_object_set(value, "message", xjson_new_string(message));
    }
    xjson_object_set(params, "value", value);

    xlsp_send_notification(server, "$/progress", params);
}

// ============================================================================
// Diagnostic Debounce
// ============================================================================

// Schedule debounced diagnostics for a document.
// Uses timer-based debounce in the main event loop instead of blocking
// a background thread with usleep.
void xlsp_schedule_diagnostics(XrLspServer *server, XrLspDocument *doc) {
    if (!server->config.diagnostics_enabled)
        return;

    uint64_t now = get_monotonic_ms();
    doc->last_change_time = now;
    int debounce_ms = server->config.diagnostic_debounce_ms > 0
                          ? server->config.diagnostic_debounce_ms
                          : DIAGNOSTIC_DEBOUNCE_MS;
    doc->diagnostic_deadline = now + (uint64_t) debounce_ms;

    // O(1) dedup via per-document flag
    if (doc->diag_pending)
        return;

    // Grow queue if needed
    if (server->pending_diag_count >= server->pending_diag_capacity) {
        int new_cap = server->pending_diag_capacity ? server->pending_diag_capacity * 2 : 16;
        XrLspDocument **tmp =
            xr_realloc(server->pending_diag, (size_t) new_cap * sizeof(XrLspDocument *));
        if (!tmp)
            return;
        server->pending_diag = tmp;
        server->pending_diag_capacity = new_cap;
    }
    server->pending_diag[server->pending_diag_count++] = doc;
    doc->diag_pending = true;
}

// Publish empty diagnostics for all open documents (used when diagnostics
// are disabled to clear stale squiggles on the client side).
void xlsp_clear_all_diagnostics(XrLspServer *server) {
    if (!server->doc_table)
        return;
    XrLspDocTable *table = server->doc_table;
    for (int i = 0; i < table->bucket_count; i++) {
        XrLspDocBucket *bucket = table->buckets[i];
        while (bucket) {
            XrLspDocument *doc = bucket->doc;
            if (doc) {
                XrJsonValue *params = xjson_new_object();
                xjson_object_set(params, "uri", xjson_new_string(doc->uri));
                xjson_object_set(params, "diagnostics", xjson_new_array());
                xlsp_send_notification(server, "textDocument/publishDiagnostics", params);
                xjson_free(params);
            }
            bucket = bucket->next;
        }
    }
}

// Check and publish diagnostics for documents in the pending queue.
// O(K) where K = pending count, instead of O(N) full table scan.
static void flush_pending_diagnostics(XrLspServer *server) {
    if (!server->config.diagnostics_enabled)
        return;
    if (server->pending_diag_count == 0)
        return;

    uint64_t now = get_monotonic_ms();
    int write = 0;

    for (int i = 0; i < server->pending_diag_count; i++) {
        XrLspDocument *doc = server->pending_diag[i];
        if (doc && doc->diagnostic_deadline > 0 && now >= doc->diagnostic_deadline) {
            doc->diagnostic_deadline = 0;
            doc->diag_pending = false;
            lsp_log("Diagnostic debounce: publishing for %s", doc->uri);
            xlsp_publish_diagnostics(server, doc);
        } else if (doc && doc->diagnostic_deadline > 0) {
            // Not yet ready, keep in queue
            server->pending_diag[write++] = doc;
        } else {
            // deadline == 0 means already published or cancelled, drop
            if (doc)
                doc->diag_pending = false;
        }
    }
    server->pending_diag_count = write;
}

// ============================================================================
// Method Dispatch Table (O(1) lookup instead of if-else chain)
// ============================================================================

typedef XrJsonValue *(*LspRequestHandler)(XrLspServer *, XrJsonValue *);
typedef void (*LspNotificationHandler)(XrLspServer *, XrJsonValue *);

typedef struct {
    const char *method;
    LspRequestHandler request_handler;
    LspNotificationHandler notification_handler;
} LspMethodEntry;

// Method dispatch table (sorted by frequency for cache locality)
static const LspMethodEntry lsp_methods[] = {
    // High-frequency methods first
    {"textDocument/completion", xlsp_handle_td_completion, NULL},
    {"completionItem/resolve", xlsp_handle_td_completion_resolve, NULL},
    {"textDocument/hover", xlsp_handle_td_hover, NULL},
    {"textDocument/didChange", NULL, xlsp_handle_td_did_change},
    {"textDocument/didOpen", NULL, xlsp_handle_td_did_open},
    {"textDocument/didClose", NULL, xlsp_handle_td_did_close},
    {"textDocument/definition", xlsp_handle_td_definition, NULL},
    {"textDocument/references", xlsp_handle_td_references, NULL},
    {"textDocument/documentSymbol", xlsp_handle_td_document_symbol, NULL},
    {"textDocument/signatureHelp", xlsp_handle_td_signature_help, NULL},
    {"textDocument/semanticTokens/full", xlsp_handle_td_semantic_tokens_full, NULL},
    {"textDocument/semanticTokens/full/delta", xlsp_handle_td_semantic_tokens_delta, NULL},
    {"textDocument/semanticTokens/range", xlsp_handle_td_semantic_tokens_range, NULL},
    // Medium-frequency
    {"textDocument/rename", xlsp_handle_td_rename, NULL},
    {"textDocument/prepareRename", xlsp_handle_td_prepare_rename, NULL},
    {"textDocument/formatting", xlsp_handle_td_formatting, NULL},
    {"textDocument/onTypeFormatting", xlsp_handle_td_on_type_formatting, NULL},
    {"textDocument/codeLens", xlsp_handle_td_code_lens, NULL},
    {"textDocument/inlayHint", xlsp_handle_td_inlay_hint, NULL},
    {"textDocument/foldingRange", xlsp_handle_folding_range, NULL},
    {"textDocument/codeAction", xlsp_handle_code_action, NULL},
    {"textDocument/documentHighlight", xlsp_handle_document_highlight, NULL},
    {"textDocument/selectionRange", xlsp_handle_selection_range, NULL},
    {"textDocument/documentLink", xlsp_handle_document_link, NULL},
    {"workspace/symbol", xlsp_handle_workspace_symbol, NULL},
    {"workspace/didChangeWatchedFiles", NULL, xlsp_handle_ws_did_change_watched_files},
    {"workspace/didChangeWorkspaceFolders", NULL, xlsp_handle_ws_did_change_workspace_folders},
    {"workspace/didChangeConfiguration", NULL, xlsp_handle_ws_did_change_configuration},
    // Low-frequency
    {"textDocument/prepareCallHierarchy", xlsp_handle_prepare_call_hierarchy, NULL},
    {"callHierarchy/incomingCalls", xlsp_handle_call_hierarchy_incoming, NULL},
    {"callHierarchy/outgoingCalls", xlsp_handle_call_hierarchy_outgoing, NULL},
    {"textDocument/prepareTypeHierarchy", xlsp_handle_prepare_type_hierarchy, NULL},
    {"typeHierarchy/supertypes", xlsp_handle_type_hierarchy_supertypes, NULL},
    {"typeHierarchy/subtypes", xlsp_handle_type_hierarchy_subtypes, NULL},
    {"textDocument/implementation", xlsp_handle_implementation, NULL},
    // Lifecycle (rare but special)
    {"initialize", xlsp_handle_lc_initialize, NULL},
    {"initialized", NULL, xlsp_handle_lc_initialized},
    {"shutdown", xlsp_handle_lc_shutdown, NULL},
    {"exit", NULL, xlsp_handle_lc_exit},
};

#define LSP_METHOD_COUNT (sizeof(lsp_methods) / sizeof(lsp_methods[0]))

// Hash table for O(1) method lookup (definition moved to xlsp_server.h)

struct MethodHashEntry {
    const LspMethodEntry *entry;
    struct MethodHashEntry *next;
};

static uint32_t method_hash(const char *s) {
    return xr_hash_bytes(s, strlen(s)) % METHOD_HASH_SIZE;
}

static void init_method_table(XrLspServer *server) {
    if (server->method_table_initialized)
        return;
    for (size_t i = 0; i < LSP_METHOD_COUNT; i++) {
        uint32_t h = method_hash(lsp_methods[i].method);
        MethodHashEntry *e = xr_malloc(sizeof(MethodHashEntry));
        if (!e) {
            // Under OOM we bail on partial initialisation — find_method will
            // return NULL for any method not yet linked, which handle_message
            // reports to the client as -32601 (MethodNotFound). Leaving
            // method_table_initialized = false lets a future successful call
            // retry once memory pressure clears.
            lsp_log("[fatal] init_method_table: xr_malloc failed at i=%zu\n", i);
            return;
        }
        e->entry = &lsp_methods[i];
        e->next = server->method_hash_table[h];
        server->method_hash_table[h] = e;
    }
    server->method_table_initialized = true;
}

static void free_method_table(XrLspServer *server) {
    for (int i = 0; i < METHOD_HASH_SIZE; i++) {
        MethodHashEntry *e = server->method_hash_table[i];
        while (e) {
            MethodHashEntry *next = e->next;
            xr_free(e);
            e = next;
        }
        server->method_hash_table[i] = NULL;
    }
    server->method_table_initialized = false;
}

static const LspMethodEntry *find_method(XrLspServer *server, const char *method) {
    uint32_t h = method_hash(method);
    for (MethodHashEntry *e = server->method_hash_table[h]; e; e = e->next) {
        if (strcmp(e->entry->method, method) == 0)
            return e->entry;
    }
    return NULL;
}

// Handle incoming message.
//
// Request vs notification: per JSON-RPC 2.0 a request has an `id` member
// that is a number or a string; a notification has no `id`. We honour
// both `id` shapes, echo them back verbatim in responses, and use them
// as the cancellation key.
static void handle_message(XrLspServer *server, XrJsonValue *msg) {
    init_method_table(server);

    const char *method = xjson_get_string(msg, "method");
    XrJsonValue *params = xjson_get(msg, "params");

    XlspRequestId id = parse_request_id(msg);
    bool is_request = (id.kind != XLSP_ID_NONE);

    if (!method) {
        if (is_request) {
            send_error(server, &id, -32600, "Invalid Request");
        }
        xlsp_request_id_free(&id);
        return;
    }

    // Handle $/cancelRequest specially (it's a notification)
    if (strcmp(method, "$/cancelRequest") == 0) {
        handle_cancel_request(server, params);
        xlsp_request_id_free(&id);
        return;
    }

    // LSP spec: once `shutdown` was received, only `shutdown` and
    // `exit` remain legal. Reject requests with -32002 and silently
    // drop notifications (including $/cancelRequest handled above so
    // clients can still cancel outstanding work during teardown).
    if (server->shutdown_received && strcmp(method, "shutdown") != 0 &&
        strcmp(method, "exit") != 0) {
        if (is_request) {
            send_error(server, &id, -32002, "Server is shutting down");
        }
        lsp_log("Dropping '%s' after shutdown", method);
        xlsp_request_id_free(&id);
        return;
    }

    // Handle window/workDoneProgress/cancel (cancellation from progress UI)
    if (strcmp(method, "window/workDoneProgress/cancel") == 0) {
        // Mark indexing as cancelled if token matches
        if (params) {
            const char *token = xjson_get_string(params, "token");
            if (token && server->index_progress_token &&
                strcmp(token, server->index_progress_token) == 0) {
                lsp_log("Workspace indexing cancelled by user");
                server->index_cancelled = true;
            }
        }
        xlsp_request_id_free(&id);
        return;
    }

    lsp_log("Received: %s (id=%s)", method, xlsp_request_id_debug(&id));

    // Track pending requests for cancellation. Ownership of the id's
    // string (if any) moves into the ring buffer on success; we keep
    // a cloned copy here so we can still identify the slot when the
    // call returns.
    XlspRequestId id_for_slot = {.kind = XLSP_ID_NONE, .as.number = 0};
    if (is_request) {
        id_for_slot = xlsp_request_id_clone(&id);
        pending_request_add(server, id_for_slot, method);
    }

    // O(1) method lookup
    const LspMethodEntry *entry = find_method(server, method);
    if (entry) {
        if (entry->request_handler) {
            // Check if already cancelled before executing
            if (pending_request_is_cancelled(server, &id)) {
                lsp_log("Request %s was cancelled before execution", xlsp_request_id_debug(&id));
                send_error(server, &id, LSP_ERROR_REQUEST_CANCELLED, "Request cancelled");
            } else {
                XrJsonValue *result = entry->request_handler(server, params);
                // Check if cancelled during execution
                if (pending_request_is_cancelled(server, &id)) {
                    lsp_log("Request %s was cancelled during execution",
                            xlsp_request_id_debug(&id));
                    send_error(server, &id, LSP_ERROR_REQUEST_CANCELLED, "Request cancelled");
                    if (result)
                        xjson_free(result);
                } else {
                    send_response(server, &id, result);
                }
            }
            pending_request_remove(server, &id);
        } else if (entry->notification_handler) {
            entry->notification_handler(server, params);
        }
    } else {
        if (is_request) {
            send_error(server, &id, -32601, "Method not found");
            pending_request_remove(server, &id);
        }
        lsp_log("Unhandled method: %s", method);
    }

    xlsp_request_id_free(&id);
}

// Context identifiers for poll events
typedef struct {
    int type;  // 0=stdin, 1=async, 2=index_pool
} XlspPollCtx;

static XlspPollCtx g_stdin_ctx = {0};
static XlspPollCtx g_async_ctx = {1};
static XlspPollCtx g_index_ctx = {2};

int xlsp_server_run(XrLspServer *server) {
    // Set thread-local server for logging
    xlsp_set_log_server(server);

    lsp_log("xray Language Server starting...");

    // Initialize poll subsystem. xr_poll has backends for kqueue, epoll,
    // IOCP and select, so failure here means OOM or a kernel subsystem
    // catastrophe — there is no useful blocking fallback (the main loop
    // also has to wake on async task completions, index pool results
    // and diagnostic debounce timers, all of which are poll-driven).
    XrPoll poll;
    if (xr_poll_init(&poll) < 0) {
        lsp_log("FATAL: xr_poll_init failed (OOM?); server cannot start");
        return 1;
    }

    // Register stdin for reading
    int stdin_fd = xlsp_transport_get_fd(server->transport);
    if (stdin_fd >= 0) {
        if (xr_poll_add(&poll, stdin_fd, XR_POLL_IN, &g_stdin_ctx) < 0) {
            lsp_log("Failed to add stdin to poll");
        }
    }

    // Register async task notification fd
    if (server->async) {
        int async_fd = xlsp_async_get_notify_fd(server->async);
        if (async_fd >= 0) {
            if (xr_poll_add(&poll, async_fd, XR_POLL_IN, &g_async_ctx) < 0) {
                lsp_log("Failed to add async notify fd to poll");
            }
        }
    }

    // Register index pool notification fd
    if (server->index_pool) {
        int index_fd = xlsp_index_pool_get_notify_fd(server->index_pool);
        if (index_fd >= 0) {
            if (xr_poll_add(&poll, index_fd, XR_POLL_IN, &g_index_ctx) < 0) {
                lsp_log("Failed to add index pool notify fd to poll");
            }
        }
    }

    lsp_log("Event loop initialized with poll");

    // Event-driven main loop — runs until `exit` notification arrives.
    // Receiving `shutdown` alone does NOT stop the loop: the spec
    // requires us to stay up and reject subsequent requests.
    while (!server->exit_received) {
        XrPollEvent events[8];
        int n = xr_poll_wait(&poll, events, 8, 100);  // 100ms timeout

        if (n < 0) {
            lsp_log("Poll error, continuing...");
            continue;
        }

        bool stdin_ready = false;
        bool async_ready = false;
        bool index_ready = false;

        // Determine which sources are ready
        for (int i = 0; i < n; i++) {
            XlspPollCtx *ctx = (XlspPollCtx *) events[i].user_data;
            if (!ctx)
                continue;

            switch (ctx->type) {
                case 0:
                    stdin_ready = true;
                    break;
                case 1:
                    async_ready = true;
                    break;
                case 2:
                    index_ready = true;
                    break;
            }
        }

        // Process async task completions
        if (async_ready && server->async) {
            int completed = xlsp_async_poll(server->async);
            if (completed > 0) {
                lsp_log("Processed %d background tasks", completed);
            }
        }

        // Process parallel indexing results
        if (index_ready) {
            xlsp_workspace_poll_index_results(server);
        }

        // Process stdin messages
        if (stdin_ready) {
            size_t len;
            bool would_block;
            char *msg_str = xlsp_transport_try_read(server->transport, &len, &would_block);

            if (msg_str) {
                XrJsonValue *msg = xjson_parse(msg_str, len);
                xr_free(msg_str);

                if (msg) {
                    handle_message(server, msg);
                    xjson_free(msg);
                } else {
                    lsp_log("Failed to parse JSON message");
                }
            } else if (!would_block) {
                // EOF or error
                lsp_log("EOF or read error, exiting");
                break;
            }
        }

        // Also process any pending background work even without events
        // (timeout case: n == 0)
        if (n == 0) {
            // Periodic maintenance: process any accumulated results
            if (server->async) {
                xlsp_async_poll(server->async);
            }
            xlsp_workspace_poll_index_results(server);
        }

        // Drain pending background analysis (budgeted: ≤5ms per tick)
        xlsp_drain_pending_analysis(server);

        // Check debounced diagnostic deadlines (every loop iteration)
        flush_pending_diagnostics(server);
    }

    xr_poll_destroy(&poll);
    lsp_log("Server shutting down");
    // Spec: exit with 0 iff `shutdown` arrived before `exit`, else 1.
    return server->shutdown_received ? 0 : 1;
}

// Handler implementations split into separate files.
// Dispatch table references updated in xlsp_handlers_lifecycle.h,
// xlsp_handlers_workspace.h, xlsp_handlers_textdoc.h.
