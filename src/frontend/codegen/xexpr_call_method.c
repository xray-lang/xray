/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xexpr_call_method.c - Method-call dispatch
 *
 * IMPLEMENTATION NOTE:
 *   Implements the `obj.method(...)` lowering branch of
 *   compile_call_internal. Living in its own TU lets contributors edit
 *   StringBuilder / Channel / Map / Coro inlines without scrolling
 *   past 1000+ lines of regular-call infrastructure, and keeps the
 *   regular-call dispatcher under the 150-line guideline.
 *
 *   Includes mirror xexpr_call.c so any helper used by the inline
 *   optimisations resolves identically.
 */

#include "xexpr_call_method.h"
#include "xexpr.h"
#include "../../base/xchecks.h"
#include "xexpr_desc.h"
#include "xexpr_higher_order.h"
#include "xcompiler.h"
#include "xcompiler_context.h"
#include "xcompiler_class_registry.h"
#include "xemit.h"
#include "xregalloc.h"
#include "../parser/xast.h"
#include "../../runtime/value/xtype.h"
#include "../../runtime/value/xtype_names.h"
#include "../../runtime/xisolate_api.h"
#include "../../runtime/symbol/xsymbol_table.h"
#include "../analyzer/xanalyzer_symbol.h"
#include <stdio.h>
#include <string.h>

