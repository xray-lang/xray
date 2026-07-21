/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xchunk.c - Bytecode chunk implementation
 *
 * KEY CONCEPT:
 *   Manages function prototypes (XrProto) and constant pools.
 *   Provides bytecode emission and constant deduplication.
 */

#include "xchunk.h"
#include "xffi_sig.h"
#include "../object/xstring.h"
#include "../../base/xmalloc.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "xopcode_info.h"
#include "../../base/xchecks.h"

/*
 * Monotonic proto-id allocator. Each XrProto gets a fresh, never-reused
 * identifier assigned at creation. The id is used as an index into the
 * per-coroutine IC tables on XrVMContext, so two protos created in the
 * same isolate get distinct slots even when they live on different
 * workers. 32 bits is more than sufficient for any sane program.
 */
static _Atomic uint32_t s_proto_id_counter = 0;
static XrProtoOpaqueFreeFn s_ir_free_fn = NULL;

// xr_opcode_name is implemented in runtime/value/xopcode_info.c.

// ========== Constant Table Operations ==========

// Initialize constant array
void xr_valuearray_init(ValueArray *array) {
    XR_DCHECK(array != NULL, "valuearray_init: NULL array");
    DYNARRAY_INIT(array, XrValue);
}

// Free constant array
void xr_valuearray_free(ValueArray *array) {
    XR_DCHECK(array != NULL, "valuearray_free: NULL array");
    DYNARRAY_FREE(array);
}

// Add constant to array
// Returns constant index
// Auto-dedup: if constant exists, return existing index
int xr_valuearray_add(ValueArray *array, XrValue value) {
    XR_DCHECK(array != NULL, "valuearray_add: NULL array");
    // Check if same constant already exists (dedup)
    // Require both value equality AND type identity (int vs float must be separate)
    int count = DYNARRAY_COUNT(array);
    for (int i = 0; i < count; i++) {
        XrValue existing = DYNARRAY_GET(array, i, XrValue);
        if (XR_IS_INT(existing) != XR_IS_INT(value))
            continue;
        if (XR_IS_FLOAT(existing) != XR_IS_FLOAT(value))
            continue;
        if (XR_IS_FLOAT(value)) {
            /* Float constants must dedup by exact bit pattern. xr_value_deep_eq
             * compares with ==, under which -0.0 equals +0.0, so it would fold a
             * -0.0 literal onto an existing +0.0 pool slot and silently drop the
             * sign of zero (diverging from the AOT backend, which emits the
             * literal directly). Bit comparison keeps -0.0 and +0.0 distinct. */
            if (memcmp(&existing.f, &value.f, sizeof(double)) == 0)
                return i;
            continue;
        }
        if (xr_value_deep_eq(existing, value)) {
            return i;  // Found duplicate with same type, return existing index
        }
    }

    // Add new constant
    return DYNARRAY_ADD(array, value, XrValue);
}

// ========== XrProto Operations ==========

// Create new function prototype
XrProto *xr_vm_proto_new(void) {
    XrProto *proto = (XrProto *) xr_calloc(1, sizeof(XrProto));
    if (proto == NULL) {
        return NULL;
    }

    // NOTE: All scalar fields are zero-initialized by xr_calloc.
    // Only containers that require explicit init are called below.
    DYNARRAY_INIT(&proto->code, XrInstruction);
    xr_valuearray_init(&proto->constants);
    DYNARRAY_INIT(&proto->protos, XrProto *);
    DYNARRAY_INIT(&proto->upvalues, UpvalInfo);
    DYNARRAY_INIT(&proto->lineinfo, int);
    DYNARRAY_INIT(&proto->locvars, XrLocVar);

    // Assign a globally-unique, never-reused id. IC tables on
    // XrVMContext index by this value to keep XrProto immutable.
    proto->proto_id = atomic_fetch_add_explicit(&s_proto_id_counter, 1u, memory_order_relaxed);

    return proto;
}

void xr_vm_proto_set_ir_free_fn(XrProtoOpaqueFreeFn free_fn) {
    s_ir_free_fn = free_fn;
}

