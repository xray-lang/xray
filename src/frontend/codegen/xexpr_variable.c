/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xexpr_variable.c - Xray variable access expression compilation
 *
 * KEY CONCEPT:
 *   - Local variables
 *   - Upvalue (closure captured variables)
 *   - Namespace symbols (project mode)
 *   - Global variables
 */

#include "xexpr.h"
#include "../../base/xchecks.h"
#include "xcompiler.h"
#include "xcompiler_context.h"
#include "xemit.h"
#include "xexpr_desc.h"
#include "../../runtime/value/xvalue.h"  // xr_int, xr_float, xr_string_value
#include "../../runtime/xisolate_api.h"  // XrayIsolate full definition
#include "../parser/xast_nodes.h"        // XR_STORAGE_OWNED
#include "../../runtime/value/xchunk.h"  // MAXARG_sBx
#include <stdio.h>
#include <string.h>

// External functions (need to be public in xcompiler.h)
// - scope_resolve_local
// - scope_resolve_upvalue
// - globals_get_or_add

// ========== Variable Access Compilation ==========

/*
 * Compile variable access expression (returns XrExprDesc)
 *
 * Lookup order:
 * 1. Local variable: return XEXPR_LOCAL
 * 2. Upvalue: return XEXPR_RELOC (GETUPVAL)
 * 3. Global variable: return XEXPR_RELOC (GETGLOBAL)
 *
 * @return Expression descriptor
 */
