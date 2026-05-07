/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xm.h - Xm (Xray Intermediate Representation) core data structures
 *
 * KEY CONCEPT:
 *   Single-layer SSA IR for JIT/AOT compilation. Compact design
 *   inspired by QBE (Ref 4B + Ins 16B) with xray-specific semantics
 *   (tagged values, GC safepoints, coroutine yield).
 *
 * WHY THIS DESIGN:
 *   - Single-layer: isel replaces Xm opcodes with machine opcodes in-place
 *   - SSA form: enables efficient optimization passes
 *   - 4 machine types (i64/f64/ptr/tagged) cover all XrSlotType variants
 *   - Phi nodes separate from instruction array for efficient iteration
 *
 * Xm INVARIANTS:
 *
 *   INVARIANT 1 (SSA form): Each virtual register (vreg) is defined
 *   exactly once. Every use of a vreg is dominated by its definition.
 *   Phi nodes at block entries merge values from predecessor blocks.
 *   SSA form is maintained from builder output through optimization
 *   passes until register allocation lowers it.
 *
 *   INVARIANT 2 (Block structure): Each basic block has a single entry
 *   point (the first instruction) and ends with exactly one terminator
 *   (XM_JMP, XM_BR, XM_RET, or XM_DEOPT). No terminator appears
 *   in the middle of a block. Successor/predecessor edges are consistent.
 *
 *   INVARIANT 3 (Type consistency): Each vreg has a machine type
 *   (i64/f64/ptr/tagged). XM_BOX produces tagged from native;
 *   XM_UNBOX produces native from tagged (with type guard).
 *   Operations on mismatched types must go through explicit conversion.
 *
 *   INVARIANT 4 (Deoptimization safety): Every deopt point (XM_DEOPT,
 *   XM_GUARD_TAG, XM_GUARD_CLASS, XM_GUARD_BOUNDS) has a deopt_id
 *   that maps to a bytecode PC. The deopt metadata records which vregs
 *   hold which bytecode registers, enabling reconstruction of the
 *   interpreter state. All GC-visible values must be in a recoverable
 *   location at every safepoint.
 *
 *   INVARIANT 5 (Safepoint coverage): XM_SAFEPOINT instructions are
 *   inserted at loop back-edges, function entries, and after allocations.
 *   At every safepoint, the GC must be able to find all live heap
 *   pointers (via stack map bitmaps and jit_frame_stack).
 *
 *   INVARIANT 6 (Ref encoding): XmRef uses bits [31:29] for kind tag
 *   and bits [28:0] for index. XM_REF_VREG=0, XM_REF_CONST=1,
 *   XM_REF_SLOT=2, XM_REF_BLOCK=3, XM_REF_NONE=7. Mixing ref
 *   kinds in operations that expect a specific kind is undefined.
 *
 *   INVARIANT 7 (Shadow ref coverage): XmRef values exist in FOUR
 *   locations that any vreg-rewriting pass must handle:
 *     (a) ins->args[0..1]        — instruction operands
 *     (b) phi->args[]            — phi node inputs
 *     (c) blk->jmp.arg           — terminator operand
 *     (d) call_arg_pool[]        — CALL argument pool (shadow)
 *     (e) deopt_infos[].slots[]  — deopt snapshots (shadow)
 *   Passes that substitute vreg references (copy_prop) or count
 *   uses (defuse_build) MUST visit ALL five locations. Omitting
 *   (d) or (e) causes stale refs, wrong register allocation, or
 *   incorrect DCE. xm_verify_cfg checks (d) in debug builds.
 *
 *   INVARIANT 8 (Type metadata): Compile-time type lives on the
 *   defining instruction (ins->ctype), not on XmVReg. XmVReg
 *   carries auxiliary metadata (heap_type, xrtype, callee_proto,
 *   shape_hint, layout, struct_idx) set by the builder.
 *   Passes that substitute vreg references must propagate auxiliary
 *   metadata from source to target when the target has less precise
 *   info. type_prop refines ins->ctype via worklist iteration.
 *
 * JIT COMPILATION PIPELINE:
 *
 *   Bytecode ──► Xm Builder ──► Optimization Passes ──► Register Alloc
 *                                     │                       │
 *                                     ├─ DCE                  ├─ Linear scan
 *                                     ├─ Copy prop            ├─ Spill/reload
 *                                     ├─ Const fold           └─► ARM64 Codegen
 *                                     ├─ Type specialization        │
 *                                     └─ If-conversion              └─► Executable code
 *
 * RELATED MODULES:
 *   - xi_to_xm.c: Xi IR → Xm lowering
 *   - xm_printer.c: text dump for debugging
 *   - xm_code_alloc.h: executable memory for emitted code
 */

