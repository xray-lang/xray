/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xchunk.h - Bytecode chunk for register-based VM
 *
 * KEY CONCEPT:
 *   64-bit instruction encoding with multiple formats.
 *   Supports up to 65536 opcodes/register operands and 32-bit Bx payloads.
 *
 * INSTRUCTION FORMATS:
 *    63           47           31           15            0
 *    +------------+------------+------------+-------------+
 *    |      C     |      B     |      A     |     OP      | iABC
 *    +------------+------------+------------+-------------+
 *    |            Bx           |      A     |     OP      | iABx
 *    +------------+------------+------------+-------------+
 *    |            sBx          |      A     |     OP      | iAsBx
 *    +------------+------------+------------+-------------+
 *    |                  Ax                  |     OP      | iAx
 *    +------------+------------+------------+-------------+
 *    |                  sJ                  |     OP      | isJ
 *    +------------+------------+------------+-------------+
 *
 * FIELD SIZES:
 *   OP:  16-bit (65536 opcodes)
 *   A:   16-bit
 *   B:   16-bit
 *   C:   16-bit
 *   Bx:  32-bit
 *   sBx: 32-bit biased signed (-2147483647 to 2147483647)
 *   Ax:  48-bit
 *   sJ:  48-bit biased signed jump offset
 */

#ifndef XCHUNK_H
#define XCHUNK_H

#include "xvalue.h"
#include "../../base/xdynarray.h"
#include <stdint.h>
#include "../../base/xdefs.h"

typedef struct XrString XrString;

struct XrICMethodTable;
struct XrICFieldTable;

/* ========== 64-bit Instruction Type ========== */

typedef uint64_t XrInstruction;

/* ========== Instruction Format ========== */

#define SIZE_OP 16
#define SIZE_A 16
#define SIZE_B 16
#define SIZE_C 16
#define SIZE_Bx 32
#define SIZE_Ax 48

#define XR_BC_OP_MASK 0xFFFFull
#define XR_BC_ARG_MASK 0xFFFFull
#define XR_BC_BX_MASK 0xFFFFFFFFull
#define XR_BC_AX_MASK 0xFFFFFFFFFFFFull

#define MAXARG_A ((uint32_t) XR_BC_ARG_MASK)
#define MAXARG_B ((uint32_t) XR_BC_ARG_MASK)
#define MAXARG_C ((uint32_t) XR_BC_ARG_MASK)
#define MAXARG_Bx ((uint64_t) XR_BC_BX_MASK)
#define MAXARG_sBx ((int32_t) (MAXARG_Bx >> 1))
#define MAXARG_Ax ((uint64_t) XR_BC_AX_MASK)
#define MAXARG_sJ ((int64_t) ((1ull << (SIZE_Ax - 1)) - 1ull))

// OP_LOADI immediate range (sBx format, bias=MAXARG_sBx)
// Representable signed range: [-MAXARG_sBx, MAXARG_sBx]
#define LOADI_MAX MAXARG_sBx
#define LOADI_MIN (-MAXARG_sBx)

/* ========== Opcode Enum ========== */

#include "xopcode_def.h"

/*
 * The OpCode enum body is generated from XR_OPCODE_TABLE in
 * xopcode_def.h -- the single source of truth shared with the
 * disassembler info table (xopcode_info.c) and the computed-goto
 * label array (xvm_jumptab.h). Adding a new opcode means editing
 * one X-macro entry there; the three consumers stay in lockstep.
 */
typedef enum {
#define _XR_OPCODE_ENUM(name, fmt, kop, desc) OP_##name,
    XR_OPCODE_TABLE(_XR_OPCODE_ENUM)
#undef _XR_OPCODE_ENUM
} OpCode;

// Opcode count (instruction encoding uses 16-bit opcode field, max 65536)
#define NUM_OPCODES (OP_NOP + 1)
_Static_assert(NUM_OPCODES <= 65536, "Opcode count exceeds 16-bit encoding limit (max 65536)");

/* ========== Sub-opcode Constants ========== */

// OP_CORO_CTRL sub-opcodes (C field)
#define CORO_CTRL_STATS 0
#define CORO_CTRL_LIST 1
#define CORO_CTRL_INFO 2
#define CORO_CTRL_DUMP 3
#define CORO_CTRL_STALLED 4
#define CORO_CTRL_DEADLOCKS 5
#define CORO_CTRL_TOP 6
#define CORO_CTRL_GROUP_BY 7
// Registry/lifecycle (merged from stdlib/coro)
#define CORO_CTRL_WHEREIS 8
#define CORO_CTRL_MONITOR 9
#define CORO_CTRL_DEMONITOR 10
#define CORO_CTRL_SELF 11
#define CORO_CTRL_KILL 12

