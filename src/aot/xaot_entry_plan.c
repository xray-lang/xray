/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 */

#include "xaot_entry_plan.h"
#include "xaot_bundle.h"
#include "../base/xmalloc.h"
#include <string.h>

static uint32_t xaot_freestanding_core_capabilities(void) {
    const uint32_t hosted_only = XG_CAP_COROUTINE | XG_CAP_CHANNEL | XG_CAP_SYS_THREAD |
                                 XG_CAP_SCOPE | XG_CAP_TIMER | XG_CAP_NETPOLL | XG_CAP_TASK |
                                 XG_CAP_WORK_QUEUE | XG_CAP_RESULT_GROUP | XG_CAP_COUNTDOWN_LATCH |
                                 XG_CAP_SEMAPHORE | XG_CAP_EVENT_COUNT | XG_CAP_GENERATOR |
                                 XG_CAP_PARALLEL;
    return UINT32_MAX & ~hosted_only;
}

bool xaot_bundle_uses_parallel_intrinsic(const XaotBundle *bundle) {
    if (!bundle)
        return false;
    for (uint32_t fi = 0; fi < bundle->nfunc_plans; fi++) {
        const XaotFuncPlan *plan = &bundle->func_plans[fi];
        const XiFunc *func = plan->func;
        if (bundle->has_callable_reachability && !plan->reachable)
            continue;
        if (!func)
            continue;
        for (uint32_t bi = 0; bi < func->nblocks; bi++) {
            const XiBlock *block = func->blocks[bi];
            if (!block)
                continue;
            for (uint32_t vi = 0; vi < block->nvalues; vi++) {
                const XiValue *value = block->values[vi];
                if (value && (value->op == XI_PAR_FOR || value->op == XI_PAR_MAP ||
                              value->op == XI_PAR_REDUCE))
                    return true;
            }
        }
    }
    return false;
}

uint32_t xaot_entry_plan_required_provider_hooks(const XrEntryPlan *plan) {
    uint32_t hooks = 0;
    uint32_t caps;
    if (!plan)
        return 0;
    caps = plan->required_capability_bits;
    if ((caps & (XR_CAP_COROUTINE | XR_CAP_TASK | XR_CAP_CHANNEL | XR_CAP_SCOPE | XR_CAP_GENERATOR |
                 XR_CAP_PARALLEL | XR_CAP_SYS_THREAD)) != 0)
        hooks |= XAOT_PROVIDER_HOOK_TASK_ALLOC;
    if ((plan->reachable_effect_bits & XR_EFFECT_MAY_SPAWN) != 0 ||
        (caps & (XR_CAP_PARALLEL | XR_CAP_SYS_THREAD)) != 0)
        hooks |= XAOT_PROVIDER_HOOK_SUBMIT;
    if (plan->root_representation != XR_ROOT_ELIDED ||
        (caps & (XR_CAP_TASK | XR_CAP_CHANNEL | XR_CAP_SCOPE | XR_CAP_TIMER)) != 0)
        hooks |= XAOT_PROVIDER_HOOK_PARK_WAKE;
    if ((caps & XR_CAP_TIMER) != 0)
        hooks |= XAOT_PROVIDER_HOOK_TIMER;
    if (plan->root_representation != XR_ROOT_ELIDED)
        hooks |= XAOT_PROVIDER_HOOK_EXECUTOR_PUMP;
    return hooks;
}

static XgFuncId entry_func_id(const XaotBundle *bundle, const XgGlobalEvidence *evidence) {
    const XiModule *entry;
    XgFuncId fallback = XG_NO_ID;
    if (!bundle || !evidence || bundle->entry_module >= bundle->nmodules)
        return XG_NO_ID;
    entry = bundle->modules[bundle->entry_module];
    if (entry && entry->init && entry->init->xg_body_func_id != XG_NO_ID)
        return entry->init->xg_body_func_id;
    for (uint32_t i = 0; i < evidence->nbodies; i++) {
        const XgBodySummary *body = &evidence->bodies[i];
        if (body->kind == XG_BODY_MODULE_INIT && body->module_id == bundle->entry_module + 1)
            return body->func_id;
        if (body->kind == XG_BODY_MODULE_INIT)
            fallback = body->func_id;
    }
    /* Package payload import can remap module ids independently of Xi's topo
     * index.  Executable entry is the final module-init root in topo/evidence
     * order; imported modules precede it. */
    return fallback;
}

