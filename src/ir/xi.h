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
 *   Xm prefix in src/jit/ (Xray Machine IR, low-level SSA for JIT).
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

/*
 * aux / aux_int semantic contract per op (authoritative reference):
 *
 *  Op               aux                  aux_int
 *  ──────────────── ──────────────────── ────────────────────────────
 *  XI_CONST         string: char*        int/bool/null literal value
 *                   (other: unused)
 *  XI_PARAM         —                    parameter index
 *  XI_LOAD_FIELD    —                    field index
 *  XI_STORE_FIELD   —                    field index
 *  XI_JSON_NEW      char** field_names   field count
 *  XI_JSON_INIT_F   —                    field index
 *  XI_JSON_GET_F    —                    field index
 *  XI_JSON_SET_F    —                    field index
 *  XI_JSON_DECODE   char** field_names   field count
 *  XI_CALL          —                    bits[0:7]=flags, bits[8:15]=nresults
 *  XI_CALL_METHOD   method name (char*)  0=normal, 1=super  (NOT SymbolId)
 *  XI_CALL_BUILTIN  —                    builtin_id
 *  XI_EXTRACT       —                    result index (1-based)
 *  XI_LOAD_UPVAL    —                    upvalue index
 *  XI_STORE_UPVAL   —                    upvalue index
 *  XI_GET_SHARED    —                    shared slot index (relative)
 *  XI_SET_SHARED    —                    shared slot index (relative)
 *  XI_PRINT         —                    print flags
 *  XI_CLOSURE_NEW   XiFunc* (child)      —
 *  XI_CLASS_CREATE  XiClassData*         —
 *  XI_SCOPE_ENTER   —                    scope mode
 *  XI_SCOPE_EXIT    —                    scope mode
 *  XI_ASSERT        loc string (char*)   0=assert_true, 1=assert_false
 *  XI_GET_BUILTIN   name string (char*)  global index
 *  XI_IMPORT_REF    XiImportRef*         resolved shared slot (-1=unresolved)
 *
 *  Consumers: xi_emit.c (VM bytecode), xi_to_xm.c (JIT), xi_cgen.c (AOT).
 *  WARNING: XI_CALL_METHOD.aux_int is NOT the method SymbolId.
 *  JIT resolves SymbolId from proto bytecode; AOT resolves from method name.
 */

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
    XI_EQ_STRICT,   /* === identity/reference equality */
    XI_NE_STRICT,   /* !== identity/reference inequality */

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

    /* Json / Allocation */
    XI_JSON_NEW,    /* Create Json object: aux=field_count, aux_ptr=field_names[] */
    XI_JSON_INIT_F, /* Init field by index: args[0]=json, args[1]=val, aux_int=field_idx */
    XI_JSON_GET_F,  /* Read field by index: args[0]=json, aux_int=field_idx */
    XI_JSON_SET_F,  /* Write field by index: args[0]=json, args[1]=val, aux_int=field_idx */
    XI_JSON_DECODE, /* Typed decode: args[0]=string, aux=field_names[], aux_int=field_count
                     * result: T? (sealed Json or null on validation failure) */
    XI_ARRAY_NEW,   /* new array: args[0]=capacity */
    XI_MAP_NEW,     /* new map: args[0]=capacity */

    /* Function calls */
    XI_CALL,        /* function call: args[0]=callee, args[1..n]=params
                     * aux_int bits 0-7: flags (1=self_call)
                     * aux_int bits 8-15: nresults (0 means 1) */
    XI_CALL_METHOD, /* method call: args[0]=recv, aux_int=method_id, args[1..n]=params */
    XI_CALL_BUILTIN,/* builtin call: aux_int=builtin_id, args[0..n]=params */
    XI_EXTRACT,     /* extract i-th result from multi-return call:
                     * args[0]=call_value, aux_int=result_index (1-based offset) */

    /* Closure / upvalue */
    XI_CLOSURE_NEW, /* create closure: aux=proto, args=captures */
    XI_LOAD_UPVAL,  /* load upvalue: aux_int=upval_index */
    XI_STORE_UPVAL, /* store upvalue: aux_int=upval_index, args[0]=val */

    /* Shared (module-level) variables */
    XI_GET_SHARED,  /* load from shared array: aux_int=shared_index */
    XI_SET_SHARED,  /* store to shared array: aux_int=shared_index, args[0]=val */

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
    XI_IS,          /* runtime type check: args[0]=value, args[1]=type (tid int or class), returns bool */
    XI_AS,          /* type cast: args[0]=value, aux=target type */
    XI_SLICE,       /* slice: args[0]=source, args[1]=start, args[2]=end */
    XI_RANGE,       /* range: args[0]=start, args[1]=end */

    /* Multi-value return packaging */
    XI_MULTI_RET,   /* args[0..n]=return values, placed in consecutive regs */

    /* Null check */
    XI_ISNULL,      /* args[0]=value, returns bool (true if null) */

    /* Phi node (not in value list — separate on XiBlock) */
    XI_PHI,         /* SSA phi: args[i] corresponds to block->preds[i] */

    /* Conditional select (from if-conversion) */
    XI_SELECT,      /* dst = args[0] ? args[1] : args[2] (cond, true_val, false_val) */

    /* Identity / type narrowing */
    XI_COPY,        /* identity: dst = args[0], may carry narrowed type */

    /* OOP: class creation */
    XI_CLASS_CREATE, /* create class from descriptor: aux=XiClassData* */

    /* Structured concurrency scope */
    XI_SCOPE_ENTER, /* enter scope: aux_int=scope_mode (0=WAIT,1=LINKED,2=SUPERVISOR) */
    XI_SCOPE_EXIT,  /* exit scope: aux_int=scope_mode, dst=result (supervisor) */

    /* Exception handling */
    XI_TRY,         /* begin try: marks start of protected region */
    XI_CATCH,       /* catch: receive exception into dst register */
    XI_FINALLY,     /* begin finally block */
    XI_END_TRY,     /* end try-catch-finally region */

    /* Builtin calls: compile-time recognized functions */
    XI_ASSERT,      /* args[0]=cond; aux=loc_string; aux_int: 0=true,1=false */
    XI_ASSERT_EQ,   /* args[0]=actual, args[1]=expected; aux=loc_string */
    XI_ASSERT_NE,   /* args[0]=actual, args[1]=unexpected; aux=loc_string */
    XI_ASSERT_THROWS, /* args[0]=fn; aux=loc_string; emits try-catch sequence */
    XI_TYPEOF,      /* args[0]=value; result=string typename */
    XI_GET_BUILTIN, /* aux=name_string; aux_int=global_index; loads runtime global */

    /* Cross-module import reference (resolved at cgen time).
     * aux = XiImportRef* (module_path + member_name).
     * aux_int = resolved shared index (set by driver post-lowering, -1 if unresolved). */
    XI_IMPORT_REF,

    XI_REGEX_COMPILE, /* args[0]=pattern(str), args[1]=flags(str); compiles regex literal */

    XI_OP_COUNT     /* sentinel */
} XiOp;

