/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xstmt_coroutine.c - Coroutine statement compiler
 *
 * KEY CONCEPT:
 *   Compiles coroutine-related statements: go, await, Channel, select, defer, scope.
 */

#include "xstmt.h"
#include "../../base/xchecks.h"
#include "xcompiler.h"
#include "xcompiler_context.h"
#include "xemit.h"
#include "xregalloc.h"
#include "../../runtime/value/xchunk.h"
#include "../parser/xast.h"
#include "xexpr.h"
#include "../../runtime/symbol/xsymbol_table.h"
#include "../../runtime/xisolate_api.h"
#include "../../runtime/value/xtype.h"
#include "../parser/xast_nodes.h"
#include "xconst_fold.h"
#include <stdio.h>
#include <string.h>

/* ========== Forward Declarations ========== */

static void compile_select_nonblocking(XrCompilerContext *ctx, XrCompiler *c,
                                       SelectStmtNode *node, int default_case,
                                       int *end_jumps, int *end_jump_count);

static void compile_select_blocking(XrCompilerContext *ctx, XrCompiler *c,
                                    SelectStmtNode *node, int timeout_case,
                                    int channel_case_count,
                                    int *end_jumps, int *end_jump_count);

/* ========== Coroutine Closure Upvalue Check ========== */

/*
 * Check if coroutine closure upvalues are legal.
 *
 * New rule: coroutine closures can ONLY capture shared const variables.
 *   - shared const: allowed (immutable, safe for concurrent read)
 *   - shared let: forbidden (mutable, must use Move semantics via parameters)
 *   - normal variables: forbidden (must pass through parameters for deep copy)
 *
 * @return true if all upvalues are legal, false if there are illegal upvalues
 */
static bool check_coro_closure_upvalues(XrCompilerContext *ctx, XrCompiler *c,
                                         XrCompiler *closure_compiler) {
    int upvalue_count = PROTO_UPVAL_COUNT(closure_compiler->proto);
    bool has_error = false;

    for (int i = 0; i < upvalue_count; i++) {
        XrUpvalueDesc *uv = &closure_compiler->upvalues[i];
        const char *var_name = uv->name ? XR_STRING_CHARS(uv->name) : "?";

        // shared const - safe for concurrent read (global heap + refcount)
        if (uv->storage_mode == XR_STORAGE_SHARED && uv->is_const) {
            continue;
        }

        // const (non-shared) - safe: immutable; child borrows parent's coro heap
        // via shared closure pointer (parent outlives children through scope).
        if (uv->is_const) {
            continue;
        }

        // shared let - forbidden (move semantics only; pass via argument)
        if (uv->storage_mode == XR_STORAGE_SHARED && !uv->is_const) {
            xr_compiler_error(ctx, c,
                "go closure cannot capture mutable 'shared let' variable '%s'\n"
                "hint: pass it through an argument with 'move': go worker(move %s)",
                var_name, var_name);
            has_error = true;
            continue;
        }

        // Normal mutable variables - forbidden (no implicit promotion)
        xr_compiler_error(ctx, c,
            "go closure cannot capture mutable variable '%s'\n"
            "hint: use one of the following:\n"
            "  1. pass through argument: go worker(%s)  // deep-copied\n"
            "  2. declare as 'shared const %s = ...' for concurrent reads",
            var_name, var_name, var_name);
        has_error = true;
    }

    return !has_error;
}

/* ========== Closure Coroutine Safety Check ========== */

/*
 * Check if closure in go argument is coroutine-safe.
 *
 * When a function is passed as argument to go, need to check if the function
 * captured non-shared variables or Channel. If captured normal variables,
 * the function cannot execute in coroutine.
 *
 * @param arg Parameter AST node
 * @return true if safe, false if unsafe
 */
static bool check_go_arg_closure_safety(XrCompilerContext *ctx, XrCompiler *c, AstNode *arg) {
    if (arg == NULL) return true;

    // If argument is variable reference, check if the corresponding closure is safe
    if (arg->type == AST_VARIABLE) {
        VariableNode *var = &arg->as.variable;
        XrLocalInfo *local = compiler_get_local_by_name(c, var->name);

        if (local && local->is_closure) {
            // Check if closure's Proto is marked as coroutine-safe
            XrProto *proto = local->closure_proto;
            if (proto && !proto->is_coro_safe) {
                // Find the unsafe variable names captured
                char unsafe_vars[256] = "";
                int len = 0;
                int upvalue_count = PROTO_UPVAL_COUNT(proto);
                for (int i = 0; i < upvalue_count && len < 200; i++) {
                    UpvalInfo uv = PROTO_UPVALUE(proto, i);
                    if (uv.storage_mode == 0) {  // Non-shared
                        // Find variable name from enclosing compiler
                        XrCompiler *enc = c;
                        while (enc) {
                            for (int j = 0; j < enc->local_count; j++) {
                                XrLocalInfo *l = compiler_get_local_at(enc, j);
                                if (l && l->reg == uv.index && uv.source == UPVAL_SRC_REG) {
                                    if (len > 0) len += snprintf(unsafe_vars + len, 256 - len, ", ");
                                    len += snprintf(unsafe_vars + len, 256 - len, "'%s'",
                                                   l->name ? l->name->data : "?");
                                    goto found;
                                }
                            }
                            enc = enc->enclosing;
                        }
                        found:;
                    }
                }

                xr_compiler_error(ctx, c,
                    "Function '%s' cannot be passed to coroutine (captured non-thread-safe variable %s)\n"
                    "hint: Declare %s as shared, or pass through parameter",
                    var->name,
                    len > 0 ? unsafe_vars : "?",
                    len > 0 ? unsafe_vars : "the variable");
                return false;
            }
        }
    }

    return true;
}

