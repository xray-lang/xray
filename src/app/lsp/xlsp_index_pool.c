/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_index_pool.c - Multi-isolate parallel indexing pool implementation
 */

#include "xlsp_index_pool.h"
#include "xlsp_server.h"
#include "xray_isolate.h"
#include "../../runtime/xisolate_api.h"
#include "../../frontend/parser/xparse.h"
#include "../../frontend/parser/xast.h"
#include "../../frontend/parser/xast_nodes.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include "../../base/xmalloc.h"

// lsp_log declared in xlsp_server.h

// ============================================================================
// Symbol Extraction from AST
// ============================================================================

// Create a new index symbol
static XrLspIndexSymbol *create_symbol(const char *name, XrLspSymbolKind kind,
                                        int line, int column, bool is_exported) {
    XrLspIndexSymbol *sym = xr_calloc(1, sizeof(XrLspIndexSymbol));
    if (!sym) return NULL;

    sym->name = name ? xr_strdup(name) : NULL;
    sym->kind = kind;
    sym->line = line;
    sym->column = column;
    sym->is_exported = is_exported;
    sym->is_definition = true;

    return sym;
}

// Add symbol to result
static void add_symbol_to_result(XrLspIndexResult *result, XrLspIndexSymbol *sym) {
    if (!result || !sym) return;

    sym->next = result->symbols;
    result->symbols = sym;
    result->symbol_count++;
}

// Extract symbols from AST node (recursive)
static void extract_symbols(XrLspIndexResult *result, AstNode *node, bool in_export) {
    if (!node) return;

    switch (node->type) {
        case AST_PROGRAM: {
            int count = node->as.program.count;
            for (int i = 0; i < count; i++) {
                extract_symbols(result, node->as.program.statements[i], false);
            }
            break;
        }

        case AST_EXPORT_STMT: {
            // Mark nested declarations as exported
            AstNode *decl = node->as.export_stmt.declaration;
            if (decl) {
                extract_symbols(result, decl, true);
            }
            break;
        }

        case AST_VAR_DECL: {
            const char *name = node->as.var_decl.name;
            if (name) {
                XrLspSymbolKind kind = node->as.var_decl.is_const ?
                                       XR_SYMBOL_CONSTANT : XR_SYMBOL_VARIABLE;
                XrLspIndexSymbol *sym = create_symbol(name, kind,
                                                       node->line - 1, 0, in_export);
                add_symbol_to_result(result, sym);
            }
            break;
        }

        case AST_FUNCTION_DECL: {
            const char *name = node->as.function_decl.name;
            if (name) {
                XrLspIndexSymbol *sym = create_symbol(name, XR_SYMBOL_FUNCTION,
                                                       node->line - 1, 0, in_export);
                add_symbol_to_result(result, sym);
            }
            // Also extract symbols from function body
            extract_symbols(result, node->as.function_decl.body, false);
            break;
        }

        case AST_CLASS_DECL:
        case AST_STRUCT_DECL: {
            const char *name = node->as.class_decl.name;
            if (name) {
                XrLspIndexSymbol *sym = create_symbol(name, XR_SYMBOL_CLASS,
                                                       node->line - 1, 0, in_export);
                add_symbol_to_result(result, sym);
            }
            // Extract methods
            for (int i = 0; i < node->as.class_decl.method_count; i++) {
                AstNode *method = node->as.class_decl.methods[i];
                if (method && method->type == AST_FUNCTION_DECL) {
                    const char *method_name = method->as.function_decl.name;
                    if (method_name) {
                        XrLspIndexSymbol *msym = create_symbol(method_name, XR_SYMBOL_METHOD,
                                                                method->line - 1, 0, false);
                        add_symbol_to_result(result, msym);
                    }
                }
            }
            break;
        }

        case AST_ENUM_DECL: {
            const char *name = node->as.enum_decl.name;
            if (name) {
                XrLspIndexSymbol *sym = create_symbol(name, XR_SYMBOL_ENUM,
                                                       node->line - 1, 0, in_export);
                add_symbol_to_result(result, sym);
            }
            // Extract enum members
            for (int i = 0; i < node->as.enum_decl.member_count; i++) {
                AstNode *member = node->as.enum_decl.members[i];
                if (member && member->type == AST_ENUM_MEMBER) {
                    const char *member_name = member->as.enum_member.name;
                    if (member_name) {
                        XrLspIndexSymbol *msym = create_symbol(member_name, XR_SYMBOL_ENUM_MEMBER,
                                                                member->line - 1, 0, false);
                        add_symbol_to_result(result, msym);
                    }
                }
            }
            break;
        }

        case AST_INTERFACE_DECL: {
            const char *name = node->as.interface_decl.name;
            if (name) {
                XrLspIndexSymbol *sym = create_symbol(name, XR_SYMBOL_INTERFACE,
                                                       node->line - 1, 0, in_export);
                add_symbol_to_result(result, sym);
            }
            break;
        }

        case AST_TYPE_ALIAS: {
            const char *name = node->as.type_alias.name;
            if (name) {
                XrLspIndexSymbol *sym = create_symbol(name, XR_SYMBOL_TYPE_PARAMETER,
                                                       node->line - 1, 0, in_export);
                add_symbol_to_result(result, sym);
            }
            break;
        }

        case AST_BLOCK: {
            int count = node->as.block.count;
            for (int i = 0; i < count; i++) {
                extract_symbols(result, node->as.block.statements[i], false);
            }
            break;
        }

        default:
            // Skip other node types
            break;
    }
}

