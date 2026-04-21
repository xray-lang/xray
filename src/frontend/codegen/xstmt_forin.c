/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xstmt_forin.c - For-in loop compilation (all variants)
 *
 * KEY CONCEPT:
 *   Compiles for-in loops: array, range, enum, channel, map key-value,
 *   set, and custom iterator. Extracted from xstmt_control.c.
 */

#include "xstmt.h"
#include "../../base/xchecks.h"
#include "../../runtime/xisolate_api.h"
#include "xcompiler.h"
#include "xcompiler_context.h"
#include "xemit.h"
#include "xexpr.h"
#include "xexpr_desc.h"
#include "../xdiag_fmt.h"
#include "../../runtime/symbol/xsymbol_table.h"
#include "../../runtime/value/xtype.h"
#include "../analyzer/xanalyzer.h"
#include <stdio.h>

// Forward declarations
static void compile_for_in_single(XrCompilerContext *ctx, XrCompiler *compiler, ForInStmtNode *node);
static void compile_for_in_keyvalue(XrCompilerContext *ctx, XrCompiler *compiler, ForInStmtNode *node);
static void compile_for_in_array_single(XrCompilerContext *ctx, XrCompiler *compiler, ForInStmtNode *node);
static void compile_for_in_enum_single(XrCompilerContext *ctx, XrCompiler *compiler, ForInStmtNode *node);
static void compile_for_in_range(XrCompilerContext *ctx, XrCompiler *compiler, ForInStmtNode *node,
                                  int64_t start, int64_t end);
static void compile_for_in_range_dynamic(XrCompilerContext *ctx, XrCompiler *compiler,
                                          ForInStmtNode *node, RangeNode *range);
static void compile_for_in_range_object(XrCompilerContext *ctx, XrCompiler *compiler, ForInStmtNode *node);
static void compile_for_in_lazy_iterator(XrCompilerContext *ctx, XrCompiler *compiler, ForInStmtNode *node, const char *iter_method);
static void compile_for_in_custom_iterator(XrCompilerContext *ctx, XrCompiler *compiler, ForInStmtNode *node);
static void compile_for_in_channel(XrCompilerContext *ctx, XrCompiler *compiler, ForInStmtNode *node);
// Check if type is Channel
static bool is_channel_type(XrType *ct) {
    return ct && (ct->kind == XR_KIND_CHANNEL);
}

/* ========== For-In Loop ========== */

/*
 * Compile for-in loop.
 *
 * for (item in collection) { body }
 *
 * Desugars to traditional for loop (Array only):
 * for (let __idx = 0; __idx < collection.size; __idx++) {
 *     let item = collection[__idx]
 *     <body>
 * }
 *
 * Note: Set, Map etc. require manual iterator usage.
 */
void compile_for_in(XrCompilerContext *ctx, XrCompiler *compiler, ForInStmtNode *node) {
    XR_DCHECK(ctx != NULL, "compile_for_in: NULL ctx");
    XR_DCHECK(compiler != NULL, "compile_for_in: NULL compiler");
    if (node->is_keyvalue) {
        // Key-value mode: use entries array
        compile_for_in_keyvalue(ctx, compiler, node);
    } else {
        // Single variable mode: use index traversal
        compile_for_in_single(ctx, compiler, node);
    }
}

/*
 * Compile for-in single variable mode.
 * for (item in collection) {...}
 *
 * Desugars to:
 *   let __idx = 0
 *   for (__idx < collection.size) {
 *       let item = collection[__idx]
 *       <body>
 *       __idx = __idx + 1
 *   }
 */
