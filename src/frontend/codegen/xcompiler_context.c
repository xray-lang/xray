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
#include "../../runtime/object/xshape_cache.h"
#include "../../base/xmalloc.h"
#include "../xdiag_fmt.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

XrCompilerContext *xr_compiler_context_new(XrayIsolate *X) {
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

    ctx->X = X;
    ctx->current_line = 1;
    ctx->current_column = 0;
    ctx->source_file = NULL;
    ctx->global_var_count = 0;
    ctx->shared_var_count = 0;
    ctx->shared_offset = 0;
    ctx->had_error = false;
    ctx->panic_mode = false;
    ctx->repl_mode = false;
    ctx->max_globals = MAX_GLOBALS;

    ctx->enum_type_names = NULL;
    ctx->enum_type_count = 0;
    ctx->enum_type_capacity = 0;

    ctx->shape_cache = xr_shape_cache_new();

    xr_arena_init(&ctx->arena, 0);

    ctx->current_operator = NULL;
    ctx->current_storage_mode = 0;
    ctx->current_elem_tid = 0;
    ctx->current_key_tid = 0;
    ctx->current_class_desc = NULL;
    ctx->current_class_node = NULL;
    ctx->current_object_type = NULL;

    ctx->const_entries = NULL;
    ctx->const_entry_count = 0;
    ctx->const_entry_capacity = 0;

    // Create unified type analyzer
    ctx->analyzer = xa_analyzer_new(ctx->X);

    return ctx;
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

    if (ctx->enum_type_names) {
        for (int i = 0; i < ctx->enum_type_count; i++) {
            if (ctx->enum_type_names[i]) {
                xr_free(ctx->enum_type_names[i]);
            }
        }
        xr_free(ctx->enum_type_names);
    }

    if (ctx->shape_cache) {
        xr_shape_cache_free(ctx->shape_cache);
    }

    xr_arena_destroy(&ctx->arena);

    if (ctx->const_entries) {
        xr_free(ctx->const_entries);
        ctx->const_entries = NULL;
        ctx->const_entry_count = 0;
        ctx->const_entry_capacity = 0;
    }

    if (ctx->analyzer) {
        xa_analyzer_free(ctx->analyzer);
        ctx->analyzer = NULL;
    }

    xr_free(ctx);
}

void xr_compiler_context_reset(XrCompilerContext *ctx) {
    if (!ctx)
        return;

    ctx->current_line = 1;
    ctx->global_var_count = 0;
    ctx->had_error = false;
    ctx->panic_mode = false;
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

void xr_compiler_ctx_register_enum_type(XrCompilerContext *ctx, const char *enum_name) {
    if (!ctx || !enum_name)
        return;

    for (int i = 0; i < ctx->enum_type_count; i++) {
        if (ctx->enum_type_names[i] && strcmp(ctx->enum_type_names[i], enum_name) == 0) {
            return;
        }
    }

    if (ctx->enum_type_count >= ctx->enum_type_capacity) {
        int new_capacity = ctx->enum_type_capacity == 0 ? 8 : ctx->enum_type_capacity * 2;
        char **new_array = (char **) xr_malloc(sizeof(char *) * new_capacity);
        if (!new_array)
            return;

        if (ctx->enum_type_names) {
            for (int i = 0; i < ctx->enum_type_count; i++) {
                new_array[i] = ctx->enum_type_names[i];
            }
            xr_free(ctx->enum_type_names);
        }

        ctx->enum_type_names = new_array;
        ctx->enum_type_capacity = new_capacity;
    }

    ctx->enum_type_names[ctx->enum_type_count] = strdup(enum_name);
    ctx->enum_type_count++;
    XR_DCHECK(ctx->enum_type_count <= ctx->enum_type_capacity,
              "register_enum_type: count > capacity");
}

bool xr_compiler_ctx_is_enum_type(XrCompilerContext *ctx, const char *var_name) {
    if (!ctx || !var_name)
        return false;

    for (int i = 0; i < ctx->enum_type_count; i++) {
        if (ctx->enum_type_names[i] && strcmp(ctx->enum_type_names[i], var_name) == 0) {
            return true;
        }
    }

    return false;
}

static void ensure_const_capacity(XrCompilerContext *ctx) {
    if (ctx->const_entry_count >= ctx->const_entry_capacity) {
        int new_capacity = ctx->const_entry_capacity == 0 ? 16 : ctx->const_entry_capacity * 2;
        ConstEntry *new_entries = (ConstEntry *) xr_malloc(sizeof(ConstEntry) * new_capacity);
        if (!new_entries)
            return;

        if (ctx->const_entries) {
            for (int i = 0; i < ctx->const_entry_count; i++) {
                new_entries[i] = ctx->const_entries[i];
            }
            xr_free(ctx->const_entries);
        }

        ctx->const_entries = new_entries;
        ctx->const_entry_capacity = new_capacity;
    }
}

void xr_compiler_ctx_add_const_int(XrCompilerContext *ctx, XrString *name, int64_t value) {
    if (!ctx || !name)
        return;

    ensure_const_capacity(ctx);

    ConstEntry *entry = &ctx->const_entries[ctx->const_entry_count++];
    entry->name = name;
    entry->type = CONST_INT;
    entry->value.int_val = value;
}

void xr_compiler_ctx_add_const_float(XrCompilerContext *ctx, XrString *name, double value) {
    if (!ctx || !name)
        return;

    ensure_const_capacity(ctx);

    ConstEntry *entry = &ctx->const_entries[ctx->const_entry_count++];
    entry->name = name;
    entry->type = CONST_FLOAT;
    entry->value.float_val = value;
}

void xr_compiler_ctx_add_const_string(XrCompilerContext *ctx, XrString *name, XrString *value) {
    if (!ctx || !name)
        return;

    ensure_const_capacity(ctx);

    ConstEntry *entry = &ctx->const_entries[ctx->const_entry_count++];
    entry->name = name;
    entry->type = CONST_STRING;
    entry->value.str_val = value;
}

ConstEntry *xr_compiler_ctx_find_const(XrCompilerContext *ctx, XrString *name) {
    if (!ctx || !name)
        return NULL;

    for (int i = 0; i < ctx->const_entry_count; i++) {
        if (ctx->const_entries[i].name == name ||
            (ctx->const_entries[i].name && name &&
             strcmp(ctx->const_entries[i].name->data, name->data) == 0)) {
            return &ctx->const_entries[i];
        }
    }

    return NULL;
}
