/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcgen.h - AOT C code generator for XIR
 *
 * KEY CONCEPT:
 *   Translates XIR SSA form into section-based C source code.
 *   Each XIR function becomes a static C function with labels + goto.
 *   gcc/clang -O2 handles all backend optimizations.
 *
 * WHY THIS DESIGN:
 *   - XIR already optimized (DCE/GVN/LICM), generated C quality is high
 *   - Section-based output enables forward declarations and multi-function
 *   - Same XIR pipeline serves both JIT (ARM64) and AOT (C transpile)
 */

#ifndef XCGEN_H
#define XCGEN_H

#include "../jit/xir.h"
#include "xcgen_struct.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdarg.h>
#include "../base/xdefs.h"

/* ========== Dynamic String Buffer ========== */

typedef struct XcgenBuf {
    char *data;
    size_t len;
    size_t cap;
} XcgenBuf;

XR_FUNC void xcgen_buf_init(XcgenBuf *b);
XR_FUNC void xcgen_buf_free(XcgenBuf *b);
XR_FUNC void xcgen_buf_ensure(XcgenBuf *b, size_t extra);
XR_FUNC void xcgen_buf_puts(XcgenBuf *b, const char *s);
XR_FUNC void xcgen_buf_append(XcgenBuf *dst, const XcgenBuf *src);

XR_PRINTF_FMT(2, 3) XR_FUNC void xcgen_buf_printf(XcgenBuf *b, const char *fmt, ...);

/* ========== C File Sections ========== */

typedef enum {
    XCGEN_SEC_HEADERS,  // #include directives
    XCGEN_SEC_FORWARD,  // forward declarations
    XCGEN_SEC_TYPES,    // struct/typedef
    XCGEN_SEC_DATA,     // static data (string literals, constants)
    XCGEN_SEC_FUNCS,    // function bodies
    XCGEN_SEC_MAIN,     // main() wrapper
    XCGEN_SEC_COUNT,
} XcgenSection;

/* ========== Proto → C Name Mapping ========== */

#define XCGEN_MAX_CALL_ARGS 65  // initial capacity: slot 0 (closure) + 64 args

typedef struct {
    void *proto_ptr;     // XrProto* (opaque, compared by address)
    const char *c_name;  // C function name (e.g. "xr_fib")
    int func_idx;        // index into mod->funcs (-1 = not yet compiled)
    bool non_escaping;   // true if closure only called via CALL_KNOWN (never stored/returned)
    int num_upvals;      // upvalue count for non-escaping closures (0 if escaping)
} XcgenProtoEntry;

/* ========== Per-Function Codegen State ========== */

typedef struct XcgenFunc {
    XirFunc *xfunc;            // source XIR function (may be freed after compile)
    const char *c_name;        // generated C function name (e.g. "xr_fib")
    int num_params;            // cached from xfunc->num_params (valid after xfunc freed)
    XcgenBuf body;             // function body buffer
    int tmp_count;             // temp variable counter
    bool needs_runtime;        // true if function calls runtime APIs
    bool needs_closure_param;  // true if function accesses upvalues (needs XrValue xrt_closure
                               // param)
    bool non_escaping;         // true if all callers pass upvalues inline (no closure object)
    int num_upvals;  // upvalue count for non-escaping closures (upval params after regular params)
    bool needs_exception;  // true if function uses try/catch (needs XrValue xrt_exception local)
    bool void_return;      // true if function always returns null → emit as void

    // Exception frame tracking for finally support:
    // Maps catch/finally handler block IDs to their exception frame index,
    // and maintains a stack for TRY_END re-throw emission.
    int exc_catch_frame[256];  // block_id → frame_index (-1 = none)
    int exc_pending_stack[8];  // frame indices awaiting TRY_END re-throw
    int exc_pending_depth;     // depth of pending stack
    bool exc_has_finally[8];   // per-frame: true if this try has a finally block

    // Dead code elimination: which vregs are actually read
    bool *used_vregs;

    // Struct promotion: vreg → struct index (-1 = not a struct)
    int16_t *vreg_struct_id;  // [nvreg] array, -1 = not promoted

    // Call args buffer: tracks STORE_CORO writes to jit_call_args[]
    // Used by CALL_KNOWN to reconstruct direct C function calls.
    // Dynamically grown when a function has more than 64 parameters.
    XirRef *call_args;  // heap-allocated, capacity = call_args_cap
    int call_args_cap;  // current allocated size
    int call_args_count;

    // Defer tracking: number of deferred closures in this function.
    // Codegen emits _defer_N / _defer_N_set locals and LIFO cleanup at returns.
    int defer_count;

    // Conditional select: XIR_SELECT_COND stores the condition ref here,
    // XIR_SELECT reads it to emit a C ternary expression.
    XirRef last_select_cond;
} XcgenFunc;

