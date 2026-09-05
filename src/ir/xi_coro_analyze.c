/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_coro_analyze.c - Shared coroutine suspension analysis for Xi IR
 */

#include "xi_coro_analyze.h"
#include "xi_module.h"
#include "xi_own.h"
#include "xi_ops_gen.h"
#include "xi_receiver_alias.h"
#include "xi_value_query.h"
#include "../frontend/analyzer/xanalyzer_builtins.h"
#include "../base/xglobal_indices.h"
#include "../base/xmalloc.h"
#include "../runtime/value/xtype.h"
#include <string.h>

/* Machine representation set by xi_opt_select_rep (an IR field); mirrors the
 * AOT cg_rep() reader so the analysis can reason about physical slot kinds. */
static XrRep xi_coro_rep(const XiValue *v) {
    return v ? (XrRep) v->rep : XR_REP_TAGGED;
}

/* Interprocedural recursion bound for suspendability. This walk is a
 * LOCAL PROOF WALK: every plan-covered function answers through
 * resolver->func_suspendability (the analyzer's converged, fail-closed
 * summaries) and never recurses here — only plan-less synthetic functions
 * reach the walk at all. Past the bound the walk reports SUSPENDABLE:
 * "conservative" for this analysis means assuming a suspension may exist
 * (the caller compiles a resumable frame it may not need), never assuming
 * its absence (a suspendable function emitted with a plain sync ABI is a
 * miscompile). The depth bound therefore treats unknown callees as suspendable; the old text
 * claimed non-suspendable was the conservative answer, which is fail-open. */
#define XI_CORO_RESOLVE_DEPTH_MAX 8

/* Receiver-returning native members preserve the receiver's declaration
 * identity even when their result type is intentionally erased to `any`.
 * Follow only the sealed alias fact stamped by semantic lowering; an ordinary
 * method result is never treated as its receiver. */
static const XiValue *xi_coro_unwrap_receiver_identity(const XiValue *receiver) {
    while (receiver && receiver->nargs > 0 &&
           (xi_copy_is_identity_alias(receiver) || xi_call_result_aliases_receiver(receiver)))
        receiver = receiver->args[0];
    return receiver;
}

/* Name the callee of a call that targets a native-module ABI member, and hand
 * back the import reference it came from.  NULL when the callee is not a
 * declared native member at all. */
static const char *xi_coro_native_import_member(const XiFunc *f, const XiValue *call,
                                                const XiImportRef **out_ref) {
    if (out_ref)
        *out_ref = NULL;
    if (!f || !call ||
        (call->op != XI_CALL && call->op != XI_CALL_METHOD && call->op != XI_CALL_METHOD_DIRECT) ||
        call->nargs < 1)
        return NULL;
    const XiImportRef *ref = xi_value_import_ref(f, call->args[0]);
    if (!ref || !ref->module_path || ref->resolved_module || ref->resolved_func)
        return NULL;
    const char *member = NULL;
    if (call->op == XI_CALL)
        member = ref->member_name;
    else if (!ref->member_name && call->aux)
        member = (const char *) call->aux;
    if (!member || !xa_builtin_get_module_func_abi_signature(ref->module_path, member))
        return NULL;
    if (out_ref)
        *out_ref = ref;
    return member;
}

/* True when the call reaches its callee through a module import reference that
 * resolves to neither a readable source module nor a grounded native one.
 *
 * The semantic plan classifies exactly this reference as unresolved and grants
 * every call through it no call-target authority, which in turn forbids a
 * coroutine state at the call.  The plan has therefore already settled the
 * call: it is identified and non-suspending, not an open target set.  A single
 * module compiled on its own never runs the module-graph resolver, so this is
 * the state of every cross-module callee it sees. */
static bool xi_coro_call_through_unresolved_import(const XiFunc *f, const XiValue *call) {
    if (!f || !call ||
        (call->op != XI_CALL && call->op != XI_CALL_METHOD && call->op != XI_CALL_METHOD_DIRECT) ||
        call->nargs < 1)
        return false;
    const XiImportRef *ref = xi_value_import_ref(f, call->args[0]);
    /* A reference that did bind a function, module or slot still carries a
     * target for the resolvers to find, whichever bucket the plan files it
     * under.  Only one that bound nothing at all is settled here. */
    return xi_import_ref_is_unresolved(ref) && !ref->resolved_func && !ref->resolved_module &&
           ref->resolved_shared_slot == -1 && ref->resolved_export_slot == -1;
}

/* Classify a call whose callee is reached through a module import: 1 suspends,
 * 0 does not, -1 unknown (the callee does not come from an import at all).
 *
 * Native yieldability is only a proof once the import reference is grounded,
 * and a member absent from the sealed ABI registry is not a native callee at
 * all - in a single-module compile it is usually another module's source
 * export, which no registry can describe.  Both cases share one answer: the
 * reference is unresolved, the plan refuses the call any target authority and
 * forbids a coroutine state at it.  Answering 0 rather than -1 records that
 * the plan has settled the call, so call-resolution completeness holds and the
 * state count matches what the plan will accept. */
static int xi_coro_import_call_suspendability(const XiFunc *f, const XiValue *call) {
    const XiImportRef *ref = NULL;
    const char *member = xi_coro_native_import_member(f, call, &ref);
    if (!member)
        return xi_coro_call_through_unresolved_import(f, call) ? 0 : -1;
    if (!xi_import_ref_is_grounded_native(ref))
        return 0;
    return xa_builtin_module_func_is_yieldable(ref->module_path, member) ? 1 : 0;
}

/* A value-level MAY_SUSPEND stamp is a conservative semantic contract that a
 * target resolver may never erase.  The single exception is a call whose
 * import reference is still unresolved: the plan cannot confirm that spelling,
 * refuses the call any target authority, and rejects a coroutine state at it,
 * so honouring the stamp would only contradict the authority the state is
 * checked against. */
static bool xi_coro_value_carries_suspend_contract(const XiFunc *f, const XiValue *v) {
    return v && (v->flags & XI_FLAG_MAY_SUSPEND) != 0 &&
           !xi_coro_call_through_unresolved_import(f, v);
}

/* ========== Op classifier ========== */

XR_FUNC bool xi_op_is_coroutine(uint16_t op) {
    return xi_generated_op_class(op) == XI_GEN_CLASS_COROUTINE;
}

/* ========== Concurrency method-call recognizers ========== */

static bool xi_value_is_method_call_like(const XiValue *v) {
    return v && (v->op == XI_CALL_METHOD || v->op == XI_CALL_METHOD_DIRECT);
}

XR_FUNC bool xi_value_is_channel_method_call(const XiValue *v, const char *method, int nargs) {
    if (!xi_value_is_method_call_like(v) || v->nargs < 1)
        return false;
    const char *actual = (const char *) v->aux;
    if (!actual || strcmp(actual, method) != 0)
        return false;
    if (nargs >= 0 && (int) v->nargs - 1 != nargs)
        return false;
    if (xi_value_type_is_channel(v->args[0]))
        return true;
    return xi_value_type_is_unknown(v->args[0]) &&
           ((strcmp(method, "send") == 0 && v->nargs == 2) ||
            (strcmp(method, "recv") == 0 && v->nargs == 1) ||
            (strcmp(method, "recvOr") == 0 && v->nargs == 2));
}

XR_FUNC bool xi_value_is_task_method_call(const XiValue *v, const char *method, int nargs) {
    if (!xi_value_is_method_call_like(v) || v->nargs < 1 || !xi_value_type_is_task(v->args[0]))
        return false;
    const char *actual = (const char *) v->aux;
    if (!actual || strcmp(actual, method) != 0)
        return false;
    return nargs < 0 || (int) v->nargs - 1 == nargs;
}

/* The blocking builtin families below are restated for the semantic plan
 * in xr_semantic_builtin_yieldable_methods: same selectors, same arities. A
 * selector added here without adding it there makes the plan reject the very
 * coroutine state this analysis just created. */
static bool xi_value_is_blocking_channel_method_call(const XiValue *v) {
    return xi_value_is_channel_method_call(v, "send", 1) ||
           xi_value_is_channel_method_call(v, "sendTimeout", 2) ||
           xi_value_is_channel_method_call(v, "recv", 0) ||
           xi_value_is_channel_method_call(v, "recvOr", 1) ||
           xi_value_is_channel_method_call(v, "recvTimeout", 1);
}

XR_FUNC bool xi_value_is_blocking_task_method_call(const XiValue *v) {
    return xi_value_is_task_method_call(v, "awaitResult", 0) ||
           xi_value_is_task_method_call(v, "awaitTimeout", 1);
}

/* ========== Intrinsic suspendability predicates (intraprocedural) ========== */

/* A channel send/recv method whose blocking variant requires a coroutine
 * context, also accepting the legacy unknown-typed send/recv shapes. */
static bool xi_channel_method_may_suspend(const XiValue *v) {
    if (!xi_value_is_method_call_like(v) || v->nargs < 1)
        return false;
    const char *method = (const char *) v->aux;
    if (!method)
        return false;
    bool blocking_channel_method = strcmp(method, "send") == 0 || strcmp(method, "recv") == 0 ||
                                   strcmp(method, "recvOr") == 0 ||
                                   strcmp(method, "sendTimeout") == 0 ||
                                   strcmp(method, "recvTimeout") == 0;
    if (!blocking_channel_method)
        return false;
    if (xi_value_type_is_channel(v->args[0]))
        return true;
    return xi_value_type_is_unknown(v->args[0]) &&
           ((strcmp(method, "send") == 0 && v->nargs == 2) ||
            (strcmp(method, "recv") == 0 && v->nargs == 1) ||
            (strcmp(method, "recvOr") == 0 && v->nargs == 2));
}

/* `time.sleep(...)` recognized via the resolver's module-import query so the
 * analysis never depends on the backend's import-resolution internals. */
