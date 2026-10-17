/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcompiler_context.c - Compiler context implementation
 */

#include "xcompiler_context.h"
#include "../../base/xchecks.h"
#include "../../runtime/value/xtype_internal.h"
#include "../../base/xmalloc.h"
#include "../../toolchain/xcompiler_session.h"
#include "../xdiag_fmt.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* Shared initializer for both public constructors.  When `analyzer` is
 * NULL a fresh owned analyzer is created; otherwise the provided one is
 * borrowed (ownership stays with the caller). */
static XrCompilerContext *xr_compiler_context_new_impl(XrCompilerSession *session,
                                                       XaAnalyzer *analyzer) {
    XrVMRuntime *X = xr_compiler_session_vm_host(session);
    if (!session || !X) {
        return NULL;
    }

    XrCompilerContext *ctx = (XrCompilerContext *) xr_malloc(sizeof(XrCompilerContext));
    if (!ctx) {
        return NULL;
    }

    ctx->global_vars = (XrGlobalVar *) xr_malloc(sizeof(XrGlobalVar) * MAX_GLOBALS);
    if (!ctx->global_vars) {
        xr_free(ctx);
        return NULL;
    }

#define SHARED_INITIAL_CAPACITY 64
    ctx->shared_vars = (XrSharedVar *) xr_malloc(sizeof(XrSharedVar) * SHARED_INITIAL_CAPACITY);
    if (!ctx->shared_vars) {
        xr_free(ctx->global_vars);
        xr_free(ctx);
        return NULL;
    }
    ctx->shared_var_capacity = SHARED_INITIAL_CAPACITY;
    for (int i = 0; i < SHARED_INITIAL_CAPACITY; i++) {
        ctx->shared_vars[i].name = NULL;
        ctx->shared_vars[i].index = -1;
        ctx->shared_vars[i].is_const = false;
        ctx->shared_vars[i].compile_type = NULL;
        ctx->shared_vars[i].state = SHARED_STATE_OWNED;
        ctx->shared_vars[i].moved_line = 0;
        ctx->shared_vars[i].moved_column = 0;
    }

    ctx->compiler_session = session;
    ctx->X = X;
    ctx->current_line = 1;
    ctx->current_column = 0;
    ctx->source_file = NULL;
    ctx->source_content = NULL;
    ctx->global_var_count = 0;
    ctx->shared_var_count = 0;
    ctx->had_error = false;
    ctx->panic_mode = false;
    ctx->repl_mode = false;
    ctx->post_analyze_hook = NULL;
    ctx->post_analyze_user_data = NULL;
    ctx->module_graph = NULL;
    ctx->graph_modules = NULL;
    ctx->graph_module_count = 0;
    ctx->max_globals = MAX_GLOBALS;

    ctx->current_operator = NULL;
    ctx->current_storage_mode = 0;
    ctx->current_elem_tid = 0;
    ctx->current_key_tid = 0;
    ctx->current_class_desc = NULL;
    ctx->current_class_node = NULL;
    ctx->current_object_type = NULL;

    if (analyzer) {
        ctx->analyzer = analyzer;
        ctx->owns_analyzer = false;
        if (analyzer->type_pool)
            xr_type_set_current_pool(analyzer->type_pool, &analyzer->type_pool->next_type_id);
    } else {
        ctx->analyzer = xa_analyzer_new(ctx->compiler_session);
        ctx->owns_analyzer = true;
    }

    return ctx;
}

XrCompilerContext *xr_compiler_context_new(XrCompilerSession *session) {
    return xr_compiler_context_new_impl(session, NULL);
}

XrCompilerContext *xr_compiler_context_new_with_analyzer(XrCompilerSession *session,
                                                         XaAnalyzer *analyzer) {
    return xr_compiler_context_new_impl(session, analyzer);
}

void xr_compiler_context_free(XrCompilerContext *ctx) {
    if (!ctx)
        return;

    if (ctx->global_vars) {
        xr_free(ctx->global_vars);
    }

    if (ctx->shared_vars) {
        xr_free(ctx->shared_vars);
    }

    if (ctx->analyzer && ctx->owns_analyzer) {
        xa_analyzer_free(ctx->analyzer);
    }
    ctx->analyzer = NULL;

    xr_free(ctx);
}

void xr_compiler_context_reset(XrCompilerContext *ctx) {
    if (!ctx)
        return;

    ctx->current_line = 1;
    ctx->source_content = NULL;
    ctx->global_var_count = 0;
    ctx->had_error = false;
    ctx->panic_mode = false;
    ctx->post_analyze_hook = NULL;
    ctx->post_analyze_user_data = NULL;
    ctx->module_graph = NULL;
    ctx->graph_modules = NULL;
    ctx->graph_module_count = 0;
}

int xr_compiler_ctx_get_or_add_global(XrCompilerContext *ctx, XrString *name) {
    if (!ctx || !name)
        return -1;

    for (int i = 0; i < ctx->global_var_count; i++) {
        if (ctx->global_vars[i].name == name) {
            return ctx->global_vars[i].index;
        }
    }

    if (ctx->global_var_count >= ctx->max_globals) {
        char msg[128];
        snprintf(msg, sizeof(msg), "too many global variables (max %d)", ctx->max_globals);
        xr_diag_print(XR_DIAG_ERROR, 0, msg, ctx->source_file, ctx->current_line,
                      ctx->current_column > 0 ? ctx->current_column : 1, 0, NULL, NULL);
        ctx->had_error = true;
        return -1;
    }

    int index = ctx->global_var_count;
    ctx->global_vars[index].name = name;
    ctx->global_vars[index].index = index;
    ctx->global_var_count++;
    XR_DCHECK(ctx->global_var_count <= ctx->max_globals, "get_or_add_global: count > max_globals");

    return index;
}

int xr_compiler_ctx_find_global(XrCompilerContext *ctx, XrString *name) {
    if (!ctx || !name)
        return -1;

    for (int i = 0; i < ctx->global_var_count; i++) {
        if (ctx->global_vars[i].name != NULL &&
            strcmp(ctx->global_vars[i].name->data, name->data) == 0) {
            return ctx->global_vars[i].index;
        }
    }

    return -1;
}

void xr_compiler_ctx_set_error(XrCompilerContext *ctx) {
    if (ctx) {
        ctx->had_error = true;
    }
}

bool xr_compiler_ctx_has_error(XrCompilerContext *ctx) {
    return ctx ? ctx->had_error : false;
}
