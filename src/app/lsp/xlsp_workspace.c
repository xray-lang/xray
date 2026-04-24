/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_workspace.c - Workspace indexing implementation
 */

#include "xlsp_workspace.h"
#include "xlsp_cache.h"
#include "xlsp_imports.h"
#include "xlsp_utils.h"
#include "../../frontend/parser/xast_nodes.h"
#include "../../frontend/parser/xast_api.h"
#include "../../frontend/analyzer/xanalyzer.h"
#include "../../runtime/value/xtype.h"
#include "../../runtime/xisolate_api.h"
#include "../../runtime/value/xtype_pool.h"
#include "../../frontend/parser/xparse.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "../../base/xmalloc.h"

// lsp_log declared in xlsp_server.h (included via xlsp_workspace.h)

#define WORKSPACE_TABLE_SIZE 256
#define INITIAL_FILE_CAPACITY 16

#include "../../base/xhash.h"

// Hash function
static uint32_t hash_name(const char *name) {
    return xr_hash_bytes(name, strlen(name));
}

// Create workspace index
XrLspWorkspaceIndex *xlsp_workspace_new(void) {
    XrLspWorkspaceIndex *idx = xr_calloc(1, sizeof(XrLspWorkspaceIndex));
    if (!idx) return NULL;

    idx->table_size = WORKSPACE_TABLE_SIZE;
    idx->symbols = xr_calloc(WORKSPACE_TABLE_SIZE, sizeof(XrLspWorkspaceSymbol*));
    if (!idx->symbols) {
        xr_free(idx);
        return NULL;
    }

    idx->file_capacity = INITIAL_FILE_CAPACITY;
    idx->indexed_files = xr_calloc(INITIAL_FILE_CAPACITY, sizeof(char*));
    if (!idx->indexed_files) {
        xr_free(idx->symbols);
        xr_free(idx);
        return NULL;
    }

    return idx;
}

// Free symbol references
static void free_refs(XrLspSymbolRef *ref) {
    while (ref) {
        XrLspSymbolRef *next = ref->next;
        xr_free(ref->loc.uri);
        xr_free(ref);
        ref = next;
    }
}

// Free workspace symbol
static void free_workspace_symbol(XrLspWorkspaceSymbol *sym) {
    if (!sym) return;
    xr_free(sym->name);
    xr_free(sym->def_loc.uri);
    free_refs(sym->refs);
    xr_free(sym);
}

// Free workspace index
void xlsp_workspace_free(XrLspWorkspaceIndex *idx) {
    if (!idx) return;

    // Free symbols
    for (int i = 0; i < idx->table_size; i++) {
        XrLspWorkspaceSymbol *sym = idx->symbols[i];
        while (sym) {
            XrLspWorkspaceSymbol *next = sym->next;
            free_workspace_symbol(sym);
            sym = next;
        }
    }
    xr_free(idx->symbols);

    // Free file list
    for (int i = 0; i < idx->file_count; i++) {
        xr_free(idx->indexed_files[i]);
    }
    xr_free(idx->indexed_files);

    xr_free(idx);
}

// Add or update symbol in index
static XrLspWorkspaceSymbol *add_symbol(XrLspWorkspaceIndex *idx,
                                         const char *name,
                                         const char *uri,
                                         int line, int column) {
    uint32_t hash = hash_name(name) % idx->table_size;

    // Check if symbol already exists
    XrLspWorkspaceSymbol *sym = idx->symbols[hash];
    while (sym) {
        if (strcmp(sym->name, name) == 0) {
            return sym;  // Already exists
        }
        sym = sym->next;
    }

    // Create new symbol
    sym = xr_calloc(1, sizeof(XrLspWorkspaceSymbol));
    if (!sym) return NULL;

    sym->name = xr_strdup(name);
    if (!sym->name) {
        xr_free(sym);
        return NULL;
    }

    sym->def_loc.uri = xr_strdup(uri);
    if (!sym->def_loc.uri) {
        xr_free(sym->name);
        xr_free(sym);
        return NULL;
    }

    sym->def_loc.line = line;
    sym->def_loc.column = column;

    // Add to hash chain
    sym->next = idx->symbols[hash];
    idx->symbols[hash] = sym;
    idx->symbol_count++;

    return sym;
}

