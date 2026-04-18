/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xstmt_simple.c - Simple statement compilation
 *
 * KEY CONCEPT:
 *   Handles simple statement types: expression statements, print,
 *   variable declarations, assignments, and destructuring.
 */

#include "xstmt.h"
#include "../../base/xchecks.h"
#include "../../runtime/xisolate_api.h"
#include "../../runtime/value/xtype.h"
#include "xcompiler.h"
#include "xcompiler_context.h"
#include "xemit.h"
#include "xexpr_desc.h"
#include "xexpr.h"
#include "xconst_fold.h"
#include "../../runtime/value/xtype.h"
#include "../../runtime/symbol/xsymbol_table.h"
#include "../../runtime/value/xtype_names.h"
#include "../../runtime/value/xstruct_layout.h"
#include "../../runtime/object/xarray.h"
#include "xcompiler_class_registry.h"
#include <stdio.h>

#include <string.h>

/*
 * Emit value-type copy: OP_STRUCT_COPY for structs with layout, OP_COPY as fallback.
 * Allocates new struct_area space and encodes slot offset in C operand.
 */
static void emit_value_copy(XrCompilerContext *ctx, XrCompiler *compiler,
                            int dest_reg, int src_reg, XrType *compile_type) {
    if (compile_type && ctx->class_registry) {
        const char *cn = NULL;
        if (compile_type->kind == XR_KIND_CLASS || compile_type->kind == XR_KIND_INSTANCE)
            cn = compile_type->instance.class_name;
        if (cn) {
            ClassInfo *ci = xr_class_registry_lookup(ctx->class_registry, cn);
            if (ci && ci->struct_layout) {
                int alloc_size = 8 + ci->struct_layout->total_size;
                int aligned_size = (alloc_size + 15) & ~15;
                int slot_offset = compiler->struct_area_offset / 16;
                compiler->struct_area_offset += aligned_size;
                emit_abc(compiler->emitter, OP_STRUCT_COPY, dest_reg, src_reg, slot_offset);
                return;
            }
        }
    }
    emit_abc(compiler->emitter, OP_COPY, dest_reg, src_reg, 0);
}

// Human-readable type name from XrType (for error messages)
static const char *type_flag_name(XrType *type) {
    if (!type) return "unknown";
    if (XR_TYPE_IS_INT(type)) return "int";
    if (XR_TYPE_IS_FLOAT(type)) return "float";
    if (XR_TYPE_IS_STRING(type) || (type->kind == XR_KIND_STRING && type->is_literal)) return "string";
    if (XR_TYPE_IS_BOOL(type) || (type->kind == XR_KIND_BOOL && type->is_literal)) return "bool";
    if (type->kind == XR_KIND_NULL) return "null";
    if (type->kind == XR_KIND_ARRAY) return "Array";
    if (type->kind == XR_KIND_MAP) return "Map";
    if (type->kind == XR_KIND_SET) return "Set";
    if (type->kind == XR_KIND_JSON) return "Json";
    if (type->kind == XR_KIND_INSTANCE) {
        return type->instance.class_name ? type->instance.class_name : "instance";
    }
    if (type->kind == XR_KIND_CLASS) return "class";
    if (type->kind == XR_KIND_FUNCTION) return "function";
    if (type->kind == XR_KIND_CHANNEL) return "Channel";
    if (xr_type_is_named_class(type, "BigInt")) return "BigInt";
    if (type->kind == XR_KIND_UNION && type->union_type.member_count > 0) {
        static char buf[256];
        int pos = 0;
        for (int i = 0; i < type->union_type.member_count && pos < 240; i++) {
            if (i > 0) { buf[pos++] = ' '; buf[pos++] = '|'; buf[pos++] = ' '; }
            const char *m = type_flag_name(type->union_type.members[i]);
            int len = (int)strlen(m);
            if (pos + len >= 250) { memcpy(buf + pos, "...", 3); pos += 3; break; }
            memcpy(buf + pos, m, len);
            pos += len;
        }
        if (type->is_nullable && pos < 245) {
            memcpy(buf + pos, " | null", 7);
            pos += 7;
        }
        buf[pos] = '\0';
        return buf;
    }
    return "unknown";
}

