/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_owned_function.inc.c - Exact code bindings recovered from genuine Env owners
 */
typedef struct RbcOwnedFunction {
    const XrRbcCodeOwner *code;
    XrtEnvironmentBorrow environment;
    uint32_t function;
} RbcOwnedFunction;

/* A raw dependency always precedes its parent. One receiving plan per raw id
 * lets this cold check walk downward without recursion or a second type pool. */
static bool rbc_owned_dependency(const XrtValuePlan **plans, uint32_t id,
    const XrtValuePlan *plan, uint32_t before) {
    if (!id || id>=before || !plan || (plans[id-1] && plans[id-1]!=plan)) return false;
    plans[id-1]=plan;
    return true;
}
static XrtManagedStatus rbc_owned_plan_matches(const XrRbcCodeOwner *code,
    const XrtValuePlan *plan, uint32_t id) {
    const XrtValueImage *image=code->values;
    if (!image || !id || id>image->type_count) return XRT_MANAGED_ARGUMENT;
    size_t count=image->type_count;
    if (count>SIZE_MAX/sizeof(const XrtValuePlan *)) return XRT_MANAGED_OVERFLOW;
    const XrtValuePlan **plans=xr_calloc(count,sizeof(*plans));
    if (!plans) return XRT_MANAGED_OOM;
    plans[id-1]=plan;
    bool valid=true;
    for (uint32_t current=id;valid && current;--current) {
        if (!plans[current-1]) continue;
        const XrtValueImageType *row=&image->types[current-1];
        XrtValuePlanSpec view;
        valid=xrt_value_plan_view(plans[current-1],&view)==XRT_MANAGED_OK && view.kind==row->kind &&
            view.size==row->size && view.alignment==row->alignment && view.count==row->count &&
            view.owned_count==row->owned_count;
        for (uint32_t n=0;valid && n<row->owned_count;++n)
            valid=view.owned_offsets[n]==image->owned_offsets[row->first_owned+n];
        if (!valid) break;
        if (row->kind==XRT_VALUE_FUNCTION || row->kind==XRT_VALUE_ENVIRONMENT)
            valid=xr_rbc_code_owner_borrow(view.code)==code;
        else valid=!view.code.context && !view.code.retain && !view.code.drop;
        if (!valid) break;
        if (row->kind==XRT_VALUE_FUNCTION) {
            valid=view.signature.parameter_count==row->parameter_count && view.signature.flags==row->flags &&
                view.signature.throws==(row->throws!=0) &&
                rbc_owned_dependency(plans,row->result,view.signature.result,current);
            for (uint32_t n=0;valid && n<row->parameter_count;++n) {
                XrtValueImageParameter parameter=image->parameters[row->first_parameter+n];
                XrtValueParameterSpec actual=view.signature.parameters[n];
                valid=(uint32_t)actual.mode==parameter.mode && actual.escaping==(parameter.escaping!=0) &&
                    rbc_owned_dependency(plans,parameter.type,actual.type,current);
            }
        } else if (row->kind==XRT_VALUE_ENVIRONMENT) {
            valid=view.entry.ordinal==row->entry_ordinal && view.capture_count==row->capture_count &&
                rbc_owned_dependency(plans,row->entry_signature,view.entry.signature,current);
            for (uint32_t n=0;valid && n<row->capture_count;++n) {
                XrtValueImageSlot capture=image->slots[row->first_capture+n];
                valid=view.captures[n].offset==capture.offset &&
                    rbc_owned_dependency(plans,capture.type,view.captures[n].type,current);
            }
        } else valid=row->kind==XRT_VALUE_UNIT || row->kind==XRT_VALUE_BOOL || row->kind==XRT_VALUE_I64 ||
            row->kind==XRT_VALUE_U8 || row->kind==XRT_VALUE_U64 || row->kind==XRT_VALUE_F64 ||
            (row->kind>=XRT_VALUE_STRING && row->kind<=XRT_VALUE_MAP_STRING_I64);
    }
    xr_free(plans);
    return valid ? XRT_MANAGED_OK : XRT_MANAGED_ARGUMENT;
}
static XrtManagedStatus rbc_owned_function_borrow(const XrtFunction *binding, RbcOwnedFunction *output) {
    if (!binding || !binding->entry || !binding->owner || !output) return XRT_MANAGED_ARGUMENT;
    XrtEnvironmentBorrow environment={0};
    XrtManagedStatus status=xrt_environment_borrow_owned(binding->owner,&environment);
    if (status!=XRT_MANAGED_OK) return status;
    if (binding->environment!=(uintptr_t)environment.payload) return XRT_MANAGED_ARGUMENT;
    XrtValuePlanSpec shape;
    status=xrt_value_plan_view(environment.plan,&shape);
    if (status!=XRT_MANAGED_OK) return status;
    const XrRbcCodeOwner *code=xr_rbc_code_owner_borrow(shape.code);
    if (!code || !code->values) return XRT_MANAGED_ARGUMENT;
    for (size_t n=0;n<code->function_count;++n) {
        const XrRbcCallableEntry *entry=&code->entries[n];
        if (binding->entry!=entry) continue;
        if (entry->source_function!=shape.entry.ordinal || !entry->environment_type ||
            entry->producer!=code || entry->physical_function!=n ||
            code->functions[n].state_capture || !entry->signature_type) return XRT_MANAGED_ARGUMENT;
        status=rbc_owned_plan_matches(code,environment.plan,entry->environment_type);
        if (status!=XRT_MANAGED_OK) return status;
        *output=(RbcOwnedFunction){code,environment,(uint32_t)n};
        return XRT_MANAGED_OK;
    }
    return XRT_MANAGED_ARGUMENT;
}
