/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_workspace.c - Workspace background indexing and file scanning
 */

#include "xlsp_workspace.h"
#include "xlsp_cache.h"
#include "xlsp_imports.h"
#include "xlsp_utils.h"
#include "../../frontend/parser/xast_nodes.h"
#include "../../frontend/parser/xast_api.h"
#include "../../frontend/analyzer/xanalyzer.h"
#include "../../runtime/xisolate_api.h"
#include "../../runtime/value/xtype_pool.h"
#include "../../frontend/parser/xparse.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <time.h>
#include "../../base/xmalloc.h"

// lsp_log declared in xlsp_server.h (included via xlsp_workspace.h)
// ============================================================================
// Background Indexing
// ============================================================================

#include "xlsp_async.h"
#include "xlsp_index_pool.h"
#include <dirent.h>
#include <sys/stat.h>

// Recursively find all .xr files in a directory (with configurable ignore rules)
static void find_xr_files_with_config(const char *dir_path, char ***files, int *count,
                                      int *capacity, XlspConfig *config) {
    DIR *dir = opendir(dir_path);
    if (!dir)
        return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        char path[XLSP_MAX_PATH];
        snprintf(path, sizeof(path), "%s/%s", dir_path, entry->d_name);

        struct stat st;
        if (stat(path, &st) < 0)
            continue;

        bool is_dir = S_ISDIR(st.st_mode);

        // Check ignore rules (handles hidden files, configured patterns)
        if (xlsp_config_should_ignore(config, entry->d_name, is_dir)) {
            continue;
        }

        if (is_dir) {
            find_xr_files_with_config(path, files, count, capacity, config);
        } else if (S_ISREG(st.st_mode)) {
            // Check if .xr file
            size_t len = strlen(entry->d_name);
            if (len > 3 && strcmp(entry->d_name + len - 3, ".xr") == 0) {
                if (*count >= *capacity) {
                    int new_capacity = *capacity * 2;
                    // Overflow check
                    if (new_capacity < *capacity)
                        continue;

                    char **new_files = xr_realloc(*files, new_capacity * sizeof(char *));
                    if (!new_files)
                        continue;  // Skip this file on failure

                    *files = new_files;
                    *capacity = new_capacity;
                }
                char *path_copy = xr_strdup(path);
                if (path_copy) {
                    (*files)[(*count)++] = path_copy;
                }
            }
        }
    }

    closedir(dir);
}

// Simplified wrapper without custom ignore rules
static void find_xr_files(const char *dir_path, char ***files, int *count, int *capacity) {
    // Use empty config - only hidden files will be ignored
    XlspConfig empty_config = {0};
    find_xr_files_with_config(dir_path, files, count, capacity, &empty_config);
}

// Background task: scan and index files
void xlsp_workspace_index_task_execute(void *data) {
    XrLspIndexTaskData *task_data = (XrLspIndexTaskData *) data;

    // Find all .xr files using server's ignore configuration
    int capacity = 64;
    task_data->files = xr_malloc(capacity * sizeof(char *));
    task_data->file_count = 0;

    if (task_data->server) {
        find_xr_files_with_config(task_data->root_path, &task_data->files, &task_data->file_count,
                                  &capacity, &task_data->server->config);
    } else {
        find_xr_files(task_data->root_path, &task_data->files, &task_data->file_count, &capacity);
    }
}

// Completion: update server state (runs in main thread)
// Index all files in the scanned directory (typically current file's directory)
// On-demand import indexing handles cross-directory dependencies

// Index a single file (helper function)
static void index_single_file(XrLspServer *server, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f)
        return;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *content = xr_malloc(size + 1);
    if (!content) {
        fclose(f);
        return;
    }

    size_t read_size = fread(content, 1, size, f);
    content[read_size] = '\0';
    fclose(f);

    char uri[1100];
    snprintf(uri, sizeof(uri), "file://%s", path);

    if (server->workspace_analyzer && server->isolate) {
        XrTypePool *apool = xr_isolate_get_analyzer_pool(server->isolate);
        if (apool) {
            xr_type_set_current_pool(apool, &apool->next_type_id);
        }
        Parser parser;
        xr_parser_init(&parser, server->isolate, content, uri, NULL);
        AstNode *ast = xr_parse_recoverable(&parser);

        if (ast && !parser.had_error) {
            xa_analyzer_analyze(server->workspace_analyzer, uri, (XrAstNode *) ast);
        }
        if (ast) {
            xr_program_destroy(ast);
        }
    }

    xr_free(content);
}

