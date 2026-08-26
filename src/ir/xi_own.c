/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_own.c - Backward ownership / borrow inference for Xi IR
 *
 * ALGORITHM:
 *   1. Classify every SSA value as RC-managed (heap) or not (scalar).
 *   2. Build def-use chains (xi_defuse) and block-level liveness
 *      (xi_compute_liveness).
 *   3. For each RC value, find its last (consuming) use:
 *        - the use site in the latest block (by RPO) where the value is
 *          NOT live-out, i.e. the value dies there.
 *        - if no use exists, the value is dead → drop at definition.
 *   4. Classify each use as a borrow when the using op only reads the
 *      value (load/compare/print), or owned when it stores/returns/
 *      forwards the value (store_field, return, call argument, ...).
 *   5. Infer a per-function borrow signature for parameters.
 *
 * This is a PURE ANALYSIS: it never mutates the IR. The xi_arc rewrite
 * consumes these annotations to insert XI_RETAIN/XI_RELEASE/XI_OWNER_FORWARD.
 */

#include "xi_own.h"
#include "xi_defuse.h"
#include "xi_analysis.h"
#include "xi_escape.h"
#include "xi_ops_gen.h"
#include "xi_module.h"
#include "xi_value_query.h"
#include "../frontend/analyzer/xanalyzer_builtins.h"
#include "../frontend/analyzer/xa_intrinsic_registry.h"
#include "../frontend/analyzer/xbuiltin_receiver_registry.h"
#include "../runtime/value/xtype.h"
#include "../shared/xr_exact_scalar_registry.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"

#include <stdio.h>
#include <string.h>

/* ========== Type Classification ========== */

XR_FUNC bool xi_own_type_is_rc(const XrType *type) {
    if (!type)
        return true; /* unknown: conservative (treat as RC) */
    /* Runtime-managed objects are owned by the scheduler/runtime, not the
     * compiler's per-coroutine RC. The object-header backstop also protects
     * values whose static type was erased or unknown. */
    if (xr_type_is_runtime_managed(type))
        return false;
    /* CFn<...> is a bare C function pointer (no closure header), not RC-managed. */
    if (XR_TYPE_IS_C_FUNCTION(type))
        return false;
    /* An enum whose variants carry no payload is its ordinal and nothing more:
     * the layout states a zero payload, so there is no allocation behind the
     * value and nothing for a retain to count. Kind alone cannot tell it from
     * the ADT form, whose variants do hold references, so the layout answers.
     *
     * Counting a scalar costs more than the count: the reference operations are
     * emitted, every layer downstream has to describe an ownership the value
     * does not have, and the representation passes refuse a retain whose
     * operand is an i64. */
    if (type->kind == XR_KIND_ENUM && type->enum_type.layout &&
        type->enum_type.layout->is_zero_payload)
        return false;
    return xr_kind_is_reference_counted(type->kind);
}

static const XiModule *xi_own_function_module(const XiFunc *function) {
    for (const XiFunc *owner = function; owner; owner = owner->parent_func)
        if (owner->module)
            return owner->module;
    return NULL;
}

static bool xi_own_locator_matches(XiSourceLocator xi, XrProgramSemanticSourceLocator program) {
    return xi.kind != 0 && xi.kind == program.kind && xi.span.start_line != 0 &&
           xi.span.start_line == program.start_line && xi.span.start_column != 0 &&
           xi.span.start_column == program.start_column && xi.span.end_line != 0 &&
           xi.span.end_line == program.end_line && xi.span.end_column != 0 &&
           xi.span.end_column == program.end_column;
}

