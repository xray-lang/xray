/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * Defaults retain their exact producer and instantiated type arguments. */
static XrXirStatus body_default_types_stage(XrHirBody *body,const HirBodySignature *call,
    const XrHirCallUseInput *input,XrCompileRequestControl *control,XrXirType **staged) {
    *staged=NULL;
    if(input->default_conversion==XR_XIR_INVALID) {
        if(input->default_value!=input->value)return XR_XIR_BAD_STRUCTURE;
    } else if((input->default_conversion!=XR_XIR_COPY&&input->default_conversion!=XR_XIR_FUNCTION_WEAKEN)||
        input->default_value==input->value)return XR_XIR_BAD_STRUCTURE;
    uint32_t count=input->substitution_count;
    if(count&&!input->substitution)return XR_XIR_BAD_STRUCTURE;
    if(call->default_types_ready) {
        if(count!=call->default_type_count)return XR_XIR_BAD_STRUCTURE;
        for(uint32_t p=0;p<count;++p) {
            if(!(p%64)){XrXirStatus status=body_poll(control);if(status!=XR_XIR_OK)return status;}
            if(input->substitution[p]!=call->default_types[p])return XR_XIR_BAD_STRUCTURE;
        }
        return XR_XIR_OK;
    }
    if(!count)return XR_XIR_OK;
    if(sizeof(**staged)>SIZE_MAX/count)return XR_XIR_OVERFLOW;
    XrXirStatus status=xir_compile_resource_status(xr_compile_resources_alloc(body->resources,
        (size_t)count*sizeof(**staged),(void **)staged));
    if(status==XR_XIR_OK)status=body_bytes(*staged,input->substitution,(size_t)count*sizeof(**staged),control);
    if(status!=XR_XIR_OK){xr_compile_resources_free(*staged);*staged=NULL;}
    return status;
}
static XrXirStatus body_default_project(const HirBodySignature *call,const HirBodyUse *use,
    const XrHirBodyProjection *projection,XrCompileRequestControl *control) {
    if(!projection->has_default||projection->default_owner!=call->declaration.function||
        projection->default_parameter!=use->action.formal||projection->default_value!=use->default_value||
        projection->default_call!=use->default_call||projection->default_conversion!=use->default_conversion||
        !call->default_types_ready||projection->default_type_argument_count!=call->default_type_count||
        (call->default_type_count&&!projection->default_type_arguments))return XR_XIR_BAD_STRUCTURE;
    for(uint32_t p=0;p<call->default_type_count;++p) {
        if(!(p%64)){XrXirStatus status=body_poll(control);if(status!=XR_XIR_OK)return status;}
        if(projection->default_type_arguments[p]!=call->default_types[p])return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}
