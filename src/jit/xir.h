/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir.h - XIR (Xray Intermediate Representation) core data structures
 *
 * KEY CONCEPT:
 *   Single-layer SSA IR for JIT/AOT compilation. Compact design
 *   inspired by QBE (Ref 4B + Ins 16B) with xray-specific semantics
 *   (tagged values, GC safepoints, coroutine yield).
 *
 * WHY THIS DESIGN:
 *   - Single-layer: isel replaces XIR opcodes with machine opcodes in-place
 *   - SSA form: enables efficient optimization passes
 *   - 4 machine types (i64/f64/ptr/tagged) cover all XrSlotType variants
 *   - Phi nodes separate from instruction array for efficient iteration
 *
 * XIR INVARIANTS:
 *
 *   INVARIANT 1 (SSA form): Each virtual register (vreg) is defined
 *   exactly once. Every use of a vreg is dominated by its definition.
 *   Phi nodes at block entries merge values from predecessor blocks.
 *   SSA form is maintained from builder output through optimization
 *   passes until register allocation lowers it.
 *
 *   INVARIANT 2 (Block structure): Each basic block has a single entry
 *   point (the first instruction) and ends with exactly one terminator
 *   (XIR_JMP, XIR_BR, XIR_RET, or XIR_DEOPT). No terminator appears
 *   in the middle of a block. Successor/predecessor edges are consistent.
 *
 *   INVARIANT 3 (Type consistency): Each vreg has a machine type
 *   (i64/f64/ptr/tagged). XIR_BOX produces tagged from native;
 *   XIR_UNBOX produces native from tagged (with type guard).
 *   Operations on mismatched types must go through explicit conversion.
 *
 *   INVARIANT 4 (Deoptimization safety): Every deopt point (XIR_DEOPT,
 *   XIR_GUARD_TAG, XIR_GUARD_CLASS, XIR_GUARD_BOUNDS) has a deopt_id
 *   that maps to a bytecode PC. The deopt metadata records which vregs
 *   hold which bytecode registers, enabling reconstruction of the
 *   interpreter state. All GC-visible values must be in a recoverable
 *   location at every safepoint.
 *
 *   INVARIANT 5 (Safepoint coverage): XIR_SAFEPOINT instructions are
 *   inserted at loop back-edges, function entries, and after allocations.
 *   At every safepoint, the GC must be able to find all live heap
 *   pointers (via stack map bitmaps and jit_frame_stack).
 *
 *   INVARIANT 6 (Ref encoding): XirRef uses bits [31:29] for kind tag
 *   and bits [28:0] for index. XIR_REF_VREG=0, XIR_REF_CONST=1,
 *   XIR_REF_SLOT=2, XIR_REF_BLOCK=3, XIR_REF_NONE=7. Mixing ref
 *   kinds in operations that expect a specific kind is undefined.
 *
 *   INVARIANT 7 (Shadow ref coverage): XirRef values exist in FOUR
 *   locations that any vreg-rewriting pass must handle:
 *     (a) ins->args[0..1]        — instruction operands
 *     (b) phi->args[]            — phi node inputs
 *     (c) blk->jmp.arg           — terminator operand
 *     (d) call_arg_pool[]        — CALL argument pool (shadow)
 *     (e) deopt_infos[].slots[]  — deopt snapshots (shadow)
 *   Passes that substitute vreg references (copy_prop) or count
 *   uses (defuse_build) MUST visit ALL five locations. Omitting
 *   (d) or (e) causes stale refs, wrong register allocation, or
 *   incorrect DCE. xir_verify_cfg checks (d) in debug builds.
 *
 *   INVARIANT 8 (Type metadata): Compile-time type lives on the
 *   defining instruction (ins->ctype), not on XirVReg. XirVReg
 *   carries auxiliary metadata (heap_type, xrtype, callee_proto,
 *   shape_hint, layout, struct_idx) set by the builder.
 *   Passes that substitute vreg references must propagate auxiliary
 *   metadata from source to target when the target has less precise
 *   info. type_prop refines ins->ctype via worklist iteration.
 *
 * JIT COMPILATION PIPELINE:
 *
 *   Bytecode ──► XIR Builder ──► Optimization Passes ──► Register Alloc
 *                                     │                       │
 *                                     ├─ DCE                  ├─ Linear scan
 *                                     ├─ Copy prop            ├─ Spill/reload
 *                                     ├─ Const fold           └─► ARM64 Codegen
 *                                     ├─ Type specialization        │
 *                                     └─ If-conversion              └─► Executable code
 *
 * RELATED MODULES:
 *   - xir_builder.c: bytecode → XIR translation
 *   - xir_printer.c: text dump for debugging
 *   - xir_code_alloc.h: executable memory for emitted code
 */

