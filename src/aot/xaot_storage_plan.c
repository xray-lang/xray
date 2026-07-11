/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 */

#include "xaot_storage_plan.h"
#include "xaot_bundle.h"
#include "../base/xmalloc.h"
#include "../ir/xi_own.h"
#include <stdio.h>
#include <string.h>

static bool reserve_rows(void **rows, uint32_t *cap, uint32_t need, size_t elem_size) {
    uint32_t next;
    void *grown;
    if (need <= *cap)
        return true;
    next = *cap ? *cap * 2 : 8;
    while (next < need)
        next *= 2;
    grown = xr_realloc(*rows, (size_t) next * elem_size);
    if (!grown)
        return false;
    *rows = grown;
    *cap = next;
    return true;
}

static const XgDeclSummary *find_storage_decl(const XaotBundle *bundle, const XiModule *module,
                                              uint32_t module_index, uint32_t slot) {
    const XgGlobalEvidence *evidence;
    XgModuleId module_id;
    const char *name;
    uint32_t name_id;
    const XgDeclSummary *match = NULL;
    if (!bundle || !module || !module->init || !module->init->slot_owned_names ||
        slot >= module->init->nshared)
        return NULL;
    name = module->init->slot_owned_names[slot];
    if (!name)
        return NULL;
    evidence = bundle->global_evidence_plan.evidence;
    if (!evidence)
        return NULL;
    module_id = XG_NO_ID;
    if (module->init->xg_body_func_id != XG_NO_ID) {
        for (uint32_t i = 0; i < evidence->nbodies; i++) {
            if (evidence->bodies[i].func_id == module->init->xg_body_func_id) {
                module_id = evidence->bodies[i].module_id;
                break;
            }
        }
    }
    if (module_id == XG_NO_ID && module_index < evidence->nmodules)
        module_id = evidence->modules[module_index].module_id;
    if (module_id == XG_NO_ID)
        return NULL;
    name_id = xg_name_id(name);
    for (uint32_t i = 0; i < evidence->ndecls; i++) {
        const XgDeclSummary *decl = &evidence->decls[i];
        if (decl->module_id != module_id || decl->name_id != name_id ||
            decl->storage_owner == XR_STORAGE_NONE)
            continue;
        if (match)
            return NULL;
        match = decl;
    }
    return match;
}

static bool derive_storage(const XaotBundle *bundle, const XiModule *module, uint32_t module_index,
                           uint32_t slot, XaotStoragePlan *out) {
    const XgDeclSummary *decl = find_storage_decl(bundle, module, module_index, slot);
    if (!out || !decl)
        return false;
    memset(out, 0, sizeof(*out));
    out->decl_id = decl->decl_id;
    out->module_index = module_index;
    out->slot = slot;
    out->flags = decl->storage_flags;
    out->owner = decl->storage_owner;
    out->mutability = decl->storage_mutability;
    out->address_identity = decl->address_identity;
    out->materialization_kind = decl->materialization_kind;
    return true;
}

static XaotCapturePlan derive_capture(const XiFunc *func, uint16_t index) {
    const XiCapture *capture = &func->captures[index];
    XaotCapturePlan plan;
    memset(&plan, 0, sizeof(plan));
    plan.func = func;
    plan.capture_index = index;
    plan.evidence = XAOT_CAPTURE_EV_CLOSED_CAPTURE | XAOT_CAPTURE_EV_STORAGE_OWNER |
                    XAOT_CAPTURE_EV_TYPE_SHAPE | XAOT_CAPTURE_EV_MUTABILITY;
    plan.action = (uint8_t) xi_capture_cross_execution_action(capture);
    if (capture->capture_kind == XI_CAPTURE_SHARED || capture->is_shared) {
        plan.source_owner = XR_STORAGE_SHARED_SYSTEM;
    } else if (capture->capture_kind == XI_CAPTURE_MODULE_LIVE) {
        plan.source_owner = XR_STORAGE_MODULE;
    } else {
        plan.source_owner = XR_STORAGE_EXEC_LOCAL;
    }
    return plan;
}

