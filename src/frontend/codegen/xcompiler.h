/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcompiler.h - Compiler core interface
 *
 * KEY CONCEPT:
 * Compiles AST to bytecode. Uses scope management with
 *   dynamic register allocation and upvalue capture.
 *
 * RELATED MODULES:
 *   - xcompiler_context.h: Shared compilation context
 *   - xemit.h: Bytecode emission with peephole optimization
 *   - xregalloc.h: Register allocation
 */

#ifndef XCOMPILER_H
#define XCOMPILER_H

#include "../../runtime/value/xchunk.h"
#include "../parser/xast.h"
#include "../../runtime/object/xstring.h"
#include "../../runtime/value/xtype.h"
#include "../../base/xarena.h"
#include "../../base/xarena_vec.h"
#include "xcompiler_scope.h"
#include <stdbool.h>
#include <stdint.h>
#include "../../base/xdefs.h"

/* ========== Forward Declarations ========== */

typedef struct XrCompilerContext XrCompilerContext;
typedef struct XRegAlloc XRegAlloc;
typedef struct XrEmitter XrEmitter;
typedef struct XrExprDesc XrExprDesc;

/* ========== Function Types ========== */

typedef enum {
    FUNCTION_SCRIPT,    // Top-level script
    FUNCTION_FUNCTION,  // Regular function
} XrFunctionType;

/* ========== Upvalue Description ========== */

typedef struct XrUpvalueDesc {
    int index;                 // SRC_REG: register number; SRC_UPVAL: enclosing upval index
    struct XrType *type_info;  // Unified type from analyzer (XrType*)
    uint8_t storage_mode;      // 0=normal, 1=shared (for coroutine check)
    uint8_t slot_type;         // XrSlotType: mirrors UpvalInfo.slot_type (for GC)
    uint8_t source;            // UPVAL_SRC_REG or UPVAL_SRC_UPVAL
    bool is_const;             // For shared let send-only check
    XrString *name;            // For error messages
} XrUpvalueDesc;

/* ========== Global Variables ========== */

#define MAX_GLOBALS 256

typedef struct XrGlobalVar {
    XrString *name;
    int index;
    bool is_const;
} XrGlobalVar;

// Ownership state for shared let variables (Move semantics)
typedef enum {
    SHARED_STATE_OWNED,  // Can be used
    SHARED_STATE_MOVED,  // Cannot be used after Channel.send()
} XrSharedState;

// Shared variable info (separate from globals, for coroutine optimization)
typedef struct XrSharedVar {
    XrString *name;
    int index;           // Index in shared_array
    int scope_depth;     // Scope depth at declaration
    int function_depth;  // Function nesting depth
    bool is_const;
    XrSharedState state;          // Ownership (shared let only)
    int moved_line;               // Line where variable was moved (for error message)
    int moved_column;             // Column where variable was moved
    struct XrType *compile_type;  // Inferred return type (for functions) or var type
} XrSharedVar;

/* ========== Block Counter ========== */

#define XR_MAX_BLOCK_DEPTH 64

// Saves compiler state on block entry for proper restoration on exit
typedef struct XrBlockCnt {
    int local_end_on_entry;  // local_end value on block entry
    int scope_depth;
    bool is_loop;
} XrBlockCnt;

/* ========== Compiler State ========== */

