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
 *   Mid-level typed SSA IR that both backends (VM/AOT) consume.
 *   Every SSA value carries an authoritative XrType* from the semantic
 *   analyzer, and XrRep is derived from it via xr_type_rep().
 *
 *   Design follows Go SSA: Value = instruction (def + op + args combined
 *   in one struct). This eliminates the split between "instruction" and
 *   "virtual register" common to lower-level IRs.
 *
 *   Operation granularity is mid-level: high-level semantics preserved
 *   (XI_CALL_METHOD, XI_ITER_NEXT) while control flow is explicit
 *   (basic blocks + branch/jump terminators, phi nodes for merges).
 *
 * NAMING:
 *   Xi prefix (Xray IR) names this typed SSA layer.
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
#include "../base/xconstants.h"
#include "../shared/xr_param_mode.h"
#include "../runtime/value/xtransfer_mode.h"

/* Forward declarations for types defined in other modules */
struct XrType;
struct XrCtValue;
struct AstNode;
struct XaAnalyzer;
struct XiCoroPlan;
struct XiEvidenceSet;
struct XrCExportPlan;
struct XrLinkSymbolPlan;
struct XrFreestandingEntryPlan;

/* ========== IR Stage ========== */

/*
 * Each XiFunc progresses through a sequence of stages.  Stages are
 * monotonically non-decreasing: once a function reaches a stage it
 * never goes back.  Passes and backends declare which stage they
 * require as input and which stage they produce as output, so the
 * pipeline can statically reject illegal orderings.
 *
 * Stages are design-level milestones — not every stage needs to be
 * implemented immediately.  Intermediate stages that are not yet
 * active simply pass through (input == output == same stage).
 */
typedef enum {
    XI_STAGE_RAW = 0,
    XI_STAGE_CANONICAL,
    XI_STAGE_CLOSED,
    XI_STAGE_OWNED,
    XI_STAGE_LOWERED,
    XI_STAGE_OPTIMIZED,
    XI_STAGE_REPPED,
    XI_STAGE_BACKEND,
    XI_STAGE_COUNT,
} XiStage;

/* Human-readable stage name (for dumps and diagnostics). */
static inline const char *xi_stage_name(XiStage s) {
    static const char *names[] = {
        "Raw", "Canonical", "Closed", "Owned", "Lowered", "Optimized", "Repped", "Backend",
    };
    return (unsigned) s < XI_STAGE_COUNT ? names[s] : "?";
}

/*
 * Invariant mask — each bit records an established property.
 * Bits are cumulative: once set, they remain set. Passes and
 * stage-specific verifiers check the mask to gate their work.
 */
typedef uint32_t XiInvariantMask;

#define XI_INV_CFG_WELL_FORMED ((XiInvariantMask) (1u << 0))
#define XI_INV_SSA_WELL_FORMED ((XiInvariantMask) (1u << 1))
#define XI_INV_EVAL_ORDER_FIXED ((XiInvariantMask) (1u << 2))
#define XI_INV_UPVALS_RESOLVED ((XiInvariantMask) (1u << 3))
#define XI_INV_OWNERSHIP_EXPLICIT ((XiInvariantMask) (1u << 4))
#define XI_INV_SEMANTIC_OPS_LOWERED ((XiInvariantMask) (1u << 5))
#define XI_INV_OPTIMIZATION_COMPLETE ((XiInvariantMask) (1u << 6))
#define XI_INV_REPS_SELECTED ((XiInvariantMask) (1u << 7))
#define XI_INV_BACKEND_LEGAL ((XiInvariantMask) (1u << 8))

typedef uint16_t XiVarId;

typedef enum {
    XI_AUX_KIND_NONE = 0,
    XI_AUX_KIND_ADT_FIELD = 1,
    XI_AUX_KIND_PAR_FOR = 2,
    XI_AUX_KIND_PAR_REDUCE = 3,
    XI_AUX_KIND_PAR_MAP = 4,
    XI_AUX_KIND_THREAD_SPAWN = 5,
    XI_AUX_KIND_MAP_LITERAL = 6,
    XI_AUX_KIND_ENUM_NAMESPACE = 7,
    XI_AUX_KIND_ENUM_CASE = 8,
} XiAuxKind;

/* Source-variable IDs are carried on XiValue for backend register/cell
 * coalescing.  0xffff is reserved as the "no source variable" sentinel, so
 * real variables occupy the closed range [0, 65534]. */
#define XI_NO_VAR_ID ((XiVarId) UINT16_MAX)
#define XI_MAX_VAR_ID ((uint32_t) UINT16_MAX - 1u)

static inline bool xi_var_id_is_valid(XiVarId var_id) {
    return var_id != XI_NO_VAR_ID;
}

struct XiValue;

typedef enum XiPlaceOrigin {
    XI_PLACE_ORIGIN_NONE = 0,
    XI_PLACE_ORIGIN_STACK_LOCAL = 1,
    XI_PLACE_ORIGIN_PARAM = 2,
    XI_PLACE_ORIGIN_PROJECTION_TEMP = 3,
} XiPlaceOrigin;

typedef enum XiPlaceLifetime {
    XI_PLACE_LIFETIME_NONE = 0,
    XI_PLACE_LIFETIME_CALL_BOUND = 1,
} XiPlaceLifetime;

typedef enum XiPlaceEscape {
    XI_PLACE_ESCAPE_NONE = 0,
    XI_PLACE_ESCAPE_RETURN = 1,
    XI_PLACE_ESCAPE_STORE = 2,
    XI_PLACE_ESCAPE_CAPTURE = 3,
    XI_PLACE_ESCAPE_THREAD = 4,
} XiPlaceEscape;

typedef struct XiCallArgPlan {
    XrParamMode param_mode;
    XrCallArgAccess access;
    uint8_t origin;
    uint8_t lifetime;
    uint8_t escape;
    bool addressable;
    XiVarId origin_var_id;
    struct XiValue *place;
} XiCallArgPlan;

typedef struct XiCallPlan {
    XiCallArgPlan receiver;
    XiCallArgPlan *args;
    uint16_t nargs;
    bool has_receiver;
    bool verified;
} XiCallPlan;

typedef struct XiLoweringFacts {
    bool initialized;
    bool coroutine_required;
    bool coroutine_lowered;
    bool callable_required;
    bool callable_lowered;
    bool semantic_ops_lowered;
} XiLoweringFacts;

/* Invariant mask implied by reaching a given stage. */
static inline XiInvariantMask xi_stage_invariants(XiStage s) {
    switch (s) {
        case XI_STAGE_RAW:
            return XI_INV_CFG_WELL_FORMED | XI_INV_SSA_WELL_FORMED;
        case XI_STAGE_CANONICAL:
            return xi_stage_invariants(XI_STAGE_RAW) | XI_INV_EVAL_ORDER_FIXED;
        case XI_STAGE_CLOSED:
            return xi_stage_invariants(XI_STAGE_CANONICAL) | XI_INV_UPVALS_RESOLVED;
        case XI_STAGE_OWNED:
            return xi_stage_invariants(XI_STAGE_CLOSED) | XI_INV_OWNERSHIP_EXPLICIT;
        case XI_STAGE_LOWERED:
            return xi_stage_invariants(XI_STAGE_OWNED) | XI_INV_SEMANTIC_OPS_LOWERED;
        case XI_STAGE_OPTIMIZED:
            return xi_stage_invariants(XI_STAGE_LOWERED) | XI_INV_OPTIMIZATION_COMPLETE;
        case XI_STAGE_REPPED:
            return xi_stage_invariants(XI_STAGE_OPTIMIZED) | XI_INV_REPS_SELECTED;
        case XI_STAGE_BACKEND:
            return xi_stage_invariants(XI_STAGE_REPPED) | XI_INV_BACKEND_LEGAL;
        default:
            return 0;
    }
}

/* ========== Operation Kinds ========== */

/*
 * aux / aux_int semantic contract per op (authoritative reference):
 *
 *  Op               aux                  aux_int
 *  ──────────────── ──────────────────── ────────────────────────────
 *  XI_CONST         string: char*        int/bool/char/null literal value
 *                   (other: unused)
 *  XI_PARAM         —                    parameter index
 *  XI_TARGET_SIZEOF —                    XrNativeType whose target C sizeof is needed
 *  XI_TARGET_ALIGNOF —                   XrNativeType whose target C alignment is needed
 *  XI_TARGET_SIMD_BYTES —                canonical target SIMD width query
 *  XI_BIT_*         —                    receiver XrNativeType (exact width/sign contract)
 *  XI_LOAD_FIELD    field name or NULL   symbol id or field index
 *  XI_STORE_FIELD   field name or NULL   symbol id or field index
 *  XI_STATIC_ADDR   —                    shared slot index for freestanding static data
 *  XI_PTR_LOAD      —                    XrFFIType width of pointee | unaligned flag;
 *                                         args[1] carries Endian
 *  XI_PTR_STORE     —                    XrFFIType width of pointee | unaligned flag;
 *                                         args[2] carries Endian
 *  XI_JSON_NEW      char** field_names   field count
 *  XI_JSON_INIT_F   —                    field index
 *  XI_JSON_GET_F    —                    field index
 *  XI_JSON_SET_F    —                    field index
 *  XI_JSON_DECODE   char** field_names   field count
 *  XI_CALL          —                    bits[0:7]=flags, bits[8:15]=nresults
 *  XI_CALL_METHOD   method name (debug)  (global_symbol_id << 1) | is_super
 *  XI_CALL_METHOD_DIRECT method name      method index
 *  XI_CALL_BUILTIN  —                    builtin_id
 *  XI_EXTRACT       —                    obsolete; verifier rejects it
 *  XI_LOAD_UPVAL    —                    upvalue index
 *  XI_STORE_UPVAL   —                    upvalue index
 *  XI_GET_SHARED    —                    shared slot index (relative)
 *  XI_SET_SHARED    —                    shared slot index (relative)
 *  XI_PRINT         —                    print flags
 *  XI_CLOSURE_NEW   XiFunc* (child)      —
 *  XI_CLASS_CREATE  XiClassData*         —
 *  XI_SCOPE_ENTER   —                    scope mode
 *  XI_SCOPE_EXIT    —                    scope mode
 *  XI_YIELD         —                    0=immediate, >0=poll/reduction hint
 *  XI_ASSERT        loc string (char*)   0=assert_true, 1=assert_false
 *  XI_GET_BUILTIN   name string (char*)  global index
 *  XI_IMPORT_REF    XiImportRef*         resolved shared slot (-1=unresolved)
 *  XI_PAR_FOR       XiParallelForData*   —
 *  XI_PAR_MAP   XiParallelMapData* —
 *  XI_PAR_REDUCE    XiParallelReduceData* —
 *
 *  Consumers: xi_emit.c (VM bytecode), xi_cgen.c (AOT).
 *  XI_CALL_METHOD.aux_int carries SymbolId (resolved at lowering time).
 *  xi_emit reads it for OP_INVOKE.
 */