static XaotModuleInitPlan derive_module_init(const XaotBundle *bundle, const XiModule *module,
                                             uint32_t module_index) {
    XaotModuleInitPlan plan;
    const XgGlobalEvidence *evidence = bundle->global_evidence_plan.evidence;
    memset(&plan, 0, sizeof(plan));
    plan.func = module ? module->init : NULL;
    plan.body_func_id = module && module->init ? module->init->xg_body_func_id : XG_NO_ID;
    plan.module_index = module_index;
    plan.allocation_owner = XR_STORAGE_MODULE;
    plan.evidence = XAOT_MODULE_INIT_EV_ENTRY_FUNC | XAOT_MODULE_INIT_EV_STORAGE_OWNER |
                    XAOT_MODULE_INIT_EV_NONSUSPEND;
    for (uint32_t i = 0; evidence && i < evidence->nbodies; i++) {
        if (evidence->bodies[i].func_id == plan.body_func_id) {
            /* The executable entry Xi body currently contains both published
             * module initialization and the logical root body.  Suspension
             * belongs to EntryPlan, never to its ModuleInitPlan view. */
            plan.may_suspend = module_index != bundle->entry_module &&
                               (evidence->bodies[i].effect_bits & XG_BODY_MAY_SUSPEND) != 0;
            break;
        }
    }
    return plan;
}

static bool add_captures_recursive(XaotBundle *bundle, const XiFunc *func) {
    if (!func)
        return true;
    for (uint16_t i = 0; i < func->ncaptures; i++) {
        if (!reserve_rows((void **) &bundle->capture_plans, &bundle->capture_plan_cap,
                          bundle->ncapture_plans + 1, sizeof(XaotCapturePlan)))
            return false;
        bundle->capture_plans[bundle->ncapture_plans++] = derive_capture(func, i);
    }
    for (uint16_t i = 0; i < func->nchildren; i++) {
        if (!add_captures_recursive(bundle, func->children[i]))
            return false;
    }
    return true;
}

bool xaot_storage_capture_plans_build(XaotBundle *bundle) {
    if (!bundle)
        return false;
    for (uint32_t mi = 0; mi < bundle->nmodules; mi++) {
        const XiModule *module = bundle->modules[mi];
        if (!module || !module->init)
            return false;
        if (!reserve_rows((void **) &bundle->module_init_plans, &bundle->module_init_plan_cap,
                          bundle->nmodule_init_plans + 1, sizeof(XaotModuleInitPlan)))
            return false;
        bundle->module_init_plans[bundle->nmodule_init_plans++] =
            derive_module_init(bundle, module, mi);
        if (bundle->module_init_plans[bundle->nmodule_init_plans - 1].may_suspend)
            return false;
        for (uint32_t slot = 0; slot < module->nslots; slot++) {
            XaotStoragePlan plan;
            if (!module->init->slot_owned_names || !module->init->slot_owned_names[slot])
                continue;
            if (!derive_storage(bundle, module, mi, slot, &plan))
                return false;
            if (!reserve_rows((void **) &bundle->storage_plans, &bundle->storage_plan_cap,
                              bundle->nstorage_plans + 1, sizeof(XaotStoragePlan)))
                return false;
            bundle->storage_plans[bundle->nstorage_plans++] = plan;
        }
        if (!add_captures_recursive(bundle, module->init))
            return false;
    }
    return true;
}

