/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_instance_cell_access.inc.c - Actual module owner admission
 *
 * KEY CONCEPT:
 *   A frame loan does not grant worker access to module storage.
 */
#include "xxir_slot_access_internal.h"

static bool instance_cell_work(XrXirCallView *view, uint64_t work) {
    XrXirValueAdmission *admission = xr_xir_call_admission(view);
    if (!admission || work > admission->work) return false;
    admission->work -= work;
    return admission->domain && xr_xir_domain_work(admission->domain, work);
}
static XrXirCallStatus instance_received_type(XrXirValueAdmission *admission,const XrXirValue *value,
    XrXirType *type) {
    if (!admission || !type || !xr_xir_value_valid(value)) return XR_XIR_CALL_BAD_ARGUMENT;
    const XrXirTypeArena *arena=xr_xir_value_arena(value);*type=(XrXirType)value->type;
    if (arena && arena!=admission->arena) {
        uint64_t work=xir_type_arena_lookup_work(admission->arena);
        if (work>admission->work || !admission->domain || !xr_xir_domain_work(admission->domain,work))
            return XR_XIR_CALL_LIMIT;
        admission->work-=work;
        if (!xir_type_arena_local_type(admission->arena,arena,(XrXirType)value->type,type))
            return XR_XIR_CALL_BAD_ARGUMENT;
    }
    return xr_xir_value_argument(value,admission->arena,*type) ? XR_XIR_CALL_READY : XR_XIR_CALL_BAD_ARGUMENT;
}
static XrXirCallStatus instance_received_value(XrXirValueAdmission *admission,const XrXirValue *value,
    XrXirType *type) {
    XrXirCallStatus status=instance_received_type(admission,value,type);
    return status==XR_XIR_CALL_READY ? admit_instance_value(admission,value,*type) : status;
}
static XrXirCallStatus instance_slot_scope(XrXirCallView *view,const XrXirInstance *instance,
    uint32_t function,uint32_t module) {
    const XrXirDeclarations *d=instance->program->declarations;
    if (function>=instance->program->entry_count || module>=d->module_count) return XR_XIR_CALL_BAD_STATE;
    uint32_t from=d->functions[function].module;
    if (from==module) return XR_XIR_CALL_READY;
    if (!xr_xir_module_same_scope(d,from,module)) return XR_XIR_CALL_BAD_STATE;
    if (!instance_cell_work(view,(uint64_t)d->modules[from].dependency_count+1)) return XR_XIR_CALL_LIMIT;
    return xr_xir_module_imports(d,from,module) ? XR_XIR_CALL_READY : XR_XIR_CALL_BAD_STATE;
}
/* The live Program Input owns these facts for every synchronous read. */
static const XrXirSlotFacts *instance_slot_facts(const XrXirInstance *instance) {
    const XrXirProgramPermissions *permissions=instance->program->permissions;
    const XrXirProgramInput *input=permissions?permissions->input:NULL;
    return input && input->artifact ? xr_xir_compile_construction_slots(input->artifact->construction) : NULL;
}
static XrXirCallStatus instance_slot_read_scope(XrXirCallView *view,const XrXirInstance *instance,
    uint32_t function,uint32_t slot) {
    const XrXirDeclarations *d=instance->program->declarations;
    if(function>=instance->program->entry_count || slot>=d->slot_count)return XR_XIR_CALL_BAD_STATE;
    uint32_t from=d->functions[function].module,to=d->slots[slot].module;
    if(from==to || xr_xir_module_same_scope(d,from,to))return instance_slot_scope(view,instance,function,to);
    if(from>=d->module_count || !instance_cell_work(view,(uint64_t)d->modules[from].dependency_count+1))
        return from>=d->module_count?XR_XIR_CALL_BAD_STATE:XR_XIR_CALL_LIMIT;
    return xir_slot_public_read(d,instance_slot_facts(instance),from,slot)?XR_XIR_CALL_READY:XR_XIR_CALL_BAD_STATE;
}
static bool instance_cell_published(const XrXirInstance *instance, uint32_t slot,
    const XrXirValue *cell) {
    const XrXirDeclarations *d = instance->program->declarations;
    if (slot<d->slot_count && instance->slot_origins && instance->slot_origins[slot].owner) {
        InstanceStorageRoute route=instance->slot_origins[slot];
        return instance_cell_published(route.owner,route.index,cell);
    }
    if (slot >= d->slot_count || !d->slots[slot].mutable || !instance->published[slot]) return false;
    XrXirCellPublication publication = {instance, instance->domain, slot};
    return xr_xir_cell_publication_matches(cell, &publication) &&
        xr_xir_cell_same_owner(cell, &instance->slots[slot]);
}
static XrXirCallStatus instance_cell_access(XrXirCallView *view,
    const XrXirInstance *instance, const XrXirValue *cell) {
    if (!xr_xir_cell_in_domain(cell, instance->domain)) return XR_XIR_CALL_BAD_ARGUMENT;
    const XrXirValue *root=xr_xir_cell_canonical_root(cell);
    const XrXirTypes *root_types=root ? xr_xir_compile_type_arena_types(xr_xir_value_arena(root)) : NULL;
    if (root && xr_xir_type_is_class(root_types,(XrXirType)root->type) &&
        view_is_child(instance,view)) return XR_XIR_CALL_BAD_STATE;
    if (!xr_xir_cell_module_storage(cell)) return XR_XIR_CALL_READY;
    if (view_is_child(instance, view)) return XR_XIR_CALL_BAD_STATE;
    if (root && xr_xir_type_is_class(root_types,(XrXirType)root->type)) {
        XrXirType type=XR_XIR_UNIT;
        XrXirCallStatus status=instance_received_type(xr_xir_call_admission(view),root,&type);
        if (status!=XR_XIR_CALL_READY)
            return status==XR_XIR_CALL_BAD_ARGUMENT ? XR_XIR_CALL_BAD_STATE : status;
        return xr_xir_class_module_storage(root) ? XR_XIR_CALL_READY : XR_XIR_CALL_BAD_STATE;
    }
    const XrXirDeclarations *d = instance->program->declarations;
    for (uint32_t slot = 0; slot < d->slot_count; ++slot) {
        if (!instance_cell_work(view, 1)) return XR_XIR_CALL_LIMIT;
        if (!instance_cell_published(instance, slot, cell)) continue;
        uint32_t module = d->slots[slot].module;
        return instance->ready[module] || instance->current_module == module ?
            XR_XIR_CALL_READY : XR_XIR_CALL_BAD_STATE;
    }
    return XR_XIR_CALL_BAD_STATE;
}