/* Import reference metadata for XI_IMPORT_REF.
 * Stored in XiValue.aux, resolved by the AOT driver after all modules
 * are lowered.  The resolved_mod_index + resolved_shared_slot fields
 * are filled in by the driver's cross-module resolution pass. */
typedef struct XiImportRef {
    const char *module_path;       /* import source (e.g. "./math_lib") */
    const char *member_name;       /* exported name (e.g. "square") */
    int resolved_mod_index;        /* index into the driver's module array, -1 = unresolved */
    int resolved_shared_slot;      /* shared slot in the target module, -1 = unresolved */
} XiImportRef;

/* Arena-safe method descriptor for XI_CLASS_CREATE.
 * One entry per instance/static method, ordered as the class declares them.
 * All strings are arena-allocated (survive AST destruction). */
typedef struct XiClassMethod {
    const char *name;          /* method name (arena copy) */
    bool is_constructor;       /* true for constructor or "constructor" */
    bool is_static;            /* true for static methods */
    bool is_static_constructor; /* true for static constructor */
} XiClassMethod;

/* Lowerer → emitter bridge for XI_CLASS_CREATE.
 * All data is arena-allocated; does NOT depend on AST after lowering. */
typedef struct XiClassData {
    struct AstNode *ast;     /* AST_CLASS_DECL node (temporary, may be NULL after lowering) */
    const char *class_name;  /* arena copy of class name */
    const char *super_name;  /* arena copy of parent class name (NULL if none) */
    XiClassMethod *methods;  /* arena array [nmethod] of method descriptors */
    uint16_t nmethod;        /* total method count (instance + static) */
    uint16_t *child_idx;     /* maps method order → XiFunc::children index */
    uint16_t ninst;          /* instance method count */
    uint16_t nstat;          /* static method count */
    int clinit_child_idx;    /* children index for static constructor (-1 if none) */
} XiClassData;

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
    bool needs_cell;       /* true if the captured variable is mutated in the child */
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
    uint8_t var_id;         /* source variable ID for register coalescing (0xFF = none) */
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

    /* Arena allocator for all IR nodes.
     * Chunked free-list — once a pointer is returned it never moves,
     * so previously allocated XiValue/XiBlock/XiPhi pointers stay valid
     * across subsequent allocations even when the arena grows. */
    struct XiArenaChunk *arena_head; /* head of chunk list (for free) */
    struct XiArenaChunk *arena_cur;  /* current chunk for new allocations */

    /* Nested functions / closures lowered from this function */
    struct XiFunc **children;
    uint16_t nchildren;
    uint16_t children_cap;

    /* Upvalue captures (populated during lowering for closures) */
    XiCapture captures[XI_MAX_CAPTURES];
    uint16_t ncaptures;

    /* Shared (module-level) variable count.  Top-level program functions
     * use shared_array for variables that must be visible across closures
     * and support forward references.  The proto's shared_offset is set
     * at emit time; shared indices are 0-based local to this func. */
    uint16_t nshared;

    /* Export table: maps shared slot → exported name.  Populated during
     * lowering for top-level declarations so the AOT driver can build
     * cross-module import resolution tables.  NULL entries = not exported. */
    const char **export_names;   /* array of nshared entries (arena-alloc'd) */

    /* VM entry metadata (propagated to XrProto during emission) */
    bool is_vararg;             /* has rest parameter (...args) */
    uint8_t entry_type;         /* 0=normal, 1=has_defaults, 2=generator */
    uint16_t min_params;        /* required parameter count (no defaults) */
    uint8_t test_attr;          /* AttributeKind: @test / @before_each / etc. */
    int test_timeout;           /* @test(timeout: N) seconds, 0 = no timeout */

    /* Source info */
    struct XaAnalyzer *analyzer; /* back-pointer for type queries */

    /* C code generation scratch (assigned by xi_cgen, not by IR construction) */
    int cgen_id;                /* unique name suffix for generated C functions */
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
XR_FUNC XiValue *xi_const_bigint(XiFunc *f, XiBlock *blk,
                                  const char *digits,
                                  struct XrType *bigint_type);

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

