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

static bool address_plan_for_value(const XaotBundle *bundle, const XiModule *module,
                                   const XiFunc *func, const XiValue *value, XaotAddressPlan *out) {
    const XaotStoragePlan *storage;
    const XaotAddressPlan *source;
    if (!bundle || !module || !func || !value || !out || !value->type ||
        !XR_TYPE_IS_POINTER(value->type))
        return false;
    memset(out, 0, sizeof(*out));
    out->func = func;
    out->value = value;
    out->evidence =
        XAOT_ADDRESS_EV_POINTER_TYPE | XAOT_ADDRESS_EV_LIFETIME | XAOT_ADDRESS_EV_ESCAPE_SCAN;
    if (value->op == XI_STATIC_ADDR) {
        const XiModule *storage_module = module;
        uint32_t storage_slot = (uint32_t) value->aux_int;
        if (value->aux_int >= 0 && value->aux_int < module->nslots && module->slot_imports) {
            const XiImportRef *ref = module->slot_imports[value->aux_int];
            if (ref && ref->resolved_mod_index >= 0 && ref->resolved_shared_slot >= 0 &&
                (uint32_t) ref->resolved_mod_index < bundle->nmodules &&
                bundle->modules[ref->resolved_mod_index]) {
                storage_module = bundle->modules[ref->resolved_mod_index];
                storage_slot = (uint32_t) ref->resolved_shared_slot;
            }
        }
        storage = xaot_storage_plan_find(bundle, storage_module, storage_slot);
        if (!storage)
            return false;
        out->provenance.storage_id = storage->decl_id;
        out->provenance.lifetime_id = storage->decl_id;
        out->provenance.owner = storage->owner;
        out->provenance.mutability = storage->mutability;
        out->provenance.address_identity = storage->address_identity;
        out->provenance.origin = storage->owner == XR_STORAGE_MODULE ? XR_POINTER_ORIGIN_MODULE
                                                                     : XR_POINTER_ORIGIN_STATIC;
        out->provenance.escape = XR_POINTER_ESCAPE_STABLE;
        out->evidence |= XAOT_ADDRESS_EV_STORAGE_PLAN;
        return true;
    }
    if (value->op == XI_STATIC_BYTES_PTR && value->aux_int >= 0 && value->aux) {
        out->origin_value = value;
        out->provenance.storage_id = value->id + 1;
        out->provenance.lifetime_id = value->id + 1;
        out->provenance.owner = XR_STORAGE_MODULE;
        out->provenance.mutability = XR_STORAGE_READONLY;
        out->provenance.address_identity = XR_ADDRESS_MODULE_STABLE;
        out->provenance.origin = XR_POINTER_ORIGIN_STATIC;
        out->provenance.escape = XR_POINTER_ESCAPE_STABLE;
        return true;
    }
    if (value->op == XI_ARRAY_DATA_PTR && value->nargs > 0 && value->args[0]) {
        const XiValue *owner = value->args[0];
        if (owner->type && owner->type->kind == XR_KIND_FIXED_ARRAY && owner->op == XI_GET_SHARED &&
            owner->aux_int >= 0) {
            const XiModule *storage_module = module;
            uint32_t storage_slot = (uint32_t) owner->aux_int;
            if (owner->aux_int < module->nslots && module->slot_imports) {
                const XiImportRef *ref = module->slot_imports[owner->aux_int];
                if (ref && ref->resolved_mod_index >= 0 && ref->resolved_shared_slot >= 0 &&
                    (uint32_t) ref->resolved_mod_index < bundle->nmodules &&
                    bundle->modules[ref->resolved_mod_index]) {
                    storage_module = bundle->modules[ref->resolved_mod_index];
                    storage_slot = (uint32_t) ref->resolved_shared_slot;
                }
            }
            storage = xaot_storage_plan_find(bundle, storage_module, storage_slot);
            if (storage && storage->owner == XR_STORAGE_MODULE &&
                storage->mutability == XR_STORAGE_READONLY &&
                storage->address_identity == XR_ADDRESS_MODULE_STABLE) {
                out->origin_value = owner;
                out->provenance.storage_id = storage->decl_id;
                out->provenance.lifetime_id = storage->decl_id;
                out->provenance.owner = storage->owner;
                out->provenance.mutability = storage->mutability;
                out->provenance.address_identity = storage->address_identity;
                out->provenance.origin = XR_POINTER_ORIGIN_MODULE;
                out->provenance.escape = XR_POINTER_ESCAPE_STABLE;
                out->evidence |= XAOT_ADDRESS_EV_STORAGE_PLAN;
                return true;
            }
        }
        out->origin_value = value->args[0];
        out->provenance.storage_id = value->args[0]->id + 1;
        out->provenance.lifetime_id = value->args[0]->id + 1;
        out->provenance.owner = XR_STORAGE_EXEC_LOCAL;
        out->provenance.mutability =
            value->type->ptr_is_mut ? XR_STORAGE_MUTABLE : XR_STORAGE_READONLY;
        out->provenance.address_identity = XR_ADDRESS_LEXICAL;
        out->provenance.origin = XR_POINTER_ORIGIN_OWNER_BORROW;
        out->provenance.escape = XR_POINTER_ESCAPE_CALL_BOUND;
        return true;
    }
    if ((value->op == XI_ADD || value->op == XI_COPY || value->op == XI_MOVE) && value->nargs > 0 &&
        value->args[0]) {
        source = xaot_address_plan_find(bundle, value->args[0]);
        if (!source)
            return false;
        out->origin_value = source->origin_value ? source->origin_value : source->value;
        out->provenance = source->provenance;
        out->evidence = source->evidence;
        return true;
    }
    if (value->op == XI_CONST && value->aux_int == 0) {
        out->provenance.origin = XR_POINTER_ORIGIN_NULL;
        out->provenance.escape = XR_POINTER_ESCAPE_STABLE;
        return true;
    }
    return false;
}