// OP_JSON_INIT mode flags (encoded in high bits of C)
#define JSON_INIT_REG 0   // C = register index
#define JSON_INIT_IMM 1   // C = signed immediate int (use GETARG_sC)
#define JSON_INIT_NULL 2  // C ignored, value = null

/* ========== Instruction Encode/Decode Macros ========== */
#define GET_OPCODE(i) ((OpCode) ((i) & XR_BC_OP_MASK))

// Create instruction
// Note: Use 64-bit unsigned constants to avoid signed integer overflow
// when left-shifting. This is important for UBSan compliance.
#define CREATE_ABC(op, a, b, c)                                                                    \
    ((XrInstruction) (((uint64_t) (op) & XR_BC_OP_MASK) |                                          \
                      (((uint64_t) (a) & XR_BC_ARG_MASK) << 16) |                                  \
                      (((uint64_t) (b) & XR_BC_ARG_MASK) << 32) |                                  \
                      (((uint64_t) (c) & XR_BC_ARG_MASK) << 48)))

#define CREATE_ABx(op, a, bx)                                                                      \
    ((XrInstruction) (((uint64_t) (op) & XR_BC_OP_MASK) |                                          \
                      (((uint64_t) (a) & XR_BC_ARG_MASK) << 16) |                                  \
                      (((uint64_t) (bx) & XR_BC_BX_MASK) << 32)))

#define CREATE_AsBx(op, a, sbx) CREATE_ABx(op, a, (uint64_t) ((int64_t) (sbx) + MAXARG_sBx))

#define CREATE_Ax(op, ax)                                                                          \
    ((XrInstruction) (((uint64_t) (op) & XR_BC_OP_MASK) |                                          \
                      (((uint64_t) (ax) & XR_BC_AX_MASK) << 16)))

#define CREATE_sJ(op, sj) CREATE_Ax(op, (uint64_t) ((int64_t) (sj) + MAXARG_sJ))

// Extract arguments
#define GETARG_A(i) ((uint32_t) (((i) >> 16) & XR_BC_ARG_MASK))
#define GETARG_B(i) ((uint32_t) (((i) >> 32) & XR_BC_ARG_MASK))
#define GETARG_C(i) ((uint32_t) (((i) >> 48) & XR_BC_ARG_MASK))
#define GETARG_sB(i) ((int16_t) (GETARG_B(i)))  // signed B
#define GETARG_sC(i) ((int16_t) (GETARG_C(i)))  // signed C
#define GETARG_Bx(i) ((uint32_t) (((i) >> 32) & XR_BC_BX_MASK))
#define GETARG_sBx(i) ((int64_t) GETARG_Bx(i) - (int64_t) MAXARG_sBx)
#define GETARG_Ax(i) (((uint64_t) ((i) >> 16)) & XR_BC_AX_MASK)
#define GETARG_sJ(i) ((int64_t) GETARG_Ax(i) - (int64_t) MAXARG_sJ)

// Set arguments (modify instruction)
// Note: Use unsigned constants to avoid signed integer overflow.
#define SETARG_A(i, v)                                                                             \
    ((i) = ((i) & ~(XrInstruction) (XR_BC_ARG_MASK << 16)) |                                       \
           (XrInstruction) (((uint64_t) (v) & XR_BC_ARG_MASK) << 16))
#define SETARG_B(i, v)                                                                             \
    ((i) = ((i) & ~(XrInstruction) (XR_BC_ARG_MASK << 32)) |                                       \
           (XrInstruction) (((uint64_t) (v) & XR_BC_ARG_MASK) << 32))
#define SETARG_C(i, v)                                                                             \
    ((i) = ((i) & ~(XrInstruction) (XR_BC_ARG_MASK << 48)) |                                       \
           (XrInstruction) (((uint64_t) (v) & XR_BC_ARG_MASK) << 48))
#define SETARG_Bx(i, v)                                                                            \
    ((i) = ((i) & ~(XrInstruction) (XR_BC_BX_MASK << 32)) |                                        \
           (XrInstruction) (((uint64_t) (v) & XR_BC_BX_MASK) << 32))

