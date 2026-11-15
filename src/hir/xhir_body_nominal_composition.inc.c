/*
 * xray - Lightweight typed scripting with native concurrency
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xhir_body_nominal_composition.inc.c - Checked suppliers for nominal literals
 *
 * KEY CONCEPT:
 *   Every source schema is admitted before selecting immutable suppliers.
 *   Explicit values dominate spreads; declaration defaults fill only holes.
 */
static bool body_nominal_composition_access(const XrHirBody *body,const HirBodyNominalPlan *plan,
    const XrHirNominalInfo *info,uint32_t field) {
    uint32_t owner=((HirBodyScope *)body->scopes.items)[plan->input.scope.index].nominal_owner;
    return !(info->fields[field].flags&(XR_XIR_FIELD_PRIVATE|XR_XIR_FIELD_PROTECTED)) || owner==info->declaration+1;
}
static bool body_nominal_spread_producer(const XrHirBody *body,const HirBodyNominalPlan *plan,
    const XrHirNominalSupplier *supplier,const XrHirNominalValue *value) {
    if(value->producer.owner!=body || value->producer.index>=body->count)return false;
    HirBodyUse producer=body->uses[value->producer.index];
    return producer.nominal_get && producer.consumed && producer.nominal_emitted && producer.nominal_has_result &&
        producer.nominal.owner==supplier->shape.owner && producer.nominal.index==supplier->shape.index &&
        producer.nominal_field==supplier->field && producer.source_operand==supplier->value &&
        producer.nominal_result==value->value && producer.action.target==value->type && supplier->type==value->type &&
        producer.origin.function==plan->input.origin.function && producer.origin.module==plan->input.origin.module;
}
XR_FUNC XrXirStatus xr_hir_body_nominal_compose(XrHirBody *body,XrHirNominalPlan plan,const XrHirNominalPool *pool,
    const XrHirNominalComponent *components,uint32_t count,const XrHirNominalSupplier **out,const char **reason) {
    if(!body || body->ended || plan.owner!=body || plan.index>=body->nominal_plans.count || !out ||
        (count && !components) || !pool || !body_nominal_pool(body,pool,pool->control))return XR_XIR_BAD_STRUCTURE;
    HirBodyNominalPlan row=((HirBodyNominalPlan *)body->nominal_plans.items)[plan.index];
    XrHirNominalInfo target={0};
    if(!row.applied || row.complete || row.composed || row.selected!=UINT32_MAX || row.begin ||
        row.input.kind!=XR_HIR_NOMINAL_STRUCT_LITERAL || !xr_hir_body_nominal_info(body,row.input.shape,&target))
        return XR_XIR_BAD_STRUCTURE;
    for(uint32_t f=0;f<row.count;++f)if(row.present[f])return XR_XIR_BAD_STRUCTURE;
    XrHirNominalSupplier *suppliers=NULL;
    XrXirStatus status=body_nominal_allocate(body,(size_t)row.count*sizeof(*suppliers),pool->control,(void **)&suppliers);
    if(status!=XR_XIR_OK)return status;
    for(uint32_t c=0;c<count && status==XR_XIR_OK;++c) {
        XrHirNominalComponent component=components[c];
        status=body_poll(pool->control);if(status!=XR_XIR_OK)break;
        if(component.value==UINT32_MAX || (component.kind!=XR_HIR_NOMINAL_EXPLICIT_VALUE &&
            component.kind!=XR_HIR_NOMINAL_SPREAD_VALUE)) {status=XR_XIR_BAD_STRUCTURE;break;}
        if(component.kind!=XR_HIR_NOMINAL_EXPLICIT_VALUE)continue;
        uint32_t field=0;
        status=xr_hir_body_nominal_find(body,row.input.shape,component.name,pool->control,&field,reason);
        if(status!=XR_XIR_OK)break;
        if(suppliers[field].kind) {status=body_nominal_failure(reason,XR_XIR_BAD_TYPE,"duplicate struct literal field");break;}
        XrHirNominalField resolved={0};
        status=xr_hir_body_nominal_field(body,row.input.shape,pool,field,&resolved,reason);
        if(status!=XR_XIR_OK)break;
        if(!body_nominal_composition_access(body,&row,&target,field))
            status=body_nominal_failure(reason,XR_XIR_BAD_TYPE,"field access is not permitted");
        else if(resolved.type!=component.type)status=body_nominal_failure(reason,XR_XIR_BAD_TYPE,"struct field type mismatch");
        else suppliers[field]=(XrHirNominalSupplier){.type=component.type,.component=c,.value=component.value,.kind=component.kind};
    }
    for(uint32_t c=0;c<count && status==XR_XIR_OK;++c) {
        XrHirNominalComponent component=components[c];
        if(component.kind!=XR_HIR_NOMINAL_SPREAD_VALUE)continue;
        XrHirNominalInfo source={0};
        if(component.name.bytes || component.name.length || !xr_hir_body_nominal_info(body,component.shape,&source) ||
            source.type!=component.type || source.kind!=XR_XIR_NOMINAL_STRUCT) {
            status=body_nominal_failure(reason,XR_XIR_BAD_TYPE,"struct spread source must be a struct value");break;
        }
        for(uint32_t f=0;f<source.count && status==XR_XIR_OK;++f) {
            XrHirNominalField from={0},to={0};uint32_t field=0;
            status=xr_hir_body_nominal_field(body,component.shape,pool,f,&from,reason);
            if(status==XR_XIR_OK)status=xr_hir_body_nominal_find(body,row.input.shape,from.name,pool->control,&field,reason);
            if(status==XR_XIR_OK)status=xr_hir_body_nominal_field(body,row.input.shape,pool,field,&to,reason);
            if(status!=XR_XIR_OK)break;
            if(from.type!=to.type) {status=body_nominal_failure(reason,XR_XIR_BAD_TYPE,"struct spread field type does not match the target field");break;}
            if(!body_nominal_composition_access(body,&row,&source,f) || !body_nominal_composition_access(body,&row,&target,field)) {
                status=body_nominal_failure(reason,XR_XIR_BAD_TYPE,"field access is not permitted");break;
            }
            if(suppliers[field].kind==XR_HIR_NOMINAL_EXPLICIT_VALUE)continue;
            if(suppliers[field].kind) {status=body_nominal_failure(reason,XR_XIR_BAD_TYPE,"struct field has more than one spread source; supply it explicitly");break;}
            suppliers[field]=(XrHirNominalSupplier){.shape=component.shape,.type=from.type,
                .component=c,.value=component.value,.field=f,.kind=XR_HIR_NOMINAL_SPREAD_VALUE};
        }
    }
    if(status!=XR_XIR_OK){xr_compile_resources_free(suppliers);return status;}
    HirBodyNominalPlan *owned=&((HirBodyNominalPlan *)body->nominal_plans.items)[plan.index];
    owned->suppliers=suppliers;owned->composed=true;*out=suppliers;return XR_XIR_OK;
}
