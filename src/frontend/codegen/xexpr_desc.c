/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xexpr_desc.c - Xray expression descriptor implementation
 *
 * KEY CONCEPT:
 *   Smart register allocation
 */

#include "xexpr_desc.h"
#include "../../base/xchecks.h"
#include "../../base/xlog.h"
#include "../../base/xmalloc.h"
#include "xcompiler.h"
#include "xcompiler_context.h"
#include "xemit.h"
#include "xregalloc.h"
#include "../../runtime/value/xtype.h"
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

// Record per-instruction type annotation during compilation.
// Grows inst_type_buf on demand. Called at each discharge point.
static void compiler_record_inst_type(XrCompiler *compiler, int pc, struct XrType *type) {
    if (pc < 0 || !type) return;
    if (pc >= compiler->inst_type_cap) {
        int new_cap = (pc + 64) & ~63;  // round up to 64
        struct XrType **buf = (struct XrType **)xr_realloc(
            compiler->inst_type_buf, new_cap * sizeof(struct XrType *));
        if (!buf) return;
        memset(buf + compiler->inst_type_cap, 0,
               (new_cap - compiler->inst_type_cap) * sizeof(struct XrType *));
        compiler->inst_type_buf = buf;
        compiler->inst_type_cap = new_cap;
    }
    compiler->inst_type_buf[pc] = type;
}

// ========== Initialization Functions ==========

void xexpr_init(XrExprDesc *e, XExprKind kind, int reg) {
    XR_DCHECK(e != NULL, "xexpr_init: NULL expr desc");
    e->kind = kind;
    e->reg = reg;
    e->u.ival = 0;
    e->t = -1; // NO_JUMP
    e->f = -1; // NO_JUMP
    e->has_jumps = false;
    e->is_const = false;
    e->is_raw = false;
    e->compile_type = NULL;
}

void xexpr_init_void(XrExprDesc *e) {
    xexpr_init(e, XEXPR_VOID, -1);
}

void xexpr_init_int(XrExprDesc *e, int64_t val) {
    xexpr_init(e, XEXPR_INT, -1);
    e->u.ival = val;
    e->is_const = true;
    e->compile_type = xr_type_new_int(NULL);
}

void xexpr_init_number(XrExprDesc *e, double val) {
    xexpr_init(e, XEXPR_FLOAT, -1);
    e->u.nval = val;
    e->is_const = true;
    e->compile_type = xr_type_new_float(NULL);
}

void xexpr_init_bool(XrExprDesc *e, bool val) {
    xexpr_init(e, val ? XEXPR_TRUE : XEXPR_FALSE, -1);
    e->is_const = true;
}

void xexpr_init_null(XrExprDesc *e) {
    xexpr_init(e, XEXPR_NULL, -1);
    e->is_const = true;
}

// ========== Core Smart Allocation Functions ==========

/*
 * Put expression into any register (DYNAMIC consumer).
 *
 * This is the typed→dynamic boundary: if the expression holds a raw
 * I64/F64 value, it is automatically BOXed into a tagged XrValue.
 * All generic/dynamic consumers (template strings, closures, generic
 * OP_ADD, OP_PRINT without hint, etc.) should use this function.
 */
int xexpr_to_anyreg(XrCompilerContext *ctx, XrCompiler *compiler, XrExprDesc *e) {
    XR_DCHECK(ctx != NULL, "xexpr_to_anyreg: NULL ctx");
    XR_DCHECK(compiler != NULL, "xexpr_to_anyreg: NULL compiler");
    XR_DCHECK(e != NULL, "xexpr_to_anyreg: NULL expr desc");
    xexpr_discharge(ctx, compiler, e);
    
    if (xexpr_in_register(e)) {
        if (!e->has_jumps) {
            if (e->kind == XEXPR_TEMP || e->kind == XEXPR_CALL) {
                // Auto-BOX raw temps at the boundary
                if (xexpr_is_raw_i64(e)) {
                    int dst = reg_alloc(ctx, compiler);
                    emit_abc(compiler->emitter, OP_BOX_I64, dst, e->reg, 0);
                    e->kind = XEXPR_TEMP;
                    e->reg = dst;
                    xexpr_clear_raw(e);
                    return dst;
                } else if (xexpr_is_raw_f64(e)) {
                    int dst = reg_alloc(ctx, compiler);
                    emit_abc(compiler->emitter, OP_BOX_F64, dst, e->reg, 0);
                    e->kind = XEXPR_TEMP;
                    e->reg = dst;
                    xexpr_clear_raw(e);
                    return dst;
                }
                return e->reg;
            }
            // LOCAL/PARAM: don't reuse, fall through to nextreg
        }
    }
    
    // Allocate new register (discharge2reg uses MOVE, preserves raw)
    int reg = xexpr_to_nextreg(ctx, compiler, e);
    
    // BOX at the boundary if raw-typed
    if (xexpr_is_raw_i64(e)) {
        emit_abc(compiler->emitter, OP_BOX_I64, reg, reg, 0);
        xexpr_clear_raw(e);
    } else if (xexpr_is_raw_f64(e)) {
        emit_abc(compiler->emitter, OP_BOX_F64, reg, reg, 0);
        xexpr_clear_raw(e);
    }
    return reg;
}