static bool xi_own_type_is_pointer_free_program_leaf(const XiFunc *function, const XrType *type) {
    const XiModule *module = xi_own_function_module(function);
    const XrProgramSemanticClosure *closure = module ? module->program_semantic_closure : NULL;
    if (!closure || !type || type->kind != XR_KIND_INSTANCE || !type->instance.class_ref ||
        type->is_nullable || type->is_const || type->is_literal ||
        type->instance.type_arg_count != 0 || type->scalar_rep != XR_SCALAR_REP_NONE ||
        !xr_program_semantic_closure_is_frozen(closure) ||
        !xr_program_semantic_closure_is_verified(closure) ||
        xr_program_semantic_closure_schema(closure) != XR_PROGRAM_SEMANTIC_CLOSURE_SCHEMA_VERSION ||
        xr_program_semantic_closure_family(closure) !=
            XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_AGGREGATE_DIRECT_CALL)
        return false;
    const XiClassData *declaration = NULL;
    for (uint16_t i = 0; i < module->nclasses; i++) {
        const XiClassData *candidate = module->classes ? module->classes[i] : NULL;
        if (!candidate || candidate->class_info != type->instance.class_ref)
            continue;
        if (declaration)
            return false;
        declaration = candidate;
    }
    const XrProgramSemanticTypeRecord *row =
        declaration && declaration->psc_type_index != XI_PSC_ROW_NONE
            ? xr_program_semantic_closure_type(closure, declaration->psc_type_index)
            : NULL;
    const uint8_t required = XR_PROGRAM_SEMANTIC_TYPE_NONNULLABLE |
                             XR_PROGRAM_SEMANTIC_TYPE_NONGENERIC | XR_PROGRAM_SEMANTIC_TYPE_VALUE |
                             XR_PROGRAM_SEMANTIC_TYPE_POINTER_FREE;
    return declaration && row &&
           xi_own_locator_matches(declaration->source_locator, row->declaration_locator) &&
           row->kind == XR_PROGRAM_SEMANTIC_TYPE_LEAF_VALUE_AGGREGATE &&
           row->exact_scalar == XR_EXACT_SCALAR_NONE && row->field_count > 0 &&
           row->flags == required && row->reserved == 0;
}

XR_FUNC bool xi_own_value_is_psc_leaf_aggregate(const XiValue *value) {
    const XiFunc *function = value && value->block ? value->block->func : NULL;
    return value && xi_own_type_is_pointer_free_program_leaf(function, value->type);
}

XR_FUNC bool xi_own_value_is_rc(const XiValue *value) {
    return value && !xi_own_value_is_psc_leaf_aggregate(value) && xi_own_type_is_rc(value->type);
}

XR_FUNC bool xi_own_function_return_is_rc(const XiFunc *function) {
    return function && !xi_own_type_is_pointer_free_program_leaf(function, function->return_type) &&
           xi_own_type_is_rc(function->return_type);
}

XR_FUNC XrTransferAction xi_capture_cross_execution_action(const XiCapture *capture) {
    if (!capture)
        return XR_TRANSFER_REJECT;
    if (capture->storage_domain == XR_STORAGE_CONST_SHARED)
        return XR_TRANSFER_CONST_SHARE;
    if (capture->storage_domain == XR_STORAGE_SYNC_SHARED)
        return XR_TRANSFER_SYNC_SHARE;
    if (capture->storage_domain == XR_STORAGE_MODULE_STATIC)
        return capture->is_mutable ? XR_TRANSFER_REJECT : XR_TRANSFER_MODULE_READ;
    if (capture->capture_kind == XI_CAPTURE_SHARED)
        return XR_TRANSFER_SYNC_SHARE;
    if (capture->storage_domain == XR_STORAGE_TRANSFERABLE)
        return XR_TRANSFER_MOVE_UNIQUE;
    if (capture->capture_kind == XI_CAPTURE_MODULE_LIVE)
        return capture->is_mutable ? XR_TRANSFER_REJECT : XR_TRANSFER_MODULE_READ;
    if (capture->is_mutable || capture->is_reassigned ||
        capture->capture_kind == XI_CAPTURE_BY_IMM_REF)
        return XR_TRANSFER_REJECT;
    /* Cross-execution capture never invents a graph copy. Source `copy(...)`
     * materializes an independent value before capture; an ordinary managed
     * capture without const/sync/move evidence is rejected. */
    if (capture->needs_cell || xi_own_type_is_rc(capture->type))
        return XR_TRANSFER_REJECT;
    return XR_TRANSFER_INLINE_COPY;
}