/* ========== go Expression Compilation ========== */

/*
 * Old design: check_go_args_const checked parameters must be const
 * New design: runtime unified deep copy, no compile-time restriction
 *
 * Removed: go parameter const restriction
 * Reason: const variables also have lifetime issues, unified deep copy is safer
 */

/*
 * Emit coroutine name NOP (if name present).
 *
 * Format: OP_NOP A Bx
 *   A = 1 means this is coroutine name NOP
 *   Bx = name string constant index
 */
static void emit_coro_name_if_present(XrCompilerContext *ctx, XrCompiler *c, const char *name) {
    if (name == NULL) return;

    // Add name to constant table
    XrString *name_str = xr_compile_time_intern(ctx->X, name, (int)strlen(name));
    int name_idx = xr_vm_proto_add_constant(c->proto, xr_string_value(name_str));

    // Emit name-carrying NOP: A=1 means coroutine name, Bx=constant index
    emit_abx(c->emitter, OP_NOP, 1, name_idx);
}

/*
 * Emit coroutine priority setting (if priority present).
 *
 * Format: OP_NOP A Bx
 *   A = 2 means this is coroutine priority NOP
 *   Bx = priority value (0=LOW, 1=NORMAL, 2=HIGH)
 *
 * Priority expression support:
 *   - Coro.LOW / Coro.NORMAL / Coro.HIGH (compile-time constants)
 *   - Numeric literals 0/1/2
 */
static void emit_coro_priority_if_present(XrCompilerContext *ctx, XrCompiler *c, AstNode *priority) {
    if (priority == NULL) return;

    int prio_val = 1;  // Default NORMAL

    // Try compile-time evaluation
    if (priority->type == AST_LITERAL_INT) {
        // Numeric literal
        prio_val = (int)priority->as.literal.raw_value.int_val;
        if (prio_val < 0) prio_val = 0;
        if (prio_val > 2) prio_val = 2;
    } else if (priority->type == AST_MEMBER_ACCESS) {
        // Coro.LOW / Coro.NORMAL / Coro.HIGH
        MemberAccessNode *member = &priority->as.member_access;
        if (member->object->type == AST_VARIABLE &&
            strcmp(member->object->as.variable.name, "Coro") == 0) {
            if (strcmp(member->name, "LOW") == 0) {
                prio_val = 0;
            } else if (strcmp(member->name, "NORMAL") == 0) {
                prio_val = 1;
            } else if (strcmp(member->name, "HIGH") == 0) {
                prio_val = 2;
            } else {
                xr_compiler_error(ctx, c,
                    "Invalid priority constant 'Coro.%s', expected Coro.LOW / Coro.NORMAL / Coro.HIGH",
                    member->name);
                return;
            }
        } else {
            xr_compiler_error(ctx, c,
                "Priority must be Coro.LOW / Coro.NORMAL / Coro.HIGH or number 0/1/2");
            return;
        }
    } else {
        xr_compiler_error(ctx, c,
            "Priority must be Coro.LOW / Coro.NORMAL / Coro.HIGH or number 0/1/2");
        return;
    }

    // Emit priority-carrying NOP: A=2 means coroutine priority, Bx=priority value
    emit_abx(c->emitter, OP_NOP, 2, prio_val);
}

/*
 * Emit link mode NOP (if link_mode != XR_LINK_NONE).
 *
 * Format: OP_NOP A Bx
 *   A = 3 means this is link mode NOP
 *   Bx = link mode (1=LINKED, 2=MONITORED)
 */
static void emit_link_mode_if_present(XrCompiler *c, uint8_t link_mode) {
    if (link_mode == XR_LINK_NONE) return;
    emit_abx(c->emitter, OP_NOP, 3, link_mode);
}

/*
 * Compile go expression.
 * go fn() or go fn or go { block } or go(name: "xxx") fn()
 *
 * go supports four forms:
 * 1. go fn()           - Start coroutine to execute function call
 * 2. go fn             - Start coroutine to execute closure (no arguments)
 * 3. go { ... }        - Anonymous block coroutine
 * 4. go(name: "xxx")   - Named coroutine (can combine with above)
 *
 * Generated instructions:
 *   OP_GO A B C         - A=result, B=closure, C=argument count
 *   [OP_NOP 1 Bx]       - Optional, Bx=name constant index
 */