typedef struct XrCompiler {
    struct XrCompiler *enclosing;  // Enclosing compiler (nested functions)

    XrProto *proto;
    XrFunctionType type;

    // Scope system
    XrArena *arena;          // Shared arena allocator
    XrLocalList local_list;  // Local variable list (sequential)
    int local_count;

    XrUpvalueDesc upvalues[UINT8_MAX];
    int captured_count;  // Number of captured locals in this function

    // Pre-scan result: names of locals pre-marked captured before codegen starts.
    // Filled by prescan_fn_body(); checked by scope_define_local().
    XrArenaVec(const char *) prescan_captured;

    int scope_depth;

    // Block management (stack-allocated, no malloc per scope)
    XrBlockCnt block_stack[XR_MAX_BLOCK_DEPTH];
    int block_depth;

    // Core components
    XRegAlloc *regalloc;
    XrEmitter *emitter;  // Bytecode emitter with peephole optimization

    // Scope block tracking for continuation stealing.
    int scope_block_depth;  // > 0 when inside scope{}, go emits OP_SPAWN_CONT

    // Loop control
    int loop_depth;
    int loop_start;     // Loop start position (condition check)
    int loop_continue;  // continue jump target
    int loop_scope;

    // Bounds Check Elimination (BCE) optimization
    const char *bce_loop_var;   // Loop variable name (NULL=no optimization)
    const char *bce_limit_var;  // Loop limit variable for safe index inference
    int bce_loop_var_reg;

    // break/continue jump lists for backpatching
    XrArenaVec(int) break_jumps;
    XrArenaVec(int) continue_jumps;

    // Global variables (shared)
    XrGlobalVar *globals;
    int *global_count;

    // Error flags
    bool had_error;
    bool panic_mode;

    // Per-instruction type buffer: inst_type_buf[pc] = XrType* for instruction at pc.
    // Written during compilation at each discharge point, copied to
    // proto->inst_types in xr_compiler_end().
    struct XrType **inst_type_buf;
    int inst_type_cap;

    // Struct native storage: tracks allocation within struct_area
    // Each struct instance needs (8 + layout->total_size) bytes, rounded up to 16B
    uint16_t struct_area_offset;  // current allocation offset in bytes (0 = none)

    // Declared return type for Json→primitive runtime check in return statements
    struct XrType *declared_return_type;

    // Bytecode stackmap builder: records (pc, live_slots) at GC safepoints
    // (alloc instructions). Finalized in xr_compiler_end → proto->bc_stackmap.
    struct XrBcStackMapBuilder *bc_stackmap_builder;

} XrCompiler;

/* ========== Compile API ========== */

// Compile AST to function prototype, returns NULL on failure
XR_FUNC XrProto *xr_compile(XrCompilerContext *ctx, AstNode *ast);

/* ========== Internal Functions (exposed for testing) ========== */

XR_FUNC void xr_compiler_init(XrCompilerContext *ctx, XrCompiler *compiler, XrFunctionType type);
XR_FUNC XrProto *xr_compiler_end(XrCompilerContext *ctx, XrCompiler *compiler);
XR_FUNC void xr_compile_statement(XrCompilerContext *ctx, XrCompiler *compiler, AstNode *node);
XR_FUNC int xr_compile_expression(XrCompilerContext *ctx, XrCompiler *compiler, AstNode *node);
XR_FUNC void emit_ctx_sync_before_closure(XrCompilerContext *ctx, XrCompiler *compiler);

/* ========== Internal Compile Functions ========== */

// Compile function call, is_tail indicates tail call optimization
XR_FUNC int compile_call_internal(XrCompilerContext *ctx, XrCompiler *compiler, CallExprNode *node,
                                  bool is_tail, uint8_t *out_slot_type);

/* ========== Scope Helper Functions ========== */

typedef struct XrLocalInfo XrLocalInfo;
XR_FUNC XrLocalInfo *compiler_get_local_by_name(XrCompiler *compiler, const char *name);
XR_FUNC int compiler_get_local_count(XrCompiler *compiler);
XR_FUNC XrLocalInfo *compiler_get_local_at(XrCompiler *compiler, int index);

/* ========== Register Allocation ========== */

XR_FUNC int reg_alloc(XrCompilerContext *ctx, XrCompiler *compiler);

// Allocate next consecutive register (freereg auto-increment)
XR_FUNC int reg_alloc_next(XrCompilerContext *ctx, XrCompiler *compiler);

// Reserve n registers, returns first reserved register
XR_FUNC int reg_reserve_n(XrCompiler *compiler, int n);