static bool xi_coro_is_time_sleep_call(const XiFunc *f, const XiValue *v,
                                       const XiCoroResolver *resolver) {
    if (!xi_value_is_method_call_like(v) || v->nargs != 2)
        return false;
    const char *method = (const char *) v->aux;
    if (!method || strcmp(method, "sleep") != 0)
        return false;
    return resolver && resolver->value_is_module_import &&
           resolver->value_is_module_import(resolver->ud, f, v->args[0], "time");
}

static bool xi_coro_is_test_yield_call(const XiFunc *f, const XiValue *v,
                                       const XiCoroResolver *resolver) {
    if (!resolver || !resolver->call_is_module_member)
        return false;
    static const char *const members[] = {"simple", "add", "counter_inc"};
    for (size_t i = 0; i < sizeof(members) / sizeof(members[0]); i++) {
        if (resolver->call_is_module_member(resolver->ud, f, v, "test_yield", members[i]))
            return true;
    }
    return false;
}

/* Hosted AOT lowers the hot TCP operations to a non-blocking step followed by
 * backend-neutral netpoll suspension. Keep this classification in shared IR
 * analysis so frame liveness, state numbering, and codegen agree exactly. */
static bool xi_coro_is_net_io_call(const XiFunc *f, const XiValue *v,
                                   const XiCoroResolver *resolver) {
    if (!resolver || !resolver->call_is_module_member)
        return false;
    static const char *const members[] = {"accept", "read", "write", "writeBytes"};
    for (size_t i = 0; i < sizeof(members) / sizeof(members[0]); i++) {
        if (resolver->call_is_module_member(resolver->ud, f, v, "net", members[i]))
            return true;
    }
    return false;
}

/* ========== Suspendability (function-level, optionally interprocedural) ========== */

static bool xi_coro_func_intrinsic_suspends(const XiFunc *f, const XiCoroResolver *resolver) {
    if (!f)
        return false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!v)
                continue;
            if (xi_coro_value_carries_suspend_contract(f, v))
                return true;
            if (v->op == XI_YIELD || v->op == XI_GEN_YIELD || v->op == XI_GO || v->op == XI_AWAIT ||
                v->op == XI_CHAN_SEND || v->op == XI_CHAN_RECV || v->op == XI_SELECT_BLOCK ||
                v->op == XI_SCOPE_EXIT)
                return true;
            if (xi_channel_method_may_suspend(v))
                return true;
            if (xi_coro_is_time_sleep_call(f, v, resolver))
                return true;
            if (xi_coro_is_test_yield_call(f, v, resolver))
                return true;
            if (xi_coro_is_net_io_call(f, v, resolver))
                return true;
        }
    }
    return false;
}

static const XiFunc *xi_coro_resolve_local_callee(const XiFunc *caller, const XiValue *callee);

static const XiModule *xi_coro_owning_module(const XiFunc *f) {
    for (const XiFunc *owner = f; owner; owner = owner->parent_func) {
        if (owner->module)
            return owner->module;
    }
    return NULL;
}

/* Keep coroutine reachability and ARC parameter ownership on the same frozen
 * namespace/source-method resolver. */
static const XiFunc *xi_coro_resolve_method_callee(const XiFunc *caller, const XiValue *call) {
    return xi_value_resolve_method_callee(caller, call);
}

/* A call through an open function value — a parameter, an upvalue, a value
 * read out of a container — has no statically closed target set.  The
 * semantic plan already classifies exactly this shape as
 * XR_SEM_CALL_TARGET_INDIRECT_CALLABLE and its verifier requires one
 * coroutine state for it, so the Xi layer answers the same way instead of
 * rejecting the call as unresolved.  This is the same predicate the semantic
 * builder uses, kept in step with it deliberately.
 *
 * The excluded spellings all carry a statically recoverable target that
 * xi_coro_resolve_local_callee already resolves, so they never reach here. */
static bool xi_coro_call_is_open_callable(const XiValue *v) {
    if (!v || v->op != XI_CALL || v->nargs < 1)
        return false;
    const XiValue *callee = v->args[0];
    if (!callee || !callee->type || callee->type->kind != XR_KIND_FUNCTION)
        return false;
    while (callee && xi_copy_is_identity_alias(callee) && callee->nargs == 1)
        callee = callee->args[0];
    if (!callee)
        return false;
    return callee->op != XI_IMPORT_REF && callee->op != XI_GET_BUILTIN &&
           callee->op != XI_GET_SHARED && callee->op != XI_CLOSURE_NEW &&
           !(callee->op == XI_STACK_ALLOC && callee->aux_int == XI_CLOSURE_NEW);
}

/* An open target set obliges the plan to allocate a coroutine state, but it is
 * not a proof that control ever leaves this frame - only a resolved target or
 * an explicit suspension op is.  Analyses that must reserve state answer with
 * the obligation; analyses that must prove control transfer ask for the proof
 * alone.  See xi_coro_value_live_across_proven_suspend. */
static bool xi_coro_func_is_suspendable_depth(const XiFunc *f, const XiCoroResolver *resolver,
                                              int depth, bool open_target_suspends) {
    if (f && f->coro_plan && f->coro_plan->analysis_complete) {
        const XiCoroPlan *published = f->coro_plan;
        bool current = published->cfg_rewritten
                           ? published->lowered_ir_revision == f->ir_revision &&
                                 published->lowered_cfg_revision == f->cfg_version
                           : published->analyzed_ir_revision == f->ir_revision &&
                                 published->analyzed_cfg_revision == f->cfg_version;
        if (current)
            return published->is_coroutine;
    }
    if (f && resolver && resolver->func_suspendability) {
        int prepared = resolver->func_suspendability(resolver->ud, f);
        if (prepared >= 0)
            return prepared != 0;
    }
    if (xi_coro_func_intrinsic_suspends(f, resolver))
        return true;
    if (!f)
        return false;
    if (depth >= XI_CORO_RESOLVE_DEPTH_MAX)
        return true; /* unknown past the bound: assume it suspends (fail-closed) */
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!v)
                continue;
            const XiFunc *target = NULL;
            if (v->op == XI_CALL && v->nargs >= 1 && resolver && resolver->resolve_callee) {
                target = resolver->resolve_callee(resolver->ud, f, v->args[0]);
            } else if ((v->op == XI_CALL_METHOD || v->op == XI_CALL_METHOD_DIRECT) &&
                       v->nargs >= 1 && resolver && resolver->resolve_method) {
                target = resolver->resolve_method(resolver->ud, f, v);
            }
            if (!target && v->op == XI_CALL && v->nargs >= 1)
                target = xi_coro_resolve_local_callee(f, v->args[0]);
            if (!target && (v->op == XI_CALL_METHOD || v->op == XI_CALL_METHOD_DIRECT))
                target = xi_coro_resolve_method_callee(f, v);
            int import_suspendability = xi_coro_import_call_suspendability(f, v);
            if (!target && import_suspendability > 0)
                return true;
            if (!target && import_suspendability < 0 && open_target_suspends &&
                xi_coro_call_is_open_callable(v))
                return true;
            if (!target && resolver && resolver->call_suspendability &&
                (v->op == XI_CALL || v->op == XI_CALL_METHOD || v->op == XI_CALL_METHOD_DIRECT)) {
                int prepared = resolver->call_suspendability(resolver->ud, f, v);
                if (prepared > 0)
                    return true;
            }
            if (!target || target == f || target->entry_type == 2)
                continue;
            if (xi_coro_func_is_suspendable_depth(target, resolver, depth + 1,
                                                  open_target_suspends))
                return true;
        }
    }
    return false;
}

XR_FUNC bool xi_coro_func_is_suspendable(const XiFunc *f, const XiCoroResolver *resolver) {
    return xi_coro_func_is_suspendable_depth(f, resolver, 0, true);
}

/* A direct call whose resolved target is (transitively) suspendable is itself
 * a suspension site in the caller. */
static bool xi_coro_call_suspends(const XiFunc *f, const XiValue *v, const XiCoroResolver *resolver,
                                  bool open_target_suspends) {
    if (!v || v->op != XI_CALL || v->nargs < 1)
        return false;
    const XiFunc *target = resolver && resolver->resolve_callee
                               ? resolver->resolve_callee(resolver->ud, f, v->args[0])
                               : NULL;
    if (!target)
        target = xi_coro_resolve_local_callee(f, v->args[0]);
    if (target && target->entry_type == 2)
        return false;
    if (target)
        return xi_coro_func_is_suspendable_depth(target, resolver, 0, open_target_suspends);
    int import_suspendability = xi_coro_import_call_suspendability(f, v);
    if (import_suspendability >= 0)
        return import_suspendability > 0;
    /* An open target set is a state obligation, never a proof of transfer, so
     * a caller asking only for proofs settles the call here rather than
     * letting a resolver answer with the same conservative obligation. */
    if (xi_coro_call_is_open_callable(v))
        return open_target_suspends;
    if (resolver && resolver->call_suspendability) {
        int prepared = resolver->call_suspendability(resolver->ud, f, v);
        /* Unknown targets are rejected independently before a plan is
         * published.  Only a positive closed-world classification creates a
         * suspension state; treating -1 as true would corrupt sync plans. */
        return prepared > 0;
    }
    return false;
}

/* A statically resolved method call whose target is (transitively)
 * suspendable is also a suspension site in the caller. */
static bool xi_coro_method_call_suspends(const XiFunc *f, const XiValue *v,
                                         const XiCoroResolver *resolver,
                                         bool open_target_suspends) {
    if (!v || (v->op != XI_CALL_METHOD && v->op != XI_CALL_METHOD_DIRECT) || v->nargs < 1)
        return false;
    const XiFunc *target =
        resolver && resolver->resolve_method ? resolver->resolve_method(resolver->ud, f, v) : NULL;
    if (!target)
        target = xi_coro_resolve_method_callee(f, v);
    if (target && target->entry_type == 2)
        return false;
    if (target)
        return xi_coro_func_is_suspendable_depth(target, resolver, 0, open_target_suspends);
    int import_suspendability = xi_coro_import_call_suspendability(f, v);
    if (import_suspendability >= 0)
        return import_suspendability > 0;
    if (resolver && resolver->call_suspendability)
        return resolver->call_suspendability(resolver->ud, f, v) > 0;
    return false;
}