#ifndef XM_H
#define XM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "../runtime/value/xtype.h"
#include "../base/xdefs.h"

/* ========== Xm Value Reference (4 bytes) ========== */

/*
 * Compact reference to an SSA value, constant, or slot.
 *
 *   bits [31:29] = type tag (XmRefKind)
 *   bits [28:0]  = index
 *
 * Encoding as uint32_t for efficient comparison and hashing.
 */
typedef uint32_t XmRef;

typedef enum {
    XM_REF_VREG = 0,   // virtual register (SSA value)
    XM_REF_CONST = 1,  // constant pool index
    XM_REF_SLOT = 2,   // stack slot (spill/local)
    XM_REF_BLOCK = 3,  // basic block reference
    XM_REF_NONE = 7,   // void / no value
} XmRefKind;

#define XM_REF_KIND_BITS 3
#define XM_REF_INDEX_BITS 29
#define XM_REF_INDEX_MASK ((1u << XM_REF_INDEX_BITS) - 1)

#define XM_REF(kind, index)                                                                       \
    ((XmRef) (((uint32_t) (kind) << XM_REF_INDEX_BITS) |                                         \
               ((uint32_t) (index) & XM_REF_INDEX_MASK)))
#define XM_REF_KIND(r) ((XmRefKind) ((r) >> XM_REF_INDEX_BITS))
#define XM_REF_INDEX(r) ((uint32_t) ((r) & XM_REF_INDEX_MASK))

#define XM_NONE XM_REF(XM_REF_NONE, 0)

static inline bool xm_ref_is_vreg(XmRef r) {
    return XM_REF_KIND(r) == XM_REF_VREG;
}
static inline bool xm_ref_is_const(XmRef r) {
    return XM_REF_KIND(r) == XM_REF_CONST;
}
static inline bool xm_ref_is_none(XmRef r) {
    return XM_REF_KIND(r) == XM_REF_NONE;
}

/* ========== Xm Machine Types (XrRep from xr_type.h) ========== */

/* ========== Xm Opcodes (see xm_ops.h) ========== */

#include "xm_ops.h"

/* ========== XM_STORE_FIELD Tag Convention ========== */
/*
 * XM_STORE_FIELD uses ins->type to carry the XrValue tag (0-15).
 * This replaces the previous XR_REP_VOID usage and eliminates
 * the lossy Xm machine type → XrValue tag inference in codegen.
 *
 * Layout:
 *   ins->type    = XrValue tag (0-15), or XM_SF_TAG_RUNTIME (0xFF)
 *   ins->dst     = const(byte_offset)  — pure field byte offset
 *   ins->args[0] = obj ptr
 *   ins->args[1] = value to store
 *
 * XM_SF_TAG_RUNTIME: codegen loads tag from the value's memory
 * representation at [val_reg + XRVALUE_TAG_OFFSET] instead of
 * using a compile-time constant. Used for dynamic-typed slots.
 */
#define XM_SF_TAG_RUNTIME 0xFF

/* ========== Xm Instruction Flags ========== */