typedef enum {
    /* Constants */
    XI_CONST = 0, /* constant value (int/float/bool/char/null/string in aux) */
    XI_PARAM,     /* function parameter (aux_int = param index) */
    XI_TARGET_SIZEOF,
    XI_TARGET_ALIGNOF,
    XI_TARGET_SIMD_BYTES,

    /* Arithmetic (polymorphic: type determines int vs float) */
    XI_ADD,
    XI_SUB,
    XI_MUL,
    XI_DIV,
    XI_MOD,
    XI_NEG, /* unary negate */

    /* Bitwise */
    XI_BAND, /* & */
    XI_BOR,  /* | */
    XI_BXOR, /* ^ */
    XI_BNOT, /* ~ */
    XI_SHL,  /* << */
    XI_SHR,  /* >> */
    XI_BIT_ROTL,
    XI_BIT_ROTR,
    XI_BIT_BSWAP,
    XI_BIT_POPCOUNT,
    XI_BIT_CLZ,
    XI_BIT_CTZ,
    XI_BIT_MUL_HIGH,

    /* Comparison (result is always bool) */
    XI_EQ,
    XI_NE,
    XI_LT,
    XI_LE,
    XI_GT,
    XI_GE,

    /* Logical */
    XI_NOT, /* ! (unary) */

    /* Type conversion */
    XI_CONVERT,               /* explicit type cast: aux stores target type */
    XI_BOX,                   /* unboxed -> tagged XrValue */
    XI_UNBOX,                 /* tagged -> unboxed (type guard) */
    XI_ENUM_DESCRIPTOR_BOX,   /* typed scalar -> erased {layout, kind, scalar} box */
    XI_ENUM_DESCRIPTOR_UNBOX, /* erased enum descriptor box -> typed scalar */

    /* Explicit narrowing: truncate int64/double to sub-width, re-extend.
     * Result rep stays I64 (or F64 for F32 variant) — only value range changes.
     * Inserted by xi_lower at typed-storage write points. */
    XI_NARROW_I8,  /* int64 → (int8_t)  → int64  (sign-extend back) */
    XI_NARROW_U8,  /* int64 → (uint8_t) → int64  (zero-extend back) */
    XI_NARROW_I16, /* int64 → (int16_t) → int64 */
    XI_NARROW_U16, /* int64 → (uint16_t)→ int64 */
    XI_NARROW_I32, /* int64 → (int32_t) → int64 */
    XI_NARROW_U32, /* int64 → (uint32_t)→ int64 */
    XI_NARROW_F32, /* double → (float)  → double (precision roundtrip) */

    /* Explicit widening: sign/zero extend sub-width to int64.
     * Inserted by xi_lower at typed-storage read points.
     * Makes sign-extension vs zero-extension unambiguous. */
    XI_WIDEN_I8,  /* sign-extend int8 value in int64 */
    XI_WIDEN_U8,  /* zero-extend uint8 value in int64 (mask 0xFF) */
    XI_WIDEN_I16, /* sign-extend int16 */
    XI_WIDEN_U16, /* zero-extend uint16 */
    XI_WIDEN_I32, /* sign-extend int32 */
    XI_WIDEN_U32, /* zero-extend uint32 */
    XI_WIDEN_F32, /* (double)(float) roundtrip — explicit precision gate */

    /* Memory / field access */
    XI_LOAD_FIELD,      /* obj.field: args[0]=obj, aux=name, aux_int=symbol id */
    XI_STORE_FIELD,     /* obj.field=val: args[0]=obj, args[1]=val, aux=name, aux_int=symbol id */
    XI_INDEX_GET,       /* obj[key]: args[0]=obj, args[1]=key */
    XI_INDEX_SET,       /* obj[key]=val: args[0]=obj, args[1]=key, args[2]=val */
    XI_ENUM_VARIANT_AT, /* checked EnumVariants<E>[index] -> EnumVariant<E> */
    XI_ENUM_PAYLOAD_AT, /* checked EnumPayloads<E>[index] -> EnumPayloadField<E> */
    XI_ENUM_META_GET,   /* cold descriptor field: args[0]=enum namespace, args[1]=descriptor */

    /* U8 memory primitives: all offsets/counts are integer values.
     * LOAD args: args[0]=bytes, args[1]=offset, args[2]=Endian.
     * STORE args: args[0]=bytes, args[1]=offset, args[2]=value, args[3]=Endian.
     * COPY_WITHIN args: args[0]=bytes, args[1]=dst, args[2]=src, args[3]=count.
     * COPY_FROM args: args[0]=dst, args[1]=src, args[2]=src_offset,
     *                 args[3]=dst_offset, args[4]=count.
     * REPEAT_FROM args: args[0]=bytes, args[1]=dst, args[2]=distance, args[3]=count. */
    XI_BYTE_SLICE_LOAD_U16,
    XI_BYTE_SLICE_LOAD_U32,
    XI_BYTE_SLICE_LOAD_U64,
    XI_BYTE_SLICE_LOAD_F32,
    XI_BYTE_SLICE_LOAD_F64,
    XI_BYTE_SLICE_STORE_U16,
    XI_BYTE_SLICE_STORE_U32,
    XI_BYTE_SLICE_STORE_U64,
    XI_BYTE_SLICE_STORE_F32,
    XI_BYTE_SLICE_STORE_F64,
    XI_BYTE_SLICE_FILL,
    XI_BYTE_SLICE_COPY,
    XI_BYTE_SLICE_COMPARE,
    XI_BYTE_SLICE_COMMON_PREFIX,
    XI_BYTE_SLICE_REPEAT,
    XI_SLICE_WINDOW,   /* args[0]=Slice<T>, args[1]=start, args[2]=count; strict borrowed slice */
    XI_SLICE_AS_BYTES, /* args[0]=Slice<T>; result Slice<byte>; aux unused */
    XI_SLICE_FILL,     /* args[0]=Slice<T> dst, args[1]=T value; result dst */
    XI_SLICE_COPY,     /* args[0]=Slice<T> dst, args[1]=Slice<T> src; result dst */
    XI_SLICE_COMPARE,  /* args[0]=Slice<T> left, args[1]=Slice<T> right; result int */
    XI_SLICE_REINTERPRET,  /* args[0]=Slice<byte>; result Slice<T>; aux packs elem metadata */
    XI_SLICE_FROM_PTR,     /* args[0]=Ptr<T>, args[1]=count, args[2]=owner; unsafe boundary */
    XI_BUFFER_MATERIALIZE, /* args[0]=moved Buffer; exact native-output proof is analyzer-owned */
    XI_BYTE_ARRAY_COPY_WITHIN,
    XI_BYTE_ARRAY_COPY_FROM,   /* args[0]=dst, args[1]=src, args[2]=src_off,
                                * args[3]=dst_off, args[4]=count */
    XI_BYTE_ARRAY_APPEND_FROM, /* args[0]=dst Array<byte>, args[1]=src Slice<byte>; result dst */
    XI_BYTE_ARRAY_REPEAT_FROM, /* args[0]=dst Array<byte>, args[1]=distance, args[2]=count */
    XI_ARRAY_DATA_PTR,         /* args[0]=Array<T>/Slice<T>/[T;N]; result raw pointer borrow */
    XI_STATIC_BYTES_PTR,       /* aux=exact bytes, aux_int=length; stable Ptr<byte> */
    XI_STATIC_ADDR,            /* aux_int=shared slot; result Ptr<T>/MutPtr<T> to static data */
    XI_LOCAL_ADDR,             /* args[0]=caller SSA slot; call-bound place address */
    XI_PLACE_LOAD,             /* args[0]=call-bound place; result pointee value */
    XI_PLACE_STORE,            /* args[0]=call-bound place, args[1]=value; result void */

    /* FFI raw-pointer memory access. The address is an address-width int
     * (Ptr<T>/MutPtr<T> value). aux_int carries an XrFFIType width code in
     * the low bits plus optional pointer-load flags (see xffi_sig.h) so both
     * backends pick the exact scalar access semantics without re-deriving them
     * from operand types. No bounds check: only valid inside `unsafe`
     * (analyzer-enforced). The pointee group aliases everything (C may mutate
     * it), so these ops are never value-numbered or hoisted.
     * LOAD args: args[0]=address; result type = pointee T.
     * STORE args: args[0]=address, args[1]=value; result void. */
    XI_PTR_LOAD,
    XI_PTR_STORE,
    XI_PTR_COPY_NONOVERLAP, /* args[0]=dst MutPtr<T>, args[1]=src Ptr<T>, args[2]=count */

    /* Struct native storage: typed field access with compile-time layout.
     * args[0]=class_val for NEW; args[0]=struct for GET/SET.
     * aux=XrAggregateLayout*; aux_int=field_index for GET/SET. */
    XI_AGG_NEW,         /* allocate struct: args[0]=class, aux=XrAggregateLayout* */
    XI_AGG_GET,         /* read field: args[0]=struct, aux_int=field_idx, aux=XrAggregateLayout* */
    XI_AGG_SET,         /* write field: args[0]=struct, args[1]=val, aux_int=field_idx,
                              aux=XrAggregateLayout* */
    XI_FIXED_ARRAY_NEW, /* allocate fixed array in frame storage: type=[T; N], aux_int=native */
    XI_FIXED_BYTES_CONST, /* compact byte payload copied into fixed-array frame storage */

    /* Json / Allocation */
    XI_JSON_NEW,     /* Create Json object: aux=field_count, aux_ptr=field_names[] */
    XI_JSON_INIT_F,  /* Init field by index: args[0]=json, args[1]=val, aux_int=field_idx */
    XI_JSON_GET_F,   /* Read field by index: args[0]=json, aux_int=field_idx */
    XI_JSON_SET_F,   /* Write field by index: args[0]=json, args[1]=val, aux_int=field_idx */
    XI_JSON_MERGE,   /* Merge all fields of src into dst: args[0]=dst, args[1]=src
                      * (object spread `{...base}`; later fields override earlier) */
    XI_JSON_DECODE,  /* Typed decode: args[0]=string, aux=field_names[], aux_int=field_count
                      * result: T? (sealed Json or null on validation failure) */
    XI_ARRAY_NEW,    /* new array: args[0]=capacity */
    XI_ARRAY_PUSH,   /* append one element: args[0]=array, args[1]=value (in-place, void) */
    XI_ARRAY_EXTEND, /* splice all elements of src array into dst: args[0]=dst, args[1]=src
                      * (in-place, void; retains each copied element) — array spread `[...a]` */
    XI_MAP_NEW,      /* new map: args[0]=capacity */
    XI_TUPLE_NEW,    /* new tuple: args[0..n-1]=elements, aux_int=n (arity) */
    XI_TUPLE_GET,    /* read tuple field: args[0]=tuple, aux_int=field_index (zero-based) */

    /* Function calls */
    XI_CALL,        /* function call: args[0]=callee, args[1..n]=params
                     * aux_int bits 0-7: flags (1=self_call)
                     * aux_int bits 8-15: nresults (0 means 1) */
    XI_CALL_METHOD, /* method call: args[0]=recv, aux_int=(sym<<1)|super, args[1..n]=params */
    XI_CALL_METHOD_DIRECT,
    XI_TAIL_CALL,        /* general tail call: args[0]=callee, args[1..n]=params
                          * Terminates the function — semantically a call + return.
                          * The current frame is cleaned up (ARC release) before
                          * transferring control.  aux_int mirrors XI_CALL encoding.
                          * Lowered to OP_TAILCALL (function) or OP_INVOKE_TAIL (method). */
    XI_CALL_BUILTIN,     /* builtin call: aux_int=builtin_id, args[0..n]=params */
    XI_ATOMIC_LOAD,      /* canonical Atomic<T>.load; identity is xa_intrinsic_id */
    XI_ATOMIC_STORE,     /* canonical Atomic<T>.store */
    XI_ATOMIC_RMW,       /* canonical RMW; exact operation is xa_intrinsic_id */
    XI_ATOMIC_TO_STRING, /* canonical allocating Atomic<T>.toString */
    XI_EXTRACT,          /* obsolete multi-return extraction marker; verifier-only reject */

    /* Closure / upvalue */
    XI_CLOSURE_NEW, /* create closure: aux=proto, args=captures */
    XI_LOAD_UPVAL,  /* load upvalue: aux_int=upval_index */
    XI_STORE_UPVAL, /* store upvalue: aux_int=upval_index, args[0]=val */

    /* Shared (module-level) variables */
    XI_GET_SHARED, /* load from shared array: aux_int=shared_index */
    XI_SET_SHARED, /* store to shared array: aux_int=shared_index, args[0]=val */

    /* Name-keyed top-level globals (REPL incremental mode).  aux is an
     * arena-owned C string with the binding's source name; the emitter
     * interns it into the proto constant pool and emits OP_GETGLOBAL/
     * OP_SETGLOBAL.  Coexists with XI_GET/SET_SHARED until the script-
     * mode path also migrates. */
    XI_GET_GLOBAL, /* load from globals dict: aux=name (const char*) */
    XI_SET_GLOBAL, /* store to globals dict: aux=name, args[0]=val */

    /* Print (builtin, kept as dedicated op for convenience) */
    XI_PRINT, /* print: args[0..n]=values, aux_int=flags */

    /* Coroutine */
    XI_GO,           /* go expr: args[0]=callee, args[1..n]=params */
    XI_THREAD_SPAWN, /* sys.Thread.spawn: go-like closure on a dedicated OS thread */
    XI_AWAIT,        /* await task: args[0]=task */

    /* Batch-parallel high-level ops.
     * These are semantic placeholders for stdlib parallel.* intrinsic lowering:
     * they carry enough information for VM/AOT to choose persistent workers,
     * lane batching, native result buffers and reducer strategies without
     * preserving the removed dedicated grammar surface. */
    XI_PAR_FOR,
    XI_PAR_MAP,
    XI_PAR_REDUCE,
    XI_TASK_GROUP_NEW,
    XI_TASK_GROUP_SPAWN_RANGE,
    XI_TASK_GROUP_AWAIT_REDUCE,
    XI_TASK_GROUP_JOIN,

    XI_CHAN_SEND,          /* ch.send(v): args[0]=chan, args[1]=val */
    XI_CHAN_RECV,          /* raw recv payload: args[0]=chan; status in adjacent VM slot */
    XI_CHAN_RECV_STATUS,   /* bool status for XI_CHAN_RECV / XI_CHAN_TRY_RECV */
    XI_CHAN_TRY_SEND,      /* internal try-send-ready bool: args[0]=chan, args[1]=val */
    XI_CHAN_TRY_RECV,      /* raw tryRecv payload: args[0]=chan; status in adjacent VM slot */
    XI_CHAN_IS_CLOSED,     /* ch.isClosed: args[0]=chan */
    XI_TIME_AFTER,         /* time.after(ms): args[0]=timeout_ms, returns timer channel */
    XI_CHAN_TIMER_DISPOSE, /* dispose select-owned timer channel: args[0]=timer chan */
    XI_SELECT_BLOCK,       /* blocking select wait: args[0..n]=channels */
    XI_YIELD,              /* cooperative yield (Coro.yield / Gosched) */
    XI_GEN_YIELD,          /* generator `yield expr`: args[0]=value, suspend */
    XI_GEN_CALL, /* call generator fn: args[0]=callee, args[1..n]=params; result=Iterator */
    /* Exception handling (legacy, retained for panic) */
    XI_THROW, /* throw exception: args[0]=value */

    /* Value-return error channel */
    XI_ERR_SET,    /* write args[0] to error channel (no return) */
    XI_ERR_RETURN, /* write args[0] to error channel + return from function */
    XI_ERR_CHECK,  /* after fallible call: check pending_error, propagate if set */
    XI_ERR_CATCH,  /* read pending_error into result, clear error channel */

    /* Iteration (for-in protocol) */
    XI_ITER_NEW,   /* create iterator: args[0]=collection */
    XI_ITER_NEXT,  /* advance + get value: args[0]=iterator, returns next value */
    XI_ITER_VALID, /* test not-done: args[0]=iterator, returns bool */

    /* Defer */
    XI_DEFER,        /* defer expr: args[0]=callee (executed at scope exit) */
    XI_DEFER_MARK,   /* returns current per-frame defer stack count */
    XI_DEFER_RUN_TO, /* args[0]=mark; run defers registered after mark */

    /* Channel creation */
    XI_CHAN_NEW, /* create channel: args[0]=buffer_size (optional) */

    /* Set creation */
    XI_SET_NEW, /* create set: args[0]=capacity */

    /* String concatenation (for template strings) */
    XI_STR_CONCAT, /* concat: args[0..n]=parts, produces string */

    /* Type operations */
    XI_IS, /* runtime type check: args[0]=value, args[1]=type (tid int or class), returns bool */
    XI_AS, /* type cast: args[0]=value, aux=target type */
    XI_CHECKTYPE, /* strict runtime type assertion: args[0]=value, aux_int=(tid<<1)|allow_null */
    XI_SLICE,     /* slice: args[0]=source, args[1]=start, args[2]=end */
    XI_RANGE,     /* range: args[0]=start, args[1]=end */

    /* Obsolete multi-value return packaging; verifier-only reject */
    XI_MULTI_RET,

    /* Null check */
    XI_ISNULL, /* args[0]=value, returns bool (true if null) */

    /* Phi node (not in value list — separate on XiBlock) */
    XI_PHI, /* SSA phi: args[i] corresponds to block->preds[i] */

    /* Conditional select (from if-conversion) */
    XI_SELECT, /* dst = args[0] ? args[1] : args[2] (cond, true_val, false_val) */

    /* Identity / type narrowing.  Most XI_COPY values are optimizer-inserted
     * aliases.  Lowering marks semantic value-struct copies with
     * XI_COPY_KIND_VALUE_CLONE in aux_int so VM/AOT emit an independent clone,
     * and mutable-capture reads with XI_COPY_KIND_CELL_READ so optimizers do
     * not fold them through stale SSA values before backend cell loads. */
    XI_COPY, /* identity by default: dst = args[0], may carry narrowed type */

    /* OOP: class creation */
    XI_CLASS_CREATE, /* create class from descriptor: aux=XiClassData* */

    /* Structured concurrency scope */
    XI_SCOPE_ENTER, /* enter scope: aux_int=scope_mode (0=WAIT,1=LINKED,2=SUPERVISOR) */
    XI_SCOPE_EXIT,  /* exit scope: aux_int=scope_mode, dst=result (supervisor) */

    /* Exception handling (panic channel only) */
    XI_TRY,     /* begin try: marks start of panic-protected region */
    XI_CATCH,   /* catch panic: receive exception into dst register */
    XI_END_TRY, /* end try-catch region */

    /* Builtin calls: compile-time recognized functions */
    XI_ASSERT,        /* args[0]=cond; aux=loc_string; aux_int: 0=true,1=false */
    XI_ASSERT_EQ,     /* args[0]=actual, args[1]=expected; aux=loc_string */
    XI_ASSERT_NE,     /* args[0]=actual, args[1]=unexpected; aux=loc_string */
    XI_ASSERT_THROWS, /* args[0]=fn; aux=loc_string; emits try-catch sequence */
    XI_TYPEID,        /* args[0]=value; result=int XrTypeId */
    XI_TYPENAME,      /* args[0]=value; result=string typename */
    XI_LEN,           /* args[0]=value; aux_int: dynamic lookup may throw */
    XI_GET_BUILTIN,   /* aux=name_string; aux_int=global_index; loads runtime global */

    /* Cross-module import reference (resolved at cgen time).
     * aux = XiImportRef* (module_path + member_name).
     * aux_int = resolved shared index (set by driver post-lowering, -1 if unresolved). */
    XI_IMPORT_REF,

    XI_REGEX_COMPILE, /* args[0]=pattern(str), args[1]=flags(str); compiles regex literal */

    /* Bounds check (inserted before INDEX_GET/INDEX_SET by xi_lower).
     * args[0]=index, args[1]=length; traps if index < 0 || index >= length.
     * Result is args[0] (passthrough for SSA chain).
     * BCE pass eliminates when range(idx) ⊆ [0, range(len).lo - 1]. */
    XI_BOUNDS_CHECK,

    /* Ownership / ARC ops (inserted by xi_arc_insert after escape analysis) */
    XI_RETAIN,        /* args[0]=value; increment refcount (no-op for scalars) */
    XI_RELEASE,       /* args[0]=value; decrement refcount, free if zero (no-op for scalars) */
    XI_SOURCE_MOVE,   /* explicit source consume; requires sealed FinalMoveProof evidence */
    XI_OWNER_FORWARD, /* ARC ownership-edge forwarding; does not invalidate a source binding */

    /* Stack allocation (replaces heap alloc for NO_ESCAPE values).
     * aux_int = original op (XI_ARRAY_NEW etc.) for codegen dispatch.
     * Inherits args from the original op. Freed automatically at return. */
    XI_STACK_ALLOC,

    /* Coro built-in module methods.
     * aux_int = XI_CORO_SUB_* sub-type, args = method arguments.
     * Emits dedicated opcodes (OP_SET_LOCAL, OP_GET_LOCAL, OP_LOCK_THREAD,
     * OP_UNLOCK_THREAD) or OP_CORO_CTRL with sub-opcode. */
    XI_CORO_OP,

    /* Typed SIMD / Vector ops shared by explicit portable SIMD and automatic
     * vectorization.  Explicit shapes encode lane count + XrNativeType in
     * aux_int; legacy automatic-vectorization producers carry only VF. */
    XI_VEC_LOAD,
    XI_VEC_STORE,
    XI_VEC_SPLAT,
    XI_VEC_EXTRACT,
    XI_VEC_REPLACE,
    XI_VEC_ADD,
    XI_VEC_SUB,
    XI_VEC_MUL,
    XI_VEC_BIT_AND,
    XI_VEC_BIT_OR,
    XI_VEC_BIT_XOR,
    XI_VEC_BIT_NOT,
    XI_VEC_SHL,
    XI_VEC_SHR,
    XI_VEC_REINTERPRET,
    XI_VEC_SHUFFLE,
    XI_VEC_WIDEN_MUL,
    XI_VEC_UNZIP,
    XI_VEC_WIDEN_MUL_HALF,
    XI_VEC_REDUCE_ADD,

    XI_OP_COUNT /* sentinel */
} XiOp;