static void compile_for_in_single(XrCompilerContext *ctx, XrCompiler *compiler, ForInStmtNode *node) {
    XR_DCHECK(ctx != NULL, "compile_for_in_single: NULL ctx");
    XR_DCHECK(compiler != NULL, "compile_for_in_single: NULL compiler");
    XR_DCHECK(node != NULL, "compile_for_in_single: NULL node");
    // Optimization 1: Detect direct range loop for (i in 0..N)
    if (node->collection->type == AST_RANGE) {
        RangeNode *range = &node->collection->as.range;

        // If start and end are both integer constants, use FORLOOP optimization
        if (range->start->type == AST_LITERAL_INT &&
            range->end->type == AST_LITERAL_INT) {
            int64_t start = range->start->as.literal.raw_value.int_val;
            int64_t end_val = range->end->as.literal.raw_value.int_val;
            compile_for_in_range(ctx, compiler, node, start, end_val);
            return;
        }

        // Optimization 1.5: Dynamic range loop for (i in a..b), bounds are variables or expressions
        compile_for_in_range_dynamic(ctx, compiler, node, range);
        return;
    }

    /* Optimization 2: Detect compile-time constant range variable.
     * Example: const range = 0..100; for (i in range) {...}
     */
    if (node->collection->type == AST_VARIABLE) {
        const char *var_name = node->collection->as.variable.name;

        // Check if variable is compile-time range constant
        for (int i = compiler->local_list.count - 1; i >= 0; i--) {
            XrLocalInfo *local = compiler->local_list.items[i];
            if (local->name && strcmp(local->name->data, var_name) == 0) {
                // Check if compile-time range constant
                if (local->is_const && local->comptime.type == COMPTIME_RANGE) {
                    int64_t start = local->comptime.as.range.start;
                    int64_t end_val = local->comptime.as.range.end;
                    compile_for_in_range(ctx, compiler, node, start, end_val);
                    return;
                }
                break;  // Found variable but not compile-time constant, stop searching
            }
        }
    }

    // Detect collection type (for enum iteration)
    bool is_enum = false;
    if (node->collection->type == AST_VARIABLE) {
        is_enum = xr_compiler_ctx_is_enum_type(ctx, node->collection->as.variable.name);
    }

    if (is_enum) {
        // Enum type: use special desugaring logic
        compile_for_in_enum_single(ctx, compiler, node);
        return;
    }

    // Check collection type for Range, Map, Set, Channel
    XrType *collection_type = get_expr_type(ctx, compiler, node->collection);

    // Optimization 3: Range object → RANGE_UNPACK + FORPREP/FORLOOP
    if (xr_type_is_named_class(collection_type, "Range")) {
        compile_for_in_range_object(ctx, compiler, node);
        return;
    }

    if (is_channel_type(collection_type)) {
        compile_for_in_channel(ctx, compiler, node);
        return;
    }

    // Map single-var: lazy iterator (no intermediate array allocation)
    if (collection_type && (collection_type->kind == XR_KIND_MAP)) {
        compile_for_in_lazy_iterator(ctx, compiler, node, "entriesIterator");
        return;
    }

    // Set single-var: lazy iterator (no intermediate array allocation)
    if (collection_type && (collection_type->kind == XR_KIND_SET)) {
        compile_for_in_lazy_iterator(ctx, compiler, node, "iterator");
        return;
    }

    // String: for (ch in str) -> index-based iteration with caching
    // VM OP_INDEX_GET natively supports string character indexing
    if (collection_type && (collection_type->kind == XR_KIND_STRING)) {
        compile_for_in_array_single(ctx, compiler, node);
        return;
    }

    // Check if collection is a custom iterable class (has iterator() method)
    // Case 1: Direct constructor call: for (i in Range(1, 4))
    if (ctx->analyzer && node->collection->type == AST_CALL_EXPR) {
        CallExprNode *call = &node->collection->as.call_expr;
        if (call->callee && call->callee->type == AST_VARIABLE) {
            const char *name = call->callee->as.variable.name;
            XaSymbol *xa_sym = xa_scope_lookup(ctx->analyzer->global_scope, name);
            if (xa_sym && xa_sym->kind == XA_SYM_CLASS) {
                XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, xa_sym);
                if (links && links->class_info) {
                    XaSymbol *iter_method = xa_class_info_lookup_member(links->class_info, "iterator");
                    if (iter_method && iter_method->kind == XA_SYM_METHOD) {
                        compile_for_in_custom_iterator(ctx, compiler, node);
                        return;
                    }
                }
            }
        }
    }

    // Case 2: Variable holding a class instance: let r = Range(1, 4); for (i in r)
    if (ctx->analyzer) {
        const char *class_name = NULL;
        // Try from inferred compile_type
        if (collection_type && (collection_type->kind == XR_KIND_CLASS || collection_type->kind == XR_KIND_INSTANCE) && collection_type->instance.class_name) {
            class_name = collection_type->instance.class_name;
        }
        // Try from analyzer scope (for variables without compile_type)
        if (!class_name && node->collection->type == AST_VARIABLE) {
            XrType *var_type = xa_analyzer_lookup_var(ctx->analyzer, node->collection->as.variable.name);
            if (var_type && (var_type->kind == XR_KIND_CLASS || var_type->kind == XR_KIND_INSTANCE) && var_type->instance.class_name) {
                class_name = var_type->instance.class_name;
            }
        }
        if (class_name) {
            XaSymbol *xa_sym = xa_scope_lookup(ctx->analyzer->global_scope, class_name);
            if (xa_sym && xa_sym->kind == XA_SYM_CLASS) {
                XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, xa_sym);
                if (links && links->class_info) {
                    XaSymbol *iter_method = xa_class_info_lookup_member(links->class_info, "iterator");
                    if (iter_method && iter_method->kind == XA_SYM_METHOD) {
                        compile_for_in_custom_iterator(ctx, compiler, node);
                        return;
                    }
                }
            }
        }
    }

    // Case 3: collection_type is a class (from type inference) - assume custom iterator
    if (collection_type && (collection_type->kind == XR_KIND_CLASS || collection_type->kind == XR_KIND_INSTANCE)) {
        compile_for_in_custom_iterator(ctx, compiler, node);
        return;
    }

    // Compile-time warning for known non-iterable types
    if (collection_type && collection_type->kind != XR_KIND_ARRAY &&
        collection_type->kind != XR_KIND_UNKNOWN) {
        char msg[192];
        snprintf(msg, sizeof(msg),
                 "for-in over non-iterable type (kind=%d), "
                 "falling back to array iteration",
                 collection_type->kind);
        xr_diag_print(XR_DIAG_WARNING, 0, msg,
                      ctx->source_file, node->collection->line,
                      ctx->current_column > 0 ? ctx->current_column : 1,
                      0, NULL, NULL);
    }

    // Default: Array type desugaring logic (for Array type or unknown type)
    compile_for_in_array_single(ctx, compiler, node);
}