void compile_go_expr(XrCompilerContext *ctx, XrCompiler *c, GoExprNode *node, int target, bool fire_and_forget) {
    XR_DCHECK(ctx != NULL, "compile_go_expr: NULL ctx");
    XR_DCHECK(c != NULL, "compile_go_expr: NULL compiler");
    XR_DCHECK(node != NULL, "compile_go_expr: NULL node");
    AstNode *expr = node->expr;

    if (expr->type == AST_CALL_EXPR) {
        // go fn() or go obj.method() - function/method call form
        CallExprNode *call = &expr->as.call_expr;

        // New design: runtime unified deep copy, no compile-time const check needed

        // Check if it's a method call
        if (call->callee->type == AST_MEMBER_ACCESS) {
            /* go obj.method(args) - method call coroutine
             *
             * Register layout (consistent with OP_INVOKE):
             *   R[base]   = return value position
             *   R[base+1] = this (receiver)
             *   R[base+2] = arg1
             *   R[base+3] = arg2
             *   ...
             */
            MemberAccessNode *member = &call->callee->as.member_access;
            int arg_count = call->arg_count;

            // Allocate base register (use reg_alloc to ensure correct allocation)
            int base = reg_alloc(ctx, c);

            // Protect base to base + 1 + arg_count region, prevent temporary register overwrite
            int protect_id = xreg_protect_begin(c->regalloc, base, 2 + arg_count, "go_invoke");

            /* Compile this to R[base+1]
             * Key: first set freereg to base+2, ensure this compilation's temporary registers don't overwrite base */
            xreg_set_freereg(c->regalloc, base + 2);
            XrExprDesc obj_desc = xr_compile_expr(ctx, c, member->object);
            xexpr_to_specific_reg(ctx, c, &obj_desc, base + 1);

            /* Compile arguments to R[base+2], R[base+3], ...
             * Key: update freereg to target position +1 before each argument compilation */
            for (int i = 0; i < arg_count; i++) {
                int arg_reg = base + 2 + i;
                xreg_set_freereg(c->regalloc, arg_reg + 1);
                XrExprDesc arg_desc = xr_compile_expr(ctx, c, call->arguments[i]);
                xexpr_to_specific_reg(ctx, c, &arg_desc, arg_reg);
            }

            // End protection
            xreg_protect_end(c->regalloc, protect_id);

            // Get method symbol (local index via per-function symbol table)
            XrSymbolTable *symtab = (XrSymbolTable*)xr_isolate_get_symbol_table(ctx->X);
            int global_sym = xr_symbol_register_in_table(symtab, member->name);
            int local_sym = emitter_add_symbol(c->emitter, global_sym);

            // Emit OP_GO_INVOKE base method_symbol arg_count
            emit_abc(c->emitter, OP_GO_INVOKE, base, local_sym, arg_count);

            // Emit coroutine name (if present)
            emit_coro_name_if_present(ctx, c, node->name);

            // Emit link mode (if present)
            emit_link_mode_if_present(c, node->link_mode);

            // Reset freereg
            xreg_set_freereg(c->regalloc, base + 1);

            // Result in R[base], need to move to target
            if (target != base) {
                emit_abc(c->emitter, OP_MOVE, target, base, 0);
            }
            return;
        }

        // go fn() - normal function call

        // Compile-time callee closure safety check
        if (call->callee && call->callee->type == AST_VARIABLE) {
            const char *callee_name = call->callee->as.variable.name;
            XrLocalInfo *callee_local = compiler_get_local_by_name(c, callee_name);
            if (callee_local && callee_local->closure_proto) {
                XrProto *callee_proto = callee_local->closure_proto;
                if (!callee_proto->is_coro_safe) {
                    xr_compiler_error(ctx, c,
                        "go: function '%s' captures non-thread-safe variables, cannot run in coroutine\n"
                        "hint: use 'shared const' to declare shared variables, or pass via arguments",
                        callee_name);
                }
            }
        }

        // Compile callee (function/closure)
        int base_reg = xreg_get_freereg(c->regalloc);
        XrExprDesc callee_desc = xr_compile_expr(ctx, c, call->callee);
        xexpr_to_specific_reg(ctx, c, &callee_desc, base_reg);

        // Key: update freereg to prevent argument compilation from overwriting closure register
        xreg_set_freereg(c->regalloc, base_reg + 1);

        // Compile arguments to consecutive registers
        // Track moved shared let indices for runtime nullification
        int moved_shared[8];
        int moved_count = 0;

        int arg_count = call->arg_count;
        for (int i = 0; i < arg_count; i++) {
            int arg_reg = base_reg + 1 + i;
            AstNode *arg = call->arguments[i];

            // Check argument closure coroutine safety
            check_go_arg_closure_safety(ctx, c, arg);

            // Compile argument first, then handle move marking
            XrExprDesc arg_desc = xr_compile_expr(ctx, c, arg);
            xexpr_to_specific_reg(ctx, c, &arg_desc, arg_reg);

            // Explicit move semantics: only 'move var' triggers ownership transfer
            // Must be after compile so the variable is still accessible during codegen
            if (arg && arg->type == AST_MOVE_EXPR) {
                AstNode *inner = arg->as.move_expr.expr;
                if (inner && inner->type == AST_VARIABLE) {
                    const char *var_name = inner->as.variable.name;
                    XrString *name_str = xr_compile_time_intern(ctx->X, var_name, strlen(var_name));
                    int shared_idx = shared_get_in_scope(ctx, c, name_str);
                    if (shared_idx >= 0 && !shared_is_const(ctx, shared_idx)) {
                        shared_set_moved(ctx, shared_idx, inner->line, inner->column);
                        if (moved_count < 8) {
                            moved_shared[moved_count++] = shared_idx;
                        }
                    }
                }
            }
        }

        // Update freereg
        xreg_set_freereg(c->regalloc, base_reg + 1 + arg_count);

        // All go statements use OP_SPAWN_CONT for continuation stealing.
        // DFS execution: child runs first, parent pushed to cont_deque.
        // C bit 7: fire-and-forget flag (result never awaited, safe to recycle)
        int spawn_c = arg_count | (fire_and_forget ? 0x80 : 0);
        emit_abc(c->emitter, OP_SPAWN_CONT, target, base_reg, spawn_c);

        // Emit coroutine name (if present)
        emit_coro_name_if_present(ctx, c, node->name);

        // Emit coroutine priority (if present)
        emit_coro_priority_if_present(ctx, c, node->priority);

        // Emit link mode (if present)
        emit_link_mode_if_present(c, node->link_mode);

        // Runtime nullification: set moved shared let variables to null
        // Must be after OP_SPAWN_CONT so child coroutine reads args first
        if (moved_count > 0) {
            int tmp_reg = reg_alloc(ctx, c);
            for (int i = 0; i < moved_count; i++) {
                emit_abx(c->emitter, OP_LOADNULL, tmp_reg, 0);
                emit_abx(c->emitter, OP_SETSHARED, tmp_reg, moved_shared[i]);
            }
            reg_free(c, tmp_reg);
        }

    } else if (expr->type == AST_FUNCTION_DECL || expr->type == AST_FUNCTION_EXPR) {
        /* go { block } or go fn => ... - anonymous closure form
         * Inline compile closure to check upvalues
         */
        FunctionDeclNode *func_expr = (expr->type == AST_FUNCTION_EXPR)
            ? &expr->as.function_expr
            : &expr->as.function_decl;

        // Create new compiler (nested)
        XrCompiler closure_compiler;
        xr_compiler_init(ctx, &closure_compiler, FUNCTION_FUNCTION);
        closure_compiler.enclosing = c;
        closure_compiler.proto->numparams = func_expr->param_count;

        // Enter function scope
        scope_begin(&closure_compiler);

        // Pre-scan before params: mark captured params before codegen
        prescan_fn_body(&closure_compiler, func_expr, func_expr->body);

        // Define parameters as local variables
        for (int i = 0; i < func_expr->param_count; i++) {
            XrParamNode *param = func_expr->params[i];
            if (!param) continue;
            XrString *param_str = xr_compile_time_intern(ctx->X, param->name, strlen(param->name));
            scope_define_local_reg(ctx, &closure_compiler, param_str, i);
        }

        // Set freereg
        if (closure_compiler.regalloc) {
            xreg_set_freereg(closure_compiler.regalloc,
                            xreg_get_local_end(closure_compiler.regalloc));
        }

        // Compile function body
        xr_compile_statement(ctx, &closure_compiler, func_expr->body);

        // Check coroutine closure upvalues (compile-time mandatory check)
        // New rule: only shared const can be captured, shared let is forbidden
        bool upvalue_ok = check_coro_closure_upvalues(ctx, c, &closure_compiler);

        // End compilation
        XrProto *proto = xr_compiler_end(ctx, &closure_compiler);

        // If upvalue check failed, mark error and return early
        if (!upvalue_ok) {
            c->had_error = true;
            return;
        }

        int fn_reg = reg_alloc(ctx, c);
        if (proto != NULL) {
            int proto_idx = xr_vm_proto_add_proto(c->proto, proto);
            emit_ctx_sync_before_closure(ctx, c);
            emit_abx(c->emitter, OP_CLOSURE, fn_reg, proto_idx);
        }

        // All go statements use OP_SPAWN_CONT for continuation stealing.
        emit_abc(c->emitter, OP_SPAWN_CONT, target, fn_reg, fire_and_forget ? 0x80 : 0);

        // Emit coroutine name (if present)
        emit_coro_name_if_present(ctx, c, node->name);

        // Emit coroutine priority (if present)
        emit_coro_priority_if_present(ctx, c, node->priority);

        // Emit link mode (if present)
        emit_link_mode_if_present(c, node->link_mode);

    } else {
        // go fn - existing closure (no-argument call)
        XrExprDesc expr_desc = xr_compile_expr(ctx, c, expr);
        int fn_reg = xexpr_to_anyreg(ctx, c, &expr_desc);

        // All go statements use OP_SPAWN_CONT for continuation stealing.
        emit_abc(c->emitter, OP_SPAWN_CONT, target, fn_reg, fire_and_forget ? 0x80 : 0);

        // Emit coroutine name (if present)
        emit_coro_name_if_present(ctx, c, node->name);

        // Emit coroutine priority (if present)
        emit_coro_priority_if_present(ctx, c, node->priority);

        // Emit link mode (if present)
        emit_link_mode_if_present(c, node->link_mode);
    }
}


