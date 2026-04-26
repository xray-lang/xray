/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcompiler.c - Register-based compiler implementation
 *
 * KEY CONCEPT:
 *   Compiles AST to register-based bytecode using LIFO register allocation.
 *   Expression compilation in expr/ module, statement compilation in stmt/ module.
 *
 * RELATED MODULES:
 *   - xcompiler_context.h: Shared compilation context
 *   - xregalloc.h: Register allocator (LIFO mode)
 *   - xemit.h: Instruction emitter with peephole optimization
 */

#include "xcompiler.h"
#include "../../runtime/gc/xbc_stackmap.h"
#include "../../base/xlog.h"
#include "xcompiler_context.h"
#include "xcompiler_scope.h"
#include "../../base/xchecks.h"
#include "xoptimize.h"
#include "xpeephole.h"
#include "xfusion.h"
#include "xinline.h"
#include "../../base/xmalloc.h"
#include "../../runtime/object/xstring.h"
#include "../../runtime/symbol/xsymbol_table.h"
#include "../../runtime/class/xclass.h"
#include "../../runtime/class/xmethod.h"
#include "../../vm/xdebug.h"
#include "xregalloc.h"
#include "xemit.h"
#include "xexpr.h"
#include "xstmt.h"
#include "xoop.h"
#include "../../runtime/value/xtype.h"
#include "../analyzer/xanalyzer_mono.h"
#include "../analyzer/xanalyzer_escape.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "../xdiag_fmt.h"

// Forward declarations
struct XrBlueprint *xr_blueprint_generate(struct XrProto *proto);
XrExprDesc compile_ternary(XrCompilerContext *ctx, XrCompiler *compiler, TernaryNode *node);
XrExprDesc compile_nullish_coalesce(XrCompilerContext *ctx, XrCompiler *compiler, BinaryNode *node);
XrExprDesc compile_optional_chain(XrCompilerContext *ctx, XrCompiler *compiler, OptionalChainNode *node);
int compile_range(XrCompilerContext *ctx, XrCompiler *compiler, RangeNode *node);

/* ========== Local Variable Access ========== */

XrLocalInfo* compiler_get_local_at(XrCompiler *compiler, int index) {
    XR_DCHECK(compiler != NULL, "get_local_at: NULL compiler");
    XR_DCHECK(index >= 0, "get_local_at: negative index");
    return xr_local_list_get(&compiler->local_list, index);
}

int compiler_get_local_count(XrCompiler *compiler) {
    XR_DCHECK(compiler != NULL, "get_local_count: NULL compiler");
    return compiler->local_list.count;
}

// Linear scan from back to front (handles shadowing automatically)
XrLocalInfo* compiler_get_local_by_name(XrCompiler *compiler, const char *name) {
    if (name == NULL) return NULL;

    for (int i = compiler->local_count - 1; i >= 0; i--) {
        XrLocalInfo *local = compiler_get_local_at(compiler, i);
        if (local == NULL || local->name == NULL) continue;

        if (strcmp(local->name->data, name) == 0) {
            return local;
        }
    }

    return NULL;
}

/* ========== Error Handling ========== */

void xr_compiler_error(XrCompilerContext *ctx, XrCompiler *compiler, const char *format, ...) {
    XR_DCHECK(ctx != NULL, "compiler_error: NULL ctx");
    XR_DCHECK(compiler != NULL, "compiler_error: NULL compiler");
    if (compiler->panic_mode) {
        return;  // Avoid error cascade
    }
    compiler->panic_mode = true;
    compiler->had_error = true;

    char msg[512];
    va_list args;
    va_start(args, format);
    vsnprintf(msg, sizeof(msg), format, args);
    va_end(args);

    xr_diag_print(
        XR_DIAG_ERROR, 0, msg,
        ctx->source_file,
        ctx->current_line,
        ctx->current_column > 0 ? ctx->current_column : 1,
        0, NULL, NULL
    );
}

/* ========== Register Allocation ========== */

// Allocate temporary register (LIFO mode)
int reg_alloc(XrCompilerContext *ctx, XrCompiler *compiler) {
    XR_DCHECK(compiler != NULL, "reg_alloc: NULL compiler");
    (void)ctx;
    if (!compiler->regalloc) {
        return 0;
    }
    return xreg_alloc_temp(compiler->regalloc);
}

// Allocate next consecutive register
int reg_alloc_next(XrCompilerContext *ctx, XrCompiler *compiler) {
    if (!compiler->regalloc) {
        xr_compiler_error(ctx, compiler, "XRegAlloc not initialized");
        return 0;
    }

    int reg = xreg_alloc_next(compiler->regalloc);
    if (reg < 0) {
        xr_compiler_error(ctx, compiler, "Failed to allocate next register");
        return 0;
    }

    return reg;
}

// Reserve n registers
int reg_reserve_n(XrCompiler *compiler, int n) {
    if (!compiler->regalloc) {
        return -1;
    }

    return xreg_reserve(compiler->regalloc, n);
}

// Compile expression to next consecutive register
// 1. Compile expr to any register
// 2. MOVE to next_reg if not already there
// 3. Return next_reg
int compile_expr_to_next_reg(XrCompilerContext *ctx, XrCompiler *compiler, AstNode *expr) {
    XR_DCHECK(compiler != NULL, "compile_expr_to_next_reg: NULL compiler");
    XR_DCHECK(expr != NULL, "compile_expr_to_next_reg: NULL expr");
    int next_reg = xreg_get_freereg(compiler->regalloc);

    // Temporarily bump freereg so expr can use higher registers
    int temp_freereg = next_reg + 1;
    xreg_set_freereg(compiler->regalloc, temp_freereg);

    int expr_reg = xr_compile_expression(ctx, compiler, expr);

    if (expr_reg != next_reg) {
        emit_move(compiler->emitter, next_reg, expr_reg);
    }

    xreg_set_freereg(compiler->regalloc, next_reg + 1);

    return next_reg;
}

// Compile argument list to consecutive registers starting at base_reg
int compile_args_to_base(XrCompilerContext *ctx, XrCompiler *compiler,
                         AstNode **args, int arg_count, int base_reg) {
    XR_DCHECK(base_reg >= 0, "compile_args_to_base: negative base_reg");
    XR_DCHECK(arg_count >= 0, "compile_args_to_base: negative arg_count");
    // Protect the entire argument region to prevent find_dead_temp from reusing
    int protect_id = -1;
    if (arg_count > 1) {
        protect_id = xreg_protect_begin(compiler->regalloc, base_reg, arg_count, "compile_args");
    }

    for (int i = 0; i < arg_count; i++) {
        int target_reg = base_reg + i;
        xreg_set_freereg(compiler->regalloc, target_reg + 1);

        if (args[i]) {
            XrExprDesc expr = xr_compile_expr(ctx, compiler, args[i]);
            xexpr_to_specific_reg(ctx, compiler, &expr, target_reg);
            // BOX if raw typed (callee expects tagged arguments)
            if (xexpr_is_raw(&expr)) {
                int boxed = xexpr_ensure_boxed(ctx, compiler, &expr, target_reg);
                if (boxed != target_reg) {
                    xemit_move(compiler->emitter, target_reg, boxed);
                }
            }
        } else {
            emit_loadnull(compiler->emitter, target_reg);
        }
    }

    if (protect_id >= 0) {
        xreg_protect_end(compiler->regalloc, protect_id);
    }

    xreg_set_freereg(compiler->regalloc, base_reg + arg_count);
    return base_reg + arg_count;
}

// Compile multiple expressions to consecutive registers, returns base register
int compile_exprs_consecutive(XrCompilerContext *ctx, XrCompiler *compiler,
                              AstNode **exprs, int count, const char *reason) {
    if (count <= 0) return -1;

    int base_reg = xreg_get_freereg(compiler->regalloc);
    int protect_id = xreg_protect_begin(compiler->regalloc, base_reg, count, reason);

    for (int i = 0; i < count; i++) {
        int target_reg = base_reg + i;
        xreg_set_freereg(compiler->regalloc, target_reg + 1);

        if (exprs[i]) {
            XrExprDesc expr = xr_compile_expr(ctx, compiler, exprs[i]);
            xexpr_to_specific_reg(ctx, compiler, &expr, target_reg);
        } else {
            emit_loadnull(compiler->emitter, target_reg);
        }
    }

    xreg_protect_end(compiler->regalloc, protect_id);
    xreg_set_freereg(compiler->regalloc, base_reg + count);

    return base_reg;
}

void reg_free(XrCompiler *compiler, int reg) {
    if (!compiler->regalloc) {
        return;
    }
    xreg_free_temp(compiler->regalloc, reg);
}