#define XM_FLAG_SAFEPOINT (1 << 0)    // this instruction is a safepoint
#define XM_FLAG_MAY_THROW (1 << 1)    // may raise an exception
#define XM_FLAG_SIDE_EFFECT (1 << 2)  // has side effects (cannot be eliminated)
#define XM_FLAG_COMMUTATIVE (1 << 3)  // operands are commutative
#define XM_FLAG_MAY_GC (1 << 4)       // may trigger GC (allocation, call, safepoint)
#define XM_FLAG_NO_BARRIER (1 << 5)   // skip write barrier (container is freshly allocated)
// Bits 6-7: extra register args for XM_CALL_SELF_DIRECT (0 = none, 1-3 = extra count)
// arg0/arg1 in ins->args[], arg(2..2+N-1) pre-stored to call_args[2..N-1] by STORE_CORO,
// then loaded to alloc_regs[2..N-1] by codegen before BL fast_entry.
#define XM_FLAG_EXTRA_ARGS(n) ((uint8_t) ((n) << 6))
#define XM_FLAG_EXTRA_ARGS_GET(f) (((f) >> 6) & 0x3)

/* ========== Compile-Time Type (XmType) ========== */
/*
 * Unified compile-time type for instruction results.
 * Lives on XmIns (ins->ctype), not on XmVReg.
 *
 * Design: each instruction carries its own XmType describing the
 * value it produces. Passes that replace vreg references (copy_prop,
 * auto_inline) no longer need to propagate type metadata — the type
 * travels with the defining instruction.
 *
 * The `kind` field is the primary type discriminator. `rep` (machine
 * representation) is a separate codegen concern on XmIns.
 */

typedef enum {
    XM_TK_UNKNOWN = 0,  // uninitialized / not yet inferred (passes may upgrade)
    XM_TK_INT = 1,      // int64
    XM_TK_FLOAT = 2,    // float64
    XM_TK_BOOL = 3,     // bool
    XM_TK_NULL = 4,     // null
    XM_TK_PTR = 5,      // GC heap pointer (generic)
    XM_TK_NUMERIC = 6,  // int|float union
    XM_TK_STRING = 7,   // string (PTR refinement)
    XM_TK_ARRAY = 8,    // array (PTR refinement)
    XM_TK_MAP = 9,      // map (PTR refinement)
    XM_TK_FUNC = 10,    // function/closure (PTR refinement)
    XM_TK_TAGGED = 11,  // confirmed dynamic/any type (passes must NOT upgrade)
} XmTypeKind;

#define XM_TF_NULLABLE (1 << 0)     // value can be null
#define XM_TF_EXACT (1 << 1)        // exact type (no subtypes)
#define XM_TF_FRESH_ALLOC (1 << 2)  // freshly allocated, no GC since alloc

typedef struct {
    uint8_t kind;       // XmTypeKind
    uint8_t flags;      // XM_TF_*
    uint16_t heap_cid;  // GC class ID (0 = unknown)
} XmType;

// Derive machine representation from compile-time type kind
static inline uint8_t xm_type_to_rep(uint8_t kind) {
    switch (kind) {
        case XM_TK_INT:
        case XM_TK_BOOL:
            return XR_REP_I64;
        case XM_TK_FLOAT:
            return XR_REP_F64;
        case XM_TK_PTR:
        case XM_TK_STRING:
        case XM_TK_ARRAY:
        case XM_TK_MAP:
        case XM_TK_FUNC:
            return XR_REP_PTR;
        default:
            return XR_REP_TAGGED;
    }
}

// True if XmType kind represents a GC heap pointer
static inline bool xm_type_is_ptr(uint8_t kind) {
    return kind >= XM_TK_PTR && kind <= XM_TK_FUNC;
}

// Derive compile-time type from machine representation (lossy: no BOOL distinction)
static inline XmType xm_type_from_rep(uint8_t rep) {
    switch (rep) {
        case XR_REP_I64:
            return (XmType){XM_TK_INT, 0, 0};
        case XR_REP_F64:
            return (XmType){XM_TK_FLOAT, 0, 0};
        case XR_REP_PTR:
            return (XmType){XM_TK_PTR, 0, 0};
        default:
            return (XmType){XM_TK_UNKNOWN, 0, 0};
    }
}

