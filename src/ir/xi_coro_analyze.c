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
#include "xi_own.h"
#include "xi_ops_gen.h"
#include "../base/xglobal_indices.h"
#include "../runtime/value/xtype.h"
#include <string.h>

/* Machine representation set by xi_opt_select_rep (an IR field); mirrors the
 * AOT cg_rep() reader so the analysis can reason about physical slot kinds. */
static XrRep xi_coro_rep(const XiValue *v) {
    return v ? (XrRep) v->rep : XR_REP_TAGGED;
}

/* Interprocedural recursion bound for suspendability (matches the historic
 * AOT depth limit; deeper call graphs conservatively report non-suspendable). */
#define XI_CORO_RESOLVE_DEPTH_MAX 8

/* ========== Op classifier ========== */

XR_FUNC bool xi_op_is_coroutine(uint16_t op) {
    return xi_generated_op_class(op) == XI_GEN_CLASS_COROUTINE;
}

/* ========== Concurrency method-call recognizers ========== */

XR_FUNC bool xi_value_is_channel_method_call(const XiValue *v, const char *method, int nargs) {
    if (!v || v->op != XI_CALL_METHOD || v->nargs < 1)
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
            (strcmp(method, "recv") == 0 && v->nargs == 1));
}

XR_FUNC bool xi_value_is_task_method_call(const XiValue *v, const char *method, int nargs) {
    if (!v || v->op != XI_CALL_METHOD || v->nargs < 1 || !xi_value_type_is_task(v->args[0]))
        return false;
    const char *actual = (const char *) v->aux;
    if (!actual || strcmp(actual, method) != 0)
        return false;
    return nargs < 0 || (int) v->nargs - 1 == nargs;
}

XR_FUNC bool xi_value_is_work_queue_method_call(const XiValue *v, const char *method, int nargs) {
    if (!v || v->op != XI_CALL_METHOD || v->nargs < 1 || !xi_value_type_is_work_queue(v->args[0]))
        return false;
    const char *actual = (const char *) v->aux;
    if (!actual || strcmp(actual, method) != 0)
        return false;
    return nargs < 0 || (int) v->nargs - 1 == nargs;
}

XR_FUNC bool xi_value_is_result_group_method_call(const XiValue *v, const char *method, int nargs) {
    if (!v || v->op != XI_CALL_METHOD || v->nargs < 1 || !xi_value_type_is_result_group(v->args[0]))
        return false;
    const char *actual = (const char *) v->aux;
    if (!actual || strcmp(actual, method) != 0)
        return false;
    return nargs < 0 || (int) v->nargs - 1 == nargs;
}

static bool xi_value_is_blocking_channel_method_call(const XiValue *v) {
    return xi_value_is_channel_method_call(v, "send", 1) ||
           xi_value_is_channel_method_call(v, "sendTimeout", 2) ||
           xi_value_is_channel_method_call(v, "recv", 0) ||
           xi_value_is_channel_method_call(v, "recvTimeout", 1);
}

XR_FUNC bool xi_value_is_blocking_task_method_call(const XiValue *v) {
    return xi_value_is_task_method_call(v, "awaitResult", 0) ||
           xi_value_is_task_method_call(v, "awaitTimeout", 1);
}

XR_FUNC bool xi_value_is_blocking_work_queue_method_call(const XiValue *v) {
    return xi_value_is_work_queue_method_call(v, "pop", 0) ||
           xi_value_is_work_queue_method_call(v, "pop", 1);
}

XR_FUNC bool xi_value_is_blocking_result_group_method_call(const XiValue *v) {
    return xi_value_is_result_group_method_call(v, "recv", 0);
}

/* ========== Intrinsic suspendability predicates (intraprocedural) ========== */

/* A channel send/recv method whose blocking variant requires a coroutine
 * context, also accepting the legacy unknown-typed send/recv shapes. */
static bool xi_channel_method_may_suspend(const XiValue *v) {
    if (!v || v->op != XI_CALL_METHOD || v->nargs < 1)
        return false;
    const char *method = (const char *) v->aux;
    if (!method)
        return false;
    bool blocking_channel_method = strcmp(method, "send") == 0 || strcmp(method, "recv") == 0 ||
                                   strcmp(method, "sendTimeout") == 0 ||
                                   strcmp(method, "recvTimeout") == 0;
    if (!blocking_channel_method)
        return false;
    if (xi_value_type_is_channel(v->args[0]))
        return true;
    return xi_value_type_is_unknown(v->args[0]) &&
           ((strcmp(method, "send") == 0 && v->nargs == 2) ||
            (strcmp(method, "recv") == 0 && v->nargs == 1));
}