// Compile expression to next consecutive register
XR_FUNC int compile_expr_to_next_reg(XrCompilerContext *ctx, XrCompiler *compiler, AstNode *expr);

// Compile argument list to consecutive registers starting at base_reg
XR_FUNC int compile_args_to_base(XrCompilerContext *ctx, XrCompiler *compiler, AstNode **args,
                                 int arg_count, int base_reg);

// Compile multiple expressions to consecutive registers, returns base register
XR_FUNC int compile_exprs_consecutive(XrCompilerContext *ctx, XrCompiler *compiler, AstNode **exprs,
                                      int count, const char *reason);

XR_FUNC void reg_free(XrCompiler *compiler, int reg);

/* ========== Scope and Variable Resolution ========== */

XR_FUNC void scope_begin(XrCompiler *compiler);
XR_FUNC void scope_end(XrCompilerContext *ctx, XrCompiler *compiler);

// Define local variable, returns NULL on failure
XR_FUNC XrLocalInfo *scope_define_local(XrCompilerContext *ctx, XrCompiler *compiler,
                                        XrString *name);

// Define local variable with specific register (for method parameters)
XR_FUNC XrLocalInfo *scope_define_local_reg(XrCompilerContext *ctx, XrCompiler *compiler,
                                            XrString *name, int reg);

// Infer full compile-time type (for Struct field type propagation)
XR_FUNC struct XrType *get_expr_type(XrCompilerContext *ctx, XrCompiler *compiler, AstNode *expr);

// Resolve local variable, returns register or -1 if not found
XR_FUNC int scope_resolve_local(XrCompiler *compiler, XrString *name);

// Resolve upvalue, returns upvalue index or -1 if not found
XR_FUNC int scope_resolve_upvalue(XrCompilerContext *ctx, XrCompiler *compiler, XrString *name);

// Pre-scan function body: find locals captured by nested fns, fill prescan_captured.
// Must be called BEFORE scope_define_local_reg for params (uses fn_node->params as seed).
XR_FUNC void prescan_fn_body(XrCompiler *compiler, struct FunctionDeclNode *fn_node,
                             struct AstNode *body);

// Find predefined builtin global by name, returns index or -1
XR_FUNC int builtin_get(XrCompilerContext *ctx, XrString *name);

/* ========== Shared Variable Management ========== */

// Add new shared variable (for definition), reports error if exists
XR_FUNC int shared_add(XrCompilerContext *ctx, XrCompiler *compiler, XrString *name);

// Get or add shared variable (for access, e.g. module import)
XR_FUNC int shared_get_or_add(XrCompilerContext *ctx, XrCompiler *compiler, XrString *name);

// Find shared variable without creating, returns -1 if not found
XR_FUNC int shared_get(XrCompilerContext *ctx, XrString *name);

// Find shared variable considering lexical scope (innermost first)
XR_FUNC int shared_get_in_scope(XrCompilerContext *ctx, XrCompiler *compiler, XrString *name);

XR_FUNC void shared_set_const(XrCompilerContext *ctx, int index, bool is_const);
XR_FUNC bool shared_is_const(XrCompilerContext *ctx, int index);
XR_FUNC void shared_set_type(XrCompilerContext *ctx, int index, struct XrType *type);
XR_FUNC struct XrType *shared_get_type(XrCompilerContext *ctx, int index);

// Move semantics for shared let variables
XR_FUNC void shared_set_moved(XrCompilerContext *ctx, int index, int line, int column);
XR_FUNC bool shared_is_moved(XrCompilerContext *ctx, int index);
XR_FUNC void shared_get_moved_location(XrCompilerContext *ctx, int index, int *line, int *column);
XR_FUNC void shared_reset_state(XrCompilerContext *ctx,
                                int index);  // Reset to OWNED on reassignment

/* ========== Error Reporting ========== */

XR_FUNC void xr_compiler_error(XrCompilerContext *ctx, XrCompiler *compiler, const char *format,
                               ...);

#endif  // XCOMPILER_H