// Free function prototype
void xr_vm_proto_free(XrProto *proto) {
    if (proto == NULL) {
        return;
    }

    // Free nested functions (recursive)
    int proto_count = DYNARRAY_COUNT(&proto->protos);
    for (int i = 0; i < proto_count; i++) {
        XrProto *child = DYNARRAY_GET(&proto->protos, i, XrProto *);
        xr_vm_proto_free(child);
    }

    // Free all dynamic arrays
    DYNARRAY_FREE(&proto->code);
    xr_valuearray_free(&proto->constants);
    DYNARRAY_FREE(&proto->protos);
    DYNARRAY_FREE(&proto->upvalues);
    DYNARRAY_FREE(&proto->lineinfo);
    DYNARRAY_FREE(&proto->locvars);

    // Free return type string
    if (proto->return_type != NULL) {
        xr_free(proto->return_type);
        proto->return_type = NULL;
    }
    if (proto->source_file != NULL) {
        xr_free((void *) proto->source_file);
        proto->source_file = NULL;
    }

    // Free per-function symbol table
    if (proto->symbols != NULL) {
        xr_free(proto->symbols);
        proto->symbols = NULL;
    }

    // Free type pipeline
    if (proto->param_types != NULL) {
        xr_free(proto->param_types);
        proto->param_types = NULL;
    }
    if (proto->inst_types != NULL) {
        xr_free(proto->inst_types);
        proto->inst_types = NULL;
    }
    // return_type_info points into analyzer_pool arena, do not free

    // Free retained Xi SSA IR (consumed by AOT/REPL lowering)
    if (proto->xi_func != NULL) {
        if (s_ir_free_fn != NULL) {
            s_ir_free_fn(proto->xi_func);
        }
        proto->xi_func = NULL;
    }

    // Free self-contained extern signature
    if (proto->ffi_sig != NULL) {
        xr_ffi_sig_free(proto->ffi_sig);
        proto->ffi_sig = NULL;
    }

    // Inline caches are owned by XrVMContext (per-coroutine), not the
    // immutable proto. Nothing to free here for IC.

    // Free XrProto itself
    xr_free(proto);
}

// Add a global symbol to the per-function symbol table.
// Returns local index (0-based), or -1 on allocation failure. Deduplicates:
// if the same global symbol was already added, returns the existing index.
// NEVER return 0 on failure — 0 is a valid local index and the bytecode
// would silently bind the wrong global symbol.
int xr_proto_add_symbol(XrProto *proto, int32_t global_symbol) {
    XR_DCHECK(proto != NULL, "proto_add_symbol: NULL proto");
    XR_DCHECK(global_symbol >= 0, "proto_add_symbol: negative symbol id");
    XR_DCHECK(proto->symbol_count >= 0, "proto_add_symbol: negative count");
    XR_DCHECK(proto->symbol_capacity >= 0, "proto_add_symbol: negative capacity");
    XR_DCHECK(proto->symbol_count <= proto->symbol_capacity, "proto_add_symbol: count > capacity");

    // Dedup: check if already registered
    for (int i = 0; i < proto->symbol_count; i++) {
        if (proto->symbols[i] == global_symbol) {
            return i;
        }
    }

    XR_CHECK(proto->symbol_count <= (int) MAXARG_B,
             "proto: too many unique symbols (>65536), function too complex");

    // Grow if needed
    if (proto->symbol_count >= proto->symbol_capacity) {
        int new_cap = proto->symbol_capacity < 8 ? 8 : proto->symbol_capacity * 2;
        int32_t *new_buf = (int32_t *) xr_realloc(proto->symbols, new_cap * sizeof(int32_t));
        if (!new_buf) {
            fprintf(stderr, "xr_proto_add_symbol: out of memory\n");
            return -1;
        }
        proto->symbols = new_buf;
        proto->symbol_capacity = new_cap;
    }

    int local_idx = proto->symbol_count;
    proto->symbols[local_idx] = global_symbol;
    proto->symbol_count++;

    return local_idx;
}