/*
 * Put expression into any register (TYPED consumer, readonly).
 *
 * Preserves raw format — no BOX. Returns the original register for
 * LOCAL/PARAM (zero overhead). Used by native _I64/_F64 instructions,
 * comparisons with native paths, etc.
 */
int xexpr_to_anyreg_readonly(XrCompilerContext *ctx, XrCompiler *compiler, XrExprDesc *e) {
    xexpr_discharge(ctx, compiler, e);
    
    if (xexpr_in_register(e)) {
        return e->reg;
    }
    
    return xexpr_to_nextreg(ctx, compiler, e);
}

/*
 * Allocate to next register
 */
int xexpr_to_nextreg(XrCompilerContext *ctx, XrCompiler *compiler, XrExprDesc *e) {
    XR_DCHECK(ctx != NULL, "xexpr_to_nextreg: NULL ctx");
    XR_DCHECK(compiler != NULL, "xexpr_to_nextreg: NULL compiler");
    XR_DCHECK(e != NULL, "xexpr_to_nextreg: NULL expr desc");
    // Ensure expression is discharged
    xexpr_discharge(ctx, compiler, e);
    
    // Free old temporary resources
    xexpr_free(compiler, e);
    
    // Allocate new register
    int reg = reg_alloc(ctx, compiler);
    
    // Move expression to new register
    xexpr_to_specific_reg(ctx, compiler, e, reg);
    
    return reg;
}

/*
 * Ensure expression is in a register
 */
void xexpr_to_reg(XrCompilerContext *ctx, XrCompiler *compiler, XrExprDesc *e) {
    XR_DCHECK(ctx != NULL, "xexpr_to_reg: NULL ctx");
    XR_DCHECK(compiler != NULL, "xexpr_to_reg: NULL compiler");
    XR_DCHECK(e != NULL, "xexpr_to_reg: NULL expr desc");
    if (!xexpr_in_register(e)) {
        xexpr_to_nextreg(ctx, compiler, e);
    }
}

/*
 * Core function: discharge expression to specific register
 * 
 * This is the core mechanism for relocatable expressions:
 * - For constants: generate LOAD instruction to target_reg
 * - For XEXPR_RELOC: writeback modify A field of generated instruction
 * - For values already in register: generate MOVE (or use directly)
 */
static void xexpr_discharge2reg(XrCompilerContext *ctx, XrCompiler *compiler, 
                                XrExprDesc *e, int target_reg) {
    (void)ctx;  // Unused, but keep interface
    
    switch (e->kind) {
        // ===== Constants: directly generate LOAD instruction =====
        case XEXPR_NULL:
            emit_loadnull(compiler->emitter, target_reg);
            break;
            
        case XEXPR_TRUE:
            emit_loadtrue(compiler->emitter, target_reg);
            break;
            
        case XEXPR_FALSE:
            emit_loadfalse(compiler->emitter, target_reg);
            break;
            
        case XEXPR_INT: {
            int64_t ival = e->u.ival;

            if (ival >= -MAXARG_sBx && ival <= MAXARG_sBx) {
                emit_asbx(compiler->emitter, OP_LOADI, target_reg, (int)ival);
            } else {
                int kidx = xr_vm_proto_add_constant(compiler->proto, xr_int(ival));
                emit_loadk(compiler->emitter, target_reg, kidx);
            }
            break;
        }
            
        case XEXPR_FLOAT: {
            // Float constant: add to constant table
            int kidx = xr_vm_proto_add_constant(compiler->proto, xr_float(e->u.nval));
            emit_loadk(compiler->emitter, target_reg, kidx);
            break;
        }
            
        case XEXPR_NUMBER:
            // Number already in constant table
            emit_loadk(compiler->emitter, target_reg, e->u.const_idx);
            break;
            
        case XEXPR_STRING:
            emit_loadk(compiler->emitter, target_reg, e->u.const_idx);
            break;
            
        // ===== Relocatable expression: writeback modify instruction! =====
        case XEXPR_RELOC: {
            /*
             * This is key optimization: directly modify target register of generated instruction
             * Example: ADD R[0] R[1] R[2] -> ADD R[5] R[1] R[2]
             * Avoid generating extra MOVE R[5] R[0]
             */
            emit_patch_instruction_A(compiler->emitter, e->u.pc, target_reg);
            break;
        }
        
        // ===== Global variable: reload =====
        case XEXPR_GLOBAL:
            emit_abx(compiler->emitter, OP_GETBUILTIN, target_reg, e->u.global_idx);
            break;
            
        // ===== Already in register: MOVE (preserves raw format) =====
        case XEXPR_TEMP:
        case XEXPR_LOCAL:
        case XEXPR_PARAM:
        case XEXPR_CALL:
            if (e->reg != target_reg) {
                emit_move(compiler->emitter, target_reg, e->reg);
                // compile_type preserved — caller decides whether to BOX
            }
            break;
            
        // ===== Other: error =====
        case XEXPR_VOID:
            // void expression should not be discharged
            xr_log_warning("compiler", "cannot discharge VOID expression");
            break;
            
        default:
            xr_log_warning("compiler", "cannot discharge expression kind %d", e->kind);
            break;
    }
    
    // Record per-PC type annotation for flow-sensitive inst_types
    if (e->compile_type && target_reg >= 0 && target_reg < 256) {
        int last_pc = compiler->emitter->window.last_pc;
        if (last_pc >= 0)
            compiler_record_inst_type(compiler, last_pc, e->compile_type);
    }
    
    // Update expression state: becomes temporary after discharge
    e->kind = XEXPR_TEMP;
    e->reg = target_reg;
    e->has_jumps = false;  // Clear jumps after discharge
}

