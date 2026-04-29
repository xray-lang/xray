/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi.h - Typed SSA IR core data structures
 *
 * KEY CONCEPT:
 *   Mid-level typed SSA IR that all three backends (VM/JIT/AOT) consume.
 *   Every SSA value carries an authoritative XrType* from the semantic
 *   analyzer, and XrRep is derived from it via xr_type_rep().
 *
 *   Design follows Go SSA: Value = instruction (def + op + args combined
 *   in one struct). This eliminates the split between "instruction" and
 *   "virtual register" that the legacy JIT IR uses.
 *
 *   Operation granularity is mid-level: high-level semantics preserved
 *   (XI_CALL_METHOD, XI_ITER_NEXT) while control flow is explicit
 *   (basic blocks + branch/jump terminators, phi nodes for merges).
 *
 * NAMING:
 *   Xi prefix (Xray IR, new generation) avoids collision with the legacy
 *   Xir prefix in src/jit/. After the legacy IR is retired, Xi -> Xir.
 *
 * INVARIANTS:
 *   1. SSA form: each XiValue has exactly one definition point.
 *   2. Every XiValue carries a non-NULL XrType* (XI_CONST_NULL uses
 *      xr_type_null; unknown uses xr_type_any).
 *   3. XrRep is derived from XrType* and never set independently.
 *   4. Block terminators are on XiBlock (kind + control value), not
 *      encoded as instructions in the value list.
 *   5. Phi nodes are separate from the instruction list, ordered by
 *      predecessor index.
 */

#ifndef XI_H
#define XI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "../base/xdefs.h"

/* Forward declarations for types defined in other modules */
struct XrType;
struct AstNode;
struct XaAnalyzer;

/* ========== Operation Kinds ========== */