// Write one instruction
void xr_vm_proto_write(XrProto *proto, XrInstruction inst, int line) {
    XR_DCHECK(proto != NULL, "proto_write: NULL proto");
    XR_DCHECK(line >= 0, "proto_write: negative line number");
    // Add instruction
    DYNARRAY_ADD(&proto->code, inst, XrInstruction);

    // Record line number
    DYNARRAY_ADD(&proto->lineinfo, line, int);
}

// Add constant to constant pool
// Returns constant index
int xr_vm_proto_add_constant(XrProto *proto, XrValue value) {
    XR_DCHECK(proto != NULL, "proto_add_constant: NULL proto");
    return xr_valuearray_add(&proto->constants, value);
}

// Add nested function prototype
// Returns prototype index
int xr_vm_proto_add_proto(XrProto *proto, XrProto *child) {
    XR_DCHECK(proto != NULL, "proto_add_proto: NULL proto");
    XR_DCHECK(child != NULL, "proto_add_proto: NULL child");
    child->enclosing = proto;
    return DYNARRAY_ADD(&proto->protos, child, XrProto *);
}

// Add upvalue info
// Returns upvalue index
int xr_vm_proto_add_upvalue(XrProto *proto, uint16_t index, uint8_t storage_mode, uint8_t is_const,
                            uint8_t slot_type, uint8_t source, uint8_t capture_action,
                            struct XrType *type_info) {
    XR_DCHECK(proto != NULL, "proto_add_upvalue: NULL proto");
    // No dedup here: dedup is done at compiler level in scope_add_upvalue.
    // proto->upvalues must stay in 1-to-1 correspondence with XrCompiler->upvalues[].

    // Add new upvalue
    UpvalInfo new_uv = {.index = index,
                        .storage_mode = storage_mode,
                        .is_const = is_const,
                        .slot_type = slot_type,
                        .source = source,
                        .capture_action = capture_action,
                        .type_info = type_info};
    return DYNARRAY_ADD(&proto->upvalues, new_uv, UpvalInfo);
}

