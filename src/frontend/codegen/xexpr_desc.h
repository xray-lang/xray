/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xexpr_desc.h - Expression descriptor for smart register allocation
 *
 * KEY CONCEPT:
 *   Delayed evaluation: records expression info instead of immediate code gen.
 *   Smart reuse: automatically determines register reusability.
 *   Zero overhead: temp values modified in-place, locals auto-protected.
 */

#ifndef XEXPR_DESC_H
#define XEXPR_DESC_H

#include <stdint.h>
#include <stdbool.h>
#include "../../base/xdefs.h"
#include "../../runtime/value/xtype.h"

// Forward declarations
typedef struct XrCompiler XrCompiler;
typedef struct XrCompilerContext XrCompilerContext;

// Expression types
typedef enum {
    // Constant values
    XEXPR_VOID,    // No value (empty expression)
    XEXPR_NULL,    // null constant
    XEXPR_TRUE,    // true constant
    XEXPR_FALSE,   // false constant
    XEXPR_INT,     // Integer constant (uses ival)
    XEXPR_FLOAT,   // Float constant (uses nval)
    XEXPR_NUMBER,  // Number constant (in constant table, uses const_idx)
    XEXPR_STRING,  // String constant

    // Values in registers
    XEXPR_TEMP,   // Temporary value (can be overwritten in-place)
    XEXPR_LOCAL,  // Local variable (needs protection)
    XEXPR_PARAM,  // Function parameter (needs protection)

    // Special expressions
    XEXPR_GLOBAL,   // Global variable (reload each time)
    XEXPR_UPVAL,    // Upvalue
    XEXPR_INDEXED,  // Table/array index
    XEXPR_CALL,     // Function call (return value)

    // Jump-related (for short-circuit evaluation)
    XEXPR_JMP,    // Expression with jumps
    XEXPR_RELOC,  // Relocatable expression
} XExprKind;

// Expression descriptor
//
// Core idea:
// 1. Record expression "state" instead of generating code immediately
// 2. Choose optimal code generation strategy based on usage context
// 3. Distinguish "overwritable" and "protected" registers
typedef struct XrExprDesc {
    XExprKind kind;  // Expression type
    int reg;         // Register number (-1 means not in register)

    // Constant value (different fields used based on kind)
    union {
        int64_t ival;    // XEXPR_NUMBER (integer)
        double nval;     // XEXPR_NUMBER (float)
        int symbol;      // XEXPR_STRING (symbol)
        int const_idx;   // Constant table index
        int global_idx;  // XEXPR_GLOBAL (global variable index)
        int pc;          // XEXPR_RELOC (instruction position for patching)
    } u;

    // Jump lists (for short-circuit evaluation)
    int t;  // True jump list head
    int f;  // False jump list head

    // Metadata
    bool has_jumps;  // Has pending jumps
    bool is_const;   // Is compile-time constant
    bool is_raw;     // Register holds raw (unboxed) I64/F64 value

    // Compile-time type: single source of truth for semantic type.
    // NULL = unknown / tagged fallback (XrValue). Drives inst_types[pc]
    // propagation. Combined with is_raw for BOX/UNBOX decisions.
    struct XrType *compile_type;  // NULL = unknown / tagged fallback
} XrExprDesc;

/* ========== Compile-Type Helpers ========== */

// Check if expression holds a raw (unboxed) int64 value.
static inline bool xexpr_is_raw_i64(const XrExprDesc *e) {
    return e->is_raw && e->compile_type && e->compile_type->kind == XR_KIND_INT;
}

// Check if expression holds a raw (unboxed) float64 value.
static inline bool xexpr_is_raw_f64(const XrExprDesc *e) {
    return e->is_raw && e->compile_type && e->compile_type->kind == XR_KIND_FLOAT;
}

// Check if expression holds any raw (unboxed) numeric value.
static inline bool xexpr_is_raw(const XrExprDesc *e) {
    return e->is_raw;
}

// Clear raw flag after BOX (value is now tagged).
static inline void xexpr_clear_raw(XrExprDesc *e) {
    e->is_raw = false;
}

// Mark expression as holding a raw value.
static inline void xexpr_set_raw(XrExprDesc *e) {
    e->is_raw = true;
}

/* ========== Initialization and Basic Operations ========== */

// Initialize expression descriptor
XR_FUNC void xexpr_init(XrExprDesc *e, XExprKind kind, int reg);

// Initialize as void expression
XR_FUNC void xexpr_init_void(XrExprDesc *e);