#define XI_SLICE_FROM_PTR_AUX_MUTABLE (INT64_C(1) << 48)
#define XI_BUFFER_MATERIALIZE_AGGREGATE UINT8_C(0xff)
#define XI_BUFFER_MATERIALIZE_AUX(code, size, align)                                               \
    ((int64_t) ((uint64_t) (uint8_t) (code) | ((uint64_t) (uint32_t) (size) << 8) |                \
                ((uint64_t) (uint16_t) (align) << 40)))
#define XI_BUFFER_MATERIALIZE_CODE(aux) ((uint8_t) ((uint64_t) (aux) & UINT64_C(0xff)))
#define XI_BUFFER_MATERIALIZE_SIZE(aux)                                                            \
    ((uint32_t) (((uint64_t) (aux) >> 8) & UINT64_C(0xffffffff)))
#define XI_BUFFER_MATERIALIZE_ALIGN(aux) ((uint16_t) (((uint64_t) (aux) >> 40) & UINT64_C(0xffff)))

static inline bool xi_op_is_identity_forward(uint16_t op) {
    return op == XI_SOURCE_MOVE || op == XI_OWNER_FORWARD;
}

/* XI_CORO_OP sub-type constants (stored in aux_int) */
#define XI_CORO_SUB_SET_LOCAL 0
#define XI_CORO_SUB_GET_LOCAL 1
#define XI_CORO_SUB_LOCK_THREAD 2
#define XI_CORO_SUB_UNLOCK_THREAD 3
/* Values >= XI_CORO_SUB_CTRL_BASE map to OP_CORO_CTRL with
 * sub-opcode = (aux_int - XI_CORO_SUB_CTRL_BASE), which corresponds
 * to the CORO_CTRL_* constants in xchunk.h. */
