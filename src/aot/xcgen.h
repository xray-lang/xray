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
    char  *data;
    size_t len;
    size_t cap;
} XcgenBuf;

XR_FUNC void xcgen_buf_init(XcgenBuf *b);
XR_FUNC void xcgen_buf_free(XcgenBuf *b);
XR_FUNC void xcgen_buf_ensure(XcgenBuf *b, size_t extra);
XR_FUNC void xcgen_buf_puts(XcgenBuf *b, const char *s);
XR_FUNC void xcgen_buf_append(XcgenBuf *dst, const XcgenBuf *src);

__attribute__((format(printf, 2, 3)))
XR_FUNC void xcgen_buf_printf(XcgenBuf *b, const char *fmt, ...);

/* ========== C File Sections ========== */

typedef enum {
    XCGEN_SEC_HEADERS,       // #include directives
    XCGEN_SEC_FORWARD,       // forward declarations
    XCGEN_SEC_TYPES,         // struct/typedef
    XCGEN_SEC_DATA,          // static data (string literals, constants)
    XCGEN_SEC_FUNCS,         // function bodies
    XCGEN_SEC_MAIN,          // main() wrapper
    XCGEN_SEC_COUNT,
} XcgenSection;

/* ========== Proto → C Name Mapping ========== */

#define XCGEN_MAX_CALL_ARGS  65  // initial capacity: slot 0 (closure) + 64 args

typedef struct {
    void       *proto_ptr;   // XrProto* (opaque, compared by address)
    const char *c_name;      // C function name (e.g. "xr_fib")
    int         func_idx;    // index into mod->funcs (-1 = not yet compiled)
    bool        non_escaping; // true if closure only called via CALL_KNOWN (never stored/returned)
    int         num_upvals;   // upvalue count for non-escaping closures (0 if escaping)
} XcgenProtoEntry;

/* ========== Per-Function Codegen State ========== */

typedef struct XcgenFunc {
    XirFunc       *xfunc;          // source XIR function (may be freed after compile)
    const char    *c_name;         // generated C function name (e.g. "xr_fib")
    int            num_params;     // cached from xfunc->num_params (valid after xfunc freed)
    XcgenBuf       body;           // function body buffer
    int            tmp_count;      // temp variable counter
    bool           needs_runtime;  // true if function calls runtime APIs
    bool           needs_gc;       // true if function allocates GC objects
    bool           needs_closure_param; // true if function accesses upvalues (needs XrtValue xrt_closure param)
    bool           non_escaping;        // true if all callers pass upvalues inline (no closure object)
    int            num_upvals;          // upvalue count for non-escaping closures (upval params after regular params)
    bool           needs_exception;     // true if function uses try/catch (needs XrtValue xrt_exception local)
    bool           void_return;    // true if function always returns null → emit as void

    // Dead code elimination: which vregs are actually read
    bool          *used_vregs;

    // Struct promotion: vreg → struct index (-1 = not a struct)
    int16_t       *vreg_struct_id;  // [nvreg] array, -1 = not promoted

    // Call args buffer: tracks STORE_CORO writes to jit_call_args[]
    // Used by CALL_KNOWN to reconstruct direct C function calls.
    // Dynamically grown when a function has more than 64 parameters.
    XirRef        *call_args;       // heap-allocated, capacity = call_args_cap
    int            call_args_cap;   // current allocated size
    int            call_args_count;

    // Shadow stack: number of XrtValue locals registered for GC scanning
    int            shadow_stack_count;
} XcgenFunc;

/* ========== Forward Declarations ========== */

typedef struct XcgenCompilation XcgenCompilation;

/* ========== Module-Level Export Entry ========== */

typedef struct XcgenExport {
    const char *name;       // export name (e.g. "pi", "add")
    const char *c_var;      // C global variable name (e.g. "mod_math__pi")
    int         shared_index; // index into xrt_shared[] array (-1 = named global)
    bool        is_const;   // true if const export
} XcgenExport;

/* ========== Module-Level Codegen State ========== */