// Unknown type constant
#define XM_TYPE_UNKNOWN ((XmType){XM_TK_UNKNOWN, 0, 0})

/* ========== Xm Instruction (20 bytes) ========== */

typedef struct XmIns {
    uint16_t op;     // XmOp (or machine opcode after isel)
    uint8_t rep;     // result XrRep (machine representation, codegen decision)
    uint8_t flags;   // XM_FLAG_*
    XmType ctype;   // compile-time type of result value
    XmRef dst;      // result vreg (XM_NONE if void)
    XmRef args[2];  // up to 2 inline operands
} XmIns;

// Extended args for instructions with >2 operands (CALL)
typedef struct XmInsExtra {
    XmIns base;
    uint16_t nargs;  // total argument count
    uint16_t _pad;
    XmRef *extra_args;  // heap-allocated for args[2..nargs)
} XmInsExtra;

/* ========== Phi Node ========== */

typedef struct XmPhi {
    XmRef dst;     // result vreg
    uint8_t rep;    // XrRep (machine representation)
    uint16_t narg;  // number of incoming values (== block->npred)
    uint8_t _pad;
    XmType ctype;        // compile-time type of PHI result
    XmRef *args;         // args[i] corresponds to block->preds[i]
    struct XmPhi *next;  // linked list within block
} XmPhi;

/* ========== Jump Type ========== */

typedef enum {
    XM_JMP_NONE = 0,     // no terminator yet
    XM_JMP_JMP,          // unconditional jump to s1
    XM_JMP_BR,           // branch: if(arg) s1 else s2
    XM_JMP_RET,          // return (arg = return value)
    XM_JMP_UNREACHABLE,  // deopt / throw (no successor)
} XmJmpType;

/* ========== Basic Block ========== */

typedef struct XmBlock {
    uint32_t id;  // block index
    char *label;  // optional debug label (NULL for unnamed)

    // Phi nodes (linked list, only present at merge points)
    XmPhi *phis;

    // Instruction array (contiguous, grows via arena)
    XmIns *ins;
    uint32_t nins;
    uint32_t ins_cap;  // allocated capacity

    // Terminator
    struct {
        uint16_t type;  // XmJmpType
        XmRef arg;     // condition (for BR) or return value (for RET)
    } jmp;

    // CFG edges
    struct XmBlock *s1, *s2;  // successors (s2 only for BR)
    struct XmBlock **preds;   // predecessor array
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
    struct XmBlock *exception_handler;
} XmBlock;

/* ========== Deoptimization Info ========== */

// Per-slot entry in a deopt snapshot: maps a bytecode register to an Xm value
typedef struct {
    int16_t bc_slot;  // bytecode register index (R[bc_slot])
    uint8_t rep;      // XrRep of the value (I64/F64/PTR/TAGGED)
    uint8_t xr_tag;   // XrValue tag (0-15), or XRVREG_TAG_UNKNOWN (0xFF)
    XmRef value;     // Xm ref (vreg or const) holding the slot value
} XmDeoptSlot;

// Deoptimization snapshot attached to a guard/deopt point
typedef struct {
    uint32_t bc_pc;       // bytecode PC to resume at
    uint16_t nslots;      // number of live slots in this snapshot
    uint16_t deopt_id;    // index into XmFunc.deopt_infos / XrProto.deopt_table
    XmDeoptSlot *slots;  // array of slot mappings (arena-allocated)
} XmDeoptInfo;

#define XM_MAX_DEOPT_POINTS 128

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

// ---- XmType ↔ vtag conversion (requires VTAG_* enum above) ----