/* ========== Instruction Emission Macros ========== */

#include "xcompiler_emit.h"

/* ========== Scope Management ========== */

// Enter new scope, saves block entry state in XrBlockCnt
void scope_begin(XrCompiler *compiler) {
    XR_DCHECK(compiler != NULL, "scope_begin: NULL compiler");
    XR_DCHECK(compiler->scope_depth >= 0, "scope_begin: negative scope_depth");
    compiler->scope_depth++;

    if (compiler->block_depth >= XR_MAX_BLOCK_DEPTH) {
        xr_log_warning("compiler", "block nesting too deep (max %d)", XR_MAX_BLOCK_DEPTH);
        return;
    }

    XrBlockCnt *bl = &compiler->block_stack[compiler->block_depth++];
    bl->local_end_on_entry = compiler->regalloc ? xreg_get_local_end(compiler->regalloc) : 0;
    bl->scope_depth = compiler->scope_depth;
    bl->is_loop = false;

    // Invariant check: freereg >= local_end
    if (compiler->regalloc) {
        int freereg = xreg_get_freereg(compiler->regalloc);
        int local_end = xreg_get_local_end(compiler->regalloc);
        if (freereg < local_end) {
            xr_log_warning("compiler", "scope_begin invariant violation: freereg=%d < local_end=%d",
                    freereg, local_end);
        }
    }
}

// Leave scope, restores block entry state from XrBlockCnt
void scope_end(XrCompilerContext *ctx, XrCompiler *compiler) {
    XR_DCHECK(compiler != NULL, "scope_end: NULL compiler");
    if (compiler->block_depth <= 0) {
        xr_log_warning("compiler", "scope_end without matching scope_begin");
        return;
    }

    XrBlockCnt *bl = &compiler->block_stack[compiler->block_depth - 1];

    if (bl->scope_depth != compiler->scope_depth) {
        xr_log_warning("compiler", "scope_end depth mismatch: expected %d, got %d",
                bl->scope_depth, compiler->scope_depth);
    }

    compiler->scope_depth--;

    // Release local variables in current scope
    while (compiler->local_count > 0) {
        XrLocalInfo *local = compiler_get_local_at(compiler, compiler->local_count - 1);
        if (local->depth <= compiler->scope_depth) {
            break;
        }

        compiler->local_count--;
    }

    xr_local_list_pop_above_depth(&compiler->local_list, compiler->scope_depth);
    compiler->local_count = compiler->local_list.count;

    // Restore local_end to block entry value
    if (compiler->regalloc) {
        xreg_set_local_end(compiler->regalloc, bl->local_end_on_entry);
        xreg_set_freereg(compiler->regalloc, bl->local_end_on_entry);
    }

    if (compiler->regalloc) {
        int local_end = xreg_get_local_end(compiler->regalloc);
        if (local_end != bl->local_end_on_entry) {
            xr_log_warning("compiler", "local_end restoration failed: expected %d, got %d",
                    bl->local_end_on_entry, local_end);
        }
    }

    compiler->block_depth--;
}

// Common initialization for local variable info (shared by scope_define_local and scope_define_local_reg)
static XrLocalInfo* scope_init_local_info(XrCompiler *compiler, XrString *name, int reg) {
    XrLocalInfo *local = xr_arena_new(compiler->arena, XrLocalInfo);
    local->name = name;
    local->reg = reg;
    local->depth = compiler->scope_depth;

    // Check prescan: if this local was found to be captured by a nested fn,
    // eagerly mark is_captured and allocate a ctx_slot before codegen proceeds.
    bool captured = false;
    if (name && compiler->prescan_captured.count > 0) {
        for (int i = 0; i < compiler->prescan_captured.count; i++) {
            if (compiler->prescan_captured.data[i] &&
                strcmp(compiler->prescan_captured.data[i], name->data) == 0) {
                captured = true;
                break;
            }
        }
    }
    local->is_captured = captured;
    local->ctx_slot = captured ? compiler->captured_count++ : -1;
    local->compile_type = NULL;
    local->spill_slot = -1;
    local->cached_reg = -1;
    local->cached_gen = 0;
    local->can_rematerialize = false;
    local->remat_value = 0;

    xr_local_list_add(&compiler->local_list, local, (void*(*)(size_t))xr_malloc_fn);
    compiler->local_count = compiler->local_list.count;

    // Record debug info: local variable name to register mapping
    if (compiler->proto && name) {
        XrLocVar locvar = {
            .name = name->data,
            .start_pc = (int)PROTO_CODE_COUNT(compiler->proto),
            .end_pc = -1,
            .reg = reg
        };
        DYNARRAY_ADD(&compiler->proto->locvars, locvar, XrLocVar);
    }

    return local;
}

// Define local variable (LIFO mode), returns NULL on failure
XrLocalInfo* scope_define_local(XrCompilerContext *ctx, XrCompiler *compiler, XrString *name) {
    XR_DCHECK(compiler != NULL, "scope_define_local: NULL compiler");
    (void)ctx;
    int reg = xreg_alloc_local(compiler->regalloc, name ? name->data : "<anonymous>");
    if (reg < 0) {
        return NULL;
    }
    XR_DCHECK(reg < XREG_MAX, "scope_define_local: register exceeds max");

    XrLocalInfo *local = scope_init_local_info(compiler, name, reg);

    // local_end = next available register number (not variable count)
    if (compiler->regalloc) {
        xreg_set_local_end(compiler->regalloc, reg + 1);

        if (xreg_get_freereg(compiler->regalloc) <= reg) {
            xreg_set_freereg(compiler->regalloc, reg + 1);
        }
    }

    return local;
}

// Define local variable with specific register (for method parameters)
XrLocalInfo* scope_define_local_reg(XrCompilerContext *ctx, XrCompiler *compiler, XrString *name, int reg) {
    (void)ctx;
    XrLocalInfo *local = scope_init_local_info(compiler, name, reg);

    if (compiler->regalloc) {
        xreg_set_local(compiler->regalloc, reg, name ? name->data : "<param>");
    }

    return local;
}

// Get compile-time type for an expression node (read-only, no inference).
// Primary source: Analyzer's cached compile_type on AST node.
// For variables: also checks Codegen's local info (which has native_width for raw slots).
XrType* get_expr_type(XrCompilerContext *ctx, XrCompiler *compiler, AstNode *expr) {
    if (!expr || !compiler) return NULL;

    // Read the inferred type from the analyzer's side table. Codegen
    // always has access to the owning analyzer via ctx->analyzer.
    XrType *ct_node = xa_analyzer_get_node_type(ctx->analyzer, expr);
    if (ct_node) {
        XrType *ct = ct_node;
        if (ct->kind != XR_KIND_UNKNOWN && ct->kind != XR_KIND_NEVER) {
            // For variables, prefer Codegen's local type (has native_width info)
            if (expr->type == AST_VARIABLE) {
                VariableNode *var = &expr->as.variable;
                if (var->name) {
                    XrLocalInfo *local = compiler_get_local_by_name(compiler, var->name);
                    if (local && local->compile_type) {
                        return local->compile_type;
                    }
                }
            }
            return ct;
        }
    }

    // Variable lookup: Codegen's local/shared type info (has native_width, Channel type)
    if (expr->type == AST_VARIABLE) {
        VariableNode *var = &expr->as.variable;
        if (var->name) {
            XrLocalInfo *local = compiler_get_local_by_name(compiler, var->name);
            if (local && local->compile_type) {
                return local->compile_type;
            }
            XrString *name_str = xr_compile_time_intern(ctx->X, var->name, strlen(var->name));
            int shared_idx = shared_get_in_scope(ctx, compiler, name_str);
            if (shared_idx >= 0) {
                XrType *shared_type = shared_get_type(ctx, shared_idx);
                if (shared_type) return shared_type;
            }
        }
    }

    // Call-site return type propagation: if callee is a named function with an
    // inferred Json return type (stored in shared_var->compile_type), return it.
    // This lets  let x = makeTree(d)  pick up the Json shape without annotations.
    if (expr->type == AST_CALL_EXPR) {
        CallExprNode *call = &expr->as.call_expr;
        if (call->callee && call->callee->type == AST_VARIABLE) {
            const char *fname = call->callee->as.variable.name;
            if (fname) {
                XrString *fn_str = xr_compile_time_intern(ctx->X, fname, strlen(fname));
                int si = shared_get_in_scope(ctx, compiler, fn_str);
                if (si >= 0) {
                    XrType *ret_type = shared_get_type(ctx, si);
                    if (ret_type && (ret_type->kind == XR_KIND_JSON)) {
                        return ret_type;
                    }
                }
            }
        }
    }

    return NULL;
}