static bool address_has_forbidden_escape(const XiFunc *func, const XaotAddressPlan *plan) {
    if (!func || !plan || plan->provenance.escape == XR_POINTER_ESCAPE_STABLE)
        return false;
    for (uint32_t bi = 0; bi < func->nblocks; bi++) {
        const XiBlock *block = func->blocks[bi];
        if (!block)
            continue;
        if (block->kind == XI_BLOCK_RETURN && block->control == plan->value)
            return true;
        for (uint32_t vi = 0; vi < block->nvalues; vi++) {
            const XiValue *use = block->values[vi];
            if (!use)
                continue;
            for (uint16_t ai = 0; ai < use->nargs; ai++) {
                if (use->args[ai] != plan->value)
                    continue;
                switch (use->op) {
                    case XI_GO:
                    case XI_THREAD_SPAWN:
                    case XI_SET_SHARED:
                    case XI_SET_GLOBAL:
                    case XI_STORE_UPVAL:
                    case XI_CLOSURE_NEW:
                    case XI_ARRAY_PUSH:
                        return true;
                    default:
                        break;
                }
            }
        }
    }
    return false;
}

static bool add_addresses_recursive(XaotBundle *bundle, const XiModule *module,
                                    const XiFunc *func) {
    if (!func)
        return true;
    for (uint32_t bi = 0; bi < func->nblocks; bi++) {
        const XiBlock *block = func->blocks[bi];
        if (!block)
            continue;
        for (uint32_t vi = 0; vi < block->nvalues; vi++) {
            XaotAddressPlan plan;
            const XiValue *value = block->values[vi];
            if (!address_plan_for_value(bundle, module, func, value, &plan))
                continue;
            if (address_has_forbidden_escape(func, &plan)) {
                bundle->error_msg = "address borrow escapes its verified lifetime";
                return false;
            }
            if (!reserve_rows((void **) &bundle->address_plans, &bundle->address_plan_cap,
                              bundle->naddress_plans + 1, sizeof(XaotAddressPlan))) {
                bundle->error_msg = "address plan allocation failed";
                return false;
            }
            bundle->address_plans[bundle->naddress_plans++] = plan;
        }
    }
    for (uint16_t i = 0; i < func->nchildren; i++) {
        if (!add_addresses_recursive(bundle, module, func->children[i])) {
            return false;
        }
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
        if (bundle->module_init_plans[bundle->nmodule_init_plans - 1].may_suspend) {
            bundle->error_msg = "module initializer may not suspend";
            return false;
        }
        for (uint32_t slot = 0; slot < module->nslots; slot++) {
            XaotStoragePlan plan;
            if (!module->init->slot_owned_names || !module->init->slot_owned_names[slot])
                continue;
            if (!derive_storage(bundle, module, mi, slot, &plan)) {
                bundle->error_msg = "module storage provenance is missing";
                return false;
            }
            if (!reserve_rows((void **) &bundle->storage_plans, &bundle->storage_plan_cap,
                              bundle->nstorage_plans + 1, sizeof(XaotStoragePlan)))
                return false;
            bundle->storage_plans[bundle->nstorage_plans++] = plan;
        }
        if (!add_captures_recursive(bundle, module->init)) {
            bundle->error_msg = "capture plan allocation failed";
            return false;
        }
    }
    for (uint32_t mi = 0; mi < bundle->nmodules; mi++) {
        const XiModule *module = bundle->modules[mi];
        if (!add_addresses_recursive(bundle, module, module ? module->init : NULL))
            return false;
    }
    return true;
}