#ifndef XIR_H
#define XIR_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "../runtime/value/xtype.h"
#include "../base/xdefs.h"

/* ========== XIR Value Reference (4 bytes) ========== */

/*
 * Compact reference to an SSA value, constant, or slot.
 *
 *   bits [31:29] = type tag (XirRefKind)
 *   bits [28:0]  = index
 *
 * Encoding as uint32_t for efficient comparison and hashing.
 */
typedef uint32_t XirRef;

typedef enum {
    XIR_REF_VREG = 0,   // virtual register (SSA value)
    XIR_REF_CONST = 1,  // constant pool index
    XIR_REF_SLOT = 2,   // stack slot (spill/local)
    XIR_REF_BLOCK = 3,  // basic block reference
    XIR_REF_NONE = 7,   // void / no value
} XirRefKind;

#define XIR_REF_KIND_BITS 3
#define XIR_REF_INDEX_BITS 29
#define XIR_REF_INDEX_MASK ((1u << XIR_REF_INDEX_BITS) - 1)

#define XIR_REF(kind, index)                                                                       \
    ((XirRef) (((uint32_t) (kind) << XIR_REF_INDEX_BITS) |                                         \
               ((uint32_t) (index) & XIR_REF_INDEX_MASK)))
#define XIR_REF_KIND(r) ((XirRefKind) ((r) >> XIR_REF_INDEX_BITS))
#define XIR_REF_INDEX(r) ((uint32_t) ((r) & XIR_REF_INDEX_MASK))

#define XIR_NONE XIR_REF(XIR_REF_NONE, 0)

static inline bool xir_ref_is_vreg(XirRef r) {
    return XIR_REF_KIND(r) == XIR_REF_VREG;
}
static inline bool xir_ref_is_const(XirRef r) {
    return XIR_REF_KIND(r) == XIR_REF_CONST;
}
static inline bool xir_ref_is_none(XirRef r) {
    return XIR_REF_KIND(r) == XIR_REF_NONE;
}

/* ========== XIR Machine Types (XrRep from xr_type.h) ========== */

/* ========== XIR Opcodes (see xir_ops.h) ========== */

#include "xir_ops.h"

/* ========== XIR_STORE_FIELD Tag Convention ========== */
/*
 * XIR_STORE_FIELD uses ins->type to carry the XrValue tag (0-15).
 * This replaces the previous XR_REP_VOID usage and eliminates
 * the lossy XIR machine type → XrValue tag inference in codegen.
 *
 * Layout:
 *   ins->type    = XrValue tag (0-15), or XIR_SF_TAG_RUNTIME (0xFF)
 *   ins->dst     = const(byte_offset)  — pure field byte offset
 *   ins->args[0] = obj ptr
 *   ins->args[1] = value to store
 *
 * XIR_SF_TAG_RUNTIME: codegen loads tag from the value's memory
 * representation at [val_reg + XRVALUE_TAG_OFFSET] instead of
 * using a compile-time constant. Used for dynamic-typed slots.
 */
#define XIR_SF_TAG_RUNTIME 0xFF

/* ========== XIR Instruction Flags ========== */