static bool entry_root_uses_resumable_frame(const XaotBundle *bundle, XgFuncId root,
                                            uint32_t reachable_effect_bits) {
    const XiFunc *func = xaot_bundle_find_body_func(bundle, root, NULL);
    const XaotFuncPlan *plan = xaot_bundle_find_func_plan(bundle, func);
    if (plan)
        return plan->may_suspend;
    /* Global-evidence installation precedes function-plan preparation. Keep
     * that early plan conservative; prepare refreshes it from the exact root
     * execution shape once all callable plans have converged. */
    return (reachable_effect_bits & XR_EFFECT_MAY_SUSPEND) != 0;
}

bool xaot_entry_plan_derive(const XaotBundle *bundle, const XgGlobalEvidence *evidence,
                            uint32_t profile, XrEntryPlan *out) {
    uint8_t *reachable;
    bool has_root = false;
    bool prepared_reachability;
    uint32_t provided;
    XgFuncId root;
    if (!bundle || !evidence || !out)
        return false;
    memset(out, 0, sizeof(*out));
    root = entry_func_id(bundle, evidence);
    out->entry_func_id = root;
    if (root == XG_NO_ID) {
        for (uint32_t di = 0; di < evidence->ndecls && !has_root; di++)
            has_root = (evidence->decls[di].flags & XG_DECL_C_EXPORT) != 0;
    } else {
        has_root = true;
    }
    if (!has_root) {
        /* Plan-only unit bundles and libraries may intentionally have no
         * executable root.  Their exact requirement set is empty and the
         * physical root is therefore proven elided. */
        out->provided_capability_bits =
            profile == XG_BUILD_FREESTANDING ? xaot_freestanding_core_capabilities() : UINT32_MAX;
        out->evidence = XR_ENTRY_EV_CLOSED_WORLD_REACHABILITY | XR_ENTRY_EV_TARGET_PROVIDER;
        if (bundle->nmodules != 0)
            out->evidence |= XR_ENTRY_EV_ARTIFACT_ROOT_SET;
        return true;
    }
    reachable = (uint8_t *) xr_calloc(evidence->nbodies ? evidence->nbodies : 1, sizeof(uint8_t));
    if (!reachable) {
        return false;
    }
    prepared_reachability = bundle->has_callable_reachability;
    if (!prepared_reachability) {
        if (root != XG_NO_ID && !xg_body_reachability_mark_closed_world_calls(
                                    evidence, root, reachable, evidence->nbodies)) {
            out->unproven_reason = XR_ENTRY_OPEN_REACHABILITY;
            xr_free(reachable);
            return true;
        }
        /* Every imported module initializer executes before the entry module.
         * Treating only the final entry initializer as a root can omit runtime
         * capabilities used by imported global initializers. */
        for (uint32_t bi = 0; bi < evidence->nbodies; bi++) {
            const XgBodySummary *body = &evidence->bodies[bi];
            if (body->kind != XG_BODY_MODULE_INIT || body->func_id == root)
                continue;
            if (!xg_body_reachability_mark_closed_world_calls(evidence, body->func_id, reachable,
                                                              evidence->nbodies)) {
                out->unproven_reason = XR_ENTRY_OPEN_REACHABILITY;
                xr_free(reachable);
                return true;
            }
        }
        for (uint32_t di = 0; di < evidence->ndecls; di++) {
            const XgDeclSummary *decl = &evidence->decls[di];
            if ((decl->flags & XG_DECL_C_EXPORT) == 0)
                continue;
            for (uint32_t bi = 0; bi < evidence->nbodies; bi++) {
                if (evidence->bodies[bi].owner_decl_id != decl->decl_id)
                    continue;
                if (!xg_body_reachability_mark_closed_world_calls(
                        evidence, evidence->bodies[bi].func_id, reachable, evidence->nbodies)) {
                    out->unproven_reason = XR_ENTRY_OPEN_REACHABILITY;
                    xr_free(reachable);
                    return true;
                }
            }
        }
        /* Pre-prepare callers can still attach verified callable target rows.
         * Close those over the source graph. Once function plans exist, their
         * final Xi reachability below replaces this conservative source view. */
        {
            bool changed = true;
            while (changed) {
                changed = false;
                for (uint32_t pi = 0; pi < bundle->ncallable_invoke_plans; pi++) {
                    const XaotCallableInvokePlan *plan = &bundle->callable_invoke_plans[pi];
                    XgFuncId owner_id = plan->owner ? plan->owner->xg_body_func_id : XG_NO_ID;
                    bool owner_reachable = false;
                    for (uint32_t bi = 0; bi < evidence->nbodies; bi++) {
                        if (evidence->bodies[bi].func_id == owner_id && reachable[bi]) {
                            owner_reachable = true;
                            break;
                        }
                    }
                    if (!owner_reachable)
                        continue;
                    for (uint16_t ti = 0; ti < plan->target_count; ti++) {
                        const XaotCallableTargetCase *target =
                            xaot_bundle_callable_target_case(bundle, plan, ti);
                        XgFuncId target_id = target && target->target_func
                                                 ? target->target_func->xg_body_func_id
                                                 : XG_NO_ID;
                        for (uint32_t bi = 0; bi < evidence->nbodies; bi++) {
                            if (target_id != XG_NO_ID &&
                                evidence->bodies[bi].func_id == target_id && !reachable[bi]) {
                                reachable[bi] = 1;
                                changed = true;
                            }
                        }
                    }
                }
            }
        }
    }
    /* Final Xi function reachability is authoritative after prepare. Source
     * method target sets deliberately over-approximate open class hierarchies
     * and must not reintroduce dead coroutine bodies into an executable. */
    for (uint32_t fi = 0; fi < bundle->nfunc_plans; fi++) {
        const XaotFuncPlan *func_plan = &bundle->func_plans[fi];
        XgFuncId func_id = func_plan->func ? func_plan->func->xg_body_func_id : XG_NO_ID;
        if (!func_plan->reachable || func_id == XG_NO_ID)
            continue;
        for (uint32_t bi = 0; bi < evidence->nbodies; bi++) {
            if (evidence->bodies[bi].func_id == func_id) {
                reachable[bi] = 1;
                break;
            }
        }
    }
    for (uint32_t i = 0; i < evidence->nbodies; i++) {
        const XgBodySummary *body;
        uint32_t effect_bits;
        if (!reachable[i])
            continue;
        body = &evidence->bodies[i];
        effect_bits = body->effect_bits;
        if (!prepared_reachability &&
            !xg_body_effects_compose_closed_world_calls(evidence, body, &effect_bits)) {
            out->unproven_reason = XR_ENTRY_OPEN_REACHABILITY;
            xr_free(reachable);
            return true;
        }
        out->reachable_body_count++;
        out->reachable_effect_bits |= effect_bits;
        /* Function-value convergence can prove transitive suspension that the
         * source-summary call graph could not close (for example, a direct
         * caller of a variadic suspendable function).  The prepared function
         * execution shape is authoritative for runtime selection too. */
        const XiFunc *func = xaot_bundle_find_body_func(bundle, body->func_id, NULL);
        const XaotFuncPlan *func_plan = xaot_bundle_find_func_plan(bundle, func);
        if (func_plan && func_plan->may_suspend)
            out->reachable_effect_bits |= XG_BODY_MAY_SUSPEND;
        out->required_capability_bits |= body->capability_bits;
    }
    for (uint32_t pi = 0; pi < bundle->ncallable_invoke_plans; pi++) {
        const XaotCallableInvokePlan *plan = &bundle->callable_invoke_plans[pi];
        XgFuncId owner_id = plan->owner ? plan->owner->xg_body_func_id : XG_NO_ID;
        bool owner_reachable = false;
        for (uint32_t bi = 0; bi < evidence->nbodies; bi++) {
            if (evidence->bodies[bi].func_id == owner_id && reachable[bi]) {
                owner_reachable = true;
                break;
            }
        }
        if (owner_reachable)
            out->reachable_effect_bits |= plan->effect_bits;
    }
    if ((out->reachable_effect_bits & XR_EFFECT_MAY_SUSPEND) != 0)
        out->required_capability_bits |= XG_CAP_COROUTINE;
    xr_free(reachable);

    /* The parallel capability is not carried by any body's summary bits; the
     * prepared IR is where it is visible.  Record it here so every consumer --
     * the scheduler mode below, the C emitter's runtime-bridge decision, and
     * the link feature set -- reads it from one place. */
    if (xaot_bundle_uses_parallel_intrinsic(bundle))
        out->required_capability_bits |= XG_CAP_PARALLEL;

    out->runtime_component_bits = out->required_capability_bits;
    if (entry_root_uses_resumable_frame(bundle, root, out->reachable_effect_bits))
        out->root_representation = XR_ROOT_RESUMABLE_FRAME;
    else if ((out->reachable_effect_bits & (XR_EFFECT_MAY_SPAWN | XR_EFFECT_OBSERVES_TASK_ID)) != 0)
        out->root_representation = XR_ROOT_DESCRIPTOR;
    else
        out->root_representation = XR_ROOT_ELIDED;

    if ((out->required_capability_bits & (XG_CAP_SYS_THREAD | XG_CAP_PARALLEL)) != 0)
        out->scheduler_mode = XR_SCHED_MULTI;
    else if ((out->required_capability_bits &
              (XG_CAP_COROUTINE | XG_CAP_TASK | XG_CAP_CHANNEL | XG_CAP_SCOPE)) != 0)
        out->scheduler_mode = XR_SCHED_SINGLE;
    else
        out->scheduler_mode = XR_SCHED_NONE;

    if (profile != XG_BUILD_FREESTANDING) {
        provided = UINT32_MAX;
        out->provider_hook_bits = UINT32_MAX;
    } else if (bundle->target_provider.abi_version != 0) {
        if (bundle->target_provider.abi_version != XAOT_PROVIDER_ABI_VERSION) {
            out->unproven_reason = XR_ENTRY_PROVIDER_ABI;
            return true;
        }
        provided = xaot_freestanding_core_capabilities() |
                   bundle->target_provider.provided_capability_bits;
    } else {
        provided = xaot_freestanding_core_capabilities();
    }
    out->provided_capability_bits = provided;
    out->evidence = XR_ENTRY_EV_GLOBAL_BODY | XR_ENTRY_EV_CLOSED_WORLD_REACHABILITY |
                    XR_ENTRY_EV_ROOT_EFFECT | XR_ENTRY_EV_TARGET_PROVIDER;
    if (bundle->nmodules != 0)
        out->evidence |= XR_ENTRY_EV_ARTIFACT_ROOT_SET;
    if ((out->required_capability_bits & ~provided) != 0)
        out->unproven_reason = XR_ENTRY_MISSING_CAPABILITY;
    out->provider_hook_bits = xaot_entry_plan_required_provider_hooks(out);
    if (out->unproven_reason == XR_ENTRY_PROVEN && profile == XG_BUILD_FREESTANDING &&
        (out->provider_hook_bits & ~bundle->target_provider.hook_bits) != 0)
        out->unproven_reason = XR_ENTRY_MISSING_PROVIDER_HOOK;
    return true;
}