/* ========== Forward Declarations ========== */

typedef struct XcgenCompilation XcgenCompilation;

/* ========== Class Info (for type registration in generated code) ========== */

typedef struct {
    void *ctor_proto;         // constructor XrProto* (matching key)
    const char *class_name;   // class name (e.g. "Shape")
    const char *parent_name;  // parent class name (NULL = no parent)
    int nfields;              // instance field count
} XcgenClassInfo;

/* ========== Module-Level Export Entry ========== */

typedef struct XcgenExport {
    const char *name;  // export name (e.g. "pi", "add")
    int shared_index;  // index into xrt_shared[] array (-1 = named global)
    bool is_const;     // true if const export
} XcgenExport;

/* ========== Module-Level Codegen State ========== */

typedef struct XcgenModule {
    // Module metadata
    const char *module_name;  // e.g. "math", "./utils"
    const char *module_path;  // absolute path
    int16_t module_id;        // unique id within compilation

    XcgenBuf sections[XCGEN_SEC_COUNT];
    XcgenFunc *funcs;  // compiled functions in this module
    int nfuncs;
    int funcs_cap;

    // Module-level exports (populated during GETSHARED/SETSHARED scan)
    XcgenExport *exports;
    int nexports;
    int exports_cap;

    // Struct promotion registry (non-owning ptr → comp->struct_reg)
    XcgenStructRegistry *struct_reg;

    // Backpointer to parent compilation context
    XcgenCompilation *comp;
} XcgenModule;

/* ========== Top-Level Compilation Context ========== */

struct XcgenCompilation {
    XcgenModule **modules;  // array of module pointers
    int nmodules;
    int modules_cap;

    // Global proto registry (cross-module function lookup)
    XcgenProtoEntry *proto_map;
    int proto_map_count;
    int proto_map_cap;

    // Global struct promotion registry (owned externally by cmd_build)
    XcgenStructRegistry *struct_reg;

    // Shared variable tracking (for GETSHARED/SETSHARED → C globals)
    int max_shared_index;  // highest shared index seen (-1 = none)

    // Class info registry (for type registration codegen)
    XcgenClassInfo *class_infos;
    int nclass_infos;
    int class_infos_cap;
};

/* ========== Compilation API ========== */

// Create a new compilation context
XR_FUNC XcgenCompilation *xcgen_compilation_new(void);

// Free compilation and all owned modules
XR_FUNC void xcgen_compilation_free(XcgenCompilation *comp);

// Add a module to the compilation; returns the module pointer
XR_FUNC XcgenModule *xcgen_compilation_add_module(XcgenCompilation *comp, const char *name,
                                                  const char *path);

// Register a proto → C name mapping in the global registry
XR_FUNC void xcgen_register_proto(XcgenCompilation *comp, void *proto_ptr, const char *c_name);

// Register a class for type registration codegen
XR_FUNC void xcgen_register_class(XcgenCompilation *comp, void *ctor_proto, const char *class_name,
                                  const char *parent_name, int nfields);

// Lookup class info by constructor proto; returns NULL if not found
XR_FUNC const XcgenClassInfo *xcgen_lookup_class(XcgenCompilation *comp, void *ctor_proto);

// Emit combined C source for all modules (single-file mode)
// Caller must free the returned string
XR_FUNC char *xcgen_emit_source(XcgenCompilation *comp);

/* ========== Module API ========== */

// Compile a single XIR function into a module
XR_FUNC XcgenFunc *xcgen_compile_func(XcgenModule *mod, XirFunc *xfunc, const char *c_name);

// Register an export in a module's export table
XR_FUNC void xcgen_module_add_export(XcgenModule *mod, const char *name, int shared_index,
                                     bool is_const);

// Free a single module (also called by xcgen_compilation_free)
XR_FUNC void xcgen_module_free(XcgenModule *mod);

/* ========== Internal API (used by xcgen_expr.c / xcgen_stmt.c) ========== */

// Type helpers
XR_FUNC const char *xcg_c_type(uint8_t xir_type);
XR_FUNC bool xcg_is_float_type(uint8_t xir_type);