/* ========== await Expression Compilation ========== */

/*
 * Compile await expression.
 * await task              - Wait for single coroutine (OP_AWAIT)
 * await [t1, t2]          - Wait for all coroutines (OP_AWAIT_ALL)
 * await.any [t1,t2]       - Wait for any coroutine to complete (OP_AWAIT_ANY)
 * await.anySuccess [t1,t2] - Wait for any coroutine to succeed (OP_AWAIT_ANY_SUCCESS)
 *
 * Generated instructions:
 *   OP_AWAIT A B             - R[A] = await R[B]
 *   OP_AWAIT_TIMEOUT A B C   - R[A] = await(timeout: R[C]) R[B]
 *   OP_AWAIT_ALL A B         - R[A] = await.all R[B]:Array
 *   OP_AWAIT_ANY A B         - R[A] = await.any R[B]:Array
 *   OP_AWAIT_ANY_SUCCESS A B - R[A] = await.anySuccess R[B]:Array
 */
void compile_await_expr(XrCompilerContext *ctx, XrCompiler *c, AwaitExprNode *node, int target) {
    XR_DCHECK(ctx != NULL, "compile_await_expr: NULL ctx");
    XR_DCHECK(c != NULL, "compile_await_expr: NULL compiler");
    XR_DCHECK(node != NULL, "compile_await_expr: NULL node");
    // Compile expression to wait for
    XrExprDesc expr_desc = xr_compile_expr(ctx, c, node->expr);
    int coro_reg = xexpr_to_anyreg(ctx, c, &expr_desc);

    // Compile timeout parameter (if present)
    int timeout_reg = 0;
    if (node->timeout != NULL) {
        XrExprDesc timeout_desc = xr_compile_expr(ctx, c, node->timeout);
        timeout_reg = xexpr_to_anyreg(ctx, c, &timeout_desc);
    }

    if (node->is_any_success) {
        // await.anySuccess [tasks] - C=1 means success-only mode
        emit_abc(c->emitter, OP_AWAIT_ANY, target, coro_reg, 1);
    } else if (node->is_any) {
        // await.any [tasks] - C=0 means any completion
        emit_abc(c->emitter, OP_AWAIT_ANY, target, coro_reg, 0);
    } else if (node->is_all || node->expr->type == AST_ARRAY_LITERAL) {
        // await.all tasks or await [tasks] - wait for all
        emit_abc(c->emitter, OP_AWAIT_ALL, target, coro_reg, 0);
    } else if (node->timeout != NULL) {
        // await(timeout: N) task
        emit_abc(c->emitter, OP_AWAIT_TIMEOUT, target, coro_reg, timeout_reg);
    } else {
        // await task - wait for single
        emit_abc(c->emitter, OP_AWAIT, target, coro_reg, 0);
    }
}

