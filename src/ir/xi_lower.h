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
#include "xi_module.h"

struct AstNode;
struct XaAnalyzer;
struct XrType;
struct XrVMRuntime;

/* ========== Variable Tracking for Braun SSA ========== */

/*
 * Per-variable definition map: var_defs[var_id * max_blocks + block_id].
 * Dense 2D layout chosen over hash map — variable and block counts are
 * small (< 1000 each) for typical functions.
 */

/* Initial capacities for the per-function variable and basic-block maps.
 * These are no longer hard caps: vars, the parallel shared-slot tables, and
 * the 2D var_defs map grow on demand (see xi_lower_grow_vars/_blocks), so a
 * function/module is bounded only by memory, not by a fixed 256. */
#define XI_LOWER_INIT_VARS 256
#define XI_LOWER_INIT_BLOCKS 256
#define XI_LOWER_MAX_INCOMPLETE 256

typedef struct XiVarEntry {
    uint32_t symbol_id;     /* unique ID from analyzer (0 = unresolved / synthetic) */
    const char *name;       /* variable name (debug only, not owned, points into AST) */
    struct XrType *type;    /* declared type */
    bool hoisted;           /* true if pre-registered by function hoisting; captures
                             * of this variable need cell indirection because the
                             * actual closure is assigned after sibling captures. */
    bool captured_by_child; /* true if a hoisted child function captures this
                             * variable — definitions must survive DCE because
                             * hoisting reorders the closure before the actual
                             * initializer, and the upvalue/cell must see the
                             * real value rather than the braun-read null. */
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

typedef struct XiLoopTarget {
    const char *label;
    XiBlock *break_target;
    XiBlock *continue_target;
    int defer_scope_depth;
    struct XiLoopTarget *prev;
} XiLoopTarget;

typedef struct XiDeferScope {
    XiValue *mark;
} XiDeferScope;

/* ========== Lowering Context ========== */

typedef struct XiLower {
    XiFunc *func;
    XiBlock *cur_block;

    /* Semantic analysis context (type queries) */
    struct XaAnalyzer *analyzer;
    struct XrVMRuntime *isolate;

    /* Braun SSA variable tracking.
     * Each entry is keyed by symbol_id from the analyzer.  Scope resolution
     * is done by the analyzer (which assigns unique IDs even for same-named
     * variables in different scopes), so no scope stack is needed here. */
    XiVarEntry *vars;
    int var_count;
    int var_cap;   /* allocated length of vars / shared_map / shared_slot_* */
    int block_cap; /* allocated block dimension (stride) of var_defs */

    /* 2D def map: var_defs[var_id * block_cap + block_id] = XiValue*
     * Heap-allocated; grows in either dimension on demand. */
    XiValue **var_defs;

    /* Incomplete phis for unsealed blocks */
    XiIncompletePhi incomplete[XI_LOWER_MAX_INCOMPLETE];
    int incomplete_count;

    /* Loop targets for break/continue */
    XiBlock *break_target;
    XiBlock *continue_target;
    XiLoopTarget *loop_targets;

    /* Lexical defer scopes.  A scope receives a mark lazily when the first
     * `defer` owned by that block is lowered; scopes with no direct defer stay
     * free at runtime. */
#define XI_MAX_DEFER_SCOPE_NESTING 256
    XiDeferScope defer_scopes[XI_MAX_DEFER_SCOPE_NESTING];
    int defer_scope_depth;

    /* Cached singleton types (obtained once from isolate) */
    struct XrType *type_int;
    struct XrType *type_float;
    struct XrType *type_bool;
    struct XrType *type_string;
    struct XrType *type_char;
    struct XrType *type_null;
    struct XrType *type_unit;
    struct XrType *type_any;
    struct XrType *type_bigint;
    struct XrType *type_regex;

    /* Self-reference for recursive named functions.
     * Set to a dummy XI_CONST in xi_lower_func so the function body
     * can resolve its own name; lower_call detects this and emits
     * a self-call (OP_CALLSELF) instead of a regular call. */
    XiValue *self_value;
    int self_var_id; /* Braun var_id of the self-reference (-1 = none) */

    /* Parent lowering context for upvalue capture resolution.
     * NULL for top-level program or standalone functions.
     * When a variable is not found locally, walk up the parent chain;
     * each traversed level records a capture entry on its XiFunc. */
    struct XiLower *parent;

    /* Monotonic counter for generating unique synthetic variable names
     * (e.g. __for_idx_0, __for_idx_1) to avoid collisions in nested loops. */
    int synthetic_id;

    /* Shared variable map: var_id → shared_index.
     * -1 means the variable is not shared.  Only used in top-level program
     * lowering to support forward references and cross-closure access. */
    int16_t *shared_map;

    /* Shared slot → function/class tracking (built during lowering).
     * Enables direct construction of XiModule.exports without IR scanning.
     * Parallel to vars (length var_cap), grown together. */
    struct XiFunc **shared_slot_funcs;
    struct XiClassData **shared_slot_classes;
    XiEnumData **shared_slot_enums;
    XiImportRef **shared_slot_imports;

    /* Whether this lowering context is for a top-level program */
    bool is_program;

    /* REPL incremental compilation flag.  When true and is_program is
     * true, top-level variable / function / class bindings lower to
     * XI_GET_GLOBAL / XI_SET_GLOBAL (name-keyed dict) instead of the
     * slot-indexed XI_GET_SHARED / XI_SET_SHARED.  Inherited by child
     * function lowering contexts so nested closures resolve free
     * top-level names through the dict as well. */
    bool repl_mode;

    /* Nesting depth of try-catch blocks.  When > 0, throw inside the
     * try body jumps to catch_targets[try_depth-1] instead of returning
     * from the function. */
    int try_depth;

    /* Stack of catch target blocks for nested try-catch.
     * throw inside try { } writes pending_error and jumps here. */
#define XI_MAX_TRY_NESTING 32
    struct XiBlock *catch_targets[XI_MAX_TRY_NESTING];
    int catch_defer_depths[XI_MAX_TRY_NESTING];

    /* True when cur_block's last instruction is XI_ERR_RETURN but the
     * block is kept alive for SSA predecessor edges (try_depth > 0).
     * Consumers must NOT append semantically live code to this block.
     * Reset when cur_block changes to a genuinely new block. */
    bool dead_after_throw;

    /* Error tracking */
    bool had_error;
} XiLower;

/* ========== API ========== */

/*
 * Lower a function AST node into typed SSA IR.
 * The AST must be canonicalized (xr_canon_func) before lowering.
 * The analyzer provides type information for each AST node.
 * Returns NULL on failure.
 */
XR_FUNC XiFunc *xi_lower_func(struct AstNode *func_node, struct XaAnalyzer *analyzer,
                              struct XrVMRuntime *isolate);

/*
 * Lower a top-level program (sequence of statements) into a
 * synthetic "main" function. Used for script-mode execution.
 * The AST must be canonicalized (xr_canon_program) before lowering.
 */
XR_FUNC XiFunc *xi_lower_program(struct AstNode *program_node, struct XaAnalyzer *analyzer,
                                 struct XrVMRuntime *isolate);

/* Same as xi_lower_program but enables REPL incremental compilation.
 * When repl_mode is true, top-level name resolution / store goes
 * through the name-keyed XrGlobalDict instead of the slot-indexed
 * XrSharedArray; cross-input bindings resolve at runtime by name. */
XR_FUNC XiFunc *xi_lower_program_ex(struct AstNode *program_node, struct XaAnalyzer *analyzer,
                                    struct XrVMRuntime *isolate, bool repl_mode);

#endif  // XI_LOWER_H