// Convert legacy vtag to XmTypeKind
static inline uint8_t vtag_to_type_kind(uint8_t vtag) {
    switch (vtag) {
        case VTAG_I64:
            return XM_TK_INT;
        case VTAG_F64:
            return XM_TK_FLOAT;
        case VTAG_PTR:
            return XM_TK_PTR;
        case VTAG_BOOL:
            return XM_TK_BOOL;
        case VTAG_NUMERIC:
            return XM_TK_NUMERIC;
        case VTAG_NULL:
            return XM_TK_NULL;
        case VTAG_TAGGED:
            return XM_TK_TAGGED;
        default:
            return XM_TK_UNKNOWN;
    }
}

// Convert XmTypeKind to legacy vtag (for transition period)
static inline uint8_t type_kind_to_vtag(uint8_t kind) {
    switch (kind) {
        case XM_TK_INT:
            return VTAG_I64;
        case XM_TK_FLOAT:
            return VTAG_F64;
        case XM_TK_BOOL:
            return VTAG_BOOL;
        case XM_TK_NULL:
            return VTAG_NULL;
        case XM_TK_PTR:
        case XM_TK_STRING:
        case XM_TK_ARRAY:
        case XM_TK_MAP:
        case XM_TK_FUNC:
            return VTAG_PTR;
        case XM_TK_NUMERIC:
            return VTAG_NUMERIC;
        case XM_TK_TAGGED:
            return VTAG_TAGGED;
        default:
            return VTAG_TAGGED;
    }
}

// Build XmType from legacy vtag + heap_type
static inline XmType xm_type_from_vtag(uint8_t vtag, uint16_t heap_type) {
    XmType t = {vtag_to_type_kind(vtag), 0, heap_type};
    return t;
}

typedef struct {
    XmIns *def;          // defining instruction
    uint8_t rep;          // XrRep (machine rep: I64/F64/PTR/TAGGED)
    uint16_t heap_type;   // GC heap type (XR_T*), valid when ctype.kind is PTR; 0=unknown
    int8_t reg;           // physical register (-1 = unallocated)
    int16_t bc_slot;      // bytecode register slot (-1 = not mapped)
    int16_t struct_idx;   // proto->struct_layouts[] index (-1 = unknown)
    uint16_t cost;        // spill cost (heuristic)
    uint32_t start, end;  // live range [start, end)
    XrType *xrtype;       // precise static type (NULL = unknown)
    // Compiler hints for optimization
    struct XrProto *callee_proto;   // known callee proto (from OP_CLOSURE/GETSHARED)
    struct XrShape *shape_hint;     // known Json shape (from OP_NEWJSON / type feedback)
    struct XrStructLayout *layout;  // struct field layout (from OP_NEW_STRUCT / params)
    uint8_t array_etype;            // native array element type (0xFF = not a fixed array)
    uint16_t array_ecount;          // native array element count
    bool is_fresh_alloc;            // true if allocated with no GC safepoint since alloc
    // Call argument pool linkage (only valid for CALL dst vregs)
    uint32_t call_arg_start;  // index into func->call_arg_pool
    uint16_t call_nargs;      // number of call arguments (0 = not a call)
} XmVReg;

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
} XmConstVal;

typedef struct {
    XmConstVal val;
    uint8_t rep;  // XrRep (machine representation)
} XmConst;

/* ========== Function-level IR Container ========== */