/* ========== Channel Creation Compilation ========== */

/*
 * Compile Channel creation.
 * Channel() or Channel(10) or Channel(N)
 * Compile-time constant → OP_CHAN_NEW (18-bit immediate)
 * Runtime expression    → OP_CHAN_NEW_NAMED with null name (size from register)
 */
// Channel buffer size limit (18-bit immediate for compile-time path)
#define XR_CHANNEL_MAX_BUFFER_SIZE 262143

void compile_channel_new(XrCompilerContext *ctx, XrCompiler *c, ChannelNewNode *node, int target) {
    XR_DCHECK(ctx != NULL, "compile_channel_new: NULL ctx");
    XR_DCHECK(c != NULL, "compile_channel_new: NULL compiler");
    XR_DCHECK(node != NULL, "compile_channel_new: NULL node");

    if (node->buffer_size == NULL) {
        // Channel() - unbuffered
        emit_abx(c->emitter, OP_CHAN_NEW, target, 0);
        return;
    }

    // Try compile-time evaluation (supports literals and const variables)
    XrConstEvalResult result = xr_const_eval_with_ctx(ctx, node->buffer_size);

    if (result.success && XR_IS_INT(result.value)) {
        // Compile-time constant: use OP_CHAN_NEW with immediate
        int buffer_size = (int)XR_TO_INT(result.value);
        if (buffer_size < 0) {
            xr_compiler_error(ctx, c, "Channel buffer size cannot be negative");
            return;
        }
        if (buffer_size > XR_CHANNEL_MAX_BUFFER_SIZE) {
            xr_compiler_error(ctx, c,
                "Channel buffer size cannot exceed %d (current value: %d)",
                XR_CHANNEL_MAX_BUFFER_SIZE, buffer_size);
            return;
        }
        emit_abx(c->emitter, OP_CHAN_NEW, target, buffer_size);
    } else {
        // Runtime expression: compile size to register, use OP_CHAN_NEW_NAMED
        // with null name to create anonymous channel with dynamic size
        XrExprDesc size_desc = xr_compile_expr(ctx, c, node->buffer_size);
        int size_reg = xexpr_to_anyreg(ctx, c, &size_desc);
        int null_reg = reg_alloc(ctx, c);
        emit_abx(c->emitter, OP_LOADNULL, null_reg, 0);
        emit_abc(c->emitter, OP_CHAN_NEW_NAMED, target, size_reg, null_reg);
        reg_free(c, null_reg);
        reg_free(c, size_reg);
    }
}

/* ========== defer Statement Compilation ========== */

/*
 * Compile defer statement.
 * defer fn()        - Defer execution of no-argument function call
 * defer fn(a, b)    - Defer execution of function call with arguments
 * defer fn          - Defer execution of closure (no arguments)
 *
 * defer defers closure and arguments execution until function returns.
 * Multiple defers execute in LIFO (last-in-first-out) order.
 *
 * Generated instruction: OP_DEFER A B
 *   A = closure register
 *   B = argument count (arguments in A+1..A+B)
 */