/*
 * Compile for-in by calling a method on the collection first,
 * then iterating over the result as an array.
 * Used for Map (entries()) and Set (toArray()).
 *
 * Desugars: for (x in map) -> { let __col = map.entries(); for (x in __col) {...} }
 */

// Compile for-in array mode (original logic)
static void compile_for_in_array_single(XrCompilerContext *ctx, XrCompiler *compiler, ForInStmtNode *node) {
    XR_DCHECK(ctx != NULL, "compile_for_in_array_single: NULL ctx");
    XR_DCHECK(compiler != NULL, "compile_for_in_array_single: NULL compiler");
    int line = node->collection->line;

    /* Cache collection expression to avoid repeated evaluation.
     * If collection is a literal ([1,2,3]), function call (getItems()),
     * or any non-variable expression, each iteration would re-evaluate it.
     * We cache to a temp variable: let __for_col = collection
     */
    AstNode *col_ref = node->collection;
    AstNode *cache_decl = NULL;
    bool needs_cache = (node->collection->type != AST_VARIABLE);
    if (needs_cache) {
        cache_decl = xr_ast_var_decl(ctx->X, "__for_col", node->collection, false, line);
        col_ref = xr_ast_variable(ctx->X, "__for_col", line);
    }

    // 1. Build initializer: let __idx = 0
    AstNode *zero_literal = xr_ast_literal_int(ctx->X, 0, line);
    AstNode *init_decl = xr_ast_var_decl(ctx->X, "__for_idx", zero_literal, false, line);

    // 2. Build condition: __idx < col_ref.length
    AstNode *idx_var = xr_ast_variable(ctx->X, "__for_idx", line);
    AstNode *size_access = xr_ast_member_access(ctx->X, col_ref, "length", line);
    AstNode *condition = xr_ast_binary(ctx->X, AST_BINARY_LT, idx_var, size_access, line);

    // 3. Build increment: __idx = __idx + 1
    AstNode *idx_var2 = xr_ast_variable(ctx->X, "__for_idx", line);
    AstNode *one_literal = xr_ast_literal_int(ctx->X, 1, line);
    AstNode *add_expr = xr_ast_binary(ctx->X, AST_BINARY_ADD, idx_var2, one_literal, line);
    AstNode *increment = xr_ast_assignment(ctx->X, "__for_idx", add_expr, line);

    // 4. Build new loop body: { let item = col_ref[__idx]; <original body statements> }
    AstNode *new_body = xr_ast_block(ctx->X, line);

    // 4a. Add: let item = col_ref[__idx] (skip if blank identifier _)
    bool is_blank = (node->item_name[0] == '_' && node->item_name[1] == '\0');
    if (!is_blank) {
        AstNode *idx_var3 = xr_ast_variable(ctx->X, "__for_idx", line);
        AstNode *col_ref2 = needs_cache ? xr_ast_variable(ctx->X, "__for_col", line) : node->collection;
        AstNode *item_init = xr_ast_index_get(ctx->X, col_ref2, idx_var3, line);
        AstNode *item_decl = xr_ast_var_decl(ctx->X, node->item_name, item_init, false, line);
        xr_ast_block_add(ctx->X, new_body, item_decl);
    }

    // 4b. Add original loop body statements
    BlockNode *orig_body = &node->body->as.block;
    for (int i = 0; i < orig_body->count; i++) {
        xr_ast_block_add(ctx->X, new_body, orig_body->statements[i]);
    }

    // 5. Build for loop node and compile
    ForStmtNode for_node;
    for_node.initializer = init_decl;
    for_node.condition = condition;
    for_node.increment = increment;
    for_node.body = new_body;

    // 6. Wrap in scope if caching: { let __for_col = ...; for (...) {...} }
    if (needs_cache) {
        scope_begin(compiler);
        xr_compile_statement(ctx, compiler, cache_decl);
        compile_for(ctx, compiler, &for_node);
        scope_end(ctx, compiler);
    } else {
        compile_for(ctx, compiler, &for_node);
    }
}

