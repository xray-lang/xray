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

static int body_index_for_func(const XgGlobalEvidence *evidence, XgFuncId func_id) {
    if (!evidence || func_id == XG_NO_ID)
        return -1;
    for (uint32_t i = 0; i < evidence->nbodies; i++) {
        if (evidence->bodies[i].func_id == func_id)
            return (int) i;
    }
    return -1;
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

static void enqueue_target(const XgGlobalEvidence *evidence, XgFuncId func_id, uint8_t *seen,
                           uint32_t *queue, uint32_t *tail) {
    int index = body_index_for_func(evidence, func_id);
    if (index < 0 || seen[index])
        return;
    seen[index] = 1;
    queue[(*tail)++] = (uint32_t) index;
}

bool xaot_entry_plan_derive(const XaotBundle *bundle, const XgGlobalEvidence *evidence,
                            uint32_t profile, XaotEntryPlan *out) {
    uint8_t *seen;
    uint32_t *queue;
    uint32_t head = 0;
    uint32_t tail = 0;
    uint32_t provided;
    XgFuncId root;
    if (!bundle || !evidence || !out)
        return false;
    memset(out, 0, sizeof(*out));
    root = entry_func_id(bundle, evidence);
    out->entry_func_id = root;
    if (root == XG_NO_ID) {
        /* Plan-only unit bundles and libraries may intentionally have no
         * executable root.  Their exact requirement set is empty and the
         * physical root is therefore proven elided. */
        out->provided_capability_bits =
            profile == XG_BUILD_FREESTANDING ? xaot_freestanding_core_capabilities() : UINT32_MAX;
        out->evidence = XAOT_ENTRY_EV_CLOSED_WORLD_REACHABILITY | XAOT_ENTRY_EV_TARGET_PROVIDER;
        return true;
    }
    seen = (uint8_t *) xr_calloc(evidence->nbodies ? evidence->nbodies : 1, sizeof(uint8_t));
    queue = (uint32_t *) xr_calloc(evidence->nbodies ? evidence->nbodies : 1, sizeof(uint32_t));
    if (!seen || !queue) {
        xr_free(seen);
        xr_free(queue);
        return false;
    }
    enqueue_target(evidence, root, seen, queue, &tail);
    while (head < tail) {
        const XgBodySummary *body = &evidence->bodies[queue[head++]];
        out->reachable_body_count++;
        out->reachable_effect_bits |= body->effect_bits;
        out->required_capability_bits |= body->capability_bits;
        for (uint32_t ci = 0; ci < body->callsite_count; ci++) {
            uint32_t index = body->callsite_start + ci;
            const XgCallsiteSummary *call =
                index > 0 && index <= evidence->ncallsites ? &evidence->callsites[index - 1] : NULL;
            if (call && call->kind == XG_CALL_DIRECT_FUNC)
                enqueue_target(evidence, call->static_target_func_id, seen, queue, &tail);
        }
    }
    xr_free(seen);
    xr_free(queue);

    out->runtime_component_bits = out->required_capability_bits;
    if ((out->reachable_effect_bits & XG_BODY_MAY_SUSPEND) != 0)
        out->root_representation = XAOT_ROOT_RESUMABLE_FRAME;
    else if ((out->reachable_effect_bits & (XG_BODY_MAY_SPAWN | XG_BODY_OBSERVES_TASK_ID)) != 0)
        out->root_representation = XAOT_ROOT_DESCRIPTOR;
    else
        out->root_representation = XAOT_ROOT_ELIDED;

    if ((out->required_capability_bits & (XG_CAP_SYS_THREAD | XG_CAP_PARALLEL)) != 0)
        out->scheduler_mode = XAOT_SCHED_MULTI;
    else if ((out->required_capability_bits &
              (XG_CAP_COROUTINE | XG_CAP_TASK | XG_CAP_CHANNEL | XG_CAP_SCOPE)) != 0)
        out->scheduler_mode = XAOT_SCHED_SINGLE;
    else
        out->scheduler_mode = XAOT_SCHED_NONE;

    if (profile != XG_BUILD_FREESTANDING) {
        provided = UINT32_MAX;
        out->provider_hook_bits = UINT32_MAX;
    } else if (bundle->target_provider.abi_version != 0) {
        if (bundle->target_provider.abi_version != XAOT_PROVIDER_ABI_VERSION) {
            out->unproven_reason = XAOT_ENTRY_PROVIDER_ABI;
            return true;
        }
        provided = bundle->target_provider.provided_capability_bits;
        out->provider_hook_bits = bundle->target_provider.hook_bits;
    } else {
        provided = xaot_freestanding_core_capabilities();
    }
    out->provided_capability_bits = provided;
    out->evidence = XAOT_ENTRY_EV_GLOBAL_BODY | XAOT_ENTRY_EV_CLOSED_WORLD_REACHABILITY |
                    XAOT_ENTRY_EV_ROOT_EFFECT | XAOT_ENTRY_EV_TARGET_PROVIDER;
    if ((out->required_capability_bits & ~provided) != 0)
        out->unproven_reason = XAOT_ENTRY_MISSING_CAPABILITY;
    return true;
}

const char *xaot_root_representation_name(uint8_t value) {
    switch ((XaotRootRepresentation) value) {
        case XAOT_ROOT_ELIDED:
            return "elided";
        case XAOT_ROOT_DESCRIPTOR:
            return "descriptor";
        case XAOT_ROOT_RESUMABLE_FRAME:
            return "resumable_frame";
    }
    return "invalid";
}

const char *xaot_scheduler_mode_name(uint8_t value) {
    switch ((XaotSchedulerMode) value) {
        case XAOT_SCHED_NONE:
            return "none";
        case XAOT_SCHED_SINGLE:
            return "single";
        case XAOT_SCHED_MULTI:
            return "multi";
    }
    return "invalid";
}

const char *xaot_entry_unproven_reason_name(uint8_t value) {
    switch ((XaotEntryUnprovenReason) value) {
        case XAOT_ENTRY_PROVEN:
            return "none";
        case XAOT_ENTRY_NO_ROOT_BODY:
            return "no_root_body";
        case XAOT_ENTRY_PROVIDER_ABI:
            return "provider_abi";
        case XAOT_ENTRY_MISSING_CAPABILITY:
            return "missing_capability";
        case XAOT_ENTRY_MODULE_INIT_SUSPENDS:
            return "module_init_suspends";
    }
    return "invalid";
}