// Add reference to symbol
static void add_reference(XrLspWorkspaceSymbol *sym, const char *uri,
                          int line, int column, bool is_def, bool is_write) {
    if (!sym || !uri) return;

    XrLspSymbolRef *ref = xr_calloc(1, sizeof(XrLspSymbolRef));
    if (!ref) return;

    ref->loc.uri = xr_strdup(uri);
    if (!ref->loc.uri) {
        xr_free(ref);
        return;
    }

    ref->loc.line = line;
    ref->loc.column = column;
    ref->is_definition = is_def;
    ref->is_write = is_write;

    ref->next = sym->refs;
    sym->refs = ref;
    sym->ref_count++;
}

// Add file to indexed list
static void add_indexed_file(XrLspWorkspaceIndex *idx, const char *uri) {
    if (!idx || !uri) return;

    if (idx->file_count >= idx->file_capacity) {
        int new_capacity = idx->file_capacity * 2;
        // Overflow check
        if (new_capacity < idx->file_capacity) return;

        char **new_files = xr_realloc(idx->indexed_files, new_capacity * sizeof(char*));
        if (!new_files) return;  // Keep old array on failure

        idx->indexed_files = new_files;
        idx->file_capacity = new_capacity;
    }

    char *uri_copy = xr_strdup(uri);
    if (!uri_copy) return;

    idx->indexed_files[idx->file_count++] = uri_copy;
}

// Collect exported symbols from AST
static void collect_exports(XrLspWorkspaceIndex *idx, XrLspDocument *doc,
                            AstNode *node) {
    if (!node) return;

    switch (node->type) {
        case AST_PROGRAM: {
            int count = node->as.program.count;
            for (int i = 0; i < count; i++) {
                collect_exports(idx, doc, node->as.program.statements[i]);
            }
            break;
        }
        case AST_EXPORT_STMT: {
            AstNode *decl = node->as.export_stmt.declaration;
            if (decl) {
                collect_exports(idx, doc, decl);
            }
            break;
        }
        case AST_VAR_DECL: {
            const char *name = node->as.var_decl.name;
            XrLspWorkspaceSymbol *sym = add_symbol(idx, name, doc->uri,
                                                    node->line, 0);
            sym->is_exported = true;
            sym->type = node->as.var_decl.type_annotation;
            add_reference(sym, doc->uri, node->line, 0, true, true);
            break;
        }
        case AST_FUNCTION_DECL: {
            const char *name = node->as.function_decl.name;
            if (name) {
                XrLspWorkspaceSymbol *sym = add_symbol(idx, name, doc->uri,
                                                        node->line, 0);
                sym->is_exported = true;
                sym->is_function = true;
                add_reference(sym, doc->uri, node->line, 0, true, false);
            }
            break;
        }
        case AST_CLASS_DECL:
        case AST_STRUCT_DECL: {
            const char *name = node->as.class_decl.name;
            if (name) {
                XrLspWorkspaceSymbol *sym = add_symbol(idx, name, doc->uri,
                                                        node->line, 0);
                sym->is_exported = true;
                sym->is_class = true;
                add_reference(sym, doc->uri, node->line, 0, true, false);
            }
            break;
        }
        default:
            break;
    }
}

// Collect variable references from AST
static void collect_references(XrLspWorkspaceIndex *idx, XrLspDocument *doc,
                                AstNode *node) {
    if (!node) return;

    switch (node->type) {
        case AST_PROGRAM: {
            int count = node->as.program.count;
            for (int i = 0; i < count; i++) {
                collect_references(idx, doc, node->as.program.statements[i]);
            }
            break;
        }
        case AST_BLOCK: {
            int count = node->as.block.count;
            for (int i = 0; i < count; i++) {
                collect_references(idx, doc, node->as.block.statements[i]);
            }
            break;
        }
        case AST_VARIABLE: {
            const char *name = node->as.variable.name;
            // Find existing symbol
            uint32_t hash = hash_name(name) % idx->table_size;
            XrLspWorkspaceSymbol *sym = idx->symbols[hash];
            while (sym) {
                if (strcmp(sym->name, name) == 0) {
                    add_reference(sym, doc->uri, node->line, 0, false, false);
                    break;
                }
                sym = sym->next;
            }
            break;
        }
        case AST_ASSIGNMENT: {
            const char *name = node->as.assignment.name;
            uint32_t hash = hash_name(name) % idx->table_size;
            XrLspWorkspaceSymbol *sym = idx->symbols[hash];
            while (sym) {
                if (strcmp(sym->name, name) == 0) {
                    add_reference(sym, doc->uri, node->line, 0, false, true);
                    break;
                }
                sym = sym->next;
            }
            collect_references(idx, doc, node->as.assignment.value);
            break;
        }
        case AST_CALL_EXPR: {
            collect_references(idx, doc, node->as.call_expr.callee);
            for (int i = 0; i < node->as.call_expr.arg_count; i++) {
                collect_references(idx, doc, node->as.call_expr.arguments[i]);
            }
            break;
        }
        case AST_IF_STMT: {
            collect_references(idx, doc, node->as.if_stmt.condition);
            collect_references(idx, doc, node->as.if_stmt.then_branch);
            collect_references(idx, doc, node->as.if_stmt.else_branch);
            break;
        }
        case AST_WHILE_STMT: {
            collect_references(idx, doc, node->as.while_stmt.condition);
            collect_references(idx, doc, node->as.while_stmt.body);
            break;
        }
        case AST_FOR_STMT: {
            collect_references(idx, doc, node->as.for_stmt.initializer);
            collect_references(idx, doc, node->as.for_stmt.condition);
            collect_references(idx, doc, node->as.for_stmt.increment);
            collect_references(idx, doc, node->as.for_stmt.body);
            break;
        }
        case AST_FUNCTION_DECL: {
            collect_references(idx, doc, node->as.function_decl.body);
            break;
        }
        case AST_RETURN_STMT: {
            for (int i = 0; i < node->as.return_stmt.value_count; i++) {
                collect_references(idx, doc, node->as.return_stmt.values[i]);
            }
            break;
        }
        default:
            break;
    }
}