/* ========== Suspension-point predicate ========== */

static bool xi_coro_is_suspend_point_impl(const XiFunc *f, const XiValue *v,
                                          const XiCoroResolver *resolver,
                                          bool open_target_suspends) {
    if (!v)
        return false;
    /* A resolved pure-Xray wrapper is not itself suspendable, but these calls
     * are deliberately replaced by the hosted AOT netpoll operation. Test the
     * intrinsic boundary before the direct-target override below. */
    if (xi_coro_is_net_io_call(f, v, resolver))
        return true;
    if (xi_coro_value_carries_suspend_contract(f, v)) {
        /* MAY_SUSPEND is a conservative semantic contract.  A target resolver
         * may identify the child frame but may never erase the shared state. */
        return true;
    }
    if (v->op == XI_YIELD || v->op == XI_GEN_YIELD || v->op == XI_GO || v->op == XI_AWAIT ||
        v->op == XI_CHAN_SEND || v->op == XI_CHAN_RECV || v->op == XI_SELECT_BLOCK ||
        v->op == XI_SCOPE_EXIT)
        return true;
    if (xi_value_is_blocking_channel_method_call(v) || xi_value_is_blocking_task_method_call(v))
        return true;
    if (xi_coro_is_time_sleep_call(f, v, resolver))
        return true;
    if (xi_coro_is_test_yield_call(f, v, resolver))
        return true;
    return xi_coro_call_suspends(f, v, resolver, open_target_suspends) ||
           xi_coro_method_call_suspends(f, v, resolver, open_target_suspends);
}

XR_FUNC bool xi_coro_is_suspend_point(const XiFunc *f, const XiValue *v,
                                      const XiCoroResolver *resolver) {
    return xi_coro_is_suspend_point_impl(f, v, resolver, true);
}

static const XiEnumData *xi_coro_static_enum_namespace(const XiFunc *f, const XiValue *receiver) {
    if (!f || !receiver)
        return NULL;
    const XiValue *identity = xi_coro_unwrap_receiver_identity(receiver);
    if (identity != receiver)
        return xi_coro_static_enum_namespace(f, identity);
    const XiModule *owner_module = xi_coro_owning_module(f);
    if (receiver->op == XI_GET_SHARED && receiver->aux_int >= 0 && owner_module &&
        owner_module->slot_enums && receiver->aux_int < owner_module->nslots)
        return owner_module->slot_enums[receiver->aux_int];
    if (receiver->op == XI_IMPORT_REF && receiver->aux) {
        const XiImportRef *ref = (const XiImportRef *) receiver->aux;
        const XiModule *module = ref->resolved_module;
        if (module && module->slot_enums && ref->resolved_shared_slot >= 0 &&
            ref->resolved_shared_slot < module->nslots)
            return module->slot_enums[ref->resolved_shared_slot];
    }
    return NULL;
}

static bool xi_coro_is_static_enum_member_call(const XiFunc *f, const XiValue *v) {
    if (!v || (v->op != XI_CALL_METHOD && v->op != XI_CALL_METHOD_DIRECT) || v->nargs == 0 ||
        !v->aux)
        return false;
    const XiEnumData *data = xi_coro_static_enum_namespace(f, v->args[0]);
    if (!data)
        return false;
    const char *member = (const char *) v->aux;
    for (uint32_t i = 0; i < data->member_count; i++) {
        if (data->members[i].name && strcmp(data->members[i].name, member) == 0)
            return true;
    }
    return false;
}

/* Calls on language-owned value/container domains have a closed method table
 * after semantic lowering.  Their blocking members are classified separately
 * by the coroutine predicates above; every other validated member is
 * synchronously complete and does not require a user-call target row. */
static bool xi_coro_is_closed_builtin_method_call(const XiValue *v) {
    if (!v || (v->op != XI_CALL_METHOD && v->op != XI_CALL_METHOD_DIRECT) || v->nargs == 0 ||
        !v->args[0] || !v->args[0]->type)
        return false;
    const XiValue *receiver = xi_coro_unwrap_receiver_identity(v->args[0]);
    if (!receiver || !receiver->type)
        return false;
    if (xi_value_type_is_channel(receiver) || xi_value_type_is_task(receiver))
        return true;
    /* Native classes and interfaces carry a NULL class_ref as their frozen
     * declaration identity.  User declarations always carry their owning
     * XrClassInfo, including when they shadow a builtin spelling.  This one
     * identity gate covers StringBuilder, Iterator, Buffer, and the remaining
     * compiler-owned named method tables without a name-based allow-list. */
    const XrType *receiver_type = receiver->type;
    if ((receiver_type->kind == XR_KIND_INSTANCE || receiver_type->kind == XR_KIND_INTERFACE) &&
        receiver_type->instance.class_name && receiver_type->instance.class_ref == NULL)
        return true;
    /* GET_BUILTIN is the frozen class-namespace form for native constructors
     * and static members.  Only reserved compiler globals may carry it; user
     * bindings start at XR_USER_GLOBALS_START and remain subject to ordinary
     * callsite/target resolution. */
    if (receiver->op == XI_GET_BUILTIN && receiver_type->kind == XR_KIND_CLASS &&
        receiver->aux_int > XR_GLOBAL_VAR_RESERVED0 && receiver->aux_int < XR_USER_GLOBALS_START)
        return true;
    switch (receiver_type->kind) {
        case XR_KIND_INT:
        case XR_KIND_FLOAT:
        case XR_KIND_STRING:
        case XR_KIND_BOOL:
        case XR_KIND_ARRAY:
        case XR_KIND_MAP:
        case XR_KIND_SET:
        case XR_KIND_CHANNEL:
        case XR_KIND_JSON:
        case XR_KIND_ENUM:
        case XR_KIND_TUPLE:
        case XR_KIND_FIXED_ARRAY:
        case XR_KIND_POINTER:
        case XR_KIND_RUNE:
        case XR_KIND_STRUCT_OBJECT:
        case XR_KIND_SLICE:
            return true;
        case XR_KIND_NULL:
            /* An optional chain leaves the null branch calling a method on a
             * receiver the plan has already narrowed to null. That call either
             * never runs or raises; neither parks the coroutine, so it is
             * synchronously complete in the same sense as the domains above.
             * Left out, the whole function fails coroutine analysis closed --
             * which is what `a?.m()` on a null-typed receiver used to do. */
            return true;
        default:
            return false;
    }
}

/* A class with no declared instance constructor has a compiler-defined
 * allocation-only constructor. Its closed XiClassData row is the proof: if a
 * constructor member exists, ordinary target/effect resolution must classify
 * that body instead. */
static bool xi_coro_is_default_class_constructor_call(const XiFunc *f, const XiValue *call) {
    const XiFunc *constructor = NULL;
    return xi_value_class_constructor_call(f, call, &constructor) != NULL && !constructor;
}

static bool xi_coro_call_resolution_complete(const XiFunc *f, const XiValue *v,
                                             const XiCoroResolver *resolver) {
    const XiFunc *target = NULL;
    if (!v || (v->op != XI_CALL && v->op != XI_CALL_METHOD && v->op != XI_CALL_METHOD_DIRECT))
        return true;

    /* A plain call is complete only when its callee is statically known or
     * the closed-world callsite summary explicitly classifies it.  Method
     * recognizers below are not valid for XI_CALL: args[0] is the callee,
     * whereas a method call stores its receiver there. */
    if (v->op == XI_CALL) {
        if (xi_coro_is_test_yield_call(f, v, resolver) || xi_coro_is_net_io_call(f, v, resolver))
            return true;
        if (xi_coro_is_default_class_constructor_call(f, v))
            return true;
        if (xi_coro_import_call_suspendability(f, v) >= 0)
            return true;
        if (v->nargs > 0) {
            if (resolver && resolver->resolve_callee)
                target = resolver->resolve_callee(resolver->ud, f, v->args[0]);
            if (!target)
                target = xi_coro_resolve_local_callee(f, v->args[0]);
        }
        if (target)
            return true;
        if (xi_coro_call_is_open_callable(v))
            return true;
        return resolver && resolver->call_suspendability &&
               resolver->call_suspendability(resolver->ud, f, v) >= 0;
    }

    if (xi_coro_is_static_enum_member_call(f, v) || xi_coro_is_closed_builtin_method_call(v) ||
        xi_coro_import_call_suspendability(f, v) >= 0 ||
        xi_value_is_blocking_channel_method_call(v) || xi_value_is_blocking_task_method_call(v) ||
        xi_coro_is_time_sleep_call(f, v, resolver) || xi_coro_is_test_yield_call(f, v, resolver) ||
        xi_coro_is_net_io_call(f, v, resolver))
        return true;

    if (resolver && resolver->resolve_method)
        target = resolver->resolve_method(resolver->ud, f, v);
    if (!target)
        target = xi_coro_resolve_method_callee(f, v);
    if (target)
        return true;
    return resolver && resolver->call_suspendability &&
           resolver->call_suspendability(resolver->ud, f, v) >= 0;
}

static bool xi_coro_all_calls_resolved(const XiFunc *f, const XiCoroResolver *resolver) {
    if (!f)
        return false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *block = f->blocks[bi];
        for (uint32_t vi = 0; block && vi < block->nvalues; vi++) {
            if (!xi_coro_call_resolution_complete(f, block->values[vi], resolver)) {
                return false;
            }
        }
    }
    return true;
}

/* ========== Typed recv/await slot-reuse recognizers ========== */

static bool xi_coro_is_channel_recv_value(const XiValue *v) {
    if (!v)
        return false;
    if (v->op == XI_CHAN_RECV || v->op == XI_CHAN_TRY_RECV)
        return true;
    if (!xi_value_is_method_call_like(v) || v->nargs < 1 || !xi_value_type_is_channel(v->args[0]))
        return false;
    const char *method = (const char *) v->aux;
    return method && strcmp(method, "recv") == 0;
}