int scope_resolve_local(XrCompiler *compiler, XrString *name) {
    XrLocalInfo *local = compiler_get_local_by_name(compiler, name->data);
    if (local) {
        return local->reg;
    }
    return -1;
}

// Add captured variable reference to this function's upvalue table.
static int scope_add_upvalue(XrCompilerContext *ctx, XrCompiler *compiler,
                              int index,
                              XrType *compile_type, uint8_t storage_mode,
                              bool is_const, XrString *name, uint8_t slot_type,
                              uint8_t source) {
    int upvalue_count = PROTO_UPVAL_COUNT(compiler->proto);

    for (int i = 0; i < upvalue_count; i++) {
        XrUpvalueDesc *upvalue = &compiler->upvalues[i];
        if (upvalue->source == source && upvalue->index == index) {
            return i;
        }
    }

    if (upvalue_count >= UINT8_MAX) {
        xr_compiler_error(ctx, compiler, "Too many upvalues");
        return 0;
    }

    compiler->upvalues[upvalue_count].index         = index;
    compiler->upvalues[upvalue_count].type_info     = compile_type;
    compiler->upvalues[upvalue_count].storage_mode  = storage_mode;
    compiler->upvalues[upvalue_count].slot_type     = slot_type;
    compiler->upvalues[upvalue_count].source        = source;
    compiler->upvalues[upvalue_count].is_const      = is_const;
    compiler->upvalues[upvalue_count].name          = name;

    xr_vm_proto_add_upvalue(compiler->proto, (uint8_t)index,
                            storage_mode, is_const, slot_type,
                            source, compile_type);
    return upvalue_count;
}

// Resolve captured variable reference for inner function.
// ALL captures are BY_VALUE now:
//   - const → direct value snapshot from register
//   - let   → cell ref snapshot from register (cellified before OP_CLOSURE)
// Direct captures use SRC_REG; transitive captures use SRC_UPVAL.
int scope_resolve_upvalue(XrCompilerContext *ctx, XrCompiler *compiler, XrString *name) {
    if (compiler->enclosing == NULL) {
        return -1;
    }

    XrCompiler *enclosing = compiler->enclosing;
    XrLocalInfo *local_info = compiler_get_local_by_name(enclosing, name->data);
    if (local_info) {
        // Direct capture: variable is a local in the immediately enclosing function.
        local_info->is_captured = true;
        // Assign capture index for cellification tracking
        if (local_info->ctx_slot < 0) {
            local_info->ctx_slot = enclosing->captured_count++;
        }
        uint8_t uv_slot = xr_type_to_slot_type(local_info->compile_type);

        // ALL direct captures are BY_VALUE via SRC_REG.
        // index stores the register number for OP_CLOSURE to read from.
        // Initialized const locals: snapshot raw value (is_const=true).
        // Uninitialized (hoisted) const/let locals: capture cell ref (is_const=false).
        // Already-cellified locals: register holds cell ref, must use CELL_GET.
        bool uv_const = local_info->is_const && !local_info->is_hoisted && !local_info->is_cellified;
        return scope_add_upvalue(ctx, compiler, local_info->reg,
                                  local_info->compile_type, local_info->storage_mode,
                                  uv_const, name, uv_slot,
                                  UPVAL_SRC_REG);
    }

    // Transitive capture: variable is an upvalue in enclosing function.
    // ALL transitive captures are BY_VALUE via SRC_UPVAL.
    int upvalue_idx = scope_resolve_upvalue(ctx, enclosing, name);
    if (upvalue_idx >= 0) {
        XrUpvalueDesc *outer_uv = &enclosing->upvalues[upvalue_idx];
        return scope_add_upvalue(ctx, compiler, upvalue_idx,
                                  outer_uv->type_info, outer_uv->storage_mode,
                                  outer_uv->is_const, outer_uv->name, outer_uv->slot_type,
                                  UPVAL_SRC_UPVAL);
    }

    return -1;
}

/* ========== Bytecode Stackmap Safepoint ========== */

// Record a GC safepoint at the last emitted instruction's PC.
// Bitmap: bits [0, freereg) are set (all live), bits >= freereg are clear (dead).
void xr_codegen_record_gc_safepoint(XrCompiler *compiler) {
    if (!compiler || !compiler->emitter || !compiler->regalloc) return;

    int pc = compiler->emitter->pc - 1;  // last emitted instruction
    if (pc < 0) return;

    int freereg = xreg_get_freereg(compiler->regalloc);
    if (freereg <= 0) return;

    uint16_t maxslots = (uint16_t)freereg;
    uint16_t num_words = (maxslots + 63) / 64;

    // Lazy-create the builder
    if (!compiler->bc_stackmap_builder) {
        int watermark = xreg_watermark(compiler->regalloc);
        uint16_t max_est = (watermark > freereg) ? (uint16_t)watermark : maxslots;
        if (max_est < 64) max_est = 64;  // minimum bitmap width
        compiler->bc_stackmap_builder = xr_bc_stackmap_builder_create(max_est);
        if (!compiler->bc_stackmap_builder) return;
    }

    // Build bitmap: all bits [0, freereg) set
    uint64_t bitmap[16] = {0};  // supports up to 1024 slots (64*16)
    XR_DCHECK(num_words <= 16, "safepoint: too many bitmap words");
    for (uint16_t w = 0; w < num_words; w++) {
        uint32_t lo = w * 64;
        uint32_t hi = lo + 64;
        if (hi > (uint32_t)freereg) hi = (uint32_t)freereg;
        if (lo >= (uint32_t)freereg) break;
        uint32_t nbits = hi - lo;
        bitmap[w] = (nbits >= 64) ? ~(uint64_t)0 : ((uint64_t)1 << nbits) - 1;
    }

    xr_bc_stackmap_builder_add(compiler->bc_stackmap_builder, (uint32_t)pc, bitmap);
}

/* ========== Compiler Initialization ========== */

void xr_compiler_init(XrCompilerContext *ctx, XrCompiler *compiler, XrFunctionType type) {
    compiler->enclosing = ctx->current;
    compiler->proto = xr_vm_proto_new();
    compiler->proto->source_file = ctx->source_file;
    compiler->proto->shared_offset = ctx->shared_offset;
    compiler->type = type;
    compiler->local_count = 0;
    compiler->captured_count = 0;
    XR_AVEC_INIT(compiler->prescan_captured);
    compiler->scope_depth = 0;
    compiler->scope_block_depth = 0;
    compiler->block_depth = 0;
    compiler->loop_depth = 0;
    compiler->loop_start = 0;
    compiler->loop_continue = 0;
    compiler->loop_scope = 0;
    XR_AVEC_INIT(compiler->break_jumps);
    XR_AVEC_INIT(compiler->continue_jumps);

    // Initialize BCE (Bounds Check Elimination) info
    compiler->bce_loop_var = NULL;
    compiler->bce_limit_var = NULL;
    compiler->bce_loop_var_reg = -1;
    compiler->had_error = false;
    compiler->panic_mode = false;

    compiler->arena = &ctx->arena;
    compiler->inst_type_buf = NULL;
    compiler->inst_type_cap = 0;
    compiler->struct_area_offset = 0;
    compiler->declared_return_type = NULL;
    compiler->bc_stackmap_builder = NULL;  // created lazily on first safepoint
    xr_local_list_init(&compiler->local_list);

    compiler->regalloc = xreg_new();
    if (!compiler->regalloc) {
        xr_log_warning("compiler", "failed to create register allocator");
        compiler->had_error = true;
    }

    compiler->emitter = emitter_new(ctx, compiler->proto, compiler->regalloc);
    if (!compiler->emitter) {
        xr_log_warning("compiler", "failed to create emitter");
        compiler->had_error = true;
    }

    #ifdef XR_DEBUG_REGALLOC_VERBOSE
    if (compiler->regalloc) {
        xreg_set_debug(compiler->regalloc, true);
    }
    #endif

    #ifdef XR_DEBUG_EMITTER
    if (compiler->emitter) {
        emitter_set_debug(compiler->emitter, true);
    }
    #endif

    compiler->globals = ctx->global_vars;
    compiler->global_count = &ctx->global_var_count;

    ctx->current = compiler;

}

/* Cellify captured mutable locals before OP_CLOSURE emission.
 * Called before every OP_CLOSURE emit so that nested closures capture
 * cell references rather than raw register values. */
