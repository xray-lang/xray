/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xstmt.c - Statement compiler dispatcher
 *
 * KEY CONCEPT:
 *   Central dispatch for all statement types. Routes each AST node
 *   to its corresponding compilation function.
 */

#include "xstmt.h"
#include "../../base/xlog.h"
#include "../../base/xchecks.h"
#include "xcompiler.h"
#include "xcompiler_context.h"
#include "xemit.h"
#include "xregalloc.h"
#include <stdio.h>

void compile_statement(XrCompilerContext *ctx, XrCompiler *c, AstNode *node) {
    XR_DCHECK(ctx != NULL, "compile_statement: NULL ctx");
    XR_DCHECK(c != NULL, "compile_statement: NULL compiler");
    if (!node) {
        return;
    }
    
    switch (node->type) {
        // Simple statements
        case AST_EXPR_STMT:
            compile_expr_stmt(ctx, c, node->as.expr_stmt);
            break;
        
        case AST_VAR_DECL:
            compile_var_decl(ctx, c, &node->as.var_decl);
            break;
        
        case AST_DESTRUCTURE_DECL:
            compile_destructure_decl(ctx, c, &node->as.destructure_decl);
            break;
        
        case AST_MULTI_VAR_DECL:
            compile_multi_var_decl(ctx, c, &node->as.multi_var_decl);
            break;
        
        case AST_MULTI_ASSIGN:
            compile_multi_assign(ctx, c, &node->as.multi_assign);
            break;
        
        case AST_PRINT_STMT:
            compile_print(ctx, c, &node->as.print_stmt);
            break;
        
        // Assignment statements
        case AST_ASSIGNMENT:
            compile_assignment(ctx, c, &node->as.assignment);
            break;
        
        case AST_COMPOUND_ASSIGNMENT:
            compile_compound_assignment(ctx, c, &node->as.compound_assignment);
            break;
        
        case AST_INC:
            compile_inc(ctx, c, &node->as.inc);
            break;
        
        case AST_DEC:
            compile_dec(ctx, c, &node->as.dec);
            break;
        
        // Control flow statements
        case AST_IF_STMT:
            compile_if(ctx, c, &node->as.if_stmt);
            break;
        
        case AST_WHILE_STMT:
            compile_while(ctx, c, &node->as.while_stmt);
            break;
        
        case AST_FOR_STMT:
            compile_for(ctx, c, &node->as.for_stmt);
            break;
        
        case AST_RETURN_STMT:
            compile_return(ctx, c, &node->as.return_stmt);
            break;
        
        case AST_BREAK_STMT:
            if (c->loop_depth == 0) {
                xr_compiler_error(ctx, c, "Cannot use 'break' outside of loop");
            } else {
                int jump_pos = emit_jump(c->emitter, OP_JMP);
                if (c->break_count < 256) {
                    c->break_jumps[c->break_count++] = jump_pos;
                }
            }
            break;
        
        case AST_CONTINUE_STMT:
            if (c->loop_depth == 0) {
                xr_compiler_error(ctx, c, "Cannot use 'continue' outside of loop");
            } else {
                int jump_pos = emit_jump(c->emitter, OP_JMP);
                if (c->continue_count < 256) {
                    c->continue_jumps[c->continue_count++] = jump_pos;
                }
            }
            break;
        
        // Advanced assignment statements
        case AST_INDEX_SET:
            compile_index_set(ctx, c, &node->as.index_set);
            break;
        
        case AST_MEMBER_SET:
            compile_member_set(ctx, c, &node->as.member_set);
            break;
        
        // Function definition
        case AST_FUNCTION_DECL:
            compile_function(ctx, c, &node->as.function_decl);
            break;
        
        // Exception handling
        case AST_TRY_CATCH:
            compile_try_catch(ctx, c, &node->as.try_catch);
            break;
        
        case AST_THROW_STMT:
            compile_throw(ctx, c, &node->as.throw_stmt);
            break;
        
        // Module system
        case AST_IMPORT_STMT:
            compile_import(ctx, c, &node->as.import_stmt);
            break;
        
        case AST_EXPORT_STMT:
            compile_export(ctx, c, &node->as.export_stmt);
            break;
        
        // Coroutine statements
        case AST_DEFER_STMT:
            compile_defer_stmt(ctx, c, &node->as.defer_stmt);
            break;
        
        case AST_SELECT_STMT:
            compile_select_stmt(ctx, c, &node->as.select_stmt);
            break;
        
        case AST_SCOPE_BLOCK:
            compile_scope_block(ctx, c, &node->as.scope_block, -1);
            break;
        
        case AST_YIELD_STMT:
            emit_abc(c->emitter, OP_YIELD, 0, 0, 0);
            break;
        
        default:
            xr_log_debug("compiler", "statement type %d not yet extracted to stmt/ module", node->type);
            xr_log_debug("compiler", "falling back to xr_compile_statement in xcompiler.c");
            break;
    }
}

