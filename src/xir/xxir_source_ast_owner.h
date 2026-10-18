/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_ast_owner.h - Shared immutable parser input for Source history
 */
#ifndef XXIR_SOURCE_AST_OWNER_H
#define XXIR_SOURCE_AST_OWNER_H
#include "xxir_source.h"
/* The descriptor borrows until prepare returns. Retain must pin the complete
 * parser Program, source lexemes and original state; release drops that pin.
 * The AST remains immutable while any retained record exists. The real parser
 * arena is still admitted independently; a claimed ledger grants no authority. */
typedef struct XrXirSourceAstOwner {
    XrXirSourceAst input;
    XrCompileResources *resources;
    void *owner;
    XrXirStatus (*retain)(void *owner,const XrXirSourceAst *input);
    void (*release)(void *owner);
} XrXirSourceAstOwner;
#endif // XXIR_SOURCE_AST_OWNER_H