/* Whether a projection (field / element read) of this type could yield a heap
 * reference, so its owner must stay live until the projection's last use. Only
 * the fixed scalar value types are pure copies that never alias the owner;
 * null / any / unknown are dynamic — a Json field typed `null` from its
 * initializer can hold a reference after reassignment — so they conservatively
 * count as possible references. */
XR_FUNC bool xi_own_type_may_be_ref(const XrType *type) {
    if (!type)
        return true;
    switch (type->kind) {
        case XR_KIND_INT:
        case XR_KIND_FLOAT:
        case XR_KIND_BOOL:
        case XR_KIND_RUNE:
        case XR_KIND_UNIT:
        case XR_KIND_NEVER:
        case XR_KIND_POINTER:
            return false;
        default:
            return true;
    }
}

/* ========== Use-Site Classification: borrow vs owned ========== */

/* Does the using op |user_op| CONSUME (take ownership of) the argument at
 * |arg_idx|, or merely BORROW (read) it?
 *
 * Owned (consuming) uses transfer the value out of the current scope:
 *   - stored into a heap object / container / closure env
 *   - returned / thrown out of the function
 *   - sent across a channel / goroutine / shared slot
 *   - passed as a constructor/aggregate element
 * Borrow (reading) uses leave ownership with the caller:
 *   - arithmetic / comparison / type test
 *   - field/index read, length, print, assertions
 *
 * Mirrors the owned/borrowed split in Koka Parc.hs and Roc inc_dec.rs.
 * Call arguments are conservatively owned until borrow signature inference
 * refines the contract. The per-op policy lives in generated Xi metadata
 * so ownership analysis and ARC rewriting share the same semantic table. */
static bool channel_send_method_payload_arg(const XiValue *user, uint16_t arg_idx) {
    if (!user || arg_idx != 1)
        return false;
    if (user->op == XI_CHAN_SEND || user->op == XI_CHAN_TRY_SEND)
        return true;
    if (user->op != XI_CALL_METHOD || user->nargs < 2 || !xi_value_type_is_channel(user->args[0]))
        return false;
    const char *method = (const char *) user->aux;
    return method && (strcmp(method, "send") == 0 || strcmp(method, "trySend") == 0 ||
                      strcmp(method, "sendTimeout") == 0);
}

static bool type_is_u8_contiguous_view(const XrType *type) {
    if (!type)
        return false;
    if (type->kind == XR_KIND_UNION) {
        for (uint8_t i = 0; i < type->union_type.member_count; i++) {
            if (type_is_u8_contiguous_view(type->union_type.members[i]))
                return true;
        }
        return false;
    }
    if (type->kind != XR_KIND_ARRAY && type->kind != XR_KIND_SLICE && type->kind != XR_KIND_SLICE)
        return false;
    const XrType *elem = xr_type_contiguous_element_type(type);
    return elem && xr_type_is_exact_u8(elem) && !elem->is_nullable;
}

static bool own_type_is_pod_span_elem(const XrType *type) {
    if (!type || type->is_nullable)
        return false;
    switch (type->kind) {
        case XR_KIND_INT:
        case XR_KIND_FLOAT:
        case XR_KIND_BOOL:
        case XR_KIND_RUNE:
            return true;
        default:
            return false;
    }
}

static bool own_builtin_receiver_registry_matches(const XrType *receiver_type,
                                                  XaBuiltinReceiverKind kind) {
    switch (kind) {
        case XA_BUILTIN_RECEIVER_EXACT_INTEGER:
            return receiver_type && receiver_type->kind == XR_KIND_INT &&
                   !receiver_type->is_nullable;
        case XA_BUILTIN_RECEIVER_EXACT_UNSIGNED_INTEGER:
            return xr_type_is_exact_unsigned_integer(receiver_type);
        case XA_BUILTIN_RECEIVER_U8_ARRAY:
            return xr_type_is_u8_array(receiver_type);
        case XA_BUILTIN_RECEIVER_ARRAY:
            return receiver_type && receiver_type->kind == XR_KIND_ARRAY;
        case XA_BUILTIN_RECEIVER_U8_SLICE:
            return xr_type_is_u8_slice(receiver_type);
        case XA_BUILTIN_RECEIVER_POD_SLICE:
            return receiver_type && receiver_type->kind == XR_KIND_SLICE &&
                   own_type_is_pod_span_elem(receiver_type->container.element_type);
    }
    return false;
}