static bool xi_coro_is_recv_status_for(const XiValue *user, const XiValue *recv) {
    return user && recv && user->op == XI_CHAN_RECV_STATUS && user->nargs >= 1 &&
           user->args[0] == recv;
}

XR_FUNC const XiValue *xi_coro_recv_status_user(const XiFunc *f, const XiValue *recv) {
    if (!f || !recv || recv->op != XI_CHAN_RECV)
        return NULL;
    const XiValue *status = NULL;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *user = blk->values[vi];
            if (!xi_coro_is_recv_status_for(user, recv))
                continue;
            if (status && status != user)
                return NULL;
            status = user;
        }
    }
    return status;
}

XR_FUNC bool xi_coro_is_paired_recv_status(const XiFunc *f, const XiValue *v) {
    if (!v || v->op != XI_CHAN_RECV_STATUS || v->nargs < 1)
        return false;
    return xi_coro_recv_status_user(f, v->args[0]) == v;
}

XR_FUNC const XiValue *xi_coro_typed_recv_unbox_user(const XiFunc *f, const XiValue *recv) {
    if (!f || !xi_coro_is_channel_recv_value(recv) || xi_coro_rep(recv) != XR_REP_TAGGED)
        return NULL;

    const XiValue *typed_unbox = NULL;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        if (blk->control == recv)
            return NULL;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t a = 0; a < phi->value.nargs; a++) {
                if (phi->value.args[a] == recv)
                    return NULL;
            }
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *user = blk->values[vi];
            if (!user)
                continue;
            for (uint16_t a = 0; a < user->nargs; a++) {
                if (user->args[a] != recv)
                    continue;
                if (recv->op == XI_CHAN_RECV && a == 0 && xi_coro_is_recv_status_for(user, recv))
                    continue;
                if (user->op != XI_UNBOX || a != 0)
                    return NULL;
                XrRep rep = xi_coro_rep(user);
                if (rep != XR_REP_I64 && rep != XR_REP_F64)
                    return NULL;
                if (typed_unbox && typed_unbox != user)
                    return NULL;
                typed_unbox = user;
            }
        }
    }
    return typed_unbox;
}

XR_FUNC bool xi_coro_unbox_from_typed_recv(const XiFunc *f, const XiValue *v) {
    if (!v || v->op != XI_UNBOX || v->nargs < 1)
        return false;
    return xi_coro_typed_recv_unbox_user(f, v->args[0]) == v;
}

XR_FUNC const XiValue *xi_coro_typed_await_unbox_user(const XiFunc *f, const XiValue *await_value) {
    if (!f || !await_value || await_value->op != XI_AWAIT ||
        xi_coro_rep(await_value) != XR_REP_TAGGED)
        return NULL;

    const XiValue *typed_unbox = NULL;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        if (blk->control == await_value)
            return NULL;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t a = 0; a < phi->value.nargs; a++) {
                if (phi->value.args[a] == await_value)
                    return NULL;
            }
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *user = blk->values[vi];
            if (!user)
                continue;
            for (uint16_t a = 0; a < user->nargs; a++) {
                if (user->args[a] != await_value)
                    continue;
                if (user->op != XI_UNBOX || a != 0)
                    return NULL;
                XrRep rep = xi_coro_rep(user);
                if (rep != XR_REP_I64 && rep != XR_REP_F64)
                    return NULL;
                if (typed_unbox && typed_unbox != user)
                    return NULL;
                typed_unbox = user;
            }
        }
    }
    return typed_unbox;
}

XR_FUNC bool xi_coro_unbox_from_typed_await(const XiFunc *f, const XiValue *v) {
    if (!v || v->op != XI_UNBOX || v->nargs < 1)
        return false;
    return xi_coro_typed_await_unbox_user(f, v->args[0]) == v;
}

/* ========== Runtime-slot / aggregate-await recognizers ========== */

XR_FUNC bool xi_coro_value_needs_runtime_slot(const XiValue *v) {
    return v && (v->op == XI_CHAN_RECV || xi_value_is_channel_method_call(v, "sendTimeout", 2) ||
                 xi_value_is_channel_method_call(v, "recv", 0) ||
                 xi_value_is_channel_method_call(v, "recvOr", 1) ||
                 xi_value_is_channel_method_call(v, "recvTimeout", 1) ||
                 xi_value_is_blocking_task_method_call(v));
}

XR_FUNC bool xi_coro_value_is_aggregate_await_tasks(const XiFunc *f, const XiValue *target) {
    if (!f || !target)
        return false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!v || v->op != XI_AWAIT || v->nargs < 1 || v->args[0] != target)
                continue;
            if (((int) v->aux_int & 0x7) != 0)
                return true;
        }
    }
    return false;
}

static bool xi_coro_value_is_await_into_result(const XiFunc *f, const XiValue *target) {
    if (!f || !target)
        return false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!v || v->op != XI_AWAIT || v->nargs < 2 || v->args[1] != target)
                continue;
            if ((v->aux_int & XI_AWAIT_AUX_INTO_RESULT) != 0)
                return true;
        }
    }
    return false;
}

/* ========== Cross-suspend liveness (reuses XiLiveness) ========== */

static bool xi_coro_value_is_func_param(const XiFunc *f, const XiValue *target) {
    for (uint16_t i = 0; i < f->nparams; i++) {
        if (f->params[i] == target)
            return true;
    }
    return false;
}

static bool xi_coro_block_defines_phi(const XiBlock *blk, const XiValue *target) {
    for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
        if (&phi->value == target)
            return true;
    }
    return false;
}

static bool xi_coro_block_defines_value(const XiBlock *blk, const XiValue *target) {
    for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
        if (blk->values[vi] == target)
            return true;
    }
    return false;
}

static bool xi_coro_value_uses_target(const XiValue *user, const XiValue *target) {
    if (!user || !target)
        return false;
    for (uint16_t a = 0; a < user->nargs; a++) {
        if (user->args[a] == target)
            return true;
    }
    return false;
}

/* A retry-on-resume operation reuses its evaluated operands after the
 * scheduler returns to the suspension state.  Ordinary SSA liveness sees the
 * call as one atomic use and would otherwise leave those values in ephemeral
 * resume-stack locals.  The callee/receiver slot at args[0] is metadata for
 * CALL forms; the retry lowering consumes only the actual operands. */
static bool xi_coro_retry_suspend_uses_target(const XiValue *user, const XiValue *target) {
    if (!user || !target || (user->lowering_flags & XI_LOWERING_FLAG_RETRY_SUSPEND_OPERANDS) == 0)
        return false;
    uint16_t start =
        (user->op == XI_CALL || user->op == XI_CALL_METHOD || user->op == XI_CALL_METHOD_DIRECT)
            ? 1u
            : 0u;
    for (uint16_t a = start; a < user->nargs; a++) {
        if (user->args[a] == target)
            return true;
    }
    return false;
}

XR_FUNC bool xi_coro_value_is_retry_suspend_operand(const XiFunc *f, const XiValue *target) {
    if (!f || !target)
        return false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            if (xi_coro_retry_suspend_uses_target(blk->values[vi], target))
                return true;
        }
    }
    return false;
}

static bool xi_coro_block_uses_target_after(const XiBlock *blk, uint32_t start,
                                            const XiValue *target) {
    for (uint32_t vi = start; vi < blk->nvalues; vi++) {
        if (xi_coro_value_uses_target(blk->values[vi], target))
            return true;
    }
    return blk->control == target;
}

static bool xi_coro_block_successor_phi_uses_target(const XiBlock *blk, const XiValue *target) {
    if (!blk || !target)
        return false;
    for (int s = 0; s < 2; s++) {
        const XiBlock *succ = blk->succs[s];
        if (!succ)
            continue;
        for (const XiPhi *phi = succ->phis; phi; phi = phi->next) {
            for (uint16_t p = 0; p < succ->npreds && p < phi->value.nargs; p++) {
                if (succ->preds[p] == blk && phi->value.args[p] == target)
                    return true;
            }
        }
    }
    return false;
}

/* A ref/out/read call plan passes a nonescaping place into the active call.
 * When that call suspends, the place remains part of the child/retry boundary
 * even if ordinary SSA sees only one call-site use. The pointee is lifted by
 * xi_coro_value_address_live_across_suspend, so a local place is reconstructed
 * against stable coroutine-frame storage rather than a resume-stack local. */
static bool xi_coro_suspend_call_plan_uses_place(const XiValue *user, const XiValue *target) {
    if (!user || !target || !user->call_plan || !user->call_plan->verified)
        return false;
    const XiCallPlan *plan = user->call_plan;
    if (plan->has_receiver && plan->receiver.place == target)
        return true;
    for (uint16_t i = 0; i < plan->nargs; i++) {
        if (plan->args && plan->args[i].place == target)
            return true;
    }
    return false;
}

static bool xi_coro_suspend_boundary_uses_target(const XiValue *user, const XiValue *target) {
    return xi_coro_retry_suspend_uses_target(user, target) ||
           xi_coro_suspend_call_plan_uses_place(user, target);
}

static void xi_coro_find_shared_store_in_function(const XiFunc *function, int64_t slot,
                                                  const XiValue **source, bool *ambiguous) {
    if (!function || !source || !ambiguous || *ambiguous)
        return;
    for (uint32_t block_index = 0; block_index < function->nblocks; block_index++) {
        const XiBlock *block = function->blocks[block_index];
        for (uint32_t value_index = 0; block && value_index < block->nvalues; value_index++) {
            const XiValue *store = block->values[value_index];
            if (!store || store->op != XI_SET_SHARED || store->aux_int != slot || store->nargs < 1)
                continue;
            if (*source && *source != store->args[0]) {
                *ambiguous = true;
                return;
            }
            *source = store->args[0];
        }
    }
}

/* Shared-slot numbers are lexical-domain local. Search the caller first and
 * then its parents; never combine numerically equal slots from sibling child
 * functions. The first lexical domain containing a store owns the load. */