#define XIR_FLAG_SAFEPOINT (1 << 0)    // this instruction is a safepoint
#define XIR_FLAG_MAY_THROW (1 << 1)    // may raise an exception
#define XIR_FLAG_SIDE_EFFECT (1 << 2)  // has side effects (cannot be eliminated)
#define XIR_FLAG_COMMUTATIVE (1 << 3)  // operands are commutative
#define XIR_FLAG_MAY_GC (1 << 4)       // may trigger GC (allocation, call, safepoint)
#define XIR_FLAG_NO_BARRIER (1 << 5)   // skip write barrier (container is freshly allocated)
// Bits 6-7: extra register args for XIR_CALL_SELF_DIRECT (0 = none, 1-3 = extra count)
// arg0/arg1 in ins->args[], arg(2..2+N-1) pre-stored to call_args[2..N-1] by STORE_CORO,
// then loaded to alloc_regs[2..N-1] by codegen before BL fast_entry.
#define XIR_FLAG_EXTRA_ARGS(n) ((uint8_t) ((n) << 6))
#define XIR_FLAG_EXTRA_ARGS_GET(f) (((f) >> 6) & 0x3)

/* ========== Compile-Time Type (XirType) ========== */
/*
 * Unified compile-time type for instruction results.
 * Lives on XirIns (ins->ctype), not on XirVReg.
 *
 * Design: each instruction carries its own XirType describing the
 * value it produces. Passes that replace vreg references (copy_prop,
 * auto_inline) no longer need to propagate type metadata — the type
 * travels with the defining instruction.
 *
 * The `kind` field is the primary type discriminator. `rep` (machine
 * representation) is a separate codegen concern on XirIns.
 */

typedef enum {
    XIR_TK_UNKNOWN = 0,  // uninitialized / not yet inferred (passes may upgrade)
    XIR_TK_INT = 1,      // int64
    XIR_TK_FLOAT = 2,    // float64
    XIR_TK_BOOL = 3,     // bool
    XIR_TK_NULL = 4,     // null
    XIR_TK_PTR = 5,      // GC heap pointer (generic)
    XIR_TK_NUMERIC = 6,  // int|float union
    XIR_TK_STRING = 7,   // string (PTR refinement)
    XIR_TK_ARRAY = 8,    // array (PTR refinement)
    XIR_TK_MAP = 9,      // map (PTR refinement)
    XIR_TK_FUNC = 10,    // function/closure (PTR refinement)
    XIR_TK_TAGGED = 11,  // confirmed dynamic/any type (passes must NOT upgrade)
} XirTypeKind;

#define XIR_TF_NULLABLE (1 << 0)     // value can be null
#define XIR_TF_EXACT (1 << 1)        // exact type (no subtypes)
#define XIR_TF_FRESH_ALLOC (1 << 2)  // freshly allocated, no GC since alloc

typedef struct {
    uint8_t kind;       // XirTypeKind
    uint8_t flags;      // XIR_TF_*
    uint16_t heap_cid;  // GC class ID (0 = unknown)
} XirType;

// Derive machine representation from compile-time type kind
static inline uint8_t xir_type_to_rep(uint8_t kind) {
    switch (kind) {
        case XIR_TK_INT:
        case XIR_TK_BOOL:
            return XR_REP_I64;
        case XIR_TK_FLOAT:
            return XR_REP_F64;
        case XIR_TK_PTR:
        case XIR_TK_STRING:
        case XIR_TK_ARRAY:
        case XIR_TK_MAP:
        case XIR_TK_FUNC:
            return XR_REP_PTR;
        default:
            return XR_REP_TAGGED;
    }
}

// True if XirType kind represents a GC heap pointer
static inline bool xir_type_is_ptr(uint8_t kind) {
    return kind >= XIR_TK_PTR && kind <= XIR_TK_FUNC;
}

// Derive compile-time type from machine representation (lossy: no BOOL distinction)
static inline XirType xir_type_from_rep(uint8_t rep) {
    switch (rep) {
        case XR_REP_I64:
            return (XirType){XIR_TK_INT, 0, 0};
        case XR_REP_F64:
            return (XirType){XIR_TK_FLOAT, 0, 0};
        case XR_REP_PTR:
            return (XirType){XIR_TK_PTR, 0, 0};
        default:
            return (XirType){XIR_TK_UNKNOWN, 0, 0};
    }
}

// Unknown type constant
#define XIR_TYPE_UNKNOWN ((XirType){XIR_TK_UNKNOWN, 0, 0})

/* ========== XIR Instruction (20 bytes) ========== */