// Get XIR type of a ref (vreg type or const type)
XR_FUNC uint8_t xcg_ref_type(XirFunc *func, XirRef ref);

// Emit a C expression for an XirRef
XR_FUNC void xcg_emit_ref(XcgenBuf *b, XirFunc *func, XirRef ref);

// Emit binary/compare/unary operations
XR_FUNC void xcg_emit_binary_op(XcgenBuf *b, XirFunc *func, XirIns *ins, const char *op);
XR_FUNC void xcg_emit_compare_op(XcgenBuf *b, XirFunc *func, XirIns *ins, const char *op);

// Resolve a constant int64 from an XIR ref, tracing through MOV chains (max 8 hops).
// Returns true if resolved, writing the value to *out_val.
XR_FUNC bool xcg_resolve_const_i64(XirFunc *func, XirRef ref, int64_t *out_val);

// Find the defining instruction for a vreg, tracing through MOV chains (max 8 hops).
// Returns the defining XirIns* or NULL if not found / not a vreg.
XR_FUNC XirIns *xcg_find_def(XirFunc *func, XirRef ref);

// Emit a single XIR instruction as C code
XR_FUNC void xcg_emit_instruction(XcgenBuf *b, XirFunc *func, XirIns *ins, const char *self_name,
                                  XcgenModule *mod, XcgenFunc *cf);

// Call translation (xcgen_call.c)
// Returns true if the instruction was handled as a call-related op
XR_FUNC bool xcg_emit_call_instruction(XcgenBuf *b, XirFunc *func, XirIns *ins,
                                       const char *self_name, XcgenModule *mod, XcgenFunc *cf);

// Lookup proto → C name in module mapping
XR_FUNC const char *xcg_lookup_proto_name(XcgenModule *mod, void *proto_ptr);
// Lookup proto → index in mod->funcs (-1 if not found/compiled yet)
XR_FUNC int xcg_lookup_proto_func_idx(XcgenModule *mod, void *proto_ptr);

// Emit phi copies for a control-flow edge
XR_FUNC void xcg_emit_phi_copies_for_edge(XcgenBuf *b, XirFunc *func, XirBlock *from, XirBlock *to);

// Emit block terminator (branch/jump/return)
XR_FUNC void xcg_emit_terminator(XcgenBuf *b, XirFunc *func, XirBlock *blk, const char *self_name,
                                 XcgenFunc *cf);

/* ========== Shared Emit Helpers ========== */

// Auto-box int64_t/double vregs to XrValue for runtime call arguments
XR_FUNC void xcg_emit_ref_as_tagged(XcgenBuf *b, XirFunc *func, XirRef ref);

/* ========== Intrinsic Lowering (xcgen_intrinsic.c) ========== */

// Emit C code for a CALL_INTRINSIC instruction
XR_FUNC void xcg_emit_call_intrinsic(XcgenBuf *b, XirFunc *func, XirIns *ins, XcgenFunc *cf,
                                     XcgenModule *mod);

/* ========== Prescan / Analysis Passes (xcgen_prescan.c) ========== */

// Block reachability: BFS from entry to find live blocks
XR_FUNC void xcg_compute_reachable(XirFunc *func, bool *reachable);

// Dead vreg elimination: seed-then-propagate used-vreg analysis
XR_FUNC void xcg_compute_used_vregs(XirFunc *func, bool *reachable, bool *used);

// Detect struct-promoted vregs (JSON shapes → C struct pointers)
XR_FUNC void xcg_prescan_struct_vregs(XcgenModule *mod, XcgenFunc *cf);

// Retype LOAD_FIELD results from narrow I64/F64 to TAGGED (16-byte slots)
XR_FUNC void xcg_retype_field_loads(XcgenFunc *cf);

// Retype boolean method results to TAGGED so print preserves true/false
XR_FUNC void xcg_retype_bool_method_results(XcgenFunc *cf);

// Returns true if every return in the function returns null → emit as void
XR_FUNC bool xcg_detect_void_return(XirFunc *func);

// Lookup proto entry by pointer in global compilation registry
XR_FUNC XcgenProtoEntry *xcg_lookup_proto_entry(XcgenModule *mod, void *proto_ptr);

// Escape analysis: mark non-escaping child closures in proto_map
XR_FUNC void xcg_prescan_closure_escape(XcgenModule *mod, XirFunc *func);

#endif  // XCGEN_H
