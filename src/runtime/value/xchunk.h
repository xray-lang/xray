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
 *   32-bit instruction encoding with multiple formats.
 *   Supports up to 256 opcodes, 256 registers.
 *
 * INSTRUCTION FORMATS:
 *    31           23           15            7    0
 *    +------------+------------+------------+----+
 *    |      C     |      B     |      A     | OP | iABC
 *    +------------+------------+------------+----+
 *    |            Bx           |      A     | OP | iABx
 *    +------------+------------+------------+----+
 *    |            sBx          |      A     | OP | iAsBx
 *    +------------+------------+------------+----+
 *    |            Ax                        | OP | iAx
 *    +------------+------------+------------+----+
 *    |            sJ                        | OP | isJ
 *    +------------+------------+------------+----+
 *
 * FIELD SIZES:
 *   OP:  8-bit  (256 opcodes)
 *   A:   8-bit  (256 registers)
 *   B:   8-bit
 *   C:   8-bit
 *   Bx:  16-bit (65536 constants)
 *   sBx: 16-bit signed (-32768 to 32767)
 *   Ax:  24-bit
 *   sJ:  24-bit signed (jump offset)
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

/* ========== 32-bit Instruction Type ========== */

typedef uint32_t XrInstruction;

/* ========== Instruction Format ========== */

#define SIZE_OP 8
#define SIZE_A 8
#define SIZE_B 8
#define SIZE_C 8
#define SIZE_Bx 16
#define SIZE_Ax 24

#define MAXARG_A ((1 << SIZE_A) - 1)
#define MAXARG_B ((1 << SIZE_B) - 1)
#define MAXARG_C ((1 << SIZE_C) - 1)
#define MAXARG_Bx ((1 << SIZE_Bx) - 1)
#define MAXARG_sBx (MAXARG_Bx >> 1)
#define MAXARG_Ax ((1 << SIZE_Ax) - 1)
#define MAXARG_sJ ((1 << (SIZE_Ax - 1)) - 1)

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

// Opcode count (instruction encoding uses 8-bit opcode field, max 256)
#define NUM_OPCODES (OP_NOP + 1)
_Static_assert(NUM_OPCODES <= 256, "Opcode count exceeds 8-bit encoding limit (max 256)");

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

/* ========== OP_INVOKE_BUILTIN Type Hints ========== */

typedef enum {
    BUILTIN_TYPE_MAP = 0,
    BUILTIN_TYPE_ARRAY = 1,
    BUILTIN_TYPE_STRING = 2,
    BUILTIN_TYPE_SET = 3,
    BUILTIN_TYPE_INT = 4,
    BUILTIN_TYPE_FLOAT = 5,
} BuiltinTypeHint;

/* ========== Instruction Encode/Decode Macros ========== */
#define GET_OPCODE(i) ((OpCode) ((i) & 0xFFu))

// Create instruction
// Note: Use unsigned constants (0xFFu, 0xFFFFu) to avoid signed integer overflow
// when left-shifting. This is important for UBSan compliance.
#define CREATE_ABC(op, a, b, c)                                                                    \
    ((XrInstruction) (((op) & 0xFFu) | (((unsigned) (a) & 0xFFu) << 8) |                           \
                      (((unsigned) (b) & 0xFFu) << 16) | (((unsigned) (c) & 0xFFu) << 24)))

#define CREATE_ABx(op, a, bx)                                                                      \
    ((XrInstruction) (((op) & 0xFFu) | (((unsigned) (a) & 0xFFu) << 8) |                           \
                      (((unsigned) (bx) & 0xFFFFu) << 16)))

#define CREATE_AsBx(op, a, sbx) CREATE_ABx(op, a, (sbx) + MAXARG_sBx)

#define CREATE_Ax(op, ax) ((XrInstruction) (((op) & 0xFFu) | (((unsigned) (ax) & 0xFFFFFFu) << 8)))

#define CREATE_sJ(op, sj) CREATE_Ax(op, (sj) + MAXARG_sJ)