#include "xxir_instance_view_internal.h"
XR_FUNC XrXirCallStatus xir_instance_view_module_root(XrXirCallView *view,const XrXirValue *root) {
    if (!instance_view_current(view) || !xr_xir_value_valid(root) ||
        xr_xir_cell_is_projection(root) || !xr_xir_cell_module_storage(root)) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirInstance *instance=view_instance(view);
    if (!instance || view_is_child(instance,view) || !xr_xir_cell_in_domain(root,instance->domain))
        return XR_XIR_CALL_BAD_STATE;
    const XrXirDeclarations *d=instance->program->declarations;
    uint32_t caller=xr_xir_call_current_entry(view->activation);
    for (uint32_t slot=0;slot<d->slot_count;++slot) {
        if (!instance_cell_work(view,1)) return XR_XIR_CALL_LIMIT;
        if (!instance_cell_published(instance,slot,root)) continue;
        XrXirCellPublication publication={instance,instance->domain,slot};
        uint32_t module=d->slots[slot].module;
        if (!xr_xir_cell_publication_matches(root,&publication) || module>=d->module_count ||
            d->functions[caller].module!=module ||
            (!instance->ready[module] && instance->current_module!=module)) return XR_XIR_CALL_BAD_STATE;
        return XR_XIR_CALL_READY;
    }
    return XR_XIR_CALL_BAD_STATE;
}