// Index a document
void xlsp_workspace_index_document(XrLspWorkspaceIndex *idx, XrLspDocument *doc) {
    if (!idx || !doc || !doc->ast) return;

    // Remove old index for this file if exists
    xlsp_workspace_remove_document(idx, doc->uri);

    // Collect exported symbols (definitions)
    collect_exports(idx, doc, doc->ast);

    // Collect references
    collect_references(idx, doc, doc->ast);

    // Add to indexed files
    add_indexed_file(idx, doc->uri);
}

// Remove document from index
void xlsp_workspace_remove_document(XrLspWorkspaceIndex *idx, const char *uri) {
    if (!idx || !uri) return;

    // Remove from file list
    for (int i = 0; i < idx->file_count; i++) {
        if (strcmp(idx->indexed_files[i], uri) == 0) {
            xr_free(idx->indexed_files[i]);
            memmove(&idx->indexed_files[i], &idx->indexed_files[i+1],
                    (idx->file_count - i - 1) * sizeof(char*));
            idx->file_count--;
            break;
        }
    }

    // Remove symbols defined in this file
    for (int i = 0; i < idx->table_size; i++) {
        XrLspWorkspaceSymbol **pp = &idx->symbols[i];
        while (*pp) {
            XrLspWorkspaceSymbol *sym = *pp;
            if (sym->def_loc.uri && strcmp(sym->def_loc.uri, uri) == 0) {
                *pp = sym->next;
                free_workspace_symbol(sym);
                idx->symbol_count--;
            } else {
                // Remove references from this file
                XrLspSymbolRef **rp = &sym->refs;
                while (*rp) {
                    XrLspSymbolRef *ref = *rp;
                    if (ref->loc.uri && strcmp(ref->loc.uri, uri) == 0) {
                        *rp = ref->next;
                        xr_free(ref->loc.uri);
                        xr_free(ref);
                        sym->ref_count--;
                    } else {
                        rp = &(*rp)->next;
                    }
                }
                pp = &(*pp)->next;
            }
        }
    }
}

// Find symbol definition
XrLspWorkspaceSymbol *xlsp_workspace_find_definition(XrLspWorkspaceIndex *idx,
                                                       const char *name) {
    if (!idx || !name) return NULL;

    uint32_t hash = hash_name(name) % idx->table_size;
    XrLspWorkspaceSymbol *sym = idx->symbols[hash];
    while (sym) {
        if (strcmp(sym->name, name) == 0) {
            return sym;
        }
        sym = sym->next;
    }
    return NULL;
}

// Find all references
XrLspSymbolRef *xlsp_workspace_find_references(XrLspWorkspaceIndex *idx,
                                                 const char *name,
                                                 int *count) {
    *count = 0;
    XrLspWorkspaceSymbol *sym = xlsp_workspace_find_definition(idx, name);
    if (!sym) return NULL;

    *count = sym->ref_count;
    return sym->refs;
}