void emit_ctx_sync_before_closure(XrCompilerContext *ctx, XrCompiler *compiler) {
    (void)ctx;
    if (compiler->captured_count == 0) return;

    // Cellify captured mutable (non-const) locals before closure creation.
    // OP_CELL_NEW wraps the register value in a heap cell; subsequent reads/writes
    // in the outer function go through OP_CELL_GET/SET.
    // OP_CLOSURE reads the cell ref directly from the register (SRC_REG).
    XrLocalList *ll = &compiler->local_list;
    for (int i = 0; i < ll->count; i++) {
        XrLocalInfo *local = xr_local_list_get(ll, i);
        if (!local || !local->is_captured || local->ctx_slot < 0) continue;
        if (local->is_cellified) continue;  // already wrapped
        // Skip const locals that are already initialized (value won't change).
        // But keep uninitialized (hoisted) const locals: they need cells because
        // the closure is created before their value is assigned (Phase 2 vs Phase 3).
        if (local->is_const && !local->is_hoisted) continue;
        if (local->is_hoisted) {
            // Hoisted variable: register not yet initialized by user code.
            // Pre-create cell with null so the closure captures a valid cell ref.
            // Actual value will be written via CELL_SET at initialization time.
            xemit_loadnull(compiler->emitter, local->reg);
        }
        xemit_cell_new(compiler->emitter, local->reg);
        local->is_cellified = true;
    }
}

XrProto *xr_compiler_end(XrCompilerContext *ctx, XrCompiler *compiler) {
    EMIT_RETURN(ctx, compiler, 0, 0);


    XrProto *proto = compiler->proto;
    ctx->current = compiler->enclosing;

    if (compiler->emitter) {
        #ifdef XR_DEBUG_EMITTER
        emitter_print_stats(compiler->emitter);
        #endif
        emitter_free(compiler->emitter);
        compiler->emitter = NULL;
    }

    if (compiler->regalloc) {
        #ifdef XR_DEBUG_REGALLOC
        xreg_print_stats(compiler->regalloc);
        #endif
        proto->maxstacksize = xreg_watermark(compiler->regalloc);
        xreg_free(compiler->regalloc);
        compiler->regalloc = NULL;
    }

    // Struct native storage: record size (allocated independently per call frame)
    if (compiler->struct_area_offset > 0) {
        proto->struct_area_size = compiler->struct_area_offset;
    }

    if (compiler->had_error) {
        xr_vm_proto_free(proto);
        return NULL;
    }

    // Generate param_types: per-parameter XrType* (authoritative source).
    // Parameters are always alive at function end, so compile_type is available.
    {
        int lcount = compiler_get_local_count(compiler);
        if (proto->numparams > 0) {
            proto->param_types = (struct XrType **)xr_calloc(
                proto->numparams, sizeof(struct XrType *));
            proto->param_types_count = proto->numparams;
            for (int i = 0; i < proto->numparams && i < lcount; i++) {
                XrLocalInfo *loc = compiler_get_local_at(compiler, i);
                if (loc && loc->compile_type) {
                    proto->param_types[i] = loc->compile_type;
                }
            }
        }
    }

    // Generate bytecode stackmap: transfer builder → proto->bc_stackmap.
    if (compiler->bc_stackmap_builder) {
        proto->bc_stackmap = xr_bc_stackmap_builder_finish(compiler->bc_stackmap_builder);
        compiler->bc_stackmap_builder = NULL;  // ownership transferred or freed
    }

    // Generate inst_types: per-PC XrType* (flow-sensitive, authoritative for non-params).
    // Transfer from compiler's inst_type_buf to proto->inst_types.
    {
        int code_count = PROTO_CODE_COUNT(proto);
        if (compiler->inst_type_buf && code_count > 0) {
            bool has_any = false;
            int limit = (compiler->inst_type_cap < code_count)
                        ? compiler->inst_type_cap : code_count;
            for (int i = 0; i < limit; i++) {
                if (compiler->inst_type_buf[i]) { has_any = true; break; }
            }
            if (has_any) {
                proto->inst_types = (struct XrType **)xr_calloc(
                    code_count, sizeof(struct XrType *));
                if (proto->inst_types) {
                    proto->inst_types_count = code_count;
                    for (int i = 0; i < limit; i++) {
                        proto->inst_types[i] = compiler->inst_type_buf[i];
                    }
                }
            }
        }
        // Free temporary buffer
        if (compiler->inst_type_buf) {
            xr_free(compiler->inst_type_buf);
            compiler->inst_type_buf = NULL;
            compiler->inst_type_cap = 0;
        }
    }

    // Apply compiler optimizations
    xr_peephole_optimize(proto);
    xr_fusion_optimize(proto);
    xr_inline_detect_indirect_recursion(proto);
    xr_inline_mark_candidates(proto);

    // Collect loop headers for JIT/AOT (OSR entry points) — single pass
    {
        int code_count = PROTO_CODE_COUNT(proto);
        // Pass 1: count loops (backward JMP)
        int loop_count = 0;
        for (int pc = 0; pc < code_count; pc++) {
            XrInstruction inst = PROTO_CODE(proto, pc);
            OpCode op = GET_OPCODE(inst);
            if (op == OP_JMP && GETARG_sJ(inst) < 0) {
                loop_count++;
            }
        }
        // Pass 2: collect targets
        if (loop_count > 0) {
            proto->loop_headers = (int16_t *)xr_malloc(loop_count * sizeof(int16_t));
            proto->loop_header_count = 0;
            for (int pc = 0; pc < code_count; pc++) {
                XrInstruction inst = PROTO_CODE(proto, pc);
                OpCode op = GET_OPCODE(inst);
                int target = -1;
                if (op == OP_JMP) {
                    int sj = GETARG_sJ(inst);
                    if (sj < 0) target = pc + sj + 1;
                }
                if (target >= 0 && target < code_count &&
                    proto->loop_header_count < loop_count) {
                    proto->loop_headers[proto->loop_header_count++] = (int16_t)target;
                }
            }
            if (proto->loop_header_count == 0) {
                xr_free(proto->loop_headers);
                proto->loop_headers = NULL;
            }
        }
    }

    // Generate basic block leader bitmap for JIT CFG construction
    {
        int code_count = PROTO_CODE_COUNT(proto);
        if (code_count > 0) {
            int bitmap_bytes = (code_count + 7) / 8;
            uint8_t *bm = (uint8_t *)xr_calloc(bitmap_bytes, 1);

            // PC 0 is always a leader
            bm[0] |= 1;

            for (int pc = 0; pc < code_count; pc++) {
                XrInstruction inst = PROTO_CODE(proto, pc);
                OpCode op = GET_OPCODE(inst);

                switch (op) {
                    case OP_JMP: {
                        int sj = GETARG_sJ(inst);
                        int target = pc + sj + 1;
                        if (target >= 0 && target < code_count)
                            bm[target / 8] |= (1 << (target % 8));
                        // Fallthrough after unconditional jump is dead, but mark anyway
                        if (pc + 1 < code_count)
                            bm[(pc + 1) / 8] |= (1 << ((pc + 1) % 8));
                        break;
                    }
                    case OP_RETURN:
                    case OP_RETURN0:
                    case OP_RETURN1:
                    case OP_TAILCALL:
                        // Next instruction starts a new block (if reachable)
                        if (pc + 1 < code_count)
                            bm[(pc + 1) / 8] |= (1 << ((pc + 1) % 8));
                        break;

                    // Conditional instructions: both paths are leaders
                    case OP_EQ: case OP_EQK: case OP_EQI:
                    case OP_LT: case OP_LTI: case OP_LE: case OP_LEI:
                    case OP_TEST: case OP_TESTSET:
                        if (pc + 1 < code_count)
                            bm[(pc + 1) / 8] |= (1 << ((pc + 1) % 8));
                        if (pc + 2 < code_count)
                            bm[(pc + 2) / 8] |= (1 << ((pc + 2) % 8));
                        break;

                    default:
                        break;
                }
            }

            proto->bb_leaders = bm;
            proto->bb_leaders_size = bitmap_bytes;
        }
    }

    // Generate Blueprint: compiler-provided JIT metadata (per-instruction types,
    // loop live maps). Must be after bb_leaders, loop_headers, and param_types
    // are all populated. Blueprint.inst_info is the single source of truth for
    // per-PC result types (replaces the former pc_result_tags).
    proto->blueprint = (struct XrBlueprint *)xr_blueprint_generate(proto);

    // Compute coroutine safety flag
    // Safe if all upvalues are one of:
    //   - shared (global heap + refcount)
    //   - const (immutable, deep copy to new coro heap is safe)
    //   - self-recursive reference
    bool is_safe = true;
    int upvalue_count = PROTO_UPVAL_COUNT(proto);
    for (int i = 0; i < upvalue_count; i++) {
        UpvalInfo uv = PROTO_UPVALUE(proto, i);
        // shared variables (storage_mode != 0) are safe - global heap with refcount
        if (uv.storage_mode != 0) continue;
        // const variables are safe - immutable, deep copied to new coro heap
        if (uv.is_const) continue;
        // self-recursive reference is safe
        bool is_self_ref = false;
        if (proto->name && compiler->upvalues[i].name) {
            if (compiler->upvalues[i].name == proto->name) {
                is_self_ref = true;
            }
        }
        if (!is_self_ref) {
            is_safe = false;
            break;
        }
    }
    proto->is_coro_safe = is_safe;

    return proto;
}

