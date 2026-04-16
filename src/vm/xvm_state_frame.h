/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm_state_frame.h - VM execution state types
 *
 * KEY CONCEPT:
 *   Core types for VM execution state used by XrayIsolate.
 *   Placed in core layer to avoid backends circular dependencies.
 */

#ifndef XVM_STATE_FRAME_H
#define XVM_STATE_FRAME_H

#include <stdint.h>
#include <stdbool.h>
#include "../runtime/value/xvalue.h"
#include "../runtime/value/xchunk.h"

/* ========== Closure Object ========== */

// Function + captured environment (flat upvalue model)
typedef struct XrClosure {
    XrGCHeader gc;
    XrProto *proto;              // compiled function prototype
    uint16_t upval_count;        // number of entries in upvals[]
    XrValue upvals[];            // flat upvalue array (const values + cell refs)
} XrClosure;

/* ========== Call Frame ========== */

// Call status flags
#define XR_CALL_C           (1 << 0)  // C function call
#define XR_CALL_YIELDED     (1 << 1)  // coroutine yielded from this frame
#define XR_CALL_HAS_CONT    (1 << 2)  // has continuation callback
#define XR_CALL_YPCALL      (1 << 3)  // protected call (pcall)
#define XR_CALL_FRESH       (1 << 4)  // fresh call (not resumed)
#define XR_CALL_KEEP_FUNC   (1 << 5)  // OP_CALL_KEEP: return value to result_offset
#define XR_CALL_JIT         (1 << 6)  // JIT compiled frame (for mixed stack unwinding)
#define XR_CALL_CLOSURE_PENDING (1 << 7) // waiting for closure called via xr_yield_call_closure

// Single activation record on call stack
// Uses base_offset (not pointer) for stack resizing safety
typedef struct XrBcCallFrame {
    XrClosure *closure;         // current function
    XrInstruction *pc;          // program counter
    int base_offset;            // base = stack + base_offset
    int result_offset;          // return value destination = stack + result_offset
    uint8_t flags;
    uint32_t call_status;       // XR_CALL_* flags
    union {
        struct {  // xray function state
            bool pending_operator_check;
            int operator_check_k;
        } l;
        struct {  // C function state
            void *continuation;
            void *continuation_ctx;
            XrValue cfunc_result;
            int16_t result_slot;
            bool has_cfunc_result;
        } c;
    } u;
} XrBcCallFrame;

/* ========== Exception Handler ========== */

// try-catch-finally block state
typedef struct XrExceptionHandler {
    uint32_t catch_offset;      // jump offset to catch block
    uint32_t finally_offset;    // jump offset to finally block
    int stack_size;             // stack size when entering try
    int frame_count;            // frame count when entering try
    XrValue exception;          // caught exception value
    bool caught;                // exception was caught
    XrInstruction *try_pc;      // PC at try block start
} XrExceptionHandler;

/* ========== C Function Types ========== */

#ifndef XR_CFUNC_RESULT_DEFINED
typedef enum {
    XR_CFUNC_DONE = 0,
    XR_CFUNC_YIELD,
    XR_CFUNC_BLOCKED,
    XR_CFUNC_ERROR,
    XR_CFUNC_CALL_CLOSURE   // closure frame pushed, execute it
} XrCFuncResult;
#define XR_CFUNC_RESULT_DEFINED
#endif

#ifndef XR_CFUNCTION_PTR_DEFINED
typedef XrValue (*XrCFunctionPtr)(XrayIsolate *isolate, XrValue *args, int nargs);
#define XR_CFUNCTION_PTR_DEFINED
#endif

#ifndef XR_YIELDABLE_CFUNCTION_PTR_DEFINED
typedef XrCFuncResult (*XrYieldableCFunctionPtr)(XrayIsolate *isolate, XrValue *args, int nargs, XrValue *result);
#define XR_YIELDABLE_CFUNCTION_PTR_DEFINED
#endif // C function scheduling classification
typedef enum {
    XR_CFUNC_FAST = 0,  // < 1ms, execute directly on P (default)
    XR_CFUNC_SLOW = 1,  // > 1ms, release P before execution (dirty worker)
} XrCFuncClass;