/*
 * Put expression into specific register (public interface)
 */
void xexpr_to_specific_reg(XrCompilerContext *ctx, XrCompiler *compiler, 
                           XrExprDesc *e, int target_reg) {
    // Directly call discharge2reg
    xexpr_discharge2reg(ctx, compiler, e, target_reg);
    
    // Jump expression materialization: convert bool jump chains to concrete true/false values.
    // When short-circuit expressions (&&, ||, !) produce a value (not just branch),
    // we must generate LOADTRUE/LOADFALSE at the jump targets.
    if (e->t >= 0 || e->f >= 0) {
        XrEmitter *em = compiler->emitter;
        int current_pc = emit_get_current_pc(em);
        
        // Fall-through path: value already in target_reg from discharge2reg.
        // Jump over the materialization code.
        int skip_jmp = emit_sj(em, OP_JMP, 0);  // patched below
        
        // False materialization point: patch false jump list here
        int false_pc = emit_get_current_pc(em);
        emit_abc(em, OP_LOADFALSE, target_reg, 0, 0);
        int skip_after_false = emit_sj(em, OP_JMP, 0);  // skip past LOADTRUE
        
        // True materialization point: patch true jump list here
        int true_pc = emit_get_current_pc(em);
        emit_abc(em, OP_LOADTRUE, target_reg, 0, 0);
        
        // Patch: skip_jmp jumps past both LOADFALSE and LOADTRUE
        int end_pc = emit_get_current_pc(em);
        patch_jump(em, skip_jmp, end_pc);
        patch_jump(em, skip_after_false, end_pc);
        
        // Patch jump lists to their materialization points
        if (e->f >= 0) patch_jump_list(em, e->f, false_pc);
        if (e->t >= 0) patch_jump_list(em, e->t, true_pc);
        
        e->t = -1;
        e->f = -1;
        e->has_jumps = false;
        (void)current_pc;
    }
}

// ========== Helper Functions ==========

/*
 * Check if expression is relocatable
 */
bool xexpr_is_relocatable(const XrExprDesc *e) {
    return e->kind == XEXPR_RELOC;
}

// ========== Expression Analysis ==========

bool xexpr_can_reuse_reg(const XrExprDesc *e) {
    if (e->has_jumps) {
        return false;  // Has jumps, not safe
    }
    
    switch (e->kind) {
        case XEXPR_TEMP:
        case XEXPR_CALL:
            // Temporary values and function call return values can be reused
            return true;
        
        case XEXPR_LOCAL:
        case XEXPR_PARAM:
            // Local variables need protection
            return false;
        
        default:
            return false;
    }
}

bool xexpr_needs_protect(const XrExprDesc *e) {
    return (e->kind == XEXPR_LOCAL || e->kind == XEXPR_PARAM);
}

bool xexpr_in_register(const XrExprDesc *e) {
    switch (e->kind) {
        case XEXPR_TEMP:
        case XEXPR_LOCAL:
        case XEXPR_PARAM:
        case XEXPR_CALL:
            return e->reg >= 0;
        default:
            return false;
    }
}