// Compile for-in enum mode
static void compile_for_in_enum_single(XrCompilerContext *ctx, XrCompiler *compiler, ForInStmtNode *node) {
    int line = node->collection->line;

    // 1. Build initializer: let __idx = 0
    AstNode *zero_literal = xr_ast_literal_int(ctx->X, 0, line);
    AstNode *init_decl = xr_ast_var_decl(ctx->X, "__for_idx", zero_literal, false, line);

    // 2. Build condition: __idx < enum_type.memberCount
    AstNode *idx_var = xr_ast_variable(ctx->X, "__for_idx", line);
    AstNode *member_count_access = xr_ast_member_access(ctx->X, node->collection, "memberCount", line);
    AstNode *condition = xr_ast_binary(ctx->X, AST_BINARY_LT, idx_var, member_count_access, line);

    // 3. Build increment: __idx = __idx + 1
    AstNode *idx_var2 = xr_ast_variable(ctx->X, "__for_idx", line);
    AstNode *one_literal = xr_ast_literal_int(ctx->X, 1, line);
    AstNode *add_expr = xr_ast_binary(ctx->X, AST_BINARY_ADD, idx_var2, one_literal, line);
    AstNode *increment = xr_ast_assignment(ctx->X, "__for_idx", add_expr, line);

    // 4. Build new loop body: { let item = enum_type.getMember(__idx); <original body statements> }
    AstNode *new_body = xr_ast_block(ctx->X, line);

    // 4a. Add: let item = OP_ENUM_ACCESS(enum_type, __idx) (skip if blank identifier _)
    bool is_blank = (node->item_name[0] == '_' && node->item_name[1] == '\0');
    if (!is_blank) {
        AstNode *idx_var3 = xr_ast_variable(ctx->X, "__for_idx", line);
        AstNode *enum_index = xr_ast_enum_index(ctx->X, node->collection, idx_var3, line);
        AstNode *item_decl = xr_ast_var_decl(ctx->X, node->item_name, enum_index, false, line);
        xr_ast_block_add(ctx->X, new_body, item_decl);
    }

    // 4b. Add original loop body statements
    BlockNode *orig_body = &node->body->as.block;
    for (int i = 0; i < orig_body->count; i++) {
        xr_ast_block_add(ctx->X, new_body, orig_body->statements[i]);
    }

    // 5. Build for loop node and compile
    ForStmtNode for_node;
    for_node.initializer = init_decl;
    for_node.condition = condition;
    for_node.increment = increment;
    for_node.body = new_body;

    // 6. Call existing compile_for() - reuse all optimizations and logic
    compile_for(ctx, compiler, &for_node);
}

/*
 * Compile for-in range mode (standard CMP+JMP loop).
 * for (i in 0..100) { ... }
 *
 * Desugared to: for (let i = start; i < end; i++)
 * Range is [start, end), includes start but excludes end.
 */
static void compile_for_in_range(XrCompilerContext *ctx, XrCompiler *compiler,
                                  ForInStmtNode *node, int64_t start, int64_t end_val) {
    int line = node->collection->line;
    const char *var_name = node->item_name;

    // 1. Initializer: let i = start
    AstNode *start_lit = xr_ast_literal_int(ctx->X, start, line);
    AstNode *init_decl = xr_ast_var_decl(ctx->X, var_name, start_lit, false, line);

    // 2. Condition: i < end
    AstNode *idx_var = xr_ast_variable(ctx->X, var_name, line);
    AstNode *end_lit = xr_ast_literal_int(ctx->X, end_val, line);
    AstNode *condition = xr_ast_binary(ctx->X, AST_BINARY_LT, idx_var, end_lit, line);

    // 3. Increment: i = i + 1
    AstNode *idx_var2 = xr_ast_variable(ctx->X, var_name, line);
    AstNode *one_lit = xr_ast_literal_int(ctx->X, 1, line);
    AstNode *add_expr = xr_ast_binary(ctx->X, AST_BINARY_ADD, idx_var2, one_lit, line);
    AstNode *increment = xr_ast_assignment(ctx->X, var_name, add_expr, line);

    // 4. Build for loop and compile via generic path
    ForStmtNode for_node;
    for_node.initializer = init_decl;
    for_node.condition = condition;
    for_node.increment = increment;
    for_node.body = node->body;

    compile_for(ctx, compiler, &for_node);
}

/*
 * Compile for-in dynamic range mode (standard CMP+JMP loop).
 * for (i in a..b) { ... }
 *
 * Desugared to: for (let i = a; i < b; i++)
 * Range is [start, end), includes start but excludes end.
 */
static void compile_for_in_range_dynamic(XrCompilerContext *ctx, XrCompiler *compiler,
                                          ForInStmtNode *node, RangeNode *range) {
    int line = node->collection->line;
    const char *var_name = node->item_name;

    // 1. Initializer: let i = range.start
    AstNode *init_decl = xr_ast_var_decl(ctx->X, var_name, range->start, false, line);

    // 2. Condition: i < range.end
    AstNode *idx_var = xr_ast_variable(ctx->X, var_name, line);
    AstNode *condition = xr_ast_binary(ctx->X, AST_BINARY_LT, idx_var, range->end, line);

    // 3. Increment: i = i + 1
    AstNode *idx_var2 = xr_ast_variable(ctx->X, var_name, line);
    AstNode *one_lit = xr_ast_literal_int(ctx->X, 1, line);
    AstNode *add_expr = xr_ast_binary(ctx->X, AST_BINARY_ADD, idx_var2, one_lit, line);
    AstNode *increment = xr_ast_assignment(ctx->X, var_name, add_expr, line);

    // 4. Build for loop and compile via generic path
    ForStmtNode for_node;
    for_node.initializer = init_decl;
    for_node.condition = condition;
    for_node.increment = increment;
    for_node.body = node->body;

    compile_for(ctx, compiler, &for_node);
}

/*
 * Compile for-in over a Range object variable.
 * Uses OP_RANGE_UNPACK to extract (start, end, step) from the Range,
 * then standard CMP+JMP loop.
 *
 * Generated code:
 *   RANGE_UNPACK base, range_reg   ; R[base]=start, R[base+1]=end, R[base+2]=step
 * loop:
 *   LT tmp, R[base], R[base+1]    ; i < end
 *   TEST tmp 0 → JMP exit
 *   <loop body>                    ; loop variable = R[base]
 *   ADD R[base], R[base], R[base+2] ; i += step
 *   JMP loop
 * exit:
 */