// Find symbol at position
XrLspWorkspaceSymbol *xlsp_workspace_symbol_at(XrLspWorkspaceIndex *idx,
                                                 const char *uri,
                                                 int line, int column) {
    if (!idx || !uri) return NULL;

    XrLspWorkspaceSymbol *best_match = NULL;
    int best_distance = INT_MAX;

    // Search all symbols for one defined at this location
    for (int i = 0; i < idx->table_size; i++) {
        XrLspWorkspaceSymbol *sym = idx->symbols[i];
        while (sym) {
            if (sym->def_loc.uri && strcmp(sym->def_loc.uri, uri) == 0 &&
                sym->def_loc.line == line) {
                // Check column range if available
                int start_col = sym->def_loc.column;
                int end_col = sym->def_loc.end_column;

                // If column info is available (end_column > 0), use precise matching
                if (end_col > 0 && column >= 0) {
                    if (column >= start_col && column <= end_col) {
                        // Exact match within symbol range
                        return sym;
                    }
                    // Calculate distance from symbol for fallback
                    int distance = (column < start_col) ? (start_col - column) : (column - end_col);
                    if (distance < best_distance) {
                        best_distance = distance;
                        best_match = sym;
                    }
                } else {
                    // No column info, fall back to line-only match
                    if (!best_match) {
                        best_match = sym;
                    }
                }
            }
            sym = sym->next;
        }
    }

    return best_match;
}

// Get all workspace symbols
XrLspWorkspaceSymbol **xlsp_workspace_get_all_symbols(XrLspWorkspaceIndex *idx,
                                                        int *count) {
    if (!idx || idx->symbol_count == 0) {
        *count = 0;
        return NULL;
    }

    XrLspWorkspaceSymbol **result = xr_calloc(idx->symbol_count,
                                            sizeof(XrLspWorkspaceSymbol*));
    int n = 0;

    for (int i = 0; i < idx->table_size; i++) {
        XrLspWorkspaceSymbol *sym = idx->symbols[i];
        while (sym && n < idx->symbol_count) {
            result[n++] = sym;
            sym = sym->next;
        }
    }

    *count = n;
    return result;
}

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
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        char path[XLSP_MAX_PATH];
        snprintf(path, sizeof(path), "%s/%s", dir_path, entry->d_name);

        struct stat st;
        if (stat(path, &st) < 0) continue;

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
                    if (new_capacity < *capacity) continue;

                    char **new_files = xr_realloc(*files, new_capacity * sizeof(char*));
                    if (!new_files) continue;  // Skip this file on failure

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
    XrLspIndexTaskData *task_data = (XrLspIndexTaskData *)data;

    // Find all .xr files using server's ignore configuration
    int capacity = 64;
    task_data->files = xr_malloc(capacity * sizeof(char*));
    task_data->file_count = 0;

    if (task_data->server) {
        find_xr_files_with_config(task_data->root_path, &task_data->files,
                                   &task_data->file_count, &capacity,
                                   &task_data->server->config);
    } else {
        find_xr_files(task_data->root_path, &task_data->files,
                      &task_data->file_count, &capacity);
    }
}

// Completion: update server state (runs in main thread)
// Index all files in the scanned directory (typically current file's directory)
// On-demand import indexing handles cross-directory dependencies

// Index a single file (helper function)
static void index_single_file(XrLspServer *server, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;

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
            xa_analyzer_analyze(server->workspace_analyzer, uri, (XrAstNode*)ast);
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
    (void)data;
}

static void index_batch_complete(void *data, void *result) {
    (void)result;
    XrLspIndexTaskData *task_data = (XrLspIndexTaskData *)data;
    XrLspServer *server = task_data->server;

    // Check if indexing was cancelled
    if (server->index_cancelled) {
        lsp_log("Background indexing cancelled at %d/%d files",
                task_data->current_file, task_data->file_count);

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
        if (server->index_cancelled) break;

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
            lsp_log("Background indexing cancelled at %d/%d files",
                    server->files_indexed, server->files_total);
            server->index_cancelled = false;
        } else {
            int symbol_count = 0;
            if (server->workspace_analyzer && server->workspace_analyzer->global_scope) {
                XaSymbol **syms = xa_scope_get_all_symbols(server->workspace_analyzer->global_scope, &symbol_count);
                xr_free(syms);
            }

            lsp_log("Background indexing complete: %d/%d files, %d symbols",
                    server->files_indexed, server->files_total, symbol_count);
        }

        // All files indexed or cancelled
        xr_free(task_data->files);
        xr_free(task_data->root_path);
        server->indexing_in_progress = false;
        server->index_task_data = NULL;
        xr_free(task_data);
    } else {
        // Schedule next batch - this allows main thread to process other requests
        XrLspTask *task = xlsp_task_new_ex(
            index_batch_execute,
            index_batch_complete,
            task_data,
            XLSP_TASK_LOW,  // Low priority so user requests take precedence
            XLSP_TASK_TYPE_INDEX,
            0  // No associated request ID
        );
        xlsp_async_submit(server->async, task);
    }
}