/* ========== Module Metadata ========== */

/* Explicit export entry: one per module-level exported binding. */
typedef struct XiModuleExport {
    const char *name;           /* exported identifier (e.g. "square") */
    uint16_t shared_slot;       /* slot in module's shared array */
    XiFunc *function;           /* non-NULL if this export is a function */
    XiClassData *class_data;    /* non-NULL if this export is a class */
} XiModuleExport;

/* Explicit import entry: one per imported member from another module. */
typedef struct XiModuleImport {
    const char *module_path;    /* source path of exporting module (e.g. "./math_lib") */
    const char *member_name;    /* imported name (e.g. "square") */
    XiModuleExport *resolved;   /* resolved after module graph linking (NULL until then) */
} XiModuleImport;

/* Per-module compilation unit: holds init function and explicit metadata.
 * Replaces the pattern of scanning IR blocks to infer exports/classes. */
typedef struct XiModule {
    const char *path;           /* source file path */
    const char *name;           /* C-safe identifier (e.g. "math_lib") */
    XiFunc *init;               /* module init function (top-level) */
    XiFunc **functions;         /* all top-level functions (init's children) */
    uint16_t nfuncs;
    XiClassData **classes;      /* all class descriptors lowered in this module */
    uint16_t nclasses;
    XiModuleExport *exports;    /* explicit export table */
    uint16_t nexports;
    XiModuleImport *imports;    /* explicit import table */
    uint16_t nimports;
} XiModule;

/* Allocate a new XiModule. Caller owns the returned pointer. */
XR_FUNC XiModule *xi_module_new(const char *path, const char *name, XiFunc *init);

/* Free a module and its metadata arrays (does NOT free init/functions). */
XR_FUNC void xi_module_free(XiModule *mod);

/* Populate exports[] and classes[] by scanning init's IR for
 * XI_SET_SHARED(slot, CLOSURE_NEW/CLASS_CREATE) patterns.
 * Must be called after lowering, before C codegen or import resolution. */
XR_FUNC void xi_module_populate_exports(XiModule *mod);

/* ========== Slot Map: Xi IR value → bytecode register mapping ========== */

/* Single entry mapping an Xi IR value to its bytecode register assignment.
 * Generated by xi_emit during bytecode emission; consumed by JIT for
 * deopt snapshot generation from Xi IR values. */
typedef struct {
    uint32_t value_id;   /* Xi IR value ID (XiValue.id) */
    uint32_t bc_pc;      /* bytecode PC where this assignment takes effect */
    uint8_t  bc_slot;    /* bytecode register R[0..255] */
    uint8_t  xr_tag;     /* XR_TAG_* (NULL=0, BOOL=1, I64=3, F64=4, PTR=5) */
} XiSlotMapEntry;

/* Mapping table from Xi IR SSA values to bytecode slots.
 * Enables JIT to generate deopt snapshots directly from Xi IR
 * without re-scanning bytecode. */
typedef struct XiSlotMap {
    XiSlotMapEntry *entries;
    uint32_t count;
    uint32_t capacity;
} XiSlotMap;

/* Free a slot map and its entries array. */
XR_FUNC void xi_slot_map_free(XiSlotMap *map);

#endif  // XI_H