/* ========== Constant Table ========== */

/*
** Constant array (using new dynamic array implementation)
** Preserve compatible interface, use XrDynArray internally
*/
typedef XrDynArray ValueArray;

// Constant table operations
XR_FUNC void xr_valuearray_init(ValueArray *array);
XR_FUNC void xr_valuearray_free(ValueArray *array);
XR_FUNC int xr_valuearray_add(ValueArray *array, XrValue value);

// Convenience macros
#define VALUEARRAY_GET(arr, index) DYNARRAY_GET(arr, index, XrValue)
#define VALUEARRAY_COUNT(arr) DYNARRAY_COUNT(arr)

/* ========== Function Prototype (XrProto) ========== */
// Upvalue source: where OP_CLOSURE reads the initial value from
#define UPVAL_SRC_UPVAL 1  // from enclosing closure's upvals[] (transitive)
#define UPVAL_SRC_REG 2    // from enclosing frame's register (direct capture)

typedef struct UpvalInfo {
    uint16_t index;            // SRC_REG: register number; SRC_UPVAL: enclosing upval index
    uint8_t storage_mode;      // 0=normal, 1=shared
    uint8_t is_const;          // const variable flag
    uint8_t slot_type;         // XrSlotType: storage class for GC traversal
    uint8_t source;            // UPVAL_SRC_REG or UPVAL_SRC_UPVAL
    struct XrType *type_info;  // full compile-time type (NULL = unknown/any)
} UpvalInfo;

// Local variable debug info
typedef struct XrLocVar {
    const char *name;  // variable name
    int start_pc;      // scope start instruction index
    int end_pc;        // scope end instruction index (-1 = not ended)
    int reg;           // register number
} XrLocVar;

// entry_type values for XrProto.entry_type
#define XR_ENTRY_NORMAL 0     // plain function: no default params, no generator
#define XR_ENTRY_DEFAULTS 1   // has default parameters (fill missing args with null)
#define XR_ENTRY_GENERATOR 2  // generator function (supports yield)

// Function prototype (compiled function)
typedef struct XrProto {
    XrDynArray code;          // bytecode array
    ValueArray constants;     // constant pool
    XrDynArray protos;        // nested functions
    XrDynArray upvalues;      // upvalue info
    XrDynArray lineinfo;      // line number info
    XrDynArray locvars;       // local variable names
    const char *source_file;  // source file path

    /*
     * Per-function symbol table: maps local index to global SymbolId.
     * Instructions encode local indices in 16-bit B/C fields.
     * VM dereferences: global_sym = proto->symbols[local_idx]
     *
     * WHY THIS DESIGN:
     *   Global symbol IDs are process-wide and can grow independently
     *   of a function's bytecode operands.  Instructions therefore carry
     *   compact per-function symbol indices, while this table maps them
     *   back to global SymbolId values.
     */
    int32_t *symbols;     // local-to-global symbol mapping
    int symbol_count;     // number of symbols used
    int symbol_capacity;  // allocated capacity

    XrString *name;             // function name
    char *return_type;          // return type (NULL = unspecified)
    int maxstacksize;           // max stack (register) size
    int numparams;              // parameter count
    int min_params;             // minimum required params (for default params)
    int num_globals;            // global variable count
    uint16_t struct_area_size;  // bytes needed for struct_area in stack frame (0 = none)

    int shared_offset;  // per-module shared variable offset into isolate->vm.shared
    bool is_vararg;     // is variadic function

    // Entry type: controls VM function setup (skips irrelevant init code)
    // 0=normal, 1=has_defaults (fill missing params), 2=generator (yield support)
    uint8_t entry_type;

    /*
     * Monotonic proto identifier assigned at creation. Used as the index
     * into per-coroutine inline-cache tables (see XrVMContext.ic_*_tables).
     * Inline caches live ctx-side to keep XrProto immutable across workers;
     * proto_id is the bridge between an immutable bytecode unit and the
     * mutable IC slot it owns inside each coroutine.
     */
    uint32_t proto_id;

    uint8_t test_attr;  // test attribute type
    int test_timeout;   // test timeout (seconds)
    bool is_coro_safe;  // safe to call in coroutine

    // bit[i]=1: TFIELD field index i is F64. Typed-compilation metadata set by
    // the compiler; consumed by AOT codegen and VM typed struct access.
    uint64_t tfield_float_bitmap;

    /*
     * Per-parameter type annotations (authoritative source for param types).
     * param_types[i] = XrType* for parameter i (NULL = untyped/any).
     * Typed-compilation metadata generated by the Xi pipeline during lowering;
     * consumed by AOT codegen and VM typed value access.
     */
    struct XrType **param_types;  // [numparams] parameter types
    uint8_t param_types_count;    // = numparams when allocated

    /*
     * Per-instruction type annotations (flow-sensitive, authoritative for non-params).
     * inst_types[pc] = XrType* for the result of instruction at pc (NULL = untyped).
     * Generated by codegen from compile_type at each emit site; consumed by AOT
     * struct inference.
     */
    struct XrType **inst_types;  // [code_count] per-PC result types
    uint32_t inst_types_count;   // = code_count when allocated

    struct XrType *return_type_info;  // full return type (NULL = void/any)

    // Parent proto: set by xr_vm_proto_add_proto when this proto is added as a
    // child. Used to walk up to the module root and build the shared_protos
    // mapping for CALL_KNOWN optimization.
    struct XrProto *enclosing;

    // Retained Xi SSA IR from compilation (XiFunc*), consumed by AOT/REPL
    // lowering. Freed in xr_proto_free(). NULL if the Xi pipeline was not used.
    void *xi_func;  // opaque XiFunc* (owned, freed via xi_func_free)
} XrProto;