// Task for incremental batch indexing
static void index_batch_execute(void *data) {
    // Nothing to do in worker thread - just a placeholder
    (void) data;
}

static void index_batch_complete(void *data, void *result) {
    (void) result;
    XrLspIndexTaskData *task_data = (XrLspIndexTaskData *) data;
    XrLspServer *server = task_data->server;

    // Check if indexing was cancelled
    if (server->index_cancelled) {
        lsp_log("Background indexing cancelled at %d/%d files", task_data->current_file,
                task_data->file_count);

        // Cleanup remaining files
        for (int i = task_data->current_file; i < task_data->file_count; i++) {
            xr_free(task_data->files[i]);
        }
        xr_free(task_data->files);
        xr_free(task_data->root_path);
        server->indexing_in_progress = false;
        server->index_task_data = NULL;
        server->index_cancelled = false;  // Reset for next indexing
        xr_free(task_data);
        return;
    }

// Process a small batch of files (max 3 per cycle to keep UI responsive)
#define BATCH_SIZE 3
    int processed = 0;

    while (task_data->current_file < task_data->file_count && processed < BATCH_SIZE) {
        // Check cancellation between files
        if (server->index_cancelled)
            break;

        char *path = task_data->files[task_data->current_file];
        index_single_file(server, path);
        xr_free(path);
        task_data->current_file++;
        server->files_indexed++;
        processed++;
    }

    // Check if done or cancelled
    if (task_data->current_file >= task_data->file_count || server->index_cancelled) {
        if (server->index_cancelled) {
            // Cleanup remaining files
            for (int i = task_data->current_file; i < task_data->file_count; i++) {
                xr_free(task_data->files[i]);
            }
            lsp_log("Background indexing cancelled at %d/%d files", server->files_indexed,
                    server->files_total);
            server->index_cancelled = false;
        } else {
            int symbol_count = 0;
            if (server->workspace_analyzer && server->workspace_analyzer->global_scope) {
                XaSymbol **syms = xa_scope_get_all_symbols(server->workspace_analyzer->global_scope,
                                                           &symbol_count);
                xr_free(syms);
            }

            lsp_log("Background indexing complete: %d/%d files, %d symbols", server->files_indexed,
                    server->files_total, symbol_count);
        }

        // All files indexed or cancelled
        xr_free(task_data->files);
        xr_free(task_data->root_path);
        server->indexing_in_progress = false;
        server->index_task_data = NULL;
        xr_free(task_data);
    } else {
        // Schedule next batch - this allows main thread to process other requests
        XrLspTask *task =
            xlsp_task_new_ex(index_batch_execute, index_batch_complete, task_data,
                             XLSP_TASK_LOW,  // Low priority so user requests take precedence
                             XLSP_TASK_TYPE_INDEX,
                             0  // No associated request ID
            );
        xlsp_async_submit(server->async, task);
    }
}

void xlsp_workspace_index_task_complete(void *data, void *result) {
    (void) result;
    XrLspIndexTaskData *task_data = (XrLspIndexTaskData *) data;
    XrLspServer *server = task_data->server;

    server->files_total = task_data->file_count;
    server->files_indexed = 0;
    task_data->current_file = 0;

    // Store task data for incremental processing
    server->index_task_data = task_data;

    lsp_log("Starting incremental indexing: %d files", task_data->file_count);

    // Start incremental indexing - process first batch
    index_batch_complete(task_data, NULL);
}

// Index a single file by path (for file watcher updates)
void xlsp_workspace_index_file(XrLspServer *server, const char *uri, const char *path) {
    if (!server || !server->workspace_analyzer || !server->isolate || !path)
        return;

    FILE *f = fopen(path, "r");
    if (!f)
        return;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *content = xr_malloc(size + 1);
    if (!content) {
        fclose(f);
        return;
    }

    size_t read_size = fread(content, 1, size, f);
    content[read_size] = '\0';
    fclose(f);

    // Calculate content hash for incremental analysis
    uint64_t content_hash = xlsp_content_hash(content, read_size);

    // Parse and analyze
    XrTypePool *apool2 = xr_isolate_get_analyzer_pool(server->isolate);
    if (apool2) {
        xr_type_set_current_pool(apool2, &apool2->next_type_id);
    }
    Parser parser;
    xr_parser_init(&parser, server->isolate, content, uri, NULL);
    AstNode *ast = xr_parse_recoverable(&parser);

    if (ast && !parser.had_error) {
        // Use incremental update with content hash for true change detection
        // This will:
        // 1. Skip re-analysis if content_hash unchanged
        // 2. Remove old symbols before adding new ones
        // 3. Propagate dirty flags to dependent files
        xa_analyzer_refresh_file(server->workspace_analyzer, uri, (XrAstNode *) ast, content_hash);
        lsp_log("Indexed file: %s (hash: %llx)", path, (unsigned long long) content_hash);
    }
    if (ast) {
        xr_program_destroy(ast);
    }

    xr_free(content);
}

