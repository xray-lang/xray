/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xstmt.h - Statement compiler
 */

#ifndef XSTMT_H
#define XSTMT_H

#include "../parser/xast.h"
#include "xcompiler.h"
#include "xemit.h"
#include "../../base/xdefs.h"

// Saved loop state for nested loop support
typedef struct {
    int loop_depth;
    int loop_start;
    int break_count;
    int continue_count;
} XrLoopState;

static inline void loop_state_save(XrCompiler *compiler, XrLoopState *state) {
    state->loop_depth = compiler->loop_depth;
    state->loop_start = compiler->loop_start;
    state->break_count = compiler->break_count;
    state->continue_count = compiler->continue_count;
}

static inline void loop_state_enter(XrCompiler *compiler, int loop_start) {
    compiler->loop_depth++;
    compiler->loop_start = loop_start;
}

static inline void loop_state_patch_continue(XrCompiler *compiler, XrLoopState *state, int target) {
    for (int i = state->continue_count; i < compiler->continue_count; i++) {
        patch_jump(compiler->emitter, compiler->continue_jumps[i], target);
    }
    compiler->continue_count = state->continue_count;
}

static inline void loop_state_patch_break(XrCompiler *compiler, XrLoopState *state, int target) {
    for (int i = state->break_count; i < compiler->break_count; i++) {
        patch_jump(compiler->emitter, compiler->break_jumps[i], target);
    }
    compiler->break_count = state->break_count;
}

static inline void loop_state_restore(XrCompiler *compiler, XrLoopState *state) {
    compiler->loop_depth = state->loop_depth;
    compiler->loop_start = state->loop_start;
}

XR_FUNC void compile_expr_stmt(XrCompilerContext *ctx, XrCompiler *c, AstNode *expr);
XR_FUNC void compile_var_decl(XrCompilerContext *ctx, XrCompiler *c, VarDeclNode *node);
XR_FUNC void compile_destructure_decl(XrCompilerContext *ctx, XrCompiler *c, DestructureDeclNode *node);
XR_FUNC void compile_destructure_assign(XrCompilerContext *ctx, XrCompiler *c, DestructureAssignNode *node);
XR_FUNC void compile_multi_var_decl(XrCompilerContext *ctx, XrCompiler *c, MultiVarDeclNode *node);
XR_FUNC void compile_multi_assign(XrCompilerContext *ctx, XrCompiler *c, MultiAssignNode *node);
XR_FUNC void compile_print(XrCompilerContext *ctx, XrCompiler *c, PrintNode *node);
XR_FUNC void compile_assignment(XrCompilerContext *ctx, XrCompiler *c, AssignmentNode *node);
XR_FUNC void compile_compound_assignment(XrCompilerContext *ctx, XrCompiler *c, CompoundAssignmentNode *node);
XR_FUNC void compile_inc(XrCompilerContext *ctx, XrCompiler *c, IncDecNode *node);
XR_FUNC void compile_dec(XrCompilerContext *ctx, XrCompiler *c, IncDecNode *node);

XR_FUNC void compile_while(XrCompilerContext *ctx, XrCompiler *c, WhileStmtNode *node);
XR_FUNC void compile_for(XrCompilerContext *ctx, XrCompiler *c, ForStmtNode *node);
XR_FUNC void compile_for_in(XrCompilerContext *ctx, XrCompiler *c, ForInStmtNode *node);
XR_FUNC void compile_return(XrCompilerContext *ctx, XrCompiler *c, ReturnStmtNode *node);
XR_FUNC void compile_if(XrCompilerContext *ctx, XrCompiler *c, IfStmtNode *node);

XR_FUNC void compile_index_set(XrCompilerContext *ctx, XrCompiler *c, IndexSetNode *node);
XR_FUNC void compile_member_set(XrCompilerContext *ctx, XrCompiler *c, MemberSetNode *node);

XR_FUNC void compile_function(XrCompilerContext *ctx, XrCompiler *c, FunctionDeclNode *node);
XR_FUNC void compile_function_decl_only(XrCompilerContext *ctx, XrCompiler *c, FunctionDeclNode *node);

XR_FUNC void compile_try_catch(XrCompilerContext *ctx, XrCompiler *c, TryCatchNode *node);
XR_FUNC void compile_throw(XrCompilerContext *ctx, XrCompiler *c, ThrowStmtNode *node);

XR_FUNC void compile_import(XrCompilerContext *ctx, XrCompiler *c, ImportStmtNode *node);
XR_FUNC void compile_export(XrCompilerContext *ctx, XrCompiler *c, ExportStmtNode *node);

XR_FUNC void compile_go_expr(XrCompilerContext *ctx, XrCompiler *c, GoExprNode *node, int target, bool fire_and_forget);
XR_FUNC void compile_await_expr(XrCompilerContext *ctx, XrCompiler *c, AwaitExprNode *node, int target);
XR_FUNC void compile_channel_new(XrCompilerContext *ctx, XrCompiler *c, ChannelNewNode *node, int target);
XR_FUNC void compile_defer_stmt(XrCompilerContext *ctx, XrCompiler *c, DeferStmtNode *node);
XR_FUNC void compile_select_stmt(XrCompilerContext *ctx, XrCompiler *c, SelectStmtNode *node);
XR_FUNC void compile_scope_block(XrCompilerContext *ctx, XrCompiler *c, ScopeBlockNode *node, int target);
XR_FUNC void compile_cancelled_expr(XrCompilerContext *ctx, XrCompiler *c, int target);

#endif // XSTMT_H
