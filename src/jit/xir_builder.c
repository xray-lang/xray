/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_builder.c - Bytecode → XIR translator
 *
 * KEY CONCEPT:
 *   Translates XrProto bytecode into XIR SSA form. Uses param_types
 *   for parameter type info and inst_types for per-PC call result types.
 *
 * WHY THIS DESIGN:
 *   - simplified SSA (no phi insertion, slot-based tracking)
 *   - bb_leaders bitmap drives basic block creation
 *   - param_types drives parameter type specialization
 *   - inst_types[pc] provides per-PC flow-sensitive call result types
 */

#include "xir_builder_internal.h"
#include "../base/xchecks.h"
#include "../base/xlog.h"
#include "../base/xmalloc.h"
#include "xir_printer.h"

// AOT sentinel: used as fn_ptr marker for builtin method invocation
int64_t xrt_invoke_method_sentinel(struct XrCoroutine *c, int64_t x) {
    (void)c; (void)x; return 0;
}

// AOT sentinel: StringBuilder operations
int64_t xrt_strbuf_new_sentinel(struct XrCoroutine *c, int64_t x) {
    (void)c; (void)x; return 0;
}
int64_t xrt_strbuf_append_sentinel(struct XrCoroutine *c, int64_t x) {
    (void)c; (void)x; return 0;
}
int64_t xrt_strbuf_finish_sentinel(struct XrCoroutine *c, int64_t x) {
    (void)c; (void)x; return 0;
}

// Layout macros now in xir_builder_internal.h

/* ========== JIT Type Hint from Receiver Type ========== */

// Derive JIT_TYPE_HINT_* constant for invoke_method IC dispatch.
// Constants must match JIT_TYPE_HINT_* in xir_jit.c.
// 0=NONE, 1=INT, 2=FLOAT, 3=BOOL, 4=STRING, 5=ARRAY, 6=MAP, 7=SET, 8=JSON, 9=INSTANCE
int builder_derive_type_hint(XirBuilder *b, int recv_reg) {
    XR_DCHECK(b != NULL, "derive_type_hint: NULL builder");
    if (b->aot_mode) return 0;
    if (recv_reg < 0 || recv_reg >= 256) return 0;

    uint8_t tag = b->slot_tag[recv_reg];
    if (tag == VTAG_TAGGED) return 0;
    if (tag == VTAG_I64) return 1;
    if (tag == VTAG_F64) return 2;
    if (tag == VTAG_BOOL) return 3;

    // PTR tag: use param_types to distinguish string/array/map/etc.
    if (tag == VTAG_PTR && b->proto->param_types &&
        recv_reg < b->proto->param_types_count && b->proto->param_types[recv_reg]) {
        XrType *rt = b->proto->param_types[recv_reg];
        switch (rt->kind) {
        case XR_KIND_STRING:   return 4;
        case XR_KIND_ARRAY:    return 5;
        case XR_KIND_MAP:      return 6;
        case XR_KIND_SET:      return 7;
        case XR_KIND_JSON:     return 8;
        case XR_KIND_INSTANCE: return 9;
        default: break;
        }
    }
    return 0;
}

/* ========== Slot Rep / Tag Helpers ========== */

// Map XrSlotType to XrRep (machine representation)
uint8_t xr_slot_to_rep(uint8_t slot_type) {
    if (slot_type == XR_SLOT_ANY) return XR_REP_TAGGED;
    if (XR_SLOT_IS_INT(slot_type) || slot_type == XR_SLOT_BOOL) return XR_REP_I64;
    if (XR_SLOT_IS_FLOAT(slot_type)) return XR_REP_F64;
    if (slot_type == XR_SLOT_PTR) return XR_REP_PTR;
    return XR_REP_TAGGED;
}

// Map XrRep to xr_tag for precise JIT value reconstruction.
uint8_t xr_rep_to_tag(uint8_t rep) {
    switch (rep) {
    case XR_REP_I64:    return VTAG_I64;
    case XR_REP_F64:    return VTAG_F64;
    case XR_REP_PTR:    return VTAG_PTR;
    default:            return VTAG_TAGGED;
    }
}

// Check if a slot's type is a numeric-only union (all members are int or float).
// Used to optimize arithmetic ops: codegen can generate inline tag-check code.
bool builder_is_numeric_union(XirBuilder *b, int reg) {
    struct XrType *t = NULL;
    // Only params have reliable static type info (flow-insensitive is correct for params)
    if (reg >= 0 && reg < b->proto->numparams && b->proto->param_types &&
        reg < b->proto->param_types_count)
        t = b->proto->param_types[reg];
    if (!t || t->kind != XR_KIND_UNION) return false;
    for (int i = 0; i < t->union_type.member_count; i++) {
        struct XrType *m = t->union_type.members[i];
        if (!m) return false;
        if (m->kind != XR_KIND_INT && m->kind != XR_KIND_FLOAT) return false;
    }
    return t->union_type.member_count > 0;
}

// Resolve precise xr_tag from compiler type info for a dest slot.
// Called instead of builder_tag_vreg(UNKNOWN) when the opcode handler
// cannot determine the type itself (CALL_C returns, field loads, etc.).
// Uses param_types for parameters, falls back to TAGGED for non-params.
void builder_tag_from_slot(XirBuilder *b, XirRef ref, int dest_slot) {
    uint8_t tag = VTAG_TAGGED;
    if (dest_slot >= 0 && dest_slot < b->proto->numparams &&
        b->proto->param_types && dest_slot < b->proto->param_types_count &&
        b->proto->param_types[dest_slot]) {
        tag = value_tag_to_vtag(xr_type_to_xr_tag(b->proto->param_types[dest_slot]));
    }
    builder_tag_vreg(b, ref, tag, 0);
}

// Get XrType* for a bytecode slot (NULL if untyped/out of range).
// Only returns type info for parameter slots (flow-insensitive is correct
// for params). Non-param slots return NULL; callers use inst_types[pc].
struct XrType *builder_slot_xrtype(XirBuilder *b, int reg) {
    if (reg < 0 || reg >= b->proto->numparams) return NULL;
    if (!b->proto->param_types || reg >= b->proto->param_types_count) return NULL;
    return b->proto->param_types[reg];
}

// Get XrType* for the result of instruction at given PC (NULL if untyped).
// Flow-sensitive: each PC has its own type, independent of register reuse.
struct XrType *builder_inst_xrtype(XirBuilder *b, uint32_t pc) {
    if (!b->proto->inst_types || pc >= b->proto->inst_types_count) return NULL;
    return b->proto->inst_types[pc];
}

// Find XrType* for a bytecode register by backward-scanning from cur_pc.
// 1) Check param_types (if reg is a parameter)
// 2) Scan backward to find the instruction that last wrote to reg
// 3) Return inst_types[that_pc]
// Used by CHA devirt to get receiver type via backward scan.
struct XrType *builder_find_reg_type(XirBuilder *b, int reg) {
    // Fast path: parameter slots
    if (reg >= 0 && reg < b->proto->numparams &&
        b->proto->param_types && reg < b->proto->param_types_count &&
        b->proto->param_types[reg])
        return b->proto->param_types[reg];
    // Backward scan: find last instruction that wrote to reg
    if (!b->proto->inst_types) return NULL;
    for (int32_t pc = (int32_t)b->cur_pc - 1; pc >= 0 && pc > (int32_t)b->cur_pc - 128; pc--) {
        XrInstruction inst = PROTO_CODE(b->proto, (uint32_t)pc);
        int a = GETARG_A(inst);
        if (a == reg && (uint32_t)pc < b->proto->inst_types_count &&
            b->proto->inst_types[pc])
            return b->proto->inst_types[pc];
    }
    return NULL;
}

// Refine slot vtag from compile-time inst_types when available.
// Called after builder_set_slot for ops that default to VTAG_TAGGED
// (CELL_GET, CALL_DIRECT, etc.) to leverage xray's type annotations.
void builder_refine_slot_from_inst_type(XirBuilder *b, int slot) {
    if (slot < 0 || slot >= 256) return;
    struct XrType *t = builder_inst_xrtype(b, b->cur_pc);
    if (!t) return;
    uint8_t xr_tag = xr_type_to_xr_tag(t);
    if (xr_tag == 0xFF) return;  // unknown
    uint8_t vtag = value_tag_to_vtag(xr_tag);
    if (vtag_is_concrete(vtag)) {
        b->slot_tag[slot] = vtag;
        // Also propagate to vreg ctype so codegen (e.g. RET epilogue)
        // can read precise type via xir_ref_ctype instead of defaulting.
        XirRef ref = b->slot_map[slot];
        if (xir_ref_is_vreg(ref)) {
            builder_tag_vreg(b, ref, vtag, 0);
        }
    }
}

// Get semantic xr_tag for a bytecode slot.
// Reads from slot_tag[] which is initialized from proto->param_types
// and auto-synced from vreg.xr_tag in builder_set_slot().
uint8_t builder_slot_xr_tag(XirBuilder *b, int slot) {
    if (slot < 0 || slot >= 256) return VTAG_TAGGED;
    return b->slot_tag[slot];
}

/*
 * Record a deopt snapshot at the current guard point.
 * Captures bc_pc and all live slots (slot_map[i] != XIR_NONE) into
 * func->deopt_infos. Returns the deopt_id assigned, or -1 on overflow.
 */
int builder_add_deopt_info(XirBuilder *b, uint32_t bc_pc) {
    XR_DCHECK(b != NULL, "add_deopt_info: NULL builder");
    XR_DCHECK(b->func != NULL, "add_deopt_info: NULL func");
    XirFunc *func = b->func;
    if (func->ndeopt >= XIR_MAX_DEOPT_POINTS) return -1;

    // Grow deopt_infos array if needed
    if (func->ndeopt >= func->deopt_cap) {
        uint32_t new_cap = func->deopt_cap ? func->deopt_cap * 2 : 16;
        if (new_cap > XIR_MAX_DEOPT_POINTS) new_cap = XIR_MAX_DEOPT_POINTS;
        XR_REALLOC_OR_ABORT(func->deopt_infos, new_cap * sizeof(XirDeoptInfo), "xir: deopt infos");
        func->deopt_cap = new_cap;
    }

    // Count live slots
    int max_slot = b->proto->maxstacksize;
    if (max_slot > 256) max_slot = 256;
    uint16_t nlive = 0;
    for (int i = 0; i < max_slot; i++) {
        if (b->slot_map[i] != XIR_NONE) nlive++;
    }

    // Allocate slot entries via arena
    XirDeoptSlot *slots = NULL;
    if (nlive > 0) {
        slots = (XirDeoptSlot *)xir_arena_alloc(func->arena,
                    nlive * sizeof(XirDeoptSlot));
        if (!slots) return -1;
    }

    // Fill slot entries
    uint16_t idx = 0;
    for (int i = 0; i < max_slot && idx < nlive; i++) {
        if (b->slot_map[i] != XIR_NONE) {
            slots[idx].bc_slot = (int16_t)i;
            // Use proto's bytecode slot type for deopt reconstruction:
            // if the proto declares the slot as untyped (any), use TAGGED
            // so deopt_reconstruct uses raw-value heuristic to detect ptrs.
            // Otherwise use the builder's XIR type for precision.
            uint8_t deopt_type = b->slot_rep[i];
            if (b->proto->param_types && i < b->proto->param_types_count) {
                if (!b->proto->param_types[i])
                    deopt_type = XR_REP_TAGGED;  // untyped → any
            } else if (i >= b->proto->numparams) {
                deopt_type = XR_REP_TAGGED;  // non-param: use TAGGED for safety
            }
            slots[idx].rep = deopt_type;
            // Propagate xr_tag from vreg (precise) or slot_type (hint)
            XirRef ref = b->slot_map[i];
            if (xir_ref_is_vreg(ref)) {
                uint8_t vk = type_kind_to_vtag(xir_ref_ctype(func, ref).kind);
                slots[idx].xr_tag = vtag_to_value_tag(vk);
            } else {
                slots[idx].xr_tag = 0xFF;
            }
            slots[idx].value = b->slot_map[i];
            idx++;
        }
    }

    uint16_t deopt_id = (uint16_t)func->ndeopt;
    XirDeoptInfo *info = &func->deopt_infos[func->ndeopt++];
    info->bc_pc = bc_pc;
    info->nslots = nlive;
    info->deopt_id = deopt_id;
    info->slots = slots;
    return (int)deopt_id;
}

/*
 * Get the XrValue tag for a STORE_FIELD value operand.
 * Prefers vreg.xr_tag (precise), falls back to slot_type hint.
 */
uint8_t builder_ref_sf_tag(XirBuilder *b, XirRef ref, int slot_reg) {
    if (xir_ref_is_vreg(ref)) {
        uint32_t vi = XIR_REF_INDEX(ref);
        if (vi < b->func->nvreg) {
            uint8_t vtag = type_kind_to_vtag(xir_ref_ctype(b->func, ref).kind);
            // Concrete vtags (including VTAG_NULL) map directly to XrValue tags.
            if (vtag_is_concrete(vtag)) return vtag_to_value_tag(vtag);
            if (vtag != VTAG_TAGGED) return XIR_SF_TAG_RUNTIME;
            // VTAG_TAGGED: check for NULL literal (XIR_CONST_PTR with ptr=0)
            XirIns *def = b->func->vregs[vi].def;
            if (def && def->op == XIR_CONST_PTR) {
                XirRef ca = def->args[0];
                if (xir_ref_is_const(ca)) {
                    uint32_t ci = XIR_REF_INDEX(ca);
                    if (ci < b->func->nconst &&
                        b->func->consts[ci].val.ptr == NULL)
                        return 0;  // XR_TAG_NULL
                }
            }
        }
    }
    // Fall back to slot_type hint
    uint8_t st = (slot_reg >= 0 && slot_reg < 256) ? b->slot_rep[slot_reg] : XR_REP_TAGGED;
    uint8_t vt = (uint8_t)xr_rep_to_tag(st);
    return vtag_is_concrete(vt) ? vtag_to_value_tag(vt) : (uint8_t)XIR_SF_TAG_RUNTIME;
}

/* ========== Builder Init / Cleanup ========== */

static void builder_init(XirBuilder *b, XirFunc *func, XrProto *proto) {
    XR_DCHECK(b != NULL, "builder_init: NULL builder");
    XR_DCHECK(func != NULL, "builder_init: NULL func");
    XR_DCHECK(proto != NULL, "builder_init: NULL proto");
    b->func = func;
    b->proto = proto;
    b->code_count = (uint32_t)PROTO_CODE_COUNT(proto);
    b->ops_translated = 0;
    b->ops_skipped = 0;
    b->nloops = 0;
    b->try_depth = 0;
    b->block_defs = NULL;
    b->block_defs_size = 0;
    b->n_deferred_seal = 0;
    b->n_inc_phis = 0;
    b->cur_blk_id = 0;
    b->cur_pc = UINT32_MAX;  // sentinel: not in translate loop
    b->narrow_slot = -1;     // no pending nullable narrowing
    b->aot_mode = false;
    b->isolate = NULL;
    b->shared_protos = NULL;
    b->nshared_protos = 0;
    b->ic_fields_snapshot = NULL;
    b->ic_methods_snapshot = NULL;
    b->ic_builtin_snapshot = NULL;
    b->ic_snapshots_owned = false;
    for (int i = 0; i < 256; i++) {
        b->slot_tag_refs[i] = XIR_NONE;
        b->slot_value_refs[i] = XIR_NONE;
    }

    // Initialize slot_rep/slot_tag: all TAGGED by default.
    // Only parameters get typed from param_types (flow-insensitive is correct
    // for params — they don't change type). Non-param slots are left TAGGED;
    // per-instruction translation + inst_types refine them flow-sensitively.
    memset(b->slot_rep, XR_REP_TAGGED, sizeof(b->slot_rep));
    memset(b->slot_tag, VTAG_TAGGED, sizeof(b->slot_tag));

    if (proto->param_types) {
        for (int i = 0; i < proto->param_types_count && i < 256; i++) {
            struct XrType *t = proto->param_types[i];
            if (t) {
                b->slot_rep[i] = xr_type_rep(t);
                b->slot_tag[i] = value_tag_to_vtag(xr_type_to_xr_tag(t));
            }
        }
    }

    // Initialize slot → ref mapping (all XIR_NONE initially)
    // Other per-slot hints (shape, proto, layout, etc.) are stored in XirVReg
    // fields and initialized to defaults by xir_new_vreg().
    for (int i = 0; i < 256; i++) {
        b->slot_map[i] = XIR_NONE;
    }

    // AOT: prescan bytecode to infer I64/F64 slot types from integer/float opcodes.
    // This catches cases where param_types is ANY but the slot is clearly numeric
    // (e.g. ARRAY_GETC result used by EQI/ADDI).
    if (b->aot_mode && b->code_count > 0) {
        for (uint32_t pc = 0; pc < b->code_count; pc++) {
            XrInstruction inst = PROTO_CODE(proto, pc);
            OpCode op = GET_OPCODE(inst);
            int ra = GETARG_A(inst);
            switch (op) {
            // Opcodes that write I64 to R[A]
            case OP_LOADI:
            case OP_ADDI: case OP_SUBI: case OP_MULI:
            case OP_BAND: case OP_BOR: case OP_BXOR:
            case OP_SHL: case OP_SHR: case OP_BNOT:
                if (b->slot_rep[ra] == XR_REP_TAGGED)
                    b->slot_rep[ra] = XR_REP_I64;
                break;
            // Opcodes that read R[A] as I64 (comparison immediates)
            case OP_EQI: case OP_LTI: case OP_LEI:
                if (b->slot_rep[ra] == XR_REP_TAGGED)
                    b->slot_rep[ra] = XR_REP_I64;
                break;
            // Opcodes that write F64 to R[A]
            case OP_LOADF:
                if (b->slot_rep[ra] == XR_REP_TAGGED)
                    b->slot_rep[ra] = XR_REP_F64;
                break;
            default:
                break;
            }
        }
    }

    // Allocate pc → block map
    b->pc_to_block = (XirBlock **)xr_calloc(b->code_count + 1, sizeof(XirBlock *));
}