typedef struct XirIns {
    uint16_t op;     // XirOp (or machine opcode after isel)
    uint8_t rep;     // result XrRep (machine representation, codegen decision)
    uint8_t flags;   // XIR_FLAG_*
    XirType ctype;   // compile-time type of result value
    XirRef dst;      // result vreg (XIR_NONE if void)
    XirRef args[2];  // up to 2 inline operands
} XirIns;

// Extended args for instructions with >2 operands (CALL)
typedef struct XirInsExtra {
    XirIns base;
    uint16_t nargs;  // total argument count
    uint16_t _pad;
    XirRef *extra_args;  // heap-allocated for args[2..nargs)
} XirInsExtra;

/* ========== Phi Node ========== */

typedef struct XirPhi {
    XirRef dst;     // result vreg
    uint8_t rep;    // XrRep (machine representation)
    uint16_t narg;  // number of incoming values (== block->npred)
    uint8_t _pad;
    XirType ctype;        // compile-time type of PHI result
    XirRef *args;         // args[i] corresponds to block->preds[i]
    struct XirPhi *next;  // linked list within block
} XirPhi;

/* ========== Jump Type ========== */

typedef enum {
    XIR_JMP_NONE = 0,     // no terminator yet
    XIR_JMP_JMP,          // unconditional jump to s1
    XIR_JMP_BR,           // branch: if(arg) s1 else s2
    XIR_JMP_RET,          // return (arg = return value)
    XIR_JMP_UNREACHABLE,  // deopt / throw (no successor)
} XirJmpType;

/* ========== Basic Block ========== */

typedef struct XirBlock {
    uint32_t id;  // block index
    char *label;  // optional debug label (NULL for unnamed)

    // Phi nodes (linked list, only present at merge points)
    XirPhi *phis;

    // Instruction array (contiguous, grows via arena)
    XirIns *ins;
    uint32_t nins;
    uint32_t ins_cap;  // allocated capacity

    // Terminator
    struct {
        uint16_t type;  // XirJmpType
        XirRef arg;     // condition (for BR) or return value (for RET)
    } jmp;

    // CFG edges
    struct XirBlock *s1, *s2;  // successors (s2 only for BR)
    struct XirBlock **preds;   // predecessor array
    uint32_t npred;
    uint32_t pred_cap;

    // Ordering
    uint32_t rpo_id;  // reverse post-order index

    // Flags
    bool visited;         // traversal scratch
    bool is_loop_header;  // set by builder for loop header blocks (OSR entry candidate)
    bool is_deferred;     // cold path: catch block, deopt, throw — RA deprioritizes
    uint32_t bc_offset;   // bytecode PC of block start (for OSR matching)

    // Branch profile: fraction of times condition was true (0..100).
    // 0 = unknown (use heuristic).  Set from type_feedback branch counters.
    uint8_t branch_taken_pct;

    // Exception handling: if non-NULL, this block is inside a try region
    // and exception_handler points to the catch block
    struct XirBlock *exception_handler;
} XirBlock;

/* ========== Deoptimization Info ========== */

// Per-slot entry in a deopt snapshot: maps a bytecode register to an XIR value
typedef struct {
    int16_t bc_slot;  // bytecode register index (R[bc_slot])
    uint8_t rep;      // XrRep of the value (I64/F64/PTR/TAGGED)
    uint8_t xr_tag;   // XrValue tag (0-15), or XRVREG_TAG_UNKNOWN (0xFF)
    XirRef value;     // XIR ref (vreg or const) holding the slot value
} XirDeoptSlot;

// Deoptimization snapshot attached to a guard/deopt point
typedef struct {
    uint32_t bc_pc;       // bytecode PC to resume at
    uint16_t nslots;      // number of live slots in this snapshot
    uint16_t deopt_id;    // index into XirFunc.deopt_infos / XrProto.deopt_table
    XirDeoptSlot *slots;  // array of slot mappings (arena-allocated)
} XirDeoptInfo;

#define XIR_MAX_DEOPT_POINTS 128

/* ========== Virtual Register ========== */