typedef enum {
    /* Constants */
    XI_CONST = 0,   /* constant value (int/float/bool/null/string in aux) */
    XI_PARAM,       /* function parameter (aux_int = param index) */

    /* Arithmetic (polymorphic: type determines int vs float) */
    XI_ADD,
    XI_SUB,
    XI_MUL,
    XI_DIV,
    XI_MOD,
    XI_NEG,         /* unary negate */

    /* Bitwise */
    XI_BAND,        /* & */
    XI_BOR,         /* | */
    XI_BXOR,        /* ^ */
    XI_BNOT,        /* ~ */
    XI_SHL,         /* << */
    XI_SHR,         /* >> */

    /* Comparison (result is always bool) */
    XI_EQ,
    XI_NE,
    XI_LT,
    XI_LE,
    XI_GT,
    XI_GE,

    /* Logical */
    XI_NOT,         /* ! (unary) */

    /* Type conversion */
    XI_CONVERT,     /* explicit type cast: aux stores target type */
    XI_BOX,         /* unboxed -> tagged XrValue */
    XI_UNBOX,       /* tagged -> unboxed (type guard) */

    /* Memory / field access */
    XI_LOAD_FIELD,  /* obj.field: args[0]=obj, aux_int=field_index */
    XI_STORE_FIELD, /* obj.field=val: args[0]=obj, args[1]=val, aux_int=field_index */
    XI_INDEX_GET,   /* obj[key]: args[0]=obj, args[1]=key */
    XI_INDEX_SET,   /* obj[key]=val: args[0]=obj, args[1]=key, args[2]=val */

    /* Allocation */
    XI_ALLOC,       /* GC allocation: aux=type info */
    XI_ARRAY_NEW,   /* new array: args[0]=capacity */
    XI_MAP_NEW,     /* new map: args[0]=capacity */

    /* Function calls */
    XI_CALL,        /* function call: args[0]=callee, args[1..n]=params */
    XI_CALL_METHOD, /* method call: args[0]=recv, aux_int=method_id, args[1..n]=params */
    XI_CALL_BUILTIN,/* builtin call: aux_int=builtin_id, args[0..n]=params */

    /* Closure / upvalue */
    XI_CLOSURE_NEW, /* create closure: aux=proto, args=captures */
    XI_LOAD_UPVAL,  /* load upvalue: aux_int=upval_index */
    XI_STORE_UPVAL, /* store upvalue: aux_int=upval_index, args[0]=val */

    /* Print (builtin, kept as dedicated op for convenience) */
    XI_PRINT,       /* print: args[0..n]=values, aux_int=flags */

    /* Coroutine */
    XI_GO,          /* go expr: args[0]=callee, args[1..n]=params */
    XI_AWAIT,       /* await task: args[0]=task */
    XI_CHAN_SEND,   /* ch.send(v): args[0]=chan, args[1]=val */
    XI_CHAN_RECV,   /* ch.recv(): args[0]=chan */
    XI_YIELD,       /* yield execution */

    /* Exception handling */
    XI_THROW,       /* throw exception: args[0]=value */

    /* Iteration (for-in protocol) */
    XI_ITER_NEW,    /* create iterator: args[0]=collection */
    XI_ITER_NEXT,   /* advance + get value: args[0]=iterator, returns next value */
    XI_ITER_VALID,  /* test not-done: args[0]=iterator, returns bool */

    /* Defer */
    XI_DEFER,       /* defer expr: args[0]=callee (executed at scope exit) */

    /* Channel creation */
    XI_CHAN_NEW,     /* create channel: args[0]=buffer_size (optional) */

    /* Set creation */
    XI_SET_NEW,     /* create set: args[0]=capacity */

    /* String concatenation (for template strings) */
    XI_STR_CONCAT,  /* concat: args[0..n]=parts, produces string */

    /* Type operations */
    XI_IS,          /* runtime type check: args[0]=value, aux=target type, returns bool */
    XI_AS,          /* type cast: args[0]=value, aux=target type */
    XI_SLICE,       /* slice: args[0]=source, args[1]=start, args[2]=end */
    XI_RANGE,       /* range: args[0]=start, args[1]=end */

    /* Null check */
    XI_ISNULL,      /* args[0]=value, returns bool (true if null) */

    /* Phi node (not in value list — separate on XiBlock) */
    XI_PHI,         /* SSA phi: args[i] corresponds to block->preds[i] */

    /* Identity / type narrowing */
    XI_COPY,        /* identity: dst = args[0], may carry narrowed type */

    XI_OP_COUNT     /* sentinel */
} XiOp;

/* ========== Block Kinds ========== */

typedef enum {
    XI_BLOCK_PLAIN = 0,   /* single successor: succs[0] */
    XI_BLOCK_IF,          /* conditional: control=bool, succs[0]=then, succs[1]=else */
    XI_BLOCK_RETURN,      /* function return: control=return value (or NULL for void) */
    XI_BLOCK_UNREACHABLE, /* throw / panic: no successors */
} XiBlockKind;

/* ========== Value Flags ========== */

#define XI_FLAG_SIDE_EFFECT (1 << 0) /* has side effects (cannot be eliminated) */
#define XI_FLAG_MAY_THROW   (1 << 1) /* may raise exception */
#define XI_FLAG_MAY_GC      (1 << 2) /* may trigger GC */
#define XI_FLAG_SAFEPOINT   (1 << 3) /* GC safepoint */

/* ========== Upvalue Capture Info ========== */

/* Source kinds matching the VM's UpvalInfo.source constants */
#define XI_CAPTURE_SRC_REG   2  /* from enclosing frame's register */
#define XI_CAPTURE_SRC_UPVAL 1  /* from enclosing closure's upvals[] */

#define XI_MAX_CAPTURES 64

typedef struct XiCapture {
    uint8_t source;        /* XI_CAPTURE_SRC_REG or XI_CAPTURE_SRC_UPVAL */
    uint8_t index;         /* SRC_UPVAL: parent upvalue index */
    const char *name;      /* variable name (debug; not owned) */
    struct XrType *type;   /* variable type */
    struct XiValue *value; /* SRC_REG: parent SSA value (register resolved at emit) */
} XiCapture;

