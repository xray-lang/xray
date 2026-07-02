/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xaot_boundary.c - explicit AOT representation boundary plan
 */

#include "xaot_boundary.h"
#include "xaot_bundle.h"
#include "../ir/xi_module.h"
#include <string.h>

XR_FUNC const char *xaot_boundary_reason_name(XaotBoundaryReason reason) {
    switch (reason) {
        case XAOT_BOUNDARY_NONE:
            return "none";
        case XAOT_BOUNDARY_DIRECT_CALL:
            return "direct-call";
        case XAOT_BOUNDARY_DYNAMIC_CALL:
            return "dynamic-call";
        case XAOT_BOUNDARY_CLOSURE_OBJECT:
            return "closure-object";
        case XAOT_BOUNDARY_MODULE_INIT:
            return "module-init";
        case XAOT_BOUNDARY_EXCEPTION_FLOW:
            return "exception-flow";
        case XAOT_BOUNDARY_CORO_FRAME:
            return "coro-frame";
        case XAOT_BOUNDARY_TAGGED_TYPE:
            return "tagged-type";
        case XAOT_BOUNDARY_BOX:
            return "box";
        case XAOT_BOUNDARY_UNBOX:
            return "unbox";
        case XAOT_BOUNDARY_SHARED_SLOT:
            return "shared-slot";
        case XAOT_BOUNDARY_IMPORT_EXPORT:
            return "import-export";
        case XAOT_BOUNDARY_REFLECTION:
            return "reflection";
        case XAOT_BOUNDARY_UNION_NULLABLE:
            return "union-nullable";
        case XAOT_BOUNDARY_RUNTIME_HELPER:
            return "runtime-helper";
        case XAOT_BOUNDARY_CORO_RESULT:
            return "coro-result";
        default:
            return "?";
    }
}

XR_FUNC const char *xaot_boundary_step_kind_name(XaotBoundaryStepKind kind) {
    switch (kind) {
        case XAOT_BOUNDARY_STEP_FUNC_ABI:
            return "func-abi";
        case XAOT_BOUNDARY_STEP_VALUE_REP:
            return "value-rep";
        case XAOT_BOUNDARY_STEP_DIRECT_CALL_ARG:
            return "direct-call-arg";
        case XAOT_BOUNDARY_STEP_DIRECT_CALL_RET:
            return "direct-call-ret";
        default:
            return "?";
    }
}

static const XiValue *unwrap_identity_value(const XiValue *v) {
    while (v && (v->op == XI_BOX || v->op == XI_UNBOX || v->op == XI_COPY || v->op == XI_MOVE) &&
           v->nargs >= 1) {
        v = v->args[0];
    }
    return v;
}

static const XiImportRef *value_import_ref(const XiValue *v) {
    v = unwrap_identity_value(v);
    if (!v || v->op != XI_IMPORT_REF || !v->aux)
        return NULL;
    return (const XiImportRef *) v->aux;
}

static const XiImportRef *shared_slot_import_ref(const XiFunc *f, int slot) {
    uint32_t bi;

    if (!f || slot < 0)
        return NULL;
    for (bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        uint32_t vi;
        if (!blk)
            continue;
        for (vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!v || v->op != XI_SET_SHARED || (int) v->aux_int != slot || v->nargs < 1)
                continue;
            const XiImportRef *ref = value_import_ref(v->args[0]);
            if (ref)
                return ref;
        }
    }
    return NULL;
}

static const XiImportRef *module_slot_import_ref(const XiModule *mod, int slot) {
    if (!mod || slot < 0 || slot >= mod->nslots || !mod->slot_imports)
        return NULL;
    return mod->slot_imports[slot];
}

static const XiModule *bundle_module_for_func(const XaotBundle *bundle, const XiFunc *func) {
    const XaotFuncPlan *plan;
    if (!bundle || !func)
        return NULL;
    plan = xaot_bundle_find_func_plan(bundle, func);
    if (plan && plan->module_index < bundle->nmodules)
        return bundle->modules[plan->module_index];
    for (const XiFunc *f = func; f; f = f->parent_func) {
        if (f->module)
            return f->module;
    }
    return NULL;
}

static bool module_matches_import(const XiModule *mod, const XiImportRef *ref) {
    if (!mod || !ref || !ref->module_path)
        return false;
    if (mod->path && strcmp(mod->path, ref->module_path) == 0)
        return true;
    if (mod->name && strcmp(mod->name, ref->module_path) == 0)
        return true;
    return false;
}