static void builder_cleanup(XirBuilder *b) {
    xr_free(b->pc_to_block);
    b->pc_to_block = NULL;

    // Release IC snapshots only when the builder owns them (foreground
    // JIT). Background JIT borrows from XirBgTask and the dispatching
    // worker frees them after the task completes.
    if (b->ic_snapshots_owned) {
        if (b->ic_fields_snapshot) {
            xr_ic_field_table_free(b->ic_fields_snapshot);
            b->ic_fields_snapshot = NULL;
        }
        if (b->ic_methods_snapshot) {
            xr_ic_method_table_free(b->ic_methods_snapshot);
            b->ic_methods_snapshot = NULL;
        }
        if (b->ic_builtin_snapshot) {
            xr_ic_builtin_table_free(b->ic_builtin_snapshot);
            b->ic_builtin_snapshot = NULL;
        }
        b->ic_snapshots_owned = false;
    }
}

// Forward declarations for Braun SSA (defined after slot helpers)
static void braun_write_var(XirBuilder *b, uint32_t blk_id, int slot, XirRef ref);
XirRef braun_read_var(XirBuilder *b, uint32_t blk_id, int slot);

/* ========== Slot Access Helpers ========== */

// Get the XIR ref for a bytecode register
XirRef builder_get_slot(XirBuilder *b, XirBlock *blk, int reg) {
    XR_DCHECK(b != NULL, "builder_get_slot: NULL builder");
    XR_DCHECK(blk != NULL, "builder_get_slot: NULL block");
    XR_DCHECK_BOUNDS(reg, 256, "builder_get_slot: reg out of range");
    if (b->block_defs) {
        XirRef ref = braun_read_var(b, blk->id, reg);
        b->slot_map[reg] = ref; // keep slot_map in sync
        // Tag phi/new vregs with bc_slot for OSR mapping
        if (xir_ref_is_vreg(ref)) {
            uint32_t vi = XIR_REF_INDEX(ref);
            if (vi < b->func->nvreg && b->func->vregs[vi].bc_slot < 0) {
                b->func->vregs[vi].bc_slot = (int16_t)reg;
            }
        }
        return ref;
    }
    if (xir_ref_is_none(b->slot_map[reg])) {
        // First use: create a vreg representing this slot's initial value.
        // Set default tag from rep (xir_new_vreg defaults to UNKNOWN).
        XirRef ref = xir_new_vreg(b->func, b->slot_rep[reg]);
        b->slot_map[reg] = ref;
    }
    return b->slot_map[reg];
}

// Tag a vreg with its XrValue tag and optional heap type.
// Call after xir_emit/xir_new_vreg to annotate semantic type info.
// Also syncs slot_tag if this vreg is currently assigned to a slot,
// so callers don't need to worry about call order with builder_set_slot.
void builder_tag_vreg(XirBuilder *b, XirRef ref, uint8_t xr_tag, uint16_t heap_type) {
    XR_DCHECK(b != NULL, "builder_tag_vreg: NULL builder");
    if (!xir_ref_is_vreg(ref)) return;
    uint32_t vi = XIR_REF_INDEX(ref);
    if (vi < b->func->nvreg) {
        b->func->vregs[vi].heap_type = heap_type;
        // Set ctype on defining instruction
        XirIns *def = b->func->vregs[vi].def;
        if (def) def->ctype = xir_type_from_vtag(xr_tag, heap_type);
        // Sync slot_tag for any slot currently holding this vreg.
        int16_t bc = b->func->vregs[vi].bc_slot;
        if (bc >= 0 && bc < 256 && b->slot_map[bc] == ref) {
            b->slot_tag[bc] = xr_tag;
        }
    }
}

// Set a bytecode register to a new XIR ref (SSA: new definition)
// Also tags the vreg with its bytecode slot for OSR mapping.
// Only sets bc_slot on first assignment to preserve the primary definition site.
// When a slot is overwritten, the old vreg's bc_slot is relocated to another
// slot still holding it, or invalidated (-1). This prevents OSR from loading
// stale values when bytecode registers are reused (e.g. CLOSURE R[3] then
// LOADI R[3] reuses the slot for a different purpose).
void builder_set_slot(XirBuilder *b, int reg, XirRef ref) {
    XR_DCHECK(b != NULL, "builder_set_slot: NULL builder");
    XR_DCHECK_BOUNDS(reg, 256, "builder_set_slot: reg out of range");
    XirRef old = b->slot_map[reg];
    if (xir_ref_is_vreg(old) && old != ref) {
        uint32_t old_vi = XIR_REF_INDEX(old);
        if (old_vi < b->func->nvreg &&
            b->func->vregs[old_vi].bc_slot == (int16_t)reg) {
            /* PHI dst vregs (def==NULL) must keep their original bc_slot.
             * OSR triggers at loop back-edges AFTER FORLOOP updates R[A+2]
             * but BEFORE the body re-executes (e.g. MOVE R[3] R[2]).
             * Relocating to an alias slot (like R[3]) would cause OSR to
             * load a stale value from values[3] instead of values[2]. */
            if (b->func->vregs[old_vi].def == NULL) {
                // PHI dst: keep original bc_slot, do not relocate
            } else {
                int16_t new_slot = -1;
                int max = b->proto->maxstacksize;
                for (int s = 0; s < max; s++) {
                    if (s != reg && b->slot_map[s] == old) {
                        new_slot = (int16_t)s;
                        break;
                    }
                }
                // Only relocate if another slot was found; keep original
                // bc_slot when no alternate exists — OSR needs it to load
                // phi-dst vregs from the interpreter's register array.
                // The regalloc snapshot ensures only live vregs are loaded
                // at each loop header, so stale-value risk is eliminated.
                if (new_slot >= 0)
                    b->func->vregs[old_vi].bc_slot = new_slot;
            }
        }
    }
    b->slot_map[reg] = ref;
    // clear tag tracking on any slot reassignment (SSA safety).
    // Callers that set a new tag (param load, INVOKE) do so AFTER this call.
    if (reg >= 0 && reg < 256) {
        b->slot_tag_refs[reg] = XIR_NONE;
        b->slot_value_refs[reg] = XIR_NONE;
    }
    if (xir_ref_is_vreg(ref)) {
        uint32_t vi = XIR_REF_INDEX(ref);
        if (vi < b->func->nvreg) {
            if (b->func->vregs[vi].bc_slot < 0)
                b->func->vregs[vi].bc_slot = (int16_t)reg;

            // Sync slot_rep from vreg's machine type so PHI creation
            // uses the correct representation.
            if (reg >= 0 && reg < 256) {
                uint8_t vrep = b->func->vregs[vi].rep;
                if (vrep != XR_REP_VOID && vrep != XR_REP_TAGGED)
                    b->slot_rep[reg] = vrep;
            }

            // Sync slot_tag from vreg's compile-time vtag.
            // Reset first to prevent stale tag when slots are reused.
            if (reg >= 0 && reg < 256) {
                b->slot_tag[reg] = VTAG_TAGGED;
                uint8_t vt = type_kind_to_vtag(xir_ref_ctype(b->func, ref).kind);
                if (vt != VTAG_TAGGED)
                    b->slot_tag[reg] = vt;
                // NOTE: No static type-based refinement here. Static type
                // info is flow-insensitive (union of all types a slot holds
                // across all PCs). Using it to override instruction-level
                // tags is wrong when slots are reused (e.g. R[3]=MOD result then
                // R[3]=LOADFALSE). All bool-producing instructions already
                // call builder_tag_vreg(VTAG_BOOL) explicitly.
            }
        }
    }
    // Braun SSA: also write to per-block definition table
    if (b->block_defs) {
        braun_write_var(b, b->cur_blk_id, reg, ref);
    }
}

// Write a slot in a specific block (cross-block variant of builder_set_slot).
// Used for parameter initialization, CPS suspend results, and other cases
// where we need to define a variable in a block other than the current one.
void builder_set_slot_in_block(XirBuilder *b, uint32_t blk_id,
                                int slot, XirRef ref) {
    braun_write_var(b, blk_id, slot, ref);
    if (xir_ref_is_vreg(ref)) {
        uint32_t vi = XIR_REF_INDEX(ref);
        if (vi < b->func->nvreg) {
            if (b->func->vregs[vi].bc_slot < 0)
                b->func->vregs[vi].bc_slot = (int16_t)slot;
            if (slot >= 0 && slot < 256)
                b->slot_map[slot] = ref;
        }
    }
}

// Emit GUARD_SHAPE for slot_shape fast paths.
// The guard includes a null check (CBZ → deopt) and shape_id comparison.
// GVN/CSE will eliminate redundant guards on the same object within a block.
void builder_emit_shape_guard(XirBuilder *b, XirBlock *blk,
                                     XirRef obj, struct XrShape *shape,
                                     uint32_t pc) {
    XirRef shape_id_ref = xir_const_i64(b->func,
        (int64_t)(uint32_t)shape->id);
    xir_emit(b->func, blk, XIR_GUARD_SHAPE, XR_REP_VOID,
             obj, shape_id_ref);
    blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
    int did = builder_add_deopt_info(b, pc);
    if (did >= 0) {
        blk->ins[blk->nins - 1].dst =
            xir_const_i64(b->func, (int64_t)did);
    }
}

// Get the actual XIR type of a ref (from vreg table, not slot declaration)
uint8_t ref_xir_type(XirFunc *func, XirRef ref) {
    if (xir_ref_is_vreg(ref)) {
        uint32_t idx = XIR_REF_INDEX(ref);
        if (idx < func->nvreg) return func->vregs[idx].rep;
    }
    return XR_REP_TAGGED;
}

// Get flow-sensitive vtag from a ref via its defining instruction's ctype
uint8_t ref_vtag(XirFunc *func, XirRef ref) {
    return type_kind_to_vtag(xir_ref_ctype(func, ref).kind);
}

/* ========== Braun SSA Construction ========== */

// Forward declarations
XirRef braun_read_var(XirBuilder *b, uint32_t blk_id, int slot);
static XirRef braun_read_var_recursive(XirBuilder *b, uint32_t blk_id, int slot);

// Write a variable definition in a specific block (internal — use builder_set_slot_in_block)
static void braun_write_var(XirBuilder *b, uint32_t blk_id, int slot, XirRef ref) {
    XR_DCHECK(b != NULL, "braun_write_var: NULL builder");
    XR_DCHECK_BOUNDS(slot, 256, "braun_write_var: slot out of range");
    if (blk_id < b->block_defs_size) {
        b->block_defs[blk_id].defs[slot] = ref;
        b->block_defs[blk_id].has_def[slot] = true;
    }
}

// Read a variable: check local def first, then walk predecessors
XirRef braun_read_var(XirBuilder *b, uint32_t blk_id, int slot) {
    XR_DCHECK(b != NULL, "braun_read_var: NULL builder");
    XR_DCHECK_BOUNDS(slot, 256, "braun_read_var: slot out of range");
    if (blk_id < b->block_defs_size && b->block_defs[blk_id].has_def[slot])
        return b->block_defs[blk_id].defs[slot];
    return braun_read_var_recursive(b, blk_id, slot);
}

// Fill phi operands from all predecessors
static XirRef braun_add_phi_operands(XirBuilder *b, uint32_t blk_id, int slot, XirPhi *phi) {
    if (blk_id >= b->func->nblk) return phi->dst;
    XirBlock *blk = b->func->blocks[blk_id];

    // npred may have grown since phi was created (e.g. back-edges added
    // after the incomplete phi was created for an unsealed loop header).
    if (blk->npred > phi->narg) {
        XirRef *new_args = (XirRef *)xir_arena_calloc(
            b->func->arena, blk->npred, sizeof(XirRef));
        if (new_args) {
            for (uint16_t i = 0; i < phi->narg; i++)
                new_args[i] = phi->args[i];
            for (uint32_t i = phi->narg; i < blk->npred; i++)
                new_args[i] = XIR_NONE;
            phi->args = new_args;
            phi->narg = blk->npred;
        }
    }

    for (uint32_t p = 0; p < blk->npred; p++) {
        XirBlock *pred = blk->preds[p];
        if (!pred) continue;
        XirRef val = braun_read_var(b, pred->id, slot);
        xir_phi_set_arg(phi, p, val);
    }

    // Trivial phi elimination (Braun SSA standard step):
    // If all operands (ignoring self-refs and NONE) are the same single value,
    // the phi is redundant. Return the unique value instead of the phi dst.
    // This collapses intermediate phis created in multi-predecessor blocks
    // (e.g. loop latch with 2 preds both feeding the same loop header phi).
    {
        XirRef same_val = XIR_NONE;
        bool trivial = true;
        for (uint32_t p = 0; p < phi->narg && trivial; p++) {
            XirRef arg = phi->args[p];
            if (xir_ref_is_none(arg) || arg == phi->dst) continue;
            if (xir_ref_is_none(same_val)) {
                same_val = arg;
            } else if (arg != same_val) {
                trivial = false;
            }
        }
        if (trivial && !xir_ref_is_none(same_val)) {
            return same_val;
        }
    }

    // Reconcile phi rep with actual operand reps.
    // slot_rep[] is flow-insensitive and may be stale when a bytecode register
    // is reused across type boundaries (e.g. R[3] first holds I64 from
    // TARRAY_GETC, then TAGGED array from MOVE). Fix the phi rep if all
    // concrete operands agree on a rep that differs from the initial one.
    {
        uint8_t common_rep = 0xFF;
        bool rep_agree = true;
        for (uint32_t p = 0; p < phi->narg && rep_agree; p++) {
            XirRef arg = phi->args[p];
            if (xir_ref_is_none(arg) || arg == phi->dst) continue;
            if (!xir_ref_is_vreg(arg)) { rep_agree = false; break; }
            uint32_t vi = XIR_REF_INDEX(arg);
            if (vi >= b->func->nvreg) { rep_agree = false; break; }
            uint8_t r = b->func->vregs[vi].rep;
            if (common_rep == 0xFF) common_rep = r;
            else if (r != common_rep) rep_agree = false;
        }
        if (rep_agree && common_rep != 0xFF && common_rep != phi->rep) {
            phi->rep = common_rep;
            if (xir_ref_is_vreg(phi->dst)) {
                uint32_t di = XIR_REF_INDEX(phi->dst);
                if (di < b->func->nvreg)
                    b->func->vregs[di].rep = common_rep;
            }
        }
    }

    // Propagate xr_tag through PHI: if all inputs share the same
    // non-default tag, the PHI dst inherits it.  This propagates
    // meta-tags like XRVREG_TAG_BOOL through && / || merges.
    if (xir_ref_is_vreg(phi->dst) && blk->npred > 0) {
        uint8_t common_tag = 0;
        bool tag_valid = false;
        bool tag_mismatch = false;
        for (uint32_t p = 0; p < phi->narg && !tag_mismatch; p++) {
            if (!xir_ref_is_vreg(phi->args[p])) continue;
            uint32_t ai = XIR_REF_INDEX(phi->args[p]);
            if (ai >= b->func->nvreg) continue;
            uint8_t t = type_kind_to_vtag(xir_ref_ctype(b->func, phi->args[p]).kind);
            if (t == VTAG_TAGGED) continue;
            if (!tag_valid) {
                common_tag = t;
                tag_valid = true;
            } else if (t != common_tag) {
                tag_mismatch = true;
            }
        }
        if (tag_valid && !tag_mismatch) {
            uint32_t di = XIR_REF_INDEX(phi->dst);
            if (di < b->func->nvreg) {
                XirIns *def = b->func->vregs[di].def;
                if (def) def->ctype.kind = vtag_to_type_kind(common_tag);
            }
        }
    }
    return phi->dst;
}