static bool own_call_method_matches_receiver_registry_id(const XiValue *user,
                                                         XaBuiltinReceiverMethodId method_id) {
    const XaBuiltinReceiverMethodSpec *spec = xa_builtin_receiver_method_by_id(method_id);
    if (!spec || !user || (user->op != XI_CALL_METHOD && user->op != XI_CALL_METHOD_DIRECT) ||
        user->nargs < 1 || !user->args[0] || !user->aux)
        return false;
    return strcmp((const char *) user->aux, spec->source_name) == 0 &&
           own_builtin_receiver_registry_matches(user->args[0]->type, spec->receiver);
}

static bool low_level_byte_method_arg_is_borrowed(const XiValue *user, uint16_t arg_idx) {
    if (user && user->op == XI_BYTE_ARRAY_APPEND_FROM) {
        return arg_idx == 1 && user->nargs > 1 &&
               type_is_u8_contiguous_view(user->args[1] ? user->args[1]->type : NULL);
    }
    if (!own_call_method_matches_receiver_registry_id(
            user, XA_BUILTIN_RECEIVER_METHOD_U8_ARRAY_APPEND_FROM))
        return false;

    /* Byte-array append copies from src during the call and never stores it. Treat
     * the source buffer as borrowed so ARC does not retain/release it inside
     * hot copy loops. */
    return arg_idx == 1 && user->nargs > 1 &&
           type_is_u8_contiguous_view(user->args[1] ? user->args[1]->type : NULL);
}

static bool builtin_call_arg_is_borrowed(const XiValue *user, uint16_t arg_idx) {
    if (!user || user->op != XI_CALL_BUILTIN)
        return false;

    if (user->xa_intrinsic_id == XA_INTRINSIC_STRING_BYTE_SLICE_VIEW &&
        user->view_evidence.complete && user->view_evidence.source_operand == 0)
        return arg_idx == 0;

    if (user->xa_intrinsic_id == XA_INTRINSIC_ARRAY_RESERVE)
        return arg_idx == 0;

    if (!user->aux)
        return false;
    const char *name = (const char *) user->aux;

    if (strcmp(name, "array_clear") == 0 || strcmp(name, "array_resize") == 0) {
        return arg_idx == 0;
    }

    /* Value copy and Array<T>(src) construction read their source and build an
     * independent result; neither backend consumes the input. Keeping it with
     * the caller lets ARC drop it at its actual death point. */
    if (strcmp(name, "copy") == 0 || strcmp(name, "array_copy_new") == 0)
        return arg_idx == 0;

    return false;
}

/* String's static constructors that read a byte source or string parts
 * (string.fromUtf8 / fromUtf8Lossy / join) inspect their arguments for the
 * duration of the call and build a fresh string; the native implementations
 * on both backends neither retain nor release them. The fromUtf8 lowering
 * also elides a full-range slice argument (`bytes[:]` becomes the array
 * itself), turning the non-RC view into an RC array in the same borrowed
 * position. Classify these arguments as borrowed so the caller keeps
 * ownership and releases at the argument's death point. */
static bool builtin_string_static_arg_is_borrowed(const XiValue *user, uint16_t arg_idx) {
    if ((user->op != XI_CALL_METHOD && user->op != XI_CALL_METHOD_DIRECT) || arg_idx < 1 ||
        arg_idx >= user->nargs || !user->args[0] || !user->aux)
        return false;
    const XiValue *recv = user->args[0];
    if (recv->op != XI_GET_BUILTIN || !recv->aux || strcmp((const char *) recv->aux, "String") != 0)
        return false;
    const char *method = (const char *) user->aux;
    return strcmp(method, "fromUtf8") == 0 || strcmp(method, "fromUtf8Lossy") == 0 ||
           strcmp(method, "join") == 0;
}

