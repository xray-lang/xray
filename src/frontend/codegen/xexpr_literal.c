/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xexpr_literal.c - Xray literal expression compilation
 *
 * KEY CONCEPT:
 *   - null
 *   - boolean (true/false)
 *   - integer
 *   - float
 *   - string
 */

#include "xexpr.h"
#include "../../base/xchecks.h"
#include "xcompiler.h"
#include "xcompiler_context.h"
#include "xemit.h"
#include "xexpr_desc.h"
#include "../../runtime/object/xbigint.h"
#include "../../runtime/xisolate_api.h"
#include "../../runtime/value/xtype.h"
#include <stdio.h>
#include <string.h>  // strlen

// ========== Literal Compilation ==========

/*
 * Compile literal expression (returns XrExprDesc)
 * 
 * Optimization strategy:
 * - null/bool: use dedicated instructions (LOADNULL/LOADTRUE/LOADFALSE)
 * - small integer: use immediate instruction (LOADI)
 * - large integer/float/string: use constant pool (LOADK)
 * 
 * All literals return XEXPR_RELOC, supporting relocatability
 * 
 * @return Expression descriptor
 */
XrExprDesc compile_literal(XrCompilerContext *ctx, XrCompiler *compiler, LiteralNode *node) {
    XR_DCHECK(ctx != NULL, "compile_literal: NULL ctx");
    XR_DCHECK(compiler != NULL, "compile_literal: NULL compiler");
    XR_DCHECK(node != NULL, "compile_literal: NULL node");
    XrExprDesc e = {0};
    xexpr_init(&e, XEXPR_VOID, -1);
    int pc;
    
    // Use new LiteralKind enum
    switch (node->kind) {
        case LITERAL_KIND_NULL:
            // null constant: A=0 pending relocation
            pc = emit_abc(compiler->emitter, OP_LOADNULL, 0, 0, 0);
            e.kind = XEXPR_RELOC;
            e.u.pc = pc;
            break;
        
        case LITERAL_KIND_BOOL:
            // boolean constant: A=0 pending relocation
            if (node->raw_value.bool_val) {
                pc = emit_abc(compiler->emitter, OP_LOADTRUE, 0, 0, 0);
            } else {
                pc = emit_abc(compiler->emitter, OP_LOADFALSE, 0, 0, 0);
            }
            e.kind = XEXPR_RELOC;
            e.u.pc = pc;
            e.compile_type = xr_type_new_bool();
            break;
        
        case LITERAL_KIND_INT: {
            int64_t ival = node->raw_value.int_val;
            
            if (ival >= -MAXARG_sBx && ival <= MAXARG_sBx) {
                // Small integer: LOADI (AsBx format)
                pc = emit_asbx(compiler->emitter, OP_LOADI, 0, (int)ival);
                e.kind = XEXPR_RELOC;
                e.u.pc = pc;
            } else {
                // Large integer: constant table (full 64-bit range)
                XrValue val = xr_int(ival);
                int kidx = xr_vm_proto_add_constant(compiler->proto, val);
                pc = emit_abx(compiler->emitter, OP_LOADK, 0, kidx);
                e.kind = XEXPR_RELOC;
                e.u.pc = pc;
            }
            e.compile_type = xr_type_new_int();
            break;
        }
        
        case LITERAL_KIND_FLOAT: {
            // float: A=0 pending relocation
            XrValue val = xr_float(node->raw_value.float_val);
            int kidx = xr_vm_proto_add_constant(compiler->proto, val);
            pc = emit_abx(compiler->emitter, OP_LOADK, 0, kidx);
            e.kind = XEXPR_RELOC;
            e.u.pc = pc;
            e.compile_type = xr_type_new_float();
            break;
        }
        
        case LITERAL_KIND_BIGINT: {
            // BigInt: compile-time constant, allocate on global GC
            const char *str = node->raw_value.bigint_val;
            XrBigInt *bigint = xr_bigint_from_string_on_gc(xr_isolate_get_gc(ctx->X), str);
            XrValue val = XR_FROM_PTR(bigint);
            int kidx = xr_vm_proto_add_constant(compiler->proto, val);
            pc = emit_abx(compiler->emitter, OP_LOADK, 0, kidx);
            e.kind = XEXPR_RELOC;
            e.u.pc = pc;
            break;
        }
        
        case LITERAL_KIND_STRING: {
            /*
             * String: A=0 pending relocation
             * Use compile-time interning to ensure same content shares same object,
             * Map key comparison can use pointer comparison
             */
            const char *str = node->raw_value.string_val;
            XrString *xstr = xr_compile_time_intern(ctx->X, str, strlen(str));
            XrValue val = xr_string_value(xstr);
            int kidx = xr_vm_proto_add_constant(compiler->proto, val);
            pc = emit_abx(compiler->emitter, OP_LOADK, 0, kidx);
            e.kind = XEXPR_RELOC;
            e.u.pc = pc;
            e.compile_type = xr_type_new_string();
            break;
        }
        
        case LITERAL_KIND_REGEX: {
            /*
             * Regex literal: compile to OP_REGEX_COMPILE instruction
             * Format: OP_REGEX_COMPILE A B C
             *   A = destination register (pending relocation)
             *   B = pattern constant index
             *   C = flags constant index
             */
            const char *pattern = node->raw_value.regex.pattern;
            const char *flags = node->raw_value.regex.flags;
            
            // Add pattern and flags to constant pool
            XrString *pattern_str = xr_compile_time_intern(ctx->X, pattern, strlen(pattern));
            XrString *flags_str = xr_compile_time_intern(ctx->X, flags, strlen(flags));
            
            int pattern_idx = xr_vm_proto_add_constant(compiler->proto, xr_string_value(pattern_str));
            int flags_idx = xr_vm_proto_add_constant(compiler->proto, xr_string_value(flags_str));
            
            pc = emit_abc(compiler->emitter, OP_REGEX_COMPILE, 0, pattern_idx, flags_idx);
            e.kind = XEXPR_RELOC;
            e.u.pc = pc;
            break;
        }
        
        default:
            fprintf(stderr, "[Literal] Unsupported literal kind: %d\n", node->kind);
            xr_compiler_error(ctx, compiler, "Unsupported literal kind");
            // Error case: return VOID
            e.kind = XEXPR_VOID;
            break;
    }
    
    return e;
}