static const XiValue *xi_coro_find_lexical_shared_store(const XiFunc *caller, int64_t slot,
                                                        const XiFunc **store_function,
                                                        bool *ambiguous) {
    if (store_function)
        *store_function = NULL;
    if (ambiguous)
        *ambiguous = false;
    for (const XiFunc *owner = caller; owner; owner = owner->parent_func) {
        const XiValue *source = NULL;
        bool owner_ambiguous = false;
        xi_coro_find_shared_store_in_function(owner, slot, &source, &owner_ambiguous);
        if (owner_ambiguous) {
            if (ambiguous)
                *ambiguous = true;
            return NULL;
        }
        if (source) {
            if (store_function)
                *store_function = owner;
            return source;
        }
    }
    return NULL;
}

static const XiFunc *xi_coro_resolve_local_callee_depth(const XiFunc *caller, const XiValue *callee,
                                                        uint8_t depth);

static const XiFunc *xi_coro_resolve_returned_callee(const XiFunc *producer, uint8_t depth) {
    if (!producer || depth > XI_CORO_RESOLVE_DEPTH_MAX)
        return NULL;
    const XiFunc *resolved = NULL;
    bool saw_return = false;
    for (uint32_t block_index = 0; block_index < producer->nblocks; block_index++) {
        const XiBlock *block = producer->blocks[block_index];
        if (!block || block->kind != XI_BLOCK_RETURN || !block->control)
            continue;
        const XiFunc *candidate =
            xi_coro_resolve_local_callee_depth(producer, block->control, (uint8_t) (depth + 1));
        if (!candidate || (resolved && resolved != candidate))
            return NULL;
        resolved = candidate;
        saw_return = true;
    }
    return saw_return ? resolved : NULL;
}

static const XiFunc *xi_coro_resolve_local_callee_depth(const XiFunc *caller, const XiValue *callee,
                                                        uint8_t depth) {
    if (depth > XI_CORO_RESOLVE_DEPTH_MAX)
        return NULL;
    if (!callee)
        return NULL;
    if ((callee->op == XI_CLOSURE_NEW ||
         (callee->op == XI_STACK_ALLOC && callee->aux_int == XI_CLOSURE_NEW)) &&
        callee->aux)
        return (const XiFunc *) callee->aux;
    if (callee->op == XI_GET_SHARED && callee->aux_int >= 0) {
        for (const XiFunc *owner = caller; owner; owner = owner->parent_func) {
            if (owner->shared_slot_funcs &&
                callee->aux_int < (int64_t) owner->shared_slot_func_count &&
                owner->shared_slot_funcs[callee->aux_int])
                return owner->shared_slot_funcs[callee->aux_int];
        }
        const XiFunc *store_function = NULL;
        bool ambiguous = false;
        const XiValue *source =
            xi_coro_find_lexical_shared_store(caller, callee->aux_int, &store_function, &ambiguous);
        if (!ambiguous && source) {
            const XiFunc *direct = xi_coro_resolve_local_callee_depth(
                store_function ? store_function : caller, source, (uint8_t) (depth + 1));
            if (direct)
                return direct;
            if (source->op == XI_CALL && source->nargs >= 1) {
                const XiFunc *producer =
                    xi_coro_resolve_local_callee_depth(store_function ? store_function : caller,
                                                       source->args[0], (uint8_t) (depth + 1));
                const XiFunc *returned =
                    xi_coro_resolve_returned_callee(producer, (uint8_t) (depth + 1));
                if (returned)
                    return returned;
            }
        }
        const XiModule *module = xi_coro_owning_module(caller);
        if (module && module->slot_classes && callee->aux_int < module->nslots) {
            const XiClassData *class_data = module->slot_classes[callee->aux_int];
            for (uint16_t i = 0; class_data && class_data->methods && class_data->child_idx &&
                                 i < class_data->nmethod;
                 i++) {
                if (!class_data->methods[i].is_constructor || class_data->methods[i].is_static)
                    continue;
                uint16_t child = class_data->child_idx[i];
                if (module->init && child < module->init->nchildren)
                    return module->init->children[child];
            }
        }
    }
    /* A successful CHECKTYPE is the same callable object with a refined
     * semantic type. SOURCE_MOVE/OWNER_FORWARD and identity COPY likewise
     * preserve the target. Follow only these first-class forwarding ops;
     * conversions and arbitrary call results remain unresolved. */
    if (callee->nargs > 0 && (xi_copy_is_identity_alias(callee) ||
                              xi_op_is_identity_forward(callee->op) || callee->op == XI_CHECKTYPE))
        return xi_coro_resolve_local_callee_depth(caller, callee->args[0], (uint8_t) (depth + 1));
    const XiImportRef *ref = xi_value_import_ref(caller, callee);
    if (!ref)
        return NULL;
    if (ref->resolved_func)
        return ref->resolved_func;
    const XiModule *module = ref->resolved_module;
    if (!module || !module->slot_classes || ref->resolved_shared_slot < 0 ||
        ref->resolved_shared_slot >= module->nslots)
        return NULL;
    const XiClassData *class_data = module->slot_classes[ref->resolved_shared_slot];
    for (uint16_t i = 0;
         class_data && class_data->methods && class_data->child_idx && i < class_data->nmethod;
         i++) {
        if (!class_data->methods[i].is_constructor || class_data->methods[i].is_static)
            continue;
        uint16_t child = class_data->child_idx[i];
        if (module->init && child < module->init->nchildren)
            return module->init->children[child];
    }
    return NULL;
}

static const XiFunc *xi_coro_resolve_local_callee(const XiFunc *caller, const XiValue *callee) {
    return xi_coro_resolve_local_callee_depth(caller, callee, 0);
}

/* Compute liveness at one suspension without consuming the materialized plan.
 * This is intentionally point-specific: a frame member can cross one state
 * without crossing every state in the function. */
static bool xi_coro_value_direct_live_at_point(const XiFunc *f, const XiLiveness *live,
                                               const XiValue *point, const XiValue *target) {
    if (!f || !live || !point || !point->block || !target)
        return false;
    const XiBlock *blk = point->block;
    bool available = xi_coro_value_is_func_param(f, target) ||
                     xi_coro_block_defines_phi(blk, target) || xi_is_live_in(live, blk, target);
    uint32_t point_index = blk->nvalues;
    for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
        if (blk->values[vi] == target)
            available = true;
        if (blk->values[vi] == point) {
            point_index = vi;
            break;
        }
    }
    if (point_index == blk->nvalues)
        return false;
    if (target == point)
        available = true;
    if (!available)
        return false;

    bool await_result = point->op == XI_AWAIT && point->nargs >= 2 && point->args[1] == target &&
                        (point->aux_int & XI_AWAIT_AUX_INTO_RESULT) != 0;
    bool aggregate_await = point->op == XI_AWAIT && point->nargs >= 1 && point->args[0] == target &&
                           (((int) point->aux_int & 0x7) != 0);
    return await_result || aggregate_await || xi_coro_suspend_boundary_uses_target(point, target) ||
           xi_is_live_out(live, blk, target) ||
           xi_coro_block_uses_target_after(blk, point_index + 1, target) ||
           xi_coro_block_successor_phi_uses_target(blk, target) ||
           (target == point && xi_coro_value_needs_runtime_slot(target));
}

static bool xi_coro_value_live_at_point(const XiFunc *f, const XiLiveness *live,
                                        const XiValue *point, const XiValue *target) {
    if (xi_coro_value_direct_live_at_point(f, live, point, target))
        return true;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *place = blk->values[vi];
            if (place && place->op == XI_LOCAL_ADDR && place->nargs >= 1 &&
                place->args[0] == target &&
                xi_coro_value_direct_live_at_point(f, live, point, place))
                return true;
        }
    }
    return false;
}

/* Once lowering has isolated the suspension in its own block, the frame
 * boundary is exactly that block's live-out set plus values consumed or
 * produced by retrying the suspension operation itself.  Do not reuse the
 * pre-split availability walk here: definitions that moved to the resume
 * block are deliberately no longer available before the scheduler exit. */
static bool xi_coro_value_live_at_split_point(const XiFunc *f, const XiLiveness *live,
                                              const XiValue *point, const XiValue *target) {
    if (!f || !live || !point || !point->block || !target)
        return false;
    bool await_result = point->op == XI_AWAIT && point->nargs >= 2 && point->args[1] == target &&
                        (point->aux_int & XI_AWAIT_AUX_INTO_RESULT) != 0;
    bool aggregate_await = point->op == XI_AWAIT && point->nargs >= 1 && point->args[0] == target &&
                           (((int) point->aux_int & 0x7) != 0);
    if (xi_is_live_out(live, point->block, target) || await_result || aggregate_await ||
        xi_coro_suspend_boundary_uses_target(point, target) ||
        (target == point && xi_coro_value_needs_runtime_slot(target)))
        return true;

    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *block = f->blocks[bi];
        if (!block)
            continue;
        for (uint32_t vi = 0; vi < block->nvalues; vi++) {
            const XiValue *place = block->values[vi];
            if (place && place->op == XI_LOCAL_ADDR && place->nargs >= 1 &&
                place->args[0] == target &&
                (xi_is_live_out(live, point->block, place) ||
                 xi_coro_suspend_boundary_uses_target(point, place)))
                return true;
        }
    }
    return false;
}

static bool xi_coro_value_live_across_suspend_impl(const XiFunc *f, const XiLiveness *live,
                                                   const XiValue *target,
                                                   const XiCoroResolver *resolver,
                                                   bool open_target_suspends) {
    if (!f || !live || !target)
        return false;
    if (xi_coro_value_is_await_into_result(f, target))
        return true;

    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        bool defined_in_block = xi_coro_block_defines_value(blk, target);
        bool available = xi_coro_value_is_func_param(f, target) ||
                         (!defined_in_block && xi_is_live_in(live, blk, target)) ||
                         xi_coro_block_defines_phi(blk, target);
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (v == target) {
                available = true;
                if (xi_coro_is_suspend_point_impl(f, v, resolver, open_target_suspends) &&
                    (xi_is_live_out(live, blk, target) ||
                     xi_coro_block_uses_target_after(blk, vi + 1, target) ||
                     xi_coro_block_successor_phi_uses_target(blk, target) ||
                     xi_coro_value_needs_runtime_slot(target)))
                    return true;
                continue;
            }
            if (!available || !xi_coro_is_suspend_point_impl(f, v, resolver, open_target_suspends))
                continue;
            if (xi_coro_suspend_boundary_uses_target(v, target) ||
                xi_is_live_out(live, blk, target) ||
                xi_coro_block_uses_target_after(blk, vi + 1, target) ||
                xi_coro_block_successor_phi_uses_target(blk, target))
                return true;
        }
    }
    return false;
}