int xr_compile_call_method(XrCompilerContext *ctx, XrCompiler *compiler,
                           CallExprNode *node, bool is_tail) {
    XR_DCHECK(ctx != NULL, "xr_compile_call_method: NULL ctx");
    XR_DCHECK(compiler != NULL, "xr_compile_call_method: NULL compiler");
    XR_DCHECK(node != NULL, "xr_compile_call_method: NULL node");
    XR_DCHECK(node->callee != NULL,
              "xr_compile_call_method: NULL callee");
    XR_DCHECK(node->callee->type == AST_MEMBER_ACCESS,
              "xr_compile_call_method: callee is not AST_MEMBER_ACCESS "
              "(caller must check before dispatching here)");

    MemberAccessNode *member = &node->callee->as.member_access;

    // Move semantics: track shared let variable that needs runtime nullification
    // after ch.send(). Set by ch.send() detection, consumed after INVOKE emission.
    int move_null_shared_idx = -1;

    // Convert method name to Symbol ID once (O(1) hash lookup)
    XrSymbolTable *_st = (XrSymbolTable*)xr_isolate_get_symbol_table(ctx->X);
    SymbolId member_sym = xr_symbol_register_in_table(_st, member->name);

    /* Method self-recursion → LOOP_BACK (converts tail recursion to loop).
     * Detects ClassName.method() and this.method() calling the same method,
     * and emits OP_LOOP_BACK instead of INVOKE_TAIL for ~5-10x speedup.
     */
    if (is_tail && compiler->type == FUNCTION_FUNCTION && compiler->proto->name &&
        strcmp(member->name, compiler->proto->name->data) == 0) {
        bool is_method_self_recursive = false;
        int skip = 0;  // 0=static method, 1=instance method (preserve R[0]=this)

        // Static method: ClassName.method() where ClassName is a registered class
        if (member->object->type == AST_VARIABLE && ctx->class_registry &&
            xr_class_registry_is_class(ctx->class_registry,
                                       member->object->as.variable.name)) {
            is_method_self_recursive = true;
            skip = 0;
        }
        // Instance method: this.method()
        else if (member->object->type == AST_THIS_EXPR) {
            is_method_self_recursive = true;
            skip = 1;
        }

        if (is_method_self_recursive) {
            int func_reg = reg_alloc(ctx, compiler);
            int first_arg_reg = func_reg + 1;
            xreg_set_freereg(compiler->regalloc, first_arg_reg);
            compile_args_to_base(ctx, compiler, node->arguments,
                                 node->arg_count, first_arg_reg);
            xemit_loop_back(compiler->emitter, func_reg, node->arg_count, skip);
            return -1;
        }
    }

    /* Optimization: static method call (determined at compile time).
     * Note: Arena class static methods are PRIMITIVE type, need nargs parameter.
     * OP_INVOKE_STATIC instruction format has no nargs, so Arena uses regular OP_INVOKE.
     */
    if (ctx->class_registry && member->object->type == AST_VARIABLE) {
        const char *obj_name = member->object->as.variable.name;

        // Coro module: coroutine statistics API and local storage
        if (strcmp(obj_name, GLOBAL_NAME_CORO) == 0) {
            // Coro.stats()
            if (strcmp(member->name, "stats") == 0 && node->arg_count == 0) {
                int result_reg = reg_alloc(ctx, compiler);
                xemit_coro_ctrl(compiler->emitter, result_reg, 0, CORO_CTRL_STATS);
                return result_reg;
            }
            // Coro.list() or Coro.list(limit, state)
            if (strcmp(member->name, "list") == 0 && node->arg_count <= 2) {
                int limit_reg;
                if (node->arg_count >= 1) {
                    XrExprDesc limit_desc = xr_compile_expr(ctx, compiler, node->arguments[0]);
                    limit_reg = xexpr_to_anyreg_readonly(ctx, compiler, &limit_desc);
                } else {
                    limit_reg = reg_alloc(ctx, compiler);
                    xemit_loadi(compiler->emitter, limit_reg, 0);
                }
                int result_reg = reg_alloc(ctx, compiler);
                if (node->arg_count >= 2) {
                    XrExprDesc state_desc = xr_compile_expr(ctx, compiler, node->arguments[1]);
                    int state_reg = xexpr_to_anyreg_readonly(ctx, compiler, &state_desc);
                    xemit_move(compiler->emitter, result_reg + 1, state_reg);
                } else {
                    xemit_loadnull(compiler->emitter, result_reg + 1);
                }
                xemit_coro_ctrl(compiler->emitter, result_reg, limit_reg, CORO_CTRL_LIST);
                return result_reg;
            }
            // Coro.deadlocks()
            if (strcmp(member->name, "deadlocks") == 0 && node->arg_count == 0) {
                int result_reg = reg_alloc(ctx, compiler);
                xemit_coro_ctrl(compiler->emitter, result_reg, 0, CORO_CTRL_DEADLOCKS);
                return result_reg;
            }
            // Coro.top(N, metric)
            if (strcmp(member->name, "top") == 0 && node->arg_count >= 1 && node->arg_count <= 2) {
                XrExprDesc n_desc = xr_compile_expr(ctx, compiler, node->arguments[0]);
                int n_reg = xexpr_to_anyreg_readonly(ctx, compiler, &n_desc);

                int result_reg = reg_alloc(ctx, compiler);
                // TOP: A=result, B=N, extra metric in R[A+1]
                if (node->arg_count >= 2) {
                    XrExprDesc metric_desc = xr_compile_expr(ctx, compiler, node->arguments[1]);
                    int metric_reg = xexpr_to_anyreg_readonly(ctx, compiler, &metric_desc);
                    xemit_move(compiler->emitter, result_reg + 1, metric_reg);
                } else {
                    xemit_loadnull(compiler->emitter, result_reg + 1);
                }
                xemit_coro_ctrl(compiler->emitter, result_reg, n_reg, CORO_CTRL_TOP);
                return result_reg;
            }
            // Coro.groupBy(field)
            if (strcmp(member->name, "groupBy") == 0 && node->arg_count == 1) {
                XrExprDesc field_desc = xr_compile_expr(ctx, compiler, node->arguments[0]);
                int field_reg = xexpr_to_anyreg_readonly(ctx, compiler, &field_desc);
                int result_reg = reg_alloc(ctx, compiler);
                xemit_coro_ctrl(compiler->emitter, result_reg, field_reg, CORO_CTRL_GROUP_BY);
                return result_reg;
            }
            // Coro.setLocal(key, value)
            if (strcmp(member->name, "setLocal") == 0 && node->arg_count == 2) {
                XrExprDesc key_desc = xr_compile_expr(ctx, compiler, node->arguments[0]);
                int key_reg = xexpr_to_anyreg_readonly(ctx, compiler, &key_desc);
                XrExprDesc val_desc = xr_compile_expr(ctx, compiler, node->arguments[1]);
                int val_reg = xexpr_to_anyreg_readonly(ctx, compiler, &val_desc);
                xemit_set_local(compiler->emitter, key_reg, val_reg);
                return -1;
            }
            // Coro.getLocal(key)
            if (strcmp(member->name, "getLocal") == 0 && node->arg_count == 1) {
                XrExprDesc key_desc = xr_compile_expr(ctx, compiler, node->arguments[0]);
                int key_reg = xexpr_to_anyreg_readonly(ctx, compiler, &key_desc);
                int result_reg = reg_alloc(ctx, compiler);
                xemit_get_local(compiler->emitter, result_reg, key_reg);
                return result_reg;
            }
            // Coro.setPriority(task, priority)
            if (strcmp(member->name, "setPriority") == 0 && node->arg_count == 2) {
                XrExprDesc task_desc = xr_compile_expr(ctx, compiler, node->arguments[0]);
                int task_reg = xexpr_to_anyreg_readonly(ctx, compiler, &task_desc);
                XrExprDesc prio_desc = xr_compile_expr(ctx, compiler, node->arguments[1]);
                int prio_reg = xexpr_to_anyreg_readonly(ctx, compiler, &prio_desc);
                xemit_set_priority(compiler->emitter, task_reg, prio_reg);
                return -1;
            }
            // Coro.lockThread()
            if (strcmp(member->name, "lockThread") == 0 && node->arg_count == 0) {
                xemit_lock_thread(compiler->emitter);
                return -1;
            }
            // Coro.unlockThread()
            if (strcmp(member->name, "unlockThread") == 0 && node->arg_count == 0) {
                xemit_unlock_thread(compiler->emitter);
                return -1;
            }
            // Coro.dump() or Coro.dump(limit)
            if (strcmp(member->name, "dump") == 0 && node->arg_count <= 1) {
                int limit = 0;
                if (node->arg_count == 1 && node->arguments[0]->type == AST_LITERAL_INT) {
                    limit = (int)node->arguments[0]->as.literal.raw_value.int_val;
                }
                xemit_coro_ctrl(compiler->emitter, limit, 0, CORO_CTRL_DUMP);
                int result_reg = reg_alloc(ctx, compiler);
                xemit_loadnull(compiler->emitter, result_reg);
                return result_reg;
            }
            // Coro.stalled() or Coro.stalled(timeout_ms)
            if (strcmp(member->name, "stalled") == 0 && node->arg_count <= 1) {
                int timeout_reg;
                if (node->arg_count == 1) {
                    XrExprDesc timeout_desc = xr_compile_expr(ctx, compiler, node->arguments[0]);
                    timeout_reg = xexpr_to_anyreg_readonly(ctx, compiler, &timeout_desc);
                } else {
                    timeout_reg = reg_alloc(ctx, compiler);
                    xemit_loadi(compiler->emitter, timeout_reg, 5000);
                }
                int result_reg = reg_alloc(ctx, compiler);
                xemit_coro_ctrl(compiler->emitter, result_reg, timeout_reg, CORO_CTRL_STALLED);
                return result_reg;
            }
            // Coro.whereis(name) -> bool
            if (strcmp(member->name, "whereis") == 0 && node->arg_count == 1) {
                XrExprDesc name_desc = xr_compile_expr(ctx, compiler, node->arguments[0]);
                int name_reg = xexpr_to_anyreg_readonly(ctx, compiler, &name_desc);
                int result_reg = reg_alloc(ctx, compiler);
                xemit_coro_ctrl(compiler->emitter, result_reg, name_reg, CORO_CTRL_WHEREIS);
                return result_reg;
            }
            // Coro.monitor(name) -> Channel
            if (strcmp(member->name, "monitor") == 0 && node->arg_count == 1) {
                XrExprDesc name_desc = xr_compile_expr(ctx, compiler, node->arguments[0]);
                int name_reg = xexpr_to_anyreg_readonly(ctx, compiler, &name_desc);
                int result_reg = reg_alloc(ctx, compiler);
                xemit_coro_ctrl(compiler->emitter, result_reg, name_reg, CORO_CTRL_MONITOR);
                return result_reg;
            }
            // Coro.demonitor(name, channel)
            if (strcmp(member->name, "demonitor") == 0 && node->arg_count == 2) {
                XrExprDesc name_desc = xr_compile_expr(ctx, compiler, node->arguments[0]);
                int name_reg = xexpr_to_anyreg_readonly(ctx, compiler, &name_desc);
                XrExprDesc ch_desc = xr_compile_expr(ctx, compiler, node->arguments[1]);
                int ch_reg = xexpr_to_anyreg_readonly(ctx, compiler, &ch_desc);
                int result_reg = reg_alloc(ctx, compiler);
                // A=result(unused), B=name_reg, C=DEMONITOR; channel in R[A+1]
                xemit_move(compiler->emitter, result_reg + 1, ch_reg);
                xemit_coro_ctrl(compiler->emitter, result_reg, name_reg, CORO_CTRL_DEMONITOR);
                return result_reg;
            }
            // Coro.self() -> string|null
            if (strcmp(member->name, "self") == 0 && node->arg_count == 0) {
                int result_reg = reg_alloc(ctx, compiler);
                xemit_coro_ctrl(compiler->emitter, result_reg, 0, CORO_CTRL_SELF);
                return result_reg;
            }
            // Coro.kill(name) or Coro.kill(name, reason) -> bool
            if (strcmp(member->name, "kill") == 0 &&
                node->arg_count >= 1 && node->arg_count <= 2) {
                int result_reg = reg_alloc(ctx, compiler);
                XrExprDesc name_desc = xr_compile_expr(ctx, compiler, node->arguments[0]);
                int name_reg = xexpr_to_anyreg(ctx, compiler, &name_desc);
                xemit_coro_ctrl(compiler->emitter, result_reg, name_reg, CORO_CTRL_KILL);
                return result_reg;
            }
        }

        // CoroPool constructor: CoroPool(4)
        if (strcmp(obj_name, GLOBAL_NAME_COROPOOL) == 0) {
            // Only handle variable access case here, like CoroPool.xxx()
            // Constructor CoroPool(4) needs to be handled in AST_CALL direct variable call
        }

    }

    // (label removed: normal_method_call)

    /* StringBuilder inline optimization.
     *
     * Detect sb.append(x) call, generate OP_STRBUF_APPEND instead of OP_INVOKE.
     *
     * Before optimization:
     *   MOVE R[12] R[6]           ; copy sb (protect local variable)
     *   MOVE R[13] R[result]      ; copy argument
     *   INVOKE R[12] "append" 1   ; method call (~100ns)
     *
     * After optimization:
     *   STRBUF_APPEND R[6] R[result]  ; direct append (~40ns)
     *   (no protection needed, STRBUF_APPEND doesn't overwrite R[A])
     *
     * Performance gain: eliminate 2 MOVEs + method lookup overhead
     */
    if (strcmp(member->name, "append") == 0 && node->arg_count == 1) {
        /* Detect if object is StringBuilder type.
         * Method 1: through compile-time type inference
         * Method 2: through variable name pattern matching (sb, builder, etc.)
         */
        bool is_string_builder = false;

        // Lookup type from local variable list
        if (member->object->type == AST_VARIABLE) {
            const char *var_name = member->object->as.variable.name;
            XrString *name_str = xr_compile_time_intern(ctx->X, var_name, strlen(var_name));

            for (int i = compiler->local_list.count - 1; i >= 0; i--) {
                XrLocalInfo *local = compiler->local_list.items[i];
                if (local->name == name_str ||
                    (local->name && strcmp(local->name->data, var_name) == 0)) {
                    XrType *ct = local->compile_type;
                    if (ct &&
                        (ct->kind == XR_KIND_CLASS || ct->kind == XR_KIND_INSTANCE) &&
                        ct->instance.class_name &&
                        strcmp(ct->instance.class_name, "StringBuilder") == 0) {
                        is_string_builder = true;
                    }
                    break;
                }
            }
        }

        if (is_string_builder) {
            /* Generate OP_STRBUF_APPEND instruction.
             *
             * Use readonly mode to get sb register, directly reuse local variable register.
             * Avoid generating MOVE instruction, and prevent parameter compilation from overwriting sb.
             */

            // Compile object expression (StringBuilder), readonly doesn't generate MOVE
            XrExprDesc obj_desc = xr_compile_expr(ctx, compiler, member->object);
            int sb_reg = xexpr_to_anyreg_readonly(ctx, compiler, &obj_desc);

            // Compile argument expression
            XrExprDesc arg_desc = xr_compile_expr(ctx, compiler, node->arguments[0]);
            int arg_reg = xexpr_to_anyreg(ctx, compiler, &arg_desc);

            // Emit OP_STRBUF_APPEND A B: strbuf(R[A]).append(R[B])
            xemit_strbuf_append(compiler->emitter, sb_reg, arg_reg);

            // StringBuilder.append() returns this, so return sb_reg
            return sb_reg;
        }
    }

    // toString() inline optimization
    if (member_sym == SYMBOL_TOSTRING && node->arg_count == 0) {
        bool is_string_builder = false;

        if (member->object->type == AST_VARIABLE) {
            const char *var_name = member->object->as.variable.name;
            XrString *name_str = xr_compile_time_intern(ctx->X, var_name, strlen(var_name));

            for (int i = compiler->local_list.count - 1; i >= 0; i--) {
                XrLocalInfo *local = compiler->local_list.items[i];
                if (local->name == name_str ||
                    (local->name && strcmp(local->name->data, var_name) == 0)) {
                    XrType *ct = (XrType*)(local->compile_type);
                    if (ct &&
                        ct->kind == XR_KIND_CLASS &&
                        ct->instance.class_name &&
                        strcmp(ct->instance.class_name, "StringBuilder") == 0) {
                        is_string_builder = true;
                    }
                    break;
                }
            }
        }

        if (is_string_builder) {
            // Generate OP_STRBUF_FINISH instruction
            XrExprDesc obj_desc = xr_compile_expr(ctx, compiler, member->object);
            int sb_reg = xexpr_to_anyreg(ctx, compiler, &obj_desc);

            // Emit OP_STRBUF_FINISH A: R[A] = strbuf(R[A]).to_string()
            xemit_strbuf_finish(compiler->emitter, sb_reg);

            return sb_reg;
        }
    }

    /* Map.increment(key) inline optimization.
     *
     * Detect map.increment(key) call, generate OP_MAP_INCREMENT instead of complex pattern.
     *
     * Before optimization (if-else pattern):
     *   INVOKE R[...] "has" 1       ; map.has(key)
     *   TEST/JMP                     ; conditional jump
     *   INDEX_GET, ADD, INDEX_SET   ; map[key] = map[key] + 1
     *   JMP
     *   INDEX_SET                    ; map[key] = 1
     *
     * After optimization:
     *   MAP_INCREMENT R[map] R[key]  ; if key doesn't exist set to 1, else +1
     *
     * Usage: freq.increment(subseq)
     */
    /* String.substring(start, end) inline optimization.
     *
     * Before optimization:
     *   MOVE R[12] R[0]      ; copy str
     *   MOVE R[13] R[10]     ; copy start
     *   ADD  R[14] ...       ; compute end
     *   INVOKE R[11] ...     ; method call (~100ns)
     *
     * After optimization:
     *   SUBSTRING R[A] R[B]  ; R[B]=str, R[B+1]=start, R[B+2]=end
     *
     * Eliminate 3 MOVEs + method lookup overhead
     */
    if (member_sym == SYMBOL_SUBSTRING && node->arg_count == 2) {
        /* String.substring(start, end) inline to OP_SUBSTRING.
         *
         * Layout: OP_SUBSTRING A B C
         *   R[A] = R[B].substring(R[C], R[C+1])
         *
         * Optimization strategy:
         *   - string: use readonly, no MOVE
         *   - start: if simple variable, use its register directly
         *   - end: place at start_reg + 1
         */

        // 1. Compile string (readonly)
        XrExprDesc obj_desc = xr_compile_expr(ctx, compiler, member->object);
        int str_reg = xexpr_to_anyreg_readonly(ctx, compiler, &obj_desc);

        /* 2. Allocate consecutive registers for start and end.
         *
         * Note: Cannot directly reuse local variable registers!
         * Because end must be at start_reg + 1, which may overwrite other local variables.
         * Example: let len = s.length; ... s.substring(i, end)
         * If i is in R[1], len is in R[2], direct reuse would cause end to overwrite len.
         */
        int start_reg = xreg_get_freereg(compiler->regalloc);
        int end_reg = start_reg + 1;
        xreg_set_freereg(compiler->regalloc, end_reg + 1);

        // Compile start to specific register
        XrExprDesc start_desc = xr_compile_expr(ctx, compiler, node->arguments[0]);
        xexpr_to_specific_reg(ctx, compiler, &start_desc, start_reg);

        // Compile end to start_reg + 1
        XrExprDesc end_desc = xr_compile_expr(ctx, compiler, node->arguments[1]);
        xexpr_to_specific_reg(ctx, compiler, &end_desc, end_reg);

        // 4. Allocate result register
        int result_reg = end_reg + 1;
        xreg_set_freereg(compiler->regalloc, result_reg + 1);

        // 5. Emit OP_SUBSTRING result str_reg start_reg
        xemit_substring(compiler->emitter, result_reg, str_reg, start_reg);

        return result_reg;
    }

    if (strcmp(member->name, "increment") == 0 && node->arg_count == 1) {
        // Detect if object is Map type (compile-time type inference)
        bool is_map = false;

        if (member->object->type == AST_VARIABLE) {
            const char *var_name = member->object->as.variable.name;
            XrString *name_str = xr_compile_time_intern(ctx->X, var_name, strlen(var_name));

            for (int i = compiler->local_list.count - 1; i >= 0; i--) {
                XrLocalInfo *local = compiler->local_list.items[i];
                if (local->name == name_str ||
                    (local->name && strcmp(local->name->data, var_name) == 0)) {
                    XrType *ct = (XrType*)(local->compile_type);
                    if (ct && ct->kind == XR_KIND_MAP) {
                        is_map = true;
                    }
                    break;
                }
            }
        }

        if (is_map) {
            /* Map.increment(key) -> OP_MAP_INCREMENT.
             *
             * Use readonly version to directly reuse map and key registers.
             * Eliminate 2 MOVE instructions.
             */

            // Compile object expression (Map) - readonly, no copy needed
            XrExprDesc obj_desc = xr_compile_expr(ctx, compiler, member->object);
            int map_reg = xexpr_to_anyreg_readonly(ctx, compiler, &obj_desc);

            // Compile key parameter expression - readonly, no copy needed
            XrExprDesc key_desc = xr_compile_expr(ctx, compiler, node->arguments[0]);
            int key_reg = xexpr_to_anyreg_readonly(ctx, compiler, &key_desc);

            // Emit OP_MAP_INCREMENT A B: map(R[A])[R[B]]++
            xemit_map_increment(compiler->emitter, map_reg, key_reg);

            // increment has no return value
            return -1;
        }
    }

    /* Map.get(key) inline optimization.
     *
     * Before: INVOKE_BUILTIN -> map_method_call_by_symbol -> map_get_handler
     * After:  MAP_GET or MAP_GETK (single instruction)
     */
    if (member_sym == SYMBOL_GET && node->arg_count == 1) {
        bool is_map = false;

        if (member->object->type == AST_VARIABLE) {
            const char *var_name = member->object->as.variable.name;
            XrString *name_str = xr_compile_time_intern(ctx->X, var_name, strlen(var_name));

            for (int i = compiler->local_list.count - 1; i >= 0; i--) {
                XrLocalInfo *local = compiler->local_list.items[i];
                if (local->name == name_str ||
                    (local->name && strcmp(local->name->data, var_name) == 0)) {
                    XrType *ct = (XrType*)(local->compile_type);
                    if (ct && ct->kind == XR_KIND_MAP) {
                        is_map = true;
                    }
                    break;
                }
            }
        }

        if (is_map) {
            // Compile map expression
            XrExprDesc obj_desc = xr_compile_expr(ctx, compiler, member->object);
            int map_reg = xexpr_to_anyreg_readonly(ctx, compiler, &obj_desc);

            // Check if key is string literal -> use MAP_GETK
            AstNode *key_node = node->arguments[0];
            if (key_node->type == AST_LITERAL_STRING) {
                // String literal key: use MAP_GETK with constant
                const char *key_str = key_node->as.literal.raw_value.string_val;
                XrString *key_intern = xr_compile_time_intern(ctx->X, key_str, strlen(key_str));
                int key_const = xr_vm_proto_add_constant(compiler->proto, xr_string_value(key_intern));

                int result_reg = reg_alloc(ctx, compiler);
                xemit_map_getk(compiler->emitter, result_reg, map_reg, key_const);
                return result_reg;
            } else {
                // Dynamic key: use MAP_GET
                XrExprDesc key_desc = xr_compile_expr(ctx, compiler, key_node);
                int key_reg = xexpr_to_anyreg_readonly(ctx, compiler, &key_desc);

                int result_reg = reg_alloc(ctx, compiler);
                xemit_map_get(compiler->emitter, result_reg, map_reg, key_reg);
                return result_reg;
            }
        }
    }

    /* Map.set(key, value) inline optimization.
     *
     * Before: INVOKE_BUILTIN -> map_method_call_by_symbol -> map_set_handler
     * After:  MAP_SET or MAP_SETK (single instruction)
     */
    if (member_sym == SYMBOL_SET && node->arg_count == 2) {
        bool is_map = false;

        if (member->object->type == AST_VARIABLE) {
            const char *var_name = member->object->as.variable.name;
            XrString *name_str = xr_compile_time_intern(ctx->X, var_name, strlen(var_name));

            for (int i = compiler->local_list.count - 1; i >= 0; i--) {
                XrLocalInfo *local = compiler->local_list.items[i];
                if (local->name == name_str ||
                    (local->name && strcmp(local->name->data, var_name) == 0)) {
                    XrType *ct = (XrType*)(local->compile_type);
                    if (ct && ct->kind == XR_KIND_MAP) {
                        is_map = true;
                    }
                    break;
                }
            }
        }

        if (is_map) {
            // Compile map expression
            XrExprDesc obj_desc = xr_compile_expr(ctx, compiler, member->object);
            int map_reg = xexpr_to_anyreg_readonly(ctx, compiler, &obj_desc);

            // Check if key is string literal -> use MAP_SETK
            AstNode *key_node = node->arguments[0];
            if (key_node->type == AST_LITERAL_STRING) {
                // String literal key: use MAP_SETK with constant
                const char *key_str = key_node->as.literal.raw_value.string_val;
                XrString *key_intern = xr_compile_time_intern(ctx->X, key_str, strlen(key_str));
                int key_const = xr_vm_proto_add_constant(compiler->proto, xr_string_value(key_intern));

                // Compile value (xexpr_to_anyreg auto-BOXes typed values)
                XrExprDesc val_desc = xr_compile_expr(ctx, compiler, node->arguments[1]);
                int val_reg = xexpr_to_anyreg(ctx, compiler, &val_desc);

                xemit_map_setk(compiler->emitter, map_reg, key_const, val_reg);
                return map_reg;  // Return map for chaining
            } else {
                // Dynamic key: use MAP_SET
                XrExprDesc key_desc = xr_compile_expr(ctx, compiler, key_node);
                int key_reg = xexpr_to_anyreg_readonly(ctx, compiler, &key_desc);

                // Compile value (xexpr_to_anyreg auto-BOXes typed values)
                XrExprDesc val_desc = xr_compile_expr(ctx, compiler, node->arguments[1]);
                int val_reg = xexpr_to_anyreg(ctx, compiler, &val_desc);

                xemit_map_set(compiler->emitter, map_reg, key_reg, val_reg);
                return map_reg;  // Return map for chaining
            }
        }
    }

    // Array.push(item) inline optimization
    if (member_sym == SYMBOL_PUSH && node->arg_count == 1) {
        // Detect if object is Array type
        bool is_array = false;

        if (member->object->type == AST_VARIABLE) {
            const char *var_name = member->object->as.variable.name;
            XrString *name_str = xr_compile_time_intern(ctx->X, var_name, strlen(var_name));

            for (int i = compiler->local_list.count - 1; i >= 0; i--) {
                XrLocalInfo *local = compiler->local_list.items[i];
                if (local->name == name_str ||
                    (local->name && strcmp(local->name->data, var_name) == 0)) {
                    XrType *ct = (XrType*)(local->compile_type);
                    if (ct && ct->kind == XR_KIND_ARRAY) {
                        is_array = true;
                    }
                    break;
                }
            }
        }

        if (is_array) {
            // Detect typed array element type for OP_TARRAY_PUSH
            uint8_t elem_slot = XR_SLOT_ANY;
            if (member->object->type == AST_VARIABLE) {
                const char *var_name = member->object->as.variable.name;
                XrString *name_str = xr_compile_time_intern(ctx->X, var_name, strlen(var_name));
                for (int i = compiler->local_list.count - 1; i >= 0; i--) {
                    XrLocalInfo *local = compiler->local_list.items[i];
                    if (local->name == name_str ||
                        (local->name && strcmp(local->name->data, var_name) == 0)) {
                        XrType *ct = (XrType*)(local->compile_type);
                        if (ct && (ct->kind == XR_KIND_ARRAY)) {
                            XrType *elem = ct->container.element_type;
                            if (elem && (elem->kind == XR_KIND_INT)) elem_slot = XR_SLOT_I64;
                            else if (elem && (elem->kind == XR_KIND_FLOAT)) elem_slot = XR_SLOT_F64;
                        }
                        break;
                    }
                }
            }

            // Compile object expression (Array) - readonly
            XrExprDesc obj_desc = xr_compile_expr(ctx, compiler, member->object);
            int arr_reg = xexpr_to_anyreg_readonly(ctx, compiler, &obj_desc);

            if (elem_slot != XR_SLOT_ANY) {
                // Typed array: OP_TARRAY_PUSH (raw input, no BOX)
                XrExprDesc item_desc = xr_compile_expr(ctx, compiler, node->arguments[0]);
                int item_reg = xexpr_to_anyreg_readonly(ctx, compiler, &item_desc);
                xemit_tarray_push(compiler->emitter, arr_reg, item_reg);
            } else {
                // Generic array: OP_ARRAY_PUSH (tagged input)
                XrExprDesc item_desc = xr_compile_expr(ctx, compiler, node->arguments[0]);
                int item_reg = xexpr_to_anyreg(ctx, compiler, &item_desc);
                xemit_array_push(compiler->emitter, arr_reg, item_reg);
            }

            // Return this (array itself), supports chaining arr.push(1).push(2)
            return arr_reg;
        }
    }

    // Channel method inline optimization (supports local, upvalue and shared)

    // Helper: check if variable is Channel type
    #define CHECK_IS_CHANNEL(var_name) ({ \
        bool _is_channel = false; \
        XrString *_name_str = xr_compile_time_intern(ctx->X, var_name, strlen(var_name)); \
        /* 1. Check local variables */ \
        for (int i = compiler->local_list.count - 1; i >= 0; i--) { \
            XrLocalInfo *local = compiler->local_list.items[i]; \
            if (local->name == _name_str || \
                (local->name && strcmp(local->name->data, var_name) == 0)) { \
                XrType *ct = (XrType*)(local->compile_type); \
                if (ct && (ct->kind == XR_KIND_CHANNEL)) { \
                    _is_channel = true; \
                } \
                break; \
            } \
        } \
        /* 2. Check upvalue (closure captured variables) */ \
        if (!_is_channel) { \
            int uv_idx = scope_resolve_upvalue(ctx, compiler, _name_str); \
            if (uv_idx >= 0) { \
                XrType *ct = (XrType*)(compiler->upvalues[uv_idx].type_info); \
                if (ct && (ct->kind == XR_KIND_CHANNEL)) { \
                    _is_channel = true; \
                } \
            } \
        } \
        /* 3. Check shared variables (e.g. shared const ch = Channel(...)) */ \
        if (!_is_channel) { \
            int _sh_idx = shared_get_in_scope(ctx, compiler, _name_str); \
            if (_sh_idx >= 0) { \
                XrType *ct = shared_get_type(ctx, _sh_idx); \
                if (ct && (ct->kind == XR_KIND_CHANNEL)) { \
                    _is_channel = true; \
                } \
            } \
        } \
        _is_channel; \
    })

    // time.sleep(seconds) -> OP_SLEEP (coroutine friendly)
    if (strcmp(member->name, "sleep") == 0 && node->arg_count == 1) {
        // Check if it's time module
        if (member->object->type == AST_VARIABLE &&
            strcmp(member->object->as.variable.name, "time") == 0) {
            // Compile argument (seconds)
            XrExprDesc arg_desc = xr_compile_expr(ctx, compiler, node->arguments[0]);
            int arg_reg = xexpr_to_anyreg(ctx, compiler, &arg_desc);

            // Generate OP_SLEEP instruction
            xemit_sleep(compiler->emitter, arg_reg);

            reg_free(compiler, arg_reg);

            // sleep has no return value, return null
            int result_reg = reg_alloc(ctx, compiler);
            xemit_loadnull(compiler->emitter, result_reg);
            return result_reg;
        }
    }

    // ch.trySend(value) → OP_CHAN_TRY_SEND
    if (member_sym == SYMBOL_TRYSEND && node->arg_count == 1) {
        bool is_channel = false;
        if (member->object->type == AST_VARIABLE) {
            is_channel = CHECK_IS_CHANNEL(member->object->as.variable.name);
        }

        if (is_channel) {
            XrExprDesc obj_desc = xr_compile_expr(ctx, compiler, member->object);
            int ch_reg = xexpr_to_anyreg_readonly(ctx, compiler, &obj_desc);
            XrExprDesc val_desc = xr_compile_expr(ctx, compiler, node->arguments[0]);
            int val_reg = xexpr_to_anyreg_readonly(ctx, compiler, &val_desc);
            int result_reg = reg_alloc(ctx, compiler);
            xemit_chan_try_send(compiler->emitter, result_reg, ch_reg, val_reg);
            return result_reg;
        }
    }

    // ch.tryRecv() -> OP_CHAN_TRY_RECV (returns multi-value: value, ok)
    if (member_sym == SYMBOL_TRYRECV && node->arg_count == 0) {
        bool is_channel = false;
        if (member->object->type == AST_VARIABLE) {
            is_channel = CHECK_IS_CHANNEL(member->object->as.variable.name);
        }

        if (is_channel) {
            XrExprDesc obj_desc = xr_compile_expr(ctx, compiler, member->object);
            int ch_reg = xexpr_to_anyreg_readonly(ctx, compiler, &obj_desc);
            // Allocate two consecutive registers: R[A]=value, R[A+1]=ok
            int result_reg = reg_alloc(ctx, compiler);
            reg_alloc(ctx, compiler);  // Reserve second register for ok
            xemit_chan_try_recv(compiler->emitter, result_reg, ch_reg);
            return result_reg;
        }
    }

    // ch.close() → OP_CHAN_CLOSE
    if (member_sym == SYMBOL_CLOSE && node->arg_count == 0) {
        bool is_channel = false;
        if (member->object->type == AST_VARIABLE) {
            is_channel = CHECK_IS_CHANNEL(member->object->as.variable.name);
        }

        if (is_channel) {
            XrExprDesc obj_desc = xr_compile_expr(ctx, compiler, member->object);
            int ch_reg = xexpr_to_anyreg_readonly(ctx, compiler, &obj_desc);
            xemit_chan_close(compiler->emitter, ch_reg);
            return -1;  // close has no return value
        }
    }

    // ch.isClosed() -> OP_CHAN_IS_CLOSED (runtime type check)
    if (member_sym == SYMBOL_IS_CLOSED && node->arg_count == 0) {
        XrExprDesc obj_desc = xr_compile_expr(ctx, compiler, member->object);
        int ch_reg = xexpr_to_anyreg_readonly(ctx, compiler, &obj_desc);
        int result_reg = reg_alloc(ctx, compiler);
        xemit_chan_is_closed(compiler->emitter, result_reg, ch_reg);
        return result_reg;
    }

    /* ch.send(value) -> OP_CHAN_SEND (blocking send, dedicated instruction).
     * VM handles deep copy internally via vm_chan_copy_send.
     * Move semantics: shared let argument is moved after send.
     */
    if (member_sym == SYMBOL_SEND && node->arg_count == 1) {
        bool is_channel = false;
        if (member->object->type == AST_VARIABLE) {
            is_channel = CHECK_IS_CHANNEL(member->object->as.variable.name);
        }

        if (is_channel) {
            AstNode *arg = node->arguments[0];

            // Compile channel object and argument first
            XrExprDesc obj_desc = xr_compile_expr(ctx, compiler, member->object);
            int ch_reg = xexpr_to_anyreg_readonly(ctx, compiler, &obj_desc);
            XrExprDesc val_desc = xr_compile_expr(ctx, compiler, arg);
            int val_reg = xexpr_to_anyreg_readonly(ctx, compiler, &val_desc);

            // Explicit move semantics: only 'move var' triggers ownership transfer
            // Must be after compile so the variable is still accessible during codegen
            if (arg && arg->type == AST_MOVE_EXPR) {
                AstNode *inner = arg->as.move_expr.expr;
                if (inner && inner->type == AST_VARIABLE) {
                    const char *var_name = inner->as.variable.name;
                    XrString *name_str = xr_compile_time_intern(ctx->X, var_name, strlen(var_name));
                    int shared_idx = shared_get_in_scope(ctx, compiler, name_str);
                    if (shared_idx >= 0 && !shared_is_const(ctx, shared_idx)) {
                        shared_set_moved(ctx, shared_idx, inner->line, inner->column);
                        move_null_shared_idx = shared_idx;
                    }
                }
            }
            int result_reg = reg_alloc(ctx, compiler);
            xemit_chan_send(compiler->emitter, result_reg, ch_reg, val_reg);

            // Handle move semantics nullification
            if (move_null_shared_idx >= 0) {
                xemit_loadnull(compiler->emitter, result_reg);
                xemit_setshared(compiler->emitter, result_reg, move_null_shared_idx);
                move_null_shared_idx = -1;
            }
            return result_reg;
        }

        // Not a known Channel variable: fall through to generic INVOKE
        // Still handle move semantics for the fallback path
        AstNode *arg = node->arguments[0];
        if (arg && arg->type == AST_VARIABLE) {
            const char *var_name = arg->as.variable.name;
            XrString *name_str = xr_compile_time_intern(ctx->X, var_name, strlen(var_name));
            int shared_idx = shared_get_in_scope(ctx, compiler, name_str);
            if (shared_idx >= 0 && !shared_is_const(ctx, shared_idx)) {
                shared_set_moved(ctx, shared_idx, arg->line, arg->column);
                move_null_shared_idx = shared_idx;
            }
        }
    }

    // ch.sendTimeout(value, timeout) -> OP_CHAN_SEND_TIMEOUT (send with timeout)
    if (member_sym == SYMBOL_SENDTIMEOUT && node->arg_count == 2) {
        bool is_channel = false;
        if (member->object->type == AST_VARIABLE) {
            is_channel = CHECK_IS_CHANNEL(member->object->as.variable.name);
        }

        if (is_channel) {
            XrExprDesc obj_desc = xr_compile_expr(ctx, compiler, member->object);
            int ch_reg = xexpr_to_anyreg_readonly(ctx, compiler, &obj_desc);
            // Compile value and timeout to consecutive registers
            int base_reg = xreg_get_freereg(compiler->regalloc);
            XrExprDesc val_desc = xr_compile_expr(ctx, compiler, node->arguments[0]);
            xexpr_to_specific_reg(ctx, compiler, &val_desc, base_reg);
            XrExprDesc timeout_desc = xr_compile_expr(ctx, compiler, node->arguments[1]);
            xexpr_to_specific_reg(ctx, compiler, &timeout_desc, base_reg + 1);
            xreg_set_freereg(compiler->regalloc, base_reg + 2);
            int result_reg = reg_alloc(ctx, compiler);
            // OP_CHAN_SEND_TIMEOUT A B C: R[A] = R[B].send(R[C], timeout: R[C+1])
            xemit_chan_send_timeout(compiler->emitter, result_reg, ch_reg, base_reg);
            return result_reg;  // Return bool
        }
    }

    // ch.recv() -> OP_CHAN_RECV (blocking receive, returns multi-value: value, ok)
    if (member_sym == SYMBOL_RECV && node->arg_count == 0) {
        bool is_channel = false;
        if (member->object->type == AST_VARIABLE) {
            is_channel = CHECK_IS_CHANNEL(member->object->as.variable.name);
        }

        if (is_channel) {
            XrExprDesc obj_desc = xr_compile_expr(ctx, compiler, member->object);
            int ch_reg = xexpr_to_anyreg_readonly(ctx, compiler, &obj_desc);
            // Allocate two consecutive registers: R[A]=value, R[A+1]=ok
            int result_reg = reg_alloc(ctx, compiler);
            reg_alloc(ctx, compiler);  // Reserve second register for ok
            xemit_chan_recv(compiler->emitter, result_reg, ch_reg);
            return result_reg;
        }
    }

    // ch.recvTimeout(timeout) -> OP_CHAN_RECV_TIMEOUT (receive with timeout)
    if (member_sym == SYMBOL_RECVTIMEOUT && node->arg_count == 1) {
        bool is_channel = false;
        if (member->object->type == AST_VARIABLE) {
            is_channel = CHECK_IS_CHANNEL(member->object->as.variable.name);
        }

        if (is_channel) {
            XrExprDesc obj_desc = xr_compile_expr(ctx, compiler, member->object);
            int ch_reg = xexpr_to_anyreg_readonly(ctx, compiler, &obj_desc);
            XrExprDesc timeout_desc = xr_compile_expr(ctx, compiler, node->arguments[0]);
            int timeout_reg = xexpr_to_anyreg_readonly(ctx, compiler, &timeout_desc);
            int result_reg = reg_alloc(ctx, compiler);
            // OP_CHAN_RECV_TIMEOUT A B C: R[A] = R[B].recv(timeout: R[C])
            xemit_chan_recv_timeout(compiler->emitter, result_reg, ch_reg, timeout_reg);
            return result_reg;  // Return value or null
        }
    }

    #undef CHECK_IS_CHANNEL  // Clean up macro

    // task.info() - get coroutine info
    if (strcmp(member->name, "info") == 0 && node->arg_count == 0) {
        // Detect if object is coroutine type
        bool is_coro = false;
        if (member->object->type == AST_VARIABLE) {
            const char *var_name = member->object->as.variable.name;
            XrString *name_str = xr_compile_time_intern(ctx->X, var_name, strlen(var_name));

            for (int i = compiler->local_list.count - 1; i >= 0; i--) {
                XrLocalInfo *local = compiler->local_list.items[i];
                if (local->name == name_str ||
                    (local->name && strcmp(local->name->data, var_name) == 0)) {
                    XrType *ct = (XrType*)(local->compile_type);
                    if (ct && (ct->kind == XR_KIND_CLASS || ct->kind == XR_KIND_INSTANCE) && ct->instance.class_name &&
                        strcmp(ct->instance.class_name, "Coroutine") == 0) {
                        is_coro = true;
                    }
                    break;
                }
            }
        }

        if (is_coro) {
            XrExprDesc obj_desc = xr_compile_expr(ctx, compiler, member->object);
            int coro_reg = xexpr_to_anyreg_readonly(ctx, compiler, &obj_desc);
            int result_reg = reg_alloc(ctx, compiler);
            xemit_coro_ctrl(compiler->emitter, result_reg, coro_reg, CORO_CTRL_INFO);
            return result_reg;
        }
    }

    // Instance method call optimization (using compile-time type inference)
    XrType *obj_type = NULL;
    if (member->object->type == AST_VARIABLE) {
        const char *var_name = member->object->as.variable.name;
        XrString *name_str = xr_compile_time_intern(ctx->X, var_name, strlen(var_name));

        // Primary source: compiler's local variable list (always correct, even inside try/catch)
        for (int i = compiler->local_list.count - 1; i >= 0; i--) {
            XrLocalInfo *local = compiler->local_list.items[i];
            if (local->name == name_str ||
                (local->name && strcmp(local->name->data, var_name) == 0)) {
                obj_type = local->compile_type;
                break;
            }
        }

        // Fallback: analyzer (for shared/global variables)
        if (!obj_type && ctx->analyzer) {
            obj_type = xa_analyzer_lookup_var(ctx->analyzer, var_name);
        }
    }
    // this.method(): infer type from current class context
    else if (member->object->type == AST_THIS_EXPR && ctx->current_class_node) {
        ClassDeclNode *cls_node = (ClassDeclNode*)ctx->current_class_node;
        if (cls_node->name) {
            obj_type = xr_type_new_class(ctx->X, cls_node->name);
        }
    }

    // Builtin type optimization: compile-time detect Map/Array/String/Set/Json -> OP_INVOKE_BUILTIN
    if (obj_type) {
        bool is_builtin = false;

        // Check if it's a builtin type
        if (obj_type->kind == XR_KIND_ARRAY || obj_type->kind == XR_KIND_MAP ||
            obj_type->kind == XR_KIND_STRING || obj_type->kind == XR_KIND_SET ||
            obj_type->kind == XR_KIND_INT || obj_type->kind == XR_KIND_FLOAT ||
            obj_type->kind == XR_KIND_JSON) {
            is_builtin = true;
        } else if ((obj_type->kind == XR_KIND_CLASS || obj_type->kind == XR_KIND_INSTANCE) && obj_type->instance.class_name) {
            // Check if it's a builtin class name
            const char *name = obj_type->instance.class_name;
            if (strcmp(name, TYPE_NAME_ARRAY) == 0 || strcmp(name, TYPE_NAME_MAP) == 0 ||
                strcmp(name, TYPE_NAME_STRING) == 0 || strcmp(name, TYPE_NAME_SET) == 0 ||
                strcmp(name, TYPE_NAME_INT64) == 0 || strcmp(name, TYPE_NAME_FLOAT64) == 0 ||
                strcmp(name, TYPE_NAME_JSON) == 0) {
                is_builtin = true;
            }
        }

        if (is_builtin) {
            // Determine opcode: int/float use specialized raw-receiver opcodes
            bool is_int_type = (obj_type->kind == XR_KIND_INT) != 0;
            bool is_float_type = (obj_type->kind == XR_KIND_FLOAT) != 0;
            if (!is_int_type && !is_float_type && (obj_type->kind == XR_KIND_CLASS || obj_type->kind == XR_KIND_INSTANCE) && obj_type->instance.class_name) {
                if (strcmp(obj_type->instance.class_name, "int64") == 0) is_int_type = true;
                else if (strcmp(obj_type->instance.class_name, "float64") == 0) is_float_type = true;
            }

            int base = reg_alloc(ctx, compiler);
            ASSERT_REG_VALID(base);

            // int/float: use readonly (keeps raw value, no BOX)
            // other builtins: use anyreg (auto-BOX for tagged dispatch)
            XrExprDesc obj_desc = xr_compile_expr(ctx, compiler, member->object);
            int obj_reg;
            if (is_int_type || is_float_type) {
                obj_reg = xexpr_to_anyreg_readonly(ctx, compiler, &obj_desc);
            } else {
                obj_reg = xexpr_to_anyreg(ctx, compiler, &obj_desc);
            }
            if (obj_reg != base + 1) {
                xemit_move(compiler->emitter, base + 1, obj_reg);
            }

            // Use auto-protect API (base allocated, now protect base and base+1)
            int first_arg_reg = base + 2;
            int protect_id = xreg_protect_begin(compiler->regalloc, base, 2, "invoke_builtin_receiver");
            xreg_set_freereg(compiler->regalloc, first_arg_reg);
            ASSERT_FREEREG(compiler->regalloc, first_arg_reg);

            // Compile arguments to R[base+2], R[base+3], ... (base and base+1 protected)
            compile_args_to_base(ctx, compiler, node->arguments, node->arg_count, first_arg_reg);

            // Get method symbol (local index via per-function symbol table)
            int local_sym = emitter_add_symbol(compiler->emitter, member_sym);

            xemit_invoke_builtin(compiler->emitter, base, local_sym, node->arg_count);

            // End protection
            xreg_protect_end(compiler->regalloc, protect_id);
            xreg_set_freereg(compiler->regalloc, base + 1);
            return base;
        }
    }

    // OP_INVOKE_DIRECT optimization: compile-time known class instance method call
    if (obj_type && ctx->class_registry &&
        (obj_type->kind == XR_KIND_INSTANCE || obj_type->kind == XR_KIND_CLASS)) {
        // Resolve class name: prefer class_name, fallback to class_ref->name
        const char *cls_name = obj_type->instance.class_name;
        if (!cls_name && obj_type->instance.class_ref) {
            cls_name = obj_type->instance.class_ref->name;
        }
        if (cls_name) {
            ClassInfo *ci = xr_class_registry_lookup(ctx->class_registry, cls_name);
            if (ci) {
                int method_idx = xr_class_find_method_index(ci, member->name);
                if (method_idx >= 0) {
                    int base = reg_alloc(ctx, compiler);
                    ASSERT_REG_VALID(base);

                    XrExprDesc obj_desc = xr_compile_expr(ctx, compiler, member->object);
                    xexpr_to_specific_reg(ctx, compiler, &obj_desc, base + 1);

                    int first_arg_reg = base + 2;
                    int protect_id = xreg_protect_begin(compiler->regalloc, base, 2, "invoke_direct_receiver");
                    xreg_set_freereg(compiler->regalloc, first_arg_reg);
                    ASSERT_FREEREG(compiler->regalloc, first_arg_reg);

                    compile_args_to_base(ctx, compiler, node->arguments, node->arg_count, first_arg_reg);

                    int c_arg = node->arg_count;
                    if (is_tail && compiler->type == FUNCTION_FUNCTION) {
                        c_arg |= 0x80;  // tail flag in bit 7
                    }
                    xemit_invoke_direct(compiler->emitter, base, method_idx, c_arg);

                    xreg_protect_end(compiler->regalloc, protect_id);
                    if (is_tail && compiler->type == FUNCTION_FUNCTION) {
                        return -1;
                    }
                    xreg_set_freereg(compiler->regalloc, base + 1);
                    return base;
                }
            }
        }
    }

    /* Chain call optimization: non-variable receiver with known primitive type.
     * e.g. f.floor().abs() — the .abs() receiver is a CallExpr, not a Variable.
     * Compile receiver first to get compile_type, then select INVOKE_INT/FLOAT. */
    if (member->object->type != AST_VARIABLE) {
        int base = reg_alloc(ctx, compiler);
        ASSERT_REG_VALID(base);

        XrExprDesc obj_desc = xr_compile_expr(ctx, compiler, member->object);

        if (xexpr_is_raw_i64(&obj_desc) || xexpr_is_raw_f64(&obj_desc)) {
            bool is_int_chain = xexpr_is_raw_i64(&obj_desc);
            (void)is_int_chain;
            int obj_reg = xexpr_to_anyreg_readonly(ctx, compiler, &obj_desc);
            if (obj_reg != base + 1) {
                xemit_move(compiler->emitter, base + 1, obj_reg);
            }

            int first_arg_reg = base + 2;
            int protect_id = xreg_protect_begin(compiler->regalloc, base, 2, "invoke_chain");
            xreg_set_freereg(compiler->regalloc, first_arg_reg);
            ASSERT_FREEREG(compiler->regalloc, first_arg_reg);

            compile_args_to_base(ctx, compiler, node->arguments, node->arg_count, first_arg_reg);

            int local_sym = emitter_add_symbol(compiler->emitter, member_sym);

            xemit_invoke_builtin(compiler->emitter, base, local_sym, node->arg_count);

            xreg_protect_end(compiler->regalloc, protect_id);
            xreg_set_freereg(compiler->regalloc, base + 1);
            return base;
        }

        // Non-primitive receiver: generic INVOKE
        xexpr_to_specific_reg(ctx, compiler, &obj_desc, base + 1);

        int first_arg_reg = base + 2;
        int protect_id = xreg_protect_begin(compiler->regalloc, base, 2, "invoke_receiver");
        xreg_set_freereg(compiler->regalloc, first_arg_reg);
        ASSERT_FREEREG(compiler->regalloc, first_arg_reg);

        compile_args_to_base(ctx, compiler, node->arguments, node->arg_count, first_arg_reg);

        int local_sym = emitter_add_symbol(compiler->emitter, member_sym);

        if (is_tail && compiler->type == FUNCTION_FUNCTION) {
            xemit_invoke_tail(compiler->emitter, base, local_sym, node->arg_count);
            xreg_protect_end(compiler->regalloc, protect_id);
            return -1;
        }
        xemit_invoke(compiler->emitter, base, local_sym, node->arg_count);

        xreg_protect_end(compiler->regalloc, protect_id);
        xreg_set_freereg(compiler->regalloc, base + 1);
        return base;
    }

    /* Unified calling convention: reserve this position at compile time.
     * Layout: R[base]=return, R[base+1]=this, R[base+2]=arg1, ... */

    // Allocate base register (for return value)
    int base = reg_alloc(ctx, compiler);
    ASSERT_REG_VALID(base);

    // Compile this to R[base+1]
    XrExprDesc obj_desc = xr_compile_expr(ctx, compiler, member->object);
    xexpr_to_specific_reg(ctx, compiler, &obj_desc, base + 1);

    // Fix: protect receiver register
    int first_arg_reg = base + 2;
    int protect_id = xreg_protect_begin(compiler->regalloc, base, 2, "invoke_receiver");
    xreg_set_freereg(compiler->regalloc, first_arg_reg);
    ASSERT_FREEREG(compiler->regalloc, first_arg_reg);

    // Compile arguments to R[base+2], R[base+3], ...
    compile_args_to_base(ctx, compiler, node->arguments, node->arg_count, first_arg_reg);

    int local_sym = emitter_add_symbol(compiler->emitter, member_sym);

    // Generate OP_INVOKE or OP_INVOKE_TAIL instruction
    if (is_tail && compiler->type == FUNCTION_FUNCTION) {
        xemit_invoke_tail(compiler->emitter, base, local_sym, node->arg_count);
        xreg_protect_end(compiler->regalloc, protect_id);
        return -1;
    }
    xemit_invoke(compiler->emitter, base, local_sym, node->arg_count);

    // Move semantics: nullify shared let variable after send completes
    if (move_null_shared_idx >= 0) {
        int tmp = reg_alloc(ctx, compiler);
        xemit_loadnull(compiler->emitter, tmp);
        xemit_setshared(compiler->emitter, tmp, move_null_shared_idx);
        reg_free(compiler, tmp);
    }

    // End protection and set freereg
    xreg_protect_end(compiler->regalloc, protect_id);
    xreg_set_freereg(compiler->regalloc, base + 1);

    // Return value in base
    return base;
}
