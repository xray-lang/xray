/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xstmt_exception.c - Xray exception handling statement compilation
 *
 * KEY CONCEPT:
 *   - try-catch-finally statements
 *   - throw statements
 */

#include "xstmt.h"
#include "../../base/xchecks.h"
#include "xcompiler.h"
#include "xcompiler_context.h"
#include "xemit.h"
#include "xexpr.h"
#include "xexpr_desc.h"
#include "xcompiler_scope.h"
#include "../../runtime/value/xtype.h"
#include <stdio.h>
#include <string.h>

// ========== Try-Catch-Finally ==========

/*
 * Compile try-catch-finally statement
 * 
 * try { ... } catch (e) { ... } finally { ... }
 * 
 * Generated code:
 *   OP_TRY catch_offset
 *   OP_NOP finally_offset  (placeholder)
 *   <try_body>
 *   JMP end
 * catch_start:
 *   OP_CATCH exception_var
 *   <catch_body>
 * finally_start:
 *   OP_FINALLY
 *   <finally_body>
 * end:
 *   OP_END_TRY
 */
void compile_try_catch(XrCompilerContext *ctx, XrCompiler *compiler, TryCatchNode *node) {
    XR_DCHECK(ctx != NULL, "compile_try_catch: NULL ctx");
    XR_DCHECK(compiler != NULL, "compile_try_catch: NULL compiler");
    XR_DCHECK(node != NULL, "compile_try_catch: NULL node");
    // 1. Emit OP_TRY instruction (catch offset)
    int try_offset = PROTO_CODE_COUNT(compiler->proto);
    emit_abx(compiler->emitter, OP_TRY, 0, 0);  // catch offset patched later
    
    // Emit finally offset (placeholder)
    int finally_placeholder = PROTO_CODE_COUNT(compiler->proto);
    emit_abc(compiler->emitter, OP_NOP, 0, 0, 0);
    
    // 2. Compile try block
    scope_begin(compiler);
    
    if (node->try_body && node->try_body->type == AST_BLOCK) {
        BlockNode *block = &node->try_body->as.block;
        for (int i = 0; i < block->count; i++) {
            xr_compile_statement(ctx, compiler, block->statements[i]);
        }
    }
    
    scope_end(ctx, compiler);
    
    // 3. Try block ends normally, skip catch and finally
    int end_jump = emit_jump(compiler->emitter, OP_JMP);
    
    // 4. Catch block (if exists)
    if (node->catch_body) {
        int catch_start = PROTO_CODE_COUNT(compiler->proto);
        
        // Patch catch offset
        PROTO_SET_CODE(compiler->proto, try_offset, CREATE_ABx(OP_TRY, 0, catch_start));
        
        // Begin new scope for catch variable
        scope_begin(compiler);
        
        // Define catch variable
        int catch_var_reg = 0;
        if (node->catch_var) {
            XrString *var_name = xr_compile_time_intern(ctx->X, node->catch_var, strlen(node->catch_var));
            XrLocalInfo *local = scope_define_local(ctx, compiler, var_name);
            local_set_compile_type(local, xr_type_new_named_instance(ctx->X, "Exception"));
            catch_var_reg = local->reg;
        }
        
        // Emit OP_CATCH instruction
        emit_abc(compiler->emitter, OP_CATCH, catch_var_reg, 0, 0);
        
        // Compile catch block content
        if (node->catch_body->type == AST_BLOCK) {
            BlockNode *block = &node->catch_body->as.block;
            for (int i = 0; i < block->count; i++) {
                xr_compile_statement(ctx, compiler, block->statements[i]);
            }
        }
        
        scope_end(ctx, compiler);
    }
    
    // 5. Finally block (if exists)
    if (node->finally_body) {
        int finally_start = PROTO_CODE_COUNT(compiler->proto);
        
        // Patch finally offset
        PROTO_SET_CODE(compiler->proto, finally_placeholder, CREATE_ABx(OP_NOP, 0, finally_start));
        
        // Patch end jump: jump to finally start (normal flow needs to execute finally)
        patch_jump(compiler->emitter, end_jump, -1);
        
        // Emit OP_FINALLY instruction
        emit_abc(compiler->emitter, OP_FINALLY, 0, 0, 0);
        
        // Compile finally block content
        scope_begin(compiler);
        if (node->finally_body->type == AST_BLOCK) {
            BlockNode *block = &node->finally_body->as.block;
            for (int i = 0; i < block->count; i++) {
                xr_compile_statement(ctx, compiler, block->statements[i]);
            }
        }
        scope_end(ctx, compiler);
    } else {
        // No finally: patch end jump to END_TRY
        patch_jump(compiler->emitter, end_jump, -1);
    }
    
    // 7. Emit OP_END_TRY
    emit_abc(compiler->emitter, OP_END_TRY, 0, 0, 0);
}

// ========== Throw Statement ==========

/*
 * Compile throw statement
 * 
 * throw expr -> OP_THROW expr_reg
 */
void compile_throw(XrCompilerContext *ctx, XrCompiler *compiler, ThrowStmtNode *node) {
    XR_DCHECK(ctx != NULL, "compile_throw: NULL ctx");
    XR_DCHECK(compiler != NULL, "compile_throw: NULL compiler");
    XR_DCHECK(node != NULL, "compile_throw: NULL node");
    // Compile expression to throw
    XrExprDesc expr = xr_compile_expr(ctx, compiler, node->expression);
    int expr_reg = xexpr_to_anyreg(ctx, compiler, &expr);
    
    // Emit OP_THROW instruction
    emit_abc(compiler->emitter, OP_THROW, expr_reg, 0, 0);
    
    reg_free(compiler, expr_reg);
}