void compile_defer_stmt(XrCompilerContext *ctx, XrCompiler *c, DeferStmtNode *node) {
    XR_DCHECK(ctx != NULL, "compile_defer_stmt: NULL ctx");
    XR_DCHECK(c != NULL, "compile_defer_stmt: NULL compiler");
    XR_DCHECK(node != NULL, "compile_defer_stmt: NULL node");
    AstNode *expr = node->expr;
    int base_reg;
    int arg_count = 0;

    if (expr->type == AST_CALL_EXPR) {
        // defer fn(args) - compile function and arguments
        CallExprNode *call = &expr->as.call_expr;
        arg_count = call->arg_count;

        // Allocate consecutive registers: R[base]=closure, R[base+1..base+n]=arguments
        base_reg = xreg_get_freereg(c->regalloc);

        // Protect base to base + arg_count region, prevent temporary register overwrite
        int protect_id = xreg_protect_begin(c->regalloc, base_reg, 1 + arg_count, "defer_call");

        /* Compile callee to base
         * Key: first set freereg to base+1, ensure callee compilation's temporary registers don't overwrite base */
        xreg_set_freereg(c->regalloc, base_reg + 1);
        XrExprDesc callee_desc = xr_compile_expr(ctx, c, call->callee);
        xexpr_to_specific_reg(ctx, c, &callee_desc, base_reg);

        /* Compile arguments to base+1..base+n
         * Key: update freereg to target position +1 before each argument compilation */
        for (int i = 0; i < arg_count; i++) {
            int arg_reg = base_reg + 1 + i;
            xreg_set_freereg(c->regalloc, arg_reg + 1);
            XrExprDesc arg_desc = xr_compile_expr(ctx, c, call->arguments[i]);
            xexpr_to_specific_reg(ctx, c, &arg_desc, arg_reg);
        }

        // End protection
        xreg_protect_end(c->regalloc, protect_id);

        // Update freereg
        xreg_set_freereg(c->regalloc, base_reg + 1 + arg_count);
    } else if (expr->type == AST_FUNCTION_DECL || expr->type == AST_FUNCTION_EXPR) {
        // defer { block } - anonymous closure form
        XrExprDesc expr_desc = xr_compile_expr(ctx, c, expr);
        base_reg = xexpr_to_anyreg(ctx, c, &expr_desc);
    } else {
        // defer fn - directly use closure (no arguments)
        XrExprDesc expr_desc = xr_compile_expr(ctx, c, expr);
        base_reg = xexpr_to_anyreg(ctx, c, &expr_desc);
    }

    // Emit OP_DEFER A B
    emit_abc(c->emitter, OP_DEFER, base_reg, arg_count, 0);
}

/* ========== select Statement Compilation ========== */

/*
 * Compile select statement (event-driven version - true blocking).
 * select { msg from ch => body, after N => timeout_body, _ => default_body }
 *
 * Implementation strategy:
 * 1. Has default: non-blocking, immediately check all cases, execute default if none ready
 * 2. No default: use OP_SELECT_WAIT for true blocking, wait for any case to be ready
 */
void compile_select_stmt(XrCompilerContext *ctx, XrCompiler *c, SelectStmtNode *node) {
    XR_DCHECK(ctx != NULL, "compile_select_stmt: NULL ctx");
    XR_DCHECK(c != NULL, "compile_select_stmt: NULL compiler");
    XR_DCHECK(node != NULL, "compile_select_stmt: NULL node");
    int case_count = node->case_count;
    if (case_count == 0) return;

    int end_jumps[64];
    int end_jump_count = 0;
    int default_case = -1;
    int timeout_case = -1;

    // Find default case and timeout case
    for (int i = 0; i < case_count; i++) {
        SelectCaseNode *sc = &node->cases[i]->as.select_case;
        if (sc->is_default) {
            default_case = i;
        }
        if (sc->is_timeout) {
            timeout_case = i;
        }
    }

    // Calculate actual channel case count (excluding default)
    int channel_case_count = 0;
    for (int i = 0; i < case_count; i++) {
        SelectCaseNode *sc = &node->cases[i]->as.select_case;
        if (!sc->is_default) channel_case_count++;
    }

    // Use non-blocking mode when has default
    if (default_case >= 0) {
        // Non-blocking version: try each case sequentially
        compile_select_nonblocking(ctx, c, node, default_case, end_jumps, &end_jump_count);
    } else {
        // No default: use event-driven blocking wait
        compile_select_blocking(ctx, c, node, timeout_case, channel_case_count,
                               end_jumps, &end_jump_count);
    }

    // Patch all end jumps
    for (int i = 0; i < end_jump_count; i++) {
        patch_jump(c->emitter, end_jumps[i], -1);
    }
}

/*
 * Non-blocking version select compilation (has default)
 */