/* ========== Expression Compilation ========== */

// Compile expression (main dispatch), returns result register.
// Delegates to xr_compile_expr (XrExprDesc-based) and materializes into a register.
int xr_compile_expression(XrCompilerContext *ctx, XrCompiler *compiler, AstNode *node) {
    XR_DCHECK(ctx != NULL, "compile_expression: NULL context");
    XR_DCHECK(compiler != NULL, "compile_expression: NULL compiler");
    if (node == NULL) {
        xr_compiler_error(ctx, compiler, "NULL expression node");
        return reg_alloc(ctx, compiler);
    }

    XrExprDesc e = xr_compile_expr(ctx, compiler, node);
    return xexpr_to_anyreg(ctx, compiler, &e);
}

/* ========== Statement Compilation ========== */

// Compile statement (main dispatch)
void xr_compile_statement(XrCompilerContext *ctx, XrCompiler *compiler, AstNode *node) {
    XR_DCHECK(ctx != NULL, "compile_statement: NULL context");
    XR_DCHECK(compiler != NULL, "compile_statement: NULL compiler");
    if (node == NULL) {
        return;
    }

    ctx->current_line = node->line;

    switch (node->type) {
        case AST_EXPR_STMT:
            compile_expr_stmt(ctx, compiler, node->as.expr_stmt);
            break;

        case AST_PRINT_STMT:
            compile_print(ctx, compiler, &node->as.print_stmt);
            break;

        case AST_VAR_DECL:
        case AST_CONST_DECL:
            compile_var_decl(ctx, compiler, &node->as.var_decl);
            break;

        case AST_DESTRUCTURE_DECL:
            compile_destructure_decl(ctx, compiler, &node->as.destructure_decl);
            break;

        case AST_MULTI_VAR_DECL:
            compile_multi_var_decl(ctx, compiler, &node->as.multi_var_decl);
            break;

        case AST_MULTI_ASSIGN:
            compile_multi_assign(ctx, compiler, &node->as.multi_assign);
            break;

        case AST_ASSIGNMENT:
            compile_assignment(ctx, compiler, &node->as.assignment);
            break;

        case AST_COMPOUND_ASSIGNMENT:
            compile_compound_assignment(ctx, compiler, &node->as.compound_assignment);
            break;

        case AST_INC:
            compile_inc(ctx, compiler, &node->as.inc);
            break;

        case AST_DEC:
            compile_dec(ctx, compiler, &node->as.dec);
            break;

        case AST_DESTRUCTURE_ASSIGN:
            compile_destructure_assign(ctx, compiler, &node->as.destructure_assign);
            break;

        case AST_IF_STMT:
            compile_if(ctx, compiler, &node->as.if_stmt);
            break;

        // Control flow
        case AST_WHILE_STMT:
            compile_while(ctx, compiler, &node->as.while_stmt);
            break;

        case AST_FOR_STMT:
            compile_for(ctx, compiler, &node->as.for_stmt);
            break;

        case AST_FOR_IN_STMT:
            compile_for_in(ctx, compiler, &node->as.for_in_stmt);
            break;

        case AST_FUNCTION_DECL:
            compile_function(ctx, compiler, &node->as.function_decl);
            break;

        case AST_RETURN_STMT:
            compile_return(ctx, compiler, &node->as.return_stmt);
            break;

        case AST_BREAK_STMT:
            if (compiler->loop_depth == 0) {
                xr_compiler_error(ctx, compiler, "Cannot use 'break' outside of loop");
            } else {
                int jump_pos = EMIT_JUMP(ctx, compiler, OP_JMP);
                XR_AVEC_PUSH(compiler->arena, compiler->break_jumps, jump_pos);
            }
            break;

        case AST_CONTINUE_STMT:
            if (compiler->loop_depth == 0) {
                xr_compiler_error(ctx, compiler, "Cannot use 'continue' outside of loop");
            } else {
                int jump_pos = EMIT_JUMP(ctx, compiler, OP_JMP);
                XR_AVEC_PUSH(compiler->arena, compiler->continue_jumps, jump_pos);
            }
            break;

        case AST_INDEX_SET:
            compile_index_set(ctx, compiler, &node->as.index_set);
            break;

        case AST_BLOCK: {
            BlockNode *block = &node->as.block;
            scope_begin(compiler);
            // Phase 1: Hoist all names (register slots only)
            for (int i = 0; i < block->count && !compiler->had_error; i++) {
                AstNode *s = block->statements[i];
                if (!s) continue;
                if (s->type == AST_FUNCTION_DECL) {
                    compile_function_decl_only(ctx, compiler, &s->as.function_decl);
                }
                // Pre-register variable names so that fn bodies
                // compiled in Phase 2 can resolve references.
                if ((s->type == AST_VAR_DECL || s->type == AST_CONST_DECL) && s->as.var_decl.name) {
                    VarDeclNode *vd = &s->as.var_decl;
                    bool is_ch = (vd->initializer && vd->initializer->type == AST_CHANNEL_NEW);
                    if (vd->storage_mode == XR_STORAGE_SHARED || is_ch) {
                        XrString *ns = xr_compile_time_intern(ctx->X, vd->name, strlen(vd->name));
                        int si = shared_get_in_scope(ctx, compiler, ns);
                        if (si < 0) {
                            si = shared_add(ctx, compiler, ns);
                            shared_set_const(ctx, si, vd->is_const);
                        }
                        // Early type tagging for Channel (same as AST_PROGRAM path)
                        if (vd->initializer && vd->initializer->type == AST_CHANNEL_NEW) {
                            XrType *ch_type = vd->type_annotation;
                            if (!ch_type || ch_type->kind != XR_KIND_CHANNEL) {
                                ch_type = xr_type_new_channel(ctx->X, xr_type_new_unknown(NULL));
                            }
                            shared_set_type(ctx, si, ch_type);
                        }
                    } else {
                        XrString *ns = xr_compile_time_intern(ctx->X, vd->name, strlen(vd->name));
                        XrLocalInfo *existing = compiler_get_local_by_name(compiler, vd->name);
                        if (!existing || existing->depth != compiler->scope_depth) {
                            XrLocalInfo *local = scope_define_local(ctx, compiler, ns);
                            local->is_const = vd->is_const;
                            local->is_hoisted = true;
                            // Set compile_type early so Phase 2 fn compilation
                            // sees correct types for captured variables (upval type_info).
                            if (vd->type_annotation) {
                                local_set_compile_type(local, vd->type_annotation);
                            } else if (vd->initializer) {
                                XrType *ct = get_expr_type(ctx, compiler, vd->initializer);
                                if (ct && ct->kind != XR_KIND_NULL)
                                    local_set_compile_type(local, ct);
                            }
                        }
                    }
                }
            }
            // Phase 2: Compile function declarations first (closures available before use)
            // Note: CTX_NEW is emitted at function entry by xr_compiler_init
            // and backpatched by xr_compiler_end.
            for (int i = 0; i < block->count && !compiler->had_error; i++) {
                if (block->statements[i] && block->statements[i]->type == AST_FUNCTION_DECL) {
                    xr_compile_statement(ctx, compiler, block->statements[i]);
                }
            }
            // CTX_NEW backpatch is handled by emit_ctx_sync_before_closure
            // and xr_compiler_end.
            // Phase 3: Compile remaining statements in source order
            for (int i = 0; i < block->count && !compiler->had_error; i++) {
                if (!block->statements[i]) continue;
                if (block->statements[i]->type == AST_FUNCTION_DECL) continue;
                xr_compile_statement(ctx, compiler, block->statements[i]);
            }
            scope_end(ctx, compiler);
            break;
        }

        case AST_PROGRAM: {
            ProgramNode *program = &node->as.program;
            // Phase 1: Hoist all declaration names (register slots only)
            // This enables forward references: functions can reference variables
            // and classes defined later in the source, and vice versa.
            for (int i = 0; i < program->count && !compiler->had_error; i++) {
                AstNode *s = program->statements[i];
                if (!s) continue;
                if (s->type == AST_FUNCTION_DECL) {
                    compile_function_decl_only(ctx, compiler, &s->as.function_decl);
                } else if (s->type == AST_EXPORT_STMT && s->as.export_stmt.declaration &&
                           s->as.export_stmt.declaration->type == AST_FUNCTION_DECL) {
                    compile_function_decl_only(ctx, compiler,
                        &s->as.export_stmt.declaration->as.function_decl);
                }
                // Pre-register module-level variable names so that fn bodies
                // compiled in Phase 2 can resolve upvalue references to them.
                VarDeclNode *vd = NULL;
                if (s->type == AST_VAR_DECL || s->type == AST_CONST_DECL) {
                    vd = &s->as.var_decl;
                } else if (s->type == AST_EXPORT_STMT && s->as.export_stmt.declaration &&
                           (s->as.export_stmt.declaration->type == AST_VAR_DECL ||
                            s->as.export_stmt.declaration->type == AST_CONST_DECL)) {
                    vd = &s->as.export_stmt.declaration->as.var_decl;
                }
                bool is_ch_decl = (vd && vd->initializer && vd->initializer->type == AST_CHANNEL_NEW);
                if (vd && vd->name && (vd->storage_mode == XR_STORAGE_SHARED || is_ch_decl)) {
                    // Pre-register shared/Channel variables so fn bodies in Phase 2
                    // can resolve references to them via GETSHARED.
                    XrString *ns = xr_compile_time_intern(ctx->X, vd->name, strlen(vd->name));
                    int si = shared_get_in_scope(ctx, compiler, ns);
                    if (si < 0) {
                        si = shared_add(ctx, compiler, ns);
                        shared_set_const(ctx, si, vd->is_const);
                    }
                    // Early type tagging: if initializer is Channel(...), set type
                    // so Phase 2 fn bodies can emit OP_CHAN_SEND/RECV instead of INVOKE.
                    if (vd->initializer && vd->initializer->type == AST_CHANNEL_NEW) {
                        XrType *ch_type = vd->type_annotation;
                        if (!ch_type || ch_type->kind != XR_KIND_CHANNEL) {
                            ch_type = xr_type_new_channel(ctx->X, xr_type_new_unknown(NULL));
                        }
                        shared_set_type(ctx, si, ch_type);
                    }
                } else if (vd && vd->name && vd->storage_mode != XR_STORAGE_SHARED &&
                    compiler->type == FUNCTION_SCRIPT && !is_repl_top_level(ctx, compiler)) {
                    XrString *name_str = xr_compile_time_intern(ctx->X, vd->name, strlen(vd->name));
                    XrLocalInfo *existing = compiler_get_local_by_name(compiler, vd->name);
                    if (!existing) {
                        XrLocalInfo *local = scope_define_local(ctx, compiler, name_str);
                        local->is_const = vd->is_const;
                        local->is_hoisted = true;
                        // Set compile_type early (same as AST_BLOCK Phase 1)
                        if (vd->type_annotation) {
                            local_set_compile_type(local, vd->type_annotation);
                        } else if (vd->initializer) {
                            XrType *ct = get_expr_type(ctx, compiler, vd->initializer);
                            if (ct && ct->kind != XR_KIND_NULL)
                                local_set_compile_type(local, ct);
                        }
                    }
                }
                // Pre-register class/enum names (shared variables) so that fn bodies
                // compiled in Phase 2 can resolve references to them.
                const char *decl_name = NULL;
                AstNode *decl = s;
                if (s->type == AST_EXPORT_STMT && s->as.export_stmt.declaration)
                    decl = s->as.export_stmt.declaration;
                if (decl->type == AST_CLASS_DECL && decl->as.class_decl.name)
                    decl_name = decl->as.class_decl.name;
                else if (decl->type == AST_STRUCT_DECL && decl->as.struct_decl.name)
                    decl_name = decl->as.struct_decl.name;
                else if (decl->type == AST_ENUM_DECL && decl->as.enum_decl.name)
                    decl_name = decl->as.enum_decl.name;
                else if (decl->type == AST_INTERFACE_DECL && decl->as.interface_decl.name)
                    decl_name = decl->as.interface_decl.name;
                if (decl_name && compiler->scope_depth == 0) {
                    XrString *ns = xr_compile_time_intern(ctx->X, decl_name, strlen(decl_name));
                    int si = shared_get_in_scope(ctx, compiler, ns);
                    if (si < 0) {
                        si = shared_add(ctx, compiler, ns);
                        shared_set_const(ctx, si, true);
                    }
                }
            }
            // Phase 2: Compile all declarations in source order
            // (class, enum, interface, fn, type — no side effects, just bind names to values)
            for (int i = 0; i < program->count && !compiler->had_error; i++) {
                AstNode *s = program->statements[i];
                if (!s) continue;
                switch (s->type) {
                case AST_CLASS_DECL:
                case AST_STRUCT_DECL:
                case AST_ENUM_DECL:
                case AST_INTERFACE_DECL:
                case AST_TYPE_ALIAS:
                case AST_FUNCTION_DECL:
                case AST_IMPORT_STMT:
                    xr_compile_statement(ctx, compiler, s);
                    break;
                case AST_EXPORT_STMT:
                    if (s->as.export_stmt.declaration) {
                        AstNodeType dt = s->as.export_stmt.declaration->type;
                        if (dt == AST_CLASS_DECL || dt == AST_STRUCT_DECL || dt == AST_ENUM_DECL ||
                            dt == AST_INTERFACE_DECL || dt == AST_TYPE_ALIAS ||
                            dt == AST_FUNCTION_DECL) {
                            xr_compile_statement(ctx, compiler, s);
                        }
                    }
                    break;
                default: break;
                }
            }
            // Phase 3: Compile remaining statements in source order
            // (let/const, expressions, imports, control flow — may call functions/use classes)
            int last_idx = program->count - 1;
            for (int i = 0; i < program->count && !compiler->had_error; i++) {
                AstNode *stmt = program->statements[i];
                if (!stmt) continue;
                // Skip declarations (already compiled in Phase 2)
                switch (stmt->type) {
                case AST_CLASS_DECL:
                case AST_STRUCT_DECL:
                case AST_ENUM_DECL:
                case AST_INTERFACE_DECL:
                case AST_TYPE_ALIAS:
                case AST_FUNCTION_DECL:
                case AST_IMPORT_STMT:
                    continue;
                case AST_EXPORT_STMT:
                    if (stmt->as.export_stmt.declaration) {
                        AstNodeType dt = stmt->as.export_stmt.declaration->type;
                        if (dt == AST_CLASS_DECL || dt == AST_STRUCT_DECL || dt == AST_ENUM_DECL ||
                            dt == AST_INTERFACE_DECL || dt == AST_TYPE_ALIAS ||
                            dt == AST_FUNCTION_DECL) {
                            continue;
                        }
                    }
                    break;
                default: break;
                }

                // REPL auto-display: emit OP_DUMP for last expression statement
                if (ctx->repl_mode && i == last_idx && stmt->type == AST_EXPR_STMT) {
                    AstNode *expr = stmt->as.expr_stmt;
                    // Skip side-effect statements and function calls
                    if (expr && expr->type != AST_ASSIGNMENT &&
                        expr->type != AST_COMPOUND_ASSIGNMENT &&
                        expr->type != AST_INC && expr->type != AST_DEC &&
                        expr->type != AST_MEMBER_SET && expr->type != AST_INDEX_SET &&
                        expr->type != AST_CALL_EXPR) {
                        XrExprDesc e = xr_compile_expr(ctx, compiler, expr);
                        if (e.kind != XEXPR_VOID) {
                            int reg = xexpr_to_anyreg(ctx, compiler, &e);
                            xemit_dump((compiler)->emitter, reg, 2);
                            reg_free(compiler, reg);
                        }
                        continue;
                    }
                }

                xr_compile_statement(ctx, compiler, stmt);
            }
            break;
        }

        // OOP type definitions
        case AST_CLASS_DECL:
            compile_class(ctx, compiler, &node->as.class_decl);
            break;

        case AST_STRUCT_DECL:
            ctx->is_compiling_struct = true;
            compile_class(ctx, compiler, &node->as.struct_decl);
            ctx->is_compiling_struct = false;
            break;

        case AST_INTERFACE_DECL:
            compile_interface(ctx, compiler, &node->as.interface_decl);
            break;

        case AST_ENUM_DECL:
            compile_enum_decl(ctx, compiler, &node->as.enum_decl);
            break;

        case AST_MEMBER_SET:
            compile_member_set(ctx, compiler, &node->as.member_set);
            break;

        // Exception handling
        case AST_TRY_CATCH:
            compile_try_catch(ctx, compiler, &node->as.try_catch);
            break;

        case AST_THROW_STMT:
            compile_throw(ctx, compiler, &node->as.throw_stmt);
            break;

        // Module system
        case AST_IMPORT_STMT:
            compile_import(ctx, compiler, &node->as.import_stmt);
            break;

        case AST_EXPORT_STMT:
            compile_export(ctx, compiler, &node->as.export_stmt);
            break;

        // Coroutine statements
        case AST_DEFER_STMT:
            compile_defer_stmt(ctx, compiler, &node->as.defer_stmt);
            break;

        case AST_SELECT_STMT:
            compile_select_stmt(ctx, compiler, &node->as.select_stmt);
            break;

        case AST_SCOPE_BLOCK:
            compile_scope_block(ctx, compiler, &node->as.scope_block, -1);
            break;

        case AST_GO_EXPR: {
            // linked go / monitored go dispatched as statement from context keyword path
            int target = reg_alloc(ctx, compiler);
            compile_go_expr(ctx, compiler, &node->as.go_expr, target, true);
            reg_free(compiler, target);
            break;
        }

        case AST_YIELD_STMT:
            xemit_yield((compiler)->emitter, 0);
            break;

        case AST_TYPE_ALIAS:
            // Type alias is compile-time only, no code generation
            break;

        default:
            xr_compiler_error(ctx, compiler, "Unsupported statement type: %d", node->type);
            break;
    }

    // Reset freereg after each statement (release temp registers)
    if (compiler->regalloc) {
        int local_end = xreg_get_local_end(compiler->regalloc);
        xreg_set_freereg(compiler->regalloc, local_end);
    }
}