/*
 * Compile-time type annotation for virtual registers.
 * Separate from XrValueTag (runtime tag in XrValue.tag): vtag describes
 * what the JIT compiler knows statically about a vreg's type, while
 * XrValueTag describes the actual runtime tag stored in an XrValue.
 *
 * At JIT/runtime boundaries (deopt reconstruction, RET epilogue) vtag is
 * converted to XrValueTag via vtag_to_value_tag().
 */
typedef enum {
    VTAG_TAGGED = 0,   // default: full tagged XrValue (any/unknown/dynamic)
    VTAG_I64 = 1,      // confirmed int64 (raw i64 register)
    VTAG_F64 = 2,      // confirmed float64 (raw f64 register)
    VTAG_PTR = 3,      // confirmed GC heap pointer
    VTAG_BOOL = 4,     // confirmed bool (payload 0=false, non-zero=true)
    VTAG_NUMERIC = 5,  // int|float union, runtime tag check required
    VTAG_NULL = 6,     // confirmed null (payload always 0)
} XrVRegTag;

/*
 * Runtime value_tag signal constants used in JIT helpers and INVOKE bitmaps.
 * These are NOT XrVRegTag values — they appear in x1 (return tag) and
 * call_args bitmaps at runtime boundaries to convey type hints to helpers.
 */
#define XR_RTAG_NUMERIC 0xFC  // numeric union (int|float): needs runtime check
#define XR_RTAG_UNKNOWN 0xFF  // unknown: use heuristic fallback

// True if vtag is a concrete type (not tagged/union/callee)
static inline bool vtag_is_concrete(uint8_t vtag) {
    return (vtag >= VTAG_I64 && vtag <= VTAG_BOOL) || vtag == VTAG_NULL;
}

// Convert compile-time vtag to runtime value_tag for deopt/RET epilogue/helper args.
// For concrete scalars returns XrValueTag directly; for union/dynamic types
// returns the corresponding XR_RTAG_* signal so runtime helpers can dispatch.
static inline uint8_t vtag_to_value_tag(uint8_t vtag) {
    switch (vtag) {
        case VTAG_NULL:
            return 0;  // XR_TAG_NULL
        case VTAG_I64:
            return 3;  // XR_TAG_I64
        case VTAG_F64:
            return 4;  // XR_TAG_F64
        case VTAG_PTR:
            return 5;  // XR_TAG_PTR
        case VTAG_BOOL:
            return 1;  // XR_TAG_BOOL (payload 0=false, 1=true)
        case VTAG_NUMERIC:
            return 0xFC;  // XR_RTAG_NUMERIC
        default:
            return 0xFF;  // XR_RTAG_UNKNOWN (VTAG_TAGGED)
    }
}

// Convert runtime XrValueTag to compile-time vtag
static inline uint8_t value_tag_to_vtag(uint8_t tag) {
    switch (tag) {
        case 0:
            return VTAG_NULL;  // XR_TAG_NULL
        case 3:
            return VTAG_I64;  // XR_TAG_I64
        case 4:
            return VTAG_F64;  // XR_TAG_F64
        case 5:
            return VTAG_PTR;  // XR_TAG_PTR
        case 1:
            return VTAG_BOOL;  // XR_TAG_BOOL
        case 0xFC:
            return VTAG_NUMERIC;  // XR_RTAG_NUMERIC
        default:
            return VTAG_TAGGED;
    }
}

// ---- XirType ↔ vtag conversion (requires VTAG_* enum above) ----

// Convert legacy vtag to XirTypeKind
static inline uint8_t vtag_to_type_kind(uint8_t vtag) {
    switch (vtag) {
        case VTAG_I64:
            return XIR_TK_INT;
        case VTAG_F64:
            return XIR_TK_FLOAT;
        case VTAG_PTR:
            return XIR_TK_PTR;
        case VTAG_BOOL:
            return XIR_TK_BOOL;
        case VTAG_NUMERIC:
            return XIR_TK_NUMERIC;
        case VTAG_NULL:
            return XIR_TK_NULL;
        case VTAG_TAGGED:
            return XIR_TK_TAGGED;
        default:
            return XIR_TK_UNKNOWN;
    }
}