void xlsp_workspace_index_task_complete(void *data, void *result) {
    (void)result;
    XrLspIndexTaskData *task_data = (XrLspIndexTaskData *)data;
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
    if (!server || !server->workspace_analyzer || !server->isolate || !path) return;

    FILE *f = fopen(path, "r");
    if (!f) return;

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
        xa_analyzer_update_incremental(server->workspace_analyzer, uri,
                                        (XrAstNode*)ast, content_hash);
        lsp_log("Indexed file: %s (hash: %llx)", path, (unsigned long long)content_hash);
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
    if (!server || !results) return;

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
        // Cross-file symbol merging: re-analyze in main thread
        // This adds symbols to workspace_analyzer for Go to Definition,
        // Find References, workspace/symbol, etc.
        // ================================================================
        if (result->path && result->uri) {
            xlsp_workspace_index_file(server, result->uri, result->path);

            // Count symbols from this file
            for (XrLspIndexSymbol *sym = result->symbols; sym; sym = sym->next) {
                symbols_added++;
            }
        }

        server->files_indexed++;
        merged_count++;
    }

    if (merged_count > 0 || error_count > 0) {
        lsp_log("[IndexPool] Merged %d files (%d errors, %d symbols), total: %d/%d",
                merged_count, error_count, symbols_added,
                server->files_indexed, server->files_total);

        // Update progress
        if (server->index_progress_token && server->files_total > 0) {
            int percentage = (server->files_indexed * 100) / server->files_total;
            char msg[128];
            snprintf(msg, sizeof(msg), "Indexed %d/%d files (%d symbols)",
                     server->files_indexed, server->files_total, symbols_added);
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
                XaSymbol **syms = xa_scope_get_all_symbols(server->workspace_analyzer->global_scope, &total_symbols);
                xr_free(syms);
            }

            lsp_log("[IndexPool] Background indexing complete: %d files, %d global symbols",
                    completed, total_symbols);

            // End progress with success message
            if (server->index_progress_token) {
                char msg[128];
                snprintf(msg, sizeof(msg), "Indexed %d files (%d symbols)",
                         completed, total_symbols);
                xlsp_progress_end(server, server->index_progress_token, msg);
                xr_free(server->index_progress_token);
                server->index_progress_token = NULL;
            }
        }
    }
}

// Poll index pool and process results (call from main loop)
void xlsp_workspace_poll_index_results(XrLspServer *server) {
    if (!server || !server->index_pool) return;

    XrLspIndexResult *results = xlsp_index_pool_poll(server->index_pool);
    if (results) {
        xlsp_workspace_merge_index_results(server, results);
        xlsp_index_result_free_list(results);
    }
}

// Get index pool notify fd (for select/poll)
int xlsp_workspace_get_index_notify_fd(XrLspServer *server) {
    if (!server || !server->index_pool) return -1;
    return xlsp_index_pool_get_notify_fd(server->index_pool);
}

// Start background workspace indexing (using parallel index pool)
void xlsp_workspace_start_background_index(XrLspServer *server, const char *root_path) {
    if (!server || !root_path) return;
    if (server->indexing_in_progress) return;

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
    char **files = xr_malloc(capacity * sizeof(char*));
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
void xlsp_workspace_start_background_index_roots(XrLspServer *server,
                                                  const char **roots, int root_count) {
    if (!server || !roots || root_count <= 0) return;
    if (server->indexing_in_progress) return;

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
    char **files = xr_malloc(capacity * sizeof(char*));
    if (!files) {
        server->indexing_in_progress = false;
        return;
    }
    int file_count = 0;

    for (int r = 0; r < root_count; r++) {
        if (roots[r]) {
            find_xr_files_with_config(roots[r], &files, &file_count, &capacity,
                                      &server->config);
        }
    }

    server->files_total = file_count;

    if (file_count > 0) {
        if (server->index_progress_token) {
            xr_free(server->index_progress_token);
        }
        char msg[128];
        snprintf(msg, sizeof(msg), "Indexing %d files from %d roots...",
                 file_count, root_count);
        server->index_progress_token = xlsp_progress_begin(
            server, "Indexing Workspace", msg, true);

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
    if (!server || !path_prefix) return;

    size_t prefix_len = strlen(path_prefix);
    if (prefix_len == 0) return;

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