XR_FUNC bool xi_coro_value_live_across_suspend(const XiFunc *f, const XiLiveness *live,
                                               const XiValue *target,
                                               const XiCoroResolver *resolver) {
    return xi_coro_value_live_across_suspend_impl(f, live, target, resolver, true);
}

/* Liveness across only those suspension points that are proven, ignoring the
 * ones an open target set merely obliges the plan to reserve state for.
 *
 * Raw Xi verification runs before any dependency-complete call resolver and
 * before the coroutine plan exists, so an open target set is the one thing it
 * can neither confirm nor refine.  Rejecting a borrow there would forbid what
 * the later stage is required to do: coroutine lowering spills every parameter
 * and call-plan place that reaches a materialized point, and the CoroLowered
 * verification demands exactly those spills.  Ask this question when a missing
 * proof must not become a rejection. */
XR_FUNC bool xi_coro_value_live_across_proven_suspend(const XiFunc *f, const XiLiveness *live,
                                                      const XiValue *target) {
    return xi_coro_value_live_across_suspend_impl(f, live, target, NULL, false);
}

/* A call-bound place is itself a raw pointer, so ordinary SSA liveness keeps
 * the pointer but cannot express that its pointee must also have a stable
 * address.  When XI_LOCAL_ADDR crosses a suspension, lift its source into the
 * logical frame as well; the emitted address then points into the heap frame
 * instead of a vanished resume-stack local. */
static bool xi_coro_value_address_live_across_suspend(const XiFunc *f, const XiLiveness *live,
                                                      const XiValue *target,
                                                      const XiCoroResolver *resolver) {
    if (!f || !live || !target)
        return false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *place = blk->values[vi];
            if (place && place->op == XI_LOCAL_ADDR && place->nargs >= 1 &&
                place->args[0] == target &&
                xi_coro_value_live_across_suspend(f, live, place, resolver))
                return true;
        }
    }
    return false;
}

/* ========== Logical frame membership ========== */

XR_FUNC bool xi_coro_value_is_logical_member(const XiFunc *f, const XiValue *v,
                                             const XiLiveness *live,
                                             const XiCoroResolver *resolver) {
    if (!v)
        return false;
    if (xi_coro_unbox_from_typed_await(f, v))
        return true;
    if (xi_coro_unbox_from_typed_recv(f, v))
        return true;
    if (xi_coro_is_paired_recv_status(f, v))
        return true;
    if (xi_coro_value_needs_runtime_slot(v))
        return true;
    if (xi_coro_value_is_aggregate_await_tasks(f, v))
        return true;
    if (xi_coro_value_is_await_into_result(f, v))
        return true;
    if (v->op == XI_GO || v->op == XI_THREAD_SPAWN)
        return true;
    return xi_coro_value_live_across_suspend(f, live, v, resolver) ||
           xi_coro_value_address_live_across_suspend(f, live, v, resolver);
}

/* ========== Slot attributes ========== */

XR_FUNC const XiValue *xi_coro_release_origin(const XiValue *v) {
    const XiValue *cur = v;
    for (int depth = 0; cur && depth < 8; depth++) {
        if (xi_value_forwards_repr(cur) && cur->nargs >= 1) {
            cur = cur->args[0];
            continue;
        }
        break;
    }
    return cur;
}

XR_FUNC const XiValue *xi_coro_builtin_origin(const XiValue *v) {
    const XiValue *origin = xi_coro_release_origin(v);
    return origin && origin->op == XI_GET_BUILTIN ? origin : NULL;
}

/* A value produced directly by a runtime concurrency bridge already carries a
 * fresh reference, so the coroutine frame must not double-release it. */
static bool xi_coro_value_from_runtime_bridge(const XiValue *v) {
    const XiValue *origin = xi_coro_release_origin(v);
    if (!origin)
        return false;
    switch (origin->op) {
        case XI_GO:
        case XI_THREAD_SPAWN:
        case XI_AWAIT:
        case XI_CHAN_SEND:
        case XI_CHAN_RECV:
        case XI_CHAN_RECV_STATUS:
        case XI_CHAN_TRY_SEND:
        case XI_CHAN_TRY_RECV:
        case XI_CHAN_IS_CLOSED:
        case XI_TIME_AFTER:
        case XI_SELECT_BLOCK:
        case XI_CHAN_NEW:
        case XI_TUPLE_GET:
        case XI_GET_BUILTIN:
            return true;
        case XI_LOAD_FIELD:
            if (origin->nargs >= 1 && xi_coro_builtin_origin(origin->args[0]))
                return true;
            return origin->nargs >= 1 && xi_value_type_is_task(origin->args[0]);
        case XI_CALL_METHOD:
        case XI_CALL_METHOD_DIRECT:
            return origin->nargs >= 1 && (xi_value_type_is_channel(origin->args[0]) ||
                                          xi_value_type_is_task(origin->args[0]));
        default:
            return false;
    }
}

XR_FUNC bool xi_coro_value_needs_arc_release(const XiValue *v) {
    return v && (xi_coro_rep(v) == XR_REP_TAGGED || xi_coro_rep(v) == XR_REP_PTR) &&
           xi_own_type_is_rc(v->type) && !xi_coro_value_from_runtime_bridge(v);
}

XR_FUNC bool xi_coro_value_rep_can_trace_root(const XiValue *v) {
    if (!v)
        return false;
    XrRep rep = xi_coro_rep(v);
    if (rep == XR_REP_TAGGED)
        return true;
    return rep == XR_REP_PTR && xi_own_type_is_rc(v->type);
}

XR_FUNC bool xi_coro_type_needs_boundary_clone(const XrType *type) {
    if (!type)
        return true;
    switch (type->kind) {
        case XR_KIND_ARRAY:
        case XR_KIND_SLICE:
        case XR_KIND_MAP:
        case XR_KIND_SET:
        case XR_KIND_FIXED_ARRAY:
        case XR_KIND_JSON:
        case XR_KIND_STRUCT_OBJECT:
        case XR_KIND_STRING:
            return true;
        case XR_KIND_INSTANCE:
            return xr_type_is_builtin_named_class(type, "StringBuilder");
        case XR_KIND_UNION:
            for (uint8_t i = 0; i < type->union_type.member_count; i++) {
                if (xi_coro_type_needs_boundary_clone(type->union_type.members[i]))
                    return true;
            }
            return false;
        default:
            return false;
    }
}

XR_FUNC bool xi_coro_value_needs_boundary_clone(const XiValue *v) {
    const XiValue *origin = xi_coro_release_origin(v);
    return xi_coro_type_needs_boundary_clone(origin ? origin->type : (v ? v->type : NULL));
}

XR_FUNC bool xi_coro_value_has_json_type(const XiValue *v) {
    const XiValue *origin = xi_coro_release_origin(v);
    const XrType *type = origin ? origin->type : (v ? v->type : NULL);
    return type && type->kind == XR_KIND_JSON;
}

/* ========== Plan ========== */

XR_FUNC const XiCoroSlot *xi_coro_plan_find_slot(const XiCoroPlan *plan, const XiValue *v) {
    if (!plan || !v)
        return NULL;
    for (uint32_t i = 0; i < plan->nslots; i++) {
        if (plan->slots[i].value == v)
            return &plan->slots[i];
    }
    return NULL;
}

XR_FUNC const XiCoroEdge *xi_coro_point_find_edge(const XiCoroSuspendPoint *point,
                                                  XiCoroEdgeKind kind) {
    if (!point || kind >= XI_CORO_EDGE_KIND_COUNT)
        return NULL;
    for (uint8_t i = 0; i < point->nedges; i++) {
        if (point->edges[i].kind == (uint8_t) kind)
            return &point->edges[i];
    }
    return NULL;
}

XR_FUNC const XiCoroSuspendPoint *xi_coro_plan_find_point(const XiCoroPlan *plan,
                                                          const XiValue *op) {
    if (!plan || !op)
        return NULL;
    for (uint32_t i = 0; i < plan->nstates; i++) {
        if (plan->points[i].op == op)
            return &plan->points[i];
    }
    return NULL;
}

XR_FUNC bool xi_coro_plan_is_logical_member(const XiCoroPlan *plan, const XiValue *v) {
    return xi_coro_plan_find_slot(plan, v) != NULL;
}

static XiCoroSuspendKind xi_coro_suspend_kind(const XiFunc *f, const XiValue *v,
                                              const XiCoroResolver *resolver) {
    /* Direct suspend ops map straight to their kind; anything else is a
     * concurrency method call, a stdlib sleep, or a suspendable direct call. */
    if (v->op == XI_YIELD)
        return XI_CORO_SUSP_YIELD;
    if (v->op == XI_GO)
        return XI_CORO_SUSP_GO;
    if (v->op == XI_AWAIT)
        return XI_CORO_SUSP_AWAIT;
    if (v->op == XI_CHAN_SEND)
        return XI_CORO_SUSP_CHAN_SEND;
    if (v->op == XI_CHAN_RECV)
        return XI_CORO_SUSP_CHAN_RECV;
    if (v->op == XI_SELECT_BLOCK)
        return XI_CORO_SUSP_SELECT;
    if (v->op == XI_SCOPE_EXIT)
        return XI_CORO_SUSP_SCOPE_EXIT;
    if (xi_value_is_channel_method_call(v, "send", 1) ||
        xi_value_is_channel_method_call(v, "sendTimeout", 2))
        return XI_CORO_SUSP_CHAN_SEND;
    if (xi_value_is_channel_method_call(v, "recv", 0) ||
        xi_value_is_channel_method_call(v, "recvOr", 1) ||
        xi_value_is_channel_method_call(v, "recvTimeout", 1))
        return XI_CORO_SUSP_CHAN_RECV;
    if (xi_value_is_blocking_task_method_call(v))
        return XI_CORO_SUSP_AWAIT;
    if (xi_coro_is_time_sleep_call(f, v, resolver))
        return XI_CORO_SUSP_SLEEP;
    return XI_CORO_SUSP_CALL;
}