/* JSON kind/validity queries are observational: they never retain, store, or
 * return an alias of the inspected value.  Keeping their sole value argument
 * borrowed preserves scalar widening as a tag check instead of adding a
 * generic RC release to every predicate call.  JSON operations that create,
 * mutate, or return values keep the ordinary fail-closed consuming rule. */
static bool builtin_json_query_arg_is_borrowed(const XiValue *user, uint16_t arg_idx) {
    if ((user->op != XI_CALL_METHOD && user->op != XI_CALL_METHOD_DIRECT) || arg_idx != 1 ||
        user->nargs < 2 || !user->args[0] || !user->args[0]->aux || !user->aux)
        return false;
    const XiValue *receiver = user->args[0];
    if (receiver->op != XI_GET_BUILTIN || strcmp((const char *) receiver->aux, "JSON") != 0)
        return false;
    const char *method = (const char *) user->aux;
    return strcmp(method, "kindOf") == 0 || strcmp(method, "isNull") == 0 ||
           strcmp(method, "isBool") == 0 || strcmp(method, "isInt") == 0 ||
           strcmp(method, "isFloat") == 0 || strcmp(method, "isString") == 0 ||
           strcmp(method, "isArray") == 0 || strcmp(method, "isObject") == 0 ||
           strcmp(method, "isValid") == 0;
}

static bool builtin_module_call_arg_is_borrowed(const XiValue *user, uint16_t arg_idx) {
    if (!user || arg_idx == 0 || arg_idx >= user->nargs || !user->args[0])
        return false;

    const XiFunc *func = user->block ? user->block->func : NULL;
    const XiImportRef *ref = xi_value_import_ref(func, user->args[0]);
    if (!ref || !ref->module_path)
        return false;

    const char *member = NULL;
    if (user->op == XI_CALL) {
        member = ref->member_name;
    } else if ((user->op == XI_CALL_METHOD || user->op == XI_CALL_METHOD_DIRECT) &&
               !ref->member_name) {
        member = (const char *) user->aux;
    }
    if (!member || !xa_builtin_get_module_func_abi_signature(ref->module_path, member))
        return false;

    /* Bodyless native primitives use the source signature as their ABI contract,
     * including private primitives called by an Xray stdlib wrapper.  Public
     * Xray functions are deliberately absent from this registry and obtain
     * ownership from their verified call plan instead.  The current manifest
     * grammar contains only default READ parameters, so native helpers inspect
     * arguments for the duration of the call and neither retain nor release
     * them.  Keep this fail-closed behind the registered ABI lookup: a future
     * MOVE-capable primitive grammar must publish structured parameter modes
     * here before it can transfer ARC ownership. */
    return true;
}

static bool use_is_consuming(const XiValue *user, uint16_t arg_idx) {
    return xi_own_value_arg_is_consuming(user, arg_idx);
}

static bool par_map_arg_transfers_return_result(const XiValue *user, uint16_t arg_idx) {
    if (!user || user->op != XI_PAR_MAP || user->aux_kind != XI_AUX_KIND_PAR_MAP || !user->aux ||
        arg_idx != 4)
        return false;
    const XiParallelMapData *data = (const XiParallelMapData *) user->aux;
    return data && data->direct_lane_writes && !data->into_result && data->lane_count == 1;
}