static const XiFunc *resolve_export_in_module(const XiModule *mod, const XiImportRef *ref) {
    uint16_t ei;

    if (!mod || !ref)
        return NULL;
    if (ref->resolved_shared_slot >= 0 && ref->resolved_shared_slot < mod->nslots &&
        mod->slot_funcs) {
        const XiFunc *slot_func = mod->slot_funcs[ref->resolved_shared_slot];
        if (slot_func)
            return slot_func;
    }
    if (!ref->member_name)
        return NULL;
    for (ei = 0; ei < mod->nexports; ei++) {
        const XiModuleExport *exp = &mod->exports[ei];
        if (exp->function && exp->name && strcmp(exp->name, ref->member_name) == 0)
            return exp->function;
    }
    return NULL;
}

static const XiFunc *resolve_import_ref(const XaotBundle *bundle, const XiImportRef *ref) {
    uint32_t mi;

    if (!bundle || !ref)
        return NULL;
    if (ref->resolved_mod_index >= 0 && (uint32_t) ref->resolved_mod_index < bundle->nmodules) {
        const XiFunc *target =
            resolve_export_in_module(bundle->modules[ref->resolved_mod_index], ref);
        if (target)
            return target;
    }
    for (mi = 0; mi < bundle->nmodules; mi++) {
        const XiModule *mod = bundle->modules[mi];
        const XiFunc *target;
        if (!module_matches_import(mod, ref))
            continue;
        target = resolve_export_in_module(mod, ref);
        if (target)
            return target;
    }
    if (ref->member_name) {
        for (mi = 0; mi < bundle->nmodules; mi++) {
            const XiFunc *target = resolve_export_in_module(bundle->modules[mi], ref);
            if (target)
                return target;
        }
    }
    return NULL;
}

static const XiImportRef *module_import_ref_for_value(const XaotBundle *bundle,
                                                      const XiFunc *current, const XiValue *value) {
    const XiModule *mod;
    const XiValue *v = unwrap_identity_value(value);
    const XiImportRef *ref = value_import_ref(v);

    if (ref && !ref->member_name)
        return ref;
    if (!v || v->op != XI_GET_SHARED)
        return NULL;

    int slot = (int) v->aux_int;
    mod = bundle_module_for_func(bundle, current);
    ref = module_slot_import_ref(mod, slot);
    if (ref && !ref->member_name)
        return ref;
    ref = shared_slot_import_ref(current, slot);
    if (!ref && current && current->module && current->module->init != current)
        ref = shared_slot_import_ref(current->module->init, slot);
    return ref && !ref->member_name ? ref : NULL;
}

static const XiFunc *resolve_module_member_target(const XaotBundle *bundle, const XiFunc *current,
                                                  const XiValue *call) {
    const char *member_name;
    const XiImportRef *module_ref;
    XiImportRef member_ref;

    if (!bundle || !call || call->op != XI_CALL_METHOD || call->nargs < 1 || !call->aux)
        return NULL;
    if ((call->aux_int & 1) != 0)
        return NULL;

    member_name = (const char *) call->aux;
    module_ref = module_import_ref_for_value(bundle, current, call->args[0]);
    if (!module_ref)
        return NULL;

    member_ref = *module_ref;
    member_ref.member_name = member_name;
    member_ref.resolved_shared_slot = -1;
    return resolve_import_ref(bundle, &member_ref);
}

static const XiFunc *resolve_shared_function(const XaotBundle *bundle, const XiFunc *current,
                                             int slot) {
    const XiModule *mod = NULL;
    const XiFunc *f;
    const XiImportRef *ref;

    if (!current || slot < 0)
        return NULL;
    mod = bundle_module_for_func(bundle, current);
    /* Slot metadata lives on the module init function; walk the lexical
     * parent chain so calls made inside nested functions resolve too. */
    for (f = current; f; f = f->parent_func) {
        if (f->shared_slot_funcs && slot < f->shared_slot_func_count && f->shared_slot_funcs[slot])
            return f->shared_slot_funcs[slot];
    }
    if (mod && slot < mod->nslots && mod->slot_funcs && mod->slot_funcs[slot])
        return mod->slot_funcs[slot];

    ref = module_slot_import_ref(mod, slot);
    if (ref)
        return resolve_import_ref(bundle, ref);

    ref = shared_slot_import_ref(current, slot);
    if (!ref && mod && mod->init && mod->init != current)
        ref = shared_slot_import_ref(mod->init, slot);
    return resolve_import_ref(bundle, ref);
}