static bool xi_work_queue_method_needs_coro(const XiValue *v) {
    if (!v || v->op != XI_CALL_METHOD || v->nargs < 1 || !xi_value_type_is_work_queue(v->args[0]))
        return false;
    const char *method = (const char *) v->aux;
    return method && strcmp(method, "pop") == 0;
}

static bool xi_result_group_method_needs_coro(const XiValue *v) {
    return xi_value_is_blocking_result_group_method_call(v);
}

static bool xi_work_queue_constructor_needs_coro(const XiValue *v) {
    if (!v || v->op != XI_CALL || v->nargs < 1)
        return false;
    const XiValue *callee = v->args[0];
    while (callee && (callee->op == XI_BOX || callee->op == XI_UNBOX || callee->op == XI_COPY) &&
           callee->nargs >= 1) {
        callee = callee->args[0];
    }
    return callee && callee->op == XI_GET_BUILTIN && callee->aux_int == XR_GLOBAL_VAR_WORKQUEUE;
}

/* `time.sleep(...)` recognized via the resolver's module-import query so the
 * analysis never depends on the backend's import-resolution internals. */
static bool xi_coro_is_time_sleep_call(const XiFunc *f, const XiValue *v,
                                       const XiCoroResolver *resolver) {
    if (!v || v->op != XI_CALL_METHOD || v->nargs != 2)
        return false;
    const char *method = (const char *) v->aux;
    if (!method || strcmp(method, "sleep") != 0)
        return false;
    return resolver && resolver->value_is_module_import &&
           resolver->value_is_module_import(resolver->ud, f, v->args[0], "time");
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
            if (xi_op_is_coroutine(v->op))
                return true;
            if (xi_channel_method_may_suspend(v) || xi_work_queue_method_needs_coro(v) ||
                xi_result_group_method_needs_coro(v) || xi_work_queue_constructor_needs_coro(v))
                return true;
            if (xi_coro_is_time_sleep_call(f, v, resolver))
                return true;
        }
    }
    return false;
}

static bool xi_coro_func_is_suspendable_depth(const XiFunc *f, const XiCoroResolver *resolver,
                                              int depth) {
    if (xi_coro_func_intrinsic_suspends(f, resolver))
        return true;
    if (!f || !resolver || depth >= XI_CORO_RESOLVE_DEPTH_MAX)
        return false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!v)
                continue;
            const XiFunc *target = NULL;
            if (v->op == XI_CALL && v->nargs >= 1 && resolver->resolve_callee) {
                target = resolver->resolve_callee(resolver->ud, f, v->args[0]);
            } else if ((v->op == XI_CALL_METHOD || v->op == XI_CALL_METHOD_DIRECT) &&
                       v->nargs >= 1 && resolver->resolve_method) {
                target = resolver->resolve_method(resolver->ud, f, v);
            }
            if (!target || target == f)
                continue;
            if (xi_coro_func_is_suspendable_depth(target, resolver, depth + 1))
                return true;
        }
    }
    return false;
}

XR_FUNC bool xi_coro_func_is_suspendable(const XiFunc *f, const XiCoroResolver *resolver) {
    return xi_coro_func_is_suspendable_depth(f, resolver, 0);
}

/* A direct call whose resolved target is (transitively) suspendable is itself
 * a suspension site in the caller. */
static bool xi_coro_call_suspends(const XiFunc *f, const XiValue *v,
                                  const XiCoroResolver *resolver) {
    if (!v || v->op != XI_CALL || v->nargs < 1)
        return false;
    if (!resolver || !resolver->resolve_callee)
        return false;
    const XiFunc *target = resolver->resolve_callee(resolver->ud, f, v->args[0]);
    return target && xi_coro_func_is_suspendable(target, resolver);
}

/* A statically resolved method call whose target is (transitively)
 * suspendable is also a suspension site in the caller. */
static bool xi_coro_method_call_suspends(const XiFunc *f, const XiValue *v,
                                         const XiCoroResolver *resolver) {
    if (!v || (v->op != XI_CALL_METHOD && v->op != XI_CALL_METHOD_DIRECT) || v->nargs < 1)
        return false;
    if (!resolver || !resolver->resolve_method)
        return false;
    const XiFunc *target = resolver->resolve_method(resolver->ud, f, v);
    return target && xi_coro_func_is_suspendable(target, resolver);
}

/* ========== Suspension-point predicate ========== */