XrExprDesc compile_variable(XrCompilerContext *ctx, XrCompiler *compiler, VariableNode *node) {
    XR_DCHECK(ctx != NULL, "compile_variable: NULL ctx");
    XR_DCHECK(compiler != NULL, "compile_variable: NULL compiler");
    XR_DCHECK(node != NULL, "compile_variable: NULL node");
    XrExprDesc e = {0};
    xexpr_init(&e, XEXPR_VOID, -1);

    // Use compile-time interning to ensure string comparability
    XrString *name_str = xr_compile_time_intern(ctx->X, node->name, strlen(node->name));

    // 1. Lookup local variable
    XrLocalInfo *local_info = compiler_get_local_by_name(compiler, name_str->data);
    if (local_info) {
        int local_reg = local_info->reg;

        // Check if variable is spilled (using variable's own spill_slot)
        if (local_info->spill_slot >= 0) {
            // Rematerialization optimization: constants don't need RELOAD, regenerate directly
            if (local_info->can_rematerialize) {
                int temp = xreg_alloc_temp(compiler->regalloc);
                // Regenerate constant value (faster than RELOAD)
                xemit_loadi(compiler->emitter, temp, (int) local_info->remat_value);
                e.kind = XEXPR_TEMP;
                e.reg = temp;
                return e;
            }

            // After spill, need RELOAD
            int temp = xreg_alloc_temp(compiler->regalloc);
            emit_reload(compiler->emitter, temp, local_info->spill_slot);
            e.kind = XEXPR_TEMP;
            e.reg = temp;
            return e;
        }

        // Const propagation: inline compile-time constant value
        if (local_info->is_const && local_info->comptime.type != COMPTIME_NONE) {
            switch (local_info->comptime.type) {
                case COMPTIME_INT:
                    xexpr_init_int(&e, local_info->comptime.as.int_val);
                    return e;
                case COMPTIME_FLOAT: {
                    XrValue val = xr_float(local_info->comptime.as.float_val);
                    int kidx = xr_vm_proto_add_constant(compiler->proto, val);
                    int pc = xemit_loadk(compiler->emitter, 0, kidx);
                    e.kind = XEXPR_RELOC;
                    e.u.pc = pc;
                    return e;
                }
                case COMPTIME_BOOL:
                    e.kind = local_info->comptime.as.bool_val ? XEXPR_TRUE : XEXPR_FALSE;
                    e.reg = -1;
                    return e;
                default:
                    break;
            }
        }

        // Cellified variable: register holds a cell ref, deref to get value.
        if (local_info->is_cellified) {
            int pc = xemit_cell_get(compiler->emitter, 0, local_info->reg);
            e.kind = XEXPR_RELOC;
            e.u.pc = pc;
            e.compile_type = local_info->compile_type;
            return e;
        }
        // Local variable not spilled: return XEXPR_LOCAL, no instruction needed
        e.kind = XEXPR_LOCAL;
        e.reg = local_reg;  // Use top-level reg field
        // slot_type stays XR_SLOT_ANY: interpreter locals are always tagged
        e.compile_type = local_info->compile_type;  // Propagate analyzer type
        // LIFO mode doesn't need to mark usage
        return e;
    }

    // 2. Lookup shared variable (consider lexical scope, priority over upvalue)
    int shared_index = shared_get_in_scope(ctx, compiler, name_str);
    if (shared_index >= 0) {
        // Move semantics check: shared let cannot be used after move
        if (!shared_is_const(ctx, shared_index) && shared_is_moved(ctx, shared_index)) {
            int moved_line, moved_column;
            shared_get_moved_location(ctx, shared_index, &moved_line, &moved_column);
            xr_compiler_error(ctx, compiler,
                              "use of moved value '%s' (moved at line %d)\n"
                              "hint: consider using copy() if you need to retain the value",
                              node->name, moved_line);
            e.kind = XEXPR_NULL;
            return e;
        }

        int pc = xemit_getshared(compiler->emitter, 0, shared_index);
        e.kind = XEXPR_RELOC;
        e.u.pc = pc;
        return e;
    }

    // 3. Const inlining: check ConstEntry BEFORE upvalue (avoids GETUPVAL for constants)
    ConstEntry *const_entry = xr_compiler_ctx_find_const(ctx, name_str);
    if (const_entry) {
        int pc;
        switch (const_entry->type) {
            case CONST_INT: {
                int64_t value = const_entry->value.int_val;
                if (value >= -MAXARG_sBx && value <= MAXARG_sBx) {
                    pc = xemit_loadi(compiler->emitter, 0, (int) value);
                } else {
                    XrValue val = xr_int(value);
                    int kidx = xr_vm_proto_add_constant(compiler->proto, val);
                    pc = xemit_loadk(compiler->emitter, 0, kidx);
                }
                e.kind = XEXPR_RELOC;
                e.u.pc = pc;
                return e;
            }
            case CONST_FLOAT: {
                XrValue val = xr_float(const_entry->value.float_val);
                int kidx = xr_vm_proto_add_constant(compiler->proto, val);
                pc = xemit_loadk(compiler->emitter, 0, kidx);
                e.kind = XEXPR_RELOC;
                e.u.pc = pc;
                return e;
            }
            case CONST_STRING: {
                XrValue val = xr_string_value(const_entry->value.str_val);
                int kidx = xr_vm_proto_add_constant(compiler->proto, val);
                pc = xemit_loadk(compiler->emitter, 0, kidx);
                e.kind = XEXPR_RELOC;
                e.u.pc = pc;
                return e;
            }
            default:
                break;
        }
    }

    // 4. Lookup upvalue (after ConstEntry, so const values are inlined instead of CTX_LOAD)
    int upvalue = scope_resolve_upvalue(ctx, compiler, name_str);
    if (upvalue >= 0) {
        XrUpvalueDesc *uv = &compiler->upvalues[upvalue];
        int pc;
        if (uv->is_const) {
            // const: upvals[j] holds the raw value
            pc = xemit_upval_get(compiler->emitter, 0, upvalue, 0);
        } else {
            // let: upvals[j] holds a cell ref, need UPVAL_GET + CELL_GET
            int tmp = reg_alloc(ctx, compiler);
            xemit_upval_get(compiler->emitter, tmp, upvalue, 0);
            pc = xemit_cell_get(compiler->emitter, 0, tmp);
            reg_free(compiler, tmp);
        }
        e.kind = XEXPR_RELOC;
        e.u.pc = pc;
        return e;
    }

    // 5. Lookup global variable (don't auto-create)
    int global_index = builtin_get(ctx, name_str);
    if (global_index < 0) {
        // Variable undefined, report compile error
        xr_compiler_error(ctx, compiler, "undefined variable '%s'", node->name);
        e.kind = XEXPR_NULL;
        return e;
    }

    // Global variable exists: generate GETGLOBAL, A=0 pending relocation
    int pc = xemit_getbuiltin(compiler->emitter, 0, global_index);
    e.kind = XEXPR_RELOC;
    e.u.pc = pc;
    return e;
}