typedef struct XmFunc {
    const char *name;  // function name (debug)

    // Basic blocks
    XmBlock *entry;    // entry block
    XmBlock **blocks;  // all blocks array
    uint32_t nblk;
    uint32_t blk_cap;
    XmBlock **rpo;  // reverse post-order (computed)

    // Virtual registers
    XmVReg *vregs;
    uint32_t nvreg;
    uint32_t vreg_cap;

    // Constant pool
    XmConst *consts;
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
    XmDeoptInfo *deopt_infos;
    uint32_t ndeopt;
    uint32_t deopt_cap;

    // Cached analysis results (lazily computed, invalidated on CFG change)
    struct XmDomTree *domtree;    // see xm_domtree.h
    struct XmLoopInfo *loopinfo;  // see xm_looptree.h
    struct XmDefUse *defuse;      // see xm_defuse.h
    void *alias;                   // opaque AliasTable *, see xm_alias.c

    // Call argument pool: flat array of XmRef for all CALL instructions.
    // Each CALL dst vreg has (call_arg_start, call_nargs) pointing into this pool.
    // Eliminates STORE_CORO arg-passing pattern; codegen writes call_args[] from pool.
    XmRef *call_arg_pool;
    uint32_t call_arg_pool_used;
    uint32_t call_arg_pool_cap;

    // Defer tracking (AOT mode): deferred closure calls executed at function exit.
    // Each entry records the closure vreg, arg vregs, and arg count.
    // Codegen emits LIFO cleanup at every return point.
    struct {
        XmRef closure;  // closure vreg (XrValue)
        int arg_count;   // number of call arguments (0 for defer { ... })
        XmRef args[8];  // call arg vregs (if arg_count > 0)
    } defer_entries[16];
    int defer_count;  // number of defer entries

    // Flags
    bool has_coro_deopt;  // contains AWAIT/SCOPE_EXIT (OSR unsafe)
    bool conservative;    // compiled in conservative mode (no type speculation)

    // Suspend point tracking (for resume entry generation)
    uint32_t nsuspend;                // number of XM_SUSPEND instructions
    void *suspend_block_helpers[16];  // per-suspend block helper fn ptr (NULL = xr_jit_await_block)

    // Arena allocator for IR nodes (all Xm memory freed together)
    struct XmArena *arena;
} XmFunc;

/* ========== Arena Allocator ========== */

#define XM_ARENA_PAGE_SIZE (16 * 1024)  // 16KB per arena page

typedef struct XmArenaPage {
    struct XmArenaPage *next;
    size_t used;
    size_t size;
    uint8_t data[];  // flexible array member
} XmArenaPage;

typedef struct XmArena {
    XmArenaPage *current;
    XmArenaPage *pages;  // all pages (for freeing)
    size_t total_allocated;
} XmArena;

// Arena API
XR_FUNC void xm_arena_init(XmArena *arena);
XR_FUNC void xm_arena_destroy(XmArena *arena);
XR_FUNC void *xm_arena_alloc(XmArena *arena, size_t size);

// Convenience: allocate zero-initialized memory
static inline void *xm_arena_calloc(XmArena *arena, size_t count, size_t elem_size) {
    size_t total = count * elem_size;
    void *p = xm_arena_alloc(arena, total);
    if (p)
        memset(p, 0, total);
    return p;
}

/* ========== XmFunc API ========== */

// Create/destroy
XR_FUNC XmFunc *xm_func_new(const char *name);
XR_FUNC void xm_func_destroy(XmFunc *func);

// Blocks
XR_FUNC XmBlock *xm_func_add_block(XmFunc *func, const char *label);
XR_FUNC void xm_block_add_pred(XmBlock *blk, XmBlock *pred, XmArena *arena);
XR_FUNC void xm_block_set_jmp(XmBlock *blk, XmBlock *target);
XR_FUNC void xm_block_set_br(XmBlock *blk, XmRef cond, XmBlock *if_true, XmBlock *if_false);
XR_FUNC void xm_block_set_ret(XmBlock *blk, XmRef val);

// Instructions
XR_FUNC XmRef xm_emit(XmFunc *func, XmBlock *blk, uint16_t op, uint8_t type, XmRef a,
                        XmRef b);
XR_FUNC XmRef xm_emit_unary(XmFunc *func, XmBlock *blk, uint16_t op, uint8_t type, XmRef a);
XR_FUNC void xm_emit_void(XmFunc *func, XmBlock *blk, uint16_t op, XmRef a, XmRef b);

// Low-level emit with explicit dst (for Phi lowering and regalloc MOV insertion)
XR_FUNC void xm_emit_raw(XmFunc *func, XmBlock *blk, uint16_t op, uint8_t type, XmRef dst,
                          XmRef arg0, XmRef arg1);