bool xexpr_has_jumps(const XrExprDesc *e) {
    return e->has_jumps || e->t >= 0 || e->f >= 0;
}

// ========== Resource Management ==========

void xexpr_free(XrCompiler *compiler, XrExprDesc *e) {
    // Only free temporary registers
    if (e->kind == XEXPR_TEMP && e->reg >= 0) {
        // Note: reg_free internally checks register type
        reg_free(compiler, e->reg);
    }
    // Local variable and parameter registers managed by scope system, not freed here
}

void xexpr_discharge(XrCompilerContext *ctx, XrCompiler *compiler, XrExprDesc *e) {
    /*
     * Simplified version
     * 
     * XEXPR_RELOC doesn't need discharge here,
     * Real discharge happens in discharge2reg through instruction writeback
     * 
     * This function mainly ensures expression is "available",
     * but not necessarily materialized to register
     */
    (void)ctx;
    (void)compiler;
    
    switch (e->kind) {
        case XEXPR_RELOC:
            /*
             * Relocatable expression still keeps RELOC state
             * Until discharge2reg writes back the instruction
             */
            break;
            
        case XEXPR_JMP:
            // Expression with jumps
            // TODO Implement jump materialization (short-circuit evaluation optimization)
            break;
            
        case XEXPR_INDEXED:
            // TODO Implement indexed expression materialization
            break;
            
        default:
            // Constants, values already in registers, etc. don't need discharge
            break;
    }
}

// ========== Helper Functions ==========

const char* xexpr_kind_name(XExprKind kind) {
    switch (kind) {
        case XEXPR_VOID:     return "VOID";
        case XEXPR_NULL:     return "NULL";
        case XEXPR_TRUE:     return "TRUE";
        case XEXPR_FALSE:    return "FALSE";
        case XEXPR_INT:      return "INT";
        case XEXPR_FLOAT:    return "FLOAT";
        case XEXPR_NUMBER:   return "NUMBER";
        case XEXPR_STRING:   return "STRING";
        case XEXPR_TEMP:     return "TEMP";
        case XEXPR_LOCAL:    return "LOCAL";
        case XEXPR_PARAM:    return "PARAM";
        case XEXPR_GLOBAL:   return "GLOBAL";
        case XEXPR_UPVAL:    return "UPVAL";
        case XEXPR_INDEXED:  return "INDEXED";
        case XEXPR_CALL:     return "CALL";
        case XEXPR_JMP:      return "JMP";
        case XEXPR_RELOC:    return "RELOC";
        default:             return "UNKNOWN";
    }
}

void xexpr_debug_print(const XrExprDesc *e) {
    printf("[XrExprDesc] kind=%s reg=%d has_jumps=%d is_const=%d",
           xexpr_kind_name(e->kind), e->reg, e->has_jumps, e->is_const);
    
    if (e->kind == XEXPR_INT) {
        printf(" ival=%lld", (long long)e->u.ival);
    } else if (e->kind == XEXPR_FLOAT) {
        printf(" nval=%f", e->u.nval);
    }
    
    printf("\n");
}

void xexpr_copy(XrExprDesc *dest, const XrExprDesc *src) {
    memcpy(dest, src, sizeof(XrExprDesc));
}

// ========== Conditional Jumps ==========

/*
 * Jump when condition is false
 * 
 * Note: Use xexpr_to_nextreg to force allocate new register,
 * Ensure returned register is freereg-1, compatible with reg_free
 */
// ========== Type Conversion ==========

int xexpr_ensure_boxed(XrCompilerContext *ctx, XrCompiler *compiler,
                       XrExprDesc *e, int reg) {
    if (xexpr_is_raw_i64(e)) {
        int temp = reg_alloc(ctx, compiler);
        emit_abc(compiler->emitter, OP_BOX_I64, temp, reg, 0);
        return temp;
    } else if (xexpr_is_raw_f64(e)) {
        int temp = reg_alloc(ctx, compiler);
        emit_abc(compiler->emitter, OP_BOX_F64, temp, reg, 0);
        return temp;
    }
    return reg;
}

// ========== Conditional Jumps ==========

int xexpr_goiftrue(XrCompilerContext *ctx, XrCompiler *compiler, XrExprDesc *e) {
    // 1. Force allocate new register (ensure it's freereg-1, easy to free)
    int reg = xexpr_to_nextreg(ctx, compiler, e);
    
    // 2. Generate TEST instruction: if reg is false, then jump
    emit_abc(compiler->emitter, OP_TEST, reg, 0, 0);
    
    // 3. Generate JMP instruction
    int jump_pc = emit_jump(compiler->emitter, OP_JMP);
    
    // 4. Free register (reg_free handles correctly internally)
    reg_free(compiler, reg);
    
    return jump_pc;
}

