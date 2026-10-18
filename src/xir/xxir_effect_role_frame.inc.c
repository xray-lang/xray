/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_role_frame.inc.c - Current live ranges for role replay
 *
 * A ledger containment query never reads a proposed payload address. These
 * checks establish current geometry, not semantic permission or a reservation.
 */
static XrXirStatus effect_role_range(const XrXirCompileContext *work,
    const void *memory,uint64_t count,size_t width) {
    if (!count) return XR_XIR_OK;
    if (!memory || !width || count>SIZE_MAX/width) return XR_XIR_BAD_STRUCTURE;
    XrCompileResourceStatus status=xr_compile_resources_extent(work->resources,memory,(size_t)count*width);
    return status==XR_COMPILE_RESOURCE_OK?XR_XIR_OK:
        status==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:XR_XIR_BAD_STRUCTURE;
}
static bool effect_role_same_head(const EffectBodyFunctionHead *a,const EffectBodyFunction *b) {
    return a->name==b->name && a->name_length==b->name_length &&
        a->parameters==b->parameters && a->parameter_count==b->parameter_count && a->result==b->result &&
        a->blocks==b->blocks && a->block_count==b->block_count &&
        a->instruction_count==b->instruction_count && a->operands==b->operands && a->operand_count==b->operand_count;
}
static XrXirStatus effect_role_material_frame(const XrXirCompileContext *work,
    const EffectBodyMaterial *material) {
    if (!xir_compile_context_valid(work)) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=XR_XIR_OK;uint32_t depth=0;
    for (const EffectBodyMaterial *body=material;body;body=body->backbone) {
        if (depth++==3) return XR_XIR_BAD_STRUCTURE;
        status=effect_role_range(work,body,1,sizeof(*body));if (status!=XR_XIR_OK) return status;
        if (!xir_compile_work(work,3)) return XR_XIR_BUDGET;
        if (body->resources!=work->resources) return XR_XIR_BAD_STRUCTURE;
        if (body->overlay) {
            status=effect_role_range(work,body->overlay,1,sizeof(*body->overlay));
            if (status!=XR_XIR_OK) return status;
        }
    }
    return effect_body_material_shape(material)?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
}
static XrXirStatus effect_role_frame(EffectRoleCheck *check) {
    const XrXirCompileContext *work=check->work;const EffectBodyMaterial *material=check->material;
    XrXirStatus status=effect_role_material_frame(work,material);if (status!=XR_XIR_OK) return status;
    uint32_t functions=effect_body_count(material);uint64_t total=0;
    if (functions>effect_body_entry_limit(work) ||
        (check->dimensions_locked && functions!=check->functions)) return XR_XIR_BAD_STRUCTURE;
    status=effect_role_range(work,material->functions,functions,sizeof(*material->functions));
    if (status!=XR_XIR_OK) return status;
    const XrXirTypes *types=material->domain.types;
    if (types!=&material->terms.types) return XR_XIR_BAD_STRUCTURE;
    status=effect_role_range(work,types->nodes,types->count,sizeof(*types->nodes));
    if (status!=XR_XIR_OK) return status;
    if (material->domain.declarations) {
        const XrXirDeclarations *declarations=material->domain.declarations;
        status=effect_role_range(work,declarations,1,sizeof(*declarations));
        if (status==XR_XIR_OK) status=effect_role_range(work,declarations->slots,
            declarations->slot_count,sizeof(*declarations->slots));
        if (status!=XR_XIR_OK) return status;
    }
    for (uint32_t f=0;f<functions;++f) {
        if (!xir_compile_work(work,12)) return XR_XIR_BUDGET;
        const EffectBodyFunction *head=&material->functions[f];uint32_t count=head->parameter_count;
        if (head->run_count>head->run_capacity ||
            (head->instruction_count && (!head->runs || !head->run_count))) return XR_XIR_BAD_STRUCTURE;
        status=effect_role_range(work,head->parameters,count,sizeof(*head->parameters));
        if (status==XR_XIR_OK) status=effect_role_range(work,head->operands,head->operand_count,sizeof(*head->operands));
        if (status==XR_XIR_OK) status=effect_role_range(work,head->blocks,head->block_count,sizeof(*head->blocks));
        if (status==XR_XIR_OK) status=effect_role_range(work,head->runs,head->run_capacity,sizeof(*head->runs));
        if (status!=XR_XIR_OK) return status;
        total+=count;if (total>UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
        if (check->active_head && f==check->active_function &&
            !effect_role_same_head(check->active_head,head)) return XR_XIR_BAD_STRUCTURE;
    }
    if ((check->dimensions_locked && total!=check->total) ||
        (check->active_head && check->active_function>=functions)) return XR_XIR_BAD_STRUCTURE;
    if (!check->dimensions_locked) {
        check->functions=functions;check->total=(uint32_t)total;check->dimensions_locked=true;
    }
    return XR_XIR_OK;
}
static XrXirStatus effect_invocation_primitive_frame(const XrXirCompileContext *work,
    const EffectBodyMaterial *material) {
    EffectRoleCheck check={.work=work,.material=material};
    return effect_role_frame(&check);
}