/* ========== Core Structures ========== */

/*
 * SSA Value: every definition is unique, carries authoritative type.
 * Combines Go SSA's Value design with xray's type system.
 *
 * Size: ~72 bytes. Values are arena-allocated within XiFunc.
 */
typedef struct XiValue {
    uint32_t id;            /* dense SSA value ID (unique within function) */
    uint16_t op;            /* XiOp */
    uint8_t flags;          /* XI_FLAG_* */
    uint8_t _pad;
    struct XrType *type;    /* authoritative compile-time type (never NULL) */
    int64_t aux_int;        /* auxiliary integer: const value, symbol ID, etc. */
    void *aux;              /* auxiliary pointer: proto, string literal, etc. */
    struct XiValue **args;  /* operand values (SSA uses) */
    uint16_t nargs;         /* number of args */
    int16_t uses;           /* use count (for DCE; -1 = not computed) */
    uint32_t line;          /* source line number (0 = unknown) */
    struct XiBlock *block;  /* containing block */
} XiValue;

/*
 * Phi node: placed at block entry for control-flow merges.
 * Kept separate from the instruction list for efficient iteration.
 * args[i] corresponds to block->preds[i].
 */
typedef struct XiPhi {
    XiValue value;          /* embedded value (op == XI_PHI) */
    struct XiPhi *next;     /* linked list within block */
} XiPhi;

/*
 * Basic block: linear sequence of instructions, terminated by block kind.
 *
 * The terminator is encoded in (kind, control) rather than as a trailing
 * instruction. This matches Go SSA's design and simplifies iteration.
 */
typedef struct XiBlock {
    uint32_t id;            /* dense block ID (unique within function) */
    uint16_t kind;          /* XiBlockKind */
    bool visited;           /* traversal scratch */
    uint8_t _pad;

    /* Phi nodes at entry (linked list; NULL if no merge point) */
    XiPhi *phis;

    /* Instructions (contiguous array, no terminators) */
    XiValue **values;
    uint32_t nvalues;
    uint32_t values_cap;

    /* Terminator control value (condition for IF, return val for RETURN) */
    XiValue *control;

    /* CFG edges */
    struct XiBlock *succs[2]; /* succs[0]=then/next, succs[1]=else (IF only) */
    struct XiBlock **preds;   /* predecessor array */
    uint16_t npreds;
    uint16_t preds_cap;

    /* Ordering & dominance */
    uint32_t rpo;             /* reverse post-order index (0 = not computed) */
    struct XiBlock *idom;     /* immediate dominator (NULL for entry) */
    uint16_t dom_depth;       /* depth in dominator tree (entry = 0) */

    /* Braun SSA: block sealing.
     * A block is sealed when all its predecessors are known.
     * Loop headers are unsealed until the back edge is added. */
    bool sealed;

    /* Back-pointer */
    struct XiFunc *func;
} XiBlock;

/*
 * Auxiliary constant data stored in XiValue.aux_int / XiValue.aux.
 */
typedef union {
    int64_t i64;
    double f64;
    const char *str;
    void *ptr;
} XiAux;

/*
 * Function: compilation unit for the new IR.
 * One XiFunc per source-level function or closure.
 *
 * All XiValues, XiPhis, XiBlocks, and arg arrays are allocated from
 * a simple bump allocator (arena). The arena is freed as a whole when
 * the XiFunc is destroyed, so individual frees are not needed.
 */