// Convert XirTypeKind to legacy vtag (for transition period)
static inline uint8_t type_kind_to_vtag(uint8_t kind) {
    switch (kind) {
        case XIR_TK_INT:
            return VTAG_I64;
        case XIR_TK_FLOAT:
            return VTAG_F64;
        case XIR_TK_BOOL:
            return VTAG_BOOL;
        case XIR_TK_NULL:
            return VTAG_NULL;
        case XIR_TK_PTR:
        case XIR_TK_STRING:
        case XIR_TK_ARRAY:
        case XIR_TK_MAP:
        case XIR_TK_FUNC:
            return VTAG_PTR;
        case XIR_TK_NUMERIC:
            return VTAG_NUMERIC;
        case XIR_TK_TAGGED:
            return VTAG_TAGGED;
        default:
            return VTAG_TAGGED;
    }
}

// Build XirType from legacy vtag + heap_type
static inline XirType xir_type_from_vtag(uint8_t vtag, uint16_t heap_type) {
    XirType t = {vtag_to_type_kind(vtag), 0, heap_type};
    return t;
}

typedef struct {
    XirIns *def;          // defining instruction
    uint8_t rep;          // XrRep (machine rep: I64/F64/PTR/TAGGED)
    uint16_t heap_type;   // GC heap type (XR_T*), valid when ctype.kind is PTR; 0=unknown
    int8_t reg;           // physical register (-1 = unallocated)
    int16_t bc_slot;      // bytecode register slot (-1 = not mapped)
    int16_t struct_idx;   // proto->struct_layouts[] index (-1 = unknown)
    uint16_t cost;        // spill cost (heuristic)
    uint32_t start, end;  // live range [start, end)
    XrType *xrtype;       // precise static type (NULL = unknown)
    // Compiler hints for optimization (migrated from XirBuilder slot arrays)
    struct XrProto *callee_proto;   // known callee proto (from OP_CLOSURE/GETSHARED)
    struct XrShape *shape_hint;     // known Json shape (from OP_NEWJSON / Blueprint)
    struct XrStructLayout *layout;  // struct field layout (from OP_NEW_STRUCT / params)
    uint8_t array_etype;            // native array element type (0xFF = not a fixed array)
    uint16_t array_ecount;          // native array element count
    bool is_fresh_alloc;            // true if allocated with no GC safepoint since alloc
    // Call argument pool linkage (only valid for CALL dst vregs)
    uint32_t call_arg_start;  // index into func->call_arg_pool
    uint16_t call_nargs;      // number of call arguments (0 = not a call)
} XirVReg;

/* ========== Constant Entry ========== */

typedef union {
    int64_t i64;
    double f64;
    void *ptr;
    uint64_t raw;  // for bit-level access
    struct {
        const char *chars;
        uint32_t len;
    } str;  // AOT string literal
} XirConstVal;

typedef struct {
    XirConstVal val;
    uint8_t rep;  // XrRep (machine representation)
} XirConst;

/* ========== Function-level IR Container ========== */