// Initialize as constant
XR_FUNC void xexpr_init_int(XrExprDesc *e, int64_t val);
XR_FUNC void xexpr_init_number(XrExprDesc *e, double val);
XR_FUNC void xexpr_init_bool(XrExprDesc *e, bool val);
XR_FUNC void xexpr_init_null(XrExprDesc *e);

/* ========== Core API: Smart Register Allocation ========== */
/*
 * RAW/TAGGED VALUE BOUNDARY:
 *
 *   xexpr_to_anyreg()          — tagged consumer (auto-BOX raw→tagged)
 *   xexpr_to_anyreg_readonly() — raw consumer    (preserves raw format)
 *
 * Rule: native _I64/_F64 ops use readonly.
 *       XrValue consumers (OP_ADD, closures, templates) use anyreg.
 */

// Tagged consumer: auto-BOX raw I64/F64 values at the raw-to-tagged boundary.
// Returns a register guaranteed to hold a tagged XrValue.
XR_FUNC int xexpr_to_anyreg(XrCompilerContext *ctx, XrCompiler *compiler, XrExprDesc *e);

// Typed consumer (readonly): preserves raw format, zero overhead for LOCAL/PARAM.
// Returns the original register — may contain raw int64/double.
// Caller must check e->compile_type if instruction format matters.
XR_FUNC int xexpr_to_anyreg_readonly(XrCompilerContext *ctx, XrCompiler *compiler, XrExprDesc *e);

// Put expression into next available register
//
// Always allocates new register, ensures no existing value is overwritten
//
// @return newly allocated register number
XR_FUNC int xexpr_to_nextreg(XrCompilerContext *ctx, XrCompiler *compiler, XrExprDesc *e);

// Ensure expression is in some register
//
// Doesn't guarantee which register, but guarantees in a register
XR_FUNC void xexpr_to_reg(XrCompilerContext *ctx, XrCompiler *compiler, XrExprDesc *e);

// Discharge expression to specific register
// If expression is in another register, generates MOVE instruction
// If XEXPR_RELOC, patches instruction's A field
XR_FUNC void xexpr_to_specific_reg(XrCompilerContext *ctx, XrCompiler *compiler, XrExprDesc *e,
                                   int target_reg);

// Check if expression is relocatable
XR_FUNC bool xexpr_is_relocatable(const XrExprDesc *e);

/* ========== Expression Analysis ========== */

// Can expression modify register in-place
//
// Returns true if:
// - XEXPR_TEMP without jumps
// - XEXPR_CALL (function call return value is temporary)
//
// @return true means register can be reused
XR_FUNC bool xexpr_can_reuse_reg(const XrExprDesc *e);

// Does expression need protection
//
// Returns true if:
// - XEXPR_LOCAL (local variable)
// - XEXPR_PARAM (function parameter)
//
// @return true means cannot be overwritten
XR_FUNC bool xexpr_needs_protect(const XrExprDesc *e);

// Is expression in a register
XR_FUNC bool xexpr_in_register(const XrExprDesc *e);

// Does expression have jumps
XR_FUNC bool xexpr_has_jumps(const XrExprDesc *e);

/* ========== Resource Management ========== */

// Free temporary resources occupied by expression
//
// Note: does not free registers occupied by locals and parameters
XR_FUNC void xexpr_free(XrCompiler *compiler, XrExprDesc *e);

// Ensure expression value has generated code
//
// If expression not yet "materialized" (e.g. XEXPR_RELOC), generate necessary code
XR_FUNC void xexpr_discharge(XrCompilerContext *ctx, XrCompiler *compiler, XrExprDesc *e);

/* ========== Conditional Jumps ========== */

// Jump when condition is true
//
// Generates: TEST reg, 0, 0; JMP target
// Internally frees expression's temporary register
//
// @param e condition expression (already compiled)
// @return JMP instruction position for later patching
XR_FUNC int xexpr_goiftrue(XrCompilerContext *ctx, XrCompiler *compiler, XrExprDesc *e);

/* ========== Type Conversion ========== */

// Ensure register value is tagged for generic instructions.
// If expression is raw-typed (I64/F64), BOX to a new temp register.
// Returns the (possibly new) register number.
XR_FUNC int xexpr_ensure_boxed(XrCompilerContext *ctx, XrCompiler *compiler, XrExprDesc *e,
                               int reg);

/* ========== Helper Functions ========== */

// Get expression kind name (for debugging)
XR_FUNC const char *xexpr_kind_name(XExprKind kind);

// Print expression descriptor (for debugging)
XR_FUNC void xexpr_debug_print(const XrExprDesc *e);

// Copy expression descriptor
XR_FUNC void xexpr_copy(XrExprDesc *dest, const XrExprDesc *src);

#endif  // XEXPR_DESC_H