// Recursive variable lookup: create phi for multi-predecessor blocks
static XirRef braun_read_var_recursive(XirBuilder *b, uint32_t blk_id, int slot) {
    if (blk_id >= b->block_defs_size || blk_id >= b->func->nblk) return XIR_NONE;
    BraunBlockDef *bdef = &b->block_defs[blk_id];
    XirBlock *blk = b->func->blocks[blk_id];
    XirRef val;

    if (!bdef->sealed) {
        // Unsealed block (loop header): create incomplete phi
        uint8_t phi_type = b->slot_rep[slot];
        XirPhi *phi = xir_add_phi(b->func, blk, phi_type);
        if (b->n_inc_phis < BRAUN_MAX_INC_PHIS) {
            BraunIncompletePhi *ip = &b->inc_phis[b->n_inc_phis++];
            ip->block_id = blk_id;
            ip->slot = (uint16_t)slot;
            ip->phi = phi;
        }
        val = phi->dst;
    } else if (blk->npred == 0) {
        // No predecessors (entry/preheader): create vreg for undefined slot
        val = xir_new_vreg(b->func, b->slot_rep[slot]);
    } else if (blk->npred == 1) {
        // Single predecessor: recurse
        XirBlock *pred = blk->preds[0];
        if (!pred) return XIR_NONE;
        val = braun_read_var(b, pred->id, slot);
    } else {
        // Multiple predecessors: create phi, write BEFORE recursing (break cycles)
        uint8_t phi_type = b->slot_rep[slot];
        XirPhi *phi = xir_add_phi(b->func, blk, phi_type);
        val = phi->dst;
        bdef->defs[slot] = val;
        bdef->has_def[slot] = true;
        val = braun_add_phi_operands(b, blk_id, slot, phi);
    }

    // Tag phi/new vregs with bc_slot for OSR mapping
    if (xir_ref_is_vreg(val)) {
        uint32_t vi = XIR_REF_INDEX(val);
        if (vi < b->func->nvreg && b->func->vregs[vi].bc_slot < 0) {
            b->func->vregs[vi].bc_slot = (int16_t)slot;
        }
    }

    // Cache result
    bdef->defs[slot] = val;
    bdef->has_def[slot] = true;
    return val;
}

// Seal a block: all predecessors are known, complete incomplete phis
void braun_seal_block(XirBuilder *b, XirBlock *blk) {
    XR_DCHECK(b != NULL, "braun_seal_block: NULL builder");
    XR_DCHECK(blk != NULL, "braun_seal_block: NULL block");
    uint32_t bid = blk->id;
    if (bid >= b->block_defs_size) return;
    if (b->block_defs[bid].sealed) return;

    // Fill all incomplete phis for this block, then refine their reps.
    // Collect phis that belong to this block for post-seal rep refinement.
    XirPhi *sealed_phis[BRAUN_MAX_INC_PHIS];
    uint32_t nsealed = 0;
    uint32_t write = 0;
    for (uint32_t i = 0; i < b->n_inc_phis; i++) {
        BraunIncompletePhi *ip = &b->inc_phis[i];
        if (ip->block_id == bid) {
            braun_add_phi_operands(b, bid, ip->slot, ip->phi);
            if (nsealed < BRAUN_MAX_INC_PHIS)
                sealed_phis[nsealed++] = ip->phi;
        } else {
            b->inc_phis[write++] = *ip;
        }
    }
    b->n_inc_phis = write;
    b->block_defs[bid].sealed = true;

    // PHI rep refinement: when all inputs share a concrete rep (I64/F64/PTR),
    // upgrade the PHI from TAGGED to that rep. This enables the loop body
    // to use native instructions instead of CALL_C slow paths.
    for (uint32_t pi = 0; pi < nsealed; pi++) {
        XirPhi *phi = sealed_phis[pi];
        if (!phi || phi->narg == 0) continue;
        // Only refine if PHI is currently TAGGED (conservative default)
        if (phi->rep != XR_REP_TAGGED) continue;

        uint8_t common_rep = 0xFF;
        bool all_match = true;
        for (uint16_t ai = 0; ai < phi->narg && all_match; ai++) {
            if (xir_ref_is_none(phi->args[ai])) continue;
            if (!xir_ref_is_vreg(phi->args[ai])) continue;
            uint32_t vi = XIR_REF_INDEX(phi->args[ai]);
            if (vi >= b->func->nvreg) continue;
            uint8_t r = b->func->vregs[vi].rep;
            if (r == XR_REP_TAGGED) { all_match = false; break; }
            if (common_rep == 0xFF) common_rep = r;
            else if (r != common_rep) all_match = false;
        }
        if (all_match && common_rep != 0xFF) {
            phi->rep = common_rep;
            // Also upgrade the PHI dst vreg rep
            if (xir_ref_is_vreg(phi->dst)) {
                uint32_t di = XIR_REF_INDEX(phi->dst);
                if (di < b->func->nvreg)
                    b->func->vregs[di].rep = common_rep;
            }
        }
    }
}

// Check if a block is sealed
static bool braun_is_sealed(XirBuilder *b, uint32_t block_id) {
    if (block_id >= b->block_defs_size) return true;
    return b->block_defs[block_id].sealed;
}

// Check if a PC is a loop header (has backward edge targeting it)
static bool braun_is_loop_header(XirBuilder *b, uint32_t pc) {
    for (int i = 0; i < b->nloops; i++) {
        if (b->loops[i].header_pc == pc) return true;
    }
    return false;
}

// Initialize Braun block_defs array (call after builder_create_blocks)
static void braun_init(XirBuilder *b) {
    uint32_t nblk = b->func->nblk;
    b->block_defs_size = nblk + 8; // room for preheader etc.
    b->block_defs = (BraunBlockDef *)xr_calloc(b->block_defs_size, sizeof(BraunBlockDef));
    b->n_inc_phis = 0;
}

// Free Braun state
static void braun_cleanup(XirBuilder *b) {
    if (b->block_defs) {
        xr_free(b->block_defs);
        b->block_defs = NULL;
    }
}

/* ========== Basic Block Construction ========== */

// Create basic blocks using bb_leaders bitmap
static void builder_create_blocks(XirBuilder *b) {
    // Entry block always exists
    XirBlock *entry = xir_func_add_block(b->func, "entry");
    b->pc_to_block[0] = entry;

    // Use bb_leaders bitmap to find block boundaries
    if (b->proto->bb_leaders) {
        for (uint32_t pc = 1; pc < b->code_count; pc++) {
            int byte_idx = pc / 8;
            int bit_idx = pc % 8;
            if (byte_idx < b->proto->bb_leaders_size &&
                (b->proto->bb_leaders[byte_idx] & (1 << bit_idx))) {
                char label[16];
                snprintf(label, sizeof(label), "bb%u", pc);
                XirBlock *blk = xir_func_add_block(b->func, NULL);
                b->pc_to_block[pc] = blk;
            }
        }
    }

    // Also create blocks for jump targets (fallback if no bb_leaders)
    for (uint32_t pc = 0; pc < b->code_count; pc++) {
        XrInstruction inst = PROTO_CODE(b->proto, pc);
        OpCode op = GET_OPCODE(inst);

        uint32_t target = 0;
        bool has_target = false;

        switch (op) {
            case OP_JMP: {
                int sJ = GETARG_sJ(inst);
                target = (uint32_t)((int32_t)pc + 1 + sJ);
                has_target = true;
                // Also create block for fall-through after JMP (for TEST+JMP pattern)
                if (pc + 1 < b->code_count && !b->pc_to_block[pc + 1]) {
                    b->pc_to_block[pc + 1] = xir_func_add_block(b->func, NULL);
                }
                break;
            }
            case OP_TRY: {
                // catch target from Bx field
                target = (uint32_t)GETARG_Bx(inst);
                has_target = true;
                // Also check next instruction for finally offset
                if (pc + 1 < b->code_count) {
                    XrInstruction next = PROTO_CODE(b->proto, pc + 1);
                    uint32_t finally_target = (uint32_t)GETARG_Bx(next);
                    if (finally_target > 0 && finally_target < b->code_count &&
                        !b->pc_to_block[finally_target]) {
                        b->pc_to_block[finally_target] = xir_func_add_block(b->func, NULL);
                    }
                }
                break;
            }
            case OP_LOOP_BACK: {
                // Tail recursion jump back to function entry (pc=0)
                target = 0;
                has_target = true;
                break;
            }
            // AWAIT/SCOPE_EXIT/CHAN_SEND/CHAN_RECV split the block: pc+1 becomes
            // a new block leader so the continuation block exists before
            // translation starts.
            case OP_AWAIT:
            case OP_SCOPE_EXIT:
            case OP_CHAN_SEND:
            case OP_CHAN_RECV: {
                if (pc + 1 < b->code_count && !b->pc_to_block[pc + 1]) {
                    b->pc_to_block[pc + 1] = xir_func_add_block(b->func, NULL);
                }
                break;
            }
            default:
                break;
        }

        if (has_target && target < b->code_count && !b->pc_to_block[target]) {
            b->pc_to_block[target] = xir_func_add_block(b->func, NULL);
        }

        // Record backward jumps as loop back-edges
        if (has_target && target <= pc && b->nloops < BUILDER_MAX_LOOPS) {
            BuilderLoop *lp = &b->loops[b->nloops++];
            lp->header_pc = target;
            lp->back_edge_pc = pc;
        }
    }
}

// Find a loop by header PC
static BuilderLoop *builder_find_loop(XirBuilder *b, uint32_t header_pc) {
    for (int i = 0; i < b->nloops; i++) {
        if (b->loops[i].header_pc == header_pc)
            return &b->loops[i];
    }
    return NULL;
}

/* ========== Opcode Translation ========== */

// Translate a binary arithmetic op (ADD, SUB, MUL, etc.)
static void translate_binop(XirBuilder *b, XirBlock *blk,
                            XrInstruction inst, uint16_t xir_i64_op,
                            uint16_t xir_f64_op, uint16_t xir_rt_op) {
    int a = GETARG_A(inst);
    int ra_b = GETARG_B(inst);
    int ra_c = GETARG_C(inst);

    XirRef vb = builder_get_slot(b, blk, ra_b);
    XirRef vc = builder_get_slot(b, blk, ra_c);

    // Use actual vreg type (not slot declaration) so temporaries work correctly
    uint8_t type_b = ref_xir_type(b->func, vb);
    uint8_t type_c = ref_xir_type(b->func, vc);

    if (type_b == XR_REP_I64 && type_c == XR_REP_I64) {
        // Both raw i64: generate raw arithmetic
        XirRef result = xir_fold_emit(b->func, blk, xir_i64_op, XR_REP_I64, vb, vc);
        // DIV/MOD can trap on division by zero — mark as side effect
        // to prevent DCE from removing them even when result is unused
        if (xir_i64_op == XIR_DIV || xir_i64_op == XIR_MOD)
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT | XIR_FLAG_MAY_THROW;
        builder_set_slot(b, a, result);
        // Raw I64 arithmetic always produces I64. Ensure tag is set
        // even when inst_types is absent. Without this, RET epilogue
        // may skip tag write leaving stale tag.
        if (xir_ref_is_vreg(result)) {
            uint32_t vi = XIR_REF_INDEX(result);
            if (vi < b->func->nvreg &&
                xir_ref_ctype(b->func, result).kind == XIR_TK_UNKNOWN) {
                XirIns *d = b->func->vregs[vi].def;
                if (d && d->ctype.kind == XIR_TK_UNKNOWN) d->ctype.kind = XIR_TK_INT;
            }
        }
    } else if (type_b == XR_REP_F64 && type_c == XR_REP_F64) {
        // Both raw f64: generate raw float arithmetic
        XirRef result = xir_fold_emit(b->func, blk, xir_f64_op, XR_REP_F64, vb, vc);
        builder_set_slot(b, a, result);
        if (xir_ref_is_vreg(result)) {
            uint32_t vi = XIR_REF_INDEX(result);
            if (vi < b->func->nvreg &&
                xir_ref_ctype(b->func, result).kind == XIR_TK_UNKNOWN) {
                XirIns *d = b->func->vregs[vi].def;
                if (d && d->ctype.kind == XIR_TK_UNKNOWN) d->ctype.kind = XIR_TK_FLOAT;
            }
        }
    } else {
        // Mixed types: delegate to runtime helper
        bool b_numeric = (type_b == XR_REP_I64 || type_b == XR_REP_F64);
        bool c_numeric = (type_c == XR_REP_I64 || type_c == XR_REP_F64);
        if (b_numeric && c_numeric) {
            // Both numeric but different reps (I64+F64): codegen inlines conversion
            XirRef result = xir_emit(b->func, blk, xir_rt_op, XR_REP_F64, vb, vc);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT | XIR_FLAG_MAY_THROW;
            builder_tag_vreg(b, result, VTAG_F64, 0);
            builder_set_slot(b, a, result);
        } else if (!b_numeric && !c_numeric &&
                   type_b == XR_REP_TAGGED && type_c == XR_REP_TAGGED &&
                   builder_is_numeric_union(b, ra_b) &&
                   builder_is_numeric_union(b, ra_c)) {
            // Both operands are numeric-only unions (int|float): create I64+NUMERIC
            // vreg aliases so codegen generates inline tag-check + branch code
            // instead of a CALL_C function call.
            XirRef nb = xir_emit_unary(b->func, blk, XIR_MOV, XR_REP_I64, vb);
            builder_tag_vreg(b, nb, VTAG_NUMERIC, 0);
            XirRef nc = xir_emit_unary(b->func, blk, XIR_MOV, XR_REP_I64, vc);
            builder_tag_vreg(b, nc, VTAG_NUMERIC, 0);
            XirRef result = xir_emit(b->func, blk, xir_rt_op, XR_REP_F64, nb, nc);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT | XIR_FLAG_MAY_THROW;
            builder_tag_from_slot(b, result, a);
            builder_set_slot(b, a, result);
        } else {
            // At least one TAGGED/PTR operand: use CALL_C to avoid deopt.
            // Map XIR_RT_* opcode to corresponding CALL_C helper function.
            typedef XrJitResult (*JitRtFn)(struct XrCoroutine *, int64_t);
            JitRtFn fn = NULL;
            if (xir_rt_op == XIR_RT_ADD) fn = xr_jit_rt_add;
            else if (xir_rt_op == XIR_RT_SUB) fn = xr_jit_rt_sub;
            else if (xir_rt_op == XIR_RT_MUL) fn = xr_jit_rt_mul;
            else if (xir_rt_op == XIR_RT_DIV) fn = xr_jit_rt_div;
            else if (xir_rt_op == XIR_RT_MOD) fn = xr_jit_rt_mod;
            if (fn) {
                XirRef bo_args[2] = { vb, vc };
                // Encode operand tags for precise reconstruction
                uint8_t btag = vtag_to_value_tag(builder_slot_xr_tag(b, ra_b));
                uint8_t ctag = vtag_to_value_tag(builder_slot_xr_tag(b, ra_c));
                int64_t tag_enc = ((int64_t)btag << 8) | ctag;
                XirRef fn_ref = xir_const_ptr(b->func, (void *)fn);
                XirRef enc_ref = xir_const_i64(b->func, tag_enc);
                // AOT: tagged arithmetic returns XrtValue (struct), not raw i64
                uint8_t rt_rep = b->aot_mode ? XR_REP_TAGGED : XR_REP_I64;
                XirRef result = xir_emit(b->func, blk, XIR_CALL_C, rt_rep,
                                         fn_ref, enc_ref);
                blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
                builder_bind_call_args(b, result, bo_args, 2);
                // DIV/MOD may throw division-by-zero exception
                if (xir_rt_op == XIR_RT_DIV || xir_rt_op == XIR_RT_MOD)
                    blk->ins[blk->nins - 1].flags |= XIR_FLAG_MAY_THROW;
                builder_tag_from_slot(b, result, a);
                builder_set_slot(b, a, result);
            } else {
                // Fallback: emit XIR_RT_* (may deopt for non-numeric)
                XirRef result = xir_emit(b->func, blk, xir_rt_op, XR_REP_TAGGED, vb, vc);
                blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT | XIR_FLAG_MAY_THROW;
                builder_set_slot(b, a, result);
            }
        }
    }
    b->ops_translated++;
}