static void compile_select_nonblocking(XrCompilerContext *ctx, XrCompiler *c,
                                       SelectStmtNode *node, int default_case,
                                       int *end_jumps, int *end_jump_count) {
    int case_count = node->case_count;

    // Compile each non-default case
    for (int i = 0; i < case_count; i++) {
        SelectCaseNode *sc = &node->cases[i]->as.select_case;
        if (sc->is_default) continue;

        // Compile channel expression
        XrExprDesc ch_desc = xr_compile_expr(ctx, c, sc->channel);
        int ch_reg = xexpr_to_anyreg(ctx, c, &ch_desc);

        int result_reg = xreg_alloc_temp(c->regalloc);
        xreg_alloc_temp(c->regalloc);  // Reserve ok_reg

        if (sc->is_send) {
            XrExprDesc val_desc = xr_compile_expr(ctx, c, sc->value);
            int val_reg = xexpr_to_anyreg(ctx, c, &val_desc);
            emit_abc(c->emitter, OP_CHAN_TRY_SEND, result_reg, ch_reg, val_reg);
            emit_abc(c->emitter, OP_TEST, result_reg, 0, 0);
        } else if (sc->is_timeout) {
            // timeout case: create timer channel and try to receive
            XrExprDesc timeout_desc = xr_compile_expr(ctx, c, sc->value);
            int timeout_reg = xexpr_to_anyreg(ctx, c, &timeout_desc);
            int timer_ch_reg = xreg_alloc_temp(c->regalloc);
            emit_abc(c->emitter, OP_TIME_AFTER, timer_ch_reg, timeout_reg, 0);
            emit_abc(c->emitter, OP_CHAN_TRY_RECV, result_reg, timer_ch_reg, 0);
            emit_abc(c->emitter, OP_TEST, result_reg + 1, 0, 0);
        } else {
            // recv case
            emit_abc(c->emitter, OP_CHAN_TRY_RECV, result_reg, ch_reg, 0);
            if (sc->var_name != NULL) {
                XrString *var_str = xr_compile_time_intern(ctx->X, sc->var_name, strlen(sc->var_name));
                int existing = scope_resolve_local(c, var_str);
                if (existing < 0) {
                    scope_define_local_reg(ctx, c, var_str, result_reg);
                } else {
                    emit_abc(c->emitter, OP_MOVE, existing, result_reg, 0);
                }
            }
            emit_abc(c->emitter, OP_TEST, result_reg + 1, 0, 0);
        }

        int skip_jump = emit_jump(c->emitter, OP_JMP);

        if (sc->body) {
            xr_compile_statement(ctx, c, sc->body);
        }

        end_jumps[(*end_jump_count)++] = emit_jump(c->emitter, OP_JMP);
        patch_jump(c->emitter, skip_jump, -1);
    }

    /* All channel cases failed → no events ready.
     * Hint-yield before default body: deduct reductions so the scheduler
     * preempts this coroutine sooner (after ~20 empty polls instead of 4000).
     *   - data ready  → case branch fires, zero overhead
     *   - no data     → reductions -= 200, yield when exhausted
     * A=200: ~20 consecutive empty polls trigger a real yield.
     * This avoids both starvation (old: 4000 polls) and context-switch
     * storms (immediate yield on every default). */
    emit_abc(c->emitter, OP_YIELD, 200, 0, 0);

    // Compile default body
    SelectCaseNode *sc = &node->cases[default_case]->as.select_case;
    if (sc->body) {
        xr_compile_statement(ctx, c, sc->body);
    }
}

/*
 * Blocking version select compilation (no default).
 *
 * Event-driven implementation:
 * 1. Put all channels in consecutive registers
 * 2. Poll check each case
 * 3. If all fail, emit OP_SELECT_BLOCK for true blocking
 * 4. After wakeup, jump back to poll and recheck
 */