static void xr_vm_entry_plan_scan_proto(const XrProto *proto, bool is_root, XrEntryPlan *plan) {
    if (!proto || !plan)
        return;
    plan->reachable_body_count++;
    int count = DYNARRAY_COUNT(&proto->code);
    for (int i = 0; i < count; i++) {
        XrInstruction instruction = DYNARRAY_GET(&proto->code, i, XrInstruction);
        OpCode op = GET_OPCODE(instruction);
        switch (op) {
            case OP_IMPORT: {
                uint32_t constant_index = GETARG_Bx(instruction);
                if (constant_index >= (uint32_t) VALUEARRAY_COUNT(&proto->constants))
                    break;
                XrValue path_value = VALUEARRAY_GET(&proto->constants, constant_index);
                if (!XR_IS_STRING(path_value))
                    break;
                const char *module = XR_STRING_CHARS(XR_TO_STRING(path_value));
                if (strcmp(module, "runtime") == 0) {
                    plan->required_capability_bits |= XR_CAP_COROUTINE;
                    plan->reachable_effect_bits |= XR_EFFECT_OBSERVES_TASK_ID;
                } else if (strcmp(module, "test_yield") == 0) {
                    /* The internal module contains yieldable native calls.  Its
                     * bytecode import has no per-member effect metadata, so
                     * retain a resumable VM root conservatively. */
                    plan->required_capability_bits |= XR_CAP_COROUTINE;
                    plan->reachable_effect_bits |=
                        XR_EFFECT_OBSERVES_TASK_ID | XR_EFFECT_MAY_SUSPEND;
                }
                break;
            }
            case OP_GO:
                plan->required_capability_bits |= XR_CAP_COROUTINE | XR_CAP_TASK;
                plan->reachable_effect_bits |= XR_EFFECT_MAY_SPAWN | XR_EFFECT_MAY_ALLOC;
                if (is_root && plan->root_representation < XR_ROOT_DESCRIPTOR)
                    plan->root_representation = XR_ROOT_DESCRIPTOR;
                break;
            case OP_THREAD_SPAWN:
                plan->required_capability_bits |=
                    XR_CAP_COROUTINE | XR_CAP_TASK | XR_CAP_SYS_THREAD;
                plan->reachable_effect_bits |= XR_EFFECT_MAY_SPAWN | XR_EFFECT_MAY_ALLOC;
                if (is_root && plan->root_representation < XR_ROOT_DESCRIPTOR)
                    plan->root_representation = XR_ROOT_DESCRIPTOR;
                break;
            case OP_PAR_FOR:
            case OP_PAR_MAP:
                plan->required_capability_bits |= XR_CAP_COROUTINE | XR_CAP_TASK | XR_CAP_PARALLEL;
                plan->reachable_effect_bits |=
                    XR_EFFECT_MAY_SPAWN | XR_EFFECT_MAY_SUSPEND | XR_EFFECT_MAY_ALLOC;
                if (is_root && plan->root_representation < XR_ROOT_DESCRIPTOR)
                    plan->root_representation = XR_ROOT_DESCRIPTOR;
                break;
            case OP_CHAN_NEW:
            case OP_CHAN_NEW_CAP:
            case OP_CHAN_NEW_NAMED:
                plan->required_capability_bits |= XR_CAP_COROUTINE | XR_CAP_CHANNEL;
                plan->reachable_effect_bits |= XR_EFFECT_MAY_ALLOC;
                if (is_root && plan->root_representation < XR_ROOT_DESCRIPTOR)
                    plan->root_representation = XR_ROOT_DESCRIPTOR;
                break;
            case OP_SCOPE_ENTER:
                plan->required_capability_bits |= XR_CAP_COROUTINE | XR_CAP_SCOPE;
                plan->reachable_effect_bits |= XR_EFFECT_MAY_ALLOC;
                if (is_root && plan->root_representation < XR_ROOT_DESCRIPTOR)
                    plan->root_representation = XR_ROOT_DESCRIPTOR;
                break;
            case OP_TIME_AFTER:
                plan->required_capability_bits |= XR_CAP_COROUTINE | XR_CAP_TIMER;
                plan->reachable_effect_bits |= XR_EFFECT_MAY_ALLOC;
                if (is_root && plan->root_representation < XR_ROOT_DESCRIPTOR)
                    plan->root_representation = XR_ROOT_DESCRIPTOR;
                break;
            case OP_AWAIT:
            case OP_AWAIT_TIMEOUT:
            case OP_AWAIT_ALL:
            case OP_AWAIT_ALL_INTO:
            case OP_AWAIT_ANY:
                plan->required_capability_bits |= XR_CAP_COROUTINE | XR_CAP_TASK;
                plan->reachable_effect_bits |= XR_EFFECT_MAY_SUSPEND;
                if (is_root)
                    plan->root_representation = XR_ROOT_RESUMABLE_FRAME;
                break;
            case OP_CHAN_SEND:
            case OP_CHAN_RECV:
            case OP_CHAN_SEND_TIMEOUT:
            case OP_CHAN_RECV_TIMEOUT:
            case OP_SELECT_BLOCK:
                plan->required_capability_bits |= XR_CAP_COROUTINE | XR_CAP_CHANNEL;
                plan->reachable_effect_bits |= XR_EFFECT_MAY_SUSPEND;
                if (is_root)
                    plan->root_representation = XR_ROOT_RESUMABLE_FRAME;
                break;
            case OP_SLEEP:
                plan->required_capability_bits |= XR_CAP_COROUTINE | XR_CAP_TIMER;
                plan->reachable_effect_bits |= XR_EFFECT_MAY_SUSPEND;
                if (is_root)
                    plan->root_representation = XR_ROOT_RESUMABLE_FRAME;
                break;
            case OP_SCOPE_EXIT:
                plan->required_capability_bits |= XR_CAP_COROUTINE | XR_CAP_SCOPE;
                plan->reachable_effect_bits |= XR_EFFECT_MAY_SUSPEND;
                if (is_root)
                    plan->root_representation = XR_ROOT_RESUMABLE_FRAME;
                break;
            case OP_YIELD:
                plan->required_capability_bits |= XR_CAP_COROUTINE;
                plan->reachable_effect_bits |= XR_EFFECT_MAY_SUSPEND;
                if (is_root)
                    plan->root_representation = XR_ROOT_RESUMABLE_FRAME;
                break;
            case OP_GEN_YIELD:
                plan->required_capability_bits |= XR_CAP_COROUTINE | XR_CAP_GENERATOR;
                plan->reachable_effect_bits |= XR_EFFECT_MAY_SUSPEND;
                if (is_root)
                    plan->root_representation = XR_ROOT_RESUMABLE_FRAME;
                break;
            default:
                break;
        }
    }
    int child_count = DYNARRAY_COUNT(&proto->protos);
    for (int i = 0; i < child_count; i++)
        xr_vm_entry_plan_scan_proto(DYNARRAY_GET(&proto->protos, i, XrProto *), false, plan);
}

