/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdap_expression_member_tokens.inc.c - Metered token spans and collision-free bindings
 */
#include "../../toolchain/xcompiler_arena_backing.h"
#include "../../xir/xxir_compile_domain.h"
#include "../../xir/xxir_type_arena.h"
#include "../../xir/xxir_types.h"
#include <stdio.h>
static XrXirStatus expression_member_status(XrCompileResourceStatus status) {
    return status==XR_COMPILE_RESOURCE_OK ? XR_XIR_OK : status==XR_COMPILE_RESOURCE_OUT_OF_MEMORY ?
        XR_XIR_OUT_OF_MEMORY :
        status==XR_COMPILE_RESOURCE_MEMORY_LIMIT ? XR_XIR_MEMORY_LIMIT :
        status==XR_COMPILE_RESOURCE_OVERFLOW ? XR_XIR_OVERFLOW :
        status==XR_COMPILE_RESOURCE_CANCELLED ? XR_XIR_CANCELLED :
        status==XR_COMPILE_RESOURCE_DEADLINE ? XR_XIR_DEADLINE :
        status==XR_COMPILE_RESOURCE_CLOCK_FAILURE ? XR_XIR_CLOCK_FAILURE : XR_XIR_BAD_STRUCTURE;
}
static bool expression_token_add(ExpressionState *state,const Token *token) {
    if (state->token_count==state->token_capacity) {
        uint32_t capacity=state->token_capacity ? state->token_capacity*2 : 32;
        uint64_t bytes=(uint64_t)capacity*sizeof(*state->tokens);
        if (capacity<=state->token_capacity || bytes>SIZE_MAX) { state->status=XR_XIR_OVERFLOW;return false; }
        void *memory=state->tokens;
        state->status=expression_member_status(xr_compile_resources_resize(state->request->lexical->launch->resources,&memory,(size_t)bytes));
        if (state->status!=XR_XIR_OK) return false;
        state->tokens=memory;state->token_capacity=capacity;
    }
    const char *text=state->request->text;size_t length=state->request->length;
    if (!token->start || token->start<text || token->start>text+length || token->length<=0 ||
        (size_t)token->length>length-(size_t)(token->start-text)) { state->status=XR_XIR_BAD_STRUCTURE;return false; }
    state->tokens[state->token_count++]=(ExpressionToken){(size_t)(token->start-text),(size_t)token->length,
        token->line,token->column,token->type};return true;
}
/* Token coordinates come from the same lexer as the parsed Source. A private
 * first-failure state shares the request ledger without poisoning the target. */
static bool expression_member_tokens(ExpressionState *state) {
    if (state->token_count) return true;
    XrCompileState *owner=NULL;XrArena arena={0};XrArenaBacking backing={0};Scanner scanner={0};
    state->status=expression_member_status(xr_compile_state_new(state->request->lexical->launch->resources,&owner));
    XrArenaStatus opened=XR_ARENA_OK;
    if (state->status==XR_XIR_OK) opened=xr_compiler_arena_state_backing(owner,&backing);
    if (state->status==XR_XIR_OK && opened==XR_ARENA_OK) opened=xr_arena_open(&arena,1,&backing);
    if (opened!=XR_ARENA_OK) state->status=opened==XR_ARENA_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY :
        opened==XR_ARENA_MEMORY_LIMIT ? XR_XIR_MEMORY_LIMIT :
        opened==XR_ARENA_OVERFLOW ? XR_XIR_OVERFLOW : XR_XIR_BAD_STRUCTURE;
    if (state->status==XR_XIR_OK) state->status=expression_member_status(xr_compile_scanner_open(&scanner,owner,&arena,state->request->text,NULL));
    while (state->status==XR_XIR_OK) {
        Token token=xr_scanner_scan(&scanner);
        state->status=expression_member_status(xr_compile_state_status(owner));
        if (state->status!=XR_XIR_OK || token.type==TK_EOF) break;
        if (token.type==TK_ERROR) { state->status=XR_XIR_BAD_TYPE;break; }
        if (!expression_token_add(state,&token)) break;
    }
    xr_arena_destroy(&arena);xr_compile_state_release(owner);return state->status==XR_XIR_OK;
}
static bool expression_member_token(ExpressionState *state,int line,int column,const char *name,uint32_t *output) {
    size_t length=strlen(name);
    for (uint32_t i=0;i<state->token_count;++i) {
        const ExpressionToken *token=&state->tokens[i];
        if (token->line==line && token->column==column && token->length==length &&
            !memcmp(state->request->text+token->offset,name,length)) { *output=i;return true; }
    }
    state->status=XR_XIR_BAD_STRUCTURE;return false;
}
static bool expression_member_name(ExpressionState *state,ExpressionBinding *binding) {
    for (uint32_t next=state->count;;++next) {
        char text[40];
        int length=snprintf(text,sizeof(text),"_dap_member_%u",next);
        if (length<=0 || (size_t)length>=sizeof(text)) { state->status=XR_XIR_BAD_STRUCTURE;return false; }
        bool collision=false;
        for (uint32_t i=0;i<state->token_count;++i) {
            const ExpressionToken *token=&state->tokens[i];
            if (token->length==(size_t)length && !memcmp(state->request->text+token->offset,text,(size_t)length)) collision=true;
        }
        for (uint32_t i=0;i<state->count;++i) {
            if (state->bindings[i].length==(size_t)length && !memcmp(state->bindings[i].name,text,(size_t)length)) collision=true;
        }
        if (!collision) {
            char *name=NULL;state->status=expression_member_status(xr_compile_resources_alloc(
                state->request->lexical->launch->resources,(size_t)length+1,(void **)&name));
            if (state->status!=XR_XIR_OK) return false;
            memcpy(name,text,(size_t)length+1);binding->name=name;binding->length=(size_t)length;binding->owned_name=true;return true;
        }
        if (next==UINT32_MAX) { state->status=XR_XIR_BUDGET;return false; }
    }
}
