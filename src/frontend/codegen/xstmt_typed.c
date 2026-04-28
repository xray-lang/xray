/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xstmt_typed.c - Typed declaration / runtime-bridging statements
 *
 * KEY CONCEPT:
 *   This TU owns the heavy "type-aware" statements: variable
 *   declarations (`let` / `const`) and the compile-time constant
 *   extractor that supports them. These paths interact with:
 *     - declared-type vs initializer-type checks
 *     - struct value-semantics copy (xstmt_emit_value_copy)
 *     - raw I64/F64 box at assignment edge (xstmt_emit_box_if_raw)
 *     - Json/JsonValue -> concrete-type CHECKTYPE bitmask
 *       (xstmt_emit_json_checktype)
 *     - shared / Channel storage modes
 *     - compile-time const inlining via xr_const_eval_with_ctx +
 *       xr_compiler_ctx_add_const_*
 *     - rematerialization hints on small-int locals
 *
 *   The "simple" siblings (expr-stmt, print, assignment, compound
 *   assignment, inc/dec) live in xstmt_simple.c -- they do not need
 *   any of the bridging machinery above.
 *
 * WHY THIS FILE EXISTS:
 *   xstmt_simple.c is split into xstmt_simple.c + xstmt_typed.c by
 *   cohesion. The shared helpers (xstmt_emit_*, xstmt_type_flag_name)
 *   live in xstmt_helpers.c.
 */

#include "xstmt.h"
#include "xstmt_helpers.h"
#include "../../base/xchecks.h"
#include "../../runtime/xisolate_api.h"
#include "../../runtime/value/xtype.h"
#include "../../runtime/value/xtype_names.h"
#include "../../runtime/symbol/xsymbol_table.h"
#include "../../runtime/value/xstruct_layout.h"
#include "../../runtime/object/xarray.h"
#include "xcompiler.h"
#include "xcompiler_context.h"
#include "xcompiler_class_registry.h"
#include "xemit.h"
#include "xexpr_desc.h"
#include "xexpr.h"
#include "xconst_fold.h"
#include <stdio.h>
#include <string.h>

/* ========== File-static helpers (var-decl-only) ========== */

// Extract element/value XrTypeId from a container type annotation:
// Array<T>/Set<T>/Channel<T> -> T's tid; Map<K,V> -> V's tid.
static uint8_t extract_elem_tid(XrType *type) {
    if (!type)
        return 0;
    XrTypeKind k = type->kind;
    if (k == XR_KIND_ARRAY || k == XR_KIND_SET || k == XR_KIND_CHANNEL) {
        XrType *elem = xr_type_get_element(type);
        return xr_type_to_tid(elem);
    }
    if (k == XR_KIND_MAP) {
        XrType *vt = type->map.value_type;
        return xr_type_to_tid(vt);
    }
    return 0;
}

// Extract key XrTypeId from Map<K,V> type annotation.
static uint8_t extract_key_tid(XrType *type) {
    if (!type)
        return 0;
    if (type->kind == XR_KIND_MAP) {
        XrType *kt = type->map.key_type;
        return xr_type_to_tid(kt);
    }
    return 0;
}

/* ========== Variable Declaration ========== */

/*
 * Try to extract compile-time constant value.
 *
 * If expression is literal or constant expression, returns true and fills out_value.
 * Otherwise returns false.
 */
static bool try_extract_comptime_value(AstNode *expr, ComptimeValue *out_value) {
    if (expr == NULL)
        return false;

    switch (expr->type) {
        case AST_LITERAL_INT:
            out_value->type = COMPTIME_INT;
            out_value->as.int_val = expr->as.literal.raw_value.int_val;
            return true;

        case AST_LITERAL_FLOAT:
            out_value->type = COMPTIME_FLOAT;
            out_value->as.float_val = expr->as.literal.raw_value.float_val;
            return true;

        case AST_LITERAL_TRUE:
            out_value->type = COMPTIME_BOOL;
            out_value->as.bool_val = true;
            return true;

        case AST_LITERAL_FALSE:
            out_value->type = COMPTIME_BOOL;
            out_value->as.bool_val = false;
            return true;

        case AST_LITERAL_STRING:
            out_value->type = COMPTIME_STRING;
            out_value->as.string_val = NULL;  // String value not stored for now
            return true;

        case AST_RANGE: {
            // Range expression: check if start and end are both integer constants
            RangeNode *range = &expr->as.range;
            if (range->start && range->start->type == AST_LITERAL_INT && range->end &&
                range->end->type == AST_LITERAL_INT) {
                out_value->type = COMPTIME_RANGE;
                out_value->as.range.start = range->start->as.literal.raw_value.int_val;
                out_value->as.range.end = range->end->as.literal.raw_value.int_val;
                return true;
            }
            return false;
        }

        default:
            return false;
    }
}