bool xaot_storage_capture_plans_verify(const XaotBundle *bundle, char *errbuf, size_t errbuf_len) {
    uint32_t storage_index = 0;
    uint32_t capture_index = 0;
    uint32_t address_index = 0;
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
    for (uint32_t mi = 0; mi < bundle->nmodules; mi++) {
        const XiModule *module = bundle->modules[mi];
        const XiFunc *stack[256];
        uint32_t depth = 0;
        if (!module || !module->init)
            return false;
        stack[depth++] = module->init;
        while (depth > 0) {
            const XiFunc *func = stack[--depth];
            for (uint32_t bi = 0; bi < func->nblocks; bi++) {
                const XiBlock *block = func->blocks[bi];
                if (!block)
                    continue;
                for (uint32_t vi = 0; vi < block->nvalues; vi++) {
                    XaotAddressPlan expected;
                    const XiValue *value = block->values[vi];
                    if (!address_plan_for_value(bundle, module, func, value, &expected))
                        continue;
                    if (address_has_forbidden_escape(func, &expected)) {
                        if (errbuf && errbuf_len)
                            snprintf(errbuf, errbuf_len,
                                     "AOT address provenance permits a lifetime escape");
                        return false;
                    }
                    if (address_index >= bundle->naddress_plans ||
                        memcmp(&expected, &bundle->address_plans[address_index],
                               sizeof(expected)) != 0) {
                        if (errbuf && errbuf_len)
                            snprintf(errbuf, errbuf_len, "AOT address provenance plan is stale");
                        return false;
                    }
                    address_index++;
                }
            }
            for (uint16_t ci = func->nchildren; ci > 0; ci--) {
                if (depth >= sizeof(stack) / sizeof(stack[0])) {
                    if (errbuf && errbuf_len)
                        snprintf(errbuf, errbuf_len, "AOT address provenance nesting too deep");
                    return false;
                }
                stack[depth++] = func->children[ci - 1];
            }
        }
    }
    if (address_index != bundle->naddress_plans) {
        if (errbuf && errbuf_len)
            snprintf(errbuf, errbuf_len, "AOT address provenance plan count is stale");
        return false;
    }
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

const XaotAddressPlan *xaot_address_plan_find(const XaotBundle *bundle, const XiValue *value) {
    if (!bundle || !value)
        return NULL;
    for (uint32_t i = 0; i < bundle->naddress_plans; i++) {
        if (bundle->address_plans[i].value == value)
            return &bundle->address_plans[i];
    }
    return NULL;
}

const char *xaot_materialization_kind_name(uint8_t value) {
    static const char *names[] = {"inline",         "exec_local",   "module_readonly",
                                  "module_runtime", "owned_system", "shared_system",
                                  "reject"};
    return value < sizeof(names) / sizeof(names[0]) ? names[value] : "invalid";
}

const char *xaot_capture_action_name(uint8_t value) {
    static const char *names[] = {"inline_value",    "deep_copy",  "move",
                                  "module_readonly", "shared_ref", "reject"};
    return value < sizeof(names) / sizeof(names[0]) ? names[value] : "invalid";
}

const char *xaot_pointer_origin_name(uint8_t value) {
    static const char *names[] = {"none",         "null",         "static", "module",
                                  "stack_borrow", "owner_borrow", "foreign"};
    return value < sizeof(names) / sizeof(names[0]) ? names[value] : "invalid";
}

const char *xaot_pointer_escape_name(uint8_t value) {
    static const char *names[] = {"none", "lexical", "call_bound", "stable"};
    return value < sizeof(names) / sizeof(names[0]) ? names[value] : "invalid";
}