static const char *receiver_class_name(const XiValue *recv) {
    if (!recv || !recv->type)
        return NULL;
    if ((recv->type->kind == XR_KIND_CLASS || recv->type->kind == XR_KIND_INSTANCE) &&
        recv->type->instance.class_name)
        return recv->type->instance.class_name;
    return NULL;
}

static const XiFunc *method_func_from_class(const XiModule *mod, const XiClassData *cd,
                                            const char *method_name, int method_index) {
    if (!mod || !mod->init || !cd || !cd->methods || !cd->child_idx)
        return NULL;
    for (uint16_t mi = 0; mi < cd->nmethod; mi++) {
        const XiClassMethod *method = &cd->methods[mi];
        uint16_t child_idx;
        if (method->is_static_constructor || method->is_constructor || method->is_static)
            continue;
        if (method_index >= 0) {
            if ((int) mi != method_index)
                continue;
        } else if (!method_name || !method->name || strcmp(method->name, method_name) != 0) {
            continue;
        }
        if (mi >= cd->ninst + cd->nstat)
            return NULL;
        child_idx = cd->child_idx[mi];
        if (child_idx >= mod->init->nchildren)
            return NULL;
        return mod->init->children[child_idx];
    }
    return NULL;
}

static const XiFunc *resolve_method_target(const XaotBundle *bundle, const XiValue *call) {
    const char *class_name;
    const char *method_name;
    int method_index = -1;
    uint32_t mi;

    if (!bundle || !call || (call->op != XI_CALL_METHOD && call->op != XI_CALL_METHOD_DIRECT) ||
        call->nargs < 1)
        return NULL;
    if (call->op == XI_CALL_METHOD && (call->aux_int & 1) != 0)
        return NULL;

    class_name = receiver_class_name(call->args[0]);
    if (!class_name)
        return NULL;
    method_name = call->aux ? (const char *) call->aux : NULL;
    if (call->op == XI_CALL_METHOD_DIRECT)
        method_index = (int) call->aux_int;

    for (mi = 0; mi < bundle->nmodules; mi++) {
        const XiModule *mod = bundle->modules[mi];
        uint16_t si;
        if (!mod || !mod->slot_classes)
            continue;
        for (si = 0; si < mod->nslots; si++) {
            const XiClassData *cd = mod->slot_classes[si];
            const XiFunc *target;
            if (!cd || !cd->class_name || strcmp(cd->class_name, class_name) != 0)
                continue;
            target = method_func_from_class(mod, cd, method_name, method_index);
            if (target)
                return target;
        }
    }
    return NULL;
}

XR_FUNC const XiFunc *xaot_boundary_resolve_direct_call_target(const XaotBundle *bundle,
                                                               const XiFunc *current,
                                                               const XiValue *call) {
    const XiValue *callee;

    if (!bundle || !current || !call)
        return NULL;
    if (call->op == XI_CALL_METHOD || call->op == XI_CALL_METHOD_DIRECT) {
        const XiFunc *target = resolve_method_target(bundle, call);
        if (target)
            return target;
        return resolve_module_member_target(bundle, current, call);
    }
    if (call->op != XI_CALL || call->nargs < 1)
        return NULL;

    callee = unwrap_identity_value(call->args[0]);
    if (!callee)
        return NULL;
    if (callee->op == XI_CLOSURE_NEW && callee->aux)
        return (const XiFunc *) callee->aux;
    if (callee->op == XI_STACK_ALLOC && callee->aux_int == XI_CLOSURE_NEW && callee->aux)
        return (const XiFunc *) callee->aux;
    if (callee->op == XI_CONST && callee->type && callee->type->kind == XR_KIND_NULL &&
        current->name)
        return current;
    if (callee->op == XI_GET_SHARED)
        return resolve_shared_function(bundle, current, (int) callee->aux_int);
    if (callee->op == XI_IMPORT_REF && callee->aux)
        return resolve_import_ref(bundle, (const XiImportRef *) callee->aux);
    return NULL;
}
