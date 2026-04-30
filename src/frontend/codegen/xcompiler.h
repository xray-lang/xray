/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcompiler.h - Compiler public interface
 *
 * KEY CONCEPT:
 *   Compiles AST to bytecode via the Xi IR pipeline:
 *     AST -> analysis -> Xi lower -> Xi verify -> Xi opt -> Xi emit -> XrProto
 *
 * RELATED MODULES:
 *   - xcompiler_context.h: Shared compilation context
 *   - xi_pipeline.h: Xi IR pipeline orchestration
 */

#ifndef XCOMPILER_H
#define XCOMPILER_H

#include "../../runtime/value/xchunk.h"
#include "../parser/xast.h"
#include "../../runtime/object/xstring.h"
#include <stdbool.h>
#include <stdint.h>
#include "../../base/xdefs.h"

/* ========== Forward Declarations ========== */

typedef struct XrCompilerContext XrCompilerContext;

/* ========== Global Variables ========== */

#define MAX_GLOBALS 256

typedef struct XrGlobalVar {
    XrString *name;
    int index;
    bool is_const;
} XrGlobalVar;

/* Ownership state for shared let variables (Move semantics) */
typedef enum {
    SHARED_STATE_OWNED,  /* Can be used */
    SHARED_STATE_MOVED,  /* Cannot be used after Channel.send() */
} XrSharedState;

/* Shared variable info (for coroutine variable storage) */
typedef struct XrSharedVar {
    XrString *name;
    int index;           /* Index in shared_array */
    int scope_depth;     /* Scope depth at declaration */
    int function_depth;  /* Function nesting depth */
    bool is_const;
    XrSharedState state;          /* Ownership (shared let only) */
    int moved_line;               /* Line where variable was moved */
    int moved_column;             /* Column where variable was moved */
    struct XrType *compile_type;  /* Inferred type */
} XrSharedVar;

/* ========== Compile API ========== */

/* Compile AST to function prototype via Xi IR pipeline, returns NULL on failure */
XR_FUNC XrProto *xr_compile(XrCompilerContext *ctx, AstNode *ast);

#endif  // XCOMPILER_H