typedef struct XcgenModule {
    // Module metadata
    const char    *module_name;     // e.g. "math", "./utils"
    const char    *module_path;     // absolute path
    int16_t        module_id;       // unique id within compilation

    XcgenBuf       sections[XCGEN_SEC_COUNT];
    XcgenFunc     *funcs;          // compiled functions in this module
    int            nfuncs;
    int            funcs_cap;

    // Module-level exports (populated during GETSHARED/SETSHARED scan)
    XcgenExport   *exports;
    int            nexports;
    int            exports_cap;

    // Struct promotion registry (non-owning ptr → comp->struct_reg)
    XcgenStructRegistry *struct_reg;

    // Compilation flags
    bool           emit_debug;     // true = #line directives

    // Backpointer to parent compilation context
    XcgenCompilation *comp;
} XcgenModule;

/* ========== Top-Level Compilation Context ========== */

struct XcgenCompilation {
    XcgenModule   **modules;       // array of module pointers
    int             nmodules;
    int             modules_cap;

    // Global proto registry (cross-module function lookup)
    XcgenProtoEntry *proto_map;
    int              proto_map_count;
    int              proto_map_cap;

    // Global struct promotion registry (owned externally by cmd_build)
    XcgenStructRegistry *struct_reg;

    // Shared variable tracking (for GETSHARED/SETSHARED → C globals)
    int             max_shared_index;  // highest shared index seen (-1 = none)

    // Output configuration
    bool            emit_debug;     // true = #line directives
    bool            single_file;    // true = combine all modules into one .c
};

/* ========== Compilation API ========== */

// Create a new compilation context
XR_FUNC XcgenCompilation *xcgen_compilation_new(void);

// Free compilation and all owned modules
XR_FUNC void xcgen_compilation_free(XcgenCompilation *comp);

// Add a module to the compilation; returns the module pointer
XR_FUNC XcgenModule *xcgen_compilation_add_module(XcgenCompilation *comp,
                                                   const char *name,
                                                   const char *path);

// Register a proto → C name mapping in the global registry
XR_FUNC void xcgen_register_proto(XcgenCompilation *comp, void *proto_ptr,
                                   const char *c_name);

// Emit combined C source for all modules (single-file mode)
// Caller must free the returned string
XR_FUNC char *xcgen_emit_source(XcgenCompilation *comp);

/* ========== Module API ========== */

// Compile a single XIR function into a module
XR_FUNC XcgenFunc *xcgen_compile_func(XcgenModule *mod, XirFunc *xfunc,
                                       const char *c_name);

// Register an export in a module's export table
XR_FUNC void xcgen_module_add_export(XcgenModule *mod, const char *name,
                                      int shared_index, bool is_const);

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

// Emit a single XIR instruction as C code
XR_FUNC void xcg_emit_instruction(XcgenBuf *b, XirFunc *func, XirIns *ins,
                           const char *self_name, XcgenModule *mod,
                           XcgenFunc *cf);

// Call translation (xcgen_call.c)
// Returns true if the instruction was handled as a call-related op
XR_FUNC bool xcg_emit_call_instruction(XcgenBuf *b, XirFunc *func, XirIns *ins,
                                const char *self_name, XcgenModule *mod,
                                XcgenFunc *cf);

// Lookup proto → C name in module mapping
XR_FUNC const char *xcg_lookup_proto_name(XcgenModule *mod, void *proto_ptr);
// Lookup proto → index in mod->funcs (-1 if not found/compiled yet)
XR_FUNC int xcg_lookup_proto_func_idx(XcgenModule *mod, void *proto_ptr);
// Lookup proto → compiled XcgenFunc (for struct param info); may return NULL
// NOTE: always re-derives the pointer from mod->funcs to avoid dangling refs after realloc
XR_FUNC XcgenFunc *xcg_lookup_proto_cf(XcgenModule *mod, void *proto_ptr);

// Emit phi copies for a control-flow edge
XR_FUNC void xcg_emit_phi_copies_for_edge(XcgenBuf *b, XirFunc *func,
                                   XirBlock *from, XirBlock *to);

// Emit block terminator (branch/jump/return)
XR_FUNC void xcg_emit_terminator(XcgenBuf *b, XirFunc *func, XirBlock *blk,
                          const char *self_name, XcgenFunc *cf);

#endif // XCGEN_H