XR_FUNC bool xi_own_use_is_consuming(uint16_t user_op, uint16_t arg_idx) {
    if (user_op == XI_CALL && arg_idx == 0)
        return false; /* callee closure is borrowed for the duration of the call */
    switch (xi_generated_op_own_use(user_op)) {
        case XI_GEN_OWN_USE_BORROW:
            return false;
        case XI_GEN_OWN_USE_CONSUME:
            return true;
        case XI_GEN_OWN_USE_PASS:
            /* Identity-alias op (PHI/MOVE): the operand's owning reference is
             * not read-and-kept (borrow) but transferred THROUGH to the result,
             * which is the same RC object. For dup/drop placement this behaves
             * exactly like a consume of the operand (the operand is no longer an
             * independent owner afterwards). The independent xi_arc_verify pass
             * consumes the same explicit ownership metadata for C5 checks, but
             * deliberately tracks net deltas per owning reference instead of
             * unioning object lifetimes (multiple owning aliases are legal). */
            return true;
        case XI_GEN_OWN_USE_STORED_VALUE:
            /* arg 0 = container/receiver; arg 1+ = stored value. */
            return arg_idx != 0;
        case XI_GEN_OWN_USE_METHOD_ARGS:
            /* arg 0 = method receiver; arg 1+ = ordinary call args. */
            return arg_idx != 0;
        case XI_GEN_OWN_USE__COUNT:
            return true;
    }
    return true;
}

XR_FUNC bool xi_own_value_arg_is_consuming(const XiValue *user, uint16_t arg_idx) {
    if (!user)
        return true;
    /* SELECT transfers exactly one value arm to its result.  The condition is
     * only inspected and must never consume an ownership token. */
    if (user->op == XI_SELECT && arg_idx == 0)
        return false;
    if ((user->op == XI_GO || user->op == XI_THREAD_SPAWN) && arg_idx > 0)
        return xi_go_arg_transfer_mode(user, (uint16_t) (arg_idx - 1)) == XR_TRANSFER_MOVE;
    if ((user->op == XI_CALL || user->op == XI_CALL_METHOD || user->op == XI_CALL_METHOD_DIRECT) &&
        user->call_plan && arg_idx > 0 && arg_idx <= user->call_plan->nargs &&
        user->call_plan->args[arg_idx - 1].place == user->args[arg_idx])
        /* Both writable `ref` and compiler-inferred readonly aggregate places
         * are borrowed for the duration of the call.  The latter remains a
         * semantic READ argument and must never acquire ARC ownership of the
         * place descriptor itself. */
        return false;
    if (low_level_byte_method_arg_is_borrowed(user, arg_idx))
        return false;
    if (builtin_call_arg_is_borrowed(user, arg_idx))
        return false;
    if (builtin_string_static_arg_is_borrowed(user, arg_idx))
        return false;
    if (builtin_json_query_arg_is_borrowed(user, arg_idx))
        return false;
    if (builtin_module_call_arg_is_borrowed(user, arg_idx))
        return false;
    if (channel_send_method_payload_arg(user, arg_idx))
        return xi_chan_send_transfer_mode(user) == XR_TRANSFER_MOVE;
    if (par_map_arg_transfers_return_result(user, arg_idx))
        return true;
    return xi_own_use_is_consuming(user->op, arg_idx);
}

/* ========== Last-Use Computation ========== */

/* For value |v|, scan its use sites and find the consuming "last use":
 * the use in the block with the highest RPO where the value is not live
 * past that block. Block-level liveness gives us a coarse but sound
 * answer: if the value is NOT live-out of a block that uses it, the last
 * use within that block is a candidate last use.
 *
 * We pick the use site whose block has the maximal RPO among blocks where
 * the value is dead on exit. Ties within a block are broken by taking the
 * textually last consuming use (handled by the caller's scan order). */