typedef struct XirFunc {
    const char *name;  // function name (debug)

    // Basic blocks
    XirBlock *entry;    // entry block
    XirBlock **blocks;  // all blocks array
    uint32_t nblk;
    uint32_t blk_cap;
    XirBlock **rpo;  // reverse post-order (computed)

    // Virtual registers
    XirVReg *vregs;
    uint32_t nvreg;
    uint32_t vreg_cap;

    // Constant pool
    XirConst *consts;
    uint32_t nconst;
    uint32_t const_cap;

    // Constant dedup hash table (open-addressing, power-of-2 size)
    // Each slot stores const pool index+1 (0 = empty). Key = (rep, val.raw).
    uint32_t *const_ht;      // hash table slots (NULL until first const added)
    uint32_t const_ht_mask;  // table_size - 1 (power of 2)

    // Source metadata (from XrProto)
    struct XrProto *proto;  // back-pointer: type info via proto->param_types/return_type_info
    uint16_t num_params;
    uint16_t max_stack;

    // Allocated during compilation
    uint32_t spill_slots;
    uint32_t frame_size;

    // Deoptimization info: snapshots for each guard/deopt point
    XirDeoptInfo *deopt_infos;
    uint32_t ndeopt;
    uint32_t deopt_cap;

    // Cached analysis results (lazily computed, invalidated on CFG change)
    struct XirDomTree *domtree;    // see xir_domtree.h
    struct XirLoopInfo *loopinfo;  // see xir_looptree.h
    struct XirDefUse *defuse;      // see xir_defuse.h
    void *alias;                   // opaque AliasTable *, see xir_alias.c

    // Call argument pool: flat array of XirRef for all CALL instructions.
    // Each CALL dst vreg has (call_arg_start, call_nargs) pointing into this pool.
    // Eliminates STORE_CORO arg-passing pattern; codegen writes call_args[] from pool.
    XirRef *call_arg_pool;
    uint32_t call_arg_pool_used;
    uint32_t call_arg_pool_cap;

    // Defer tracking (AOT mode): deferred closure calls executed at function exit.
    // Each entry records the closure vreg, arg vregs, and arg count.
    // Codegen emits LIFO cleanup at every return point.
    struct {
        XirRef closure;  // closure vreg (XrValue)
        int arg_count;   // number of call arguments (0 for defer { ... })
        XirRef args[8];  // call arg vregs (if arg_count > 0)
    } defer_entries[16];
    int defer_count;  // number of defer entries

    // Flags
    bool has_coro_deopt;  // contains AWAIT/SCOPE_EXIT (OSR unsafe)
    bool conservative;    // compiled in conservative mode (no type speculation)

    // Suspend point tracking (for resume entry generation)
    uint32_t nsuspend;                // number of XIR_SUSPEND instructions
    void *suspend_block_helpers[16];  // per-suspend block helper fn ptr (NULL = xr_jit_await_block)

    // Arena allocator for IR nodes (all XIR memory freed together)
    struct XirArena *arena;
} XirFunc;

/* ========== Arena Allocator ========== */

#define XIR_ARENA_PAGE_SIZE (16 * 1024)  // 16KB per arena page

typedef struct XirArenaPage {
    struct XirArenaPage *next;
    size_t used;
    size_t size;
    uint8_t data[];  // flexible array member
} XirArenaPage;

typedef struct XirArena {
    XirArenaPage *current;
    XirArenaPage *pages;  // all pages (for freeing)
    size_t total_allocated;
} XirArena;

// Arena API
XR_FUNC void xir_arena_init(XirArena *arena);
XR_FUNC void xir_arena_destroy(XirArena *arena);
XR_FUNC void *xir_arena_alloc(XirArena *arena, size_t size);

// Convenience: allocate zero-initialized memory
static inline void *xir_arena_calloc(XirArena *arena, size_t count, size_t elem_size) {
    size_t total = count * elem_size;
    void *p = xir_arena_alloc(arena, total);
    if (p)
        __builtin_memset(p, 0, total);
    return p;
}

/* ========== XirFunc API ========== */

// Create/destroy
XR_FUNC XirFunc *xir_func_new(const char *name);
XR_FUNC void xir_func_destroy(XirFunc *func);

// Blocks
XR_FUNC XirBlock *xir_func_add_block(XirFunc *func, const char *label);
XR_FUNC void xir_block_add_pred(XirBlock *blk, XirBlock *pred, XirArena *arena);
XR_FUNC void xir_block_set_jmp(XirBlock *blk, XirBlock *target);
XR_FUNC void xir_block_set_br(XirBlock *blk, XirRef cond, XirBlock *if_true, XirBlock *if_false);
XR_FUNC void xir_block_set_ret(XirBlock *blk, XirRef val);

// Instructions
XR_FUNC XirRef xir_emit(XirFunc *func, XirBlock *blk, uint16_t op, uint8_t type, XirRef a,
                        XirRef b);
XR_FUNC XirRef xir_emit_unary(XirFunc *func, XirBlock *blk, uint16_t op, uint8_t type, XirRef a);
XR_FUNC void xir_emit_void(XirFunc *func, XirBlock *blk, uint16_t op, XirRef a, XirRef b);