static void compile_for_in_range_object(XrCompilerContext *ctx, XrCompiler *compiler, ForInStmtNode *node) {
    scope_begin(compiler);

    const char *var_name = node->item_name;
    bool is_blank = (var_name[0] == '_' && var_name[1] == '\0');

    // Compile collection (Range object) to a temp register
    XrExprDesc coll_expr = xr_compile_expr(ctx, compiler, node->collection);
    int range_reg = xexpr_to_anyreg(ctx, compiler, &coll_expr);

    // Allocate 3 consecutive registers: start(loop var), end, step
    int base_reg = xreg_reserve(compiler->regalloc, 3);
    int loop_var_reg = base_reg;      // R[base]   = start / loop variable
    int end_reg      = base_reg + 1;  // R[base+1] = end
    int step_reg     = base_reg + 2;  // R[base+2] = step

    // OP_RANGE_UNPACK: R[base]=start, R[base+1]=end, R[base+2]=step
    emit_abc(compiler->emitter, OP_RANGE_UNPACK, base_reg, range_reg, 0);

    // Release range_reg temp
    xreg_set_freereg(compiler->regalloc, base_reg + 3);

    // Bind loop variable as local (skip if blank identifier _)
    if (!is_blank) {
        XrString *name_str = xr_compile_time_intern(ctx->X, var_name, strlen(var_name));
        XrLocalInfo *loop_lv = scope_define_local_reg(ctx, compiler, name_str, loop_var_reg);
        if (loop_lv) {
            local_set_compile_type(loop_lv, xr_type_new_int(NULL));
        }
    }
    xreg_set_local_end(compiler->regalloc, loop_var_reg + 1);

    // Standard CMP+JMP loop
    int loop_start = PROTO_CODE_COUNT(compiler->proto);

    // Condition: loop_var < end (expression form: R[tmp] = R[loop_var] < R[end])
    int tmp_reg = xreg_reserve(compiler->regalloc, 1);
    emit_abc(compiler->emitter, OP_CMP_LT, tmp_reg, loop_var_reg, end_reg);
    emit_abc(compiler->emitter, OP_TEST, tmp_reg, 0, 0);
    int exit_jump = emit_jump(compiler->emitter, OP_JMP);
    xreg_set_freereg(compiler->regalloc, tmp_reg);

    XrLoopState loop_state;
    loop_state_save(compiler, &loop_state);

    // Protect loop control registers during body compilation
    int protect_id = xreg_protect_begin(compiler->regalloc, base_reg, 3, "range_object_loop");
    xreg_set_freereg(compiler->regalloc, base_reg + 3);

    loop_state_enter(compiler, loop_start);
    xr_compile_statement(ctx, compiler, node->body);
    loop_state_restore(compiler, &loop_state);

    xreg_protect_end(compiler->regalloc, protect_id);

    // Increment: loop_var += step
    int increment_pos = PROTO_CODE_COUNT(compiler->proto);
    loop_state_patch_continue(compiler, &loop_state, increment_pos);
    emit_abc(compiler->emitter, OP_ADD, loop_var_reg, loop_var_reg, step_reg);

    // Jump back to loop start
    emit_loop(compiler->emitter, loop_start);

    // Patch exit jump
    patch_jump(compiler->emitter, exit_jump, -1);

    int loop_end = PROTO_CODE_COUNT(compiler->proto);
    loop_state_patch_break(compiler, &loop_state, loop_end);

    scope_end(ctx, compiler);
}

// Global counter for generating unique for-in temp variable names (avoid nesting conflicts)
static int for_in_counter = 0;

/*
 * Compile for-in key-value mode - direct bytecode generation (Iterator-Based).
 * for (key, value in map) {...}
 *
 * Generated bytecode equivalent to:
 *   {
 *       let __iter = map.entriesIterator()
 *       while (__iter.hasNext()) {
 *           let __entry = __iter.next()
 *           let key = __entry[0]
 *           let value = __entry[1]
 *           <body>
 *       }
 *   }
 */