const char *xaot_root_representation_name(uint8_t value) {
    switch ((XrRootRepresentation) value) {
        case XR_ROOT_ELIDED:
            return "elided";
        case XR_ROOT_DESCRIPTOR:
            return "descriptor";
        case XR_ROOT_RESUMABLE_FRAME:
            return "resumable_frame";
    }
    return "invalid";
}

const char *xaot_scheduler_mode_name(uint8_t value) {
    switch ((XrSchedulerMode) value) {
        case XR_SCHED_NONE:
            return "none";
        case XR_SCHED_SINGLE:
            return "single";
        case XR_SCHED_MULTI:
            return "multi";
    }
    return "invalid";
}

const char *xaot_entry_unproven_reason_name(uint8_t value) {
    switch ((XrEntryUnprovenReason) value) {
        case XR_ENTRY_PROVEN:
            return "none";
        case XR_ENTRY_NO_ROOT_BODY:
            return "no_root_body";
        case XR_ENTRY_PROVIDER_ABI:
            return "provider_abi";
        case XR_ENTRY_MISSING_CAPABILITY:
            return "missing_capability";
        case XR_ENTRY_MISSING_PROVIDER_HOOK:
            return "missing_provider_hook";
        case XR_ENTRY_OPEN_REACHABILITY:
            return "open_reachability";
        case XR_ENTRY_MODULE_INIT_SUSPENDS:
            return "module_init_suspends";
    }
    return "invalid";
}