/* ========== Builtin Global Lookup (read-only, predefined) ========== */

// Sorted table for O(log n) binary search lookup
static const struct { const char *name; int index; } builtin_sorted[] = {
    {"Array",        XR_GLOBAL_VAR_ARRAY},
    {"ArrayBuffer",  XR_GLOBAL_VAR_ARRAYBUFFER},
    {"Bytes",        XR_GLOBAL_VAR_BYTES},
    {"Float32Array", XR_GLOBAL_VAR_FLOAT32ARRAY},
    {"Float64Array", XR_GLOBAL_VAR_FLOAT64ARRAY},
    {"Int16Array",   XR_GLOBAL_VAR_INT16ARRAY},
    {"Int32Array",   XR_GLOBAL_VAR_INT32ARRAY},
    {"Int8Array",    XR_GLOBAL_VAR_INT8ARRAY},
    {"Json",         XR_GLOBAL_VAR_JSON},
    {"Map",          XR_GLOBAL_VAR_MAP},
    {"Reflect",      XR_GLOBAL_VAR_REFLECT},
    {"Set",          XR_GLOBAL_VAR_SET},
    {"String",       XR_GLOBAL_VAR_STRING},
    {"Uint16Array",  XR_GLOBAL_VAR_UINT16ARRAY},
    {"Uint32Array",  XR_GLOBAL_VAR_UINT32ARRAY},
    {"Uint8Array",   XR_GLOBAL_VAR_UINT8ARRAY},
    {"__dir__",      XR_GLOBAL_VAR_DIR},
    {"__file__",     XR_GLOBAL_VAR_FILE},
    {"process",      XR_GLOBAL_VAR_PROCESS},
};
#define BUILTIN_SORTED_COUNT (int)(sizeof(builtin_sorted) / sizeof(builtin_sorted[0]))

