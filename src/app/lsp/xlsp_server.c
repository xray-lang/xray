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
#include "xlsp_json.h"
#include "xlsp_cache.h"
#include "../../frontend/analyzer/xanalyzer.h"
#include "xlsp_semantic_tokens.h"
#include "xlsp_inlay_hints.h"
#include "xlsp_imports.h"
#include "xlsp_folding.h"
#include "xlsp_code_action.h"
#include "xlsp_call_hierarchy.h"
#include "xlsp_extra_handlers.h"
#include "xlsp_utils.h"
#include "../../frontend/parser/xast_nodes.h"
#include "../../base/xhash.h"
#include "../../runtime/object/xutf8.h"
#include "../../base/xpoll.h"  // Cross-platform poll abstraction
#include "xray_version.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>
#include "../../base/xmalloc.h"
#include "../../base/xchecks.h"

// Fallback diagnostic debounce interval (ms). The live value is
// server->config.diagnostic_debounce_ms, which is populated from
// xray.toml / workspace/didChangeConfiguration. This constant is used
// only before the server has finished initialising.
#define DIAGNOSTIC_DEBOUNCE_MS 300

// Get monotonic time in milliseconds
static uint64_t get_monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// Config functions extracted to xlsp_config.c

// Forward declarations for handlers
static XrJsonValue *handle_initialize(XrLspServer *server, XrJsonValue *params);
static XrJsonValue *handle_shutdown(XrLspServer *server, XrJsonValue *params);
static void handle_initialized(XrLspServer *server, XrJsonValue *params);
static void handle_exit(XrLspServer *server, XrJsonValue *params);
static void handle_did_open(XrLspServer *server, XrJsonValue *params);
static void handle_did_change(XrLspServer *server, XrJsonValue *params);
static void handle_did_close(XrLspServer *server, XrJsonValue *params);
static XrJsonValue *handle_completion(XrLspServer *server, XrJsonValue *params);
static XrJsonValue *handle_hover(XrLspServer *server, XrJsonValue *params);
static XrJsonValue *handle_document_symbol(XrLspServer *server, XrJsonValue *params);
static XrJsonValue *handle_definition(XrLspServer *server, XrJsonValue *params);
static XrJsonValue *handle_references(XrLspServer *server, XrJsonValue *params);
static XrJsonValue *handle_rename(XrLspServer *server, XrJsonValue *params);
static XrJsonValue *handle_prepare_rename(XrLspServer *server, XrJsonValue *params);
static XrJsonValue *handle_formatting(XrLspServer *server, XrJsonValue *params);
static XrJsonValue *handle_range_formatting(XrLspServer *server, XrJsonValue *params);
static XrJsonValue *handle_on_type_formatting(XrLspServer *server, XrJsonValue *params);
static XrJsonValue *handle_code_lens(XrLspServer *server, XrJsonValue *params);
static XrJsonValue *handle_signature_help(XrLspServer *server, XrJsonValue *params);
static XrJsonValue *handle_semantic_tokens_full(XrLspServer *server, XrJsonValue *params);
static XrJsonValue *handle_semantic_tokens_delta(XrLspServer *server, XrJsonValue *params);
static XrJsonValue *handle_semantic_tokens_range(XrLspServer *server, XrJsonValue *params);
static XrJsonValue *handle_inlay_hint(XrLspServer *server, XrJsonValue *params);
// Extracted handlers: see xlsp_folding.h, xlsp_code_action.h,
// xlsp_call_hierarchy.h, xlsp_extra_handlers.h
static XrJsonValue *handle_completion_resolve(XrLspServer *server, XrJsonValue *params);
static void handle_did_change_watched_files(XrLspServer *server, XrJsonValue *params);
static void handle_did_change_workspace_folders(XrLspServer *server, XrJsonValue *params);
static void handle_did_change_configuration(XrLspServer *server, XrJsonValue *params);
static void free_method_table(XrLspServer *server);

// Thread-local server pointer for logging (set before each operation)
static _Thread_local XrLspServer *tls_server = NULL;

void xlsp_set_log_server(XrLspServer *server) {
    tls_server = server;
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
#define DOC_TABLE_LOAD_FACTOR_NUM 3    // Numerator of load factor (75% = 3/4)
#define DOC_TABLE_LOAD_FACTOR_DEN 4    // Denominator of load factor
#define DOC_TABLE_MAX_SIZE (1 << 16)   // Max 65536 buckets

static uint32_t hash_uri(const char *uri) {
    return xr_hash_bytes(uri, strlen(uri));
}

// Forward declaration for resize
static void doc_table_resize(XrLspDocTable *table, int new_size);

static XrLspDocTable *doc_table_new(void) {
    XrLspDocTable *table = xr_calloc(1, sizeof(XrLspDocTable));
    if (!table) return NULL;
    table->bucket_count = DOC_TABLE_INITIAL_SIZE;
    table->buckets = xr_calloc(table->bucket_count, sizeof(XrLspDocBucket*));
    if (!table->buckets) {
        xr_free(table);
        return NULL;
    }
    return table;
}

static void doc_table_free(XrLspDocTable *table) {
    if (!table) return;
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
    if (!table || !uri) return NULL;
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
    if (!table || new_size <= table->bucket_count) return;
    if (new_size > DOC_TABLE_MAX_SIZE) new_size = DOC_TABLE_MAX_SIZE;

    // Allocate new bucket array
    XrLspDocBucket **new_buckets = xr_calloc(new_size, sizeof(XrLspDocBucket*));
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
    if (!table || !doc || !doc->uri) return;

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
    if (!table || !uri) return;
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
    if (!server) return NULL;

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
        if (server->isolate) xray_isolate_delete(server->isolate);
        xr_free(server);
        return NULL;
    }

    // Create workspace-level analyzer (unified index for all cross-file features)
    server->workspace_analyzer = xa_analyzer_new(server->isolate);
    if (!server->workspace_analyzer) {
        lsp_log("Warning: Failed to create workspace analyzer");
    }

    // Create background task system
    server->async = xlsp_async_new();
    if (!server->async) {
        lsp_log("Warning: Failed to create async worker, background tasks disabled");
    } else {
        // Ensure the worker thread shares the same log file / stderr routing
        // as the main loop — otherwise every lsp_log() call from a task would
        // see tls_server == NULL and go to stderr only, bypassing the log
        // file entirely (see the comment in XrLspAsync::thread_init).
        server->async->thread_init = (void (*)(void *))xlsp_set_log_server;
        server->async->thread_init_ctx = server;
    }

    // Default configuration
    server->config.diagnostic_debounce_ms = 300;
    server->config.diagnostics_enabled = true;
    server->config.completion_max_items = 100;
    server->config.format_tab_size = 4;
    server->config.format_insert_spaces = true;
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
    if (!server) return;

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
        if (doc->content[i] == '\n') line_count++;
    }

    doc->line_offsets = xr_malloc(line_count * sizeof(uint32_t));
    if (!doc->line_offsets) return false;

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