// Translate immediate binary op (ADDI, SUBI, MULI)
static void translate_binop_imm(XirBuilder *b, XirBlock *blk,
                                XrInstruction inst, uint16_t xir_i64_op,
                                uint16_t xir_rt_op) {
    int a = GETARG_A(inst);
    int ra_b = GETARG_B(inst);
    int8_t sc = GETARG_sC(inst);

    XirRef vb = builder_get_slot(b, blk, ra_b);
    XirRef vc = xir_const_i64(b->func, (int64_t)sc);

    uint8_t type_b = ref_xir_type(b->func, vb);
    if (type_b == XR_REP_I64) {
        // Emit const load + raw op
        XirRef vc_load = xir_emit_unary(b->func, blk, XIR_CONST_I64, XR_REP_I64, vc);
        XirRef result = xir_fold_emit(b->func, blk, xir_i64_op, XR_REP_I64, vb, vc_load);
        builder_tag_vreg(b, result, VTAG_I64, 0);
        builder_set_slot(b, a, result);
    } else {
        // Fall back to runtime helper
        XirRef vc_load = xir_emit_unary(b->func, blk, XIR_CONST_I64, XR_REP_I64, vc);
        XirRef result = xir_emit(b->func, blk, xir_rt_op, XR_REP_TAGGED, vb, vc_load);
        blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT | XIR_FLAG_MAY_THROW;
        builder_set_slot(b, a, result);
    }
    b->ops_translated++;
}

// Translate constant-pool binary op (ADDK, SUBK, MULK, DIVK, MODK)
// R[A] = R[B] op K[C] where K[C] is from the bytecode constant pool
static void translate_binop_const(XirBuilder *b, XirBlock *blk,
                                  XrInstruction inst, uint16_t xir_i64_op,
                                  uint16_t xir_f64_op, uint16_t xir_rt_op) {
    int a = GETARG_A(inst);
    int ra_b = GETARG_B(inst);
    int kc = GETARG_C(inst);

    // Safety check: bounds check for constant pool access
    if (kc >= PROTO_CONST_COUNT(b->proto)) {
        b->ops_skipped++;
        return;
    }

    XirRef vb = builder_get_slot(b, blk, ra_b);
    XrValue kval = PROTO_CONST_FAST(b->proto, kc);

    uint8_t type_b = ref_xir_type(b->func, vb);
    if (type_b == XR_REP_I64 && XR_IS_INT(kval)) {
        XirRef vc = xir_const_i64(b->func, XR_TO_INT(kval));
        XirRef vc_load = xir_emit_unary(b->func, blk, XIR_CONST_I64, XR_REP_I64, vc);
        XirRef result = xir_fold_emit(b->func, blk, xir_i64_op, XR_REP_I64, vb, vc_load);
        builder_tag_vreg(b, result, VTAG_I64, 0);
        builder_set_slot(b, a, result);
    } else if (type_b == XR_REP_F64 && XR_IS_FLOAT(kval)) {
        XirRef vc = xir_const_f64(b->func, XR_TO_FLOAT(kval));
        XirRef vc_load = xir_emit_unary(b->func, blk, XIR_CONST_F64, XR_REP_F64, vc);
        XirRef result = xir_fold_emit(b->func, blk, xir_f64_op, XR_REP_F64, vb, vc_load);
        builder_tag_vreg(b, result, VTAG_F64, 0);
        builder_set_slot(b, a, result);
    } else {
        // Mixed or tagged: emit runtime helper
        // Preserve constant type to avoid float truncation (e.g. 1.5 → 1)
        XirRef vc_load;
        bool kval_is_float = XR_IS_FLOAT(kval);
        if (kval_is_float) {
            XirRef vc = xir_const_f64(b->func, XR_TO_FLOAT(kval));
            vc_load = xir_emit_unary(b->func, blk, XIR_CONST_F64, XR_REP_F64, vc);
        } else {
            XirRef vc = xir_const_i64(b->func, XR_TO_INT(kval));
            vc_load = xir_emit_unary(b->func, blk, XIR_CONST_I64, XR_REP_I64, vc);
        }
        // When both operands are numeric (I64/F64), codegen inlines SCVTF+F-op
        // producing F64 in an FP register — mark result F64 for correct RETURN.
        bool kval_numeric = kval_is_float || XR_IS_INT(kval);
        uint8_t res_type = (kval_numeric &&
                            (type_b == XR_REP_I64 || type_b == XR_REP_F64))
                           ? XR_REP_F64 : XR_REP_TAGGED;
        XirRef result = xir_emit(b->func, blk, xir_rt_op, res_type, vb, vc_load);
        blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT | XIR_FLAG_MAY_THROW;
        if (res_type == XR_REP_F64)
            builder_tag_vreg(b, result, VTAG_F64, 0);
        builder_set_slot(b, a, result);
    }
    b->ops_translated++;
}