// Find predefined builtin global by name, returns index or -1
int builtin_get(XrCompilerContext *ctx, XrString *name) {
    (void)ctx;
    int lo = 0, hi = BUILTIN_SORTED_COUNT - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int cmp = strcmp(name->data, builtin_sorted[mid].name);
        if (cmp == 0) return builtin_sorted[mid].index;
        if (cmp < 0) hi = mid - 1;
        else lo = mid + 1;
    }
    return -1;
}

/* ========== Shared Variable Management ========== */

// Helper: calculate function nesting depth
static int get_function_depth(XrCompiler *compiler) {
    int depth = 0;
    for (XrCompiler *c = compiler; c != NULL; c = c->enclosing) {
        depth++;
    }
    return depth;
}

// Find shared variable considering lexical scope (from innermost to outer)
int shared_get_in_scope(XrCompilerContext *ctx, XrCompiler *compiler, XrString *name) {
    int current_scope = compiler ? compiler->scope_depth : 0;
    int current_func = get_function_depth(compiler);
    int best_index = -1;
    int best_func = -1;
    int best_scope = -1;

    for (int i = 0; i < ctx->shared_var_count; i++) {
        if (ctx->shared_vars[i].name != NULL &&
            strcmp(ctx->shared_vars[i].name->data, name->data) == 0) {
            int var_func = ctx->shared_vars[i].function_depth;
            int var_scope = ctx->shared_vars[i].scope_depth;

            // Only visible if in same or outer function context
            if (var_func <= current_func) {
                // Prefer innermost function, then innermost scope
                if (var_func > best_func ||
                    (var_func == best_func && var_scope > best_scope && var_scope <= current_scope)) {
                    best_index = i;
                    best_func = var_func;
                    best_scope = var_scope;
                }
            }
        }
    }
    return best_index;
}

// Find shared variable (simple lookup, ignores scope)
int shared_get(XrCompilerContext *ctx, XrString *name) {
    for (int i = ctx->shared_var_count - 1; i >= 0; i--) {
        if (ctx->shared_vars[i].name != NULL &&
            strcmp(ctx->shared_vars[i].name->data, name->data) == 0) {
            return i;
        }
    }
    return -1;
}

// Check if shared variable already exists in current scope
static int shared_exists_in_current_scope(XrCompilerContext *ctx, XrCompiler *compiler, XrString *name) {
    int current_scope = compiler ? compiler->scope_depth : 0;
    int current_func = get_function_depth(compiler);
    for (int i = 0; i < ctx->shared_var_count; i++) {
        if (ctx->shared_vars[i].name != NULL &&
            ctx->shared_vars[i].scope_depth == current_scope &&
            ctx->shared_vars[i].function_depth == current_func &&
            strcmp(ctx->shared_vars[i].name->data, name->data) == 0) {
            return i;
        }
    }
    return -1;
}

// Add new shared variable (for definition), allows shadowing in different scopes
int shared_add(XrCompilerContext *ctx, XrCompiler *compiler, XrString *name) {
    int existing = shared_exists_in_current_scope(ctx, compiler, name);
    if (existing >= 0) {
        // REPL mode: allow redefinition (reuse existing slot)
        if (ctx->repl_mode) {
            return existing;
        }
        xr_compiler_error(ctx, compiler,
            "shared variable '%s' already defined in current scope",
            name->data);
        return existing;
    }

    if (ctx->shared_var_count >= ctx->shared_var_capacity) {
        int new_capacity = ctx->shared_var_capacity * 2;
        XrSharedVar *new_vars = (XrSharedVar*)xr_realloc(ctx->shared_vars,
            sizeof(XrSharedVar) * new_capacity);
        if (!new_vars) {
            xr_compiler_error(ctx, compiler, "shared variable memory allocation failed");
            return 0;
        }
        for (int i = ctx->shared_var_capacity; i < new_capacity; i++) {
            new_vars[i].name = NULL;
            new_vars[i].index = -1;
            new_vars[i].is_const = false;
        }
        ctx->shared_vars = new_vars;
        ctx->shared_var_capacity = new_capacity;
    }

    int index = ctx->shared_var_count++;
    ctx->shared_vars[index].name = name;
    ctx->shared_vars[index].index = index;
    ctx->shared_vars[index].scope_depth = compiler ? compiler->scope_depth : 0;
    ctx->shared_vars[index].function_depth = get_function_depth(compiler);
    ctx->shared_vars[index].is_const = false;
    ctx->shared_vars[index].state = SHARED_STATE_OWNED;

    return index;
}