XR_FUNC bool xi_coro_is_suspend_point(const XiFunc *f, const XiValue *v,
                                      const XiCoroResolver *resolver) {
    if (!v)
        return false;
    if (v->op == XI_YIELD || v->op == XI_GO || v->op == XI_AWAIT || v->op == XI_CHAN_SEND ||
        v->op == XI_CHAN_RECV || v->op == XI_CHAN_TRY_SEND || v->op == XI_CHAN_TRY_RECV ||
        v->op == XI_SELECT_BLOCK || v->op == XI_SCOPE_EXIT)
        return true;
    if (xi_value_is_channel_method_call(v, "trySend", 1) ||
        xi_value_is_channel_method_call(v, "tryRecv", 0))
        return true;
    if (xi_value_is_blocking_channel_method_call(v) || xi_value_is_blocking_task_method_call(v) ||
        xi_value_is_blocking_work_queue_method_call(v) ||
        xi_value_is_blocking_result_group_method_call(v))
        return true;
    if (xi_coro_is_time_sleep_call(f, v, resolver))
        return true;
    return xi_coro_call_suspends(f, v, resolver) || xi_coro_method_call_suspends(f, v, resolver);
}

/* ========== Typed recv/await slot-reuse recognizers ========== */

static bool xi_coro_is_channel_recv_value(const XiValue *v) {
    if (!v)
        return false;
    if (v->op == XI_CHAN_RECV || v->op == XI_CHAN_TRY_RECV)
        return true;
    if (v->op != XI_CALL_METHOD || v->nargs < 1 || !xi_value_type_is_channel(v->args[0]))
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
                 xi_value_is_channel_method_call(v, "recvTimeout", 1) ||
                 xi_value_is_blocking_work_queue_method_call(v) ||
                 xi_value_is_blocking_result_group_method_call(v) ||
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

static bool xi_coro_block_uses_target_after(const XiBlock *blk, uint32_t start,
                                            const XiValue *target) {
    for (uint32_t vi = start; vi < blk->nvalues; vi++) {
        if (xi_coro_value_uses_target(blk->values[vi], target))
            return true;
    }
    return blk->control == target;
}

XR_FUNC bool xi_coro_value_live_across_suspend(const XiFunc *f, const XiLiveness *live,
                                               const XiValue *target,
                                               const XiCoroResolver *resolver) {
    if (!f || !live || !target)
        return false;

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
                continue;
            }
            if (!available || !xi_coro_is_suspend_point(f, v, resolver))
                continue;
            if (xi_is_live_out(live, blk, target) ||
                xi_coro_block_uses_target_after(blk, vi + 1, target))
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
    if (v->op == XI_GO)
        return true;
    return xi_coro_value_live_across_suspend(f, live, v, resolver);
}

/* ========== Slot attributes ========== */