// Low-level emit with explicit dst (for Phi lowering and regalloc MOV insertion)
XR_FUNC void xir_emit_raw(XirFunc *func, XirBlock *blk, uint16_t op, uint8_t type, XirRef dst,
                          XirRef arg0, XirRef arg1);

// Constants
XR_FUNC XirRef xir_const_i64(XirFunc *func, int64_t val);
XR_FUNC XirRef xir_const_f64(XirFunc *func, double val);
XR_FUNC XirRef xir_const_ptr(XirFunc *func, void *val);
XR_FUNC XirRef xir_const_string(XirFunc *func, const char *chars, uint32_t len);

// Virtual registers
XR_FUNC XirRef xir_new_vreg(XirFunc *func, uint8_t type);

// Call arg pool
XR_FUNC uint32_t xir_func_add_call_args(XirFunc *func, const XirRef *args, uint16_t nargs);
XR_FUNC void xir_func_bind_call_args(XirFunc *func, XirRef dst, const XirRef *args, uint16_t nargs);

// Phi nodes
XR_FUNC XirPhi *xir_add_phi(XirFunc *func, XirBlock *blk, uint8_t type);
XR_FUNC void xir_phi_set_arg(XirPhi *phi, uint32_t pred_idx, XirRef val);

// Insert a zeroed instruction slot at position `pos`, shifting subsequent instructions.
XR_FUNC XirIns *xir_block_insert_at(XirFunc *func, XirBlock *blk, uint32_t pos);

/* ========== Opcode Info ========== */

XR_FUNC const char *xir_op_name(uint16_t op);
XR_FUNC bool xir_op_is_commutative(uint16_t op);
XR_FUNC bool xir_op_has_side_effect(uint16_t op);
XR_FUNC bool xir_op_is_pure(uint16_t op);

// True for copy-like ops (MOV or REDEFINE) that can be coalesced
static inline bool xir_op_is_copy(uint16_t op) {
    return op == XIR_MOV || op == XIR_REDEFINE;
}

// Returns true if instruction cannot throw an exception (pure memory access,
// arithmetic, constants).  LICM uses this to hoist from try blocks safely.
static inline bool xir_ins_nothrow(const XirIns *ins) {
    switch (ins->op) {
        case XIR_CONST_I64:
        case XIR_CONST_F64:
        case XIR_CONST_PTR:
        case XIR_ADD:
        case XIR_SUB:
        case XIR_MUL:
        case XIR_DIV:
        case XIR_AND:
        case XIR_OR:
        case XIR_XOR:
        case XIR_SHL:
        case XIR_SHR:
        case XIR_NEG:
        case XIR_NOT:
        case XIR_EQ:
        case XIR_NE:
        case XIR_LT:
        case XIR_LE:
        case XIR_GT:
        case XIR_GE:
        case XIR_LOAD_FIELD:
        case XIR_MOV:
        case XIR_REDEFINE:
        case XIR_PHI:
        case XIR_SELECT:
            return true;
        default:
            return !(ins->flags & XIR_FLAG_MAY_THROW);
    }
}

// Get compile-time type of a ref by looking up its defining instruction.
// Falls back to rep-based inference for param vregs (no def instruction).
static inline XirType xir_ref_ctype(XirFunc *func, XirRef ref) {
    if (!xir_ref_is_vreg(ref))
        return XIR_TYPE_UNKNOWN;
    uint32_t vi = XIR_REF_INDEX(ref);
    if (vi >= func->nvreg)
        return XIR_TYPE_UNKNOWN;
    XirIns *def = func->vregs[vi].def;
    if (def)
        return def->ctype;
    return xir_type_from_rep(func->vregs[vi].rep);
}

/* ========== Dominator Utilities ========== */

// Compute immediate dominators using Cooper's algorithm.
// idom[] must be pre-allocated with nblk entries.
// (Building block for xir_domtree.c — most callers should
// go through xir_func_get_domtree() instead.)
XR_FUNC void xir_compute_idom(XirFunc *func, uint32_t *idom, uint32_t nblk);

// Rebuild vreg.def pointers by scanning all instructions.
// Call after any pass that moves or compacts instructions in block arrays.
XR_FUNC void xir_rebuild_vreg_defs(XirFunc *func);

#endif  // XIR_H