typedef struct XiFunc {
    const char *name;           /* function name (debug, not owned) */
    struct XrType *return_type; /* return type (from analyzer) */

    /* Function parameters as SSA values (in entry block) */
    XiValue **params;
    uint16_t nparams;

    /* Basic blocks */
    XiBlock **blocks;
    uint32_t nblocks;
    uint32_t blocks_cap;
    XiBlock *entry;             /* blocks[0] is always the entry block */

    /* ID allocation */
    uint32_t next_value_id;
    uint32_t next_block_id;

    /* Arena allocator for all IR nodes */
    uint8_t *arena;
    uint32_t arena_used;
    uint32_t arena_cap;

    /* Nested functions / closures lowered from this function */
    struct XiFunc **children;
    uint16_t nchildren;
    uint16_t children_cap;

    /* Upvalue captures (populated during lowering for closures) */
    XiCapture captures[XI_MAX_CAPTURES];
    uint16_t ncaptures;

    /* Source info */
    struct XaAnalyzer *analyzer; /* back-pointer for type queries */
} XiFunc;

/* ========== Arena Constants ========== */

#define XI_ARENA_INITIAL_SIZE (64 * 1024) /* 64 KiB initial arena */

/* ========== API: Function Lifecycle ========== */

XR_FUNC XiFunc *xi_func_new(const char *name, struct XrType *return_type);
XR_FUNC void xi_func_free(XiFunc *f);

/* Arena helper: allocate aligned memory from the function's bump allocator.
 * Used by xi_lower.c for phi arg arrays during Braun SSA construction. */
XR_FUNC void *xi_func_arena_alloc(XiFunc *f, uint32_t size);

/* ========== API: Block Management ========== */

XR_FUNC XiBlock *xi_block_new(XiFunc *f);
XR_FUNC void xi_block_add_pred(XiBlock *blk, XiBlock *pred);

/* ========== API: Value Construction ========== */

/* Create a new value and append to the given block. */
XR_FUNC XiValue *xi_value_new(XiFunc *f, XiBlock *blk, uint16_t op,
                               struct XrType *type, uint16_t nargs);

/* Convenience: constant constructors.
 * Caller provides XrType* (obtained from XaAnalyzer/XrTypePool).
 * The IR module does not depend on XrayIsolate. */
XR_FUNC XiValue *xi_const_int(XiFunc *f, XiBlock *blk, int64_t val,
                               struct XrType *int_type);
XR_FUNC XiValue *xi_const_float(XiFunc *f, XiBlock *blk, double val,
                                 struct XrType *float_type);
XR_FUNC XiValue *xi_const_bool(XiFunc *f, XiBlock *blk, bool val,
                                struct XrType *bool_type);
XR_FUNC XiValue *xi_const_null(XiFunc *f, XiBlock *blk,
                                struct XrType *null_type);
XR_FUNC XiValue *xi_const_str(XiFunc *f, XiBlock *blk, const char *str,
                               struct XrType *str_type);

/* Convenience: binary op */
XR_FUNC XiValue *xi_binary(XiFunc *f, XiBlock *blk, uint16_t op,
                            struct XrType *type, XiValue *lhs, XiValue *rhs);

/* Convenience: unary op */
XR_FUNC XiValue *xi_unary(XiFunc *f, XiBlock *blk, uint16_t op,
                           struct XrType *type, XiValue *arg);

/* Convenience: function parameter */
XR_FUNC XiValue *xi_param(XiFunc *f, XiBlock *blk, uint16_t index,
                           struct XrType *type);

/* ========== API: Phi Nodes ========== */

XR_FUNC XiPhi *xi_phi_new(XiFunc *f, XiBlock *blk, struct XrType *type,
                           uint16_t npreds);

/* ========== API: Block Termination ========== */

XR_FUNC void xi_block_set_return(XiBlock *blk, XiValue *val);
XR_FUNC void xi_block_set_jump(XiBlock *blk, XiBlock *target);
XR_FUNC void xi_block_set_if(XiBlock *blk, XiValue *cond,
                              XiBlock *then_blk, XiBlock *else_blk);

/* ========== API: Dump ========== */

/* Print human-readable text representation to FILE* (pass stdout for console) */
XR_FUNC void xi_func_dump(const XiFunc *f, void *stream);

#endif  // XI_H
