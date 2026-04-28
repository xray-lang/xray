/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_lower.h - AST to typed SSA IR lowering pass
 *
 * KEY CONCEPT:
 *   Translates an AstNode tree (from the parser) into XiFunc (typed SSA IR)
 *   using the Braun SSA construction algorithm. Types are carried from the
 *   semantic analyzer's XaNodeTable on every XiValue.
 *
 *   Braun Algorithm (Simple and Efficient SSA Construction):
 *   - writeVariable(var, block, value): records the current SSA def
 *   - readVariable(var, block): returns the SSA value, inserting phi
 *     nodes on-the-fly when a merge point is reached
 *   - Trivial phis (all operands identical) are removed immediately
 *
 *   The lowering is a single recursive AST walk. Control flow nodes
 *   (if, while, for) create new basic blocks and wire CFG edges.
 *
 * SCOPE:
 *   Expressions: literals, binary/unary ops, variable read/write
 *   Statements: var decl, assignment, print, block, return
 *   Control flow: if/else, while, for, break, continue
 */

#ifndef XI_LOWER_H
#define XI_LOWER_H

#include "xi.h"

struct AstNode;
struct XaAnalyzer;
struct XrType;
struct XrayIsolate;

/* ========== Variable Tracking for Braun SSA ========== */

/*
 * Per-variable definition map: var_defs[var_id * max_blocks + block_id].
 * Dense 2D layout chosen over hash map — variable and block counts are
 * small (< 1000 each) for typical functions.
 */

#define XI_LOWER_MAX_VARS   256
#define XI_LOWER_MAX_BLOCKS 256
#define XI_LOWER_MAX_INCOMPLETE 256

typedef struct XiVarEntry {
    const char *name;   /* variable name (not owned, points into AST) */
    struct XrType *type;/* declared type */
} XiVarEntry;

/*
 * Incomplete phi: recorded when a variable is read from an unsealed
 * block (loop header whose back edge hasn't been wired yet).
 * When the block is sealed, all incomplete phis are resolved.
 */
typedef struct XiIncompletePhi {
    int var_id;
    XiBlock *block;
    XiPhi *phi;
} XiIncompletePhi;

/* ========== Lowering Context ========== */

typedef struct XiLower {
    XiFunc *func;
    XiBlock *cur_block;

    /* Semantic analysis context (type queries) */
    struct XaAnalyzer *analyzer;
    struct XrayIsolate *isolate;

    /* Braun SSA variable tracking */
    XiVarEntry vars[XI_LOWER_MAX_VARS];
    int var_count;

    /* 2D def map: var_defs[var_id * max_blocks + block_id] = XiValue* */
    XiValue *var_defs[XI_LOWER_MAX_VARS * XI_LOWER_MAX_BLOCKS];

    /* Incomplete phis for unsealed blocks */
    XiIncompletePhi incomplete[XI_LOWER_MAX_INCOMPLETE];
    int incomplete_count;

    /* Loop targets for break/continue */
    XiBlock *break_target;
    XiBlock *continue_target;

    /* Cached singleton types (obtained once from isolate) */
    struct XrType *type_int;
    struct XrType *type_float;
    struct XrType *type_bool;
    struct XrType *type_string;
    struct XrType *type_null;
    struct XrType *type_void;
    struct XrType *type_any;

    /* Error tracking */
    bool had_error;
} XiLower;

/* ========== API ========== */

/*
 * Lower a function AST node into typed SSA IR.
 * The analyzer provides type information for each AST node.
 * Returns NULL on failure.
 */
XR_FUNC XiFunc *xi_lower_func(struct AstNode *func_node,
                               struct XaAnalyzer *analyzer,
                               struct XrayIsolate *isolate);

/*
 * Lower a top-level program (sequence of statements) into a
 * synthetic "main" function. Used for script-mode execution.
 */
XR_FUNC XiFunc *xi_lower_program(struct AstNode *program_node,
                                  struct XaAnalyzer *analyzer,
                                  struct XrayIsolate *isolate);

#endif  // XI_LOWER_H