static void compile_for_in_keyvalue_v3(XrCompilerContext *ctx, XrCompiler *compiler, ForInStmtNode *node) {
    XR_DCHECK(ctx != NULL, "compile_for_in_keyvalue_v3: NULL ctx");
    XR_DCHECK(compiler != NULL, "compile_for_in_keyvalue_v3: NULL compiler");
    // Generate unique temp variable names (avoid nesting for-in variable name conflicts)
    int id = for_in_counter++;

    // Enter scope - contains all temp variables and user variables
    scope_begin(compiler);

    /* ========== 1. Create iterator: let __iter = map.entriesIterator() ========== */

    // Compile map expression
    XrExprDesc map_expr = xr_compile_expr(ctx, compiler, node->collection);
    int map_reg = xexpr_to_anyreg(ctx, compiler, &map_expr);

    /* Call entriesIterator() method.
     * Unified calling convention: R[base]=return value, R[base+1]=receiver, no args.
     */
    int iter_base = reg_alloc(ctx, compiler);
    emit_move(compiler->emitter, iter_base + 1, map_reg);  // receiver to R[base+1]
    xreg_set_freereg(compiler->regalloc, iter_base + 2);  // protect receiver
    int global_sym = xr_symbol_register_in_table((XrSymbolTable*)xr_isolate_get_symbol_table(ctx->X), "entriesIterator");
    int method_symbol = emitter_add_symbol(compiler->emitter, global_sym);
    emit_abc(compiler->emitter, OP_INVOKE, iter_base, method_symbol, 0);
    int iter_reg = iter_base;  // return value in R[base]
    xreg_set_freereg(compiler->regalloc, iter_base + 1);

    // Register __iter variable (not user visible, but needs register)
    char iter_name[64];
    snprintf(iter_name, sizeof(iter_name), "__for_iter_%d", id);
    XrString *iter_str = xr_compile_time_intern(ctx->X, iter_name, strlen(iter_name));
    scope_define_local_reg(ctx, compiler, iter_str, iter_reg);

    /* ========== 2. While loop start: while (__iter.hasNext()) ========== */

    int loop_start = PROTO_CODE_COUNT(compiler->proto);

    /* Call __iter.hasNext().
     * Unified calling convention: R[base]=return value, R[base+1]=receiver.
     */
    int hasnext_base = reg_alloc(ctx, compiler);
    emit_move(compiler->emitter, hasnext_base + 1, iter_reg);  // receiver to R[base+1]
    xreg_set_freereg(compiler->regalloc, hasnext_base + 2);
    global_sym = xr_symbol_register_in_table((XrSymbolTable*)xr_isolate_get_symbol_table(ctx->X), "hasNext");
    method_symbol = emitter_add_symbol(compiler->emitter, global_sym);
    emit_abc(compiler->emitter, OP_INVOKE, hasnext_base, method_symbol, 0);
    int has_next_reg = hasnext_base;  // return value in R[base]
    xreg_set_freereg(compiler->regalloc, hasnext_base + 1);

    // Test condition: if false, exit loop
    emit_abc(compiler->emitter, OP_TEST, has_next_reg, 0, 0);
    int exit_jump = emit_jump(compiler->emitter, OP_JMP);
    reg_free(compiler, has_next_reg);

    /* ========== 3. Inside loop body: let __entry = __iter.next() ========== */

    XrLoopState loop_state;
    loop_state_save(compiler, &loop_state);
    loop_state_enter(compiler, loop_start);

    /* Call __iter.next().
     * Unified calling convention: R[base]=return value, R[base+1]=receiver.
     */
    int next_base = reg_alloc(ctx, compiler);
    emit_move(compiler->emitter, next_base + 1, iter_reg);  // receiver to R[base+1]
    xreg_set_freereg(compiler->regalloc, next_base + 2);
    global_sym = xr_symbol_register_in_table((XrSymbolTable*)xr_isolate_get_symbol_table(ctx->X), "next");
    method_symbol = emitter_add_symbol(compiler->emitter, global_sym);
    emit_abc(compiler->emitter, OP_INVOKE, next_base, method_symbol, 0);
    int entry_reg = next_base;  // return value in R[base]
    xreg_set_freereg(compiler->regalloc, next_base + 1);

    // Register __entry variable
    char entry_name[64];
    snprintf(entry_name, sizeof(entry_name), "__for_entry_%d", id);
    XrString *entry_str = xr_string_intern(ctx->X, entry_name, strlen(entry_name), 0);
    scope_define_local_reg(ctx, compiler, entry_str, entry_reg);

    /* ========== 4. Extract key-value: let key = __entry[0], let value = __entry[1] ========== */

    // Check for blank identifiers
    bool key_is_blank = (node->item_name[0] == '_' && node->item_name[1] == '\0');
    bool value_is_blank = (node->value_name[0] == '_' && node->value_name[1] == '\0');

    // let key = __entry[0] (skip if blank identifier _)
    if (!key_is_blank) {
        // Compile index 0
        int index0_reg = reg_alloc(ctx, compiler);
        emit_asbx(compiler->emitter, OP_LOADI, index0_reg, 0);

        // GETTABLE: key = entry[0]
        int key_reg = reg_alloc(ctx, compiler);
        emit_abc(compiler->emitter, OP_INDEX_GET, key_reg, entry_reg, index0_reg);
        xreg_set_freereg(compiler->regalloc, key_reg + 1);  // release index0_reg

        XrString *key_str = xr_string_intern(ctx->X, node->item_name, strlen(node->item_name), 0);
        scope_define_local_reg(ctx, compiler, key_str, key_reg);
    }

    // let value = __entry[1] (skip if blank identifier _)
    if (!value_is_blank) {
        // Compile index 1
        int index1_reg = reg_alloc(ctx, compiler);
        emit_asbx(compiler->emitter, OP_LOADI, index1_reg, 1);

        // GETTABLE: value = entry[1]
        int value_reg = reg_alloc(ctx, compiler);
        emit_abc(compiler->emitter, OP_INDEX_GET, value_reg, entry_reg, index1_reg);
        xreg_set_freereg(compiler->regalloc, value_reg + 1);  // release index1_reg

        XrString *value_str = xr_string_intern(ctx->X, node->value_name, strlen(node->value_name), 0);
        scope_define_local_reg(ctx, compiler, value_str, value_reg);
    }

    /* ========== 5. Compile user loop body ========== */

    xr_compile_statement(ctx, compiler, node->body);

    /* ========== 6. Continue jump point (loop back to start) ========== */

    int continue_pos = PROTO_CODE_COUNT(compiler->proto);

    loop_state_patch_continue(compiler, &loop_state, continue_pos);

    // Jump back to loop start
    emit_loop(compiler->emitter, loop_start);

    /* ========== 7. Exit loop ========== */

    patch_jump(compiler->emitter, exit_jump, -1);
    loop_state_patch_break(compiler, &loop_state, -1);
    loop_state_restore(compiler, &loop_state);

    // Exit scope - cleanup all temp variables
    scope_end(ctx, compiler);
}