bool xr_vm_entry_plan_derive(XrProto *root) {
    XrEntryPlan plan;
    if (!root)
        return false;
    memset(&plan, 0, sizeof(plan));
    /* VM bytecode has one canonical module-entry id.  Runtime proto ids are
     * process-local IC indices and must never leak into serialized plans. */
    plan.entry_func_id = 1;
    plan.provided_capability_bits = UINT32_MAX;
    plan.provider_hook_bits = UINT32_MAX;
    plan.evidence = XR_ENTRY_EV_CLOSED_WORLD_REACHABILITY | XR_ENTRY_EV_ROOT_EFFECT |
                    XR_ENTRY_EV_VERIFIED_BYTECODE;
    xr_vm_entry_plan_scan_proto(root, true, &plan);
    plan.runtime_component_bits = plan.required_capability_bits;
    if ((plan.reachable_effect_bits & XR_EFFECT_MAY_SUSPEND) != 0)
        plan.root_representation = XR_ROOT_RESUMABLE_FRAME;
    else if ((plan.reachable_effect_bits & (XR_EFFECT_MAY_SPAWN | XR_EFFECT_OBSERVES_TASK_ID)) != 0)
        plan.root_representation = XR_ROOT_DESCRIPTOR;
    if ((plan.required_capability_bits & (XR_CAP_SYS_THREAD | XR_CAP_PARALLEL)) != 0)
        plan.scheduler_mode = XR_SCHED_MULTI;
    else if (plan.root_representation != XR_ROOT_ELIDED)
        plan.scheduler_mode = XR_SCHED_SINGLE;
    root->entry_plan = plan;
    return xr_vm_entry_plan_validate(root);
}

bool xr_vm_entry_plan_validate(const XrProto *root) {
    const XrEntryPlan *plan;
    if (!root)
        return false;
    plan = &root->entry_plan;
    if (plan->root_representation > XR_ROOT_RESUMABLE_FRAME ||
        plan->scheduler_mode > XR_SCHED_MULTI || plan->unproven_reason != XR_ENTRY_PROVEN)
        return false;
    if ((plan->evidence & (XR_ENTRY_EV_CLOSED_WORLD_REACHABILITY | XR_ENTRY_EV_ROOT_EFFECT |
                           XR_ENTRY_EV_VERIFIED_BYTECODE)) !=
        (XR_ENTRY_EV_CLOSED_WORLD_REACHABILITY | XR_ENTRY_EV_ROOT_EFFECT |
         XR_ENTRY_EV_VERIFIED_BYTECODE))
        return false;
    if ((plan->required_capability_bits & ~plan->provided_capability_bits) != 0)
        return false;
    if (plan->root_representation == XR_ROOT_ELIDED && plan->scheduler_mode != XR_SCHED_NONE)
        return false;
    if (plan->root_representation != XR_ROOT_ELIDED && plan->scheduler_mode == XR_SCHED_NONE)
        return false;
    return true;
}
