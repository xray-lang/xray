/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_class_stage.inc.c - Exact constructor field tuple declarations
 *
 * KEY CONCEPT:
 *   Signature correspondence is necessary and never grants a private call.
 */
XR_FUNC XrXirStatus xir_compile_class_stage_signature(const XrXirModule *module,
    uint32_t helper,XrCompileRequestControl *control) {
    XrXirStatus status=xir_receive_poll(control);if(status!=XR_XIR_OK)return status;
    if(!module || !module->functions || helper>=module->function_count || !module->declarations ||
        !module->declarations->functions || !module->types || !module->types->nominals)return XR_XIR_BAD_STRUCTURE;
    const XrXirFunctionIdentity *identity=&module->declarations->functions[helper];
    const XrXirNominalTable *table=module->types->nominals;
    if(identity->method_kind!=XR_XIR_CONSTRUCTOR_FIELDS || !identity->nominal_owner ||
        identity->nominal_owner>table->count || identity->member_access!=XR_XIR_MEMBER_PRIVATE ||
        identity->exported || identity->cleanup_owner || identity->native_callable || identity->test_role ||
        (identity->promises&~XR_XIR_FUNCTION_NO_SUSPEND))return XR_XIR_BAD_TYPE;
    uint32_t owner=identity->nominal_owner-1;
    uint32_t kind=table->declarations?table->declarations[owner].kind:table->identities[owner].kind;
    uint32_t arity=table->declarations?table->declarations[owner].parameter_count:table->identities[owner].arity;
    uint32_t native=table->declarations?table->declarations[owner].native.native_id:table->identities[owner].native.native_id;
    if(kind!=XR_XIR_NOMINAL_CLASS || arity || native)return XR_XIR_BAD_TYPE;
    uint32_t constructor=UINT32_MAX,steps=0;
    for(uint32_t f=0;f<module->function_count;++f) {
        status=xir_receive_step(control,&steps);if(status!=XR_XIR_OK)return status;
        const XrXirFunctionIdentity *candidate=&module->declarations->functions[f];
        if(candidate->nominal_owner!=identity->nominal_owner || candidate->method_kind!=XR_XIR_CONSTRUCTOR)continue;
        if(constructor!=UINT32_MAX)return XR_XIR_BAD_STRUCTURE;
        constructor=f;
    }
    if(constructor==UINT32_MAX)return XR_XIR_BAD_STRUCTURE;
    const XrXirFunctionIdentity *declared=&module->declarations->functions[constructor];
    const XrXirFunction *a=&module->functions[helper],*b=&module->functions[constructor];
    if(identity->module!=declared->module || declared->member_access ||
        declared->promises!=identity->promises || a->parameter_count!=b->parameter_count ||
        a->consume_parameter_count!=b->consume_parameter_count || a->error_type!=b->error_type ||
        (a->parameter_count && (!a->parameters || !b->parameters)) ||
        (a->consume_parameter_count && (!a->consume_parameters || !b->consume_parameters)) ||
        (module->generics && (module->generics[helper].parameter_count || module->generics[helper].argument_count ||
            module->generics[constructor].parameter_count || module->generics[constructor].argument_count)))return XR_XIR_BAD_TYPE;
    for(uint32_t p=0;p<a->parameter_count;++p)
        if(a->parameters[p]!=b->parameters[p] || xr_xir_type_is_cell(module->types,a->parameters[p]))return XR_XIR_BAD_TYPE;
    for(uint32_t p=0;p<a->consume_parameter_count;++p)
        if(a->consume_parameters[p]!=b->consume_parameters[p])return XR_XIR_BAD_TYPE;
    const XrXirTypeNode *subject=xr_xir_type_node(module->types,b->result);
    const XrXirTypeNode *tuple=xr_xir_type_node(module->types,a->result);
    if(!subject || subject->kind!=XR_XIR_TYPE_NOMINAL || subject->nominal.declaration!=owner ||
        subject->nominal.argument_count || subject->parameter_span)return XR_XIR_BAD_TYPE;
    if(!subject->nominal.field_count)return a->result==XR_XIR_UNIT?XR_XIR_OK:XR_XIR_BAD_TYPE;
    if(!tuple || tuple->kind!=XR_XIR_TYPE_TUPLE || tuple->parameter_count!=subject->nominal.field_count ||
        !tuple->parameters || !subject->nominal.fields || tuple->parameter_span)return XR_XIR_BAD_TYPE;
    for(uint32_t f=0;f<tuple->parameter_count;++f) {
        status=xir_receive_step(control,&steps);if(status!=XR_XIR_OK)return status;
        if(tuple->parameters[f].mode!=XR_PARAM_READ || tuple->parameters[f].type!=subject->nominal.fields[f])return XR_XIR_BAD_TYPE;
    }
    return XR_XIR_OK;
}

#include "xxir_class_stage_verify.inc.c"