// Get or add shared variable (for access scenarios like module import)
int shared_get_or_add(XrCompilerContext *ctx, XrCompiler *compiler, XrString *name) {
    int index = shared_get_in_scope(ctx, compiler, name);
    if (index >= 0) {
        return index;
    }

    if (ctx->shared_var_count >= ctx->shared_var_capacity) {
        int new_capacity = ctx->shared_var_capacity * 2;
        XrSharedVar *new_vars = (XrSharedVar*)xr_realloc(ctx->shared_vars,
            sizeof(XrSharedVar) * new_capacity);
        if (!new_vars) {
            xr_compiler_error(ctx, compiler, "shared variable memory allocation failed");
            return 0;
        }
        for (int i = ctx->shared_var_capacity; i < new_capacity; i++) {
            new_vars[i].name = NULL;
            new_vars[i].index = -1;
            new_vars[i].is_const = false;
            new_vars[i].compile_type = NULL;
            new_vars[i].state = SHARED_STATE_OWNED;
            new_vars[i].moved_line = 0;
            new_vars[i].moved_column = 0;
        }
        ctx->shared_vars = new_vars;
        ctx->shared_var_capacity = new_capacity;
    }

    index = ctx->shared_var_count++;
    ctx->shared_vars[index].name = name;
    ctx->shared_vars[index].index = index;
    ctx->shared_vars[index].scope_depth = compiler ? compiler->scope_depth : 0;
    ctx->shared_vars[index].function_depth = get_function_depth(compiler);
    ctx->shared_vars[index].is_const = false;
    ctx->shared_vars[index].state = SHARED_STATE_OWNED;
    ctx->shared_vars[index].compile_type = NULL;
    ctx->shared_vars[index].moved_line = 0;
    ctx->shared_vars[index].moved_column = 0;

    return index;
}

void shared_set_const(XrCompilerContext *ctx, int index, bool is_const) {
    if (index >= 0 && index < ctx->shared_var_count) {
        ctx->shared_vars[index].is_const = is_const;
    }
}

bool shared_is_const(XrCompilerContext *ctx, int index) {
    if (index >= 0 && index < ctx->shared_var_count) {
        return ctx->shared_vars[index].is_const;
    }
    return false;
}

void shared_set_type(XrCompilerContext *ctx, int index, XrType *type) {
    if (index >= 0 && index < ctx->shared_var_count) {
        ctx->shared_vars[index].compile_type = type;
    }
}

XrType *shared_get_type(XrCompilerContext *ctx, int index) {
    if (index >= 0 && index < ctx->shared_var_count) {
        return ctx->shared_vars[index].compile_type;
    }
    return NULL;
}

// Move semantics: mark shared let variable as moved
void shared_set_moved(XrCompilerContext *ctx, int index, int line, int column) {
    if (index >= 0 && index < ctx->shared_var_count) {
        ctx->shared_vars[index].state = SHARED_STATE_MOVED;
        ctx->shared_vars[index].moved_line = line;
        ctx->shared_vars[index].moved_column = column;
    }
}

// Check if shared let variable has been moved
bool shared_is_moved(XrCompilerContext *ctx, int index) {
    if (index >= 0 && index < ctx->shared_var_count) {
        return ctx->shared_vars[index].state == SHARED_STATE_MOVED;
    }
    return false;
}

// Get location where variable was moved
void shared_get_moved_location(XrCompilerContext *ctx, int index, int *line, int *column) {
    if (index >= 0 && index < ctx->shared_var_count) {
        if (line) *line = ctx->shared_vars[index].moved_line;
        if (column) *column = ctx->shared_vars[index].moved_column;
    }
}

// Reset state to OWNED on reassignment
void shared_reset_state(XrCompilerContext *ctx, int index) {
    if (index >= 0 && index < ctx->shared_var_count) {
        ctx->shared_vars[index].state = SHARED_STATE_OWNED;
        ctx->shared_vars[index].moved_line = 0;
        ctx->shared_vars[index].moved_column = 0;
    }
}

// Compile AST to function prototype
XrProto *xr_compile(XrCompilerContext *ctx, AstNode *ast) {
    XR_DCHECK(ctx != NULL, "xr_compile: NULL context");
    XR_DCHECK(ast != NULL, "xr_compile: NULL ast");
    // Save initial global variable offset (for module compilation)
    int initial_global_offset = ctx->global_var_count;

    for (int i = 0; i < MAX_GLOBALS; i++) {
        ctx->global_vars[i].name = NULL;
        ctx->global_vars[i].index = -1;
    }

    ctx->global_var_count = initial_global_offset;

    // Type inference pass.
    if (ctx->analyzer) {
        xa_analyzer_analyze(ctx->analyzer, NULL, ast);

        // Report all analyzer diagnostics (errors halt compilation, warnings are informational)
        int diag_count = 0;
        XaDiagnostic *diagnostics = xa_analyzer_get_diagnostics(ctx->analyzer, &diag_count);
        if (diag_count > 0) {
            int error_count = 0;
            int warning_count = 0;
            for (XaDiagnostic *d = diagnostics; d; d = d->next) {
                if (d->code == 0) continue;
                const char *file = d->location.file ? d->location.file : ctx->source_file;
                int col = d->location.column > 0 ? d->location.column : 1;
                if (d->severity == XR_DIAG_SEV_ERROR) {
                    error_count++;
                    xr_diag_print(XR_DIAG_ERROR, d->code, d->message,
                                  file, d->location.line, col, 0, NULL, NULL);
                    d->reported = true;
                } else if (d->severity == XR_DIAG_SEV_WARNING) {
                    warning_count++;
                    xr_diag_print(XR_DIAG_WARNING, d->code, d->message,
                                  file, d->location.line, col, 0, NULL, NULL);
                    d->reported = true;
                }
            }
            if (error_count > 0) {
                xr_diag_print_summary(ctx->source_file, error_count, warning_count, 0);
                return NULL;
            }
        }
    }

    // Monomorphization pass: clone generic functions/structs for each concrete type combination
    xa_mono_pass(ast);

    // Post-mono: re-analyze monomorphized declarations to compute struct layouts
    if (ctx->analyzer) {
        xa_analyzer_analyze(ctx->analyzer, NULL, ast);
    }

    // Mark all pre-escape diagnostics as reported so the post-escape scan
    // only picks up entries freshly emitted by the escape pass. (Post-mono
    // re-analysis can produce benign duplicates we do not want to re-print.)
    if (ctx->analyzer) {
        int pre_diag_count = 0;
        XaDiagnostic *pre = xa_analyzer_get_diagnostics(ctx->analyzer, &pre_diag_count);
        for (XaDiagnostic *d = pre; d; d = d->next) {
            d->reported = true;
        }
    }

    // Escape analysis: enforce explicit sharing rules for go closure
    // captures and `move` arguments. Emits diagnostics via the analyzer.
    xa_escape_analyze(ast, ctx->analyzer);

    // Re-check analyzer diagnostics after escape pass (errors abort compile)
    if (ctx->analyzer) {
        int post_diag_count = 0;
        XaDiagnostic *post_diagnostics =
            xa_analyzer_get_diagnostics(ctx->analyzer, &post_diag_count);
        int post_error_count = 0;
        int post_warning_count = 0;
        for (XaDiagnostic *d = post_diagnostics; d; d = d->next) {
            if (d->code == 0) continue;
            // Only print diagnostics that were added by the escape pass.
            // Pre-existing ones were already printed above.
            if (d->reported) continue;
            const char *file = d->location.file ? d->location.file : ctx->source_file;
            int col = d->location.column > 0 ? d->location.column : 1;
            if (d->severity == XR_DIAG_SEV_ERROR) {
                post_error_count++;
                xr_diag_print(XR_DIAG_ERROR, d->code, d->message,
                              file, d->location.line, col, 0, NULL, NULL);
                d->reported = true;
            } else if (d->severity == XR_DIAG_SEV_WARNING) {
                post_warning_count++;
                xr_diag_print(XR_DIAG_WARNING, d->code, d->message,
                              file, d->location.line, col, 0, NULL, NULL);
                d->reported = true;
            }
        }
        if (post_error_count > 0) {
            xr_diag_print_summary(ctx->source_file, post_error_count,
                                  post_warning_count, 0);
            return NULL;
        }
    }

    // Code generation
    XrCompiler compiler;
    xr_compiler_init(ctx, &compiler, FUNCTION_SCRIPT);

    compiler.globals = ctx->global_vars;
    compiler.global_count = &ctx->global_var_count;

    xr_compile_statement(ctx, &compiler, ast);

    compiler.proto->num_globals = ctx->global_var_count;

    // Propagate nested compilation errors to current compiler
    if (ctx->had_error) {
        compiler.had_error = true;
    }

    XrProto *proto = xr_compiler_end(ctx, &compiler);

    return proto;
}

/* ========== OOP Compilation Support ========== */