#define XI_CORO_SUB_CTRL_BASE 100
#define XI_CORO_SUB_LOCAL_NEW (XI_CORO_SUB_CTRL_BASE + 2)

/* Explicit SIMD shape encoding in XiValue.aux_int. */
#define XI_VEC_SHAPE_EXPLICIT (INT64_C(1) << 16)
#define XI_VEC_SHAPE_ODD_LANES (INT64_C(1) << 17)
#define XI_VEC_SHAPE_UNZIP (INT64_C(1) << 18)
#define XI_VEC_SHAPE_CONTIGUOUS_HALF (INT64_C(1) << 19)
#define XI_VEC_SHAPE_LANES_MASK INT64_C(0xff)
#define XI_VEC_SHAPE_NATIVE_SHIFT 8
#define XI_VEC_SHAPE_NATIVE_MASK (INT64_C(0xff) << XI_VEC_SHAPE_NATIVE_SHIFT)
#define XI_VEC_SHAPE_SHUFFLE_SHIFT 24

static inline int64_t xi_vec_shape_encode(uint8_t native_type, uint8_t lanes) {
    return XI_VEC_SHAPE_EXPLICIT | (int64_t) lanes |
           ((int64_t) native_type << XI_VEC_SHAPE_NATIVE_SHIFT);
}

static inline bool xi_vec_shape_is_explicit(int64_t shape) {
    return (shape & XI_VEC_SHAPE_EXPLICIT) != 0;
}

static inline uint8_t xi_vec_shape_lanes(int64_t shape) {
    return (uint8_t) (shape & XI_VEC_SHAPE_LANES_MASK);
}

static inline uint8_t xi_vec_shape_native_type(int64_t shape) {
    return (uint8_t) ((shape & XI_VEC_SHAPE_NATIVE_MASK) >> XI_VEC_SHAPE_NATIVE_SHIFT);
}

/* Import reference metadata for XI_IMPORT_REF.
 * Stored in XiValue.aux, resolved by the AOT driver after all modules
 * are lowered.  The resolved_mod_index + resolved_shared_slot fields
 * are filled in by the driver's cross-module resolution pass. */
typedef struct XiImportRef {
    const char *module_path;  /* import source (e.g. "./math_lib") */
    const char *member_name;  /* exported name (e.g. "square") */
    int resolved_mod_index;   /* index into the driver's module array, -1 = unresolved */
    int resolved_shared_slot; /* shared slot in the target module, -1 = unresolved */
} XiImportRef;

/* Re-export entry for "export { a } from './file'" and "export * from './file'".
 * Stored on XiFunc during lowering, emitted as OP_LOAD_MODULE_SLOT + OP_SET_EXPORT. */
typedef struct XiReexportEntry {
    const char *from_path; /* source module path (arena copy) */
    const char *name;      /* original export name (NULL = star re-export) */
    const char *alias;     /* export alias (NULL = same as name) */
} XiReexportEntry;

/* Arena-safe method descriptor for XI_CLASS_CREATE.
 * One entry per instance/static method, ordered as the class declares them.
 * All strings are arena-allocated (survive AST destruction). */
typedef struct XiClassMethod {
    const char *name;           /* method name (arena copy) */
    int32_t symbol_id;          /* runtime symbol-table ID for dispatch matching */
    bool is_constructor;        /* true for constructor or "constructor" */
    bool is_static;             /* true for static methods */
    bool is_static_constructor; /* true for static constructor */
} XiClassMethod;

/* Lowerer → emitter bridge for XI_CLASS_CREATE.
 * All data is arena-allocated; does NOT depend on AST after lowering. */