// Convenience macros
#define PROTO_CODE(p, idx) DYNARRAY_GET(&(p)->code, idx, XrInstruction)
#define PROTO_CODE_PTR(p, idx) DYNARRAY_GET_PTR(&(p)->code, idx, XrInstruction)
#define PROTO_CODE_BASE(p) ((XrInstruction *) ((p)->code.data))
#define PROTO_SET_CODE(p, idx, v) DYNARRAY_SET(&(p)->code, idx, v, XrInstruction)
#define PROTO_CONSTANT(p, idx) DYNARRAY_GET(&(p)->constants, idx, XrValue)
// Direct constant access (hot path optimization)
#define PROTO_CONST_BASE(p) ((XrValue *) ((p)->constants.data))
#define PROTO_CONST_FAST(p, idx) (PROTO_CONST_BASE(p)[idx])
#define PROTO_PROTO(p, idx) DYNARRAY_GET(&(p)->protos, idx, XrProto *)
#define PROTO_UPVALUE(p, idx) DYNARRAY_GET(&(p)->upvalues, idx, UpvalInfo)
#define PROTO_LINE(p, idx) DYNARRAY_GET(&(p)->lineinfo, idx, int)

#define PROTO_SYMBOL(p, idx) ((p)->symbols[idx])
#define PROTO_SYMBOL_COUNT(p) ((p)->symbol_count)

#define PROTO_CODE_COUNT(p) DYNARRAY_COUNT(&(p)->code)
#define PROTO_CONST_COUNT(p) DYNARRAY_COUNT(&(p)->constants)
#define PROTO_PROTO_COUNT(p) DYNARRAY_COUNT(&(p)->protos)
#define PROTO_UPVAL_COUNT(p) DYNARRAY_COUNT(&(p)->upvalues)
#define PROTO_LINE_COUNT(p) DYNARRAY_COUNT(&(p)->lineinfo)
#define PROTO_LOCVAR(p, idx) DYNARRAY_GET(&(p)->locvars, idx, XrLocVar)
#define PROTO_LOCVAR_COUNT(p) DYNARRAY_COUNT(&(p)->locvars)

// XrProto Operations
XR_FUNC XrProto *xr_vm_proto_new(void);
XR_FUNC void xr_vm_proto_free(XrProto *proto);

// Bytecode Operations
XR_FUNC void xr_vm_proto_write(XrProto *proto, XrInstruction inst, int line);
XR_FUNC int xr_vm_proto_add_constant(XrProto *proto, XrValue value);
XR_FUNC int xr_vm_proto_add_proto(XrProto *proto, XrProto *child);
XR_FUNC int xr_vm_proto_add_upvalue(XrProto *proto, uint16_t index, uint8_t storage_mode,
                                    uint8_t is_const, uint8_t slot_type, uint8_t source,
                                    struct XrType *type_info);
XR_FUNC int xr_proto_add_symbol(XrProto *proto, int32_t global_symbol);

/* ========== Debug Helpers ========== */
XR_FUNC const char *xr_opcode_name(OpCode op);

#endif  // XCHUNK_H