/*
 * Compile for-in key-value mode.
 * for (key, value in map) {...}
 * Direct bytecode generation + lazy Iterator.
 *
 * Implementation:
 *   1. Create Iterator object: let __iter = map.entriesIterator()
 *   2. While loop: while (__iter.hasNext())
 *   3. Get entry: let __entry = __iter.next()
 *   4. Extract key-value: let key = __entry[0], let value = __entry[1]
 *   5. Execute loop body
 */
static void compile_for_in_keyvalue(XrCompilerContext *ctx, XrCompiler *compiler, ForInStmtNode *node) {
    compile_for_in_keyvalue_v3(ctx, compiler, node);
}

/*
 * Compile for-in using iterator protocol with a specified iterator method.
 *
 * Desugars to:
 *   let __iter = collection.<iter_method>()
 *   while (__iter.hasNext()) {
 *       let item = __iter.next()
 *       <body>
 *   }
 */
static void compile_for_in_lazy_iterator(XrCompilerContext *ctx, XrCompiler *compiler, ForInStmtNode *node, const char *iter_method) {
    XR_DCHECK(ctx != NULL, "compile_for_in_lazy_iterator: NULL ctx");
    XR_DCHECK(compiler != NULL, "compile_for_in_lazy_iterator: NULL compiler");
    int line = node->collection->line;

    // Unique iterator variable name for nested loop support
    static int iter_counter = 0;
    char iter_name[32];
    snprintf(iter_name, sizeof(iter_name), "__for_iter_%d", iter_counter++);

    // 1. Build: let __iter_N = collection.<iter_method>()
    AstNode *iter_call = xr_ast_call_expr(ctx->X,
        xr_ast_member_access(ctx->X, node->collection, iter_method, line),
        NULL, 0, line);
    AstNode *iter_decl = xr_ast_var_decl(ctx->X, iter_name, iter_call, false, line);

    // 2. Build condition: __iter_N.hasNext()
    AstNode *iter_var = xr_ast_variable(ctx->X, iter_name, line);
    AstNode *has_next_call = xr_ast_call_expr(ctx->X,
        xr_ast_member_access(ctx->X, iter_var, "hasNext", line),
        NULL, 0, line);

    // 3. Build: let item = __iter_N.next()
    AstNode *iter_var2 = xr_ast_variable(ctx->X, iter_name, line);
    AstNode *next_call = xr_ast_call_expr(ctx->X,
        xr_ast_member_access(ctx->X, iter_var2, "next", line),
        NULL, 0, line);
    AstNode *item_decl = xr_ast_var_decl(ctx->X, node->item_name, next_call, false, line);

    // 4. Build new loop body: { let item = __iter.next(); <original body statements> }
    AstNode *new_body = xr_ast_block(ctx->X, line);
    xr_ast_block_add(ctx->X, new_body, item_decl);

    // Add original body statements
    if (node->body->type == AST_BLOCK) {
        BlockNode *block = &node->body->as.block;
        for (int i = 0; i < block->count; i++) {
            xr_ast_block_add(ctx->X, new_body, block->statements[i]);
        }
    } else {
        xr_ast_block_add(ctx->X, new_body, node->body);
    }

    // 5. Build while loop: while (__iter.hasNext()) { ... }
    AstNode *while_node = xr_ast_while_stmt(ctx->X, has_next_call, new_body, line);

    // 6. Build outer block: { let __iter = ...; while (...) { ... } }
    AstNode *outer_block = xr_ast_block(ctx->X, line);
    xr_ast_block_add(ctx->X, outer_block, iter_decl);
    xr_ast_block_add(ctx->X, outer_block, while_node);

    // 7. Compile the desugared code
    xr_compile_statement(ctx, compiler, outer_block);
}

// Convenience wrapper: custom iterator classes use "iterator" method
static void compile_for_in_custom_iterator(XrCompilerContext *ctx, XrCompiler *compiler, ForInStmtNode *node) {
    compile_for_in_lazy_iterator(ctx, compiler, node, "iterator");
}

/* ========== Channel For-In ========== */

/*
 * Compile for-in loop over Channel.
 * for (item in ch) { body }
 *
 * Desugars to:
 *   let __ch = ch              // Cache channel reference (important for shared vars)
 *   while (true) {
 *       let __val, __ok = __ch.tryRecv()
 *       if (!__ok) {
 *           if (__ch.isClosed()) break
 *           yield
 *           continue
 *       }
 *       let item = __val
 *       <body>
 *   }
 */