// Extract arguments
#define GETARG_A(i) (((i) >> 8) & 0xFF)
#define GETARG_B(i) (((i) >> 16) & 0xFF)
#define GETARG_C(i) (((i) >> 24) & 0xFF)
#define GETARG_sB(i) ((int8_t) (GETARG_B(i)))  // signed B
#define GETARG_sC(i) ((int8_t) (GETARG_C(i)))  // signed C
#define GETARG_Bx(i) (((i) >> 16) & 0xFFFF)
#define GETARG_sBx(i) ((int) (GETARG_Bx(i)) - MAXARG_sBx)
#define GETARG_Ax(i) (((i) >> 8) & 0xFFFFFF)
#define GETARG_sJ(i) ((int) (GETARG_Ax(i)) - MAXARG_sJ)

// Set arguments (modify instruction)
// Note: Use unsigned constants to avoid signed integer overflow
#define SETARG_A(i, v) ((i) = ((i) & ~(0xFFu << 8)) | (((unsigned) (v) & 0xFFu) << 8))
#define SETARG_B(i, v) ((i) = ((i) & ~(0xFFu << 16)) | (((unsigned) (v) & 0xFFu) << 16))
#define SETARG_C(i, v) ((i) = ((i) & ~(0xFFu << 24)) | (((unsigned) (v) & 0xFFu) << 24))
#define SETARG_Bx(i, v) ((i) = ((i) & ~(0xFFFFu << 16)) | (((unsigned) (v) & 0xFFFFu) << 16))

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
    uint8_t index;             // SRC_REG: register number; SRC_UPVAL: enclosing upval index
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
     * Per-function symbol table: maps local index (0-254) to global SymbolId.
     * Instructions encode local indices in 8-bit B/C fields.
     * VM dereferences: global_sym = proto->symbols[local_idx]
     *
     * WHY THIS DESIGN:
     *   Global symbol IDs can exceed 255 (8-bit limit of iABC fields),
     *   but per-function unique property count is always small (<255).
     *   This eliminates symbol ID overflow by design.
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
    int num_spill_slots;        // spill slot count
    uint16_t struct_area_size;  // bytes needed for struct_area in stack frame (0 = none)

    // Struct layout cache for JIT: compile-time struct_layout pointers indexed by
    // slot_offset (from OP_NEW_STRUCT C operand). Enables JIT to resolve field
    // offsets and types at compile time instead of runtime class lookup.
    struct XrStructLayout **struct_layouts;  // [struct_layout_count] layout per slot
    int struct_layout_count;

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

    // Raw constant pool (uint64_t[]) for native-width values (int64/float64)
    // Used by OP_LOADK_RAW to load raw 64-bit values without tagged union
    uint64_t *raw_constants;
    int raw_constant_count;
    int raw_constant_capacity;

    /*
     * JIT/AOT metadata: type information preserved from compile-time analysis.
     *
     * WHY THIS DESIGN:
     *   - return_type_info: JIT specializes caller based on return type (XrType*)
     *   - inline_hint: JIT uses compiler's inline decision directly
     *   - loop_headers: JIT identifies OSR entry points and loop bodies
     *   - bb_leaders: bitmap of basic block entry points for JIT CFG construction
     */
    uint8_t *bb_leaders;           // bitmap: bit[pc] = 1 if pc is a basic block leader
    int bb_leaders_size;           // byte count of bb_leaders (ceil(code_count/8))
    uint8_t inline_hint;           // 0=no, 1=candidate, 2=always_inline
    uint8_t is_recursive;          // set by call graph DFS (indirect recursion detection)
    uint64_t tfield_float_bitmap;  // bit[i]=1: TFIELD field index i is F64 (set by compiler)
    int16_t *loop_headers;         // PC offsets of loop headers (NULL = none)
    int loop_header_count;

    /*
     * Per-parameter type annotations (authoritative source for param types).
     * param_types[i] = XrType* for parameter i (NULL = untyped/any).
     * Generated by the Xi pipeline during lowering.
     * Used by JIT entry guards, select_rep param typing, AOT codegen.
     */
    struct XrType **param_types;  // [numparams] parameter types
    uint8_t param_types_count;    // = numparams when allocated

    /*
     * Per-instruction type annotations (flow-sensitive, authoritative for non-params).
     * inst_types[pc] = XrType* for the result of instruction at pc (NULL = untyped).
     * Generated by codegen from compile_type at each emit site.
     * Used by JIT builder for field/call result types, AOT struct inference.
     */
    struct XrType **inst_types;  // [code_count] per-PC result types
    uint32_t inst_types_count;   // = code_count when allocated

    struct XrType *return_type_info;  // full return type (NULL = void/any)

    // JIT runtime state (populated at runtime, not compile time)
    void *jit_entry;                  // JIT compiled function pointer (NULL = not compiled)
    void *jit_fast_entry;             // Fast entry: skip param loading (register-passing calls)
    void *jit_resume_entry;           // Resume entry for JIT suspend/resume (NULL = none)
    void *_Atomic jit_entry_pending;  // Background JIT: compiled entry awaiting installation
    struct XmTypeFeedback *type_feedback;  // runtime type profile (lazily allocated)
    _Atomic uint32_t call_count;           // runtime call count (hot function detection)
    _Atomic uint32_t exec_count;           // runtime execution count (Tier 2 trigger)
    _Atomic uint32_t
        deopt_count;   // deopt counter (<5 normal, >=5 conservative recompile, >=20 JIT disabled)
    bool osr_pending;  // post-deopt: re-attempt OSR at next matching loop back-edge
    uint8_t jit_opt_level;    // current JIT compilation level (0=none, 1=basic, 2=full)
    uint32_t deopt_backoff;   // retry interval: calls to wait before reattempting JIT (doubles on
                              // each failure, max 10000)
    uint32_t deopt_reset_at;  // call_count snapshot taken when backoff was last set

    // OSR entry points (populated by JIT compilation, NULL if no loops)
    void *osr_entries;  // XmOsrEntry array (opaque, from xm_codegen.h)
    uint32_t nosr;      // number of OSR entry points

    // Deopt table: per-guard snapshot for precise deoptimization
    // Each entry records bc_pc + live slot→physical_reg/spill mappings
    // Populated by JIT compilation, freed on proto destruction
    void *deopt_table;  // XmRtDeoptEntry array (opaque)
    uint32_t ndeopt;

    // GC stack map: compile-time bitmap for precise GC root scanning.
    // Each safepoint (call/loop back-edge) has a bitmap recording which
    // registers and spill slots hold GC pointers. Populated by JIT codegen.
    void *stack_map;  // XrStackMapTable* (opaque, from xm_codegen.h)

    // Bytecode stack map: compile-time liveness bitmap for interpreter GC.
    // Safepoints at alloc instructions (OP_NEWARRAY, OP_NEWMAP, etc.).
    // For other PCs (CALL/INVOKE), GC falls back to conservative scan.
    void *bc_stackmap;  // XrBcStackMap* (opaque, from xbc_stackmap.h)

    // Parent proto: set by xr_vm_proto_add_proto when this proto is added
    // as a child. Used by JIT to walk up to module root and build
    // shared_protos mapping for CALL_KNOWN optimization.
    struct XrProto *enclosing;

    // Xi IR metadata preserved for JIT (opaque to runtime layer).
    // xi_func: retained SSA IR from compilation (XiFunc*); enables JIT
    //   to lower directly from SSA instead of re-building from bytecode.
    // xi_slot_map: value_id → bytecode slot mapping (XiSlotMap*); enables
    //   JIT deopt snapshot generation from Xi IR values.
    // Both freed in xr_proto_free(). NULL if Xi pipeline was not used.
    void *xi_func;      // opaque XiFunc* (owned, freed via xi_func_free)
    void *xi_slot_map;  // opaque XiSlotMap* (owned, freed via xi_slot_map_free)
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
XR_FUNC int xr_vm_proto_add_upvalue(XrProto *proto, uint8_t index, uint8_t storage_mode,
                                    uint8_t is_const, uint8_t slot_type, uint8_t source,
                                    struct XrType *type_info);
XR_FUNC int xr_proto_add_symbol(XrProto *proto, int32_t global_symbol);
XR_FUNC int xr_proto_add_raw_constant(XrProto *proto, uint64_t value);

/* ========== Debug Helpers ========== */
XR_FUNC const char *xr_opcode_name(OpCode op);

#endif  // XCHUNK_H