// ============================================================================
// Multi-Isolate Parallel Indexing (New)
// ============================================================================

// Merge index results into workspace analyzer (called from main thread)
// This performs the actual symbol merging by re-analyzing files in the main thread
void xlsp_workspace_merge_index_results(XrLspServer *server, XrLspIndexResult *results) {
    if (!server || !results)
        return;

    // Check if indexing was cancelled
    if (server->index_cancelled) {
        lsp_log("[IndexPool] Indexing was cancelled, discarding results");
        // End progress with cancellation message
        if (server->index_progress_token) {
            xlsp_progress_end(server, server->index_progress_token, "Indexing cancelled");
            xr_free(server->index_progress_token);
            server->index_progress_token = NULL;
        }
        server->indexing_in_progress = false;
        return;
    }

    int merged_count = 0;
    int error_count = 0;
    int symbols_added = 0;

    for (XrLspIndexResult *result = results; result; result = result->next) {
        // Check for cancellation during merge
        if (server->index_cancelled) {
            lsp_log("[IndexPool] Indexing cancelled during merge");
            break;
        }

        if (!result->success) {
            if (result->error_message) {
                lsp_log("[IndexPool] Failed to index %s: %s",
                        result->path ? result->path : "unknown", result->error_message);
            }
            error_count++;
            continue;
        }

        // ================================================================
        // Enqueue for budgeted main-thread analysis instead of
        // blocking the merge loop with synchronous per-file I/O.
        // ================================================================
        if (result->path && result->uri) {
            xlsp_enqueue_analysis(server, result->uri, result->path);

            // Count symbols from worker-side shallow extraction
            for (XrLspIndexSymbol *sym = result->symbols; sym; sym = sym->next) {
                symbols_added++;
            }
        }

        server->files_indexed++;
        merged_count++;
    }

    if (merged_count > 0 || error_count > 0) {
        lsp_log("[IndexPool] Merged %d files (%d errors, %d symbols), total: %d/%d", merged_count,
                error_count, symbols_added, server->files_indexed, server->files_total);

        // Update progress
        if (server->index_progress_token && server->files_total > 0) {
            int percentage = (server->files_indexed * 100) / server->files_total;
            char msg[128];
            snprintf(msg, sizeof(msg), "Indexed %d/%d files (%d symbols)", server->files_indexed,
                     server->files_total, symbols_added);
            xlsp_progress_report(server, server->index_progress_token, msg, percentage);
        }
    }

    // Check if indexing is complete
    if (server->index_pool && xlsp_index_pool_is_idle(server->index_pool)) {
        int submitted, completed;
        xlsp_index_pool_get_progress(server->index_pool, &submitted, &completed);

        if (submitted > 0 && submitted == completed) {
            server->indexing_in_progress = false;

            // Log final symbol count
            int total_symbols = 0;
            if (server->workspace_analyzer && server->workspace_analyzer->global_scope) {
                XaSymbol **syms = xa_scope_get_all_symbols(server->workspace_analyzer->global_scope,
                                                           &total_symbols);
                xr_free(syms);
            }

            lsp_log("[IndexPool] Background indexing complete: %d files, %d global symbols",
                    completed, total_symbols);

            // End progress with success message
            if (server->index_progress_token) {
                char msg[128];
                snprintf(msg, sizeof(msg), "Indexed %d files (%d symbols)", completed,
                         total_symbols);
                xlsp_progress_end(server, server->index_progress_token, msg);
                xr_free(server->index_progress_token);
                server->index_progress_token = NULL;
            }
        }
    }
}