typedef struct XrCFunction {
    XrGCHeader gc;
    union {
        XrCFunctionPtr func;
        XrYieldableCFunctionPtr yieldable;
    } as;
    const char *name;
    bool is_yieldable;
    uint8_t cfunc_class;       // XrCFuncClass: FAST or SLOW
    uint8_t auto_slow_count;   // sysmon auto-upgrade counter (upgrade after 3)
} XrCFunction;

/* ========== Unified VM Context ========== */

// Per-coroutine execution state (main thread or worker)
// Each coroutine has its own XrVMContext for isolation
typedef struct XrVMContext {
    // Value stack
    XrValue *stack;             // stack base
    XrValue *stack_top;         // current top (next free slot)
    int stack_capacity;
    
    // Call stack
    XrBcCallFrame *frames;      // call frame array
    int frame_count;            // current depth
    int frame_capacity;
    int module_base_frame;      // module boundary for error trace
    
    // Exception handling
    XrExceptionHandler *handlers;  // handler stack
    int handler_count;
    int handler_capacity;
    XrValue current_exception;  // active exception being handled
    
    // Closure support
    void *current_coro;         // owning coroutine (XrCoroutine*)
    
    // Execution control
    uint32_t instruction_count; // for preemptive scheduling
    bool preempt_pending;       // yield at next safe point
    int last_nret;              // return value count from last call
    bool trace_execution;       // debug: trace opcodes
    struct XrStrBuf *tmp_strbuf;// scratch buffer for string ops
    XrayIsolate *isolate;       // parent isolate
    
    // Per-frame struct storage (lazy-allocated, grows with frame depth)
    uint8_t **struct_areas;     // struct data pointers per frame
    uint16_t *struct_area_caps; // allocated capacity per frame
    int struct_areas_cap;       // number of slots in struct_areas/struct_area_caps
    
    // Struct return arena: when a method returns a struct_ref pointing to its
    // own struct_area (about-to-be-reused), the data is copied here.
    // Arena grows but never shrinks, so all struct_ref pointers remain valid.
    // Reset (used=0) at top-level frame boundaries (frame_count==1).
    uint8_t *struct_ret_arena;
    uint32_t struct_ret_arena_used;
    uint32_t struct_ret_arena_cap;
} XrVMContext;

/* ========== VM Context Access Macros ========== */

#define VMCTX_STACK(ctx)            ((ctx)->stack)
#define VMCTX_STACK_TOP(ctx)        ((ctx)->stack_top)
#define VMCTX_STACK_CAP(ctx)        ((ctx)->stack_capacity)
#define VMCTX_FRAMES(ctx)           ((ctx)->frames)
#define VMCTX_FRAME_COUNT(ctx)      ((ctx)->frame_count)
#define VMCTX_FRAME_CAP(ctx)        ((ctx)->frame_capacity)
#define VMCTX_MODULE_BASE(ctx)      ((ctx)->module_base_frame)
#define VMCTX_HANDLERS(ctx)         ((ctx)->handlers)
#define VMCTX_HANDLER_COUNT(ctx)    ((ctx)->handler_count)
#define VMCTX_EXCEPTION(ctx)        ((ctx)->current_exception)
#define VMCTX_CORO(ctx)             ((ctx)->current_coro)
#define VMCTX_TMP_STRBUF(ctx)       ((ctx)->tmp_strbuf)
#define VMCTX_ISOLATE(ctx)          ((ctx)->isolate)
#define VMCTX_GC(ctx)               (&VMCTX_ISOLATE(ctx)->gc)
#define VMCTX_GLOBALS(ctx)          (VMCTX_ISOLATE(ctx)->vm.builtins)
#define VMCTX_GLOBAL_COUNT(ctx)     (VMCTX_ISOLATE(ctx)->vm.builtin_count)
#define VMCTX_STRINGS(ctx)          (VMCTX_ISOLATE(ctx)->vm.strings_map)

#endif // XVM_STATE_FRAME_H