/* A borrowed result names storage owned by another Xi value.  Moving that SSA
 * name into the coroutine frame does not manufacture a second ARC reference:
 * the owner's borrow closure keeps the real owner live, rooted, and releasable.
 * Treating the alias as an independent frame root/release both double-counts
 * it and, on cancellation, can release the same object twice.  VALUE_CLONE is
 * the one COPY form that allocates an independent owner. */
static bool xi_coro_slot_is_borrowed_alias(const XiValue *v) {
    return v && !xi_copy_is_value_clone(v) &&
           xi_generated_op_result_ownership(v->op) == XI_GEN_RESULT_OWNERSHIP_BORROWED;
}

static const XiValue *xi_coro_slot_owner(const XiValue *v) {
    const XiValue *owner = xi_coro_release_origin(v);
    return owner ? owner : v;
}

/* A representation alias of a freshly owned value becomes the physical owner
 * carrier when the original SSA name no longer crosses the suspension.  The
 * stable owner id keeps that decision independent of the target representation. */
static bool xi_coro_slot_can_carry_owner(const XiCoroSlot *slot) {
    if (!slot || !slot->value)
        return false;
    if (!xi_coro_slot_is_borrowed_alias(slot->value))
        return true;
    const XiValue *owner = xi_coro_slot_owner(slot->value);
    return owner && owner != slot->value && xi_coro_value_needs_arc_release(owner);
}

static bool xi_coro_slot_carries_owner_at_point(const XiFunc *f, const XiLiveness *live,
                                                const XiValue *point, const XiCoroSlot *slot,
                                                bool split) {
    if (!xi_coro_slot_can_carry_owner(slot))
        return false;
    if (!xi_coro_slot_is_borrowed_alias(slot->value))
        return true;
    const XiValue *owner = xi_coro_slot_owner(slot->value);
    bool owner_live = split ? xi_coro_value_live_at_split_point(f, live, point, owner)
                            : xi_coro_value_live_at_point(f, live, point, owner);
    return !owner_live;
}

static bool xi_coro_value_is_logical_root(const XiValue *v) {
    return v && v->type && xi_own_type_is_rc(v->type);
}

static void xi_coro_fill_slot(XiCoroSlot *slot, const XiFunc *f, XiValue *v, XiCoroSlotKind kind,
                              const XiLiveness *live, const XiCoroResolver *resolver) {
    slot->value = v;
    slot->type = v->type;
    const XiValue *owner = xi_coro_slot_owner(v);
    slot->owner_value_id = owner ? owner->id : UINT32_MAX;
    slot->logical_rep = (uint8_t) xi_coro_rep(v);
    slot->kind = (uint8_t) kind;
    slot->is_root = xi_coro_value_is_logical_root(v);
    bool borrowed_alias = xi_coro_slot_is_borrowed_alias(v);
    slot->needs_release = xi_coro_value_needs_arc_release(v) &&
                          (!borrowed_alias || xi_coro_slot_can_carry_owner(slot));
    slot->needs_runtime_slot = xi_coro_value_needs_runtime_slot(v);
    slot->needs_boundary_clone = xi_coro_value_needs_boundary_clone(v);
    slot->live_across = xi_coro_value_live_across_suspend(f, live, v, resolver) ||
                        xi_coro_value_address_live_across_suspend(f, live, v, resolver);
    slot->frame_root = false;
    slot->frame_release = false;
}

static void *xi_coro_plan_alloc(XiFunc *f, XiCoroPlan *plan, uint32_t count, uint32_t item_size) {
    if (count == 0)
        return NULL;
    if (item_size == 0 || count > UINT32_MAX / item_size)
        return NULL;
    uint32_t size = count * item_size;
    if (!plan || size > XI_CORO_MAX_PLAN_BYTES ||
        plan->planned_bytes > XI_CORO_MAX_PLAN_BYTES - size)
        return NULL;
    void *result = xi_func_arena_alloc(f, size);
    if (result) {
        memset(result, 0, size);
        plan->planned_bytes += size;
    }
    return result;
}

static XiBlock *xi_coro_block_at_rpo(const XiFunc *f, uint32_t rpo) {
    for (uint32_t i = 0; i < f->nblocks; i++) {
        if (f->blocks[i] && f->blocks[i]->rpo == rpo)
            return f->blocks[i];
    }
    return NULL;
}

static bool xi_coro_materialize_point_sets(XiFunc *f, XiCoroPlan *plan, const XiLiveness *live) {
    for (uint32_t si = 0; si < plan->nslots; si++) {
        plan->slots[si].frame_root = false;
        plan->slots[si].frame_release = false;
    }
    for (uint32_t pi = 0; pi < plan->nstates; pi++) {
        XiCoroSuspendPoint *point = &plan->points[pi];
        uint32_t nlive = 0;
        for (uint32_t si = 0; si < plan->nslots; si++) {
            const XiCoroSlot *slot = &plan->slots[si];
            if (!xi_coro_value_live_at_point(f, live, point->op, slot->value))
                continue;
            nlive++;
        }
        if (nlive > XI_CORO_MAX_FRAME_ACTIONS - plan->spill_count)
            return false;
        point->live = (XiValue **) xi_coro_plan_alloc(f, plan, plan->slot_capacity,
                                                      (uint32_t) sizeof(XiValue *));
        point->roots = (XiValue **) xi_coro_plan_alloc(f, plan, plan->slot_capacity,
                                                       (uint32_t) sizeof(XiValue *));
        point->drops = (XiValue **) xi_coro_plan_alloc(f, plan, plan->slot_capacity,
                                                       (uint32_t) sizeof(XiValue *));
        if ((plan->nslots && !point->live) || (plan->nslots && !point->roots) ||
            (plan->nslots && !point->drops))
            return false;
        for (uint32_t si = 0; si < plan->nslots; si++) {
            const XiCoroSlot *slot = &plan->slots[si];
            if (!xi_coro_value_live_at_point(f, live, point->op, slot->value))
                continue;
            point->live[point->nlive++] = slot->value;
            bool carries_owner =
                xi_coro_slot_carries_owner_at_point(f, live, point->op, slot, false);
            if (slot->is_root && carries_owner) {
                point->roots[point->nroots++] = slot->value;
                plan->slots[si].frame_root = true;
            }
            if (slot->needs_release && carries_owner) {
                point->drops[point->ndrops++] = slot->value;
                plan->slots[si].frame_release = true;
            }
        }
        plan->spill_count += point->nlive;
    }
    return true;
}

static bool xi_coro_value_live_at_any_split_point(const XiFunc *f, const XiLiveness *live,
                                                  const XiCoroPlan *plan, const XiValue *value) {
    for (uint32_t pi = 0; pi < plan->nstates; pi++) {
        if (xi_coro_value_live_at_split_point(f, live, plan->points[pi].op, value))
            return true;
    }
    return false;
}

static bool xi_coro_append_split_slots(XiFunc *f, XiCoroPlan *plan, const XiLiveness *live) {
    for (uint32_t rpo = 1; rpo <= f->nblocks; rpo++) {
        XiBlock *block = xi_coro_block_at_rpo(f, rpo);
        if (!block)
            continue;
        for (XiPhi *phi = block->phis; phi; phi = phi->next) {
            XiValue *value = &phi->value;
            if (xi_coro_plan_find_slot(plan, value) ||
                !xi_coro_value_live_at_any_split_point(f, live, plan, value))
                continue;
            if (plan->nslots >= plan->slot_capacity)
                return false;
            xi_coro_fill_slot(&plan->slots[plan->nslots++], f, value, XI_CORO_SLOT_PHI, live, NULL);
        }
        for (uint32_t vi = 0; vi < block->nvalues; vi++) {
            XiValue *value = block->values[vi];
            if (!value || value->op == XI_PARAM || xi_coro_plan_find_slot(plan, value) ||
                !xi_coro_value_live_at_any_split_point(f, live, plan, value))
                continue;
            if (plan->nslots >= plan->slot_capacity)
                return false;
            xi_coro_fill_slot(&plan->slots[plan->nslots++], f, value, XI_CORO_SLOT_VALUE, live,
                              NULL);
        }
    }
    return true;
}