// Poll index pool and process results (call from main loop)
void xlsp_workspace_poll_index_results(XrLspServer *server) {
    if (!server || !server->index_pool)
        return;

    XrLspIndexResult *results = xlsp_index_pool_poll(server->index_pool);
    if (results) {
        xlsp_workspace_merge_index_results(server, results);
        xlsp_index_result_free_list(results);
    }
}

// Get index pool notify fd (for select/poll)
int xlsp_workspace_get_index_notify_fd(XrLspServer *server) {
    if (!server || !server->index_pool)
        return -1;
    return xlsp_index_pool_get_notify_fd(server->index_pool);
}

// Start background workspace indexing (using parallel index pool)
void xlsp_workspace_start_background_index(XrLspServer *server, const char *root_path) {
    if (!server || !root_path)
        return;

    // If indexing is already running, scan files and enqueue them into the
    // pending analysis queue so they get processed after the current batch.
    if (server->indexing_in_progress) {
        int capacity = 64;
        char **files = xr_malloc(capacity * sizeof(char *));
        if (!files)
            return;
        int file_count = 0;

        find_xr_files_with_config(root_path, &files, &file_count, &capacity, &server->config);

        for (int i = 0; i < file_count; i++) {
            char uri[XLSP_MAX_PATH + 8];
            snprintf(uri, sizeof(uri), "file://%s", files[i]);
            xlsp_enqueue_analysis(server, uri, files[i]);
            xr_free(files[i]);
        }
        xr_free(files);

        if (file_count > 0) {
            lsp_log("[IndexPool] Queued %d files from %s (indexing in progress)", file_count,
                    root_path);
        }
        return;
    }

    // Create index pool if not exists
    if (!server->index_pool) {
        server->index_pool = xlsp_index_pool_new(server);
        if (!server->index_pool) {
            lsp_log("[IndexPool] Failed to create index pool, falling back to sync");
            return;
        }
    }

    server->indexing_in_progress = true;
    server->index_cancelled = false;
    server->files_indexed = 0;

    // Find all .xr files (using config-aware ignore rules)
    int capacity = 64;
    char **files = xr_malloc(capacity * sizeof(char *));
    if (!files) {
        server->indexing_in_progress = false;
        return;
    }
    int file_count = 0;

    find_xr_files_with_config(root_path, &files, &file_count, &capacity, &server->config);

    server->files_total = file_count;

    if (file_count > 0) {
        // Start progress reporting (cancellable)
        if (server->index_progress_token) {
            xr_free(server->index_progress_token);
        }
        char msg[128];
        snprintf(msg, sizeof(msg), "Indexing %d files...", file_count);
        server->index_progress_token = xlsp_progress_begin(server, "Indexing Workspace", msg, true);

        // Submit all files to index pool
        xlsp_index_pool_submit_batch(server->index_pool, files, file_count);
        lsp_log("[IndexPool] Submitted %d files for parallel indexing: %s", file_count, root_path);
    } else {
        server->indexing_in_progress = false;
        lsp_log("[IndexPool] No .xr files found in: %s", root_path);
    }

    // Free file paths (they were copied by submit_batch)
    for (int i = 0; i < file_count; i++) {
        xr_free(files[i]);
    }
    xr_free(files);
}

// Start background indexing for multiple roots (scanned into one batch)
void xlsp_workspace_start_background_index_roots(XrLspServer *server, const char **roots,
                                                 int root_count) {
    if (!server || !roots || root_count <= 0)
        return;
    if (server->indexing_in_progress)
        return;

    // Create index pool if not exists
    if (!server->index_pool) {
        server->index_pool = xlsp_index_pool_new(server);
        if (!server->index_pool) {
            lsp_log("[IndexPool] Failed to create index pool");
            return;
        }
    }

    server->indexing_in_progress = true;
    server->index_cancelled = false;
    server->files_indexed = 0;

    // Collect .xr files from all roots into a single batch
    int capacity = 64;
    char **files = xr_malloc(capacity * sizeof(char *));
    if (!files) {
        server->indexing_in_progress = false;
        return;
    }
    int file_count = 0;

    for (int r = 0; r < root_count; r++) {
        if (roots[r]) {
            find_xr_files_with_config(roots[r], &files, &file_count, &capacity, &server->config);
        }
    }

    server->files_total = file_count;

    if (file_count > 0) {
        if (server->index_progress_token) {
            xr_free(server->index_progress_token);
        }
        char msg[128];
        snprintf(msg, sizeof(msg), "Indexing %d files from %d roots...", file_count, root_count);
        server->index_progress_token = xlsp_progress_begin(server, "Indexing Workspace", msg, true);

        xlsp_index_pool_submit_batch(server->index_pool, files, file_count);
        lsp_log("[IndexPool] Submitted %d files from %d roots", file_count, root_count);
    } else {
        server->indexing_in_progress = false;
        lsp_log("[IndexPool] No .xr files found in %d roots", root_count);
    }

    for (int i = 0; i < file_count; i++) {
        xr_free(files[i]);
    }
    xr_free(files);
}