/*
 * Compile variable declaration.
 *
 * Handles local and global variables.
 * Supports compile-time constant auto-inference.
 */
void compile_var_decl(XrCompilerContext *ctx, XrCompiler *compiler, VarDeclNode *node) {
    XR_DCHECK(ctx != NULL, "compile_var_decl: NULL ctx");
    XR_DCHECK(compiler != NULL, "compile_var_decl: NULL compiler");
    XR_DCHECK(node != NULL, "compile_var_decl: NULL node");
    // P0-1: Variable must have type annotation or initializer
    if (!node->initializer && !node->type_annotation) {
        xr_compiler_error(ctx, compiler, "Variable '%s' must have a type annotation or initializer",
                          node->name);
        return;
    }

    // Create string
    XrString *name_str = xr_compile_time_intern(ctx->X, node->name, strlen(node->name));

    // Check if variable already defined in current scope (forbid redefinition, allow shadowing)
    XrLocalInfo *existing = compiler_get_local_by_name(compiler, node->name);
    if (existing && existing->depth == compiler->scope_depth && !existing->is_hoisted) {
        // REPL mode: allow redefinition at top level
        if (!is_repl_top_level(ctx, compiler)) {
            xr_compiler_error(ctx, compiler, "Variable '%s' already defined, cannot redefine",
                              node->name);
            return;
        }
    }

    /* ========== shared variable unified handling (any scope) ========== */
    bool is_channel = (node->initializer && node->initializer->type == AST_CHANNEL_NEW);

    /* Channel must be declared with const (not let) so the handle cannot be
     * reassigned while other coroutines hold references to the same channel.
     * This is enforced regardless of storage mode because Channel has its own
     * refcount + system-heap semantics. */
    if (is_channel && !node->is_const) {
        xr_compiler_error(ctx, compiler,
                          "Channel must be declared with 'const', not 'let'\n"
                          "hint: change 'let %s = Channel(...)' to 'const %s = Channel(...)'",
                          node->name, node->name);
        return;
    }

    // Check if variable already defined (top-level scope)
    // Skip for shared/Channel variables: they may have been pre-registered in Phase 1 hoisting
    if (compiler->scope_depth == 0 && node->storage_mode != XR_STORAGE_SHARED && !is_channel &&
        shared_get_in_scope(ctx, compiler, name_str) >= 0) {
        // REPL mode: allow redefinition (shared_add handles reuse)
        if (!ctx->repl_mode) {
            xr_compiler_error(ctx, compiler, "Variable '%s' already defined, cannot redefine",
                              node->name);
            return;
        }
    }
    // Shared path: explicit 'shared' declaration or Channel initializer.
    // - shared: always global heap (user-declared sharing intent)
    // - Channel: always system-heap allocated with refcount
    if (node->storage_mode == XR_STORAGE_SHARED || is_channel) {
        // Channel objects are allocated on system heap (shared) with refcount.
        // Channels must be `const` (enforced above) — they are the only
        // cross-coroutine value that does not require an explicit `shared`
        // qualifier. Passed to coroutines via arguments (deep_copy does incref).

        // Allocate shared_array index (reuse if pre-registered in Phase 1 hoisting)
        int shared_index = shared_get_or_add(ctx, compiler, name_str);
        shared_set_const(ctx, shared_index, node->is_const);

        // shared const constant inlining optimization: compile-time eval and register to constant
        // table
        if (node->is_const && node->initializer) {
            XrConstEvalResult result = xr_const_eval_with_ctx(ctx, node->initializer);
            if (result.success) {
                if (XR_IS_INT(result.value)) {
                    xr_compiler_ctx_add_const_int(ctx, name_str, XR_TO_INT(result.value));
                } else if (XR_IS_FLOAT(result.value)) {
                    xr_compiler_ctx_add_const_float(ctx, name_str, XR_TO_FLOAT(result.value));
                } else if (XR_IS_STRING(result.value)) {
                    xr_compiler_ctx_add_const_string(ctx, name_str, XR_TO_STRING(result.value));
                }
            }
        }

        int reg = reg_alloc(ctx, compiler);
        if (node->initializer) {
            // Set storage mode, elem_type and object type context
            uint8_t old_storage_mode = ctx->current_storage_mode;
            uint8_t old_elem_type = ctx->current_elem_tid;
            uint8_t old_key_tid = ctx->current_key_tid;
            XrType *old_object_type = ctx->current_object_type;
            ctx->current_storage_mode = XR_STORAGE_SHARED;
            ctx->current_elem_tid = extract_elem_tid(node->type_annotation);
            ctx->current_key_tid = extract_key_tid(node->type_annotation);
            if (node->type_annotation) {
                XrType *ta = node->type_annotation;
                if ((ta->kind == XR_KIND_JSON) && !ta->object.allow_extension &&
                    ta->object.field_count > 0)
                    ctx->current_object_type = ta;
            }

            XrExprDesc expr = xr_compile_expr(ctx, compiler, node->initializer);
            xexpr_to_specific_reg(ctx, compiler, &expr, reg);

            ctx->current_storage_mode = old_storage_mode;
            ctx->current_elem_tid = old_elem_type;
            ctx->current_key_tid = old_key_tid;
            ctx->current_object_type = old_object_type;

            // Non-direct creation types need conversion to system heap
            AstNodeType init_type = node->initializer->type;
            bool is_direct = (init_type == AST_ARRAY_LITERAL || init_type == AST_MAP_LITERAL ||
                              init_type == AST_SET_LITERAL || init_type == AST_OBJECT_LITERAL ||
                              init_type == AST_NEW_EXPR || init_type == AST_CHANNEL_NEW);
            if (!is_direct) {
                xemit_to_shared(compiler->emitter, reg, reg);
            }
        } else {
            xemit_loadnull(compiler->emitter, reg);
        }
        xemit_setshared(compiler->emitter, reg, shared_index);
        reg_free(compiler, reg);

        // Store type info in shared variable table for type inference
        if (is_channel) {
            // Use type annotation if available (e.g., Channel<int>), otherwise Channel<unknown>
            XrType *ch_type = node->type_annotation;
            if (!ch_type || !(ch_type->kind == XR_KIND_CHANNEL)) {
                ch_type = xr_type_new_channel(ctx->X, xr_type_new_unknown(NULL));
            }
            shared_set_type(ctx, shared_index, ch_type);
        } else if (node->initializer) {
            shared_set_type(ctx, shared_index, get_expr_type(ctx, compiler, node->initializer));
        }
        return;
    }

    if (compiler->scope_depth == 0) {
        // Top-level scope

        // const constant inlining optimization: use constant folding for compile-time eval
        if (node->is_const && node->initializer) {
            XrConstEvalResult result = xr_const_eval_with_ctx(ctx, node->initializer);
            if (result.success) {
                // Compile-time eval succeeded, record to constant table
                if (XR_IS_INT(result.value)) {
                    xr_compiler_ctx_add_const_int(ctx, name_str, XR_TO_INT(result.value));
                } else if (XR_IS_FLOAT(result.value)) {
                    xr_compiler_ctx_add_const_float(ctx, name_str, XR_TO_FLOAT(result.value));
                } else if (XR_IS_STRING(result.value)) {
                    xr_compiler_ctx_add_const_string(ctx, name_str, XR_TO_STRING(result.value));
                }
            }
        }

        /* Module system optimization: module-level variables (let and const) compiled as local.
         * Condition: type == FUNCTION_SCRIPT and NOT in REPL mode.
         * REPL mode: top-level vars must be shared to persist across inputs.
         * Note: shared variables and Channel already handled at the beginning.
         */
        if (compiler->type == FUNCTION_SCRIPT && !ctx->repl_mode) {
            // Check if already pre-registered by Phase 1 hoisting
            XrLocalInfo *local = compiler_get_local_by_name(compiler, node->name);
            if (local && local->is_hoisted) {
                local->is_hoisted = false;  // Mark as now being defined
            } else if (local && local->depth == compiler->scope_depth) {
                xr_compiler_error(ctx, compiler, "Variable '%s' already defined, cannot redefine",
                                  node->name);
                return;
            } else {
                local = scope_define_local(ctx, compiler, name_str);
            }
            local->is_const = node->is_const;
            local->storage_mode = node->storage_mode;

            // Type inference
            if (node->type_annotation) {
                local_set_compile_type(local, node->type_annotation);
            } else if (node->initializer) {
                XrType *ct = get_expr_type(ctx, compiler, node->initializer);
                // Pure null inferred type → treat as untyped (null is not a valid variable type)
                if (ct && (ct->kind == XR_KIND_NULL))
                    ct = NULL;
                local_set_compile_type(local, ct);
            }

            // If variable is already cellified (hoisted capture), redirect init to temp
            int m_init_reg = local->reg;
            bool m_needs_cell_set = false;
            if (local->is_cellified) {
                m_init_reg = reg_alloc(ctx, compiler);
                m_needs_cell_set = true;
            }

            // Compile initializer expression
            if (node->initializer) {
                uint8_t old_storage_mode = ctx->current_storage_mode;
                uint8_t old_elem_type = ctx->current_elem_tid;
                uint8_t old_key_tid = ctx->current_key_tid;
                (void) old_key_tid;
                XrType *old_object_type = ctx->current_object_type;
                ctx->current_storage_mode = node->storage_mode;
                ctx->current_elem_tid = extract_elem_tid(node->type_annotation);
                ctx->current_key_tid = extract_key_tid(node->type_annotation);
                if (node->type_annotation) {
                    XrType *ta = node->type_annotation;
                    if ((ta->kind == XR_KIND_JSON) && !ta->object.allow_extension &&
                        ta->object.field_count > 0)
                        ctx->current_object_type = ta;
                }

                XrExprDesc expr = xr_compile_expr(ctx, compiler, node->initializer);
                xexpr_to_specific_reg(ctx, compiler, &expr, m_init_reg);
                xstmt_emit_box_if_raw(compiler->emitter, m_init_reg, &expr);

                // Json→concrete runtime type check
                xstmt_emit_json_checktype(ctx, compiler, m_init_reg, node->type_annotation,
                                          node->initializer);

                // Struct value semantics: copy-on-assign
                if (local->compile_type && local->compile_type->is_value_type) {
                    AstNodeType init_type = node->initializer->type;
                    if (init_type != AST_STRUCT_LITERAL && init_type != AST_NEW_EXPR) {
                        xstmt_emit_value_copy(ctx, compiler, m_init_reg, m_init_reg,
                                              local->compile_type);
                    }
                }

                ctx->current_storage_mode = old_storage_mode;
                ctx->current_elem_tid = old_elem_type;
                ctx->current_object_type = old_object_type;
            } else {
                xemit_loadnull(compiler->emitter, m_init_reg);
            }
            if (local->is_captured && local->ctx_slot >= 0) {
                if (m_needs_cell_set) {
                    xemit_cell_set(compiler->emitter, local->reg, m_init_reg, 0);
                    reg_free(compiler, m_init_reg);
                } else if (!local->is_const && !local->is_cellified) {
                    xemit_cell_new(compiler->emitter, local->reg);
                    local->is_cellified = true;
                }
            } else if (m_needs_cell_set) {
                reg_free(compiler, m_init_reg);
            }
            return;  // Handled as local variable
        }

        // Non-module top-level (e.g. REPL) - use global variable
        if (node->initializer && node->initializer->type == AST_LITERAL_INT) {
            LiteralNode *lit = (LiteralNode *) &node->initializer->as;
            xr_Integer value = lit->raw_value.int_val;

            // Create XrExprDesc
            XrExprDesc expr;
            xexpr_init(&expr, XEXPR_VOID, -1);

            // Small integer uses LOADI - AsBx format!
            if (value >= -MAXARG_sBx && value <= MAXARG_sBx) {
                // LOADI is AsBx format, A=0 temporarily, sBx=value
                int pc = xemit_loadi(compiler->emitter, 0, (int) value);
                expr.kind = XEXPR_RELOC;
                expr.u.pc = pc;
                expr.reg = -1;

                // Successfully generated RELOC expression
            } else {
                XrValue val = xr_int(value);
                int kidx = xr_vm_proto_add_constant(compiler->proto, val);
                int pc = xemit_loadk(compiler->emitter, 0, kidx);

                expr.kind = XEXPR_RELOC;
                expr.u.pc = pc;
                expr.reg = -1;
            }

            // Allocate temp register and discharge
            int reg = reg_alloc(ctx, compiler);

            // Discharge to specific register, trigger instruction writeback
            xexpr_to_specific_reg(ctx, compiler, &expr, reg);

            int shared_index = shared_get_or_add(ctx, compiler, name_str);
            shared_set_const(ctx, shared_index, node->is_const);
            xemit_setshared(compiler->emitter, reg, shared_index);
            reg_free(compiler, reg);
        } else if (node->initializer) {
            // Other expression types
            XrExprDesc expr = xr_compile_expr(ctx, compiler, node->initializer);
            int reg = xexpr_to_anyreg(ctx, compiler, &expr);
            int shared_index = shared_get_or_add(ctx, compiler, name_str);
            shared_set_const(ctx, shared_index, node->is_const);
            xemit_setshared(compiler->emitter, reg, shared_index);
            reg_free(compiler, reg);
        } else {
            // Uninitialized variable, default to null
            int reg = reg_alloc(ctx, compiler);
            xemit_loadnull(compiler->emitter, reg);
            int shared_index = shared_get_or_add(ctx, compiler, name_str);
            xemit_setshared(compiler->emitter, reg, shared_index);
            reg_free(compiler, reg);
        }
    } else {
        // Local variable

        // Static type optimization: infer type from annotation or initializer
        XrType *inferred_compile_type = NULL;
        XrType *xr_type_for_local = NULL;

        if (node->type_annotation) {
            // Has explicit type annotation (node->type_annotation is XrType*)
            xr_type_for_local = node->type_annotation;
            inferred_compile_type = node->type_annotation;
        } else if (node->initializer) {
            // No type annotation, auto-infer from initializer
            inferred_compile_type = get_expr_type(ctx, compiler, node->initializer);
            // Pure null inferred type → discard (null is not a valid variable type)
            if (inferred_compile_type && (inferred_compile_type->kind == XR_KIND_NULL)) {
                inferred_compile_type = NULL;
            }
            xr_type_for_local = inferred_compile_type;
        }

        // Unified type compatibility check for variable initialization with type annotation
        if (node->type_annotation && node->initializer && inferred_compile_type) {
            if (inferred_compile_type->kind != XR_KIND_UNKNOWN) {
                XrType *init_type = get_expr_type(ctx, compiler, node->initializer);
                XrType *check_init = init_type;
                if (check_init && check_init->is_nullable) {
                    XrType *base = xr_type_non_nullable(ctx->X, check_init);
                    if (base)
                        check_init = base;
                }
                if (check_init && !xa_typecheck_assignable(inferred_compile_type, check_init)) {
                    // Json/JsonValue→primitive/union: allowed with runtime type check
                    // (OP_CHECKTYPE)
                    if (!xr_is_json_coercion(inferred_compile_type, check_init)) {
                        xr_compiler_error(ctx, compiler,
                                          "Cannot initialize %s variable '%s' with %s value",
                                          xstmt_type_flag_name(inferred_compile_type), node->name,
                                          xstmt_type_flag_name(check_init));
                        return;
                    }
                }
            }
        }

        /* Register reuse optimization: for function calls, compile first then define variable.
         *
         * Problem:
         *   scope_define_local() allocates local_reg = R[N]
         *   Then compiling function call allocates func_reg = R[N+1]
         *   CALL return value is in R[N+1], needs MOVE to R[N]
         *
         * Optimization:
         *   Compile function call first, get result register expr_reg = R[N]
         *   Then define local variable to use R[N] directly
         *   (no MOVE needed)
         */
        if (node->initializer && node->initializer->type == AST_CALL_EXPR) {
            // Check if already pre-registered by hoisting
            XrLocalInfo *hoisted = compiler_get_local_by_name(compiler, node->name);
            if (hoisted && hoisted->is_hoisted) {
                // Already hoisted — compile call expr, then MOVE result to hoisted register
                XrExprDesc expr = xr_compile_expr(ctx, compiler, node->initializer);
                hoisted->is_hoisted = false;
                local_set_compile_type(hoisted, xr_type_for_local);
                hoisted->is_const = node->is_const;
                hoisted->comptime.type = COMPTIME_NONE;
                xexpr_to_specific_reg(ctx, compiler, &expr, hoisted->reg);
                xstmt_emit_box_if_raw(compiler->emitter, hoisted->reg, &expr);
                xstmt_emit_json_checktype(ctx, compiler, hoisted->reg, node->type_annotation,
                                          node->initializer);
                if (hoisted->is_captured && hoisted->ctx_slot >= 0) {
                    if (hoisted->is_cellified) {
                        int tmp = reg_alloc(ctx, compiler);
                        xemit_move(compiler->emitter, tmp, hoisted->reg);
                        xemit_cell_set(compiler->emitter, hoisted->reg, tmp, 0);
                        reg_free(compiler, tmp);
                    } else if (!hoisted->is_const) {
                        xemit_cell_new(compiler->emitter, hoisted->reg);
                        hoisted->is_cellified = true;
                    }
                }
                return;
            }
            // Compile function call first
            XrExprDesc expr = xr_compile_expr(ctx, compiler, node->initializer);
            int expr_reg = expr.reg;

            // Define local variable, specify using expression result's register
            XrLocalInfo *local = scope_define_local_reg(ctx, compiler, name_str, expr_reg);
            local_set_compile_type(local, xr_type_for_local);
            local->is_const = node->is_const;
            local->comptime.type = COMPTIME_NONE;
            xstmt_emit_box_if_raw(compiler->emitter, local->reg, &expr);
            xstmt_emit_json_checktype(ctx, compiler, local->reg, node->type_annotation,
                                      node->initializer);
            if (local->is_captured && local->ctx_slot >= 0) {
                if (local->is_cellified) {
                    int tmp = reg_alloc(ctx, compiler);
                    xemit_move(compiler->emitter, tmp, local->reg);
                    xemit_cell_set(compiler->emitter, local->reg, tmp, 0);
                    reg_free(compiler, tmp);
                } else if (!local->is_const) {
                    xemit_cell_new(compiler->emitter, local->reg);
                    local->is_cellified = true;
                }
            }
            // No MOVE needed, register already set correctly
            return;
        }

        /* Define local variable (only call once).
         * Note: shared variables and Channel already handled at the beginning.
         */
        // Check if already pre-registered by hoisting
        XrLocalInfo *local = compiler_get_local_by_name(compiler, node->name);
        if (local && local->is_hoisted && local->depth == compiler->scope_depth) {
            // Keep is_hoisted=true during initializer compilation so that
            // emit_ctx_sync_before_closure emits LOADNULL before CELL_NEW
            // for forward-reference support (e.g., recursive lambdas).
            // Cleared after initialization below.
        } else {
            local = scope_define_local(ctx, compiler, name_str);
        }
        local_set_compile_type(local, xr_type_for_local);
        local->is_const = node->is_const;
        local->storage_mode = node->storage_mode;

        // Try to extract compile-time constant value (only valid for const declarations)
        if (node->is_const && node->initializer) {
            if (!try_extract_comptime_value(node->initializer, &local->comptime)) {
                local->comptime.type = COMPTIME_NONE;
            }
            // Register to ConstEntry table for chain constant folding
            if (local->comptime.type == COMPTIME_INT) {
                xr_compiler_ctx_add_const_int(ctx, name_str, local->comptime.as.int_val);
            } else if (local->comptime.type == COMPTIME_FLOAT) {
                xr_compiler_ctx_add_const_float(ctx, name_str, local->comptime.as.float_val);
            }
        } else {
            local->comptime.type = COMPTIME_NONE;
        }

        int local_reg = local->reg;

        // If variable is already cellified, or will be cellified during initializer
        // compilation (captured mutable, or captured hoisted const), compile initializer
        // to a temp register so we don't overwrite the cell ref in local_reg.
        int init_reg = local_reg;
        bool needs_cell_set = false;
        bool may_cellify =
            local->is_captured && local->ctx_slot >= 0 && (!local->is_const || local->is_hoisted);
        if (local->is_cellified || may_cellify) {
            init_reg = reg_alloc(ctx, compiler);
            needs_cell_set = true;
        }

        /* ========== Test relocatable expression ========== */
        // Uninitialized variable, default to null
        if (!node->initializer) {
            xemit_loadnull(compiler->emitter, init_reg);
        }
        // Only handle simplest case: integer constant
        else if (node->initializer->type == AST_LITERAL_INT) {
            LiteralNode *lit = (LiteralNode *) &node->initializer->as;
            xr_Integer value = lit->raw_value.int_val;

            // Rematerialization optimization: mark small integer constant as rematerializable
            if (!needs_cell_set && value >= -MAXARG_sBx && value <= MAXARG_sBx) {
                local->can_rematerialize = true;
                local->remat_value = value;
            }

            // Create XrExprDesc — use XEXPR_INT for deferred discharge
            XrExprDesc expr;
            xexpr_init_int(&expr, value);
            // Discharge to init_reg
            xexpr_to_specific_reg(ctx, compiler, &expr, init_reg);
            xstmt_emit_box_if_raw(compiler->emitter, init_reg, &expr);
        }
        /* ========== Use xr_compile_expr for RELOC optimization ========== */
        else {
            // Set storage mode, elem_type and object type context
            uint8_t old_storage_mode = ctx->current_storage_mode;
            uint8_t old_elem_type = ctx->current_elem_tid;
            uint8_t old_key_tid = ctx->current_key_tid;
            XrType *old_object_type = ctx->current_object_type;
            ctx->current_storage_mode = node->storage_mode;
            ctx->current_elem_tid = extract_elem_tid(node->type_annotation);
            ctx->current_key_tid = extract_key_tid(node->type_annotation);
            if (node->type_annotation) {
                XrType *ta = node->type_annotation;
                if ((ta->kind == XR_KIND_JSON) && !ta->object.allow_extension &&
                    ta->object.field_count > 0)
                    ctx->current_object_type = ta;
            }

            // Compile initializer expression
            XrExprDesc expr = xr_compile_expr(ctx, compiler, node->initializer);

            // Register reuse optimization: function call return value directly as local variable.
            if (!needs_cell_set) {
                if ((expr.kind == XEXPR_CALL || expr.kind == XEXPR_TEMP) && expr.reg >= 0 &&
                    expr.reg == local_reg) {
                    // Expression result is exactly in local_reg, perfect reuse
                } else {
                    xexpr_to_specific_reg(ctx, compiler, &expr, local_reg);
                }
                xstmt_emit_box_if_raw(compiler->emitter, local_reg, &expr);
                xstmt_emit_json_checktype(ctx, compiler, local_reg, node->type_annotation,
                                          node->initializer);
            } else {
                // Cellified: compile to init_reg (temp) to preserve cell ref in local_reg
                xexpr_to_specific_reg(ctx, compiler, &expr, init_reg);
                xstmt_emit_box_if_raw(compiler->emitter, init_reg, &expr);
                xstmt_emit_json_checktype(ctx, compiler, init_reg, node->type_annotation,
                                          node->initializer);
            }

            // Struct value semantics: copy-on-assign for value types
            // Skip struct literals and new expressions (already fresh objects)
            if (local->compile_type && local->compile_type->is_value_type) {
                AstNodeType init_type = node->initializer->type;
                if (init_type != AST_STRUCT_LITERAL && init_type != AST_NEW_EXPR) {
                    xstmt_emit_value_copy(ctx, compiler, local_reg, local_reg, local->compile_type);
                }
            }

            // Restore context
            ctx->current_storage_mode = old_storage_mode;
            ctx->current_elem_tid = old_elem_type;
            ctx->current_key_tid = old_key_tid;
            ctx->current_object_type = old_object_type;
        }
        local->is_hoisted = false;  // Now clear is_hoisted after initialization
        // Cellify captured variable after initialization
        if (local->is_captured && local->ctx_slot >= 0) {
            if (needs_cell_set && local->is_cellified) {
                // Cell was pre-created by emit_ctx_sync during initializer compilation.
                // Write init value from init_reg into the cell.
                xemit_cell_set(compiler->emitter, local_reg, init_reg, 0);
                reg_free(compiler, init_reg);
            } else if (needs_cell_set) {
                // Predicted cellification but emit_ctx_sync didn't fire.
                // Move init value to local_reg and create cell.
                xemit_move(compiler->emitter, local_reg, init_reg);
                reg_free(compiler, init_reg);
                if (!local->is_const) {
                    xemit_cell_new(compiler->emitter, local_reg);
                    local->is_cellified = true;
                }
            } else if (!local->is_const && !local->is_cellified) {
                xemit_cell_new(compiler->emitter, local_reg);
                local->is_cellified = true;
            }
        } else if (needs_cell_set) {
            // Not captured: discard temp, move to local
            xemit_move(compiler->emitter, local_reg, init_reg);
            reg_free(compiler, init_reg);
        }
    }
}