XR_FUNC bool xi_coro_plan_ensure_slot_capacity(XiFunc *f, XiCoroPlan *plan) {
    if (!f || !plan)
        return false;
    uint32_t capacity = f->nparams;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        XiBlock *block = f->blocks[bi];
        for (XiPhi *phi = block ? block->phis : NULL; phi; phi = phi->next) {
            if (capacity == XI_CORO_MAX_SLOTS)
                return false;
            capacity++;
        }
        for (uint32_t vi = 0; block && vi < block->nvalues; vi++) {
            if (block->values[vi] && block->values[vi]->op != XI_PARAM) {
                if (capacity == XI_CORO_MAX_SLOTS)
                    return false;
                capacity++;
            }
        }
    }
    if (capacity <= plan->slot_capacity)
        return true;

    uint64_t slot_bytes = (uint64_t) capacity * sizeof(XiCoroSlot);
    uint64_t set_bytes = (uint64_t) plan->nstates * 3u * capacity * sizeof(XiValue *);
    uint64_t total_bytes = slot_bytes + set_bytes;
    if (slot_bytes > UINT32_MAX || plan->planned_bytes > XI_CORO_MAX_PLAN_BYTES ||
        total_bytes > XI_CORO_MAX_PLAN_BYTES ||
        total_bytes > XI_CORO_MAX_PLAN_BYTES - plan->planned_bytes)
        return false;

    XiCoroSlot *slots = (XiCoroSlot *) xi_func_arena_alloc(f, (uint32_t) slot_bytes);
    XiValue ***sets = (XiValue ***) xr_calloc((size_t) plan->nstates * 3u, sizeof(XiValue **));
    if (!slots || (plan->nstates && !sets)) {
        xr_free(sets);
        return false;
    }
    memset(slots, 0, (size_t) slot_bytes);
    if (plan->nslots)
        memcpy(slots, plan->slots, (size_t) plan->nslots * sizeof(XiCoroSlot));
    bool ok = true;
    uint32_t set_size = capacity * (uint32_t) sizeof(XiValue *);
    for (uint32_t i = 0; i < plan->nstates * 3u; i++) {
        sets[i] = (XiValue **) xi_func_arena_alloc(f, set_size);
        if (!sets[i]) {
            ok = false;
            break;
        }
        memset(sets[i], 0, set_size);
    }
    if (!ok) {
        xr_free(sets);
        return false;
    }

    plan->slots = slots;
    for (uint32_t i = 0; i < plan->nstates; i++) {
        plan->points[i].live = sets[i * 3u];
        plan->points[i].roots = sets[i * 3u + 1u];
        plan->points[i].drops = sets[i * 3u + 2u];
    }
    xr_free(sets);
    plan->slot_capacity = capacity;
    plan->planned_bytes += (uint32_t) total_bytes;
    return true;
}

XR_FUNC bool xi_coro_plan_refresh_point_sets(XiFunc *f, XiCoroPlan *plan) {
    if (!f || !plan || plan->nslots > plan->slot_capacity ||
        plan->slot_capacity > XI_CORO_MAX_SLOTS)
        return false;
    xi_ensure_rpo(f);
    XiLiveness *live = xi_compute_liveness(f);
    if (!live)
        return false;
    if (!xi_coro_plan_ensure_slot_capacity(f, plan)) {
        xi_liveness_free(live);
        return false;
    }
    if (!xi_coro_append_split_slots(f, plan, live)) {
        xi_liveness_free(live);
        return false;
    }
    for (uint32_t si = 0; si < plan->nslots; si++) {
        XiCoroSlotKind kind = (XiCoroSlotKind) plan->slots[si].kind;
        XiValue *value = plan->slots[si].value;
        xi_coro_fill_slot(&plan->slots[si], f, value, kind, live, NULL);
    }
    plan->spill_count = 0;
    plan->root_count = 0;
    plan->release_count = 0;
    for (uint32_t si = 0; si < plan->nslots; si++) {
        plan->slots[si].live_across = false;
        plan->slots[si].frame_root = false;
        plan->slots[si].frame_release = false;
    }
    for (uint32_t pi = 0; pi < plan->nstates; pi++) {
        XiCoroSuspendPoint *point = &plan->points[pi];
        point->nlive = point->nroots = point->ndrops = 0;
        for (uint32_t si = 0; si < plan->nslots; si++) {
            const XiCoroSlot *slot = &plan->slots[si];
            if (!xi_coro_value_live_at_split_point(f, live, point->op, slot->value))
                continue;
            point->live[point->nlive++] = slot->value;
            bool carries_owner =
                xi_coro_slot_carries_owner_at_point(f, live, point->op, slot, true);
            if (slot->is_root && carries_owner) {
                point->roots[point->nroots++] = slot->value;
                plan->slots[si].frame_root = true;
            }
            if (slot->needs_release && carries_owner) {
                point->drops[point->ndrops++] = slot->value;
                plan->slots[si].frame_release = true;
            }
            plan->slots[si].live_across = true;
        }
        if (point->nlive > XI_CORO_MAX_FRAME_ACTIONS - plan->spill_count) {
            xi_liveness_free(live);
            return false;
        }
        plan->spill_count += point->nlive;
    }
    for (uint32_t si = 0; si < plan->nslots; si++) {
        if (plan->slots[si].frame_root)
            plan->root_count++;
        if (plan->slots[si].frame_release)
            plan->release_count++;
    }
    xi_liveness_free(live);
    return true;
}

XR_FUNC XiCoroPlan *xi_coro_analyze(XiFunc *f, const XiCoroResolver *resolver) {
    if (!f)
        return NULL;
    if (f->coro_plan) {
        const XiCoroPlan *cached = f->coro_plan;
        bool current = cached->cfg_rewritten ? cached->lowered_ir_revision == f->ir_revision &&
                                                   cached->lowered_cfg_revision == f->cfg_version
                                             : cached->analyzed_ir_revision == f->ir_revision &&
                                                   cached->analyzed_cfg_revision == f->cfg_version;
        return current ? f->coro_plan : NULL;
    }
    if (!xi_coro_all_calls_resolved(f, resolver))
        return NULL;

    XiCoroPlan *plan = (XiCoroPlan *) xi_func_arena_alloc(f, (uint32_t) sizeof(XiCoroPlan));
    if (!plan)
        return NULL;
    memset(plan, 0, sizeof(*plan));
    plan->planned_bytes = (uint32_t) sizeof(*plan);
    plan->entry_block = f->entry;
    plan->needs_cl = f->ncaptures > 0;
    plan->ctx_depth = XI_CORO_RESOLVE_DEPTH_MAX;

    xi_ensure_rpo(f);
    XiLiveness *live = xi_compute_liveness(f);
    if (!live)
        return NULL;

    /* Pass 1: size the suspend-point and logical-slot arrays.  Every parameter
     * is a logical frame member; phis/values qualify via is_logical_member. */
    uint32_t npoints = 0;
    uint32_t nslots = f->nparams;
    uint32_t slot_capacity = f->nparams;
    if (nslots > XI_CORO_MAX_SLOTS || f->nblocks > XI_CORO_MAX_STATES * 3u) {
        xi_liveness_free(live);
        return NULL;
    }
    for (uint32_t rpo = 1; rpo <= f->nblocks; rpo++) {
        const XiBlock *blk = xi_coro_block_at_rpo(f, rpo);
        if (!blk)
            continue;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            slot_capacity++;
            if (xi_coro_value_is_logical_member(f, &phi->value, live, resolver))
                nslots++;
            if (nslots > XI_CORO_MAX_SLOTS || slot_capacity > XI_CORO_MAX_SLOTS) {
                xi_liveness_free(live);
                return NULL;
            }
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (xi_coro_is_suspend_point(f, v, resolver))
                npoints++;
            if (v->op != XI_PARAM) {
                slot_capacity++;
                if (xi_coro_value_is_logical_member(f, v, live, resolver))
                    nslots++;
            }
            if (npoints > XI_CORO_MAX_STATES || nslots > XI_CORO_MAX_SLOTS ||
                slot_capacity > XI_CORO_MAX_SLOTS) {
                xi_liveness_free(live);
                return NULL;
            }
        }
    }

    if (npoints > 0) {
        plan->points = (XiCoroSuspendPoint *) xi_coro_plan_alloc(
            f, plan, npoints, (uint32_t) sizeof(XiCoroSuspendPoint));
        if (!plan->points) {
            xi_liveness_free(live);
            return NULL;
        }
    }
    if (slot_capacity > 0) {
        plan->slots = (XiCoroSlot *) xi_coro_plan_alloc(f, plan, slot_capacity,
                                                        (uint32_t) sizeof(XiCoroSlot));
        if (!plan->slots) {
            xi_liveness_free(live);
            return NULL;
        }
    }

    /* Pass 2: materialize.  Slot order mirrors the logical frame layout:
     * parameters, then per-block phis and values. */
    uint32_t pi = 0, si = 0;
    for (uint16_t i = 0; i < f->nparams; i++) {
        if (plan->slots && f->params[i])
            xi_coro_fill_slot(&plan->slots[si], f, f->params[i], XI_CORO_SLOT_PARAM, live,
                              resolver);
        si++;
    }
    for (uint32_t rpo = 1; rpo <= f->nblocks; rpo++) {
        XiBlock *blk = xi_coro_block_at_rpo(f, rpo);
        if (!blk)
            continue;
        for (XiPhi *phi = blk->phis; phi; phi = phi->next) {
            if (!xi_coro_value_is_logical_member(f, &phi->value, live, resolver))
                continue;
            if (plan->slots)
                xi_coro_fill_slot(&plan->slots[si], f, &phi->value, XI_CORO_SLOT_PHI, live,
                                  resolver);
            si++;
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            XiValue *v = blk->values[vi];
            if (xi_coro_is_suspend_point(f, v, resolver)) {
                if (plan->points) {
                    XiCoroSuspendPoint *pt = &plan->points[pi];
                    pt->state_id = pi + 1; /* dense, 1-based */
                    pt->op = v;
                    pt->kind = xi_coro_suspend_kind(f, v, resolver);
                }
                pi++;
            }
            if (v->op != XI_PARAM && xi_coro_value_is_logical_member(f, v, live, resolver)) {
                if (plan->slots)
                    xi_coro_fill_slot(&plan->slots[si], f, v, XI_CORO_SLOT_VALUE, live, resolver);
                si++;
            }
        }
    }

    plan->nstates = npoints;
    plan->nslots = si;
    plan->slot_capacity = slot_capacity;
    plan->is_coroutine = npoints > 0;
    if (!xi_coro_materialize_point_sets(f, plan, live)) {
        xi_liveness_free(live);
        return NULL;
    }

    /* Logical frame root / release counts (backend-neutral; a backend may shed
     * some after applying its physical storage test). */
    for (uint32_t i = 0; i < plan->nslots; i++) {
        if (plan->slots[i].frame_root)
            plan->root_count++;
        if (plan->slots[i].frame_release)
            plan->release_count++;
    }

    xi_liveness_free(live);
    plan->analyzed_ir_revision = f->ir_revision;
    plan->analyzed_cfg_revision = f->cfg_version;
    plan->analysis_complete = true;
    f->coro_plan = plan;
    return plan;
}
