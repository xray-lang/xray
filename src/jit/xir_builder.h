/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_builder.h - Bytecode → XIR translator
 *
 * KEY CONCEPT:
 *   Translates XrProto bytecode into XIR SSA form, driven by
 *   proto->param_types for parameter types and Blueprint for per-PC types.
 *
 * WHY THIS DESIGN:
 *   - param_types drives parameter type specialization (flow-insensitive, correct)
 *   - Blueprint.inst_info provides per-PC flow-sensitive types for non-params
 *   - bb_leaders bitmap provides zero-cost basic block boundaries
 *   - Simplified SSA with automatic Phi insertion at loops and merge points
 */

#ifndef XIR_BUILDER_H
#define XIR_BUILDER_H

#include "xir.h"
#include "../base/xdefs.h"

// Forward declarations
typedef struct XrProto XrProto;

/* ========== Braun SSA: Per-block Definitions ========== */

typedef struct {
    XirRef defs[256];       // slot -> current SSA ref (XIR_NONE = no local def)
    bool   has_def[256];    // true if this block defines this slot
    bool   sealed;          // true when all predecessors are known
} BraunBlockDef;

#define BRAUN_MAX_INC_PHIS 512

typedef struct {
    uint32_t block_id;
    uint16_t slot;
    XirPhi  *phi;
} BraunIncompletePhi;

/* ========== Loop Info (for back-edge detection) ========== */

#define BUILDER_MAX_LOOPS 16

typedef struct {
    uint32_t header_pc;         // target of backward jump (loop header)
    uint32_t back_edge_pc;      // source of backward jump
} BuilderLoop;

/* ========== Builder State ========== */

typedef struct {
    XirFunc   *func;
    XrProto   *proto;
    struct XrayIsolate *isolate;  // owning isolate (NULL outside JIT; for CHA class lookup)

    // Bytecode register → current XIR vreg mapping (per block)
    XirRef     slot_map[256];   // slot_map[reg] = current XIR ref for bytecode R[reg]
    uint8_t    slot_rep[256];   // machine rep for codegen instruction selection ONLY
    uint8_t    slot_tag[256];   // semantic xr_tag per slot (type judgments use this, not slot_rep)

    // Note: callee_proto, shape_hint, is_fresh_alloc, struct_idx,
    // array_etype/ecount, layout are now stored in XirVReg fields.
    // Access via builder_vreg_for_slot(b, reg)->field_name.

    // Shared variable → proto mapping (for AOT cross-function calls)
    // Set via xir_build_from_proto_ex(); GETSHARED sets slot_proto from this.
    XrProto  **shared_protos;   // indexed by absolute shared_index
    int        nshared_protos;

    // Basic block map: pc → XirBlock*
    XirBlock **pc_to_block;     // indexed by bytecode PC
    uint32_t   code_count;      // total bytecode instructions

    // Loop tracking (back-edge detection for Braun SSA sealing)
    BuilderLoop loops[BUILDER_MAX_LOOPS];
    int         nloops;

    // Braun SSA state
    BraunBlockDef *block_defs;          // [block_defs_size] per-block defs
    uint32_t       block_defs_size;
    BraunIncompletePhi inc_phis[BRAUN_MAX_INC_PHIS];
    uint32_t           n_inc_phis;
    uint32_t           cur_blk_id;     // current block id (for braun_write_var)

    // Exception handling: try/catch nesting stack
    struct {
        XirBlock *catch_block;      // catch basic block
        XirBlock *finally_block;    // finally basic block (NULL if none)
        XirRef saved_slot_map[256]; // slot_map snapshot at try entry
    } try_stack[8];
    int try_depth;

    // Deferred seal: blocks created mid-translation (e.g., AWAIT cont_blk)
    // that need sealing after all instructions are translated.
    uint32_t   deferred_seal[16];
    int        n_deferred_seal;

    // Nullable primitive tag tracking:
    // For slots holding nullable primitive values (int?/float?/bool?),
    // slot_tag_refs[i] holds a vreg containing the runtime XrValue.tag.
    // slot_value_refs[i] holds the vreg at the time the tag was recorded.
    // OP_ISNULL uses tag comparison (tag == XR_TAG_NULL) instead of payload==0
    // when the operand vreg matches slot_value_refs[i].
    // Cleared automatically by builder_set_slot (SSA safety).
    // Set by: param creation (jit_ctx->param_tags), CALL/INVOKE (return_tag).
    XirRef slot_tag_refs[256];
    XirRef slot_value_refs[256];

    // Nullable narrowing: after ISNULL+JMP, the non-null branch can narrow
    // a nullable slot's tag from UNKNOWN to precise (I64/F64/BOOL).
    // Applied when the builder enters the target block.
    int16_t    narrow_slot;        // bytecode slot to narrow (-1 = none)
    uint8_t    narrow_tag;         // precise tag to set (XR_TAG_I64/F64/XRVREG_TAG_BOOL)
    uint8_t    narrow_rep;         // narrowed rep (XR_REP_I64/F64)
    XirBlock  *narrow_block;       // target block where narrowing applies

    // Current bytecode PC being translated (for Blueprint lookup)
    uint32_t   cur_pc;

    // Statistics
    uint32_t   ops_translated;
    uint32_t   ops_skipped;
    const char *nyi_opcode;  // first NYI bytecode name (debug diagnostics)

    // AOT mode: generate closure/upvalue XIR instead of skipping
    bool       aot_mode;

    // Conservative mode: skip type speculation guards (shape/klass guards).
    // Emits generic CALL_C paths to avoid deopt on type-unstable functions.
    // Set when deopt_count >= 5 (adaptive recompile after frequent deopts).
    bool       conservative;
} XirBuilder;

/* ========== API ========== */

// Build XIR from a bytecode function prototype
// Returns NULL on failure (unsupported opcode, etc.)
XR_FUNC XirFunc *xir_build_from_proto(XrProto *proto);

// Build XIR with shared proto mapping (for AOT cross-function calls)
// shared_protos[i] = proto for shared variable at absolute index i
XR_FUNC XirFunc *xir_build_from_proto_ex(XrProto *proto,
                                  XrProto **shared_protos, int nshared);

// Build XIR with dominant shape hint for PTR parameters.
// If dominant_shape is non-NULL, PTR-typed parameters get slot_shape set
// and a GUARD_SHAPE is emitted at function entry (deopt if shape mismatch).
XR_FUNC XirFunc *xir_build_from_proto_shaped(XrProto *proto,
                                      struct XrShape *dominant_shape);

// Build XIR for JIT with shared_protos mapping and optional shape hint
XR_FUNC XirFunc *xir_build_from_proto_jit(XrProto *proto,
                                   XrProto **shared_protos, int nshared,
                                   struct XrShape *dominant_shape,
                                   struct XrayIsolate *isolate);

// Build XIR in AOT mode: generates closure/upvalue XIR instead of skipping.
// If isolate is non-NULL, enables CHA devirtualization for class method calls.
XR_FUNC XirFunc *xir_build_from_proto_aot(XrProto *proto,
                                   XrProto **shared_protos, int nshared,
                                   struct XrayIsolate *isolate);

#endif // XIR_BUILDER_H