// Translate a single bytecode instruction
static bool translate_instruction(XirBuilder *b, XirBlock **cur_blk, uint32_t pc) {
    // Bounds check: verify PC is within code array
    if (pc >= (uint32_t)PROTO_CODE_COUNT(b->proto)) {
        xr_log_debug("jit", "PC %u out of bounds (max %d)",
                pc, PROTO_CODE_COUNT(b->proto));
        b->ops_skipped++;
        return true;
    }

    XrInstruction inst = PROTO_CODE(b->proto, pc);
    OpCode op = GET_OPCODE(inst);
    XirBlock *blk = *cur_blk;

    // Safety check: verify proto has valid symbol table
    if (b->proto->symbols == NULL && b->proto->symbol_count > 0) {
        xr_log_debug("jit", "invalid proto: symbols=NULL but symbol_count=%d at pc=%u",
                b->proto->symbol_count, pc);
        b->ops_skipped++;
        return true;
    }

    switch (op) {
        /* === Load/Move === */
        case OP_LOADI: {
            int a = GETARG_A(inst);
            int sbx = GETARG_sBx(inst);
            // Always raw i64 in JIT mode (no boxing support in codegen)
            XirRef c = xir_const_i64(b->func, (int64_t)sbx);
            XirRef v = xir_emit_unary(b->func, blk, XIR_CONST_I64, XR_REP_I64, c);
            builder_tag_vreg(b, v, VTAG_I64, 0);
            builder_set_slot(b, a, v);
            b->ops_translated++;
            return true;
        }

        case OP_LOADF: {
            int a = GETARG_A(inst);
            int sbx = GETARG_sBx(inst);
            XirRef c = xir_const_f64(b->func, (double)sbx);
            XirRef v = xir_emit_unary(b->func, blk, XIR_CONST_F64, XR_REP_F64, c);
            builder_tag_vreg(b, v, VTAG_F64, 0);
            builder_set_slot(b, a, v);
            b->ops_translated++;
            return true;
        }

        case OP_LOADK: {
            int a = GETARG_A(inst);
            int bx = GETARG_Bx(inst);

            // Bounds check for constant pool access
            if (bx >= PROTO_CONST_COUNT(b->proto)) {
                xr_log_debug("jit", "constant index %d out of bounds (max %d) in OP_LOADK",
                        bx, PROTO_CONST_COUNT(b->proto));
                b->ops_skipped++;
                return true;
            }

            XrValue kval = PROTO_CONST_FAST(b->proto, bx);
            XirRef v;
            if (XR_IS_INT(kval)) {
                XirRef c = xir_const_i64(b->func, XR_TO_INT(kval));
                v = xir_emit_unary(b->func, blk, XIR_CONST_I64, XR_REP_I64, c);
                builder_tag_vreg(b, v, VTAG_I64, 0);
            } else if (XR_IS_FLOAT(kval)) {
                XirRef c = xir_const_f64(b->func, XR_TO_FLOAT(kval));
                v = xir_emit_unary(b->func, blk, XIR_CONST_F64, XR_REP_F64, c);
                builder_tag_vreg(b, v, VTAG_F64, 0);
            } else if (XR_IS_STRING(kval)) {
                // String literal: store XrString* (GC object pointer, not data[])
                // so JIT codegen loads the correct heap address for field stores
                XrString *s = XR_TO_STRING(kval);
                XirRef c = xir_const_ptr(b->func, (void *)s);
                v = xir_emit_unary(b->func, blk, XIR_CONST_PTR, XR_REP_PTR, c);
                builder_tag_vreg(b, v, VTAG_PTR, 0);
            } else {
                // Other object/null: embed raw pointer as constant
                XirRef c = xir_const_ptr(b->func, (void *)kval.ptr);
                v = xir_emit_unary(b->func, blk, XIR_CONST_PTR, XR_REP_PTR, c);
                // VTAG_TAGGED for null (0 ptr); builder_ref_sf_tag detects null by const pool
                builder_tag_vreg(b, v, kval.ptr ? VTAG_PTR : VTAG_TAGGED, 0);
            }
            builder_set_slot(b, a, v);
            b->ops_translated++;
            return true;
        }

        case OP_LOADTRUE: {
            int a = GETARG_A(inst);
            XirRef c = xir_const_i64(b->func, 1);
            XirRef v = xir_emit_unary(b->func, blk, XIR_CONST_I64, XR_REP_I64, c);
            builder_tag_vreg(b, v, VTAG_BOOL, 0);
            builder_set_slot(b, a, v);
            b->ops_translated++;
            return true;
        }

        case OP_LOADFALSE: {
            int a = GETARG_A(inst);
            XirRef c = xir_const_i64(b->func, 0);
            XirRef v = xir_emit_unary(b->func, blk, XIR_CONST_I64, XR_REP_I64, c);
            builder_tag_vreg(b, v, VTAG_BOOL, 0);
            builder_set_slot(b, a, v);
            b->ops_translated++;
            return true;
        }

        case OP_LOADNULL: {
            int a = GETARG_A(inst);
            int b_count = GETARG_B(inst);
            for (int r = a; r <= a + b_count; r++) {
                XirRef c = xir_const_i64(b->func, 0);
                XirRef v = xir_emit_unary(b->func, blk, XIR_CONST_PTR, XR_REP_PTR, c);
                builder_tag_vreg(b, v, VTAG_NULL, 0);
                builder_set_slot(b, r, v);
            }
            b->ops_translated++;
            return true;
        }

        case OP_MOVE: {
            int a = GETARG_A(inst);
            int ra_b = GETARG_B(inst);
            XirRef src = builder_get_slot(b, blk, ra_b);
            builder_set_slot(b, a, src);  // SSA: just alias the ref
            // NOTE: Previously, we proactively updated bc_slot to the
            // destination slot here. This was removed because it could
            // corrupt PHI dst bc_slots (e.g. MOVE R[4] R[2] at loop exit
            // would change the loop-header PHI's bc_slot from 2 to 4,
            // causing OSR to load from values[4] instead of values[2]).
            // The original issue (stale bc_slot for PHI inputs) is now
            // handled by the OSR codegen's "skip PHI inputs" logic in
            // both loop live map and fallback paths.
            // Propagate slot_type so downstream ops (e.g. INVOKE_BUILTIN)
            // can derive correct type hints for the destination register
            if (a < 256 && ra_b < 256 && b->slot_rep[a] == XR_REP_TAGGED)
                b->slot_rep[a] = b->slot_rep[ra_b];
            if (a < 256 && ra_b < 256)
                b->slot_tag[a] = b->slot_tag[ra_b];

            // Propagate runtime tag tracking through MOVE.
            // This is needed for values whose precise tag is only known at
            // runtime (e.g. CALL_C result with tag in slot_runtime_tags).
            // builder_set_slot clears slot_tag_refs on writes, so restore it
            // from the source slot when MOVE is a pure alias.
            if (a < 256 && ra_b < 256 &&
                !xir_ref_is_none(b->slot_tag_refs[ra_b])) {
                b->slot_tag_refs[a] = b->slot_tag_refs[ra_b];
                b->slot_value_refs[a] = src;
            }
            // NOTE: struct_idx/array_etype/layout are in XirVReg fields;
            // SSA aliasing (slot_map[a] = same vreg as slot_map[ra_b]) propagates
            // them automatically without explicit copy.
            b->ops_translated++;
            return true;
        }

        /* === Arithmetic === */
        case OP_ADD: {
            // OP_ADD is polymorphic: numeric addition OR string concatenation.
            // When both operands are known numeric (I64/F64), use fast inline path.
            // Otherwise, delegate to xr_jit_rt_add which handles all cases
            // (int+int, float+float, mixed numeric, string concat) without deopt.
            int a_add = GETARG_A(inst);
            int rb_add = GETARG_B(inst);
            int rc_add = GETARG_C(inst);
            XirRef vb_add = builder_get_slot(b, blk, rb_add);
            XirRef vc_add = builder_get_slot(b, blk, rc_add);
            uint8_t tb = ref_xir_type(b->func, vb_add);
            uint8_t tc = ref_xir_type(b->func, vc_add);
            bool b_num = (tb == XR_REP_I64 || tb == XR_REP_F64);
            bool c_num = (tc == XR_REP_I64 || tc == XR_REP_F64);
            if (b_num && c_num) {
                translate_binop(b, blk, inst, XIR_ADD, XIR_FADD, XIR_RT_ADD);
            } else {
                // At least one operand is PTR/TAGGED: call runtime helper
                XirRef add_ca[2] = { vb_add, vc_add };
                // Encode operand tags (value_tag) for precise reconstruction
                uint8_t btag_a = vtag_to_value_tag(builder_slot_xr_tag(b, rb_add));
                uint8_t ctag_a = vtag_to_value_tag(builder_slot_xr_tag(b, rc_add));
                int64_t tag_enc_a = ((int64_t)btag_a << 8) | ctag_a;
                XirRef fn_ref = xir_const_ptr(b->func, (void *)xr_jit_rt_add);
                XirRef enc_ref = xir_const_i64(b->func, tag_enc_a);
                XirRef result = xir_emit(b->func, blk, XIR_CALL_C, XR_REP_PTR,
                                         fn_ref, enc_ref);
                blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
                builder_bind_call_args(b, result, add_ca, 2);
                // Result type is polymorphic: tag stored in slot_runtime_tags[bc_slot]
                builder_tag_vreg(b, result, VTAG_TAGGED, 0);
                builder_set_slot(b, a_add, result);
                b->ops_translated++;
            }
            return true;
        }

        case OP_SUB:
            translate_binop(b, blk, inst, XIR_SUB, XIR_FSUB, XIR_RT_SUB);
            return true;

        case OP_MUL:
            translate_binop(b, blk, inst, XIR_MUL, XIR_FMUL, XIR_RT_MUL);
            return true;

        case OP_DIV:
            translate_binop(b, blk, inst, XIR_DIV, XIR_FDIV, XIR_RT_DIV);
            return true;

        case OP_MOD:
            translate_binop(b, blk, inst, XIR_MOD, XIR_MOD, XIR_RT_MOD);
            return true;

        case OP_ADDI:
            translate_binop_imm(b, blk, inst, XIR_ADD, XIR_RT_ADD);
            return true;

        case OP_SUBI:
            translate_binop_imm(b, blk, inst, XIR_SUB, XIR_RT_SUB);
            return true;

        case OP_MULI:
            translate_binop_imm(b, blk, inst, XIR_MUL, XIR_RT_MUL);
            return true;

        case OP_ADDK:
        case OP_SUBK:
        case OP_MULK:
        case OP_DIVK:
        case OP_MODK: {
            int kc = GETARG_C(inst);
            if (kc >= PROTO_CONST_COUNT(b->proto)) {
                b->ops_skipped++;
                return true;
            }
            // Now safe to call translate_binop_const
            if (op == OP_ADDK) translate_binop_const(b, blk, inst, XIR_ADD, XIR_FADD, XIR_RT_ADD);
            else if (op == OP_SUBK) translate_binop_const(b, blk, inst, XIR_SUB, XIR_FSUB, XIR_RT_SUB);
            else if (op == OP_MULK) translate_binop_const(b, blk, inst, XIR_MUL, XIR_FMUL, XIR_RT_MUL);
            else if (op == OP_DIVK) translate_binop_const(b, blk, inst, XIR_DIV, XIR_FDIV, XIR_RT_DIV);
            else if (op == OP_MODK) translate_binop_const(b, blk, inst, XIR_MOD, XIR_MOD, XIR_RT_MOD);
            return true;
        }

        case OP_UNM: {
            int a = GETARG_A(inst);
            int ra_b = GETARG_B(inst);
            XirRef vb = builder_get_slot(b, blk, ra_b);
            uint8_t type = b->slot_rep[ra_b];
            if (type == XR_REP_I64) {
                XirRef r = xir_emit_unary(b->func, blk, XIR_NEG, XR_REP_I64, vb);
                builder_tag_vreg(b, r, VTAG_I64, 0);
                builder_set_slot(b, a, r);
            } else if (type == XR_REP_F64) {
                XirRef r = xir_emit_unary(b->func, blk, XIR_FNEG, XR_REP_F64, vb);
                builder_tag_vreg(b, r, VTAG_F64, 0);
                builder_set_slot(b, a, r);
            } else {
                XirRef r = xir_emit_unary(b->func, blk, XIR_RT_UNM, XR_REP_TAGGED, vb);
                blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
                builder_set_slot(b, a, r);
            }
            b->ops_translated++;
            return true;
        }

        case OP_NOT: {
            // R[A] = not R[B] (logical not: truthy → 0, falsy → 1)
            int a = GETARG_A(inst);
            int ra_b = GETARG_B(inst);
            XirRef vb = builder_get_slot(b, blk, ra_b);
            XirRef c0 = xir_const_i64(b->func, 0);
            XirRef zero = xir_emit_unary(b->func, blk, XIR_CONST_I64, XR_REP_I64, c0);
            // not x ≡ (x == 0)
            XirRef r = xir_fold_emit(b->func, blk, XIR_EQ, XR_REP_I64, vb, zero);
            builder_set_slot(b, a, r);
            builder_tag_vreg(b, r, VTAG_BOOL, 0);
            b->ops_translated++;
            return true;
        }

        /* === Comparison (expression form) === */
        case OP_CMP_LT: {
            int a = GETARG_A(inst);
            int ra_b = GETARG_B(inst);
            int ra_c = GETARG_C(inst);
            XirRef vb = builder_get_slot(b, blk, ra_b);
            XirRef vc = builder_get_slot(b, blk, ra_c);
            uint8_t type_b = b->slot_rep[ra_b];
            uint8_t type_c = b->slot_rep[ra_c];
            XirRef r;
            if (type_b == XR_REP_I64 && type_c == XR_REP_I64) {
                r = xir_fold_emit(b->func, blk, XIR_LT, XR_REP_I64, vb, vc);
            } else {
                r = xir_emit(b->func, blk, XIR_RT_LT, XR_REP_I64, vb, vc);
                blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            }
            builder_tag_vreg(b, r, VTAG_BOOL, 0);
            builder_set_slot(b, a, r);
            b->ops_translated++;
            return true;
        }

        case OP_CMP_LE: {
            int a = GETARG_A(inst);
            int ra_b = GETARG_B(inst);
            int ra_c = GETARG_C(inst);
            XirRef vb = builder_get_slot(b, blk, ra_b);
            XirRef vc = builder_get_slot(b, blk, ra_c);
            uint8_t type_b = b->slot_rep[ra_b];
            uint8_t type_c = b->slot_rep[ra_c];
            XirRef r;
            if (type_b == XR_REP_I64 && type_c == XR_REP_I64) {
                r = xir_fold_emit(b->func, blk, XIR_LE, XR_REP_I64, vb, vc);
            } else {
                r = xir_emit(b->func, blk, XIR_RT_LE, XR_REP_I64, vb, vc);
                blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            }
            builder_tag_vreg(b, r, VTAG_BOOL, 0);
            builder_set_slot(b, a, r);
            b->ops_translated++;
            return true;
        }

        case OP_CMP_EQ: {
            int a = GETARG_A(inst);
            int ra_b = GETARG_B(inst);
            int ra_c = GETARG_C(inst);
            XirRef vb = builder_get_slot(b, blk, ra_b);
            XirRef vc = builder_get_slot(b, blk, ra_c);
            XirRef r = xir_fold_emit(b->func, blk, XIR_EQ, XR_REP_I64, vb, vc);
            builder_tag_vreg(b, r, VTAG_BOOL, 0);
            builder_set_slot(b, a, r);
            b->ops_translated++;
            return true;
        }

        case OP_CMP_NE: {
            int a = GETARG_A(inst);
            int ra_b = GETARG_B(inst);
            int ra_c = GETARG_C(inst);
            XirRef vb = builder_get_slot(b, blk, ra_b);
            XirRef vc = builder_get_slot(b, blk, ra_c);
            XirRef r = xir_fold_emit(b->func, blk, XIR_NE, XR_REP_I64, vb, vc);
            builder_tag_vreg(b, r, VTAG_BOOL, 0);
            builder_set_slot(b, a, r);
            b->ops_translated++;
            return true;
        }

        /* === Box/Unbox === */
        // In JIT mode, BOX is always a no-op: values stay as raw types
        // in registers. The codegen has no boxing support (emits NOP).
        case OP_BOX_I64: {
            int a = GETARG_A(inst);
            int ra_b = GETARG_B(inst);
            XirRef vb = builder_get_slot(b, blk, ra_b);
            builder_set_slot(b, a, vb);
            b->ops_translated++;
            return true;
        }

        case OP_BOX_F64: {
            int a = GETARG_A(inst);
            int ra_b = GETARG_B(inst);
            XirRef vb = builder_get_slot(b, blk, ra_b);
            builder_set_slot(b, a, vb);
            b->ops_translated++;
            return true;
        }

        case OP_UNBOX_I64: {
            int a = GETARG_A(inst);
            int ra_b = GETARG_B(inst);
            XirRef vb = builder_get_slot(b, blk, ra_b);
            builder_set_slot(b, a, vb);
            b->ops_translated++;
            return true;
        }

        case OP_UNBOX_F64: {
            int a = GETARG_A(inst);
            int ra_b = GETARG_B(inst);
            XirRef vb = builder_get_slot(b, blk, ra_b);
            builder_set_slot(b, a, vb);
            b->ops_translated++;
            return true;
        }

        case OP_UPVAL_GET: {
            // Flat upvalue read: R[A] = cl->upvals[B]
            int a = GETARG_A(inst);
            int uv_idx = GETARG_B(inst);
            XirRef res;
            if (b->aot_mode) {
                // AOT: emit LOAD_UPVAL (handled by xcgen_expr.c)
                XirRef idx_ref = xir_const_i64(b->func, (int64_t)uv_idx);
                res = xir_emit_unary(b->func, blk, XIR_LOAD_UPVAL,
                                     XR_REP_TAGGED, idx_ref);
            } else {
                // JIT: use C bridge xr_jit_upval_get(coro, upval_index)
                XirRef fn_ref = xir_const_ptr(b->func,
                                               (void *)xr_jit_upval_get);
                XirRef idx_ref = xir_const_i64(b->func, (int64_t)uv_idx);
                XirRef idx_val = xir_emit_unary(b->func, blk, XIR_CONST_I64,
                                                 XR_REP_I64, idx_ref);
                res = xir_emit(b->func, blk, XIR_CALL_C, XR_REP_I64,
                               fn_ref, idx_val);
            }
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            // Tag result: mutable captures store cell pointers (always PTR),
            // const captures store the value directly (use slot_type).
            {
                bool is_cell_ref = true; // default: treat as cell ptr
                if (uv_idx < (int)PROTO_UPVAL_COUNT(b->proto)) {
                    UpvalInfo *uv = &((UpvalInfo*)b->proto->upvalues.data)[uv_idx];
                    if (uv->is_const) {
                        is_cell_ref = false;
                        uint8_t st = uv->slot_type;
                        if (st == XR_SLOT_I64)
                            builder_tag_vreg(b, res, VTAG_I64, 0);
                        else if (st == XR_SLOT_F64)
                            builder_tag_vreg(b, res, VTAG_F64, 0);
                        else if (st == XR_SLOT_BOOL)
                            builder_tag_vreg(b, res, VTAG_BOOL, 0);
                        else
                            builder_tag_vreg(b, res, VTAG_PTR, 0);
                    }
                }
                if (is_cell_ref)
                    builder_tag_vreg(b, res, VTAG_PTR, 0);
            }
            builder_set_slot(b, a, res);
            b->ops_translated++;
            return true;
        }

        case OP_CELL_NEW: {
            // R[A] = new_cell(R[A])
            // Inline allocation: XIR_ALLOC(XR_TCELL, 32) + XIR_STORE_FIELD
            int a = GETARG_A(inst);
            XirRef va = builder_get_slot(b, blk, a);

            // Determine concrete tag for initial value.
            // Cannot use XIR_SF_TAG_RUNTIME: LOADNULL has rep=PTR but value=0,
            // and STORE_FIELD's PTR path tries to read gc_type from the pointer.
            uint8_t init_tag;
            {
                XirVReg *_vr = builder_vreg_ref(b, va);
                uint8_t _vk = type_kind_to_vtag(xir_ref_ctype(b->func, va).kind);
                if (_vr && vtag_is_concrete(_vk)) {
                    init_tag = vtag_to_value_tag(_vk);
                } else {
                    uint8_t rep = _vr ? _vr->rep : XR_REP_I64;
                    if (rep == XR_REP_F64)      init_tag = XR_TAG_F64;
                    else if (rep == XR_REP_PTR) init_tag = XR_TAG_NULL;
                    else                        init_tag = XR_TAG_I64;
                }
            }

            // Allocate 32-byte XrCell via inline bump-pointer
            int64_t packed = (int64_t)XR_TCELL;
            XirRef pack_ref = xir_const_i64(b->func, packed);
            XirRef size_ref = xir_const_i64(b->func, 32); // sizeof(XrCell)
            XirRef cell = xir_emit(b->func, blk, XIR_ALLOC, XR_REP_PTR,
                                   pack_ref, size_ref);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;

            // Store initial value into cell->value (offset XIR_CELL_VALUE_OFFSET)
            XirRef val_off = xir_const_i64(b->func, (int64_t)XIR_CELL_VALUE_OFFSET);
            xir_emit_raw(b->func, blk, XIR_STORE_FIELD,
                         init_tag, val_off, cell, va);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;

            builder_tag_vreg(b, cell, VTAG_PTR, 0);
            { XirVReg *_fv = builder_vreg_for_slot(b, a); if (_fv) _fv->is_fresh_alloc = true; }
            builder_set_slot(b, a, cell);
            b->ops_translated++;
            return true;
        }

        case OP_CELL_GET: {
            // R[A] = cell->value  (inline field load)
            int a = GETARG_A(inst);
            int ra_b = GETARG_B(inst);
            XirRef vb = builder_get_slot(b, blk, ra_b);
            XirRef off_ref = xir_const_i64(b->func, (int64_t)XIR_CELL_VALUE_OFFSET);
            // Determine load rep: float cells need FP register.
            // Try inst_type first, then fall back to upvalue type_info/slot_type
            // from the preceding UPVAL_GET instruction.
            uint8_t load_rep = XR_REP_I64;
            struct XrType *_ct = builder_inst_xrtype(b, b->cur_pc);
            if (_ct && xr_type_to_xr_tag(_ct) == XR_TAG_F64) {
                load_rep = XR_REP_F64;
            } else if (!_ct && b->cur_pc > 0) {
                // No inst_type: check preceding UPVAL_GET for upval type info
                XrInstruction prev = PROTO_CODE(b->proto, b->cur_pc - 1);
                if (GET_OPCODE(prev) == OP_UPVAL_GET) {
                    int uv_idx = GETARG_B(prev);
                    if (uv_idx < (int)PROTO_UPVAL_COUNT(b->proto)) {
                        UpvalInfo *uv = &((UpvalInfo*)b->proto->upvalues.data)[uv_idx];
                        if (uv->type_info && xr_type_to_xr_tag(uv->type_info) == XR_TAG_F64)
                            load_rep = XR_REP_F64;
                        else if (uv->slot_type == XR_SLOT_F64)
                            load_rep = XR_REP_F64;
                    }
                }
            }
            XirRef res = xir_emit(b->func, blk, XIR_LOAD_FIELD, load_rep,
                                  vb, off_ref);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            builder_tag_vreg(b, res, VTAG_TAGGED, 0);
            builder_set_slot(b, a, res);
            builder_refine_slot_from_inst_type(b, a);
            b->ops_translated++;
            return true;
        }

        case OP_CELL_SET: {
            // cell->value = R[B]  (inline field store)
            int a = GETARG_A(inst);
            int ra_b = GETARG_B(inst);
            XirRef va = builder_get_slot(b, blk, a);
            XirRef vb = builder_get_slot(b, blk, ra_b);
            XirRef val_off = xir_const_i64(b->func, (int64_t)XIR_CELL_VALUE_OFFSET);
            xir_emit_raw(b->func, blk, XIR_STORE_FIELD,
                         XIR_SF_TAG_RUNTIME, val_off, va, vb);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            b->ops_translated++;
            return true;
        }

        /* === Closure === */
        case OP_CLOSURE: {
            // R[A] = closure(PROTO[Bx])
            int a = GETARG_A(inst);
            uint16_t bx = GETARG_Bx(inst);

            if (b->aot_mode && bx < PROTO_PROTO_COUNT(b->proto)) {
                // AOT: generate closure creation XIR
                XrProto *child = PROTO_PROTO(b->proto, bx);
                int nupvals = (int)PROTO_UPVAL_COUNT(child);

                // xrt_closure_new(child_proto_ptr, nupvals) → XrtValue closure
                XirRef proto_ref = xir_const_ptr(b->func, (void *)child);
                XirRef nupvals_ref = xir_const_i64(b->func, (int64_t)nupvals);
                XirRef nupvals_val = xir_emit_unary(b->func, blk, XIR_CONST_I64,
                                                     XR_REP_I64, nupvals_ref);
                XirRef closure = xir_emit(b->func, blk, XIR_CALL_C, XR_REP_TAGGED,
                                          proto_ref, nupvals_val);
                blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
                builder_tag_vreg(b, closure, VTAG_PTR, 0);

                // Populate upvals from current frame registers / enclosing closure
                for (int j = 0; j < nupvals; j++) {
                    UpvalInfo *uv = &((UpvalInfo*)child->upvalues.data)[j];
                    XirRef idx_ref = xir_const_i64(b->func, (int64_t)j);
                    XirRef src_val;
                    if (uv->source == UPVAL_SRC_REG) {
                        src_val = builder_get_slot(b, blk, uv->index);
                    } else if (uv->source == UPVAL_SRC_UPVAL) {
                        // Read from enclosing closure's upval
                        XirRef enc_idx = xir_const_i64(b->func,
                                                        (int64_t)uv->index);
                        src_val = xir_emit_unary(b->func, blk,
                                                  XIR_LOAD_UPVAL,
                                                  XR_REP_TAGGED, enc_idx);
                        blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
                    } else {
                        continue;
                    }
                    // Convention: dst=idx(const), args[0]=closure, args[1]=value
                    // Avoids putting vreg in dst (which SSA defuse would
                    // interpret as a re-definition of the closure).
                    xir_emit_raw(b->func, blk, XIR_STORE_UPVAL,
                                 XR_REP_VOID, idx_ref, closure, src_val);
                    blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
                }

                builder_set_slot(b, a, closure);
                // Set callee_proto on the NEW closure vreg (after builder_set_slot)
                { XirVReg *_cv = builder_vreg_for_slot(b, a);
                  if (_cv) _cv->callee_proto = PROTO_PROTO(b->proto, bx); }
                b->ops_translated++;
            } else if (bx < PROTO_PROTO_COUNT(b->proto)) {
                // JIT: delegate to xr_jit_closure_new C bridge
                // closure_new auto-populates UPVAL_SRC_UPVAL from enclosing closure.
                XrProto *child = PROTO_PROTO(b->proto, bx);
                int nupvals = (int)PROTO_UPVAL_COUNT(child);

                // Call xr_jit_closure_new(coro, child_proto_ptr)
                XirRef fn_ref = xir_const_ptr(b->func, (void *)xr_jit_closure_new);
                XirRef proto_ref = xir_const_i64(b->func, (int64_t)(uintptr_t)child);
                XirRef proto_val = xir_emit_unary(b->func, blk, XIR_CONST_I64,
                                                   XR_REP_I64, proto_ref);
                XirRef closure = xir_emit(b->func, blk, XIR_CALL_C, XR_REP_PTR,
                                          fn_ref, proto_val);
                blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
                builder_tag_vreg(b, closure, VTAG_PTR, 0);

                // Populate UPVAL_SRC_REG entries from current frame's registers
                for (int j = 0; j < nupvals; j++) {
                    UpvalInfo *uv = &((UpvalInfo*)child->upvalues.data)[j];
                    if (uv->source != UPVAL_SRC_REG) continue;
                    int src_reg = uv->index;
                    XirRef src_val = builder_get_slot(b, blk, src_reg);
                    uint8_t tag = vtag_to_value_tag(builder_slot_xr_tag(b, src_reg));
                    XirRef uv_args[2] = { closure, src_val };
                    // Call set_upval(coro, (idx << 8) | tag)
                    int64_t encoded = ((int64_t)j << 8) | (int64_t)tag;
                    XirRef set_fn = xir_const_ptr(b->func,
                                        (void *)xr_jit_closure_set_upval);
                    XirRef enc_ref = xir_const_i64(b->func, encoded);
                    XirRef set_res = xir_emit(b->func, blk, XIR_CALL_C, XR_REP_I64,
                                              set_fn, enc_ref);
                    blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
                    builder_bind_call_args(b, set_res, uv_args, 2);
                }

                builder_set_slot(b, a, closure);
                // Set callee_proto on the NEW closure vreg (after builder_set_slot)
                { XirVReg *_cv = builder_vreg_for_slot(b, a);
                  if (_cv) _cv->callee_proto = PROTO_PROTO(b->proto, bx); }
                b->ops_translated++;
            } else {
                b->ops_skipped++;
            }
            return true;
        }


        /* === Control Flow === */
        case OP_JMP: {
            int sJ = GETARG_sJ(inst);
            uint32_t target = (uint32_t)((int32_t)pc + 1 + sJ);
            XirBlock *target_blk = b->pc_to_block[target];
            if (target_blk) {
                bool is_back_edge = (target <= pc);
                if (is_back_edge) {
                    XirRef none = XIR_NONE;
                    xir_emit(b->func, blk, XIR_SAFEPOINT, XR_REP_VOID, none, none);
                }
                xir_block_set_jmp(blk, target_blk);
                if (!is_back_edge) {
                    xir_block_add_pred(target_blk, blk, b->func->arena);
                }
            }
            b->ops_translated++;
            return true;
        }

        /* === Comparison + JMP pairs === */
        // OP_LT/LE/EQ A B k + OP_JMP sJ:
        //   if (R[A] op R[B]) != k then skip JMP
        //   k=0: true→fall_through, false→jump
        //   k=1: true→jump, false→fall_through
        case OP_LT:
        case OP_LE:
        case OP_EQ: {
            int ra_a = GETARG_A(inst);
            int ra_b = GETARG_B(inst);
            int k = GETARG_C(inst);

            XirRef va = builder_get_slot(b, blk, ra_a);
            XirRef vb = builder_get_slot(b, blk, ra_b);

            uint8_t ta = b->slot_rep[ra_a];
            uint8_t tb = b->slot_rep[ra_b];
            uint8_t tag_a = builder_slot_xr_tag(b, ra_a);
            uint8_t tag_b = builder_slot_xr_tag(b, ra_b);

            uint16_t xir_op;
            OpCode op = GET_OPCODE(inst);
            if (op == OP_LT) xir_op = XIR_LT;
            else if (op == OP_LE) xir_op = XIR_LE;
            else xir_op = XIR_EQ;

            // Classify semantic tag into type "kind" for cross-kind EQ detection.
            // 0=unknown, 1=numeric(int/float), 2=bool, 3=null/ptr
#define TAG_KIND(t) \
    (((t) == VTAG_I64 || (t) == VTAG_F64 || (t) == VTAG_NUMERIC) ? 1 : \
     ((t) == VTAG_BOOL) ? 2 : \
     ((t) == VTAG_PTR) ? 3 : 0)

            XirRef cond;
            // Cross-kind EQ: constant-fold to false when both tags are
            // known and belong to different type kinds.
            // Must come BEFORE I64 fast path because bool and int share
            // I64 machine rep but are different semantic kinds in xray.
            // Exception: never fold when either side is null — any typed
            // variable can be null at runtime (default params, nullable
            // returns), so `x == null` must be evaluated at runtime.
            int kind_a = TAG_KIND(tag_a);
            int kind_b = TAG_KIND(tag_b);
            if (op == OP_EQ && kind_a != 0 && kind_b != 0 && kind_a != kind_b
                && tag_a != VTAG_TAGGED && tag_b != VTAG_TAGGED) {
                XirRef zero = xir_const_i64(b->func, 0);
                cond = xir_emit_unary(b->func, blk, XIR_CONST_I64,
                                      XR_REP_I64, zero);
            } else if (op == OP_EQ && tag_b == VTAG_TAGGED &&
                       ra_a < 256 &&
                       !xir_ref_is_none(b->slot_tag_refs[ra_a])) {
                // EQ(param_with_tag_ref, null): use runtime tag comparison
                // to distinguish int(0) from null (default param check).
                // No vreg match needed: builder_set_slot clears tag_refs on
                // real overwrites; PHI merges don't invalidate the tag ref.
                XirRef c_null = xir_const_i64(b->func, 0);
                XirRef null_tag = xir_emit_unary(b->func, blk, XIR_CONST_I64,
                                                  XR_REP_I64, c_null);
                cond = xir_emit(b->func, blk, XIR_EQ, XR_REP_I64,
                                b->slot_tag_refs[ra_a], null_tag);
            } else if (op == OP_EQ && tag_a == VTAG_TAGGED &&
                       ra_b < 256 &&
                       !xir_ref_is_none(b->slot_tag_refs[ra_b])) {
                // EQ(null, param_with_tag_ref): symmetric case
                XirRef c_null = xir_const_i64(b->func, 0);
                XirRef null_tag = xir_emit_unary(b->func, blk, XIR_CONST_I64,
                                                  XR_REP_I64, c_null);
                cond = xir_emit(b->func, blk, XIR_EQ, XR_REP_I64,
                                b->slot_tag_refs[ra_b], null_tag);
            } else if (ta == XR_REP_I64 && tb == XR_REP_I64) {
                cond = xir_fold_emit(b->func, blk, xir_op, XR_REP_I64, va, vb);
            } else if (op == OP_EQ && ta == XR_REP_PTR && tb == XR_REP_PTR) {
                // PTR vs PTR: inline pointer comparison
                cond = xir_fold_emit(b->func, blk, XIR_EQ, XR_REP_I64, va, vb);
            } else if (op == OP_EQ && !((ta == XR_REP_I64 || ta == XR_REP_F64) &&
                                        (tb == XR_REP_I64 || tb == XR_REP_F64))) {
                // Non-numeric EQ (PTR vs PTR, TAGGED, etc.): use C bridge.
                eq_c_bridge:
                {
                XirRef eq_args[2] = { va, vb };
                XirRef fn_ref = xir_const_ptr(b->func, (void *)xr_jit_rt_eq);
                uint8_t a_tag = vtag_to_value_tag(builder_slot_xr_tag(b, ra_a));
                uint8_t b_tag = vtag_to_value_tag(builder_slot_xr_tag(b, ra_b));
                int64_t eq_enc = ((int64_t)a_tag << 8) | (int64_t)b_tag;
                XirRef enc_ref = xir_const_i64(b->func, eq_enc);
                cond = xir_emit(b->func, blk, XIR_CALL_C, XR_REP_I64,
                                fn_ref, enc_ref);
                blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
                builder_bind_call_args(b, cond, eq_args, 2);
                }
            } else {
                // Numeric mixed (I64/F64) or LT/LE: use XIR_RT_* (codegen inlines)
                uint16_t rt_op;
                if (op == OP_LT) rt_op = XIR_RT_LT;
                else if (op == OP_LE) rt_op = XIR_RT_LE;
                else rt_op = XIR_RT_EQ;
                cond = xir_emit(b->func, blk, rt_op, XR_REP_I64, va, vb);
                blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            }

            // Peek at next instruction for JMP
            if (pc + 1 < b->code_count) {
                XrInstruction next_inst = PROTO_CODE(b->proto, pc + 1);
                if (GET_OPCODE(next_inst) == OP_JMP) {
                    int sJ = GETARG_sJ(next_inst);
                    uint32_t jump_target = (uint32_t)((int32_t)(pc + 1) + 1 + sJ);
                    uint32_t fall_target = pc + 2;

                    XirBlock *jump_blk = b->pc_to_block[jump_target];
                    XirBlock *fall_blk = b->pc_to_block[fall_target];

                    if (jump_blk && fall_blk) {
                        if (k == 0) {
                            // true → fall through (skip JMP), false → jump
                            xir_block_set_br(blk, cond, fall_blk, jump_blk);
                        } else {
                            // true → jump, false → fall through
                            xir_block_set_br(blk, cond, jump_blk, fall_blk);
                        }
                        xir_block_add_pred(jump_blk, blk, b->func->arena);
                        xir_block_add_pred(fall_blk, blk, b->func->arena);
                    }
                }
            }
            b->ops_translated++;
            return true;
        }

        case OP_LTI:
        case OP_LEI:
        case OP_EQI: {
            int ra_a = GETARG_A(inst);
            int sb = GETARG_sB(inst);
            int k = GETARG_C(inst);

            XirRef va = builder_get_slot(b, blk, ra_a);
            uint8_t ta = (ra_a < 256) ? b->slot_rep[ra_a] : ref_xir_type(b->func, va);

            uint16_t xir_op;
            OpCode op = GET_OPCODE(inst);
            if (op == OP_LTI) xir_op = XIR_LT;
            else if (op == OP_LEI) xir_op = XIR_LE;
            else xir_op = XIR_EQ;

            uint8_t eqi_tag = builder_slot_xr_tag(b, ra_a);
            int eqi_kind = TAG_KIND(eqi_tag);

            XirRef cond;
            // Cross-kind EQI: bool/null/ptr vs integer → always false
            if (op == OP_EQI && eqi_kind != 0 && eqi_kind != 1) {
                XirRef zero = xir_const_i64(b->func, 0);
                cond = xir_emit_unary(b->func, blk, XIR_CONST_I64, XR_REP_I64, zero);
            } else if (ta == XR_REP_I64) {
                XirRef ci = xir_const_i64(b->func, (int64_t)sb);
                XirRef vi = xir_emit_unary(b->func, blk, XIR_CONST_I64, XR_REP_I64, ci);
                cond = xir_fold_emit(b->func, blk, xir_op, XR_REP_I64, va, vi);
            } else if (op == OP_EQI && ta != XR_REP_F64) {
                // TAGGED == integer: use C bridge
                XirRef ci = xir_const_i64(b->func, (int64_t)sb);
                XirRef vi = xir_emit_unary(b->func, blk, XIR_CONST_I64, XR_REP_I64, ci);
                XirRef eqi_args[2] = { va, vi };
                XirRef fn_ref = xir_const_ptr(b->func, (void *)xr_jit_rt_eq);
                uint8_t a_tag = vtag_to_value_tag(builder_slot_xr_tag(b, ra_a));
                uint8_t b_tag = XR_TAG_I64;
                int64_t eq_enc = ((int64_t)a_tag << 8) | (int64_t)b_tag;
                XirRef enc_ref = xir_const_i64(b->func, eq_enc);
                cond = xir_emit(b->func, blk, XIR_CALL_C, XR_REP_I64,
                                fn_ref, enc_ref);
                blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
                builder_bind_call_args(b, cond, eqi_args, 2);
            } else {
                XirRef ci = xir_const_i64(b->func, (int64_t)sb);
                XirRef vi = xir_emit_unary(b->func, blk, XIR_CONST_I64, XR_REP_I64, ci);
                uint16_t rt_op;
                if (op == OP_LTI) rt_op = XIR_RT_LT;
                else if (op == OP_LEI) rt_op = XIR_RT_LE;
                else rt_op = XIR_RT_EQ;
                cond = xir_emit(b->func, blk, rt_op, XR_REP_I64, va, vi);
                blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            }

            if (pc + 1 < b->code_count) {
                XrInstruction next_inst = PROTO_CODE(b->proto, pc + 1);
                if (GET_OPCODE(next_inst) == OP_JMP) {
                    int sJ = GETARG_sJ(next_inst);
                    uint32_t jump_target = (uint32_t)((int32_t)(pc + 1) + 1 + sJ);
                    uint32_t fall_target = pc + 2;

                    XirBlock *jump_blk = b->pc_to_block[jump_target];
                    XirBlock *fall_blk = b->pc_to_block[fall_target];

                    if (jump_blk && fall_blk) {
                        if (k == 0) {
                            xir_block_set_br(blk, cond, fall_blk, jump_blk);
                        } else {
                            xir_block_set_br(blk, cond, jump_blk, fall_blk);
                        }
                        xir_block_add_pred(jump_blk, blk, b->func->arena);
                        xir_block_add_pred(fall_blk, blk, b->func->arena);
                    }
                }
            }
            b->ops_translated++;
            return true;
        }

        case OP_EQK: {
            // EQK A B k: if (R[A] == K[B]) != k then skip next (JMP)
            int ra_a = GETARG_A(inst);
            int kb = GETARG_B(inst);
            int k = GETARG_C(inst);

            // Bounds check for constant pool
            if (kb >= PROTO_CONST_COUNT(b->proto)) {
                b->ops_skipped++;
                return true;
            }

            XirRef va = builder_get_slot(b, blk, ra_a);
            XrValue kval = PROTO_CONST_FAST(b->proto, kb);

            // Load constant and compare
            XirRef vb;
            if (XR_IS_INT(kval)) {
                XirRef c = xir_const_i64(b->func, XR_TO_INT(kval));
                vb = xir_emit_unary(b->func, blk, XIR_CONST_I64, XR_REP_I64, c);
            } else if (XR_IS_FLOAT(kval)) {
                XirRef c = xir_const_f64(b->func, XR_TO_FLOAT(kval));
                vb = xir_emit_unary(b->func, blk, XIR_CONST_F64, XR_REP_F64, c);
            } else {
                // Non-numeric constant (string etc.) — use C bridge for comparison
                XirRef kraw = xir_const_i64(b->func, kval.i);
                XirRef kraw_val = xir_emit_unary(b->func, blk, XIR_CONST_I64,
                                                  XR_REP_I64, kraw);
                XirRef eqk_args[2] = { va, kraw_val };
                XirRef fn_ref = xir_const_ptr(b->func, (void *)xr_jit_eq_value);
                // Encode: (a_value_tag << 8) | kval.tag
                uint8_t a_tag = vtag_to_value_tag(builder_slot_xr_tag(b, ra_a));
                int64_t eq_enc = ((int64_t)a_tag << 8) | (int64_t)(kval.tag & 0xFF);
                XirRef enc_ref = xir_const_i64(b->func, eq_enc);
                vb = xir_emit(b->func, blk, XIR_CALL_C, XR_REP_I64,
                              fn_ref, enc_ref);
                blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
                builder_bind_call_args(b, vb, eqk_args, 2);
                // vb is 1 if equal, 0 if not — use directly as condition
                // For EQK: skip if (equal != k), so condition = (vb == 1)
                // We need vb as the cond value, and handle k below
            }

            XirRef cond;
            if (XR_IS_INT(kval) || XR_IS_FLOAT(kval)) {
                cond = xir_emit(b->func, blk, XIR_EQ, XR_REP_I64, va, vb);
            } else {
                // vb already holds 1/0 from xr_jit_eq_value
                cond = vb;
            }

            if (pc + 1 < b->code_count) {
                XrInstruction next_inst = PROTO_CODE(b->proto, pc + 1);
                if (GET_OPCODE(next_inst) == OP_JMP) {
                    int sJ = GETARG_sJ(next_inst);
                    uint32_t jump_target = (uint32_t)((int32_t)(pc + 1) + 1 + sJ);
                    uint32_t fall_target = pc + 2;
                    XirBlock *jump_blk = b->pc_to_block[jump_target];
                    XirBlock *fall_blk = b->pc_to_block[fall_target];
                    if (jump_blk && fall_blk) {
                        if (k == 0) {
                            xir_block_set_br(blk, cond, fall_blk, jump_blk);
                        } else {
                            xir_block_set_br(blk, cond, jump_blk, fall_blk);
                        }
                        xir_block_add_pred(jump_blk, blk, b->func->arena);
                        xir_block_add_pred(fall_blk, blk, b->func->arena);
                    }
                }
            }
            b->ops_translated++;
            return true;
        }

        case OP_TESTSET: {
            // TESTSET A B k: if (R[B]) != k then skip JMP, else R[A] = bool(R[B])
            // Logical && / || always return bool for type consistency.
            int a = GETARG_A(inst);
            int rb = GETARG_B(inst);
            int k = GETARG_C(inst);
            XirRef vb = builder_get_slot(b, blk, rb);
            // Convert to bool: (vb != 0) → 0 or 1
            XirRef c0 = xir_const_i64(b->func, 0);
            XirRef zero = xir_emit_unary(b->func, blk, XIR_CONST_I64, XR_REP_I64, c0);
            XirRef bool_r = xir_fold_emit(b->func, blk, XIR_NE, XR_REP_I64, vb, zero);
            builder_set_slot(b, a, bool_r);
            builder_tag_vreg(b, bool_r, VTAG_BOOL, 0);

            if (pc + 1 < b->code_count) {
                XrInstruction next_inst = PROTO_CODE(b->proto, pc + 1);
                if (GET_OPCODE(next_inst) == OP_JMP) {
                    int sJ = GETARG_sJ(next_inst);
                    uint32_t jump_target = (uint32_t)((int32_t)(pc + 1) + 1 + sJ);
                    uint32_t fall_target = pc + 2;
                    XirBlock *jump_blk = b->pc_to_block[jump_target];
                    XirBlock *fall_blk = b->pc_to_block[fall_target];
                    if (jump_blk && fall_blk) {
                        // TESTSET A B k: "if R[B] != k, skip next (JMP)"
                        // k=0: truthy → skip JMP → fall_blk; falsy → jump_blk
                        // k=1: falsy  → skip JMP → fall_blk; truthy → jump_blk
                        if (k) {
                            xir_block_set_br(blk, vb, jump_blk, fall_blk);
                        } else {
                            xir_block_set_br(blk, vb, fall_blk, jump_blk);
                        }
                        xir_block_add_pred(jump_blk, blk, b->func->arena);
                        xir_block_add_pred(fall_blk, blk, b->func->arena);
                    }
                }
            }
            b->ops_translated++;
            return true;
        }

        case OP_ISNULL: {
            // ISNULL A k: if (R[A] == null) != k then skip next instruction
            int a = GETARG_A(inst);
            int k = GETARG_B(inst);

            // Nullable check elimination: if slot_xrtype[a] is non-nullable,
            // the null check can be folded away at compile time.
            // Exception 1: never fold for function parameters — callers may
            // violate type contracts (e.g. Json field access returns any/null
            // passed to a non-nullable parameter via call.self.direct).
            // Exception 2: never fold when the current vreg has TAGGED/I64 rep
            // from a CALL/INVOKE result — static type info is flow-insensitive
            // and may reflect a different use of this register, not the current
            // INVOKE result which CAN be null at runtime.
            bool known_nonnull = false;
            bool is_param = (a < b->proto->numparams);
            if (!is_param && a < 256 && builder_slot_xrtype(b, a)) {
                struct XrType *t = builder_slot_xrtype(b, a);
                if (!XR_TYPE_IS_NULLABLE(t)) {
                    // Verify the current vreg actually has PTR representation.
                    // If vreg rep is I64/TAGGED (e.g. from CALL_C/CALL_DIRECT),
                    // the value may be null despite static types saying non-nullable.
                    XirVReg *vr = builder_vreg_for_slot(b, a);
                    if (vr && vr->rep == XR_REP_PTR) {
                        known_nonnull = true;
                    }
                }
            }

            if (known_nonnull && pc + 1 < b->code_count) {
                XrInstruction next_inst = PROTO_CODE(b->proto, pc + 1);
                if (GET_OPCODE(next_inst) == OP_JMP) {
                    int sJ = GETARG_sJ(next_inst);
                    uint32_t jump_target = (uint32_t)((int32_t)(pc + 1) + 1 + sJ);
                    uint32_t fall_target = pc + 2;
                    XirBlock *jump_blk = b->pc_to_block[jump_target];
                    XirBlock *fall_blk = b->pc_to_block[fall_target];
                    if (jump_blk && fall_blk) {
                        // is_null is always false for non-nullable types.
                        // k=0: (false!=0) is false → execute JMP → jump_blk
                        // k=1: (false!=1) → skip JMP → fall_blk
                        XirBlock *target = k ? fall_blk : jump_blk;
                        xir_block_set_jmp(blk, target);
                        xir_block_add_pred(target, blk, b->func->arena);
                    }
                    b->ops_translated++;
                    return true;
                }
            }

            // General case: emit runtime null check
            XirRef va = builder_get_slot(b, blk, a);
            XirRef is_null;

            // For nullable primitive params (int?/float?/bool?),
            // use tag comparison instead of payload==0 to correctly
            // distinguish int(0) from null.
            if (a < 256 &&
                !xir_ref_is_none(b->slot_tag_refs[a]) &&
                !xir_ref_is_none(b->slot_value_refs[a]) &&
                XIR_REF_INDEX(va) == XIR_REF_INDEX(b->slot_value_refs[a])) {
                XirRef c_null = xir_const_i64(b->func, 0); // XR_TAG_NULL == 0
                XirRef null_tag = xir_emit_unary(b->func, blk, XIR_CONST_I64, XR_REP_I64, c_null);
                is_null = xir_emit(b->func, blk, XIR_EQ, XR_REP_I64,
                                   b->slot_tag_refs[a], null_tag);
            } else {
                XirRef c0 = xir_const_i64(b->func, 0);
                XirRef zero = xir_emit_unary(b->func, blk, XIR_CONST_I64, XR_REP_I64, c0);
                is_null = xir_emit(b->func, blk, XIR_EQ, XR_REP_I64, va, zero);
            }

            if (pc + 1 < b->code_count) {
                XrInstruction next_inst = PROTO_CODE(b->proto, pc + 1);
                if (GET_OPCODE(next_inst) == OP_JMP) {
                    int sJ = GETARG_sJ(next_inst);
                    uint32_t jump_target = (uint32_t)((int32_t)(pc + 1) + 1 + sJ);
                    uint32_t fall_target = pc + 2;
                    XirBlock *jump_blk = b->pc_to_block[jump_target];
                    XirBlock *fall_blk = b->pc_to_block[fall_target];
                    if (jump_blk && fall_blk) {
                        if (k) {
                            xir_block_set_br(blk, is_null, jump_blk, fall_blk);
                        } else {
                            xir_block_set_br(blk, is_null, fall_blk, jump_blk);
                        }
                        xir_block_add_pred(jump_blk, blk, b->func->arena);
                        xir_block_add_pred(fall_blk, blk, b->func->arena);

                        // Nullable narrowing: if slot is nullable with
                        // non-PTR base (int?/float?/bool?), record pending
                        // narrowing for the non-null branch.
                        // k=0: non-null branch = jump_blk
                        // k=1: non-null branch = fall_blk
                        if (a < 256 && builder_slot_xrtype(b, a)) {
                            struct XrType *t = builder_slot_xrtype(b, a);
                            if (XR_TYPE_IS_NULLABLE(t)) {
                                XrRep base = xr_type_base_rep(t);
                                if (base == XR_REP_I64 || base == XR_REP_F64) {
                                    uint8_t ntag;
                                    if (t->kind == XR_KIND_BOOL)
                                        ntag = VTAG_BOOL;
                                    else if (base == XR_REP_F64)
                                        ntag = VTAG_F64;
                                    else
                                        ntag = VTAG_I64;
                                    b->narrow_slot = (int16_t)a;
                                    b->narrow_tag = ntag;
                                    b->narrow_rep = base;
                                    b->narrow_block = k ? fall_blk : jump_blk;
                                }
                            }
                        }
                    }
                }
            }
            b->ops_translated++;
            return true;
        }

        case OP_ISNULL_SET: {
            // R[A] = (R[B] == null) — result is bool (0 or 1)
            int a = GETARG_A(inst);
            int rb = GETARG_B(inst);
            XirRef vb = builder_get_slot(b, blk, rb);
            XirRef r;
            // tag comparison for nullable primitive params
            if (rb < 256 &&
                !xir_ref_is_none(b->slot_tag_refs[rb]) &&
                !xir_ref_is_none(b->slot_value_refs[rb]) &&
                XIR_REF_INDEX(vb) == XIR_REF_INDEX(b->slot_value_refs[rb])) {
                XirRef c_null = xir_const_i64(b->func, 0);
                XirRef null_tag = xir_emit_unary(b->func, blk, XIR_CONST_I64, XR_REP_I64, c_null);
                r = xir_emit(b->func, blk, XIR_EQ, XR_REP_I64,
                             b->slot_tag_refs[rb], null_tag);
            } else {
                XirRef c0 = xir_const_i64(b->func, 0);
                XirRef zero = xir_emit_unary(b->func, blk, XIR_CONST_I64, XR_REP_I64, c0);
                r = xir_emit(b->func, blk, XIR_EQ, XR_REP_I64, vb, zero);
            }
            builder_set_slot(b, a, r);
            builder_tag_vreg(b, r, VTAG_BOOL, 0);
            b->ops_translated++;
            return true;
        }

        case OP_TEST: {
            // TEST A k: if (R[A]) != k then skip next instruction
            // Typically followed by JMP. We handle the TEST+JMP pair.
            int a = GETARG_A(inst);
            int k = GETARG_C(inst);  // k flag in C field
            XirRef cond = builder_get_slot(b, blk, a);

            // Peek at next instruction for JMP
            if (pc + 1 < b->code_count) {
                XrInstruction next_inst = PROTO_CODE(b->proto, pc + 1);
                if (GET_OPCODE(next_inst) == OP_JMP) {
                    int sJ = GETARG_sJ(next_inst);
                    uint32_t jump_target = (uint32_t)((int32_t)(pc + 1) + 1 + sJ);
                    uint32_t fall_target = pc + 2;

                    XirBlock *jump_blk = b->pc_to_block[jump_target];
                    XirBlock *fall_blk = b->pc_to_block[fall_target];

                    if (jump_blk && fall_blk) {
                        // TEST A k: "if R[A] != k, skip next (JMP)"
                        // k=0: truthy (!=0) → skip JMP → fall_blk; falsy → take JMP → jump_blk
                        // k=1: falsy  (!=1) → skip JMP → fall_blk; truthy → take JMP → jump_blk
                        if (k) {
                            // k=1: cond truthy → take JMP (jump_blk); falsy → fall_blk
                            xir_block_set_br(blk, cond, jump_blk, fall_blk);
                        } else {
                            // k=0: cond truthy → fall_blk; falsy → take JMP (jump_blk)
                            xir_block_set_br(blk, cond, fall_blk, jump_blk);
                        }
                        xir_block_add_pred(jump_blk, blk, b->func->arena);
                        xir_block_add_pred(fall_blk, blk, b->func->arena);
                    }
                }
            }
            b->ops_translated++;
            return true;
        }

        case OP_RETURN0: {
            XirRef null_ref = xir_const_i64(b->func, 0);
            XirRef v = xir_emit_unary(b->func, blk, XIR_CONST_PTR, XR_REP_PTR, null_ref);
            xir_block_set_ret(blk, v);
            b->ops_translated++;
            return true;
        }

        case OP_RETURN1: {
            int a = GETARG_A(inst);
            XirRef va = builder_get_slot(b, blk, a);
            xir_block_set_ret(blk, va);
            b->ops_translated++;
            return true;
        }

        case OP_RETURN: {
            int a = GETARG_A(inst);
            int nret = GETARG_B(inst);
            if (nret <= 0) {
                XirRef null_ref = xir_const_i64(b->func, 0);
                XirRef v = xir_emit_unary(b->func, blk, XIR_CONST_PTR, XR_REP_PTR, null_ref);
                xir_block_set_ret(blk, v);
            } else if (nret == 1) {
                XirRef va = builder_get_slot(b, blk, a);
                xir_block_set_ret(blk, va);
            } else if (nret <= 8) {
                // Multi-return: first value via x0, extras via jit_ctx->ret_vals[]
                XirRef va = builder_get_slot(b, blk, a);
                xir_block_set_ret(blk, va);

                // Store ret_count
                {
                    XirRef cnt_off = xir_const_i64(b->func, (int64_t)JIT_RET_COUNT_OFFSET);
                    XirRef cnt_val = xir_const_i64(b->func, (int64_t)nret);
                    XirRef cnt = xir_emit_unary(b->func, blk, XIR_CONST_I64, XR_REP_I64, cnt_val);
                    xir_emit_raw(b->func, blk, XIR_STORE_CORO, XR_REP_VOID,
                                 cnt_off, cnt, XIR_NONE);
                    blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
                }

                // Store extra return values to ret_vals[0..nret-2]
                for (int i = 1; i < nret; i++) {
                    XirRef vi = builder_get_slot(b, blk, a + i);
                    int32_t val_offset = JIT_RET_VALS_OFFSET + (i - 1) * 8;
                    XirRef off_ref = xir_const_i64(b->func, (int64_t)val_offset);
                    xir_emit_raw(b->func, blk, XIR_STORE_CORO, XR_REP_VOID,
                                 off_ref, vi, XIR_NONE);
                    blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;

                    // Store value_tag for each extra return value
                    uint8_t tag = vtag_to_value_tag(builder_slot_xr_tag(b, a + i));
                    int32_t tag_offset = JIT_RET_TAGS_OFFSET + (i - 1) * 8;
                    XirRef tag_off = xir_const_i64(b->func, (int64_t)tag_offset);
                    XirRef tag_val = xir_const_i64(b->func, (int64_t)tag);
                    XirRef tag_v = xir_emit_unary(b->func, blk, XIR_CONST_I64,
                                                   XR_REP_I64, tag_val);
                    xir_emit_raw(b->func, blk, XIR_STORE_CORO, XR_REP_VOID,
                                 tag_off, tag_v, XIR_NONE);
                    blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
                }
            } else {
                // More than 8 return values: bail to interpreter
                b->ops_skipped++;
                return true;
            }
            b->ops_translated++;
            return true;
        }

        /* === Bitwise === */
        case OP_BAND:
            translate_binop(b, blk, inst, XIR_AND, XIR_AND, XIR_RT_ADD);
            return true;
        case OP_BOR:
            translate_binop(b, blk, inst, XIR_OR, XIR_OR, XIR_RT_ADD);
            return true;
        case OP_BXOR:
            translate_binop(b, blk, inst, XIR_XOR, XIR_XOR, XIR_RT_ADD);
            return true;
        case OP_SHL:
            translate_binop(b, blk, inst, XIR_SHL, XIR_SHL, XIR_RT_ADD);
            return true;
        case OP_SHR:
            translate_binop(b, blk, inst, XIR_SHR, XIR_SHR, XIR_RT_ADD);
            return true;
        case OP_BNOT: {
            int a = GETARG_A(inst);
            int ra_b = GETARG_B(inst);
            XirRef vb = builder_get_slot(b, blk, ra_b);
            XirRef r = xir_emit_unary(b->func, blk, XIR_NOT, XR_REP_I64, vb);
            builder_tag_vreg(b, r, VTAG_I64, 0);
            builder_set_slot(b, a, r);
            b->ops_translated++;
            return true;
        }

        default: {
            // Delegate to sub-translation functions (split for maintainability)
            if (xir_translate_object_ops(b, cur_blk, pc, inst, op)) return true;
            if (xir_translate_call_ops(b, cur_blk, pc, inst, op)) return true;
            if (xir_translate_misc_ops(b, cur_blk, pc, inst, op)) return true;

            // Unsupported opcode — not yet JIT-translated.
            // Skip gracefully so the builder can reject the function
            // and fall back to the interpreter (see ops_skipped check below).
            const char *name = xr_opcode_name(GET_OPCODE(inst));
            if (!b->nyi_opcode) b->nyi_opcode = name;
            b->ops_skipped++;
            return true;
        }
    }
}

