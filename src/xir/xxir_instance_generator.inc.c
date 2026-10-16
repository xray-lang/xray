/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_instance_generator.inc.c - Creation from sealed canonical instruction sites
 *
 * KEY CONCEPT:
 *   Only the owning executor holds code authority for a live producer.
 */
XR_FUNC XrXirCallStatus xr_xir_instance_generator_at(XrXirCallView *view,uint32_t instruction,
    const XrXirValue *arguments,uint32_t count,XrXirValue *output) {
    if(!view || !instance_view_current(view)) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirInstance *instance=view_instance(view);
    if(!instance) return XR_XIR_CALL_BAD_STATE;
    if(!output || !value_unit(*output) || (count && !arguments)) return XR_XIR_CALL_BAD_ARGUMENT;
    const XrXirProgramPermissions *permissions=instance->program->permissions;
    uint32_t caller=xr_xir_call_current_entry(view->activation);
    if(!permissions || permissions->function_count!=instance->program->entry_count ||
        caller>=permissions->function_count) return XR_XIR_CALL_BAD_STATE;
    const XrXirProgramPermission *owner=&permissions->entries[caller];
    if(owner->generator_begin>permissions->generator_count ||
        owner->generator_count>permissions->generator_count-owner->generator_begin ||
        (owner->generator_count && !permissions->generators)) return XR_XIR_CALL_BAD_STATE;
    XrXirValueAdmission *admission=xr_xir_call_admission(view);
    uint64_t work=(uint64_t)owner->generator_count*4+2;
    if(!admission || work>admission->work || !xr_xir_domain_work(instance->domain,work)) return XR_XIR_CALL_LIMIT;
    admission->work-=work;
    const XrXirProgramGeneratorSite *selected=NULL;
    for(uint32_t i=0;i<owner->generator_count;++i) {
        const XrXirProgramGeneratorSite *site=&permissions->generators[owner->generator_begin+i];
        if(site->instruction!=instruction) continue;
        if(selected) return XR_XIR_CALL_BAD_STATE;
        selected=site;
    }
    if(!selected || selected->entry>=instance->program->entry_count || selected->arguments!=count)
        return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirCallRequest request={selected->entry,arguments,count,instance_cell_role,instance->program};
    return xr_xir_task_executor_generator(instance->executor,view,selected->type,&request,output);
}
