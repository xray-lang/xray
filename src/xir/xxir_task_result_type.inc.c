/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_task_result_type.inc.c - Same-pool logical outcome arguments and storage
 */
static bool task_result_error_shape(const XrXirTypes *types,XrXirType error) {
    if(error==XR_XIR_ERROR || error==XR_XIR_NEVER || xr_xir_type_is_enum(types,error))return true;
    uint32_t id=(uint32_t)error;
    if(id>=XR_XIR_TYPE_PARAMETER_BASE && id<XR_XIR_TYPE_PARAMETER_LIMIT)return true;
    const XrXirTypeNode *node=xr_xir_type_node(types,error);
    return node && node->kind==XR_XIR_TYPE_UNION;
}
XR_FUNC bool xr_xir_task_result_type_info(const XrXirTypes *types,XrXirType type,
    XrXirTaskResultType *output) {
    if(!output || !types || !types->nominals ||
        (!!types->nominals->declarations==!!types->nominals->identities))return false;
    const XrXirTypeNode *node=xr_xir_type_node(types,type);
    if(!node || node->kind!=XR_XIR_TYPE_NOMINAL ||
        node->nominal.declaration>=types->nominals->count || node->nominal.argument_count!=2 ||
        !node->nominal.arguments || node->parameters || node->parameter_count ||
        node->element!=XR_XIR_UNIT || node->result!=XR_XIR_UNIT || node->flags)return false;
    XrXirNominalIdentity identity=nominal_identity_header(types->nominals,node->nominal.declaration);
    if(identity.native.native_id!=XR_NATIVE_DECLARATION_TASK_RESULT ||
        nominal_native_verify(&identity)!=XR_XIR_OK)return false;
    XrXirType element=node->nominal.arguments[0],error=node->nominal.arguments[1];
    if(!task_result_error_shape(types,error))return false;
    if(node->nominal.field_count) {
        if(node->nominal.field_count!=3 || !node->nominal.fields ||
            node->nominal.fields[0]!=element || node->nominal.fields[1]!=XR_XIR_ERROR ||
            node->nominal.fields[2]!=XR_XIR_PANIC_INFO)return false;
    } else {
        if(!types->nominals->declarations || node->nominal.fields)return false;
        const XrXirNominalDeclaration *d=&types->nominals->declarations[node->nominal.declaration];
        if(!d->fields || d->field_count!=3 || !d->constraints ||
            d->fields[0].type!=(XrXirType)XR_XIR_TYPE_PARAMETER_BASE ||
            d->fields[1].type!=XR_XIR_ERROR ||
            d->fields[2].type!=XR_XIR_PANIC_INFO ||
            !(d->constraints[1].markers&XR_XIR_CONSTRAINT_ERROR))return false;
    }
    *output=(XrXirTaskResultType){type,element,error};return true;
}
XR_FUNC bool xr_xir_task_result_matches_task(const XrXirTypes *types,
    XrXirType result,XrXirType task) {
    XrXirTaskResultType info={0};
    const XrXirTypeNode *node=xr_xir_type_node(types,task);
    return xr_xir_task_result_type_info(types,result,&info) && node &&
        node->kind==XR_XIR_TYPE_TASK && node->element==info.element && node->result==info.error;
}