XrLspDocument *xlsp_document_open(XrLspServer *server, const char *uri,
                                   const char *text, int version) {
    XrLspDocument *doc = xr_calloc(1, sizeof(XrLspDocument));
    if (!doc) return NULL;

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

void xlsp_document_change(XrLspDocument *doc, XrLspRange *range,
                          const char *text) {
    if (!doc || !text) return;

    if (!range) {
        // Full document sync
        char *new_content = xr_strdup(text);
        if (!new_content) return;  // Keep old content on failure

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
        if (!new_content) return;  // Keep old content on failure

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
    if (doc) return doc;

    const char *path = xlsp_uri_to_path(uri);

    // Try to load from disk
    FILE *f = fopen(path, "r");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size < 0) {
        fclose(f);
        return NULL;
    }
    if ((size_t)size >= XLSP_MAX_DOCUMENT_BYTES) {
        lsp_log("Refusing to load %s: %ld bytes exceeds %u-byte limit",
                path, size, XLSP_MAX_DOCUMENT_BYTES);
        fclose(f);
        return NULL;
    }

    char *content = xr_malloc((size_t)size + 1);
    if (!content) {
        fclose(f);
        return NULL;
    }

    size_t read_size = fread(content, 1, (size_t)size, f);
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
    if (!doc) return;
    xlsp_invalidate_import_cache(doc);
    xlsp_free_document_cache(doc);
    xr_free(doc->uri);
    xr_free(doc->content);
    xr_free(doc->line_offsets);
    xr_arena_destroy(&doc->arena);
    xr_free(doc);
}

uint32_t xlsp_position_to_offset(XrLspDocument *doc, XrLspPosition pos) {
    if (!doc || !doc->line_offsets) return 0;
    if ((int)pos.line >= doc->line_count) {
        return doc->length;
    }

    uint32_t line_start = doc->line_offsets[pos.line];

    // Calculate line length (bytes until next line or end of document)
    uint32_t line_end;
    if ((int)pos.line + 1 < doc->line_count) {
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
    size_t byte_offset_in_line = xr_utf8_utf16_to_byte_offset(
        doc->content + line_start, line_len, pos.character);

    uint32_t offset = line_start + (uint32_t)byte_offset_in_line;
    if (offset > doc->length) offset = doc->length;
    return offset;
}

XrLspPosition xlsp_offset_to_position(XrLspDocument *doc, uint32_t offset) {
    XrLspPosition pos = {0, 0};
    if (!doc || !doc->line_offsets) return pos;

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
    pos.character = (uint32_t)xr_utf8_byte_to_utf16_offset(
        doc->content + line_start, line_len, byte_offset_in_line);

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
    XlspRequestId id = { .kind = XLSP_ID_NONE, .as.number = 0 };
    XrJsonValue *v = xlsp_json_get(msg, "id");
    if (!v) return id;
    if (v->type == XR_JSON_NUMBER) {
        id.kind = XLSP_ID_NUMBER;
        id.as.number = v->as.number;
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
    if (!id) return;
    if (id->kind == XLSP_ID_STRING) {
        xr_free(id->as.string);
        id->as.string = NULL;
    }
    id->kind = XLSP_ID_NONE;
}

static XlspRequestId xlsp_request_id_clone(const XlspRequestId *src) {
    XlspRequestId dst = { .kind = XLSP_ID_NONE, .as.number = 0 };
    if (!src) return dst;
    dst.kind = src->kind;
    if (src->kind == XLSP_ID_NUMBER) {
        dst.as.number = src->as.number;
    } else if (src->kind == XLSP_ID_STRING) {
        dst.as.string = xr_strdup(src->as.string ? src->as.string : "");
        if (!dst.as.string) dst.kind = XLSP_ID_NONE;
    }
    return dst;
}

static bool xlsp_request_id_equals(const XlspRequestId *a, const XlspRequestId *b) {
    if (!a || !b) return false;
    if (a->kind != b->kind) return false;
    switch (a->kind) {
        case XLSP_ID_NUMBER: return a->as.number == b->as.number;
        case XLSP_ID_STRING:
            return a->as.string && b->as.string &&
                   strcmp(a->as.string, b->as.string) == 0;
        case XLSP_ID_NONE:   return true;  // both notifications (rare)
    }
    return false;
}

// Build the JSON id value for a response / request. Returns a JSON
// value (null for XLSP_ID_NONE — used only for error responses that
// have to be emitted before we could parse the id).
static XrJsonValue *xlsp_request_id_to_json(const XlspRequestId *id) {
    if (!id || id->kind == XLSP_ID_NONE) return xlsp_json_new_null();
    if (id->kind == XLSP_ID_NUMBER) return xlsp_json_new_number(id->as.number);
    return xlsp_json_new_string(id->as.string ? id->as.string : "");
}

// Short debug label, used for log messages only. Returns a pointer into
// a static buffer (single-threaded caller assumed: lsp_log is only ever
// called from the main loop or a thread that already holds tls_server).
static const char *xlsp_request_id_debug(const XlspRequestId *id) {
    static _Thread_local char buf[64];
    if (!id || id->kind == XLSP_ID_NONE) return "<notif>";
    if (id->kind == XLSP_ID_NUMBER) {
        snprintf(buf, sizeof(buf), "%.0f", id->as.number);
    } else {
        snprintf(buf, sizeof(buf), "\"%s\"",
                 id->as.string ? id->as.string : "");
    }
    return buf;
}

// ---------------------------------------------------------------------------
// Response / error emitters
// ---------------------------------------------------------------------------

// Send a JSON-RPC response echoing back the client's id exactly.
static void send_response(XrLspServer *server, const XlspRequestId *id,
                          XrJsonValue *result) {
    XrJsonValue *resp = xlsp_json_new_object();
    xlsp_json_object_set(resp, "jsonrpc", xlsp_json_new_string("2.0"));
    xlsp_json_object_set(resp, "id", xlsp_request_id_to_json(id));
    xlsp_json_object_set(resp, "result", result ? result : xlsp_json_new_null());

    size_t len;
    char *json = xlsp_json_stringify(resp, &len);
    xlsp_transport_write(server->transport, json, len);

    xr_free(json);
    xlsp_json_free(resp);
}

// Send a JSON-RPC error response, mirroring the client's id shape.
static void send_error(XrLspServer *server, const XlspRequestId *id,
                       int code, const char *message) {
    XrJsonValue *resp = xlsp_json_new_object();
    xlsp_json_object_set(resp, "jsonrpc", xlsp_json_new_string("2.0"));
    xlsp_json_object_set(resp, "id", xlsp_request_id_to_json(id));

    XrJsonValue *error = xlsp_json_new_object();
    xlsp_json_object_set(error, "code", xlsp_json_new_number(code));
    xlsp_json_object_set(error, "message", xlsp_json_new_string(message));
    xlsp_json_object_set(resp, "error", error);

    size_t len;
    char *json = xlsp_json_stringify(resp, &len);
    xlsp_transport_write(server->transport, json, len);

    xr_free(json);
    xlsp_json_free(resp);
}

// Send a notification
static void send_notification(XrLspServer *server, const char *method, XrJsonValue *params) {
    XrJsonValue *notif = xlsp_json_new_object();
    xlsp_json_object_set(notif, "jsonrpc", xlsp_json_new_string("2.0"));
    xlsp_json_object_set(notif, "method", xlsp_json_new_string(method));
    if (params) {
        xlsp_json_object_set(notif, "params", params);
    }

    size_t len;
    char *json = xlsp_json_stringify(notif, &len);
    xlsp_transport_write(server->transport, json, len);

    xr_free(json);
    xlsp_json_free(notif);
}

// Send a server-initiated request (fire-and-forget, response ignored)
static void send_request(XrLspServer *server, const char *method, XrJsonValue *params) {
    XrJsonValue *req = xlsp_json_new_object();
    xlsp_json_object_set(req, "jsonrpc", xlsp_json_new_string("2.0"));
    xlsp_json_object_set(req, "id", xlsp_json_new_number(++server->next_request_id));
    xlsp_json_object_set(req, "method", xlsp_json_new_string(method));
    if (params) {
        xlsp_json_object_set(req, "params", params);
    }

    size_t len;
    char *json = xlsp_json_stringify(req, &len);
    xlsp_transport_write(server->transport, json, len);

    xr_free(json);
    xlsp_json_free(req);
}

// ============================================================================
// Request Cancellation Support ($/cancelRequest)
// ============================================================================

// Add a pending request to track (ring buffer: O(1) insert).
// Takes ownership of *id (moves it into the slot). `method` is a static
// string from the dispatch table and is stored by reference.
static void pending_request_add(XrLspServer *server, XlspRequestId id,
                                const char *method) {
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
            lsp_log("Request %s (%s) marked as cancelled",
                    xlsp_request_id_debug(id),
                    pending->requests[i].method ? pending->requests[i].method : "unknown");
            found = true;
            break;
        }
    }

    if (server->async && id && id->kind == XLSP_ID_NUMBER) {
        int async_cancelled = xlsp_async_cancel_request(server->async,
                                                        (int64_t)id->as.number);
        if (async_cancelled > 0) {
            lsp_log("Cancelled %d background tasks for request %s",
                    async_cancelled, xlsp_request_id_debug(id));
        }
    }

    if (!found) {
        lsp_log("Request %s not found for cancellation",
                xlsp_request_id_debug(id));
    }
    return found;
}

// Handle $/cancelRequest notification
static void handle_cancel_request(XrLspServer *server, XrJsonValue *params) {
    if (!params) return;

    // params itself carries the id to cancel, same number-or-string
    // shape as an original request's id.
    XlspRequestId target = parse_request_id(params);
    if (target.kind == XLSP_ID_NONE) return;

    pending_request_cancel(server, &target);
    xlsp_request_id_free(&target);
}

// LSP error codes
#define LSP_ERROR_REQUEST_CANCELLED -32800

// Publish diagnostics for a document
static void publish_diagnostics(XrLspServer *server, XrLspDocument *doc) {
    XrJsonValue *diagnostics = xlsp_analyze_diagnostics(doc);

    XrJsonValue *params = xlsp_json_new_object();
    xlsp_json_object_set(params, "uri", xlsp_json_new_string(doc->uri));
    xlsp_json_object_set(params, "version", xlsp_json_new_number(doc->version));
    xlsp_json_object_set(params, "diagnostics", diagnostics);

    send_notification(server, "textDocument/publishDiagnostics", params);
}

// ============================================================================
// Progress Reporting
// ============================================================================

// Begin progress reporting (with optional cancellation support).
// The token counter lives on the server (see XrLspServer::progress_token_counter)
// so that multiple concurrent XrLspServer instances do not collide on token names.
static char *progress_begin_ex(XrLspServer *server, const char *title,
                               const char *message, bool cancellable) {
    XR_DCHECK(server != NULL, "progress_begin_ex: NULL server");
    char token[32];
    snprintf(token, sizeof(token), "xlsp-progress-%d", ++server->progress_token_counter);

    // Create progress token
    XrJsonValue *create_params = xlsp_json_new_object();
    xlsp_json_object_set(create_params, "token", xlsp_json_new_string(token));
    send_request(server, "window/workDoneProgress/create", create_params);

    // Send begin notification
    XrJsonValue *params = xlsp_json_new_object();
    xlsp_json_object_set(params, "token", xlsp_json_new_string(token));

    XrJsonValue *value = xlsp_json_new_object();
    xlsp_json_object_set(value, "kind", xlsp_json_new_string("begin"));
    xlsp_json_object_set(value, "title", xlsp_json_new_string(title));
    if (message) {
        xlsp_json_object_set(value, "message", xlsp_json_new_string(message));
    }
    xlsp_json_object_set(value, "cancellable", xlsp_json_new_bool(cancellable));
    xlsp_json_object_set(value, "percentage", xlsp_json_new_number(0));
    xlsp_json_object_set(params, "value", value);

    send_notification(server, "$/progress", params);

    return xr_strdup(token);
}

// Begin progress reporting (public API, with cancellation support)
char *xlsp_progress_begin(XrLspServer *server, const char *title,
                          const char *message, bool cancellable) {
    return progress_begin_ex(server, title, message, cancellable);
}

// Report progress (public API)
void xlsp_progress_report(XrLspServer *server, const char *token,
                          const char *message, int percentage) {
    if (!server || !token) return;

    XrJsonValue *params = xlsp_json_new_object();
    xlsp_json_object_set(params, "token", xlsp_json_new_string(token));

    XrJsonValue *value = xlsp_json_new_object();
    xlsp_json_object_set(value, "kind", xlsp_json_new_string("report"));
    if (message) {
        xlsp_json_object_set(value, "message", xlsp_json_new_string(message));
    }
    if (percentage >= 0) {
        xlsp_json_object_set(value, "percentage", xlsp_json_new_number(percentage));
    }
    xlsp_json_object_set(params, "value", value);

    send_notification(server, "$/progress", params);
}

// End progress reporting (public API)
void xlsp_progress_end(XrLspServer *server, const char *token, const char *message) {
    if (!server || !token) return;

    XrJsonValue *params = xlsp_json_new_object();
    xlsp_json_object_set(params, "token", xlsp_json_new_string(token));

    XrJsonValue *value = xlsp_json_new_object();
    xlsp_json_object_set(value, "kind", xlsp_json_new_string("end"));
    if (message) {
        xlsp_json_object_set(value, "message", xlsp_json_new_string(message));
    }
    xlsp_json_object_set(params, "value", value);

    send_notification(server, "$/progress", params);
}

// ============================================================================
// Diagnostic Debounce
// ============================================================================

// Schedule debounced diagnostics for a document.
// Uses timer-based debounce in the main event loop instead of blocking
// a background thread with usleep.
static void schedule_diagnostics(XrLspServer *server, XrLspDocument *doc) {
    if (!server->config.diagnostics_enabled) return;

    uint64_t now = get_monotonic_ms();
    doc->last_change_time = now;
    int debounce_ms = server->config.diagnostic_debounce_ms > 0
                    ? server->config.diagnostic_debounce_ms
                    : DIAGNOSTIC_DEBOUNCE_MS;
    doc->diagnostic_deadline = now + (uint64_t)debounce_ms;

    // O(1) dedup via per-document flag
    if (doc->diag_pending) return;

    // Grow queue if needed
    if (server->pending_diag_count >= server->pending_diag_capacity) {
        int new_cap = server->pending_diag_capacity ? server->pending_diag_capacity * 2 : 16;
        XrLspDocument **tmp = xr_realloc(server->pending_diag,
                                         (size_t)new_cap * sizeof(XrLspDocument *));
        if (!tmp) return;
        server->pending_diag = tmp;
        server->pending_diag_capacity = new_cap;
    }
    server->pending_diag[server->pending_diag_count++] = doc;
    doc->diag_pending = true;
}

// Publish empty diagnostics for all open documents (used when diagnostics
// are disabled to clear stale squiggles on the client side).
static void clear_all_diagnostics(XrLspServer *server) {
    if (!server->doc_table) return;
    XrLspDocTable *table = server->doc_table;
    for (int i = 0; i < table->bucket_count; i++) {
        XrLspDocBucket *bucket = table->buckets[i];
        while (bucket) {
            XrLspDocument *doc = bucket->doc;
            if (doc) {
                XrJsonValue *params = xlsp_json_new_object();
                xlsp_json_object_set(params, "uri", xlsp_json_new_string(doc->uri));
                xlsp_json_object_set(params, "diagnostics", xlsp_json_new_array());
                send_notification(server, "textDocument/publishDiagnostics", params);
                xlsp_json_free(params);
            }
            bucket = bucket->next;
        }
    }
}

// Check and publish diagnostics for documents in the pending queue.
// O(K) where K = pending count, instead of O(N) full table scan.
static void flush_pending_diagnostics(XrLspServer *server) {
    if (!server->config.diagnostics_enabled) return;
    if (server->pending_diag_count == 0) return;

    uint64_t now = get_monotonic_ms();
    int write = 0;

    for (int i = 0; i < server->pending_diag_count; i++) {
        XrLspDocument *doc = server->pending_diag[i];
        if (doc && doc->diagnostic_deadline > 0 && now >= doc->diagnostic_deadline) {
            doc->diagnostic_deadline = 0;
            doc->diag_pending = false;
            lsp_log("Diagnostic debounce: publishing for %s", doc->uri);
            publish_diagnostics(server, doc);
        } else if (doc && doc->diagnostic_deadline > 0) {
            // Not yet ready, keep in queue
            server->pending_diag[write++] = doc;
        } else {
            // deadline == 0 means already published or cancelled, drop
            if (doc) doc->diag_pending = false;
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
    {"textDocument/completion", handle_completion, NULL},
    {"completionItem/resolve", handle_completion_resolve, NULL},
    {"textDocument/hover", handle_hover, NULL},
    {"textDocument/didChange", NULL, handle_did_change},
    {"textDocument/didOpen", NULL, handle_did_open},
    {"textDocument/didClose", NULL, handle_did_close},
    {"textDocument/definition", handle_definition, NULL},
    {"textDocument/references", handle_references, NULL},
    {"textDocument/documentSymbol", handle_document_symbol, NULL},
    {"textDocument/signatureHelp", handle_signature_help, NULL},
    {"textDocument/semanticTokens/full", handle_semantic_tokens_full, NULL},
    {"textDocument/semanticTokens/full/delta", handle_semantic_tokens_delta, NULL},
    {"textDocument/semanticTokens/range", handle_semantic_tokens_range, NULL},
    // Medium-frequency
    {"textDocument/rename", handle_rename, NULL},
    {"textDocument/prepareRename", handle_prepare_rename, NULL},
    {"textDocument/formatting", handle_formatting, NULL},
    {"textDocument/rangeFormatting", handle_range_formatting, NULL},
    {"textDocument/onTypeFormatting", handle_on_type_formatting, NULL},
    {"textDocument/codeLens", handle_code_lens, NULL},
    {"textDocument/inlayHint", handle_inlay_hint, NULL},
    {"textDocument/foldingRange", xlsp_handle_folding_range, NULL},
    {"textDocument/codeAction", xlsp_handle_code_action, NULL},
    {"textDocument/documentHighlight", xlsp_handle_document_highlight, NULL},
    {"textDocument/selectionRange", xlsp_handle_selection_range, NULL},
    {"textDocument/documentLink", xlsp_handle_document_link, NULL},
    {"workspace/symbol", xlsp_handle_workspace_symbol, NULL},
    {"workspace/didChangeWatchedFiles", NULL, handle_did_change_watched_files},
    {"workspace/didChangeWorkspaceFolders", NULL, handle_did_change_workspace_folders},
    {"workspace/didChangeConfiguration", NULL, handle_did_change_configuration},
    // Low-frequency
    {"textDocument/prepareCallHierarchy", xlsp_handle_prepare_call_hierarchy, NULL},
    {"callHierarchy/incomingCalls", xlsp_handle_call_hierarchy_incoming, NULL},
    {"callHierarchy/outgoingCalls", xlsp_handle_call_hierarchy_outgoing, NULL},
    {"textDocument/prepareTypeHierarchy", xlsp_handle_prepare_type_hierarchy, NULL},
    {"typeHierarchy/supertypes", xlsp_handle_type_hierarchy_supertypes, NULL},
    {"typeHierarchy/subtypes", xlsp_handle_type_hierarchy_subtypes, NULL},
    {"textDocument/implementation", xlsp_handle_implementation, NULL},
    // Lifecycle (rare but special)
    {"initialize", handle_initialize, NULL},
    {"initialized", NULL, handle_initialized},
    {"shutdown", handle_shutdown, NULL},
    {"exit", NULL, handle_exit},
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
    if (server->method_table_initialized) return;
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
        if (strcmp(e->entry->method, method) == 0) return e->entry;
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

    const char *method = xlsp_json_get_string(msg, "method");
    XrJsonValue *params = xlsp_json_get(msg, "params");

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
    if (server->shutdown_received &&
        strcmp(method, "shutdown") != 0 &&
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
            const char *token = xlsp_json_get_string(params, "token");
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
    XlspRequestId id_for_slot = { .kind = XLSP_ID_NONE, .as.number = 0 };
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
                lsp_log("Request %s was cancelled before execution",
                        xlsp_request_id_debug(&id));
                send_error(server, &id, LSP_ERROR_REQUEST_CANCELLED,
                           "Request cancelled");
            } else {
                XrJsonValue *result = entry->request_handler(server, params);
                // Check if cancelled during execution
                if (pending_request_is_cancelled(server, &id)) {
                    lsp_log("Request %s was cancelled during execution",
                            xlsp_request_id_debug(&id));
                    send_error(server, &id, LSP_ERROR_REQUEST_CANCELLED,
                               "Request cancelled");
                    if (result) xlsp_json_free(result);
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
            XlspPollCtx *ctx = (XlspPollCtx *)events[i].user_data;
            if (!ctx) continue;

            switch (ctx->type) {
                case 0: stdin_ready = true; break;
                case 1: async_ready = true; break;
                case 2: index_ready = true; break;
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
                XrJsonValue *msg = xlsp_json_parse(msg_str, len);
                xr_free(msg_str);

                if (msg) {
                    handle_message(server, msg);
                    xlsp_json_free(msg);
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

// ============== Handler Implementations ==============

static XrJsonValue *handle_initialize(XrLspServer *server, XrJsonValue *params) {
    // Collect workspace folders from initialize params.
    // If the client provides workspaceFolders, use them directly.
    // Otherwise, fall back to rootUri / rootPath and create a
    // synthetic workspace folder so the rest of the code has a
    // single model to work with.
    XrJsonValue *folders = xlsp_json_get_array(params, "workspaceFolders");
    if (folders) {
        int count = xlsp_json_array_len(folders);
        for (int i = 0; i < count && i < MAX_WORKSPACE_FOLDERS; i++) {
            XrJsonValue *folder = xlsp_json_array_get(folders, i);
            const char *uri = xlsp_json_get_string(folder, "uri");
            const char *name = xlsp_json_get_string(folder, "name");
            if (uri) {
                int idx = server->workspace_folder_count++;
                server->workspace_folders[idx].uri = xr_strdup(uri);
                server->workspace_folders[idx].name = name ? xr_strdup(name) : xr_strdup("");
                server->workspace_folders[idx].path = xr_strdup(xlsp_uri_to_path(uri));
            }
        }
        lsp_log("Initialized with %d workspace folders", server->workspace_folder_count);
    }

    // Fold rootUri / rootPath into workspace_folders if no folders were
    // provided (single-root fallback).
    if (server->workspace_folder_count == 0) {
        const char *root_uri = xlsp_json_get_string(params, "rootUri");
        const char *root_path = xlsp_json_get_string(params, "rootPath");
        if (root_uri || root_path) {
            int idx = server->workspace_folder_count++;
            server->workspace_folders[idx].uri = root_uri ? xr_strdup(root_uri) : NULL;
            server->workspace_folders[idx].name = xr_strdup("root");
            if (root_path) {
                server->workspace_folders[idx].path = xr_strdup(root_path);
            } else if (root_uri) {
                server->workspace_folders[idx].path = xr_strdup(xlsp_uri_to_path(root_uri));
            }
            lsp_log("Folded rootUri/rootPath into workspace folder[0]: %s",
                     server->workspace_folders[0].path ? server->workspace_folders[0].path : "(null)");
        }
    }

    // Load project-specific config from xray.toml (first folder wins)
    for (int i = 0; i < server->workspace_folder_count; i++) {
        if (server->workspace_folders[i].path) {
            if (xlsp_config_load_from_toml(&server->config,
                                           server->workspace_folders[i].path)) {
                server->workspace_folders[i].config_loaded = true;
                lsp_log("Loaded LSP config from xray.toml in: %s",
                        server->workspace_folders[i].path);
                break;
            }
        }
    }

    const char *display_root = server->workspace_folder_count > 0
        ? server->workspace_folders[0].path : "(none)";
    lsp_log("Initializing with root: %s", display_root);

    // Build capabilities response
    XrJsonValue *result = xlsp_json_new_object();
    XrJsonValue *capabilities = xlsp_json_new_object();

    // Text document sync (incremental)
    XrJsonValue *textDocSync = xlsp_json_new_object();
    xlsp_json_object_set_new(textDocSync, "openClose", xlsp_json_new_bool(true));
    xlsp_json_object_set_new(textDocSync, "change", xlsp_json_new_number(LSP_SYNC_INCREMENTAL));
    xlsp_json_object_set_new(capabilities, "textDocumentSync", textDocSync);

    // Completion
    if (server->capabilities.completion) {
        XrJsonValue *completion = xlsp_json_new_object();
        XrJsonValue *triggerChars = xlsp_json_new_array();
        xlsp_json_array_push(triggerChars, xlsp_json_new_string("."));
        xlsp_json_object_set_new(completion, "triggerCharacters", triggerChars);
        xlsp_json_object_set_new(completion, "resolveProvider", xlsp_json_new_bool(true));
        xlsp_json_object_set_new(capabilities, "completionProvider", completion);
        lsp_log("completionProvider enabled with trigger '.'");
    } else {
        lsp_log("completionProvider DISABLED");
    }

    // Hover
    if (server->capabilities.hover) {
        xlsp_json_object_set_new(capabilities, "hoverProvider", xlsp_json_new_bool(true));
    }

    // Definition
    if (server->capabilities.definition) {
        xlsp_json_object_set_new(capabilities, "definitionProvider", xlsp_json_new_bool(true));
    }

    // References
    if (server->capabilities.references) {
        xlsp_json_object_set_new(capabilities, "referencesProvider", xlsp_json_new_bool(true));
    }

    // Document symbol
    if (server->capabilities.document_symbol) {
        xlsp_json_object_set_new(capabilities, "documentSymbolProvider", xlsp_json_new_bool(true));
    }

    // Rename
    if (server->capabilities.rename) {
        XrJsonValue *rename = xlsp_json_new_object();
        xlsp_json_object_set_new(rename, "prepareProvider", xlsp_json_new_bool(true));
        xlsp_json_object_set_new(capabilities, "renameProvider", rename);
    }

    // Formatting
    if (server->capabilities.formatting) {
        xlsp_json_object_set_new(capabilities, "documentFormattingProvider", xlsp_json_new_bool(true));
        // Range formatting disabled: current impl formats the whole document
        // xlsp_json_object_set_new(capabilities, "documentRangeFormattingProvider", xlsp_json_new_bool(true));

        // On-type formatting (auto-indent on } and newline)
        XrJsonValue *onType = xlsp_json_new_object();
        xlsp_json_object_set_new(onType, "firstTriggerCharacter", xlsp_json_new_string("}"));
        XrJsonValue *moreTriggers = xlsp_json_new_array();
        xlsp_json_array_push(moreTriggers, xlsp_json_new_string("\n"));
        xlsp_json_array_push(moreTriggers, xlsp_json_new_string(";"));
        xlsp_json_object_set_new(onType, "moreTriggerCharacter", moreTriggers);
        xlsp_json_object_set_new(capabilities, "documentOnTypeFormattingProvider", onType);
    }

    // Signature help
    XrJsonValue *sigHelp = xlsp_json_new_object();
    XrJsonValue *sigTriggers = xlsp_json_new_array();
    xlsp_json_array_push(sigTriggers, xlsp_json_new_string("("));
    xlsp_json_array_push(sigTriggers, xlsp_json_new_string(","));
    xlsp_json_object_set_new(sigHelp, "triggerCharacters", sigTriggers);
    xlsp_json_object_set_new(capabilities, "signatureHelpProvider", sigHelp);

    // Semantic tokens
    XrJsonValue *semTokens = xlsp_json_new_object();
    xlsp_json_object_set_new(semTokens, "legend", xlsp_semantic_tokens_legend());
    XrJsonValue *semFull = xlsp_json_new_object();
    xlsp_json_object_set_new(semFull, "delta", xlsp_json_new_bool(true));
    xlsp_json_object_set_new(semTokens, "full", semFull);
    xlsp_json_object_set_new(semTokens, "range", xlsp_json_new_bool(true));
    xlsp_json_object_set_new(capabilities, "semanticTokensProvider", semTokens);

    // Inlay hints
    xlsp_json_object_set_new(capabilities, "inlayHintProvider", xlsp_json_new_bool(true));

    // CodeLens
    xlsp_json_object_set_new(capabilities, "codeLensProvider",
        xlsp_json_new_object());  // empty object = supported, no resolve

    // Folding range
    xlsp_json_object_set_new(capabilities, "foldingRangeProvider", xlsp_json_new_bool(true));

    // Code actions
    XrJsonValue *codeAction = xlsp_json_new_object();
    XrJsonValue *codeActionKinds = xlsp_json_new_array();
    xlsp_json_array_push(codeActionKinds, xlsp_json_new_string("quickfix"));
    xlsp_json_array_push(codeActionKinds, xlsp_json_new_string("source.organizeImports"));
    xlsp_json_object_set_new(codeAction, "codeActionKinds", codeActionKinds);
    xlsp_json_object_set_new(capabilities, "codeActionProvider", codeAction);

    // Document highlight
    xlsp_json_object_set_new(capabilities, "documentHighlightProvider", xlsp_json_new_bool(true));

    // Workspace symbol
    xlsp_json_object_set_new(capabilities, "workspaceSymbolProvider", xlsp_json_new_bool(true));

    // Selection range
    xlsp_json_object_set_new(capabilities, "selectionRangeProvider", xlsp_json_new_bool(true));

    // Document link
    xlsp_json_object_set_new(capabilities, "documentLinkProvider", xlsp_json_new_bool(true));

    // Call hierarchy
    xlsp_json_object_set_new(capabilities, "callHierarchyProvider", xlsp_json_new_bool(true));

    // Type hierarchy
    xlsp_json_object_set_new(capabilities, "typeHierarchyProvider", xlsp_json_new_bool(true));

    // Implementation
    xlsp_json_object_set_new(capabilities, "implementationProvider", xlsp_json_new_bool(true));

    // Workspace capabilities
    XrJsonValue *workspace = xlsp_json_new_object();
    XrJsonValue *workspaceFolders = xlsp_json_new_object();
    xlsp_json_object_set_new(workspaceFolders, "supported", xlsp_json_new_bool(true));
    xlsp_json_object_set_new(workspaceFolders, "changeNotifications", xlsp_json_new_bool(true));
    xlsp_json_object_set_new(workspace, "workspaceFolders", workspaceFolders);
    xlsp_json_object_set_new(capabilities, "workspace", workspace);

    // Position encoding (LSP 3.17 general capability).
    // We operate on UTF-16 code units throughout xlsp_position_to_offset /
    // xlsp_offset_to_position, so declare that explicitly — clients that
    // advertised UTF-8 / UTF-32 then fall back to UTF-16, and we avoid
    // silent mismatches for files containing astral-plane characters.
    xlsp_json_object_set_new(capabilities, "positionEncoding",
                             xlsp_json_new_string("utf-16"));

    xlsp_json_object_set_new(result, "capabilities", capabilities);

    // Server info
    XrJsonValue *serverInfo = xlsp_json_new_object();
    xlsp_json_object_set_new(serverInfo, "name", xlsp_json_new_string("xray-lsp"));
    xlsp_json_object_set_new(serverInfo, "version", xlsp_json_new_string(XRAY_VERSION_STRING));
    xlsp_json_object_set_new(result, "serverInfo", serverInfo);

    server->initialized = true;

    return result;
}

static void handle_initialized(XrLspServer *server, XrJsonValue *params) {
    (void)params;
    lsp_log("Client initialized");

    // Register for file watching via client/registerCapability
    // Watch for .xr files changes (create, modify, delete)
    XrJsonValue *reg_params = xlsp_json_new_object();
    XrJsonValue *registrations = xlsp_json_new_array();

    XrJsonValue *registration = xlsp_json_new_object();
    xlsp_json_object_set_new(registration, "id", xlsp_json_new_string("xray-file-watcher"));
    xlsp_json_object_set_new(registration, "method", xlsp_json_new_string("workspace/didChangeWatchedFiles"));

    XrJsonValue *reg_options = xlsp_json_new_object();
    XrJsonValue *watchers = xlsp_json_new_array();

    // Watch all .xr files
    XrJsonValue *watcher = xlsp_json_new_object();
    xlsp_json_object_set_new(watcher, "globPattern", xlsp_json_new_string("**/*.xr"));
    xlsp_json_object_set_new(watcher, "kind", xlsp_json_new_number(LSP_WATCH_ALL));
    xlsp_json_array_push(watchers, watcher);

    xlsp_json_object_set_new(reg_options, "watchers", watchers);
    xlsp_json_object_set_new(registration, "registerOptions", reg_options);
    xlsp_json_array_push(registrations, registration);
    xlsp_json_object_set_new(reg_params, "registrations", registrations);

    // Send registration request (LSP spec requires this to be a request, not notification)
    send_request(server, "client/registerCapability", reg_params);

    lsp_log("Registered file watcher for **/*.xr");

    // Start background workspace indexing for all workspace folders.
    // root_path/root_uri were folded into workspace_folders at initialize.
    if (server->workspace_folder_count > 0) {
        const char *paths[MAX_WORKSPACE_FOLDERS];
        int path_count = 0;
        for (int i = 0; i < server->workspace_folder_count; i++) {
            if (server->workspace_folders[i].path) {
                paths[path_count++] = server->workspace_folders[i].path;
                server->workspace_folders[i].index_requested = true;
            }
        }
        if (path_count > 0) {
            xlsp_workspace_start_background_index_roots(server, paths, path_count);
        }
    }
}

static XrJsonValue *handle_shutdown(XrLspServer *server, XrJsonValue *params) {
    (void)params;
    lsp_log("Shutdown requested");
    // Per LSP spec: shutdown is a request, not an exit trigger. We
    // flush state, reply with null, then wait for the mandatory
    // `exit` notification. After this point handle_message will
    // reject everything except `shutdown`/`exit` with -32002.
    server->shutdown_received = true;
    return xlsp_json_new_null();
}

static void handle_exit(XrLspServer *server, XrJsonValue *params) {
    (void)params;
    lsp_log("Exit notification received");
    // Only `exit` drops us out of xlsp_server_run. A process-wide
    // exit code of 0 vs 1 is decided later based on shutdown_received.
    server->exit_received = true;
}

// File change types (LSP spec)
#define FILE_CHANGE_CREATED 1
#define FILE_CHANGE_CHANGED 2
#define FILE_CHANGE_DELETED 3

static void handle_did_change_watched_files(XrLspServer *server, XrJsonValue *params) {
    XrJsonValue *changes = xlsp_json_get_array(params, "changes");
    if (!changes) return;

    int change_count = xlsp_json_array_len(changes);
    lsp_log("Received %d file change events", change_count);

    for (int i = 0; i < change_count; i++) {
        XrJsonValue *change = xlsp_json_array_get(changes, i);
        const char *uri = xlsp_json_get_string(change, "uri");
        int type = (int)xlsp_json_get_int(change, "type");

        if (!uri) continue;

        lsp_log("File change: %s (type=%d)", uri, type);

        // Get document if it's open
        XrLspDocument *doc = xlsp_document_get(server, uri);

        switch (type) {
            case FILE_CHANGE_CREATED:
            case FILE_CHANGE_CHANGED: {
                const char *path = xlsp_uri_to_path(uri);

                // Invalidate exports cache for this file
                xlsp_exports_cache_remove(server, path);

                // Re-index the file in workspace analyzer (for unopened files)
                if (!doc) {
                    xlsp_workspace_index_file(server, uri, path);
                }

                // If document is open, re-parse and publish diagnostics
                if (doc) {
                    doc->dirty = true;
                    xlsp_parse_document(doc, server);
                    publish_diagnostics(server, doc);
                }
                break;
            }

            case FILE_CHANGE_DELETED: {
                const char *del_path = xlsp_uri_to_path(uri);

                // Remove from exports cache
                xlsp_exports_cache_remove(server, del_path);

                // Remove from analyzer so stale symbols don't linger
                if (server->workspace_analyzer) {
                    xa_analyzer_remove_file(server->workspace_analyzer, del_path);
                }

                lsp_log("Purged state for deleted file: %s", del_path);
                break;
            }
        }
    }

    // Re-publish diagnostics for all open documents that import changed files
    // This ensures cross-file errors are updated
    // (For simplicity, we don't track import dependencies here)
}

// ============================================================================
// Workspace Folders
// ============================================================================

// Add a workspace folder
static void add_workspace_folder(XrLspServer *server, const char *uri, const char *name) {
    if (server->workspace_folder_count >= MAX_WORKSPACE_FOLDERS) {
        lsp_log("Warning: Maximum workspace folders reached");
        return;
    }

    int idx = server->workspace_folder_count++;
    server->workspace_folders[idx].uri = xr_strdup(uri);
    server->workspace_folders[idx].name = name ? xr_strdup(name) : xr_strdup("");

    const char *path = xlsp_uri_to_path(uri);
    server->workspace_folders[idx].path = xr_strdup(path);
    server->workspace_folders[idx].config_loaded = false;
    server->workspace_folders[idx].index_requested = true;
    server->workspace_folders[idx].index_completed = false;

    lsp_log("Added workspace folder: %s (%s)", name ? name : uri, path);

    // Start indexing this folder
    xlsp_workspace_start_background_index(server, path);
}

// Remove a workspace folder and purge related state
static void remove_workspace_folder(XrLspServer *server, const char *uri) {
    for (int i = 0; i < server->workspace_folder_count; i++) {
        if (strcmp(server->workspace_folders[i].uri, uri) == 0) {
            lsp_log("Removed workspace folder: %s", server->workspace_folders[i].name);

            // Purge analyzer and cache state for files under this folder
            if (server->workspace_folders[i].path) {
                xlsp_workspace_purge_prefix(server, server->workspace_folders[i].path);
            }

            xr_free(server->workspace_folders[i].uri);
            xr_free(server->workspace_folders[i].name);
            xr_free(server->workspace_folders[i].path);

            // Shift remaining folders
            for (int j = i; j < server->workspace_folder_count - 1; j++) {
                server->workspace_folders[j] = server->workspace_folders[j + 1];
            }
            server->workspace_folder_count--;
            return;
        }
    }
}

static void handle_did_change_workspace_folders(XrLspServer *server, XrJsonValue *params) {
    XrJsonValue *event = xlsp_json_get_object(params, "event");
    if (!event) return;

    // Handle removed folders
    XrJsonValue *removed = xlsp_json_get_array(event, "removed");
    if (removed) {
        int count = xlsp_json_array_len(removed);
        for (int i = 0; i < count; i++) {
            XrJsonValue *folder = xlsp_json_array_get(removed, i);
            const char *uri = xlsp_json_get_string(folder, "uri");
            if (uri) {
                remove_workspace_folder(server, uri);
            }
        }
    }

    // Handle added folders
    XrJsonValue *added = xlsp_json_get_array(event, "added");
    if (added) {
        int count = xlsp_json_array_len(added);
        for (int i = 0; i < count; i++) {
            XrJsonValue *folder = xlsp_json_array_get(added, i);
            const char *uri = xlsp_json_get_string(folder, "uri");
            const char *name = xlsp_json_get_string(folder, "name");
            if (uri) {
                add_workspace_folder(server, uri, name);
            }
        }
    }

    lsp_log("Workspace folders updated: %d total", server->workspace_folder_count);
}

// ============================================================================
// Configuration
// ============================================================================

static void apply_configuration(XrLspServer *server, XrJsonValue *settings) {
    if (!settings) return;

    // Get xray settings section
    XrJsonValue *xray = xlsp_json_get_object(settings, "xray");
    if (!xray) return;

    // Diagnostic settings
    XrJsonValue *diagnostics = xlsp_json_get_object(xray, "diagnostics");
    if (diagnostics) {
        if (xlsp_json_get(diagnostics, "enabled")) {
            bool was_enabled = server->config.diagnostics_enabled;
            server->config.diagnostics_enabled = xlsp_json_get_bool(diagnostics, "enabled");
            // On disable: clear pending queue and publish empty diagnostics
            if (was_enabled && !server->config.diagnostics_enabled) {
                for (int i = 0; i < server->pending_diag_count; i++) {
                    if (server->pending_diag[i]) {
                        server->pending_diag[i]->diag_pending = false;
                    }
                }
                server->pending_diag_count = 0;
                clear_all_diagnostics(server);
            }
        }
        if (xlsp_json_get(diagnostics, "debounceMs")) {
            server->config.diagnostic_debounce_ms = (int)xlsp_json_get_int(diagnostics, "debounceMs");
        }
    }

    // Completion settings
    XrJsonValue *completion = xlsp_json_get_object(xray, "completion");
    if (completion) {
        if (xlsp_json_get(completion, "maxItems")) {
            server->config.completion_max_items = (int)xlsp_json_get_int(completion, "maxItems");
        }
    }

    // Format settings
    XrJsonValue *format = xlsp_json_get_object(xray, "format");
    if (format) {
        if (xlsp_json_get(format, "tabSize")) {
            server->config.format_tab_size = (int)xlsp_json_get_int(format, "tabSize");
        }
        if (xlsp_json_get(format, "insertSpaces")) {
            server->config.format_insert_spaces = xlsp_json_get_bool(format, "insertSpaces");
        }
    }

    // Inlay hints settings
    XrJsonValue *inlay_hints = xlsp_json_get_object(xray, "inlayHints");
    if (inlay_hints) {
        if (xlsp_json_get(inlay_hints, "typeAnnotations")) {
            server->config.inlay_hints_type_annotations = xlsp_json_get_bool(inlay_hints, "typeAnnotations");
        }
        if (xlsp_json_get(inlay_hints, "parameterNames")) {
            server->config.inlay_hints_parameter_names = xlsp_json_get_bool(inlay_hints, "parameterNames");
        }
    }

    lsp_log("Configuration updated: debounce=%dms, diagnostics=%s, maxItems=%d, tabSize=%d",
            server->config.diagnostic_debounce_ms,
            server->config.diagnostics_enabled ? "on" : "off",
            server->config.completion_max_items,
            server->config.format_tab_size);
}

static void handle_did_change_configuration(XrLspServer *server, XrJsonValue *params) {
    XrJsonValue *settings = xlsp_json_get_object(params, "settings");
    apply_configuration(server, settings);
}

static void handle_did_open(XrLspServer *server, XrJsonValue *params) {
    XrJsonValue *textDocument = xlsp_json_get_object(params, "textDocument");
    if (!textDocument) return;

    const char *uri = xlsp_json_get_string(textDocument, "uri");
    const char *text = xlsp_json_get_string(textDocument, "text");
    int version = (int)xlsp_json_get_int(textDocument, "version");

    if (uri && text) {
        XrLspDocument *doc = xlsp_document_open(server, uri, text, version);
        if (doc) {
            // Parse document to AST
            xlsp_parse_document(doc, server);
            if (server->config.diagnostics_enabled) {
                publish_diagnostics(server, doc);
            }
        }
    }
}

static void handle_did_change(XrLspServer *server, XrJsonValue *params) {
    XrJsonValue *textDocument = xlsp_json_get_object(params, "textDocument");
    if (!textDocument) return;

    const char *uri = xlsp_json_get_string(textDocument, "uri");
    XrLspDocument *doc = xlsp_document_get(server, uri);
    if (!doc) return;

    doc->version = (int)xlsp_json_get_int(textDocument, "version");

    XrJsonValue *changes = xlsp_json_get_array(params, "contentChanges");
    if (!changes) return;

    int change_count = xlsp_json_array_len(changes);
    for (int i = 0; i < change_count; i++) {
        XrJsonValue *change = xlsp_json_array_get(changes, i);
        const char *text = xlsp_json_get_string(change, "text");
        XrJsonValue *range_obj = xlsp_json_get_object(change, "range");

        if (text) {
            if (range_obj) {
                // Incremental change
                XrJsonValue *start = xlsp_json_get_object(range_obj, "start");
                XrJsonValue *end = xlsp_json_get_object(range_obj, "end");

                XrLspRange range = {
                    .start = {
                        .line = (uint32_t)xlsp_json_get_int(start, "line"),
                        .character = (uint32_t)xlsp_json_get_int(start, "character")
                    },
                    .end = {
                        .line = (uint32_t)xlsp_json_get_int(end, "line"),
                        .character = (uint32_t)xlsp_json_get_int(end, "character")
                    }
                };
                xlsp_document_change(doc, &range, text);
            } else {
                // Full sync
                xlsp_document_change(doc, NULL, text);
            }
        }
    }

    // Track previous error state to detect recovery
    bool had_error_before = doc->parse_error;

    // Re-parse document after change
    lsp_log("didChange: parsing %s", uri);
    xlsp_parse_document(doc, server);

    // If document recovered from error, trigger re-analysis of dependent documents
    if (had_error_before && !doc->parse_error) {
        lsp_log("didChange: document recovered from error, triggering dependent re-analysis");
        // Re-parse all open documents that might import this file
        if (server->doc_table) {
            XrLspDocTable *table = server->doc_table;
            for (int i = 0; i < table->bucket_count; i++) {
                XrLspDocBucket *bucket = table->buckets[i];
                while (bucket) {
                    XrLspDocument *other = bucket->doc;
                    if (other != doc && other->content) {
                        other->dirty = true;
                        xlsp_parse_document(other, server);
                        schedule_diagnostics(server, other);
                    }
                    bucket = bucket->next;
                }
            }
        }
    }

    // Schedule debounced diagnostics (waits 300ms for more changes)
    schedule_diagnostics(server, doc);
    lsp_log("didChange: diagnostics scheduled");
}

static void handle_did_close(XrLspServer *server, XrJsonValue *params) {
    XrJsonValue *textDocument = xlsp_json_get_object(params, "textDocument");
    if (!textDocument) return;

    const char *uri = xlsp_json_get_string(textDocument, "uri");
    if (uri) {
        xlsp_document_close(server, uri);
    }
}

static XrJsonValue *handle_completion(XrLspServer *server, XrJsonValue *params) {
    lsp_log("handle_completion: CALLED");
    XrJsonValue *textDocument = xlsp_json_get_object(params, "textDocument");
    XrJsonValue *position = xlsp_json_get_object(params, "position");
    if (!textDocument || !position) {
        lsp_log("handle_completion: missing textDocument or position");
        return xlsp_json_new_null();
    }

    const char *uri = xlsp_json_get_string(textDocument, "uri");
    XrLspDocument *doc = xlsp_document_get(server, uri);
    if (!doc) return xlsp_json_new_null();

    XrLspPosition pos = {
        .line = (uint32_t)xlsp_json_get_int(position, "line"),
        .character = (uint32_t)xlsp_json_get_int(position, "character")
    };

    lsp_log("handle_completion: uri=%s, line=%d, char=%d", uri, pos.line, pos.character);

    XrJsonValue *items = xlsp_analyze_completion(server, doc, pos);

    int item_count = items ? xlsp_json_array_len(items) : 0;
    int max_items = server->config.completion_max_items;
    bool truncated = (max_items > 0 && item_count > max_items);
    if (truncated) {
        xlsp_json_array_truncate(items, max_items);
        item_count = max_items;
    }
    lsp_log("handle_completion: returning %d items%s", item_count,
            truncated ? " (truncated)" : "");

    XrJsonValue *result = xlsp_json_new_object();
    xlsp_json_object_set(result, "isIncomplete", xlsp_json_new_bool(truncated));
    xlsp_json_object_set(result, "items", items);

    return result;
}

// Completion resolve: add detailed documentation from analyzer
static XrJsonValue *handle_completion_resolve(XrLspServer *server, XrJsonValue *params) {
    XrJsonValue *data = xlsp_json_get_object(params, "data");
    if (!data) return xlsp_json_clone(params);

    const char *uri = xlsp_json_get_string(data, "uri");
    const char *name = xlsp_json_get_string(params, "label");

    XrJsonValue *result = xlsp_json_clone(params);
    if (!name) return result;

    XaAnalyzer *analyzer = server ? server->workspace_analyzer : NULL;
    char doc_str[XLSP_MAX_PATH];
    int len = 0;
    bool resolved = false;

    // Try analyzer for real type information
    if (analyzer) {
        XaSymbol *sym = xa_analyzer_lookup(analyzer, name);
        if (sym) {
            XaSymbolLinks *links = xa_analyzer_get_links(analyzer, sym);

            if (sym->kind == XA_SYM_FUNCTION || sym->kind == XA_SYM_METHOD) {
                len = snprintf(doc_str, sizeof(doc_str), "```xray\nfn %s(", name);
                if (links && links->param_count > 0) {
                    for (int i = 0; i < links->param_count; i++) {
                        if (i > 0) len += snprintf(doc_str + len, sizeof(doc_str) - len, ", ");
                        const char *pname = (links->param_names && links->param_names[i])
                            ? links->param_names[i] : "_";
                        const char *ptype = (links->param_types && links->param_types[i])
                            ? xr_type_to_string(links->param_types[i]) : "unknown";
                        len += snprintf(doc_str + len, sizeof(doc_str) - len, "%s: %s", pname, ptype);
                    }
                }
                const char *ret = (links && links->return_type)
                    ? xr_type_to_string(links->return_type) : "void";
                snprintf(doc_str + len, sizeof(doc_str) - len, "): %s\n```", ret);
                resolved = true;
            } else if (sym->kind == XA_SYM_CLASS) {
                snprintf(doc_str, sizeof(doc_str), "```xray\nclass %s\n```", name);
                resolved = true;
            } else {
                XrType *type = xa_analyzer_get_type(analyzer, sym);
                const char *type_str = type ? xr_type_to_string(type) : "unknown";
                const char *kw = sym->is_const ? "const" : "let";
                snprintf(doc_str, sizeof(doc_str), "```xray\n%s %s: %s\n```", kw, name, type_str);
                resolved = true;
            }
        }
    }

    // Fallback: generic documentation from symbol table
    if (!resolved && uri) {
        XrLspDocument *doc = xlsp_document_get(server, uri);
        if (doc && doc->ast) {
            snprintf(doc_str, sizeof(doc_str), "Symbol `%s` defined in this module.", name);
            resolved = true;
        }
    }

    if (resolved) {
        XrJsonValue *documentation = xlsp_json_new_object();
        xlsp_json_object_set(documentation, "kind", xlsp_json_new_string("markdown"));
        xlsp_json_object_set(documentation, "value", xlsp_json_new_string(doc_str));
        xlsp_json_object_set(result, "documentation", documentation);
    }

    return result;
}

static XrJsonValue *handle_hover(XrLspServer *server, XrJsonValue *params) {
    XrJsonValue *textDocument = xlsp_json_get_object(params, "textDocument");
    XrJsonValue *position = xlsp_json_get_object(params, "position");
    if (!textDocument || !position) return xlsp_json_new_null();

    const char *uri = xlsp_json_get_string(textDocument, "uri");
    XrLspDocument *doc = xlsp_document_get(server, uri);
    if (!doc) return xlsp_json_new_null();

    XrLspPosition pos = {
        .line = (uint32_t)xlsp_json_get_int(position, "line"),
        .character = (uint32_t)xlsp_json_get_int(position, "character")
    };

    return xlsp_analyze_hover(server, doc, pos);
}

static XrJsonValue *handle_document_symbol(XrLspServer *server, XrJsonValue *params) {
    XrJsonValue *textDocument = xlsp_json_get_object(params, "textDocument");
    if (!textDocument) return xlsp_json_new_array();

    const char *uri = xlsp_json_get_string(textDocument, "uri");
    XrLspDocument *doc = xlsp_document_get(server, uri);
    if (!doc) return xlsp_json_new_array();

    return xlsp_analyze_document_symbols(doc);
}

static XrJsonValue *handle_definition(XrLspServer *server, XrJsonValue *params) {
    XrJsonValue *textDocument = xlsp_json_get_object(params, "textDocument");
    XrJsonValue *position = xlsp_json_get_object(params, "position");
    if (!textDocument || !position) return xlsp_json_new_null();

    const char *uri = xlsp_json_get_string(textDocument, "uri");
    XrLspDocument *doc = xlsp_document_get(server, uri);
    if (!doc) return xlsp_json_new_null();

    XrLspPosition pos = {
        .line = (uint32_t)xlsp_json_get_int(position, "line"),
        .character = (uint32_t)xlsp_json_get_int(position, "character")
    };

    return xlsp_analyze_definition(server, doc, pos);
}

static XrJsonValue *handle_references(XrLspServer *server, XrJsonValue *params) {
    XrJsonValue *textDocument = xlsp_json_get_object(params, "textDocument");
    XrJsonValue *position = xlsp_json_get_object(params, "position");
    if (!textDocument || !position) return xlsp_json_new_array();

    const char *uri = xlsp_json_get_string(textDocument, "uri");
    XrLspDocument *doc = xlsp_document_get(server, uri);
    if (!doc) return xlsp_json_new_array();

    XrLspPosition pos = {
        .line = (uint32_t)xlsp_json_get_int(position, "line"),
        .character = (uint32_t)xlsp_json_get_int(position, "character")
    };

    return xlsp_analyze_references(server, doc, pos);
}

static XrJsonValue *handle_rename(XrLspServer *server, XrJsonValue *params) {
    XrJsonValue *textDocument = xlsp_json_get_object(params, "textDocument");
    XrJsonValue *position = xlsp_json_get_object(params, "position");
    const char *new_name = xlsp_json_get_string(params, "newName");
    if (!textDocument || !position || !new_name) return xlsp_json_new_null();

    const char *uri = xlsp_json_get_string(textDocument, "uri");
    XrLspDocument *doc = xlsp_document_get(server, uri);
    if (!doc) return xlsp_json_new_null();

    XrLspPosition pos = {
        .line = (uint32_t)xlsp_json_get_int(position, "line"),
        .character = (uint32_t)xlsp_json_get_int(position, "character")
    };

    return xlsp_analyze_rename(server, doc, pos, new_name);
}

static XrJsonValue *handle_prepare_rename(XrLspServer *server, XrJsonValue *params) {
    XrJsonValue *textDocument = xlsp_json_get_object(params, "textDocument");
    XrJsonValue *position = xlsp_json_get_object(params, "position");
    if (!textDocument || !position) return xlsp_json_new_null();

    const char *uri = xlsp_json_get_string(textDocument, "uri");
    XrLspDocument *doc = xlsp_document_get(server, uri);
    if (!doc) return xlsp_json_new_null();

    XrLspPosition pos = {
        .line = (uint32_t)xlsp_json_get_int(position, "line"),
        .character = (uint32_t)xlsp_json_get_int(position, "character")
    };

    return xlsp_analyze_prepare_rename(doc, pos);
}

static XrJsonValue *handle_formatting(XrLspServer *server, XrJsonValue *params) {
    XrJsonValue *textDocument = xlsp_json_get_object(params, "textDocument");
    if (!textDocument) return xlsp_json_new_array();

    const char *uri = xlsp_json_get_string(textDocument, "uri");
    XrLspDocument *doc = xlsp_document_get(server, uri);
    if (!doc) return xlsp_json_new_array();

    return xlsp_analyze_format(doc);
}

static XrJsonValue *handle_range_formatting(XrLspServer *server, XrJsonValue *params) {
    XrJsonValue *textDocument = xlsp_json_get_object(params, "textDocument");
    XrJsonValue *range_obj = xlsp_json_get_object(params, "range");
    if (!textDocument || !range_obj) return xlsp_json_new_array();

    const char *uri = xlsp_json_get_string(textDocument, "uri");
    XrLspDocument *doc = xlsp_document_get(server, uri);
    if (!doc) return xlsp_json_new_array();

    XrJsonValue *start = xlsp_json_get_object(range_obj, "start");
    XrJsonValue *end = xlsp_json_get_object(range_obj, "end");

    XrLspRange range = {
        .start = {
            .line = (uint32_t)xlsp_json_get_int(start, "line"),
            .character = (uint32_t)xlsp_json_get_int(start, "character")
        },
        .end = {
            .line = (uint32_t)xlsp_json_get_int(end, "line"),
            .character = (uint32_t)xlsp_json_get_int(end, "character")
        }
    };

    return xlsp_analyze_format_range(doc, range);
}

// On-type formatting: auto-indent when typing }, newline, or ;
static XrJsonValue *handle_on_type_formatting(XrLspServer *server, XrJsonValue *params) {
    XrJsonValue *textDocument = xlsp_json_get_object(params, "textDocument");
    XrJsonValue *position = xlsp_json_get_object(params, "position");
    const char *ch = xlsp_json_get_string(params, "ch");
    if (!textDocument || !position || !ch) return xlsp_json_new_array();

    const char *uri = xlsp_json_get_string(textDocument, "uri");
    XrLspDocument *doc = xlsp_document_get(server, uri);
    if (!doc || !doc->content) return xlsp_json_new_array();

    int line = xlsp_json_get_int(position, "line");
    XrJsonValue *edits = xlsp_json_new_array();

    // Get formatting options
    XrJsonValue *options = xlsp_json_get_object(params, "options");
    int tab_size = options ? (int)xlsp_json_get_int(options, "tabSize") : 4;
    if (tab_size <= 0) tab_size = 4;

    // Get the current line content
    const char *line_start = doc->content;
    int cur_line = 0;
    while (cur_line < line && *line_start) {
        if (*line_start == '\n') cur_line++;
        line_start++;
    }
    const char *line_end = line_start;
    while (*line_end && *line_end != '\n') line_end++;

    if (ch[0] == '}') {
        // Count brace nesting up to this line to determine correct indent
        int depth = 0;
        const char *p = doc->content;
        while (p < line_start) {
            if (*p == '{') depth++;
            else if (*p == '}') depth--;
            else if (*p == '/' && p[1] == '/') {
                while (p < line_start && *p != '\n') p++;
                continue;
            } else if (*p == '"' || *p == '\'') {
                char q = *p++;
                while (p < line_start && *p != q) {
                    if (*p == '\\') p++;
                    p++;
                }
            }
            p++;
        }
        // } closes one level, so indent at depth-1
        if (depth > 0) depth--;
        int target_indent = depth * tab_size;

        // Calculate current indent on this line
        int current_indent = 0;
        const char *cp = line_start;
        while (cp < line_end && (*cp == ' ' || *cp == '\t')) {
            current_indent += (*cp == '\t') ? tab_size : 1;
            cp++;
        }

        if (current_indent != target_indent) {
            // Replace existing whitespace with correct indent
            int ws_chars = (int)(cp - line_start);
            char indent_str[256];
            int n = target_indent < (int)sizeof(indent_str) - 1 ? target_indent : (int)sizeof(indent_str) - 1;
            memset(indent_str, ' ', n);
            indent_str[n] = '\0';

            XrJsonValue *edit = xlsp_json_new_object();
            xlsp_json_object_set(edit, "range",
                xlsp_json_make_range(line, 0, line, ws_chars));
            xlsp_json_object_set(edit, "newText", xlsp_json_new_string(indent_str));
            xlsp_json_array_push(edits, edit);
        }
    } else if (ch[0] == '\n') {
        // Auto-indent: match previous line's indent, +1 level if prev ends with {
        if (line <= 0) return edits;

        // Find previous line start
        const char *prev_line_start = doc->content;
        cur_line = 0;
        while (cur_line < line - 1 && *prev_line_start) {
            if (*prev_line_start == '\n') cur_line++;
            prev_line_start++;
        }

        int prev_indent = 0;
        const char *pp = prev_line_start;
        while (*pp && *pp != '\n' && (*pp == ' ' || *pp == '\t')) {
            prev_indent += (*pp == '\t') ? tab_size : 1;
            pp++;
        }

        // Check if prev line ends with {
        const char *prev_end = prev_line_start;
        while (*prev_end && *prev_end != '\n') prev_end++;
        const char *last_non_ws = prev_end - 1;
        while (last_non_ws > prev_line_start && (*last_non_ws == ' ' || *last_non_ws == '\t' || *last_non_ws == '\r'))
            last_non_ws--;

        int target_indent = prev_indent;
        if (last_non_ws >= prev_line_start && *last_non_ws == '{')
            target_indent += tab_size;

        // Check current line indent
        int current_indent = 0;
        const char *cp = line_start;
        while (cp < line_end && (*cp == ' ' || *cp == '\t')) {
            current_indent += (*cp == '\t') ? tab_size : 1;
            cp++;
        }

        if (current_indent != target_indent) {
            int ws_chars = (int)(cp - line_start);
            char indent_str[256];
            int n = target_indent < (int)sizeof(indent_str) - 1 ? target_indent : (int)sizeof(indent_str) - 1;
            memset(indent_str, ' ', n);
            indent_str[n] = '\0';

            XrJsonValue *edit = xlsp_json_new_object();
            xlsp_json_object_set(edit, "range",
                xlsp_json_make_range(line, 0, line, ws_chars));
            xlsp_json_object_set(edit, "newText", xlsp_json_new_string(indent_str));
            xlsp_json_array_push(edits, edit);
        }
    }
    // For ';' we don't do anything special yet

    return edits;
}

// Name reference count table for CodeLens (single-pass scan)
#define REF_TABLE_SIZE 128

typedef struct RefCountEntry {
    char *name;
    int count;
    struct RefCountEntry *next;
} RefCountEntry;

typedef struct {
    RefCountEntry *buckets[REF_TABLE_SIZE];
} RefCountTable;

static void ref_table_init(RefCountTable *t) {
    memset(t->buckets, 0, sizeof(t->buckets));
}

static void ref_table_free(RefCountTable *t) {
    for (int i = 0; i < REF_TABLE_SIZE; i++) {
        RefCountEntry *e = t->buckets[i];
        while (e) {
            RefCountEntry *next = e->next;
            xr_free(e->name);
            xr_free(e);
            e = next;
        }
    }
}

static void ref_table_increment(RefCountTable *t, const char *name, size_t len) {
    uint32_t h = xr_hash_bytes(name, len) % REF_TABLE_SIZE;
    for (RefCountEntry *e = t->buckets[h]; e; e = e->next) {
        if (strlen(e->name) == len && strncmp(e->name, name, len) == 0) {
            e->count++;
            return;
        }
    }
    RefCountEntry *e = xr_malloc(sizeof(RefCountEntry));
    e->name = xr_malloc(len + 1);
    memcpy(e->name, name, len);
    e->name[len] = '\0';
    e->count = 1;
    e->next = t->buckets[h];
    t->buckets[h] = e;
}

static int ref_table_get(RefCountTable *t, const char *name) {
    size_t len = strlen(name);
    uint32_t h = xr_hash_bytes(name, len) % REF_TABLE_SIZE;
    for (RefCountEntry *e = t->buckets[h]; e; e = e->next) {
        if (strcmp(e->name, name) == 0) return e->count;
    }
    return 0;
}

// Build name reference count table in a single lexer pass
static void build_ref_count_table(const char *content, RefCountTable *table) {
    ref_table_init(table);
    if (!content) return;
    Scanner scanner;
    xr_scanner_init(&scanner, content);
    Token token;
    while (1) {
        token = xr_scanner_scan(&scanner);
        if (token.type == TK_EOF) break;
        if (token.type == TK_ERROR) continue;
        if (token.type == TK_NAME) {
            ref_table_increment(table, token.start, token.length);
        }
    }
}

// Helper: create a CodeLens JSON object
static void add_code_lens(XrJsonValue *lenses, const char *name, int line,
                           RefCountTable *ref_table) {
    // -1 to exclude the definition itself
    int refs = ref_table_get(ref_table, name) - 1;
    if (refs < 0) refs = 0;

    char title[128];
    snprintf(title, sizeof(title), "%d reference%s", refs, refs == 1 ? "" : "s");

    XrJsonValue *lens = xlsp_json_new_object();
    xlsp_json_object_set(lens, "range", xlsp_json_make_range(line, 0, line, 0));
    XrJsonValue *cmd = xlsp_json_new_object();
    xlsp_json_object_set(cmd, "title", xlsp_json_new_string(title));
    xlsp_json_object_set(cmd, "command", xlsp_json_new_string(""));
    xlsp_json_object_set(lens, "command", cmd);
    xlsp_json_array_push(lenses, lens);
}

// Collect CodeLens items from AST (functions and classes)
static void collect_code_lens(AstNode *node, XrJsonValue *lenses,
                               RefCountTable *ref_table) {
    if (!node) return;

    if (node->type == AST_FUNCTION_DECL && node->as.function_decl.name) {
        int line = node->line > 0 ? node->line - 1 : 0;
        add_code_lens(lenses, node->as.function_decl.name, line, ref_table);
    }

    if ((node->type == AST_CLASS_DECL || node->type == AST_STRUCT_DECL) && node->as.class_decl.name) {
        int line = node->line > 0 ? node->line - 1 : 0;
        add_code_lens(lenses, node->as.class_decl.name, line, ref_table);

        // Also add lenses for class methods
        for (int i = 0; i < node->as.class_decl.method_count; i++) {
            collect_code_lens(node->as.class_decl.methods[i], lenses, ref_table);
        }
        return;  // Don't recurse into children again
    }

    // Recurse into children
    if (node->type == AST_PROGRAM || node->type == AST_BLOCK) {
        int count = (node->type == AST_PROGRAM) ? node->as.program.count : node->as.block.count;
        AstNode **stmts = (node->type == AST_PROGRAM) ? node->as.program.statements : node->as.block.statements;
        for (int i = 0; i < count; i++) {
            collect_code_lens(stmts[i], lenses, ref_table);
        }
    }
}

static XrJsonValue *handle_code_lens(XrLspServer *server, XrJsonValue *params) {
    XrJsonValue *textDocument = xlsp_json_get_object(params, "textDocument");
    if (!textDocument) return xlsp_json_new_array();

    const char *uri = xlsp_json_get_string(textDocument, "uri");
    XrLspDocument *doc = xlsp_document_get(server, uri);
    if (!doc || !doc->ast) return xlsp_json_new_array();

    // Single-pass: build name→count table, then O(1) lookup per symbol
    RefCountTable ref_table;
    build_ref_count_table(doc->content, &ref_table);

    XrJsonValue *lenses = xlsp_json_new_array();
    collect_code_lens(doc->ast, lenses, &ref_table);

    ref_table_free(&ref_table);
    return lenses;
}

static XrJsonValue *handle_signature_help(XrLspServer *server, XrJsonValue *params) {
    XrJsonValue *textDocument = xlsp_json_get_object(params, "textDocument");
    XrJsonValue *position = xlsp_json_get_object(params, "position");
    if (!textDocument || !position) return xlsp_json_new_null();

    const char *uri = xlsp_json_get_string(textDocument, "uri");
    XrLspDocument *doc = xlsp_document_get(server, uri);
    if (!doc) return xlsp_json_new_null();

    XrLspPosition pos = {
        .line = (uint32_t)xlsp_json_get_int(position, "line"),
        .character = (uint32_t)xlsp_json_get_int(position, "character")
    };

    return xlsp_analyze_signature_help(doc, pos);
}

static XrJsonValue *handle_semantic_tokens_full(XrLspServer *server, XrJsonValue *params) {
    XrJsonValue *textDocument = xlsp_json_get_object(params, "textDocument");
    if (!textDocument) return xlsp_json_new_null();

    const char *uri = xlsp_json_get_string(textDocument, "uri");
    XrLspDocument *doc = xlsp_document_get(server, uri);
    if (!doc) return xlsp_json_new_null();

    XlspSemanticTokensResult *result = xlsp_analyze_semantic_tokens(doc);

    // Cache raw tokens for delta support
    int raw_count = 0;
    uint32_t *raw = xlsp_semantic_tokens_encode_raw(result, &raw_count);
    xr_free(doc->prev_sem_tokens);
    doc->prev_sem_tokens = raw;
    doc->prev_sem_token_count = raw_count;
    doc->sem_token_result_id++;

    // Build response with resultId
    XrJsonValue *response = xlsp_json_new_object();
    char rid[32];
    snprintf(rid, sizeof(rid), "%u", doc->sem_token_result_id);
    xlsp_json_object_set(response, "resultId", xlsp_json_new_string(rid));

    XrJsonValue *data = xlsp_json_new_array();
    for (int i = 0; i < raw_count; i++) {
        xlsp_json_array_push(data, xlsp_json_new_number(raw[i]));
    }
    xlsp_json_object_set(response, "data", data);

    xlsp_semantic_tokens_free(result);
    return response;
}

static XrJsonValue *handle_semantic_tokens_delta(XrLspServer *server, XrJsonValue *params) {
    XrJsonValue *textDocument = xlsp_json_get_object(params, "textDocument");
    if (!textDocument) return xlsp_json_new_null();

    const char *uri = xlsp_json_get_string(textDocument, "uri");
    XrLspDocument *doc = xlsp_document_get(server, uri);
    if (!doc) return xlsp_json_new_null();

    // Compute new tokens
    XlspSemanticTokensResult *result = xlsp_analyze_semantic_tokens(doc);
    int new_count = 0;
    uint32_t *new_data = xlsp_semantic_tokens_encode_raw(result, &new_count);
    xlsp_semantic_tokens_free(result);

    // Take ownership of old cached data before updating
    uint32_t *old_data = doc->prev_sem_tokens;
    int old_count = doc->prev_sem_token_count;
    doc->prev_sem_tokens = NULL;
    doc->prev_sem_token_count = 0;

    // Update cache with copy of new data
    if (new_data && new_count > 0) {
        doc->prev_sem_tokens = xr_malloc(sizeof(uint32_t) * new_count);
        if (doc->prev_sem_tokens)
            memcpy(doc->prev_sem_tokens, new_data, sizeof(uint32_t) * new_count);
        doc->prev_sem_token_count = new_count;
    }
    doc->sem_token_result_id++;

    char rid[32];
    snprintf(rid, sizeof(rid), "%u", doc->sem_token_result_id);

    // If no previous data, return full response
    if (!old_data || old_count == 0) {
        XrJsonValue *response = xlsp_json_new_object();
        xlsp_json_object_set(response, "resultId", xlsp_json_new_string(rid));
        XrJsonValue *data = xlsp_json_new_array();
        for (int i = 0; i < new_count; i++)
            xlsp_json_array_push(data, xlsp_json_new_number(new_data[i]));
        xlsp_json_object_set(response, "data", data);
        xr_free(old_data);
        xr_free(new_data);
        return response;
    }

    // Compute delta: find first and last differing positions
    int min_len = old_count < new_count ? old_count : new_count;
    int first_diff = 0;
    while (first_diff < min_len && old_data[first_diff] == new_data[first_diff])
        first_diff++;

    // Align to token boundary (5 values per token)
    first_diff = (first_diff / 5) * 5;

    int old_tail_match = 0;
    while (old_tail_match < (old_count - first_diff) &&
           old_tail_match < (new_count - first_diff) &&
           old_data[old_count - 1 - old_tail_match] == new_data[new_count - 1 - old_tail_match])
        old_tail_match++;
    old_tail_match = (old_tail_match / 5) * 5;

    int del_count = old_count - first_diff - old_tail_match;
    int ins_count = new_count - first_diff - old_tail_match;
    if (del_count < 0) del_count = 0;
    if (ins_count < 0) ins_count = 0;

    XrJsonValue *response = xlsp_json_new_object();
    xlsp_json_object_set(response, "resultId", xlsp_json_new_string(rid));

    XrJsonValue *edits_arr = xlsp_json_new_array();
    if (del_count > 0 || ins_count > 0) {
        XrJsonValue *edit = xlsp_json_new_object();
        xlsp_json_object_set(edit, "start", xlsp_json_new_number(first_diff));
        xlsp_json_object_set(edit, "deleteCount", xlsp_json_new_number(del_count));
        if (ins_count > 0) {
            XrJsonValue *ins_data = xlsp_json_new_array();
            for (int i = 0; i < ins_count; i++)
                xlsp_json_array_push(ins_data, xlsp_json_new_number(new_data[first_diff + i]));
            xlsp_json_object_set(edit, "data", ins_data);
        }
        xlsp_json_array_push(edits_arr, edit);
    }
    xlsp_json_object_set(response, "edits", edits_arr);

    xr_free(old_data);
    xr_free(new_data);
    return response;
}

static XrJsonValue *handle_semantic_tokens_range(XrLspServer *server, XrJsonValue *params) {
    XrJsonValue *textDocument = xlsp_json_get_object(params, "textDocument");
    XrJsonValue *range_obj = xlsp_json_get_object(params, "range");
    if (!textDocument) return xlsp_json_new_null();

    const char *uri = xlsp_json_get_string(textDocument, "uri");
    XrLspDocument *doc = xlsp_document_get(server, uri);
    if (!doc) return xlsp_json_new_null();

    int range_start_line = 0, range_end_line = 0x7FFFFFFF;
    if (range_obj) {
        XrJsonValue *start = xlsp_json_get_object(range_obj, "start");
        XrJsonValue *end = xlsp_json_get_object(range_obj, "end");
        if (start) range_start_line = xlsp_json_get_int(start, "line");
        if (end) range_end_line = xlsp_json_get_int(end, "line");
    }

    XlspSemanticTokensResult *all = xlsp_analyze_semantic_tokens(doc);
    if (!all || all->count == 0) {
        xlsp_semantic_tokens_free(all);
        return xlsp_json_new_null();
    }

    // Filter tokens to requested range
    XlspSemanticTokensResult filtered = { .tokens = all->tokens, .count = 0, .capacity = 0 };
    XlspSemanticToken *buf = xr_malloc(sizeof(XlspSemanticToken) * all->count);
    for (int i = 0; i < all->count; i++) {
        if (all->tokens[i].line >= range_start_line && all->tokens[i].line <= range_end_line) {
            buf[filtered.count++] = all->tokens[i];
        }
    }
    filtered.tokens = buf;

    XrJsonValue *response = xlsp_semantic_tokens_encode(&filtered);
    xr_free(buf);
    xlsp_semantic_tokens_free(all);

    return response;
}

static XrJsonValue *handle_inlay_hint(XrLspServer *server, XrJsonValue *params) {
    XrJsonValue *textDocument = xlsp_json_get_object(params, "textDocument");
    XrJsonValue *range_obj = xlsp_json_get_object(params, "range");
    if (!textDocument || !range_obj) return xlsp_json_new_array();

    const char *uri = xlsp_json_get_string(textDocument, "uri");
    XrLspDocument *doc = xlsp_document_get(server, uri);
    if (!doc) return xlsp_json_new_array();

    XrJsonValue *start = xlsp_json_get_object(range_obj, "start");
    XrJsonValue *end = xlsp_json_get_object(range_obj, "end");

    XrLspRange range = {
        .start = {
            .line = (uint32_t)xlsp_json_get_int(start, "line"),
            .character = (uint32_t)xlsp_json_get_int(start, "character")
        },
        .end = {
            .line = (uint32_t)xlsp_json_get_int(end, "line"),
            .character = (uint32_t)xlsp_json_get_int(end, "character")
        }
    };

    return xlsp_analyze_inlay_hints(server, doc, range);
}

// Folding range, code action, call/type hierarchy, highlight,
// workspace symbol, selection range, document link handlers
// are now in their own files. See xlsp_folding.c, xlsp_code_action.c,
// xlsp_call_hierarchy.c, xlsp_extra_handlers.c