typedef struct XiClassData {
    struct AstNode *ast; /* AST_CLASS_DECL node (temporary, may be NULL after lowering) */
    struct XrClassInfo *class_info; /* analyzer class identity; names are diagnostic only */
    const char *class_name;         /* arena copy of class name */
    const char *super_name;         /* arena copy of parent class name (NULL if none) */
    const char
        *generic_origin_name; /* Original generic class name (e.g. "Box"), NULL if not mono */
    const char *display_name; /* User-visible name (e.g. "Box"), NULL = same as class_name */
    XiClassMethod *methods;   /* arena array [nmethod] of method descriptors */
    uint16_t nmethod;         /* total method count (instance + static) */
    uint16_t *child_idx;      /* maps method order → XiFunc::children index */
    uint16_t ninst;           /* instance method count */
    uint16_t nstat;           /* static method count */
    int clinit_child_idx;     /* children index for static constructor (-1 if none) */
    uint32_t derive_flags;    /* XR_DERIVE_* flags copied from declaration attributes */
    bool is_generic_skeleton; /* template class; concrete instances are separate classes */
    bool is_monomorphized;    /* true for mono-generated classes */
    bool is_cycle_candidate;  /* type graph forms a reference cycle (enables RC cycle collector) */
    const char **mono_type_arg_names; /* concrete type display names (e.g. ["int","string"]) */
    int mono_type_arg_count;          /* element count */
    struct XrAggregateLayout *struct_layout;   /* non-NULL for VALUE_TYPE (struct) classes */
    struct XrAggregateLayout *instance_layout; /* non-NULL when class fields have native layout */
    uint16_t inherited_field_count;            /* native class fields inherited from the parent */
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
#define XI_FLAG_MAY_THROW (1 << 1)   /* may raise exception */
#define XI_FLAG_MAY_SUSPEND (1 << 2) /* may yield / await / block on channel */
#define XI_FLAG_READS_MEM (1 << 3)   /* reads heap memory (load_field, index_get, ...) */
#define XI_FLAG_WRITES_MEM (1 << 4)  /* writes heap memory (store_field, index_set, ...) */
#define XI_FLAG_TAIL (1 << 5)        /* tail-position call: emit OP_TAILCALL / OP_INVOKE_TAIL */
#define XI_FLAG_SPEC_CONST (1 << 6)  /* value is speculated constant (IC-guided, SCCP may fold) */
#define XI_FLAG_FIRE_AND_FORGET (1 << 7) /* XI_GO result is not user-visible */

/* Composite masks for query convenience */
#define XI_FLAG_MEM_ANY (XI_FLAG_READS_MEM | XI_FLAG_WRITES_MEM)
#define XI_FLAG_CALL_EFFECTS                                                                       \
    (XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW | XI_FLAG_READS_MEM | XI_FLAG_WRITES_MEM)

/* Compiler-internal facts emitted by lowering for backend-only decisions. */
#define XI_LOWERING_FLAG_PARALLEL_PLAN_LIFECYCLE (1u << 0)
#define XI_LOWERING_FLAG_ALLOCATION_STORAGE_SHIFT 1
#define XI_LOWERING_FLAG_ALLOCATION_STORAGE_MASK (3u << XI_LOWERING_FLAG_ALLOCATION_STORAGE_SHIFT)
/* Physical ABI strategy for a semantic `read` aggregate parameter.  This is
 * deliberately not a fourth source parameter mode: callers still use an
 * ordinary argument, while lowering proves a nonescaping call-bound place. */
#define XI_LOWERING_FLAG_PARAM_READ_PLACE (1u << 3)
/* Exact module-provenance fact for `time.sleep(duration)`.  The VM emitter
 * consumes this fact to select OP_SLEEP; spelling alone is not sufficient
 * because user-defined objects may also have a method named `sleep`. */
#define XI_LOWERING_FLAG_TIME_SLEEP (1u << 4)

/* XI_AWAIT aux_int bits. */
#define XI_AWAIT_AUX_ANY (1 << 0)
#define XI_AWAIT_AUX_ALL (1 << 1)
#define XI_AWAIT_AUX_ANY_SUCCESS (1 << 2)
#define XI_AWAIT_AUX_CONSUME_TASK                                                                  \
    (1 << 3) /* await consumes a direct temporary or unique-result Task handle */
#define XI_AWAIT_AUX_AGGREGATE_ONE_SHOT                                                            \
    (1 << 4) /* await all consumes a fresh task array literal                                      \
              */
#define XI_AWAIT_AUX_SUBMIT_DEFERRED_BATCH                                                         \
    (1 << 5) /* plain await should submit its producer array as a deferred batch first */
#define XI_AWAIT_AUX_INTO_RESULT                                                                   \
    (1 << 6) /* await all writes results into the provided result array and returns unit */

/* XI_GO / XI_THREAD_SPAWN aux_int bits. Low 8 bits carry link mode for GO.
 * THREAD_SPAWN reuses the flag bits and stores stackSize in high bits. */
#define XI_GO_AUX_LINK_MASK 0xff
#define XI_GO_AUX_ONE_SHOT_AWAIT (1 << 8)
#define XI_GO_AUX_DEFER_BATCH (1 << 9) /* defer child submission until aggregate await batch */
#define XI_GO_AUX_RESULT_COPY_SHARED                                                               \
    (1 << 10) /* compiler-planned shared copy for pointer-backed Copy result */
#define XI_THREAD_SPAWN_AUX_STACK_SIZE_SHIFT 16
#define XI_THREAD_SPAWN_AUX_STACK_SIZE_MASK ((int64_t) 0x0000ffffffffffffULL)

typedef struct XiThreadSpawnOptions {
    const char *name;
    int64_t stack_size;
    uint32_t affinity_cpus[XR_THREAD_AFFINITY_MAX];
    uint16_t affinity_count;
    uint8_t *transfer_modes;
} XiThreadSpawnOptions;

/* XI_YIELD aux_int values. */
#define XI_YIELD_AUX_IMMEDIATE 0
#define XI_YIELD_AUX_POLL 1

/* ========== Upvalue Capture Info ========== */

/* Source kinds matching the VM's UpvalInfo.source constants */
#define XI_CAPTURE_SRC_REG 2   /* from enclosing frame's register */
#define XI_CAPTURE_SRC_UPVAL 1 /* from enclosing closure's upvals[] */

#define XI_MAX_CAPTURES 64

/* Capture kind: how the closed-over variable is accessed by the child.
 * Determined during closure analysis (xi_pass_close). */
typedef enum XiCaptureKind {
    XI_CAPTURE_BY_COPY,     /* immutable value copied at closure creation */
    XI_CAPTURE_BY_IMM_REF,  /* immutable reference (large struct, no copy) */
    XI_CAPTURE_BY_MUT_CELL, /* mutable cell indirection (needs_cell) */
    XI_CAPTURE_MODULE_LIVE, /* module-level live binding via shared array */
    XI_CAPTURE_SHARED,      /* stable shared identity capture */
} XiCaptureKind;

typedef struct XiCapture {
    uint8_t source;           /* XI_CAPTURE_SRC_REG or XI_CAPTURE_SRC_UPVAL */
    uint16_t index;           /* SRC_UPVAL: parent upvalue index */
    uint8_t capture_kind;     /* XiCaptureKind */
    bool needs_cell;          /* true if the captured variable is mutated in the child */
    bool is_mutable;          /* true if the variable is ever reassigned after capture */
    bool is_reassigned;       /* true if variable is reassigned after capture point */
    int16_t cell_index;       /* cell table index (-1 = not assigned) */
    int16_t env_offset;       /* offset in closure env object (-1 = not assigned) */
    const char *name;         /* variable name (debug; not owned) */
    struct XrType *type;      /* variable type */
    struct XiValue *value;    /* SRC_REG: parent SSA value (register resolved at emit) */
    uint8_t storage_domain;   /* XrSemanticStorageDomain published by analyzer */
    uint8_t value_capability; /* XaValueCapability published by analyzer */
} XiCapture;

typedef enum XiViewOrigin {
    XI_VIEW_ORIGIN_NONE = 0,
    XI_VIEW_ORIGIN_PARAM = 1,
    XI_VIEW_ORIGIN_RECEIVER = 2,
    XI_VIEW_ORIGIN_STATIC = 3,
    XI_VIEW_ORIGIN_LOCAL = 4,
    XI_VIEW_ORIGIN_MULTI = 5,
    XI_VIEW_ORIGIN_UNKNOWN = 6,
    XI_VIEW_ORIGIN_FOREIGN = 7,
    XI_VIEW_ORIGIN_ALLOCATION = 8,
} XiViewOrigin;

/* Compiler-only proof attached to a Slice-producing Xi value.  Function
 * summaries publish a symbolic PARAM/RECEIVER/STATIC template; lowering
 * instantiates it at a call site by recording the actual Xi operand that is
 * the backing root.  Backends and ARC consume this fact instead of recovering
 * provenance from source syntax or callee names. */
typedef struct XiViewEvidence {
    uint32_t root_value_id;       /* caller-local XiValue id; 0 for STATIC/unknown */
    uint32_t element_type_id;     /* canonical semantic element type, when known */
    uint32_t invalidation_set_id; /* root-relative memory-effect set, 0 if absent */
    int16_t source_operand;       /* operand in producer args[], -1 for STATIC */
    int16_t source_param;         /* symbolic source parameter, -1 if not PARAM */
    uint8_t origin;               /* XiViewOrigin */
    uint8_t capability;           /* 1 = read, 2 = write-exclusive */
    uint8_t lifetime;             /* 1 = caller source, 2 = static */
    uint8_t complete;             /* proof is sufficient for safe consumption */
} XiViewEvidence;

/* Closure metadata: env layout and capture table for a single closure.
 * Built by xi_pass_close from XiFunc.captures[]; replaces ad-hoc
 * backend inspection of capture arrays.  All backends read this. */
typedef struct XiClosureMeta {
    struct XiFunc *function;    /* owning function */
    struct XiFunc *parent_func; /* lexical parent (back-pointer) */
    XiCapture *captures;        /* pointer to func->captures (not owned) */
    uint16_t ncaptures;         /* number of captures */
    uint16_t env_size;          /* total slots in closure env object */
    uint16_t ncells;            /* number of cell indirections */
    bool has_mutable_capture;   /* any capture requires cell */
    bool is_direct_callable;    /* can be called without closure alloc */
} XiClosureMeta;

/* ========== Core Structures ========== */

/*
 * SSA Value: every definition is unique, carries authoritative type.
 * Combines Go SSA's Value design with xray's type system.
 *
 * Size: ~72 bytes. Values are arena-allocated within XiFunc.
 */
typedef struct XiValue {
    uint32_t id;                  /* dense SSA value ID (unique within function) */
    uint16_t op;                  /* XiOp */
    XiVarId var_id;               /* source variable ID for coalescing (XI_NO_VAR_ID = none) */
    uint8_t flags;                /* XI_FLAG_* */
    uint8_t rep;                  /* XrRep: machine representation (set by select_rep,
                                   * default XR_REP_TAGGED until STAGE_REPPED) */
    uint8_t transfer_mode;        /* XrTransferMode for single-value coroutine boundaries.
                                   * Default 0 = SHARE. GO uses its per-arg aux table. */
    uint8_t aux_kind;             /* XiAuxKind: disambiguates aux/aux_int layouts */
    uint8_t escape;               /* XiEscapeLevel (2-bit): escape analysis result
                                   * (set by xi_escape_analyze, default 0 = NO_ESCAPE) */
    uint8_t mem_group;            /* XiMemGroup (TBAA): memory group for alias analysis
                                   * (set by xi_tbaa_annotate, default 0 = XI_MEM_NONE) */
    uint8_t lowering_flags;       /* XI_LOWERING_FLAG_* */
    uint8_t param_mode;           /* XrParamMode for XI_PARAM values (the single param
                                   * contract source; default XR_PARAM_READ). Occupies
                                   * struct padding, so it costs no extra memory. */
    struct XrType *type;          /* authoritative compile-time type (never NULL) */
    int64_t aux_int;              /* auxiliary integer: const value, symbol ID, etc. */
    void *aux;                    /* auxiliary pointer: proto, string literal, etc. */
    XiCallPlan *call_plan;        /* verified read/ref/move call contract */
    XiViewEvidence view_evidence; /* Slice origin/range lifetime proof */
    struct XiValue **args;        /* operand values (SSA uses) */
    uint16_t nargs;               /* number of args */
    int16_t uses;                 /* use count (for DCE; -1 = not computed) */
    uint32_t line;                /* source line number (0 = unknown) */
    uint32_t xg_callsite_id;      /* stable XgCallsiteId for evidence-backed calls (0 = none) */
    uint32_t xa_intrinsic_id;     /* stable XaIntrinsicId for canonical semantic operations */
    uint32_t xg_method_id;        /* XgMethodId or XgInterfaceMethodId for evidence-backed calls */
    uint32_t xg_interface_dispatch_slot; /* interface slot; UINT32_MAX means none */
    uint32_t xg_json_access_id; /* stable XgJsonAccessId for evidence-backed Json slot access */
    uint32_t xg_json_codec_id;  /* stable XgJsonCodecId for evidence-backed Json codec calls */
    uint32_t
        xg_record_access_id; /* stable XgRecordAccessId for evidence-backed Record slot access */
    uint32_t xg_record_merge_id; /* stable XgRecordMergeId for evidence-backed Record spread */
    uint32_t xg_key_access_id;   /* stable XgKeyAccessId for evidence-backed Map/Set key access */
    uint32_t xg_map_shape_id;    /* stable XgMapShapeId for evidence-backed Map/Set construction */
    uint32_t xg_class_field_id;  /* stable XgFieldId for evidence-backed class field access */
    uint32_t
        xg_sequence_access_id;  /* stable XgSequenceAccessId for linear-container access plans */
    uint32_t xg_capacity_op_id; /* stable XgCapacityOpId for capacity/growth plans */
    uint32_t xg_bulk_op_id;     /* stable XgBulkOpId for bulk operation plans */
    uint32_t xg_encoding_op_id; /* stable XgEncodingOpId for encoding validation plans */
    /* Source-move evidence is meaningful only for XI_SOURCE_MOVE. Optimizers
     * must preserve these stable IDs when replacing or migrating the op. */
    uint32_t move_evidence_id;
    uint32_t move_source_root_id;
    uint32_t move_source_symbol_id;
    uint32_t move_storage_plan_id;
    uint32_t move_transfer_plan_id;
    uint32_t move_evidence_bits;
    uint8_t move_source_capability;
    uint8_t move_target_capability;
    uint8_t move_source_domain;
    uint8_t move_target_domain;
    /* Typed enum-domain provenance.  `enum_metadata_owner` is the concrete E
     * in EnumVariant<E>/EnumPayloadField<E>; `enum_metadata_field` is a stable
     * XA_ENUM_META_* id (0 for domain iteration).  This survives lowering so
     * global/AOT evidence never has to recover enum semantics from names. */
    struct XrType *enum_metadata_owner;
    uint8_t enum_metadata_field;
    uint8_t enum_metadata_kind; /* XrEnumMetadataKind for descriptor/view values. */
    struct XiBlock *block;      /* containing block */
} XiValue;

static inline void xi_value_set_allocation_storage_mode(XiValue *value, uint8_t storage_mode) {
    if (!value)
        return;
    value->lowering_flags =
        (uint8_t) ((value->lowering_flags & ~XI_LOWERING_FLAG_ALLOCATION_STORAGE_MASK) |
                   ((storage_mode & 0x03u) << XI_LOWERING_FLAG_ALLOCATION_STORAGE_SHIFT));
}

static inline uint8_t xi_value_allocation_storage_mode(const XiValue *value) {
    return value ? (uint8_t) ((value->lowering_flags & XI_LOWERING_FLAG_ALLOCATION_STORAGE_MASK) >>
                              XI_LOWERING_FLAG_ALLOCATION_STORAGE_SHIFT)
                 : 0;
}

static inline bool xi_value_is_read_place_param(const XiValue *value) {
    return value && value->op == XI_PARAM && value->param_mode == XR_PARAM_READ &&
           (value->lowering_flags & XI_LOWERING_FLAG_PARAM_READ_PLACE) != 0;
}

static inline void xi_value_copy_metadata(XiValue *dst, const XiValue *src) {
    if (!dst || !src)
        return;
    dst->flags = src->flags;
    dst->var_id = src->var_id;
    dst->rep = src->rep;
    dst->transfer_mode = src->transfer_mode;
    dst->aux_kind = src->aux_kind;
    dst->escape = src->escape;
    dst->mem_group = src->mem_group;
    dst->lowering_flags = src->lowering_flags;
    dst->param_mode = src->param_mode;
    dst->aux_int = src->aux_int;
    dst->aux = src->aux;
    dst->view_evidence = src->view_evidence;
    dst->line = src->line;
    dst->xg_callsite_id = src->xg_callsite_id;
    dst->xa_intrinsic_id = src->xa_intrinsic_id;
    dst->xg_method_id = src->xg_method_id;
    dst->move_evidence_id = src->move_evidence_id;
    dst->move_source_root_id = src->move_source_root_id;
    dst->move_source_symbol_id = src->move_source_symbol_id;
    dst->move_storage_plan_id = src->move_storage_plan_id;
    dst->move_transfer_plan_id = src->move_transfer_plan_id;
    dst->move_evidence_bits = src->move_evidence_bits;
    dst->move_source_capability = src->move_source_capability;
    dst->move_target_capability = src->move_target_capability;
    dst->move_source_domain = src->move_source_domain;
    dst->move_target_domain = src->move_target_domain;
    dst->xg_interface_dispatch_slot = src->xg_interface_dispatch_slot;
    dst->xg_json_access_id = src->xg_json_access_id;
    dst->xg_json_codec_id = src->xg_json_codec_id;
    dst->xg_record_access_id = src->xg_record_access_id;
    dst->xg_record_merge_id = src->xg_record_merge_id;
    dst->xg_key_access_id = src->xg_key_access_id;
    dst->xg_map_shape_id = src->xg_map_shape_id;
    dst->xg_class_field_id = src->xg_class_field_id;
    dst->xg_sequence_access_id = src->xg_sequence_access_id;
    dst->xg_capacity_op_id = src->xg_capacity_op_id;
    dst->xg_bulk_op_id = src->xg_bulk_op_id;
    dst->xg_encoding_op_id = src->xg_encoding_op_id;
    dst->enum_metadata_owner = src->enum_metadata_owner;
    dst->enum_metadata_field = src->enum_metadata_field;
    dst->enum_metadata_kind = src->enum_metadata_kind;
}

/* Clone passes remap a Slice producer's operands into a new SSA namespace.
 * Rebase the subject-local ViewEvidence root after that remap so a callee or
 * loop-local value id cannot leak into the cloned proof. */
static inline void xi_value_rebase_view_evidence(XiValue *value) {
    if (!value || !value->view_evidence.complete ||
        value->view_evidence.origin == XI_VIEW_ORIGIN_STATIC)
        return;
    int16_t source_operand = value->view_evidence.source_operand;
    if (source_operand < 0 || source_operand >= (int16_t) value->nargs ||
        !value->args[source_operand])
        return;
    value->view_evidence.root_value_id = value->args[source_operand]->id;
}

typedef struct XiMapLiteralData {
    XiValue **keys;
    XiValue **values;
    uint16_t count;
    uint8_t container_kind;
} XiMapLiteralData;

static inline const XiCallPlan *xi_call_plan(const XiValue *v) {
    return v ? v->call_plan : NULL;
}

static inline bool xi_load_field_is_adt(const XiValue *v) {
    return v && v->op == XI_LOAD_FIELD && v->aux_kind == XI_AUX_KIND_ADT_FIELD;
}

static inline int64_t xi_thread_spawn_stack_size(const XiValue *v) {
    if (!v || v->op != XI_THREAD_SPAWN)
        return 0;
    if (v->aux_kind == XI_AUX_KIND_THREAD_SPAWN) {
        const XiThreadSpawnOptions *opts = (const XiThreadSpawnOptions *) v->aux;
        return opts && opts->stack_size > 0 ? opts->stack_size : 0;
    }
    int64_t stack_size =
        (v->aux_int >> XI_THREAD_SPAWN_AUX_STACK_SIZE_SHIFT) & XI_THREAD_SPAWN_AUX_STACK_SIZE_MASK;
    return stack_size > 0 ? stack_size : 0;
}

static inline const char *xi_thread_spawn_name(const XiValue *v) {
    if (!v || v->op != XI_THREAD_SPAWN || v->aux_kind != XI_AUX_KIND_THREAD_SPAWN)
        return NULL;
    const XiThreadSpawnOptions *opts = (const XiThreadSpawnOptions *) v->aux;
    return opts ? opts->name : NULL;
}

static inline uint16_t xi_thread_spawn_affinity_count(const XiValue *v) {
    if (!v || v->op != XI_THREAD_SPAWN || v->aux_kind != XI_AUX_KIND_THREAD_SPAWN)
        return 0;
    const XiThreadSpawnOptions *opts = (const XiThreadSpawnOptions *) v->aux;
    return opts ? opts->affinity_count : 0;
}

static inline const uint32_t *xi_thread_spawn_affinity_cpus(const XiValue *v) {
    if (!v || v->op != XI_THREAD_SPAWN || v->aux_kind != XI_AUX_KIND_THREAD_SPAWN)
        return NULL;
    const XiThreadSpawnOptions *opts = (const XiThreadSpawnOptions *) v->aux;
    return (opts && opts->affinity_count > 0) ? opts->affinity_cpus : NULL;
}

static inline uint8_t xi_go_arg_transfer_mode(const XiValue *go, uint16_t arg_index) {
    if (!go || (go->op != XI_GO && go->op != XI_THREAD_SPAWN) || arg_index + 1 >= go->nargs)
        return XR_TRANSFER_SHARE;
    const uint8_t *modes = NULL;
    if (go->op == XI_THREAD_SPAWN && go->aux_kind == XI_AUX_KIND_THREAD_SPAWN) {
        const XiThreadSpawnOptions *opts = (const XiThreadSpawnOptions *) go->aux;
        modes = opts ? opts->transfer_modes : NULL;
    } else {
        modes = (const uint8_t *) go->aux;
    }
    return modes ? modes[arg_index] : XR_TRANSFER_SHARE;
}

static inline uint8_t xi_chan_send_transfer_mode(const XiValue *v) {
    if (!v)
        return XR_TRANSFER_SHARE;
    switch (v->op) {
        case XI_CHAN_SEND:
        case XI_CHAN_TRY_SEND:
        case XI_CALL_METHOD:
            return v->transfer_mode;
        default:
            return XR_TRANSFER_SHARE;
    }
}

static inline void xi_chan_send_set_transfer_mode(XiValue *v, uint8_t mode) {
    if (v)
        v->transfer_mode = mode;
}

#define XI_JSON_AUX_STORAGE_SHIFT 32
#define XI_JSON_AUX_FIELD_MASK INT64_C(0xffffffff)

static inline int32_t xi_json_field_count(const XiValue *v) {
    return v ? (int32_t) (v->aux_int & XI_JSON_AUX_FIELD_MASK) : 0;
}

static inline uint8_t xi_json_storage_mode(const XiValue *v) {
    return v ? (uint8_t) ((uint64_t) v->aux_int >> XI_JSON_AUX_STORAGE_SHIFT) : 0;
}

static inline int64_t xi_json_pack_aux(int32_t field_count, uint8_t storage_mode) {
    return ((int64_t) storage_mode << XI_JSON_AUX_STORAGE_SHIFT) |
           ((int64_t) field_count & XI_JSON_AUX_FIELD_MASK);
}

static inline void xi_json_set_storage_mode(XiValue *v, uint8_t storage_mode) {
    if (v && v->op == XI_JSON_NEW)
        v->aux_int = xi_json_pack_aux(xi_json_field_count(v), storage_mode);
}

#define XI_TUPLE_AUX_STORAGE_SHIFT 32
#define XI_TUPLE_AUX_ARITY_MASK INT64_C(0xffffffff)

static inline uint8_t xi_tuple_storage_mode(const XiValue *v) {
    return v ? (uint8_t) ((uint64_t) v->aux_int >> XI_TUPLE_AUX_STORAGE_SHIFT) : 0;
}

static inline int64_t xi_tuple_pack_aux(uint32_t arity, uint8_t storage_mode) {
    return ((int64_t) storage_mode << XI_TUPLE_AUX_STORAGE_SHIFT) |
           ((int64_t) arity & XI_TUPLE_AUX_ARITY_MASK);
}

static inline void xi_tuple_set_storage_mode(XiValue *v, uint8_t storage_mode) {
    if (v && v->op == XI_TUPLE_NEW)
        v->aux_int = xi_tuple_pack_aux(v->nargs, storage_mode);
}

#define XI_COPY_KIND_IDENTITY 0
#define XI_COPY_KIND_VALUE_CLONE INT64_C(0x58434F5059434C4E)
#define XI_COPY_KIND_CELL_READ INT64_C(0x5843454C4C524541)
#define XI_COPY_KIND_LIKELY INT64_C(0x584C494B454C5901)
#define XI_COPY_KIND_UNLIKELY INT64_C(0x58554E4C494B5901)

static inline bool xi_copy_is_value_clone(const XiValue *v) {
    return v && v->op == XI_COPY && v->aux_int == XI_COPY_KIND_VALUE_CLONE;
}

static inline bool xi_copy_is_cell_read(const XiValue *v) {
    return v && v->op == XI_COPY && v->aux_int == XI_COPY_KIND_CELL_READ;
}

static inline bool xi_copy_is_identity_alias(const XiValue *v) {
    return v && v->op == XI_COPY && v->aux_int == XI_COPY_KIND_IDENTITY &&
           v->enum_metadata_owner == NULL && v->enum_metadata_field == 0 &&
           v->enum_metadata_kind == 0;
}

static inline bool xi_copy_is_branch_hint(const XiValue *v) {
    return v && v->op == XI_COPY &&
           (v->aux_int == XI_COPY_KIND_LIKELY || v->aux_int == XI_COPY_KIND_UNLIKELY);
}

/*
 * Phi node: placed at block entry for control-flow merges.
 * Kept separate from the instruction list for efficient iteration.
 * args[i] corresponds to block->preds[i].
 */
typedef struct XiPhi {
    XiValue value;      /* embedded value (op == XI_PHI) */
    struct XiPhi *next; /* linked list within block */
} XiPhi;

/*
 * Basic block: linear sequence of instructions, terminated by block kind.
 *
 * The terminator is encoded in (kind, control) rather than as a trailing
 * instruction. This matches Go SSA's design and simplifies iteration.
 */
typedef struct XiBlock {
    uint32_t id;   /* dense block ID (unique within function) */
    uint16_t kind; /* XiBlockKind */
    bool visited;  /* traversal scratch */
    uint8_t _pad;
    uint32_t line; /* source line for the block terminator (0 = unknown) */

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
    uint32_t rpo;         /* reverse post-order index (0 = not computed) */
    struct XiBlock *idom; /* immediate dominator (NULL for entry) */
    uint16_t dom_depth;   /* depth in dominator tree (entry = 0) */

    /* Braun SSA: block sealing.
     * A block is sealed when all its predecessors are known.
     * Loop headers are unsealed until the back edge is added. */
    bool sealed;

    /* Profile-guided block frequency (0 = no profile).
     * Set by xi_opt_block_layout from VM execution counts. */
    uint32_t frequency;

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

typedef enum XiConstLiteralKind {
    XI_CONST_LITERAL_NONE = 0,
    XI_CONST_LITERAL_NULL,
    XI_CONST_LITERAL_INT,
    XI_CONST_LITERAL_FLOAT,
    XI_CONST_LITERAL_BOOL,
    XI_CONST_LITERAL_CHAR,
    XI_CONST_LITERAL_STRING,
    XI_CONST_LITERAL_COMPTIME_AGGREGATE,
} XiConstLiteralKind;

typedef struct XiConstLiteral {
    XiConstLiteralKind kind;
    struct XrType *type;
    int64_t int_value;
    double float_value;
    bool bool_value;
    const char *string_value;
    const struct XrCtValue *ct_value;
    const char *data_section;
    bool data_used;
    bool data_weak;
    bool data_mutable;
} XiConstLiteral;

typedef struct XiParallelForData {
    struct XiFunc *body_func;
    struct XrType *state_type;
    const char *item_name;
    const char *state_name;
    const char *end_name;
    const char *worker_name;
    uint32_t item_symbol_id;
    uint32_t state_symbol_id;
    uint32_t end_symbol_id;
    uint32_t worker_symbol_id;
    uint16_t body_child_index;
    bool inclusive_end;
    bool range_body;
    bool plan_state;
} XiParallelForData;

typedef struct XiParallelMapData {
    struct XiFunc *body_func;
    struct XrType *element_type;
    struct XrType *state_type;
    const char *item_name;
    const char *state_name;
    const char *worker_name;
    uint32_t item_symbol_id;
    uint32_t state_symbol_id;
    uint32_t worker_symbol_id;
    uint16_t body_child_index;
    uint16_t result_capture_index;
    uint16_t start_capture_index;
    uint16_t lane_count;
    bool direct_lane_writes;
    bool inclusive_end;
    bool into_result;
    bool plan_state;
} XiParallelMapData;

typedef struct XiParallelReduceData {
    struct XiFunc *body_func;
    struct XiFunc *combine_func;
    struct XrType *accumulator_type;
    struct XrType *state_type;
    const char *item_name;
    const char *state_name;
    const char *end_name;
    const char *worker_name;
    uint32_t item_symbol_id;
    uint32_t state_symbol_id;
    uint32_t end_symbol_id;
    uint32_t worker_symbol_id;
    uint16_t body_child_index;
    uint16_t combine_child_index;
    bool inclusive_end;
    bool range_body;
    bool plan_state;
} XiParallelReduceData;

typedef enum XiNativeCallbackKind {
    XI_NATIVE_CALLBACK_NONE = 0,
    XI_NATIVE_CALLBACK_PAR_FOR_I64 = 1,
    XI_NATIVE_CALLBACK_PAR_REDUCE_I64_BODY = 2,
    XI_NATIVE_CALLBACK_PAR_REDUCE_I64_COMBINE = 3,
    XI_NATIVE_CALLBACK_PAR_REDUCE_AGG_BODY = 4,
    XI_NATIVE_CALLBACK_PAR_REDUCE_AGG_COMBINE = 5,
    XI_NATIVE_CALLBACK_PAR_MAP_SCALAR_BODY = 6,
    XI_NATIVE_CALLBACK_PAR_RANGE_I64 = 7,
} XiNativeCallbackKind;

/* Defined in xi_own.h; XiFunc caches a pointer to one (arena-allocated). */
struct XiBorrowSig;

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
    const char *source_file;    /* source path for VM/DAP debug hooks (not owned) */
    struct XrType *return_type; /* return type (from analyzer) */
    uint32_t xg_body_func_id;   /* stable global-evidence XgFuncId for this body (0 = none) */
    uint8_t view_return_source; /* XrViewReturnSourceKind symbolic return template */
    int16_t view_return_param;  /* valid only for PARAM */
    uint8_t view_return_complete;

    /* Function parameters as SSA values (in entry block).  Each param XiValue
     * carries its own XrParamMode in XiValue::param_mode, so the parameter
     * contract lives in one place per parameter with no parallel mode array. */
    XiValue **params;
    uint16_t nparams;

    /* Source variable metadata captured during lowering.  XiValue::var_id
     * indexes these arrays; AOT debug codegen uses them to expose stable
     * source-level locals without depending on the lowerer's temporary tables. */
    uint32_t source_var_count;
    const char **source_var_names;    /* arena-allocated array */
    struct XrType **source_var_types; /* arena-allocated array */

    /* Basic blocks */
    XiBlock **blocks;
    uint32_t nblocks;
    uint32_t blocks_cap;
    XiBlock *entry; /* blocks[0] is always the entry block */

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
    struct XiFunc *parent_func; /* lexical parent, if this is a nested closure */
    struct XiFunc **children;
    uint16_t nchildren;
    uint16_t children_cap;

    /* Upvalue captures (populated during lowering for closures) */
    XiCapture captures[XI_MAX_CAPTURES];
    uint16_t ncaptures;

    /* Shared (module-level) variable count.  Top-level program functions
     * use shared_array for variables that must be visible across closures
     * and support forward references.  Emit records this count on the
     * proto; the VM assigns shared_offset once when the proto is first
     * executed/loaded into a VM.  Shared indices are 0-based local to
     * this func. */
    uint16_t nshared;

    /* Export table: maps shared slot → exported name.  Populated during
     * lowering for top-level declarations so the AOT driver can build
     * cross-module import resolution tables.  NULL entries = not exported. */
    const char **export_names; /* array of nshared entries (arena-alloc'd) */

    /* Per-slot declaration names for REPL/module symbol round-tripping. */
    const char **slot_owned_names; /* array of nshared entries (arena-alloc'd) */

    /* Per-slot const flag, parallel to slot_owned_names. */
    uint8_t *slot_owned_consts; /* array of nshared bytes (arena-alloc'd) */

    /* Per-slot scalar literal metadata for top-level const bindings.  The
     * slot still exists and is initialized normally; optimization can replace
     * GET_SHARED(slot) with XI_CONST when the slot is known immutable and the
     * initializer has folded to a scalar literal. */
    XiConstLiteral *shared_const_literals; /* array of nshared entries (arena-alloc'd) */
    uint16_t shared_const_literal_count;
    XiConstLiteral *shared_init_literals; /* array of nshared static shared-slot initializers */
    uint16_t shared_init_literal_count;

    /* Program-level shared slot -> function mapping, populated during
     * lowering before XiModule is assembled so optimization passes can
     * resolve top-level function closures loaded through XI_GET_SHARED. */
    struct XiFunc **shared_slot_funcs;
    uint16_t shared_slot_func_count;

    /* Cached borrowed-parameter signature for ARC (xi_arc): which parameters
     * this function only borrows. Computed once on the pre-ARC IR and consulted
     * both by this function's own dup/drop placement and by its callers (so a
     * borrowed argument is kept-and-dropped by the caller instead of moved into
     * a callee that never releases it). Arena-allocated; NULL until computed. */
    struct XiBorrowSig *arc_borrow_sig;

    /* ARC return-ownership: 1 if this function provably returns a FRESH (+1)
     * owned reference on every return (a new allocation or a known fresh call),
     * so a caller that discards the result must release it. Computed alongside
     * arc_borrow_sig; meaningful only once arc_borrow_sig is non-NULL. */
    uint8_t arc_returns_fresh;

    /* Re-export table populated during lowering and emitted by emit_reexports. */
    XiReexportEntry *reexports; /* arena-allocated array */
    uint16_t reexport_count;
    uint16_t reexport_capacity;

    /* IR stage, monotonically non-decreasing across pipeline passes. */
    XiStage stage;

    /* Cumulative invariant mask established by passes and stage transitions. */
    XiInvariantMask invariant_mask;

    /* Target-independent coroutine/callable lowering completion facts. */
    XiLoweringFacts lowering_facts;

    /* IR revisions are the authoritative freshness keys for local evidence. */
    uint64_t ir_revision;
    uint64_t memory_revision;
    uint64_t call_revision;
    struct XiEvidenceSet *evidence;

    /* CFG and structural-analysis version tags for lazy recomputation. */
    uint64_t cfg_version;
    uint64_t rpo_version;
    uint64_t dom_version;
    uint64_t loop_version;
    struct XiLoopInfo *loop_cache;

    /* Cache-miss counters reported by xi_pipeline_stats_dump. */
    uint32_t rpo_recomputes;
    uint32_t dom_recomputes;
    uint32_t loop_recomputes;

    /* VM entry metadata (propagated to XrProto during emission) */
    bool is_vararg;      /* has rest parameter (...args) */
    uint8_t entry_type;  /* 0=normal, 1=has_defaults, 2=generator */
    uint16_t min_params; /* required parameter count (no defaults) */
    uint8_t test_attr;   /* AttributeKind: @test / @before_each / etc. */
    int test_timeout;    /* @test(timeout: N) seconds, 0 = no timeout */

    /* Runtime callback ABI classification.  These functions are compiler-
     * synthesized and called by native runtime helpers with a fixed C
     * signature; captures are passed through the hidden closure parameter, so
     * they must not be forced back to the generic tagged closure ABI. */
    XiNativeCallbackKind native_callback_kind;

    /* Body belongs to an open generic owner whose receiver/storage layout is
     * not executable.  Function-level generics instead keep a canonical
     * erased ABI and do not set this bit. */
    bool is_generic_template;

    /* FFI: foreign function declared in an extern "C" block. When set, this XiFunc
     * has no real body — the implementation is a C symbol. The AOT backend
     * emits `extern Ret sym(typed args);` plus direct C calls (no hidden _cl,
     * no tagged boxing); the VM binds `sym` through libffi. A trivial
     * zero-valued return body is synthesized so the IR stays well-formed for
     * pipeline passes; codegen never emits it. */
    bool is_extern;
    const char *extern_symbol; /* C symbol to resolve (defaults to the xray name) */
    const char *extern_dylib;  /* extern-block dylib/link target, or NULL = default/process */

    /* Link-image policy is selected by typed manifest plans, never by source
     * attributes or copied generic booleans. Plans are compiler-session owned. */
    const struct XrCExportPlan *export_plan;
    const struct XrLinkSymbolPlan *link_plan;
    const struct XrFreestandingEntryPlan *entry_plan;

    /* Analyzer-owned allocation proof. Backends consume and verify this
     * publication; they never re-infer allocation semantics from Xi op names. */
    uint8_t allocation_state; /* XaAllocState */
    uint32_t allocation_reason_bits;
    uint64_t allocation_fingerprint;
    bool allocation_effect_complete;

    /* Canonical analyzer-owned return storage contract.  A known transferable
     * or shared return lets lowering materialize fresh aggregates directly in
     * the required domain; runtimes must not repair a local result by copying
     * it at Task/Thread publication time. */
    uint8_t return_storage_domain; /* XrSemanticStorageDomain */
    bool return_storage_known;

    /* Canonical analyzer effect sidecars.  IDs are scoped to `analyzer`; the
     * stable fingerprints are the cache/verifier identity that survives
     * serialization and cross-stage comparison. */
    uint32_t analyzer_effect_id;
    uint32_t analyzer_memory_effect_id;
    uint32_t semantic_effects;
    uint32_t unknown_semantic_effects;
    uint32_t effect_unknown_reasons;
    uint64_t analyzer_effect_fingerprint;
    uint64_t analyzer_memory_effect_fingerprint;
    bool analyzer_effect_complete;
    bool analyzer_memory_effect_complete;
    bool contains_unsafe_op;
    bool requires_unsafe_at_call;

    /* True when params[0] is a borrowed method receiver. */
    bool receiver_borrowed;

    /* True when params[0] is represented as a call-bound place rather than a
     * direct SSA value (currently value-aggregate method receivers). */
    bool receiver_call_place;

    /* True when every operator-overload parameter is borrowed. */
    bool operator_borrowed;

    /* Bitwise OR of XI_FLAG_* across all values. */
    uint8_t effect_summary;

    /* Source info */
    struct XaAnalyzer *analyzer; /* back-pointer for type queries */

    /* Module back-pointer for program-level init functions. */
    struct XiModule *module;

    /* Closure metadata populated by xi_pass_close. */
    XiClosureMeta *closure_meta;

    /* C code generation scratch (assigned by xi_cgen, not by IR construction) */
    int cgen_id; /* unique name suffix for generated C functions */

    /* Active phi coalescing map for this function during AOT emission: a
     * value-id-indexed array mapping a phi's SSA id to the SSA id of the C
     * variable it shares (identity = its own). Non-owning view into the
     * XiCgenCtx scratch buffer, published by cg_build_phi_coalesce so the
     * ctx-less emit_vref can resolve coalesced phi operands. NULL = identity. */
    const uint32_t *phi_coalesce;
    uint32_t phi_coalesce_count;

    /* Backend-neutral coroutine plan produced by xi_coro_analyze() and
     * consumed by both AOT and VM coroutine lowering.  Arena-allocated, so it
     * lives and dies with this XiFunc.  NULL until the function is analyzed. */
    struct XiCoroPlan *coro_plan;

    /* Opaque side-tables owned by analysis passes. */
    void *analysis_data[2];
} XiFunc;

#include "xi_core_api.h"

/* Module metadata, slot map, and closure pass are in xi_module.h */

#endif  // XI_H