static void compute_last_use(XiFunc *f, const XiDefUse *du, const XiLiveness *live, XiValue *v,
                             XiOwnInfo *info) {
    uint32_t nuses = xi_defuse_nuses(du, v->id);
    if (nuses == 0) {
        info->is_dead = true;
        info->last_use_blk = UINT32_MAX;
        info->last_use_val = UINT32_MAX;
        return;
    }

    const XiUseSite *sites = xi_defuse_uses(du, v->id);
    uint32_t best_rpo = 0;
    uint32_t best_blk = UINT32_MAX;
    uint32_t best_val = UINT32_MAX;
    bool found_consuming = false;

    for (uint32_t i = 0; i < nuses; i++) {
        const XiUseSite *s = &sites[i];
        XiBlock *ublk = NULL;
        if (s->block_id < f->nblocks)
            ublk = f->blocks[s->block_id];
        if (!ublk)
            continue;

        bool consuming;
        if (s->kind == XI_USE_CONTROL) {
            /* Block control: return value (RETURN block) consumes; an IF
             * condition only reads (bool, never RC anyway). */
            consuming = (ublk->kind == XI_BLOCK_RETURN);
        } else if (s->kind == XI_USE_PHI_ARG) {
            /* Phi merges flow; the incoming value is consumed into the phi. */
            consuming = true;
        } else {
            /* Value arg use: resolve the user op precisely, then classify. */
            XiValue *user = NULL;
            for (uint32_t vi = 0; vi < ublk->nvalues && !user; vi++) {
                XiValue *cand = ublk->values[vi];
                if (cand && cand->id == s->value_id)
                    user = cand;
            }
            /* Unknown user (shouldn't happen) → conservatively consuming. */
            consuming = user ? use_is_consuming(user, s->arg_idx) : true;
        }

        /* A consuming use where the value is dead on block exit is the
         * authoritative last use along that path. Pick the latest by RPO. */
        bool dead_on_exit = !xi_is_live_out(live, ublk, v);
        if (consuming && dead_on_exit) {
            if (ublk->rpo >= best_rpo) {
                best_rpo = ublk->rpo;
                best_blk = ublk->id;
                best_val = s->value_id;
                found_consuming = true;
            }
        }
    }

    if (found_consuming) {
        info->consumed = true;
        info->last_use_blk = best_blk;
        info->last_use_val = best_val;
    } else {
        /* All uses are borrows (or the value stays live across exits in a
         * way we cannot statically pin to one block). The owning reference
         * must be dropped after its last borrow; mark as needing a drop at
         * a conservatively-determined point (latest using block by RPO). */
        info->consumed = false;
        for (uint32_t i = 0; i < nuses; i++) {
            const XiUseSite *s = &sites[i];
            if (s->block_id >= f->nblocks)
                continue;
            XiBlock *ublk = f->blocks[s->block_id];
            if (!ublk)
                continue;
            if (ublk->rpo >= best_rpo) {
                best_rpo = ublk->rpo;
                best_blk = ublk->id;
                best_val = s->value_id;
            }
        }
        info->last_use_blk = best_blk;
        info->last_use_val = best_val;

        /* If the value is live-out of its last using block, the consume
         * happens on a path we can't statically pin → runtime drop flag. */
        if (best_blk != UINT32_MAX && best_blk < f->nblocks) {
            XiBlock *lb = f->blocks[best_blk];
            if (lb && xi_is_live_out(live, lb, v))
                info->needs_drop_flag = true;
        }
    }
}

/* ========== Borrow Signature Inference (intraprocedural seed) ========== */

/* A parameter is OWNED if any consuming use of it exists, else BORROWED.
 * The cross-function fixpoint for mutually recursive functions can use this
 * intraprocedural result as its initial state. */
static void infer_borrow_sig(XiFunc *f, const XiOwnResult *r, XiBorrowSig *sig) {
    memset(sig, 0, sizeof(*sig));
    uint16_t n = xi_func_semantic_param_count(f);
    if (n > XI_OWN_MAX_PARAMS)
        n = XI_OWN_MAX_PARAMS;
    sig->nparams = (uint8_t) n;
    sig->valid = true;

    for (uint16_t p = 0; p < n; p++) {
        XiValue *pv = f->params[p];
        if (!pv || pv->id >= r->max_id) {
            sig->param_own[p] = XI_OWN_BORROWED;
            continue;
        }
        const XiOwnInfo *pi = &r->values[pv->id];
        if (!pi->rc_managed) {
            sig->param_own[p] = XI_OWN_NONE;
        } else {
            sig->param_own[p] = pi->consumed ? XI_OWN_OWNED : XI_OWN_BORROWED;
        }
    }
}

/* ========== Public API ========== */