// ============================================================================
// Pending Analysis Queue — budgeted main-thread drain
// ============================================================================

void xlsp_enqueue_analysis(XrLspServer *server, const char *uri, const char *path) {
    if (!server || !uri || !path)
        return;

    // O(n) dedup — queue is typically small (<100 entries per batch)
    for (int i = 0; i < server->pending_analysis_count; i++) {
        if (strcmp(server->pending_analysis[i].path, path) == 0) {
            return;  // Already queued
        }
    }

    // Grow if needed
    if (server->pending_analysis_count >= server->pending_analysis_capacity) {
        int new_cap =
            server->pending_analysis_capacity == 0 ? 32 : server->pending_analysis_capacity * 2;
        XlspPendingAnalysis *tmp =
            xr_realloc(server->pending_analysis, (size_t) new_cap * sizeof(XlspPendingAnalysis));
        if (!tmp)
            return;
        server->pending_analysis = tmp;
        server->pending_analysis_capacity = new_cap;
    }

    int idx = server->pending_analysis_count++;
    server->pending_analysis[idx].uri = xr_strdup(uri);
    server->pending_analysis[idx].path = xr_strdup(path);
}

int xlsp_drain_pending_analysis(XrLspServer *server) {
    if (!server || server->pending_analysis_count == 0)
        return 0;

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now_ms = (uint64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    uint64_t deadline = now_ms + ANALYSIS_DRAIN_BUDGET_MS;
    int processed = 0;

    while (server->pending_analysis_count > 0) {
        // Pop from front
        XlspPendingAnalysis entry = server->pending_analysis[0];

        // Shift remaining entries (could be optimised with ring buffer later)
        server->pending_analysis_count--;
        for (int i = 0; i < server->pending_analysis_count; i++) {
            server->pending_analysis[i] = server->pending_analysis[i + 1];
        }

        // Analyse on main thread
        xlsp_workspace_index_file(server, entry.uri, entry.path);
        xr_free(entry.uri);
        xr_free(entry.path);
        processed++;

        // Respect time budget
        clock_gettime(CLOCK_MONOTONIC, &ts);
        now_ms = (uint64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
        if (now_ms >= deadline)
            break;
    }

    if (processed > 0) {
        lsp_log("[Drain] Analysed %d files, %d remaining", processed,
                server->pending_analysis_count);
    }
    return processed;
}

void xlsp_free_pending_analysis(XrLspServer *server) {
    if (!server)
        return;
    for (int i = 0; i < server->pending_analysis_count; i++) {
        xr_free(server->pending_analysis[i].uri);
        xr_free(server->pending_analysis[i].path);
    }
    xr_free(server->pending_analysis);
    server->pending_analysis = NULL;
    server->pending_analysis_count = 0;
    server->pending_analysis_capacity = 0;
}

// Purge all analyzer/cache state for files under a path prefix
void xlsp_workspace_purge_prefix(XrLspServer *server, const char *path_prefix) {
    if (!server || !path_prefix)
        return;

    size_t prefix_len = strlen(path_prefix);
    if (prefix_len == 0)
        return;

    // Remove matching files from workspace analyzer
    if (server->workspace_analyzer) {
        // Get list of indexed files and remove those matching prefix
        // The analyzer tracks files by path; iterate and remove matches
        // Note: xa_analyzer_remove_file expects file path
        // We scan indexed_files from the old workspace index if available,
        // but the primary authority is the analyzer itself.
        // For now, log the purge — the analyzer remove_file API works
        // per-file, so callers need the file list.
        lsp_log("[Workspace] Purging analyzer state for prefix: %s", path_prefix);
    }

    // Invalidate exports cache for files under this prefix
    xlsp_exports_cache_remove_prefix(server, path_prefix);

    lsp_log("[Workspace] Purged state for prefix: %s (len=%zu)", path_prefix, prefix_len);
}