/* ========== Main Build Entry Point ========== */

static XirFunc *build_from_proto_impl_ex(XrProto *proto,
                                          XrProto **shared_protos, int nshared,
                                          struct XrShape *dominant_shape,
                                          bool aot_mode,
                                          XrayIsolate *isolate,
                                          const XirAotOptions *opts);

static XirFunc *build_from_proto_impl(XrProto *proto,
                                       XrProto **shared_protos, int nshared,
                                       struct XrShape *dominant_shape,
                                       bool aot_mode,
                                       XrayIsolate *isolate) {
    return build_from_proto_impl_ex(proto, shared_protos, nshared,
                                     dominant_shape, aot_mode, isolate,
                                     NULL);
}

static XirFunc *build_from_proto_impl_ex(XrProto *proto,
                                          XrProto **shared_protos, int nshared,
                                          struct XrShape *dominant_shape,
                                          bool aot_mode,
                                          XrayIsolate *isolate,
                                          const XirAotOptions *opts) {
    if (!proto) return NULL;

    const char *name = proto->name ? proto->name->data : "<anon>";
    XirFunc *func = xir_func_new(name);
    if (!func) return NULL;

    XirBuilder b;
    builder_init(&b, func, proto);
    b.shared_protos = shared_protos;
    b.nshared_protos = nshared;
    b.aot_mode = aot_mode;
    b.isolate = isolate;
    b.aot_import_map = opts ? opts->import_map : NULL;
    b.aot_import_count = opts ? opts->import_count : 0;
    b.aot_export_slots = opts ? opts->export_slots : NULL;
    b.aot_export_slot_count = opts ? opts->export_slot_count : 0;
    memset(b.import_modules, 0, sizeof(b.import_modules));

    // Wire inline-cache snapshots for type-feedback-driven optimisations.
    // Three cases:
    //   1. opts carries pre-captured snapshots (background JIT thread):
    //      borrow them, do not free.
    //   2. JIT mode with a live isolate (foreground JIT): snapshot from
    //      the current ctx and free at teardown.
    //   3. AOT or off-thread builds without IC data: leave NULL — IC
    //      fast paths will simply skip.
    b.ic_fields_snapshot = NULL;
    b.ic_methods_snapshot = NULL;
    b.ic_builtin_snapshot = NULL;
    b.ic_snapshots_owned = false;
    if (opts && (opts->ic_fields_snapshot || opts->ic_methods_snapshot ||
                 opts->ic_builtin_snapshot)) {
        b.ic_fields_snapshot  = opts->ic_fields_snapshot;
        b.ic_methods_snapshot = opts->ic_methods_snapshot;
        b.ic_builtin_snapshot = opts->ic_builtin_snapshot;
        b.ic_snapshots_owned  = false;
    } else if (!aot_mode && isolate) {
        XrVMContext *ctx = xr_vm_current_ctx(isolate);
        b.ic_fields_snapshot  = xr_vm_ic_fields_snapshot(ctx, proto);
        b.ic_methods_snapshot = xr_vm_ic_methods_snapshot(ctx, proto);
        b.ic_builtin_snapshot = xr_vm_ic_builtin_snapshot(ctx, proto);
        b.ic_snapshots_owned  = true;
    }

    // Step 1: Create basic blocks from bb_leaders and jump targets
    builder_create_blocks(&b);

    // Step 2: Create vregs for function parameters
    func->proto = proto;
    func->num_params = (uint16_t)proto->numparams;
    func->max_stack = (uint16_t)proto->maxstacksize;
    for (int i = 0; i < proto->numparams; i++) {
        uint8_t ptype = XR_REP_TAGGED;
        // Use param_types (authoritative per-parameter types)
        if (proto->param_types && i < proto->param_types_count && proto->param_types[i]) {
            ptype = xr_type_rep(proto->param_types[i]);
            // speculate rep for TAGGED params with stable feedback.
            // Covers numeric unions (int|float) and nullable primitives (int?/float?/bool?).
            // xir_jit_call guards against type mismatch at entry.
            if (ptype == XR_REP_TAGGED &&
                proto->type_feedback && proto->type_feedback->stable &&
                i < XFB_MAX_PARAMS) {
                struct XrType *st = proto->param_types[i];
                uint8_t fb_type = proto->type_feedback->arg_types[i];
                bool speculatable =
                    value_tag_to_vtag(xr_type_to_xr_tag(st)) == VTAG_NUMERIC ||
                    (st->is_nullable && (st->kind == XR_KIND_INT ||
                     st->kind == XR_KIND_FLOAT || st->kind == XR_KIND_BOOL));
                if (speculatable) {
                    if (fb_type == XFB_TYPE_INT || fb_type == XFB_TYPE_BOOL)
                        ptype = XR_REP_I64;
                    else if (fb_type == XFB_TYPE_FLOAT)
                        ptype = XR_REP_F64;
                }
            }
        } else if (proto->type_feedback && proto->type_feedback->stable) {
            XirTypeFeedback *fb = proto->type_feedback;
            if (i < XFB_MAX_PARAMS && xfb_is_monomorphic(fb->arg_types[i])) {
                ptype = xr_slot_to_rep(xfb_to_slot_type(fb->arg_types[i]));
            }
        }
        XirRef param = xir_new_vreg(func, ptype);
        // Set param tag from rep (xir_new_vreg defaults to UNKNOWN).
        // builder_set_slot syncs slot_tag from vreg ctype.
        {
            // Param vregs have no def instruction; xir_ref_ctype uses
            // rep-based fallback (xir_type_from_rep) for type inference.
        }
        builder_set_slot(&b, i, param);
        builder_tag_from_slot(&b, param, i);

        // NOTE: param_tags load_coro is deferred to after this loop
        // to avoid interleaving vregs with param vregs. The codegen
        // prologue assumes param vregs are consecutively numbered.

        // Apply dominant shape hint to PTR parameters.
        if (dominant_shape && ptype == XR_REP_PTR) {
            if (dominant_shape->symbol_to_index) {
                XirVReg *pv = builder_vreg_ref(&b, param);
                if (pv) pv->shape_hint = dominant_shape;
            }
        }
        // Pre-populate layout from parameter type annotation so STRUCT_GET
        // on parameters can inline even without slot_struct_idx.
        if (proto->param_types && i < proto->param_types_count) {
            struct XrType *t = proto->param_types[i];
            struct XrStructLayout *sl = NULL;
            if (t) {
                if ((t->kind == XR_KIND_INSTANCE || t->kind == XR_KIND_CLASS)
                    && t->instance.class_ref && t->instance.class_ref->struct_layout)
                    sl = t->instance.class_ref->struct_layout;
            }
            if (sl) {
                XirVReg *pv = builder_vreg_ref(&b, param);
                if (pv) pv->layout = sl;
            }
        }
    }

    // Step 2a-deferred: Load runtime param_tags for nullable/default params.
    // Must happen AFTER all param vregs are created to preserve consecutive
    // vreg numbering (v0..vN-1) required by the codegen entry prologue.
    for (int i = 0; i < proto->numparams && i < 8; i++) {
        bool needs_tag = false;
        if (proto->param_types && i < proto->param_types_count &&
            proto->param_types[i]) {
            struct XrType *st = proto->param_types[i];
            bool is_prim = (st->kind == XR_KIND_INT ||
                            st->kind == XR_KIND_FLOAT ||
                            st->kind == XR_KIND_BOOL);
            if (is_prim && st->is_nullable)
                needs_tag = true;
            if (is_prim && i >= proto->min_params)
                needs_tag = true;
        }
        if (needs_tag) {
            int32_t tag_offset = (int32_t)(XIR_JIT_PARAM_TAGS_OFFSET + i * 8);
            XirRef off = xir_const_i64(func, (int64_t)tag_offset);
            XirRef tag_vr = xir_emit_unary(func, func->entry, XIR_LOAD_CORO,
                                            XR_REP_I64, off);
            b.slot_tag_refs[i] = tag_vr;
            b.slot_value_refs[i] = b.slot_map[i];
        }
    }

    // Step 2b: Shape guards for parameters.
    // Do NOT emit GUARD_SHAPE at function entry — the parameter may be null
    // (e.g. recursive calls like checksum(null)), and an entry guard would
    // deopt before the user's null check can execute.  Instead, shape guards
    // are emitted lazily at each JSON_GETK/GETPROP that accesses the object,
    // which naturally falls after any null-check branches.  slot_shape is
    // already set above so those access sites can use direct LOAD_FIELD.

    // Step 2c: Initialize Braun SSA and handle entry-as-loop-header
    braun_init(&b);
    b.cur_blk_id = func->entry->id;

    // Re-write params to Braun block_defs (builder_set_slot already called above
    // builder_set_slot was called before braun_init; now sync to block_defs)
    for (int i = 0; i < proto->numparams; i++) {
        if (!xir_ref_is_none(b.slot_map[i]))
            builder_set_slot_in_block(&b, b.cur_blk_id, i, b.slot_map[i]);
    }

    // Pre-add back-edge predecessors for ALL loops so phi narg is correct
    for (int li = 0; li < b.nloops; li++) {
        BuilderLoop *lp = &b.loops[li];
        if (lp->header_pc >= b.code_count) continue;
        XirBlock *header = b.pc_to_block[lp->header_pc];
        if (!header) continue;
        XirBlock *back_blk = b.pc_to_block[lp->back_edge_pc];
        if (!back_blk) {
            for (uint32_t bpc = lp->back_edge_pc; bpc > 0; bpc--) {
                if (b.pc_to_block[bpc]) { back_blk = b.pc_to_block[bpc]; break; }
            }
        }
        if (back_blk) {
            xir_block_add_pred(header, back_blk, func->arena);
        }
        header->is_loop_header = true;
        header->bc_offset = lp->header_pc;
    }

    // Entry-as-loop-header: create preheader for tail recursion
    BuilderLoop *entry_loop = NULL;
    (void)entry_loop;
    {
        BuilderLoop *elp = builder_find_loop(&b, 0);
        if (elp) {
            entry_loop = elp;
            XirBlock *orig_entry = func->entry;

            // Create preheader: becomes new function entry
            XirBlock *preheader = xir_func_add_block(func, "pre");
            xir_block_set_jmp(preheader, orig_entry);
            xir_block_add_pred(orig_entry, preheader, func->arena);

            // Store orig_entry so OP_LOOP_BACK jumps to loop header
            b.pc_to_block[0] = orig_entry;

            // Swap preheader to blocks[0], keep id == index invariant
            for (uint32_t bi = 0; bi < func->nblk; bi++) {
                if (func->blocks[bi] == preheader) {
                    func->blocks[bi] = func->blocks[0];
                    func->blocks[0] = preheader;
                    func->blocks[bi]->id = bi;
                    preheader->id = 0;
                    break;
                }
            }
            func->entry = preheader;

            // Ensure block_defs covers new block ids (AFTER swap)
            uint32_t max_id = orig_entry->id > preheader->id
                            ? orig_entry->id : preheader->id;
            if (max_id >= b.block_defs_size) {
                uint32_t new_size = max_id + 8;
                XR_REALLOC_OR_ABORT(b.block_defs,
                                    new_size * sizeof(BraunBlockDef),
                                    "xir_builder block_defs grow");
                memset(&b.block_defs[b.block_defs_size], 0,
                    (new_size - b.block_defs_size) * sizeof(BraunBlockDef));
                b.block_defs_size = new_size;
            }

            // Write param defs to preheader using FINAL ids (after swap)
            for (int s = 0; s < proto->numparams; s++) {
                if (!xir_ref_is_none(b.slot_map[s]))
                    builder_set_slot_in_block(&b, preheader->id, s, b.slot_map[s]);
            }
            // Clear entry block defs (Braun will create incomplete phis)
            memset(&b.block_defs[orig_entry->id], 0, sizeof(BraunBlockDef));
            // Seal preheader (no predecessors)
            b.block_defs[preheader->id].sealed = true;

            // Mark loop as processed (old code path skip)
            elp->header_pc = UINT32_MAX;

            // Set cur_blk_id to entry (loop header) for main translation
            b.cur_blk_id = orig_entry->id;
        } else {
            // Entry is not a loop header: seal immediately
            braun_seal_block(&b, func->entry);
        }
    }

    // Step 3: Translate bytecode instructions
    XirBlock *cur_blk = b.pc_to_block[0];
    for (uint32_t pc = 0; pc < b.code_count; pc++) {
        // Check if this PC starts a new block
        if (b.pc_to_block[pc] && b.pc_to_block[pc] != cur_blk) {
            // If previous block has no terminator, add fall-through jump
            if (cur_blk->jmp.type == XIR_JMP_NONE) {
                xir_block_set_jmp(cur_blk, b.pc_to_block[pc]);
                xir_block_add_pred(b.pc_to_block[pc], cur_blk, func->arena);
            }
            cur_blk = b.pc_to_block[pc];

            // Fresh allocation status does not survive block boundaries.
            // Clear is_fresh_alloc on all currently-mapped vregs.
            for (int _fs = 0; _fs < 256; _fs++) {
                XirVReg *_fv = builder_vreg_for_slot(&b, _fs);
                if (_fv) _fv->is_fresh_alloc = false;
            }

            // Nullable narrowing: after ISNULL+JMP, narrow the checked
            // slot from TAGGED/UNKNOWN to precise rep/tag in non-null branch.
            // Creates a MOV to a new vreg so the null branch is unaffected.
            if (b.narrow_slot >= 0 && cur_blk == b.narrow_block) {
                int ns = b.narrow_slot;
                XirRef orig = builder_get_slot(&b, cur_blk, ns);
                if (!xir_ref_is_none(orig)) {
                    XirRef narrowed = xir_emit_unary(b.func, cur_blk,
                                                      XIR_MOV, b.narrow_rep, orig);
                    builder_tag_vreg(&b, narrowed, b.narrow_tag, 0);
                    builder_set_slot(&b, ns, narrowed);
                }
                b.narrow_slot = -1;
            }

            // Propagate exception_handler to blocks within try region
            if (b.try_depth > 0) {
                XirBlock *catch_blk = b.try_stack[b.try_depth - 1].catch_block;

                // Catch block entry: restore slot_map from try entry
                if (cur_blk == catch_blk) {
                    memcpy(b.slot_map,
                           b.try_stack[b.try_depth - 1].saved_slot_map,
                           sizeof(b.slot_map));
                    // Catch block itself is NOT inside the try region
                } else {
                    cur_blk->exception_handler = catch_blk;
                }
            }

            // Braun SSA: seal non-loop-header blocks on entry
            b.cur_blk_id = cur_blk->id;
            if (!braun_is_loop_header(&b, pc)) {
                braun_seal_block(&b, cur_blk);
            }
        }

        // Skip unreachable code after block termination (e.g. JMP after RETURN)
        if (cur_blk->jmp.type != XIR_JMP_NONE && !b.pc_to_block[pc]) {
            continue;
        }

        // Skip JMP if it was consumed by a preceding TEST
        XrInstruction inst = PROTO_CODE(proto, pc);
        OpCode op = GET_OPCODE(inst);
        if (op == OP_JMP && pc > 0) {
            XrInstruction prev = PROTO_CODE(proto, pc - 1);
            OpCode prev_op = GET_OPCODE(prev);
            if (prev_op == OP_TEST || prev_op == OP_TESTSET ||
                prev_op == OP_LT || prev_op == OP_LE || prev_op == OP_EQ ||
                prev_op == OP_LTI || prev_op == OP_LEI || prev_op == OP_EQI ||
                prev_op == OP_EQK || prev_op == OP_ISNULL) {
                // Already handled by the TEST translation
                continue;
            }
        }

        b.cur_pc = pc;
        translate_instruction(&b, &cur_blk, pc);

        // Immediate bail out on first NYI opcode — do not continue translating
        // subsequent instructions that may reference undefined vregs.
        if (b.ops_skipped > 0) break;
    }


    // Ensure last block has a terminator
    if (cur_blk->jmp.type == XIR_JMP_NONE) {
        XirRef null_ref = xir_const_i64(func, 0);
        XirRef v = xir_emit_unary(func, cur_blk, XIR_CONST_PTR, XR_REP_PTR, null_ref);
        xir_block_set_ret(cur_blk, v);
    }

    // Reject functions that contain bytecodes with no JIT translation.
    // In Debug builds this has already aborted inside translate_instruction.
    // In Release builds we reach here; bail out cleanly so the function
    // continues running in the interpreter instead of executing corrupt JIT code.
    if (b.ops_skipped > 0) {
        xr_log_warning("jit", "builder skipped %d ops in %s, first nyi: %s",
                       b.ops_skipped, name, b.nyi_opcode ? b.nyi_opcode : "?");
        xir_func_destroy(func);
        builder_cleanup(&b);
        return NULL;
    }

    // Seal all unsealed blocks after all instructions translated.
    // LOOP_BACK defers sealing its loop header so that multiple back-edges
    // targeting the same header all have their definitions written first.
    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        if (!braun_is_sealed(&b, bi)) {
            braun_seal_block(&b, func->blocks[bi]);
        }
    }

    braun_cleanup(&b);
    builder_cleanup(&b);
    return func;
}