XR_FUNC bool xi_own_analyze(XiFunc *f, XiOwnResult *out) {
    XR_DCHECK(f != NULL, "xi_own_analyze: NULL func");
    XR_DCHECK(out != NULL, "xi_own_analyze: NULL out");
    memset(out, 0, sizeof(*out));

    uint32_t max_id = f->next_value_id;
    if (max_id == 0) {
        out->sig.valid = true;
        return true;
    }

    out->values = (XiOwnInfo *) xr_calloc(max_id, sizeof(XiOwnInfo));
    if (!out->values)
        return false;
    out->max_id = max_id;

    /* Ensure RPO is current (liveness depends on it). */
    xi_ensure_rpo(f);

    XiDefUse du;
    xi_defuse_build(&du, f);

    XiLiveness *live = xi_compute_liveness(f);
    if (!live) {
        xi_defuse_free(&du);
        xr_free(out->values);
        memset(out, 0, sizeof(*out));
        return false;
    }

    /* Initialize ownership for every value, then compute last uses for
     * RC-managed ones. */
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *v = blk->values[i];
            if (!v || v->id >= max_id)
                continue;
            XiOwnInfo *info = &out->values[v->id];
            info->rc_managed = xi_own_value_is_rc(v);
            info->last_use_blk = UINT32_MAX;
            info->last_use_val = UINT32_MAX;
            if (!info->rc_managed) {
                info->ownership = XI_OWN_NONE;
                continue;
            }
            info->ownership = XI_OWN_OWNED;
            compute_last_use(f, &du, live, v, info);
            out->n_owned++;
            if (info->is_dead || info->consumed)
                out->n_drop++;
        }
    }

    /* Parameters are owning references handed in by the caller; classify
     * their RC status for the borrow signature. */
    for (uint16_t p = 0; p < xi_func_semantic_param_count(f); p++) {
        XiValue *pv = f->params[p];
        if (!pv || pv->id >= max_id)
            continue;
        XiOwnInfo *info = &out->values[pv->id];
        info->rc_managed = xi_own_value_is_rc(pv);
        if (info->rc_managed) {
            info->ownership = XI_OWN_OWNED;
            compute_last_use(f, &du, live, pv, info);
        }
    }

    infer_borrow_sig(f, out, &out->sig);

    xi_liveness_free(live);
    xi_defuse_free(&du);
    return true;
}

XR_FUNC void xi_own_free(XiOwnResult *out) {
    if (!out)
        return;
    xr_free(out->values);
    memset(out, 0, sizeof(*out));
}

XR_FUNC void xi_own_dump(const XiFunc *f, const XiOwnResult *out) {
    if (!f || !out)
        return;
    fprintf(stderr, "=== xi_own: %s ===\n", f->name ? f->name : "<anon>");
    fprintf(stderr, "  borrow sig (%u params):", out->sig.nparams);
    for (uint8_t p = 0; p < out->sig.nparams; p++) {
        const char *o = out->sig.param_own[p] == XI_OWN_OWNED      ? "owned"
                        : out->sig.param_own[p] == XI_OWN_BORROWED ? "borrow"
                                                                   : "-";
        fprintf(stderr, " p%u=%s", p, o);
    }
    fprintf(stderr, "\n");

    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *v = blk->values[i];
            if (!v || v->id >= out->max_id)
                continue;
            const XiOwnInfo *info = &out->values[v->id];
            if (!info->rc_managed)
                continue;
            fprintf(stderr, "  v%u op=%u %s%s%s", v->id, v->op,
                    info->ownership == XI_OWN_OWNED ? "owned" : "borrow",
                    info->is_dead ? " DEAD(drop@def)" : "",
                    info->needs_drop_flag ? " NEEDS_DROP_FLAG" : "");
            if (info->consumed && info->last_use_blk != UINT32_MAX)
                fprintf(stderr, " last_use=blk%u/v%u", info->last_use_blk, info->last_use_val);
            else if (!info->is_dead && info->last_use_blk != UINT32_MAX)
                fprintf(stderr, " drop@blk%u", info->last_use_blk);
            fprintf(stderr, "\n");
        }
    }
    fprintf(stderr, "  stats: owned=%u borrow=%u drops=%u\n", out->n_owned, out->n_borrow,
            out->n_drop);
}