static void compile_for_in_channel(XrCompilerContext *ctx, XrCompiler *compiler, ForInStmtNode *node) {
    XR_DCHECK(ctx != NULL, "compile_for_in_channel: NULL ctx");
    XR_DCHECK(compiler != NULL, "compile_for_in_channel: NULL compiler");
    int line = node->collection->line;

    // 0. Build: let __ch = ch (cache channel reference, critical for shared variables)
    AstNode *ch_var = xr_ast_variable(ctx->X, "__chan_ref", line);
    AstNode *ch_decl = xr_ast_var_decl(ctx->X, "__chan_ref", node->collection, false, line);

    // Compile the channel cache declaration first
    xr_compile_statement(ctx, compiler, ch_decl);

    // Set Channel type for __chan_ref (required for method call optimization)
    XrString *ref_name = xr_compile_time_intern(ctx->X, "__chan_ref", 11);
    XrLocalInfo *ref_local = compiler_get_local_by_name(compiler, ref_name->data);
    if (ref_local) {
        ref_local->compile_type = xr_type_new_channel(ctx->X, xr_type_new_unknown(NULL));
    }

    // 1. Build: let __val, __ok = __ch.tryRecv()
    AstNode *tryrecv_call = xr_ast_call_expr(ctx->X,
        xr_ast_member_access(ctx->X, ch_var, "tryRecv", line),
        NULL, 0, line);

    // Multi-value declaration: let __val, __ok = __ch.tryRecv()
    char **names = xr_malloc(2 * sizeof(char*));
    names[0] = strdup("__chan_val");
    names[1] = strdup("__chan_ok");
    AstNode **values = xr_malloc(1 * sizeof(AstNode*));
    values[0] = tryrecv_call;
    AstNode *multi_decl = xr_ast_multi_var_decl(ctx->X, names, 2, values, 1, false, line);

    // 2. Build condition for inner if: !__ok
    AstNode *ok_var = xr_ast_variable(ctx->X, "__chan_ok", line);
    AstNode *not_ok = xr_ast_unary(ctx->X, AST_UNARY_NOT, ok_var, line);

    // 3. Build inner condition: __ch.isClosed()
    AstNode *ch_var2 = xr_ast_variable(ctx->X, "__chan_ref", line);
    AstNode *is_closed_call = xr_ast_call_expr(ctx->X,
        xr_ast_member_access(ctx->X, ch_var2, "isClosed", line),
        NULL, 0, line);

    // 4. Build break statement for when channel is closed
    AstNode *break_stmt = xr_ast_break_stmt(ctx->X, line);

    // 5. Build if (ch.isClosed()) break
    AstNode *inner_if = xr_ast_if_stmt(ctx->X, is_closed_call, break_stmt, NULL, line);

    // 6. Build yield statement
    AstNode *yield_stmt = xr_ast_yield_stmt(ctx->X, line);

    // 7. Build continue statement
    AstNode *continue_stmt = xr_ast_continue_stmt(ctx->X, line);

    // 8. Build if (!__ok) { if (ch.isClosed()) break; yield; continue }
    AstNode *not_ok_body = xr_ast_block(ctx->X, line);
    xr_ast_block_add(ctx->X, not_ok_body, inner_if);
    xr_ast_block_add(ctx->X, not_ok_body, yield_stmt);
    xr_ast_block_add(ctx->X, not_ok_body, continue_stmt);
    AstNode *not_ok_if = xr_ast_if_stmt(ctx->X, not_ok, not_ok_body, NULL, line);

    // 9. Build: let item = __val (skip if blank identifier _)
    bool is_blank = (node->item_name[0] == '_' && node->item_name[1] == '\0');
    AstNode *item_decl = NULL;
    if (!is_blank) {
        AstNode *val_var = xr_ast_variable(ctx->X, "__chan_val", line);
        item_decl = xr_ast_var_decl(ctx->X, node->item_name, val_var, false, line);
    }

    // 10. Build while body: { let __val, __ok = ...; if (!__ok) {...}; let item = __val; <original body> }
    AstNode *while_body = xr_ast_block(ctx->X, line);
    xr_ast_block_add(ctx->X, while_body, multi_decl);
    xr_ast_block_add(ctx->X, while_body, not_ok_if);
    if (item_decl) {
        xr_ast_block_add(ctx->X, while_body, item_decl);
    }

    // Add original body statements
    if (node->body->type == AST_BLOCK) {
        BlockNode *block = &node->body->as.block;
        for (int i = 0; i < block->count; i++) {
            xr_ast_block_add(ctx->X, while_body, block->statements[i]);
        }
    } else {
        xr_ast_block_add(ctx->X, while_body, node->body);
    }

    // 11. Build while (true) { ... }
    AstNode *true_literal = xr_ast_literal_bool(ctx->X, true, line);
    AstNode *while_stmt = xr_ast_while_stmt(ctx->X, true_literal, while_body, line);

    // 12. Compile the desugared code
    xr_compile_statement(ctx, compiler, while_stmt);
}