// ============================================================================
// File Parsing (in worker thread)
// ============================================================================

static XrLspIndexResult *parse_file(XrayIsolate *isolate, const char *path, const char *uri) {
    XrLspIndexResult *result = xr_calloc(1, sizeof(XrLspIndexResult));
    if (!result) return NULL;

    result->path = xr_strdup(path);
    result->uri = xr_strdup(uri);
    result->success = false;

    // Read file content
    FILE *f = fopen(path, "r");
    if (!f) {
        result->error_message = xr_strdup("Cannot open file");
        return result;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *content = xr_malloc(size + 1);
    if (!content) {
        fclose(f);
        result->error_message = xr_strdup("Out of memory");
        return result;
    }

    size_t read_size = fread(content, 1, size, f);
    content[read_size] = '\0';
    fclose(f);

    // Parse with recoverable parser (tolerates errors)
    Parser parser;
    xr_parser_init(&parser, isolate, content, uri, NULL);
    AstNode *ast = xr_parse_recoverable(&parser);

    if (ast) {
        // Extract symbols from AST
        extract_symbols(result, ast, false);
        result->success = !parser.had_error;

        if (parser.had_error && parser.error_count > 0) {
            result->error_message = xr_strdup("Parse errors");
        }

        // Free AST (we've extracted what we need)
        xr_ast_free(isolate, ast);
    } else {
        result->error_message = xr_strdup("Parse failed");
    }

    xr_free(content);
    return result;
}

// ============================================================================
// Worker Thread
// ============================================================================

static void *worker_thread(void *arg) {
    XrLspIndexWorker *worker = (XrLspIndexWorker *)arg;
    XrLspIndexPool *pool = worker->pool;

    // Share the main loop's log routing (file + stderr) on this worker
    // thread. Without this, every lsp_log() below would go to stderr only.
    if (pool->server) {
        xlsp_set_log_server(pool->server);
    }

    lsp_log("[IndexPool] Worker %d started", worker->worker_id);

    // Create per-worker Isolate
    XrayIsolateParams params;
    xray_isolate_params_init(&params);
    xray_isolate_setup_full(&params);
    params.enable_gc = false;  // Minimal Isolate for parsing only

    worker->isolate = xray_isolate_new(&params);
    if (!worker->isolate) {
        lsp_log("[IndexPool] Worker %d: Failed to create Isolate", worker->worker_id);
        return NULL;
    }

    // Enter the isolate for this thread
    xray_isolate_enter(worker->isolate);

    while (atomic_load(&pool->running)) {
        // Wait for work
        pthread_mutex_lock(&pool->work_mutex);
        while (pool->work_head == NULL && atomic_load(&pool->running)) {
            pthread_cond_wait(&pool->work_cond, &pool->work_mutex);
        }

        if (!atomic_load(&pool->running)) {
            pthread_mutex_unlock(&pool->work_mutex);
            break;
        }

        // Get work item
        XrLspIndexWork *work = pool->work_head;
        if (work) {
            pool->work_head = work->next;
            if (pool->work_head == NULL) {
                pool->work_tail = NULL;
            }
            pool->work_count--;
        }
        pthread_mutex_unlock(&pool->work_mutex);

        if (!work) continue;

        atomic_fetch_add(&pool->active_workers, 1);

        // Parse file and extract symbols
        XrLspIndexResult *result = parse_file(worker->isolate, work->path, work->uri);

        // Free work item
        xr_free(work->path);
        xr_free(work->uri);
        xr_free(work);

        // Submit result
        if (result) {
            pthread_mutex_lock(&pool->result_mutex);
            result->next = NULL;
            if (pool->result_tail) {
                pool->result_tail->next = result;
            } else {
                pool->result_head = result;
            }
            pool->result_tail = result;
            pool->result_count++;
            pthread_mutex_unlock(&pool->result_mutex);

            // Notify main thread
            char byte = 1;
            ssize_t n = write(pool->notify_fd[1], &byte, 1);
            (void)n;
        }

        atomic_fetch_add(&pool->files_completed, 1);
        atomic_fetch_sub(&pool->active_workers, 1);
    }

    // Cleanup
    xray_isolate_exit();
    xray_isolate_delete(worker->isolate);
    worker->isolate = NULL;

    lsp_log("[IndexPool] Worker %d stopped", worker->worker_id);
    return NULL;
}

// ============================================================================
// Pool Management
// ============================================================================

XrLspIndexPool *xlsp_index_pool_new(XrLspServer *server) {
    XrLspIndexPool *pool = xr_calloc(1, sizeof(XrLspIndexPool));
    if (!pool) return NULL;

    pool->server = server;

    // Create notification pipe
    if (pipe(pool->notify_fd) < 0) {
        xr_free(pool);
        return NULL;
    }

    // Set read end to non-blocking
    int flags = fcntl(pool->notify_fd[0], F_GETFL, 0);
    fcntl(pool->notify_fd[0], F_SETFL, flags | O_NONBLOCK);

    // Initialize mutexes and condition variables
    pthread_mutex_init(&pool->work_mutex, NULL);
    pthread_cond_init(&pool->work_cond, NULL);
    pthread_mutex_init(&pool->result_mutex, NULL);

    atomic_store(&pool->running, true);

    // Start worker threads
    pool->worker_count = XLSP_INDEX_POOL_SIZE;
    for (int i = 0; i < pool->worker_count; i++) {
        pool->workers[i].worker_id = i;
        pool->workers[i].pool = pool;
        pool->workers[i].isolate = NULL;

        if (pthread_create(&pool->workers[i].thread, NULL, worker_thread, &pool->workers[i]) != 0) {
            lsp_log("[IndexPool] Failed to create worker %d", i);
            pool->worker_count = i;
            break;
        }
    }

    lsp_log("[IndexPool] Created with %d workers", pool->worker_count);
    return pool;
}

void xlsp_index_pool_free(XrLspIndexPool *pool) {
    if (!pool) return;

    // Signal shutdown
    atomic_store(&pool->running, false);

    // Wake all workers
    pthread_mutex_lock(&pool->work_mutex);
    pthread_cond_broadcast(&pool->work_cond);
    pthread_mutex_unlock(&pool->work_mutex);

    // Wait for workers to finish
    for (int i = 0; i < pool->worker_count; i++) {
        pthread_join(pool->workers[i].thread, NULL);
    }

    // Free remaining work items
    XrLspIndexWork *work = pool->work_head;
    while (work) {
        XrLspIndexWork *next = work->next;
        xr_free(work->path);
        xr_free(work->uri);
        xr_free(work);
        work = next;
    }

    // Free remaining results
    xlsp_index_result_free_list(pool->result_head);

    // Cleanup
    close(pool->notify_fd[0]);
    close(pool->notify_fd[1]);
    pthread_mutex_destroy(&pool->work_mutex);
    pthread_cond_destroy(&pool->work_cond);
    pthread_mutex_destroy(&pool->result_mutex);

    xr_free(pool);
    lsp_log("[IndexPool] Destroyed");
}

void xlsp_index_pool_submit(XrLspIndexPool *pool, const char *path, const char *uri) {
    if (!pool || !path) return;

    XrLspIndexWork *work = xr_calloc(1, sizeof(XrLspIndexWork));
    if (!work) return;

    work->path = xr_strdup(path);
    work->uri = uri ? xr_strdup(uri) : NULL;
    if (!work->uri) {
        // Generate URI from path
        char uri_buf[1200];
        snprintf(uri_buf, sizeof(uri_buf), "file://%s", path);
        work->uri = xr_strdup(uri_buf);
    }

    pthread_mutex_lock(&pool->work_mutex);
    work->next = NULL;
    if (pool->work_tail) {
        pool->work_tail->next = work;
    } else {
        pool->work_head = work;
    }
    pool->work_tail = work;
    pool->work_count++;
    pthread_cond_signal(&pool->work_cond);
    pthread_mutex_unlock(&pool->work_mutex);

    atomic_fetch_add(&pool->files_submitted, 1);
}

void xlsp_index_pool_submit_batch(XrLspIndexPool *pool, char **paths, int count) {
    if (!pool || !paths || count <= 0) return;

    pthread_mutex_lock(&pool->work_mutex);

    for (int i = 0; i < count; i++) {
        if (!paths[i]) continue;

        XrLspIndexWork *work = xr_calloc(1, sizeof(XrLspIndexWork));
        if (!work) continue;

        work->path = xr_strdup(paths[i]);
        char uri_buf[1200];
        snprintf(uri_buf, sizeof(uri_buf), "file://%s", paths[i]);
        work->uri = xr_strdup(uri_buf);

        work->next = NULL;
        if (pool->work_tail) {
            pool->work_tail->next = work;
        } else {
            pool->work_head = work;
        }
        pool->work_tail = work;
        pool->work_count++;
    }

    // Wake all workers
    pthread_cond_broadcast(&pool->work_cond);
    pthread_mutex_unlock(&pool->work_mutex);

    atomic_fetch_add(&pool->files_submitted, count);
}

int xlsp_index_pool_get_notify_fd(XrLspIndexPool *pool) {
    return pool ? pool->notify_fd[0] : -1;
}

XrLspIndexResult *xlsp_index_pool_poll(XrLspIndexPool *pool) {
    if (!pool) return NULL;

    // Drain notification pipe
    char buf[64];
    while (read(pool->notify_fd[0], buf, sizeof(buf)) > 0) {
        // Consume all notifications
    }

    // Get all results
    pthread_mutex_lock(&pool->result_mutex);
    XrLspIndexResult *results = pool->result_head;
    pool->result_head = NULL;
    pool->result_tail = NULL;
    pool->result_count = 0;
    pthread_mutex_unlock(&pool->result_mutex);

    return results;
}

bool xlsp_index_pool_is_idle(XrLspIndexPool *pool) {
    if (!pool) return true;

    return pool->work_count == 0 && atomic_load(&pool->active_workers) == 0;
}

void xlsp_index_pool_get_progress(XrLspIndexPool *pool, int *submitted, int *completed) {
    if (!pool) {
        if (submitted) *submitted = 0;
        if (completed) *completed = 0;
        return;
    }

    if (submitted) *submitted = atomic_load(&pool->files_submitted);
    if (completed) *completed = atomic_load(&pool->files_completed);
}

// ============================================================================
// Memory Management
// ============================================================================

void xlsp_index_symbol_free(XrLspIndexSymbol *sym) {
    if (!sym) return;
    xr_free(sym->name);
    xr_free(sym->type_str);
    xr_free(sym);
}

void xlsp_index_result_free(XrLspIndexResult *result) {
    if (!result) return;

    // Free symbols
    XrLspIndexSymbol *sym = result->symbols;
    while (sym) {
        XrLspIndexSymbol *next = sym->next;
        xlsp_index_symbol_free(sym);
        sym = next;
    }

    xr_free(result->uri);
    xr_free(result->path);
    xr_free(result->error_message);
    xr_free(result);
}

void xlsp_index_result_free_list(XrLspIndexResult *list) {
    while (list) {
        XrLspIndexResult *next = list->next;
        xlsp_index_result_free(list);
        list = next;
    }
}