bool xaot_storage_capture_plans_verify(const XaotBundle *bundle, char *errbuf, size_t errbuf_len) {
    uint32_t storage_index = 0;
    uint32_t capture_index = 0;
    if (!bundle)
        return false;
    if (bundle->nmodule_init_plans == 0 && bundle->entry_plan.entry_func_id == XG_NO_ID)
        return true;
    if (bundle->nmodule_init_plans != bundle->nmodules) {
        if (errbuf && errbuf_len)
            snprintf(errbuf, errbuf_len, "AOT module-init plan count is stale");
        return false;
    }
    for (uint32_t mi = 0; mi < bundle->nmodules; mi++) {
        XaotModuleInitPlan expected = derive_module_init(bundle, bundle->modules[mi], mi);
        const XaotModuleInitPlan *actual = &bundle->module_init_plans[mi];
        if (expected.func != actual->func || expected.body_func_id != actual->body_func_id ||
            expected.module_index != actual->module_index ||
            expected.evidence != actual->evidence ||
            expected.allocation_owner != actual->allocation_owner ||
            expected.may_suspend != actual->may_suspend || actual->may_suspend) {
            if (errbuf && errbuf_len)
                snprintf(errbuf, errbuf_len, "AOT module-init plan is stale or suspendable");
            return false;
        }
    }
    for (uint32_t mi = 0; mi < bundle->nmodules; mi++) {
        const XiModule *module = bundle->modules[mi];
        for (uint32_t slot = 0; module && slot < module->nslots; slot++) {
            XaotStoragePlan expected;
            if (!module->init->slot_owned_names || !module->init->slot_owned_names[slot])
                continue;
            if (!derive_storage(bundle, module, mi, slot, &expected)) {
                if (errbuf && errbuf_len)
                    snprintf(errbuf, errbuf_len, "AOT storage provenance evidence is missing");
                return false;
            }
            if (storage_index >= bundle->nstorage_plans ||
                memcmp(&expected, &bundle->storage_plans[storage_index], sizeof(expected)) != 0) {
                if (errbuf && errbuf_len)
                    snprintf(errbuf, errbuf_len, "AOT storage plan is stale");
                return false;
            }
            storage_index++;
        }
    }
    if (storage_index != bundle->nstorage_plans) {
        if (errbuf && errbuf_len)
            snprintf(errbuf, errbuf_len, "AOT storage plan count is stale");
        return false;
    }
    for (uint32_t i = 0; i < bundle->ncapture_plans; i++) {
        const XaotCapturePlan *actual = &bundle->capture_plans[i];
        XaotCapturePlan expected;
        if (!actual->func || actual->capture_index >= actual->func->ncaptures)
            return false;
        expected = derive_capture(actual->func, actual->capture_index);
        if (memcmp(&expected, actual, sizeof(expected)) != 0) {
            if (errbuf && errbuf_len)
                snprintf(errbuf, errbuf_len, "AOT capture plan is stale");
            return false;
        }
        capture_index++;
    }
    (void) capture_index;
    return true;
}

const XaotStoragePlan *xaot_storage_plan_find(const XaotBundle *bundle, const XiModule *module,
                                              uint32_t slot) {
    uint32_t module_index;
    if (!bundle || !module || slot >= module->nslots)
        return NULL;
    for (module_index = 0; module_index < bundle->nmodules; module_index++) {
        if (bundle->modules[module_index] == module)
            break;
    }
    if (module_index == bundle->nmodules)
        return NULL;
    for (uint32_t i = 0; i < bundle->nstorage_plans; i++) {
        const XaotStoragePlan *plan = &bundle->storage_plans[i];
        if (plan->module_index == module_index && plan->slot == slot)
            return plan;
    }
    return NULL;
}

const XaotCapturePlan *xaot_capture_plan_find(const XaotBundle *bundle, const XiFunc *func,
                                              uint16_t index) {
    if (!bundle || !func || index >= func->ncaptures)
        return NULL;
    for (uint32_t i = 0; i < bundle->ncapture_plans; i++) {
        const XaotCapturePlan *plan = &bundle->capture_plans[i];
        if (plan->func == func && plan->capture_index == index)
            return plan;
    }
    return NULL;
}

const char *xaot_materialization_kind_name(uint8_t value) {
    static const char *names[] = {"inline",         "exec_local",    "module_readonly",
                                  "module_runtime", "shared_system", "reject"};
    return value < sizeof(names) / sizeof(names[0]) ? names[value] : "invalid";
}

const char *xaot_capture_action_name(uint8_t value) {
    static const char *names[] = {"inline_value",    "deep_copy",  "move",
                                  "module_readonly", "shared_ref", "reject"};
    return value < sizeof(names) / sizeof(names[0]) ? names[value] : "invalid";
}