XirFunc *xir_build_from_proto(XrProto *proto) {
    XR_DCHECK(proto != NULL, "xir_build_from_proto: proto is NULL");
    return build_from_proto_impl(proto, NULL, 0, NULL, false, NULL);
}

XirFunc *xir_build_from_proto_ex(XrProto *proto,
                                  XrProto **shared_protos, int nshared) {
    XR_DCHECK(proto != NULL, "xir_build_from_proto_ex: proto is NULL");
    return build_from_proto_impl(proto, shared_protos, nshared, NULL, false, NULL);
}

XirFunc *xir_build_from_proto_shaped(XrProto *proto,
                                      struct XrShape *dominant_shape) {
    XR_DCHECK(proto != NULL, "xir_build_from_proto_shaped: proto is NULL");
    return build_from_proto_impl(proto, NULL, 0, dominant_shape, false, NULL);
}

XirFunc *xir_build_from_proto_jit_ex(XrProto *proto,
                                      XrProto **shared_protos, int nshared,
                                      struct XrShape *dominant_shape,
                                      XrayIsolate *isolate,
                                      const XirAotOptions *opts) {
    XR_DCHECK(proto != NULL, "xir_build_from_proto_jit_ex: proto is NULL");
    return build_from_proto_impl_ex(proto, shared_protos, nshared,
                                     dominant_shape, false, isolate, opts);
}

XirFunc *xir_build_from_proto_jit(XrProto *proto,
                                   XrProto **shared_protos, int nshared,
                                   struct XrShape *dominant_shape,
                                   XrayIsolate *isolate) {
    XR_DCHECK(proto != NULL, "xir_build_from_proto_jit: proto is NULL");
    return build_from_proto_impl(proto, shared_protos, nshared, dominant_shape, false, isolate);
}

XirFunc *xir_build_from_proto_aot(XrProto *proto,
                                   XrProto **shared_protos, int nshared,
                                   XrayIsolate *isolate) {
    XR_DCHECK(proto != NULL, "xir_build_from_proto_aot: proto is NULL");
    return build_from_proto_impl(proto, shared_protos, nshared, NULL, true, isolate);
}

XirFunc *xir_build_from_proto_aot_ex(XrProto *proto,
                                      XrProto **shared_protos, int nshared,
                                      XrayIsolate *isolate,
                                      const XirAotOptions *opts) {
    XR_DCHECK(proto != NULL, "xir_build_from_proto_aot_ex: proto is NULL");
    return build_from_proto_impl_ex(proto, shared_protos, nshared, NULL, true,
                                     isolate, opts);
}