// Constants
XR_FUNC XmRef xm_const_i64(XmFunc *func, int64_t val);
XR_FUNC XmRef xm_const_f64(XmFunc *func, double val);
XR_FUNC XmRef xm_const_ptr(XmFunc *func, void *val);
XR_FUNC XmRef xm_const_string(XmFunc *func, const char *chars, uint32_t len);

// Virtual registers
XR_FUNC XmRef xm_new_vreg(XmFunc *func, uint8_t type);

// Call arg pool
XR_FUNC uint32_t xm_func_add_call_args(XmFunc *func, const XmRef *args, uint16_t nargs);
XR_FUNC void xm_func_bind_call_args(XmFunc *func, XmRef dst, const XmRef *args, uint16_t nargs);

// Phi nodes
XR_FUNC XmPhi *xm_add_phi(XmFunc *func, XmBlock *blk, uint8_t type);
XR_FUNC void xm_phi_set_arg(XmPhi *phi, uint32_t pred_idx, XmRef val);

// Insert a zeroed instruction slot at position `pos`, shifting subsequent instructions.
XR_FUNC XmIns *xm_block_insert_at(XmFunc *func, XmBlock *blk, uint32_t pos);

/* ========== Opcode Info ========== */

XR_FUNC const char *xm_op_name(uint16_t op);
XR_FUNC bool xm_op_is_commutative(uint16_t op);
XR_FUNC bool xm_op_has_side_effect(uint16_t op);
XR_FUNC bool xm_op_is_pure(uint16_t op);

// True for copy-like ops (MOV or REDEFINE) that can be coalesced
static inline bool xm_op_is_copy(uint16_t op) {
    return op == XM_MOV || op == XM_REDEFINE;
}

// Returns true if instruction cannot throw an exception (pure memory access,
// arithmetic, constants).  LICM uses this to hoist from try blocks safely.
static inline bool xm_ins_nothrow(const XmIns *ins) {
    switch (ins->op) {
        case XM_CONST_I64:
        case XM_CONST_F64:
        case XM_CONST_PTR:
        case XM_ADD:
        case XM_SUB:
        case XM_MUL:
        case XM_DIV:
        case XM_AND:
        case XM_OR:
        case XM_XOR:
        case XM_SHL:
        case XM_SHR:
        case XM_NEG:
        case XM_NOT:
        case XM_EQ:
        case XM_NE:
        case XM_LT:
        case XM_LE:
        case XM_GT:
        case XM_GE:
        case XM_LOAD_FIELD:
        case XM_MOV:
        case XM_REDEFINE:
        case XM_PHI:
        case XM_SELECT:
            return true;
        default:
            return !(ins->flags & XM_FLAG_MAY_THROW);
    }
}

// Get compile-time type of a ref by looking up its defining instruction.
// Falls back to rep-based inference for param vregs (no def instruction).
static inline XmType xm_ref_ctype(XmFunc *func, XmRef ref) {
    if (!xm_ref_is_vreg(ref))
        return XM_TYPE_UNKNOWN;
    uint32_t vi = XM_REF_INDEX(ref);
    if (vi >= func->nvreg)
        return XM_TYPE_UNKNOWN;
    XmIns *def = func->vregs[vi].def;
    if (def)
        return def->ctype;
    return xm_type_from_rep(func->vregs[vi].rep);
}

/* ========== Dominator Utilities ========== */

// Compute immediate dominators using Cooper's algorithm.
// idom[] must be pre-allocated with nblk entries.
// (Building block for xm_domtree.c — most callers should
// go through xm_func_get_domtree() instead.)
XR_FUNC void xm_compute_idom(XmFunc *func, uint32_t *idom, uint32_t nblk);

// Rebuild vreg.def pointers by scanning all instructions.
// Call after any pass that moves or compacts instructions in block arrays.
XR_FUNC void xm_rebuild_vreg_defs(XmFunc *func);

#endif  // XM_H