// Extract element/value XrTypeId from a container type annotation
// Array<T>/Set<T>/Channel<T> → T's tid, Map<K,V> → V's tid
static uint8_t extract_elem_tid(XrType *type) {
    if (!type) return 0;
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

// Extract key XrTypeId from Map<K,V> type annotation
static uint8_t extract_key_tid(XrType *type) {
    if (!type) return 0;
    if (type->kind == XR_KIND_MAP) {
        XrType *kt = type->map.key_type;
        return xr_type_to_tid(kt);
    }
    return 0;
}

// BOX raw expression at assignment boundary.
// Locals are always tagged — if expr is raw I64/F64, emit BOX.
static void emit_box_if_raw(XrEmitter *emitter, int reg, const XrExprDesc *expr) {
    if (xexpr_is_raw_i64(expr)) {
        emit_abc(emitter, OP_BOX_I64, reg, reg, 0);
    } else if (xexpr_is_raw_f64(expr)) {
        emit_abc(emitter, OP_BOX_F64, reg, reg, 0);
    }
}

/*
 * Convert XrTypeKind to XrTypeId for CHECKTYPE bitmask.
 * Returns -1 if the kind has no direct TID mapping.
 */
static int kind_to_tid(XrTypeKind kind) {
    switch (kind) {
    case XR_KIND_INT:    return XR_TID_INT;
    case XR_KIND_FLOAT:  return XR_TID_FLOAT;
    case XR_KIND_STRING: return XR_TID_STRING;
    case XR_KIND_BOOL:   return XR_TID_BOOL;
    case XR_KIND_JSON:   return XR_TID_JSON;
    case XR_KIND_ARRAY:  return XR_TID_ARRAY;
    case XR_KIND_NULL:   return XR_TID_NULL;
    default:             return -1;
    }
}

/*
 * Build bitmask from target type for OP_CHECKTYPE.
 * Single primitive: one bit set.  Union: OR of all member bits.
 * Returns 0 if no valid bitmask can be built.
 */
static int64_t build_checktype_mask(XrType *target) {
    if (!target) return 0;
    int tid = kind_to_tid(target->kind);
    if (tid >= 0) return (1LL << tid);
    if (target->kind == XR_KIND_UNION) {
        int64_t mask = 0;
        for (int i = 0; i < target->union_type.member_count; i++) {
            int mt = kind_to_tid(target->union_type.members[i]->kind);
            if (mt < 0) return 0;
            mask |= (1LL << mt);
        }
        if (target->is_nullable) mask |= (1LL << XR_TID_NULL);
        return mask;
    }
    return 0;
}

/*
 * Emit OP_CHECKTYPE for Json/JsonValue → concrete type coercion.
 * Uses bitmask encoding: single primitive = one bit, union = OR of member bits.
 */
static void emit_json_checktype(XrCompilerContext *ctx, XrCompiler *compiler,
                                 int reg, XrType *declared_type, AstNode *init_expr) {
    if (!declared_type || !init_expr) return;

    XrType *init_type = get_expr_type(ctx, compiler, init_expr);
    if (!init_type || !xr_is_json_coercion(declared_type, init_type)) return;

    int64_t mask = build_checktype_mask(declared_type);
    if (mask != 0) {
        int type_const = xr_vm_proto_add_constant(compiler->proto, xr_int(mask));
        emit_abc(compiler->emitter, OP_CHECKTYPE, reg, type_const, 0);
    }
}

/* ========== Expression Statement ========== */

/*
 * Compile expression statement.
 *
 * When expression is used as statement, result is discarded.
 *
 * Special handling: assignment, member set, index set.
 */
void compile_expr_stmt(XrCompilerContext *ctx, XrCompiler *compiler, AstNode *expr) {
    XR_DCHECK(ctx != NULL, "compile_expr_stmt: NULL ctx");
    XR_DCHECK(compiler != NULL, "compile_expr_stmt: NULL compiler");
    XR_DCHECK(expr != NULL, "compile_expr_stmt: NULL expr");
    // Handle assignment statements (they are not expressions)
    if (expr->type == AST_ASSIGNMENT) {
        compile_assignment(ctx, compiler, &expr->as.assignment);
        return;
    }

    // Handle compound assignment statements (they are not expressions)
    if (expr->type == AST_COMPOUND_ASSIGNMENT) {
        compile_compound_assignment(ctx, compiler, &expr->as.compound_assignment);
        return;
    }

    // Handle increment statement
    if (expr->type == AST_INC) {
        compile_inc(ctx, compiler, &expr->as.inc);
        return;
    }

    // Handle decrement statement
    if (expr->type == AST_DEC) {
        compile_dec(ctx, compiler, &expr->as.dec);
        return;
    }

    // Handle member set statement
    if (expr->type == AST_MEMBER_SET) {
        compile_member_set(ctx, compiler, &expr->as.member_set);
        return;
    }

    // Handle index set statement
    if (expr->type == AST_INDEX_SET) {
        compile_index_set(ctx, compiler, &expr->as.index_set);
        return;
    }

    // Optimization: await as statement (no return value) → set C=1 to skip deep copy
    if (expr->type == AST_AWAIT_EXPR) {
        AwaitExprNode *await_node = &expr->as.await_expr;
        XrExprDesc expr_desc = xr_compile_expr(ctx, compiler, await_node->expr);
        int coro_reg = xexpr_to_anyreg(ctx, compiler, &expr_desc);
        int target = reg_alloc(ctx, compiler);
        if (await_node->is_any_success) {
            emit_abc(compiler->emitter, OP_AWAIT_ANY, target, coro_reg, 1);
        } else if (await_node->is_any) {
            emit_abc(compiler->emitter, OP_AWAIT_ANY, target, coro_reg, 0);
        } else if (await_node->is_all || await_node->expr->type == AST_ARRAY_LITERAL) {
            emit_abc(compiler->emitter, OP_AWAIT_ALL, target, coro_reg, 0);
        } else if (await_node->timeout != NULL) {
            XrExprDesc timeout_desc = xr_compile_expr(ctx, compiler, await_node->timeout);
            int timeout_reg = xexpr_to_anyreg(ctx, compiler, &timeout_desc);
            emit_abc(compiler->emitter, OP_AWAIT_TIMEOUT, target, coro_reg, timeout_reg);
        } else {
            // C=1: signal VM to skip deep copy (result discarded)
            emit_abc(compiler->emitter, OP_AWAIT, target, coro_reg, 1);
        }
        reg_free(compiler, target);
        return;
    }

    // Optimization: go as statement (fire-and-forget) → C bit 7 flag in SPAWN_CONT
    // Enables deferred coroutine recycling: child coro won't be awaited.
    if (expr->type == AST_GO_EXPR) {
        int target = reg_alloc(ctx, compiler);
        compile_go_expr(ctx, compiler, &expr->as.go_expr, target, true);
        reg_free(compiler, target);
        return;
    }

    // Other expression statements
    XrExprDesc e = xr_compile_expr(ctx, compiler, expr);

    // Fix: VOID expressions (like push with no return value) don't need register allocation
    if (e.kind == XEXPR_VOID) {
        return;
    }

    int reg = xexpr_to_anyreg(ctx, compiler, &e);
    reg_free(compiler, reg);
}

/* ========== Variable Declaration ========== */

/*
 * Try to extract compile-time constant value.
 *
 * If expression is literal or constant expression, returns true and fills out_value.
 * Otherwise returns false.
 */
static bool try_extract_comptime_value(AstNode *expr, ComptimeValue *out_value) {
    if (expr == NULL) return false;

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
            if (range->start && range->start->type == AST_LITERAL_INT &&
                range->end && range->end->type == AST_LITERAL_INT) {
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
        xr_compiler_error(ctx, compiler, "Variable '%s' must have a type annotation or initializer", node->name);
        return;
    }

    // Create string
    XrString *name_str = xr_compile_time_intern(ctx->X, node->name, strlen(node->name));

    // Check if variable already defined in current scope (forbid redefinition, allow shadowing)
    XrLocalInfo *existing = compiler_get_local_by_name(compiler, node->name);
    if (existing && existing->depth == compiler->scope_depth && !existing->is_hoisted) {
        // REPL mode: allow redefinition at top level
        if (!is_repl_top_level(ctx, compiler)) {
            xr_compiler_error(ctx, compiler, "Variable '%s' already defined, cannot redefine", node->name);
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
    // Skip for shared/Channel/escaped variables: they may have been pre-registered in Phase 1 hoisting
    if (compiler->scope_depth == 0 && node->storage_mode != XR_STORAGE_SHARED &&
        !is_channel && !node->is_escaped &&
        shared_get_in_scope(ctx, compiler, name_str) >= 0) {
        // REPL mode: allow redefinition (shared_add handles reuse)
        if (!ctx->repl_mode) {
            xr_compiler_error(ctx, compiler, "Variable '%s' already defined, cannot redefine", node->name);
            return;
        }
    }
    // Shared path: explicit 'shared', Channel auto-promotion, or escape analysis
    // - shared: always global heap (user-specified performance hint)
    // - Channel: always system-heap allocated with refcount
    // - is_escaped: variable used in 'move' within go/ch.send (auto-promoted)
    if (node->storage_mode == XR_STORAGE_SHARED || is_channel || node->is_escaped) {
        // Channel no longer requires shared const — can be declared as let/const.
        // Channel objects are allocated on system heap (shared) with refcount,
        // and should be passed to coroutines via arguments (deep_copy does incref).

        // Allocate shared_array index (reuse if pre-registered in Phase 1 hoisting)
        int shared_index = shared_get_or_add(ctx, compiler, name_str);
        shared_set_const(ctx, shared_index, node->is_const);

        // shared const constant inlining optimization: compile-time eval and register to constant table
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
                if ((ta->kind == XR_KIND_JSON) && !ta->object.allow_extension && ta->object.field_count > 0)
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
            bool is_direct = (init_type == AST_ARRAY_LITERAL ||
                             init_type == AST_MAP_LITERAL ||
                             init_type == AST_SET_LITERAL ||
                             init_type == AST_OBJECT_LITERAL ||
                             init_type == AST_NEW_EXPR ||
                             init_type == AST_CHANNEL_NEW);
            if (!is_direct) {
                emit_abc(compiler->emitter, OP_TO_SHARED, reg, reg, 0);
            }
        } else {
            emit_abc(compiler->emitter, OP_LOADNULL, reg, 0, 0);
        }
        emit_abx(compiler->emitter, OP_SETSHARED, reg, shared_index);
        reg_free(compiler, reg);

        // Store type info in shared variable table for type inference
        if (is_channel) {
            // Use type annotation if available (e.g., Channel<int>), otherwise Channel<unknown>
            XrType *ch_type = node->type_annotation;
            if (!ch_type || !(ch_type->kind == XR_KIND_CHANNEL)) {
                ch_type = xr_type_new_channel(xr_type_new_unknown());
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
                xr_compiler_error(ctx, compiler, "Variable '%s' already defined, cannot redefine", node->name);
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
                if (ct && (ct->kind == XR_KIND_NULL)) ct = NULL;
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
                (void)old_key_tid;
                XrType *old_object_type = ctx->current_object_type;
                ctx->current_storage_mode = node->storage_mode;
                ctx->current_elem_tid = extract_elem_tid(node->type_annotation);
                ctx->current_key_tid = extract_key_tid(node->type_annotation);
                if (node->type_annotation) {
                    XrType *ta = node->type_annotation;
                    if ((ta->kind == XR_KIND_JSON) && !ta->object.allow_extension && ta->object.field_count > 0)
                        ctx->current_object_type = ta;
                }

                XrExprDesc expr = xr_compile_expr(ctx, compiler, node->initializer);
                xexpr_to_specific_reg(ctx, compiler, &expr, m_init_reg);
                emit_box_if_raw(compiler->emitter, m_init_reg, &expr);

                // Json→concrete runtime type check
                emit_json_checktype(ctx, compiler, m_init_reg, node->type_annotation, node->initializer);

                // Struct value semantics: copy-on-assign
                if (local->compile_type && local->compile_type->is_value_type) {
                    AstNodeType init_type = node->initializer->type;
                    if (init_type != AST_STRUCT_LITERAL && init_type != AST_NEW_EXPR) {
                        emit_value_copy(ctx, compiler, m_init_reg, m_init_reg, local->compile_type);
                    }
                }

                ctx->current_storage_mode = old_storage_mode;
                ctx->current_elem_tid = old_elem_type;
                ctx->current_object_type = old_object_type;
            } else {
                emit_abc(compiler->emitter, OP_LOADNULL, m_init_reg, 0, 0);
            }
            if (local->is_captured && local->ctx_slot >= 0) {
                if (m_needs_cell_set) {
                    emit_abc(compiler->emitter, OP_CELL_SET, local->reg, m_init_reg, 0);
                    reg_free(compiler, m_init_reg);
                } else if (!local->is_const && !local->is_cellified) {
                    emit_abc(compiler->emitter, OP_CELL_NEW, local->reg, 0, 0);
                    local->is_cellified = true;
                }
            } else if (m_needs_cell_set) {
                reg_free(compiler, m_init_reg);
            }
            return;  // Handled as local variable
        }

        // Non-module top-level (e.g. REPL) - use global variable
        if (node->initializer && node->initializer->type == AST_LITERAL_INT) {

            LiteralNode *lit = (LiteralNode *)&node->initializer->as;
            xr_Integer value = lit->raw_value.int_val;

            // Create XrExprDesc
            XrExprDesc expr;
            xexpr_init(&expr, XEXPR_VOID, -1);

            // Small integer uses LOADI - AsBx format!
            if (value >= -MAXARG_sBx && value <= MAXARG_sBx) {
                // LOADI is AsBx format, A=0 temporarily, sBx=value
                int pc = emit_asbx(compiler->emitter, OP_LOADI, 0, (int)value);
                expr.kind = XEXPR_RELOC;
                expr.u.pc = pc;
                expr.reg = -1;

                // Successfully generated RELOC expression
            } else {
                XrValue val = xr_int(value);
                int kidx = xr_vm_proto_add_constant(compiler->proto, val);
                int pc = emit_abx(compiler->emitter, OP_LOADK, 0, kidx);

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
            emit_abx(compiler->emitter, OP_SETSHARED, reg, shared_index);
            reg_free(compiler, reg);
        } else if (node->initializer) {
            // Other expression types
            XrExprDesc expr = xr_compile_expr(ctx, compiler, node->initializer);
            int reg = xexpr_to_anyreg(ctx, compiler, &expr);
            int shared_index = shared_get_or_add(ctx, compiler, name_str);
            shared_set_const(ctx, shared_index, node->is_const);
            emit_abx(compiler->emitter, OP_SETSHARED, reg, shared_index);
            reg_free(compiler, reg);
        } else {
            // Uninitialized variable, default to null
            int reg = reg_alloc(ctx, compiler);
            emit_abc(compiler->emitter, OP_LOADNULL, reg, 0, 0);
            int shared_index = shared_get_or_add(ctx, compiler, name_str);
            emit_abx(compiler->emitter, OP_SETSHARED, reg, shared_index);
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
                    XrType *base = xr_type_non_nullable(check_init);
                    if (base) check_init = base;
                }
                if (check_init && !xr_type_assignable(inferred_compile_type, check_init)) {
                    // Json/JsonValue→primitive/union: allowed with runtime type check (OP_CHECKTYPE)
                    if (!xr_is_json_coercion(inferred_compile_type, check_init)) {
                        xr_compiler_error(ctx, compiler,
                            "Cannot initialize %s variable '%s' with %s value",
                            type_flag_name(inferred_compile_type), node->name, type_flag_name(check_init));
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
                emit_box_if_raw(compiler->emitter, hoisted->reg, &expr);
                emit_json_checktype(ctx, compiler, hoisted->reg, node->type_annotation, node->initializer);
                if (hoisted->is_captured && hoisted->ctx_slot >= 0) {
                    if (hoisted->is_cellified) {
                        int tmp = reg_alloc(ctx, compiler);
                        emit_abc(compiler->emitter, OP_MOVE, tmp, hoisted->reg, 0);
                        emit_abc(compiler->emitter, OP_CELL_SET, hoisted->reg, tmp, 0);
                        reg_free(compiler, tmp);
                    } else if (!hoisted->is_const) {
                        emit_abc(compiler->emitter, OP_CELL_NEW, hoisted->reg, 0, 0);
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
            emit_box_if_raw(compiler->emitter, local->reg, &expr);
            emit_json_checktype(ctx, compiler, local->reg, node->type_annotation, node->initializer);
            if (local->is_captured && local->ctx_slot >= 0) {
                if (local->is_cellified) {
                    int tmp = reg_alloc(ctx, compiler);
                    emit_abc(compiler->emitter, OP_MOVE, tmp, local->reg, 0);
                    emit_abc(compiler->emitter, OP_CELL_SET, local->reg, tmp, 0);
                    reg_free(compiler, tmp);
                } else if (!local->is_const) {
                    emit_abc(compiler->emitter, OP_CELL_NEW, local->reg, 0, 0);
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
        bool may_cellify = local->is_captured && local->ctx_slot >= 0 &&
                           (!local->is_const || local->is_hoisted);
        if (local->is_cellified || may_cellify) {
            init_reg = reg_alloc(ctx, compiler);
            needs_cell_set = true;
        }

        /* ========== Test relocatable expression ========== */
        // Uninitialized variable, default to null
        if (!node->initializer) {
            emit_abc(compiler->emitter, OP_LOADNULL, init_reg, 0, 0);
        }
        // Only handle simplest case: integer constant
        else if (node->initializer->type == AST_LITERAL_INT) {
            LiteralNode *lit = (LiteralNode *)&node->initializer->as;
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
            emit_box_if_raw(compiler->emitter, init_reg, &expr);
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
                if ((ta->kind == XR_KIND_JSON) && !ta->object.allow_extension && ta->object.field_count > 0)
                    ctx->current_object_type = ta;
            }

            // Compile initializer expression
            XrExprDesc expr = xr_compile_expr(ctx, compiler, node->initializer);

            // Register reuse optimization: function call return value directly as local variable.
            if (!needs_cell_set) {
                if ((expr.kind == XEXPR_CALL || expr.kind == XEXPR_TEMP) &&
                    expr.reg >= 0 && expr.reg == local_reg) {
                    // Expression result is exactly in local_reg, perfect reuse
                } else {
                    xexpr_to_specific_reg(ctx, compiler, &expr, local_reg);
                }
                emit_box_if_raw(compiler->emitter, local_reg, &expr);
                emit_json_checktype(ctx, compiler, local_reg, node->type_annotation, node->initializer);
            } else {
                // Cellified: compile to init_reg (temp) to preserve cell ref in local_reg
                xexpr_to_specific_reg(ctx, compiler, &expr, init_reg);
                emit_box_if_raw(compiler->emitter, init_reg, &expr);
                emit_json_checktype(ctx, compiler, init_reg, node->type_annotation, node->initializer);
            }

            // Struct value semantics: copy-on-assign for value types
            // Skip struct literals and new expressions (already fresh objects)
            if (local->compile_type && local->compile_type->is_value_type) {
                AstNodeType init_type = node->initializer->type;
                if (init_type != AST_STRUCT_LITERAL && init_type != AST_NEW_EXPR) {
                    emit_value_copy(ctx, compiler, local_reg, local_reg, local->compile_type);
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
                emit_abc(compiler->emitter, OP_CELL_SET, local_reg, init_reg, 0);
                reg_free(compiler, init_reg);
            } else if (needs_cell_set) {
                // Predicted cellification but emit_ctx_sync didn't fire.
                // Move init value to local_reg and create cell.
                emit_abc(compiler->emitter, OP_MOVE, local_reg, init_reg, 0);
                reg_free(compiler, init_reg);
                if (!local->is_const) {
                    emit_abc(compiler->emitter, OP_CELL_NEW, local_reg, 0, 0);
                    local->is_cellified = true;
                }
            } else if (!local->is_const && !local->is_cellified) {
                emit_abc(compiler->emitter, OP_CELL_NEW, local_reg, 0, 0);
                local->is_cellified = true;
            }
        } else if (needs_cell_set) {
            // Not captured: discard temp, move to local
            emit_abc(compiler->emitter, OP_MOVE, local_reg, init_reg, 0);
            reg_free(compiler, init_reg);
        }
    }
}

/* ========== Print Statement ========== */

/*
 * Compile print statement (supports multiple arguments).
 *
 * print(a, b, c) compiles to:
 *   OP_PRINT R[a] 0 0    // first arg, no space, no newline
 *   OP_PRINT R[b] 1 0    // second arg, space before, no newline
 *   OP_PRINT R[c] 1 1    // last arg, space before, newline
 *
 * Instruction format: OP_PRINT A B C
 *   A: value register
 *   B: 1=add space before
 *   C: 1=newline after print
 */
void compile_print(XrCompilerContext *ctx, XrCompiler *compiler, PrintNode *node) {
    XR_DCHECK(ctx != NULL, "compile_print: NULL ctx");
    XR_DCHECK(compiler != NULL, "compile_print: NULL compiler");
    XR_DCHECK(node != NULL, "compile_print: NULL node");
    // If no arguments, just print newline
    if (node->expr_count == 0) {
        // Print empty string to trigger newline
        int reg = reg_alloc(ctx, compiler);
        XrString *empty = xr_compile_time_intern(ctx->X, "", 0);
        int const_idx = xr_vm_proto_add_constant(compiler->proto, xr_string_value(empty));
        emit_abx(compiler->emitter, OP_LOADK, reg, const_idx);
        emit_abc(compiler->emitter, OP_PRINT, reg, 0, 1);  // C=1 newline
        reg_free(compiler, reg);
        return;
    }

    // Compile and print each expression
    for (int i = 0; i < node->expr_count; i++) {
        XrExprDesc expr = xr_compile_expr(ctx, compiler, node->exprs[i]);

        // C: bit0=newline, bit1-2=slot_hint (0=ANY, 1=I64, 2=F64)
        int slot_hint = 0;
        if (xexpr_is_raw_i64(&expr))      slot_hint = 1;
        else if (xexpr_is_raw_f64(&expr)) slot_hint = 2;

        // Typed values: readonly (no BOX), hint tells VM the raw type
        // Any values: anyreg (auto-BOX if needed)
        int reg = (slot_hint != 0)
            ? xexpr_to_anyreg_readonly(ctx, compiler, &expr)
            : xexpr_to_anyreg(ctx, compiler, &expr);

        int add_space = (i > 0) ? 1 : 0;
        int newline = (i == node->expr_count - 1) ? 1 : 0;
        int c_field = newline | (slot_hint << 1);

        emit_abc(compiler->emitter, OP_PRINT, reg, add_space, c_field);
        reg_free(compiler, reg);
    }
}

/* ========== Assignment Statement ========== */

/*
 * Compile assignment statement.
 *
 * Handles local variables, upvalue, and global variable assignment.
 */
void compile_assignment(XrCompilerContext *ctx, XrCompiler *compiler, AssignmentNode *node) {
    XR_DCHECK(ctx != NULL, "compile_assignment: NULL ctx");
    XR_DCHECK(compiler != NULL, "compile_assignment: NULL compiler");
    XR_DCHECK(node != NULL, "compile_assignment: NULL node");
    // Create string
    XrString *name_str = xr_compile_time_intern(ctx->X, node->name, strlen(node->name));

    // Check if trying to overwrite constant (including fn-declared functions and const variables)
    XrLocalInfo *local_info = compiler_get_local_by_name(compiler, node->name);
    if (local_info && local_info->is_const) {
        xr_compiler_error(ctx, compiler, "Cannot modify constant '%s'", node->name);
        return;
    }

    // Check shared constant (considering lexical scope)
    // Skip if local variable exists: local shadows shared
    int shared_index = shared_get_in_scope(ctx, compiler, name_str);
    if (!local_info && shared_index >= 0 && shared_is_const(ctx, shared_index)) {
        xr_compiler_error(ctx, compiler, "Cannot modify shared const '%s'", node->name);
        return;
    }

    // Builtins are read-only, cannot be assigned
    if (!local_info && shared_index < 0 && builtin_get(ctx, name_str) >= 0) {
        xr_compiler_error(ctx, compiler, "Cannot assign to built-in '%s'", node->name);
        return;
    }

    // First lookup variable, then decide how to compile expression
    int local = scope_resolve_local(compiler, name_str);
    if (local >= 0) {
        // Unified type compatibility check (first gate: compile-time)
        // Covers all typed locals: int, float, string, bool, Array, Map, class, etc.
        // Skip: unknown (unresolved), null (inferred from null literal = untyped)
        if (local_info && local_info->compile_type) {
            XrType *target_type = local_info->compile_type;
            if (target_type->kind != XR_KIND_UNKNOWN) {
                XrType *expr_type = get_expr_type(ctx, compiler, node->value);
                // Strip nullable from source: int? → int for base type check
                // (runtime UNBOX handles actual null values)
                XrType *check_source = expr_type;
                if (check_source && check_source->is_nullable) {
                    XrType *base = xr_type_non_nullable(check_source);
                    if (base) check_source = base;
                }
                if (check_source && !xr_type_assignable(target_type, check_source)) {
                    // Json/JsonValue→primitive/union: allowed with runtime type check (OP_CHECKTYPE)
                    if (!xr_is_json_coercion(target_type, check_source)) {
                        if (XR_TYPE_IS_INT(target_type) && XR_TYPE_IS_FLOAT(check_source)) {
                            xr_compiler_error(ctx, compiler,
                                "Cannot assign float to int variable '%s' (use int() for explicit conversion)",
                                node->name);
                        } else {
                            xr_compiler_error(ctx, compiler,
                                "Cannot assign %s to %s variable '%s'",
                                type_flag_name(check_source), type_flag_name(target_type), node->name);
                        }
                        return;
                    }
                }
            }
        }

        if (local_info && local_info->is_cellified) {
            // Cellified local: write through existing cell (CELL_SET).
            // Must NOT overwrite the cell ref in R[local].
            XrExprDesc expr = xr_compile_expr(ctx, compiler, node->value);
            int value_reg = xexpr_to_anyreg(ctx, compiler, &expr);
            if (local_info) emit_box_if_raw(compiler->emitter, value_reg, &expr);
            if (local_info && local_info->compile_type)
                emit_json_checktype(ctx, compiler, value_reg, local_info->compile_type, node->value);
            emit_abc(compiler->emitter, OP_CELL_SET, local, value_reg, 0);
            reg_free(compiler, value_reg);
        } else {
            // Normal local: discharge directly to target register
            XrExprDesc expr = xr_compile_expr(ctx, compiler, node->value);
            xexpr_to_specific_reg(ctx, compiler, &expr, local);
            // BOX if assigning raw to tagged local
            if (local_info) {
                emit_box_if_raw(compiler->emitter, local, &expr);
            }
            if (local_info && local_info->compile_type)
                emit_json_checktype(ctx, compiler, local, local_info->compile_type, node->value);
            // Struct value semantics: copy-on-assign
            if (local_info && local_info->compile_type && local_info->compile_type->is_value_type) {
                AstNodeType val_type = node->value->type;
                if (val_type != AST_STRUCT_LITERAL && val_type != AST_NEW_EXPR) {
                    emit_value_copy(ctx, compiler, local, local, local_info->compile_type);
                }
            }
        }
    } else if (shared_index >= 0) {
        // shared variable assignment (xexpr_to_anyreg auto-BOXes typed values)
        XrExprDesc expr = xr_compile_expr(ctx, compiler, node->value);
        int value_reg = xexpr_to_anyreg(ctx, compiler, &expr);
        emit_abx(compiler->emitter, OP_SETSHARED, value_reg, shared_index);
        reg_free(compiler, value_reg);
    } else {
        // Non-local variable: normal compilation (xexpr_to_anyreg auto-BOXes typed values)
        XrExprDesc expr = xr_compile_expr(ctx, compiler, node->value);
        int value_reg = xexpr_to_anyreg(ctx, compiler, &expr);

        int upvalue = scope_resolve_upvalue(ctx, compiler, name_str);
        if (upvalue >= 0) {
            // Upvalue assignment: UPVAL_GET (cell ref) + CELL_SET
            int cell_reg = reg_alloc(ctx, compiler);
            emit_abc(compiler->emitter, OP_UPVAL_GET, cell_reg, upvalue, 0);
            emit_abc(compiler->emitter, OP_CELL_SET, cell_reg, value_reg, 0);
            reg_free(compiler, cell_reg);
            reg_free(compiler, value_reg);
        } else {
            // Top-level variable assignment - check shared first, then predefined globals
            int shared_index = shared_get_in_scope(ctx, compiler, name_str);
            if (shared_index >= 0) {
                emit_abx(compiler->emitter, OP_SETSHARED, value_reg, shared_index);
                reg_free(compiler, value_reg);
            } else {
                int global_index = builtin_get(ctx, name_str);
                if (global_index < 0) {
                    xr_compiler_error(ctx, compiler,
                        "Undefined variable '%s', use 'let %s = ...' to define first",
                        name_str->data, name_str->data);
                    reg_free(compiler, value_reg);
                    return;
                }
                xr_compiler_error(ctx, compiler,
                    "Cannot assign to built-in '%s'", name_str->data);
                reg_free(compiler, value_reg);
            }
        }
    }
}

// Destructuring compilation is in xstmt_destructure.c

/*
 * Compile compound assignment (e.g., +=, -=, *=, /=, %=, &=, |=, ^=, <<=, >>=)
 *
 * Decomposed to individual binary ops: R[A] = R[A] op R[B].
 * No fused OP_COMPOUND_ASSIGN — each operator maps to a standard opcode
 * (OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_BAND, OP_BOR, OP_BXOR,
 * OP_SHL, OP_SHR), giving JIT/AOT independent type-inference per op.
 */
void compile_compound_assignment(XrCompilerContext *ctx, XrCompiler *compiler, CompoundAssignmentNode *node) {
    XR_DCHECK(ctx != NULL, "compile_compound_assignment: NULL ctx");
    XR_DCHECK(compiler != NULL, "compile_compound_assignment: NULL compiler");
    XR_DCHECK(node != NULL, "compile_compound_assignment: NULL node");

    // Map token to binary opcode
    OpCode binary_op;
    switch (node->op) {
        case TK_PLUS_ASSIGN:   binary_op = OP_ADD;  break;
        case TK_MINUS_ASSIGN:  binary_op = OP_SUB;  break;
        case TK_MUL_ASSIGN:    binary_op = OP_MUL;  break;
        case TK_DIV_ASSIGN:    binary_op = OP_DIV;  break;
        case TK_MOD_ASSIGN:    binary_op = OP_MOD;  break;
        case TK_AND_ASSIGN:    binary_op = OP_BAND; break;
        case TK_OR_ASSIGN:     binary_op = OP_BOR;  break;
        case TK_XOR_ASSIGN:    binary_op = OP_BXOR; break;
        case TK_LSHIFT_ASSIGN: binary_op = OP_SHL;  break;
        case TK_RSHIFT_ASSIGN: binary_op = OP_SHR;  break;
        default:
            xr_compiler_error(ctx, compiler, "Unsupported compound assignment operator");
            return;
    }

    bool is_arithmetic = (binary_op == OP_ADD || binary_op == OP_SUB ||
                          binary_op == OP_MUL || binary_op == OP_DIV ||
                          binary_op == OP_MOD);

    // Check if member access compound assignment (this.field += value)
    if (node->object != NULL) {
        /* === Member access compound assignment ===
         * Strategy:
         * 1. Compile object to register
         * 2. Read member value to temp register
         * 3. Execute binary operation: R[member] = R[member] op R[value]
         * 4. Write back member value
         */

        // Compile object expression
        XrExprDesc obj_expr = xr_compile_expr(ctx, compiler, node->object);
        int obj_reg = xexpr_to_anyreg(ctx, compiler, &obj_expr);

        // Member name symbol index
        int global_sym = xr_symbol_register_in_table(
            (XrSymbolTable*)xr_isolate_get_symbol_table(ctx->X), node->name);
        int local_sym = emitter_add_symbol(compiler->emitter, global_sym);

        // Allocate temp register to store member value
        int member_reg = reg_alloc(ctx, compiler);

        // Read current member value: R[member_reg] = R[obj_reg].prop
        emit_abc(compiler->emitter, OP_GETPROP, member_reg, obj_reg, local_sym);

        // Compile right-side expression
        XrExprDesc value_expr = xr_compile_expr(ctx, compiler, node->value);
        int value_reg = xexpr_to_anyreg(ctx, compiler, &value_expr);

        // R[member_reg] = R[member_reg] op R[value_reg]
        emit_abc(compiler->emitter, binary_op, member_reg, member_reg, value_reg);

        // Write back member value: R[obj_reg].prop = R[member_reg]
        emit_abc(compiler->emitter, OP_SETPROP, obj_reg, local_sym, member_reg);

        // Free registers
        reg_free(compiler, value_reg);
        reg_free(compiler, member_reg);
        reg_free(compiler, obj_reg);
        return;
    }

    /* === Normal variable compound assignment === */
    // Create string
    XrString *name_str = xr_compile_time_intern(ctx->X, node->name, strlen(node->name));

    // Lookup variable and determine type
    int local = scope_resolve_local(compiler, name_str);
    int upvalue = -1;
    int shared_index = -1;
    int global_index = -1;
    int var_reg = -1;
    bool need_writeback = false;
    XrLocalInfo *ca_info = (local >= 0) ? compiler_get_local_by_name(compiler, node->name) : NULL;

    if (local >= 0) {
        // Type compatibility check for compound assignment
        if (ca_info && ca_info->compile_type && XR_TYPE_IS_INT(ca_info->compile_type) && is_arithmetic) {
            XrType *rhs_type = get_expr_type(ctx, compiler, node->value);
            if (rhs_type && XR_TYPE_IS_FLOAT(rhs_type) && !XR_TYPE_IS_INT(rhs_type)) {
                xr_compiler_error(ctx, compiler,
                    "Cannot use float in compound assignment to int variable '%s' (use int() for explicit conversion)",
                    node->name);
                return;
            }
            if (rhs_type && !XR_TYPE_IS_INT(rhs_type) && !XR_TYPE_IS_FLOAT(rhs_type)) {
                xr_compiler_error(ctx, compiler,
                    "Cannot use non-numeric value in compound assignment to int variable '%s'",
                    node->name);
                return;
            }
        }
        if (ca_info && ca_info->is_cellified) {
            // Cellified local: load current value from cell into temp
            var_reg = reg_alloc(ctx, compiler);
            emit_abc(compiler->emitter, OP_CELL_GET, var_reg, local, 0);
            need_writeback = true;
        } else {
            // Normal local: use its register directly
            var_reg = local;
        }
    } else {
        upvalue = scope_resolve_upvalue(ctx, compiler, name_str);
        if (upvalue >= 0) {
            var_reg = reg_alloc(ctx, compiler);
            // Upvalue: load cell ref, then deref cell
            int cell_reg = reg_alloc(ctx, compiler);
            emit_abc(compiler->emitter, OP_UPVAL_GET, cell_reg, upvalue, 0);
            emit_abc(compiler->emitter, OP_CELL_GET, var_reg, cell_reg, 0);
            reg_free(compiler, cell_reg);
            need_writeback = true;
        } else {
            shared_index = shared_get_in_scope(ctx, compiler, name_str);
            if (shared_index >= 0) {
                var_reg = reg_alloc(ctx, compiler);
                emit_abx(compiler->emitter, OP_GETSHARED, var_reg, shared_index);
                need_writeback = true;
            } else {
                global_index = builtin_get(ctx, name_str);
                if (global_index >= 0) {
                    var_reg = reg_alloc(ctx, compiler);
                    emit_abx(compiler->emitter, OP_GETBUILTIN, var_reg, global_index);
                    need_writeback = true;
                } else {
                    xr_compiler_error(ctx, compiler, "Undefined variable '%s'", name_str->data);
                    return;
                }
            }
        }
    }

    // Compile right-side expression
    XrExprDesc value_expr = xr_compile_expr(ctx, compiler, node->value);
    int value_reg = xexpr_to_anyreg(ctx, compiler, &value_expr);

    value_reg = xexpr_ensure_boxed(ctx, compiler, &value_expr, value_reg);
    // R[var_reg] = R[var_reg] op R[value_reg]
    emit_abc(compiler->emitter, binary_op, var_reg, var_reg, value_reg);

    // If needed, write back variable
    if (need_writeback) {
        if (local >= 0 && ca_info && ca_info->is_cellified) {
            // Cellified local: write back through cell
            emit_abc(compiler->emitter, OP_CELL_SET, local, var_reg, 0);
        } else if (upvalue >= 0) {
            // Upvalue: write back through cell
            int cell_reg = reg_alloc(ctx, compiler);
            emit_abc(compiler->emitter, OP_UPVAL_GET, cell_reg, upvalue, 0);
            emit_abc(compiler->emitter, OP_CELL_SET, cell_reg, var_reg, 0);
            reg_free(compiler, cell_reg);
        } else if (shared_index >= 0) {
            emit_abx(compiler->emitter, OP_SETSHARED, var_reg, shared_index);
        } else {
            xr_compiler_error(ctx, compiler, "Cannot assign to built-in '%s'", name_str->data);
        }
        reg_free(compiler, var_reg);
    }

    reg_free(compiler, value_reg);
}


/*
 * Compile increment statement.
 * ++x or x++ (uniformly handled as prefix)
 * Decomposed to ADDI R[A] R[A] 1 (no fused OP_INC).
 */
void compile_inc(XrCompilerContext *ctx, XrCompiler *compiler, IncDecNode *node) {
    XR_DCHECK(ctx != NULL, "compile_inc: NULL ctx");
    XR_DCHECK(compiler != NULL, "compile_inc: NULL compiler");
    XR_DCHECK(node != NULL, "compile_inc: NULL node");
    XrString *name_str = xr_compile_time_intern(ctx->X, node->name, strlen(node->name));

    int local = scope_resolve_local(compiler, name_str);
    int upvalue = -1;
    int shared_index = -1;
    int global_index = -1;
    int var_reg = -1;
    bool need_writeback = false;

    XrLocalInfo *local_info = (local >= 0) ? compiler_get_local_by_name(compiler, node->name) : NULL;
    if (local >= 0) {
        if (local_info && local_info->is_cellified) {
            var_reg = reg_alloc(ctx, compiler);
            emit_abc(compiler->emitter, OP_CELL_GET, var_reg, local, 0);
            need_writeback = true;
        } else {
            var_reg = local;
        }
    } else {
        upvalue = scope_resolve_upvalue(ctx, compiler, name_str);
        if (upvalue >= 0) {
            var_reg = reg_alloc(ctx, compiler);
            int cell_reg = reg_alloc(ctx, compiler);
            emit_abc(compiler->emitter, OP_UPVAL_GET, cell_reg, upvalue, 0);
            emit_abc(compiler->emitter, OP_CELL_GET, var_reg, cell_reg, 0);
            reg_free(compiler, cell_reg);
            need_writeback = true;
        } else {
            shared_index = shared_get_in_scope(ctx, compiler, name_str);
            if (shared_index >= 0) {
                var_reg = reg_alloc(ctx, compiler);
                emit_abx(compiler->emitter, OP_GETSHARED, var_reg, shared_index);
                need_writeback = true;
            } else {
                global_index = builtin_get(ctx, name_str);
                if (global_index >= 0) {
                    var_reg = reg_alloc(ctx, compiler);
                    emit_abx(compiler->emitter, OP_GETBUILTIN, var_reg, global_index);
                    need_writeback = true;
                } else {
                    xr_compiler_error(ctx, compiler, "Undefined variable '%s'", name_str->data);
                    return;
                }
            }
        }
    }

    emit_abc(compiler->emitter, OP_ADDI, var_reg, var_reg, 1);

    if (need_writeback) {
        if (local >= 0 && local_info && local_info->is_cellified) {
            emit_abc(compiler->emitter, OP_CELL_SET, local, var_reg, 0);
        } else if (upvalue >= 0) {
            int cell_reg2 = reg_alloc(ctx, compiler);
            emit_abc(compiler->emitter, OP_UPVAL_GET, cell_reg2, upvalue, 0);
            emit_abc(compiler->emitter, OP_CELL_SET, cell_reg2, var_reg, 0);
            reg_free(compiler, cell_reg2);
        } else if (shared_index >= 0) {
            emit_abx(compiler->emitter, OP_SETSHARED, var_reg, shared_index);
        } else {
            xr_compiler_error(ctx, compiler, "Cannot assign to built-in '%s'", name_str->data);
        }
        reg_free(compiler, var_reg);
    }
}

/*
 * Compile decrement statement.
 * --x or x-- (uniformly handled as prefix)
 * Decomposed to SUBI R[A] R[A] 1 (no fused OP_DEC).
 */
void compile_dec(XrCompilerContext *ctx, XrCompiler *compiler, IncDecNode *node) {
    XR_DCHECK(ctx != NULL, "compile_dec: NULL ctx");
    XR_DCHECK(compiler != NULL, "compile_dec: NULL compiler");
    XR_DCHECK(node != NULL, "compile_dec: NULL node");
    XrString *name_str = xr_compile_time_intern(ctx->X, node->name, strlen(node->name));

    int local = scope_resolve_local(compiler, name_str);
    int upvalue = -1;
    int shared_index = -1;
    int global_index = -1;
    int var_reg = -1;
    bool need_writeback = false;

    XrLocalInfo *local_info_dec = (local >= 0) ? compiler_get_local_by_name(compiler, node->name) : NULL;
    if (local >= 0) {
        if (local_info_dec && local_info_dec->is_cellified) {
            var_reg = reg_alloc(ctx, compiler);
            emit_abc(compiler->emitter, OP_CELL_GET, var_reg, local, 0);
            need_writeback = true;
        } else {
            var_reg = local;
        }
    } else {
        upvalue = scope_resolve_upvalue(ctx, compiler, name_str);
        if (upvalue >= 0) {
            var_reg = reg_alloc(ctx, compiler);
            int cell_reg = reg_alloc(ctx, compiler);
            emit_abc(compiler->emitter, OP_UPVAL_GET, cell_reg, upvalue, 0);
            emit_abc(compiler->emitter, OP_CELL_GET, var_reg, cell_reg, 0);
            reg_free(compiler, cell_reg);
            need_writeback = true;
        } else {
            shared_index = shared_get_in_scope(ctx, compiler, name_str);
            if (shared_index >= 0) {
                var_reg = reg_alloc(ctx, compiler);
                emit_abx(compiler->emitter, OP_GETSHARED, var_reg, shared_index);
                need_writeback = true;
            } else {
                global_index = builtin_get(ctx, name_str);
                if (global_index >= 0) {
                    var_reg = reg_alloc(ctx, compiler);
                    emit_abx(compiler->emitter, OP_GETBUILTIN, var_reg, global_index);
                    need_writeback = true;
                } else {
                    xr_compiler_error(ctx, compiler, "Undefined variable '%s'", name_str->data);
                    return;
                }
            }
        }
    }

    emit_abc(compiler->emitter, OP_SUBI, var_reg, var_reg, 1);

    if (need_writeback) {
        if (local >= 0 && local_info_dec && local_info_dec->is_cellified) {
            emit_abc(compiler->emitter, OP_CELL_SET, local, var_reg, 0);
        } else if (upvalue >= 0) {
            int cell_reg = reg_alloc(ctx, compiler);
            emit_abc(compiler->emitter, OP_UPVAL_GET, cell_reg, upvalue, 0);
            emit_abc(compiler->emitter, OP_CELL_SET, cell_reg, var_reg, 0);
            reg_free(compiler, cell_reg);
        } else if (shared_index >= 0) {
            emit_abx(compiler->emitter, OP_SETSHARED, var_reg, shared_index);
        } else {
            xr_compiler_error(ctx, compiler, "Cannot assign to built-in '%s'", name_str->data);
        }
        reg_free(compiler, var_reg);
    }
}