static void compile_select_blocking(XrCompilerContext *ctx, XrCompiler *c,
                                    SelectStmtNode *node, int timeout_case,
                                    int channel_case_count,
                                    int *end_jumps, int *end_jump_count) {
    int case_count = node->case_count;
    (void)timeout_case;

    // Step 1: Put all channels in consecutive registers (for OP_SELECT_BLOCK)
    int ch_base_reg = xreg_alloc_temp(c->regalloc);
    int ch_count = 0;
    int *case_ch_regs = xr_malloc(case_count * sizeof(int));

    // Create timer channel (if has after)
    int timer_ch_reg = -1;
    for (int i = 0; i < case_count; i++) {
        SelectCaseNode *sc = &node->cases[i]->as.select_case;
        if (sc->is_timeout && sc->value) {
            XrExprDesc timeout_desc = xr_compile_expr(ctx, c, sc->value);
            int timeout_reg = xexpr_to_anyreg(ctx, c, &timeout_desc);
            timer_ch_reg = ch_base_reg + ch_count;
            xreg_alloc_temp(c->regalloc);  // Reserve register
            emit_abc(c->emitter, OP_TIME_AFTER, timer_ch_reg, timeout_reg, 0);
            case_ch_regs[i] = timer_ch_reg;
            ch_count++;
            break;
        }
    }

    // Compile all channel expressions to consecutive registers
    for (int i = 0; i < case_count; i++) {
        SelectCaseNode *sc = &node->cases[i]->as.select_case;
        if (sc->is_default) {
            case_ch_regs[i] = -1;
            continue;
        }
        if (sc->is_timeout) {
            continue;  // timer channel already handled
        }

        XrExprDesc ch_desc = xr_compile_expr(ctx, c, sc->channel);
        int ch_reg = ch_base_reg + ch_count;
        if (ch_count > 0) {
            xreg_alloc_temp(c->regalloc);  // Reserve register
        }
        xexpr_to_specific_reg(ctx, c, &ch_desc, ch_reg);
        case_ch_regs[i] = ch_reg;
        ch_count++;
    }

    // Poll loop start
    int loop_start = emit_get_current_pc(c->emitter);

    // Compile each case's check
    for (int i = 0; i < case_count; i++) {
        SelectCaseNode *sc = &node->cases[i]->as.select_case;
        if (sc->is_default) continue;

        int ch_reg = case_ch_regs[i];

        int result_reg = xreg_alloc_temp(c->regalloc);
        xreg_alloc_temp(c->regalloc); // ok_reg
        int ok_reg = result_reg + 1;

        if (sc->is_send) {
            XrExprDesc val_desc = xr_compile_expr(ctx, c, sc->value);
            int val_reg = xexpr_to_anyreg(ctx, c, &val_desc);
            emit_abc(c->emitter, OP_CHAN_TRY_SEND, result_reg, ch_reg, val_reg);
            emit_abc(c->emitter, OP_TEST, result_reg, 0, 0);
        } else {
            emit_abc(c->emitter, OP_CHAN_TRY_RECV, result_reg, ch_reg, 0);

            if (sc->var_name != NULL && !sc->is_timeout) {
                XrString *var_str = xr_compile_time_intern(ctx->X, sc->var_name, strlen(sc->var_name));
                int existing = scope_resolve_local(c, var_str);
                if (existing < 0) {
                    scope_define_local_reg(ctx, c, var_str, result_reg);
                } else {
                    emit_abc(c->emitter, OP_MOVE, existing, result_reg, 0);
                }
            }

            emit_abc(c->emitter, OP_TEST, ok_reg, 0, 0);
        }

        int skip_jump = emit_jump(c->emitter, OP_JMP);

        if (sc->body) {
            xr_compile_statement(ctx, c, sc->body);
        }
        end_jumps[(*end_jump_count)++] = emit_jump(c->emitter, OP_JMP);

        patch_jump(c->emitter, skip_jump, -1);
    }

    /* All failed: emit OP_SELECT_BLOCK for true blocking wait
     * A = channel array base register
     * B = channel count
     * Coroutine will be registered to all channels' wait queues, wakeup when any ready */
    emit_abc(c->emitter, OP_SELECT_BLOCK, ch_base_reg, ch_count, channel_case_count);

    // After wakeup, jump back to poll start and recheck
    int offset = loop_start - emit_get_current_pc(c->emitter) - 1;
    emit_sj(c->emitter, OP_JMP, offset);

    xr_free(case_ch_regs);
}

/* ========== scope Block Compilation ========== */

/*
 * Compile scope block.
 * scope { ... }
 *
 * Purpose:
 * 1. Create new lexical scope
 * 2. Execute internal code block
 * 3. Execute defer when scope ends
 *
 * Future extensions:
 * - Auto-wait coroutines started by internal go
 * - Cancellation propagation
 */
/*
 * compile_scope_block - compile scope { } / linked scope { } / supervisor scope { }
 *
 * target: result register for supervisor scope (receives errors[]).
 *         -1 = statement context (allocate temp for supervisor, discard for others).
 */
void compile_scope_block(XrCompilerContext *ctx, XrCompiler *c, ScopeBlockNode *node, int target) {
    XR_DCHECK(ctx != NULL, "compile_scope_block: NULL ctx");
    XR_DCHECK(c != NULL, "compile_scope_block: NULL compiler");
    if (!node || !node->body) return;

    int result_reg = 0;
    if (node->scope_mode == XR_SCOPE_SUPERVISOR) {
        // supervisor scope: allocate result register for errors[]
        result_reg = (target >= 0) ? target : reg_alloc(ctx, c);
    }

    // Emit SCOPE_ENTER instruction - start tracking child coroutines
    // A = scope_mode (0=WAIT, 1=LINKED, 2=SUPERVISOR)
    emit_abc(c->emitter, OP_SCOPE_ENTER, node->scope_mode, 0, 0);

    // Phase 5: Track scope block depth for continuation stealing.
    // go statements inside scope{} emit OP_SPAWN_CONT instead of OP_GO.
    c->scope_block_depth++;

    // Enter new scope
    scope_begin(c);

    // Compile internal code block
    xr_compile_statement(ctx, c, node->body);

    // Exit scope
    scope_end(ctx, c);

    c->scope_block_depth--;

    // Emit SCOPE_EXIT: A=scope_mode, B=result_reg (supervisor: errors[])
    emit_abc(c->emitter, OP_SCOPE_EXIT, node->scope_mode, result_reg, 0);

    if (node->scope_mode == XR_SCOPE_SUPERVISOR && target < 0) {
        reg_free(c, result_reg);
    }
}

/* ========== cancelled Expression Compilation ========== */

/*
 * Compile cancelled() expression.
 */
void compile_cancelled_expr(XrCompilerContext *ctx, XrCompiler *c, int target) {
    (void)ctx;  // Not used currently

    // Emit OP_CANCELLED instruction: R[A] = cancelled()
    emit_abc(c->emitter, OP_CANCELLED, target, 0, 0);
}