XR_FUNC const XiValue *xi_coro_release_origin(const XiValue *v) {
    const XiValue *cur = v;
    for (int depth = 0; cur && depth < 8; depth++) {
        if ((cur->op == XI_COPY || cur->op == XI_MOVE || cur->op == XI_BOX ||
             cur->op == XI_UNBOX) &&
            cur->nargs >= 1) {
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
        case XR_KIND_MAP:
        case XR_KIND_SET:
        case XR_KIND_FIXED_ARRAY:
        case XR_KIND_JSON:
            return true;
        case XR_KIND_INSTANCE:
            return type->instance.class_name &&
                   strcmp(type->instance.class_name, "StringBuilder") == 0;
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
    if (v->op == XI_CHAN_TRY_SEND)
        return XI_CORO_SUSP_CHAN_SEND;
    if (v->op == XI_CHAN_TRY_RECV)
        return XI_CORO_SUSP_CHAN_RECV;
    if (v->op == XI_SELECT_BLOCK)
        return XI_CORO_SUSP_SELECT;
    if (v->op == XI_SCOPE_EXIT)
        return XI_CORO_SUSP_SCOPE_EXIT;
    if (xi_value_is_channel_method_call(v, "trySend", 1))
        return XI_CORO_SUSP_CHAN_SEND;
    if (xi_value_is_channel_method_call(v, "tryRecv", 0))
        return XI_CORO_SUSP_CHAN_RECV;
    if (xi_value_is_channel_method_call(v, "send", 1) ||
        xi_value_is_channel_method_call(v, "sendTimeout", 2))
        return XI_CORO_SUSP_CHAN_SEND;
    if (xi_value_is_channel_method_call(v, "recv", 0) ||
        xi_value_is_channel_method_call(v, "recvTimeout", 1))
        return XI_CORO_SUSP_CHAN_RECV;
    if (xi_value_is_blocking_task_method_call(v) ||
        xi_value_is_blocking_work_queue_method_call(v) ||
        xi_value_is_blocking_result_group_method_call(v))
        return XI_CORO_SUSP_AWAIT;
    if (xi_coro_is_time_sleep_call(f, v, resolver))
        return XI_CORO_SUSP_SLEEP;
    return XI_CORO_SUSP_CALL;
}

/* A logical-frame value can hold a GC root if it (or the runtime) keeps a live
 * reference across the suspend: a go handle, a live-across value, a runtime
 * result slot, or an await-aggregate element.  Typed recv/await unbox reuse and
 * paired recv-status do not, even though they are frame members. */
static bool xi_coro_slot_value_may_hold_root(const XiFunc *f, const XiValue *v, bool live_across) {
    return (v && v->op == XI_GO) || live_across || xi_coro_value_needs_runtime_slot(v) ||
           xi_coro_value_is_aggregate_await_tasks(f, v);
}

static void xi_coro_fill_slot(XiCoroSlot *slot, const XiFunc *f, XiValue *v, XiCoroSlotKind kind,
                              const XiLiveness *live, const XiCoroResolver *resolver) {
    slot->value = v;
    slot->type = v->type;
    slot->logical_rep = (uint8_t) xi_coro_rep(v);
    slot->kind = (uint8_t) kind;
    slot->is_root = xi_coro_value_rep_can_trace_root(v);
    slot->needs_release = xi_coro_value_needs_arc_release(v);
    slot->needs_runtime_slot = xi_coro_value_needs_runtime_slot(v);
    slot->needs_boundary_clone = xi_coro_value_needs_boundary_clone(v);
    slot->live_across = xi_coro_value_live_across_suspend(f, live, v, resolver);
    /* Parameters and phis are frame roots only when live across a suspend; a
     * block value may also hold a root via its runtime/await origin. */
    bool root_reachable = kind == XI_CORO_SLOT_VALUE
                              ? xi_coro_slot_value_may_hold_root(f, v, slot->live_across)
                              : slot->live_across;
    slot->frame_root = slot->is_root && root_reachable;
    slot->frame_release = slot->needs_release && slot->live_across;
}

XR_FUNC XiCoroPlan *xi_coro_analyze(XiFunc *f, const XiCoroResolver *resolver) {
    if (!f)
        return NULL;
    if (f->coro_plan)
        return f->coro_plan;

    XiCoroPlan *plan = (XiCoroPlan *) xi_func_arena_alloc(f, (uint32_t) sizeof(XiCoroPlan));
    if (!plan)
        return NULL;
    memset(plan, 0, sizeof(*plan));
    plan->needs_cl = f->ncaptures > 0;
    plan->ctx_depth = XI_CORO_RESOLVE_DEPTH_MAX;

    xi_ensure_rpo(f);
    XiLiveness *live = xi_compute_liveness(f);
    if (!live) {
        /* Degrade to an empty plan; callers fall back to direct predicates. */
        f->coro_plan = plan;
        return plan;
    }

    /* Pass 1: size the suspend-point and logical-slot arrays.  Every parameter
     * is a logical frame member; phis/values qualify via is_logical_member. */
    uint32_t npoints = 0;
    uint32_t nslots = f->nparams;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            if (xi_coro_value_is_logical_member(f, &phi->value, live, resolver))
                nslots++;
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (xi_coro_is_suspend_point(f, v, resolver))
                npoints++;
            if (xi_coro_value_is_logical_member(f, v, live, resolver))
                nslots++;
        }
    }

    if (npoints > 0)
        plan->points = (XiCoroSuspendPoint *) xi_func_arena_alloc(
            f, (uint32_t) (npoints * sizeof(XiCoroSuspendPoint)));
    if (nslots > 0)
        plan->slots =
            (XiCoroSlot *) xi_func_arena_alloc(f, (uint32_t) (nslots * sizeof(XiCoroSlot)));

    /* Pass 2: materialize.  Slot order mirrors the backend frame layout
     * (parameters, then per-block phis and values).  Per-point live sets are
     * left empty here; the CFG-lowering pass fills them when it splits blocks. */
    uint32_t pi = 0, si = 0;
    for (uint16_t i = 0; i < f->nparams; i++) {
        if (plan->slots && f->params[i])
            xi_coro_fill_slot(&plan->slots[si], f, f->params[i], XI_CORO_SLOT_PARAM, live,
                              resolver);
        si++;
    }
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        XiBlock *blk = f->blocks[bi];
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
                    pt->live = NULL;
                    pt->nlive = 0;
                }
                pi++;
            }
            if (xi_coro_value_is_logical_member(f, v, live, resolver)) {
                if (plan->slots)
                    xi_coro_fill_slot(&plan->slots[si], f, v, XI_CORO_SLOT_VALUE, live, resolver);
                si++;
            }
        }
    }

    plan->nstates = npoints;
    plan->nslots = si;
    plan->is_coroutine = npoints > 0;
    /* Logical frame root / release counts (backend-neutral; a backend may shed
     * some after applying its physical storage test). */
    for (uint32_t i = 0; i < plan->nslots; i++) {
        if (plan->slots[i].frame_root)
            plan->root_count++;
        if (plan->slots[i].frame_release)
            plan->release_count++;
    }

    xi_liveness_free(live);
    f->coro_plan = plan;
    return plan;
}
