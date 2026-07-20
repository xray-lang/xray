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
#include "../../runtime/value/xtype_pool.h"
#include "../../toolchain/xcompiler_session.h"
#include "../../frontend/parser/xparse.h"
#include "../../base/xarena.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "../../base/xfileio.h"
#include "../../base/xhashmap.h"
#include "../../base/xmalloc.h"
#include "../../os/os_time.h"
#include "xlsp_analysis.h"
#include "xlsp_symbol_index.h"

// lsp_log declared in xlsp_server.h (included via xlsp_workspace.h)
// ============================================================================
// Background Indexing
// ============================================================================

#include "xlsp_async.h"
#include "xlsp_index_pool.h"
#include "../../os/os_dir.h"

static XrTypePool *workspace_compiler_analyzer_pool(XrVMRuntime *isolate) {
    XrCompilerSession *session = xr_compiler_session_current_for_isolate(isolate);
    return xr_compiler_session_analyzer_pool(session);
}

// Recursively find all .xr files in a directory (with configurable ignore rules)
static void find_xr_files_with_config(const char *dir_path, char ***files, int *count,
                                      int *capacity, XlspConfig *config) {
    XrDirIter *it = xr_dir_open(dir_path);
    if (!it)
        return;

    XrDirEntry e;
    while (xr_dir_next(it, &e)) {
        char path[XLSP_MAX_PATH];
        snprintf(path, sizeof(path), "%s/%s", dir_path, e.name);

        // Check ignore rules (handles hidden files, configured patterns)
        if (xlsp_config_should_ignore(config, e.name, e.is_dir)) {
            continue;
        }

        if (e.is_dir) {
            find_xr_files_with_config(path, files, count, capacity, config);
        } else {
            // Check if .xr file. xr_dir_next returns is_dir reliably; everything
            // else (regular file, symlink, etc.) is treated uniformly here so
            // a project that symlinks .xr files in still gets indexed.
            size_t len = strlen(e.name);
            if (len > 3 && strcmp(e.name + len - 3, ".xr") == 0) {
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

    xr_dir_close(it);
}

// Index a single file by path (for file watcher updates)
void xlsp_workspace_index_file(XrLspServer *server, const char *uri, const char *path) {
    if (!server || !server->workspace_analyzer || !server->isolate || !path)
        return;

    size_t read_size = 0;
    char *content = xr_file_read_all(path, "r", &read_size);
    if (!content)
        return;

    // Calculate content hash for incremental analysis
    uint64_t content_hash = xlsp_content_hash(content, read_size);

    // Parse and analyze
    XrTypePool *apool2 = workspace_compiler_analyzer_pool(server->isolate);
    if (apool2) {
        xr_type_set_current_pool(apool2, &apool2->next_type_id);
    }
    XrArena arena;
    xr_arena_init(&arena, 64 * 1024);
    XrCompilerSessionScope parse_scope;
    if (!xr_compiler_session_push_arena(xr_compiler_session_current_for_isolate(server->isolate),
                                        &arena, uri, &parse_scope)) {
        xr_arena_destroy(&arena);
        xr_free(content);
        return;
    }
    Parser parser;
    xr_parser_init(&parser, xr_compiler_session_current_for_isolate(server->isolate), content, uri,
                   &arena);
    AstNode *ast = xr_parse_recoverable(&parser);
    xr_compiler_session_pop_arena(&parse_scope);

    if (ast && !parser.had_error) {
        // Use incremental update with content hash for true change detection
        // This will:
        // 1. Skip re-analysis if content_hash unchanged
        // 2. Remove old symbols before adding new ones
        // 3. Propagate dirty flags to dependent files
        xa_analyzer_refresh_file(server->workspace_analyzer, uri, (XrAstNode *) ast, content_hash);
        lsp_log("Indexed file: %s (hash: %llx)", path, (unsigned long long) content_hash);
    }

    // Keep the shallow workspace symbol index current for this file (covers
    // watched-file create/change of unopened files).
    if (ast && server->symbol_index) {
        XrLspIndexSymbol *shallow = xlsp_index_symbols_from_ast(ast);
        xlsp_symbol_index_replace_file(server->symbol_index, uri, shallow);
        xlsp_index_symbol_free_list(shallow);
    }

    xr_arena_destroy(&arena);
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
        // Merge the worker's shallow symbols straight into the global index.
        // No main-thread re-parse/analyze: closed files are shallow-only
        // (full type analysis happens on demand when a file is opened).
        // ================================================================
        if (result->uri) {
            if (server->symbol_index) {
                xlsp_symbol_index_replace_file(server->symbol_index, result->uri, result->symbols);
            }
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

            // Report the shallow index size: under Approach A closed files live
            // in the workspace symbol index, not the (open-files-only) analyzer.
            int total_symbols = (int) xlsp_symbol_index_entry_count(server->symbol_index);

            lsp_log("[IndexPool] Background indexing complete: %d files, %d indexed symbols",
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

    // If indexing is already running, scan this root and hand its files to the
    // parallel pool so they get shallow-indexed alongside the current batch.
    if (server->indexing_in_progress) {
        int capacity = 64;
        char **files = xr_malloc(capacity * sizeof(char *));
        if (!files)
            return;
        int file_count = 0;

        find_xr_files_with_config(root_path, &files, &file_count, &capacity, &server->config);

        if (file_count > 0 && server->index_pool) {
            xlsp_index_pool_submit_batch(server->index_pool, files, file_count);
            server->files_total += file_count;
            lsp_log("[IndexPool] Queued %d more files from %s (indexing in progress)", file_count,
                    root_path);
        }

        for (int i = 0; i < file_count; i++)
            xr_free(files[i]);
        xr_free(files);
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

    // Drop shallow workspace-symbol entries for files under this folder.
    if (server->symbol_index) {
        xlsp_symbol_index_remove_prefix(server->symbol_index, path_prefix);
    }

    lsp_log("[Workspace] Purged state for prefix: %s (len=%zu)", path_prefix, prefix_len);
}

// ============================================================================
// Long-session analyzer memory bound
// ============================================================================
//
// xa_analyzer_refresh_file() frees the old XaSymbol structs on every edit, but
// the XrType objects they allocated came from the analyzer's type-pool arena,
// which can only be freed wholesale. Over a long editing session the pool grows
// monotonically (see xa_analyzer_type_pool_bytes). Rather than add a per-type
// refcount/ownership scheme to the whole type system, the LSP treats a full
// analyzer rebuild as a rare "garbage collection": drop the old analyzer, then
// re-analyze open documents immediately and repopulate closed files with a
// fresh background index. This reclaims all leaked types at once.
//
// The trigger is adaptive: rebuild when the live pool exceeds 2x the size it
// settled to after the last clean (re)build, floored so tiny projects never
// churn. Because the baseline is re-established only after the system settles
// post-rebuild, projects that legitimately need a large pool simply raise their
// own threshold instead of rebuilding in a loop.
#define XLSP_ANALYZER_POOL_FLOOR_BYTES (64u * 1024u * 1024u)  // 64 MiB

void xlsp_workspace_maybe_rebuild_analyzer(XrLspServer *server) {
    if (!server || !server->workspace_analyzer)
        return;

    // Only act when fully idle: a rebuild mid-index would race repopulation
    // and discard in-flight work.
    if (server->indexing_in_progress)
        return;
    if (server->index_pool && !xlsp_index_pool_is_idle(server->index_pool))
        return;

    size_t bytes = xa_analyzer_type_pool_bytes(server->workspace_analyzer);

    // Ignore everything below the floor: memory is cheap and rebuilds are not.
    if (bytes < XLSP_ANALYZER_POOL_FLOOR_BYTES)
        return;

    // First settled measurement over the floor establishes the clean baseline.
    if (server->analyzer_pool_baseline_bytes == 0) {
        server->analyzer_pool_baseline_bytes = bytes;
        return;
    }

    size_t trigger = server->analyzer_pool_baseline_bytes * 2;
    if (trigger < XLSP_ANALYZER_POOL_FLOOR_BYTES)
        trigger = XLSP_ANALYZER_POOL_FLOOR_BYTES;
    if (bytes <= trigger)
        return;

    lsp_log("[Workspace] Analyzer type pool %zu bytes > trigger %zu (baseline %zu) — rebuilding",
            bytes, trigger, server->analyzer_pool_baseline_bytes);

    XrCompilerSession *session = xr_compiler_session_current_for_isolate(server->isolate);
    XaAnalyzer *fresh = xa_analyzer_new(session);
    if (!fresh) {
        // Can't allocate a replacement; bump the baseline so we don't spin
        // retrying on every tick, and keep the (larger) current analyzer.
        lsp_log("[Workspace] Rebuild aborted: analyzer allocation failed");
        server->analyzer_pool_baseline_bytes = bytes;
        return;
    }

    xa_analyzer_free(server->workspace_analyzer);
    server->workspace_analyzer = fresh;

    // Re-analyze open documents right away so active editing keeps working
    // against a populated analyzer. The shallow symbol index survives the
    // rebuild untouched (it holds no type-pool memory), so workspace-wide
    // features keep working; the fresh background index below refreshes it.
    if (server->doc_table) {
        XrLspDocTable *table = server->doc_table;
        for (int i = 0; i < table->bucket_count; i++) {
            for (XrLspDocBucket *bucket = table->buckets[i]; bucket; bucket = bucket->next) {
                XrLspDocument *doc = bucket->doc;
                if (doc && doc->content) {
                    doc->dirty = true;
                    xlsp_parse_document(doc, server);
                }
            }
        }
    }

    // Repopulate closed workspace files in the background (mirrors startup).
    if (server->workspace_folder_count > 0) {
        const char *roots[MAX_WORKSPACE_FOLDERS];
        int root_count = 0;
        for (int i = 0; i < server->workspace_folder_count && root_count < MAX_WORKSPACE_FOLDERS;
             i++) {
            if (server->workspace_folders[i].path)
                roots[root_count++] = server->workspace_folders[i].path;
        }
        if (root_count > 0)
            xlsp_workspace_start_background_index_roots(server, roots, root_count);
    }

    // Re-establish the baseline only after the system settles again: leave it
    // 0 so the idle guards above defer the next measurement until the fresh
    // index + drain complete.
    server->analyzer_pool_baseline_bytes = 0;
    lsp_log("[Workspace] Analyzer rebuilt; baseline will re-establish once idle");
}

// ============================================================================
// Analyzer working-set eviction
// ============================================================================
//
// Post-Approach-A the workspace analyzer only holds open files + the files they
// directly import on demand. When a document is closed (or an edit drops an
// import), the previously-pulled files linger. Evicting them frees their
// symbols and — more importantly — stops them being re-analyzed, which slows
// type-pool growth so the rebuild valve fires even less often.
//
// This is intentionally cheap and self-healing: the live set is "open docs +
// their current local imports" (exactly how xlsp_parse_document/
// index_imports_on_demand populates the analyzer). If we ever evict a file that
// is still needed, the next parse of its importer re-pulls it — over-eviction
// costs a re-parse, never correctness. Synthetic (non file://) entries such as
// prelude/stdlib are never touched.
void xlsp_workspace_evict_unreferenced_files(XrLspServer *server) {
    if (!server || !server->workspace_analyzer)
        return;
    // Don't race the background indexer's on-demand analysis.
    if (server->indexing_in_progress)
        return;

    XaAnalyzer *analyzer = server->workspace_analyzer;
    if (!analyzer->files)
        return;

    XrHashMap *live = xr_hashmap_new();
    if (!live)
        return;

    // Owned import-URI strings kept alive until eviction finishes (the hash map
    // borrows keys). Open-doc URIs are owned by their documents and stable here.
    char **owned = NULL;
    int owned_count = 0, owned_cap = 0;

    XrLspDocTable *table = server->doc_table;
    if (table) {
        for (int i = 0; i < table->bucket_count; i++) {
            for (XrLspDocBucket *b = table->buckets[i]; b; b = b->next) {
                XrLspDocument *doc = b->doc;
                if (!doc || !doc->uri)
                    continue;
                xr_hashmap_set(live, doc->uri, (void *) (uintptr_t) 1);
                if (!doc->content)
                    continue;
                XlspImportInfo *imports = xlsp_parse_imports(doc->content, doc->uri);
                for (XlspImportInfo *imp = imports; imp; imp = imp->next) {
                    if (imp->type != XLSP_IMPORT_LOCAL || !imp->resolved_path)
                        continue;
                    char uri[XLSP_MAX_PATH + 8];
                    snprintf(uri, sizeof(uri), "file://%s", imp->resolved_path);
                    char *key = xr_strdup(uri);
                    if (!key)
                        continue;
                    if (owned_count >= owned_cap) {
                        int nc = owned_cap ? owned_cap * 2 : 16;
                        char **tmp = xr_realloc(owned, (size_t) nc * sizeof(char *));
                        if (!tmp) {
                            xr_free(key);
                            continue;
                        }
                        owned = tmp;
                        owned_cap = nc;
                    }
                    owned[owned_count++] = key;
                    xr_hashmap_set(live, key, (void *) (uintptr_t) 1);
                }
                xlsp_free_imports(imports);
            }
        }
    }

    // Collect analyzer file keys not in the live set (copy the keys: removal
    // frees the entry that owns them).
    char **doomed = NULL;
    int doomed_count = 0;
    int file_count = analyzer->file_count > 0 ? analyzer->file_count : 0;
    if (file_count > 0)
        doomed = xr_malloc((size_t) file_count * sizeof(char *));
    if (doomed) {
        for (XaFileEntry *e = analyzer->files; e; e = e->next) {
            if (!e->path)
                continue;
            // Only touch real workspace files; never prelude/stdlib/synthetic.
            if (strncmp(e->path, "file://", 7) != 0)
                continue;
            if (xr_hashmap_has(live, e->path))
                continue;
            if (doomed_count < file_count)
                doomed[doomed_count++] = xr_strdup(e->path);
        }
        for (int i = 0; i < doomed_count; i++) {
            if (!doomed[i])
                continue;
            xa_analyzer_remove_file(analyzer, doomed[i]);
            xr_free(doomed[i]);
        }
        xr_free(doomed);
    }

    if (doomed_count > 0)
        lsp_log("[Workspace] Evicted %d unreferenced analyzer file(s)", doomed_count);

    for (int i = 0; i < owned_count; i++)
        xr_free(owned[i]);
    xr_free(owned);
    xr_hashmap_free(live);
}
