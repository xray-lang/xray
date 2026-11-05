/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xhir_body_array.inc.c - Exact Array receivers and physical emission anchors
 *
 * KEY CONCEPT:
 *   Array contexts own their ordered values. Primitive uses validate the real
 *   canonical instruction against the independently recorded emitted result.
 */
static XrXirStatus body_array_failure(const char **reason,const char *text) {
    if(reason)*reason=text;return XR_XIR_BAD_TYPE;
}
XR_FUNC XrXirStatus xr_hir_array_type(const XrHirArrayPool *pool,const XrHirArrayTypeInput *input,
    XrXirType *out,const char **reason) {
    if(reason)*reason=NULL;
    if(!pool||!pool->types||!pool->compile||!pool->compile->resources||!pool->module||!pool->intern||!input||!out)
        return XR_XIR_BAD_STRUCTURE;
    const XrXirModule *module=pool->module;
    if(module->types?module->types!=pool->types:(pool->types->count||pool->types->nominals||pool->types->interfaces))return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=body_poll(pool->control);if(status!=XR_XIR_OK)return status;
    XrXirType element=input->element;
    if(element==XR_XIR_UNIT||xr_xir_type_is_cell(pool->types,element))
        return body_array_failure(reason,"Array element must be a copyable storable type in this declaration");
    switch(input->scope) {
    case XR_HIR_ARRAY_NOMINAL_DECLARATION:case XR_HIR_ARRAY_INTERFACE_DECLARATION: {
        uint32_t count;
        if(input->scope==XR_HIR_ARRAY_NOMINAL_DECLARATION) {
            const XrXirNominalTable *table=pool->types->nominals;
            if(!table||!table->declarations||input->declaration>=table->count||input->extra_parameters)return XR_XIR_BAD_STRUCTURE;
            count=table->declarations[input->declaration].parameter_count;
        } else {
            const XrXirInterfaceTable *table=pool->types->interfaces;
            if(!table||!table->declarations||input->declaration>=table->count)return XR_XIR_BAD_STRUCTURE;
            count=table->declarations[input->declaration].parameter_count;
            if(count>65536||input->extra_parameters>65536-count)return XR_XIR_BAD_STRUCTURE;
            count+=input->extra_parameters;
        }
        status=xr_xir_type_span(pool->types,element)<=count?XR_XIR_OK:XR_XIR_BAD_TYPE;
        break;
    }
    case XR_HIR_ARRAY_FUNCTION_SIGNATURE: {
        if(input->function>=module->function_count||!module->functions||input->extra_parameters)return XR_XIR_BAD_STRUCTURE;
        uint32_t count=module->generics?module->generics[input->function].parameter_count:0;
        status=xr_xir_compile_type_expression_shape(pool->compile,pool->types,element,count);break;
    }
    case XR_HIR_ARRAY_FUNCTION_BODY: {
        if(input->function>=module->function_count||!module->functions||input->extra_parameters)return XR_XIR_BAD_STRUCTURE;
        XrXirProofContext context={module,{XR_XIR_CONTEXT_FUNCTION,input->function,0}};
        status=xr_xir_compile_type_use_verify(pool->compile,&context,element);
        if(status==XR_XIR_OK)status=xr_xir_compile_type_access(pool->compile,module,input->function,element);
        break;
    }
    default:return XR_XIR_BAD_STRUCTURE;
    }
    if(status!=XR_XIR_OK){if(reason)*reason="Array element must be a copyable storable type in this declaration";return status;}
    status=body_poll(pool->control);if(status!=XR_XIR_OK)return status;
    XrXirTypeNode node={.kind=XR_XIR_TYPE_ARRAY,.element=element};
    XrXirType received=XR_XIR_UNIT;
    status=pool->intern(pool->intern_owner,&node,&received);
    if(status!=XR_XIR_OK)return status;
    XrHirArrayInfo info={0};status=xr_hir_array_view(pool->types,received,pool->control,&info,reason);
    if(status==XR_XIR_OK&&info.element!=element)status=body_array_failure(reason,"Array receiving interner changed its element type");
    if(status==XR_XIR_OK)*out=received;
    return status;
}
XR_FUNC XrXirStatus xr_hir_array_view(const XrXirTypes *types,XrXirType type,
    XrCompileRequestControl *control,XrHirArrayInfo *out,const char **reason) {
    if(reason)*reason=NULL;if(!types||!out)return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=body_poll(control);if(status!=XR_XIR_OK)return status;
    if(!xr_xir_type_is_array(types,type))return body_array_failure(reason,"Array receiver is not an Array");
    XrXirType element=xr_xir_array_element(types,type);
    if(element==XR_XIR_UNIT||xr_xir_type_is_cell(types,element))
        return body_array_failure(reason,"Array element must be a copyable storable type in this declaration");
    *out=(XrHirArrayInfo){type,element};return XR_XIR_OK;
}
XR_FUNC bool xr_hir_body_array_info(const XrHirBody *body,XrHirArrayShape shape,XrHirArrayInfo *out) {
    if(!body||body->ended||shape.owner!=body||shape.index>=body->arrays.count||!out)return false;
    *out=((XrHirArrayInfo *)body->arrays.items)[shape.index];return true;
}
XR_FUNC XrXirStatus xr_hir_body_array_receive(XrHirBody *body,const XrXirTypes *types,XrXirType type,
    XrCompileRequestControl *control,XrHirArrayShape *out,const char **reason) {
    if(!body||body->ended||types!=body->types||!out)return XR_XIR_BAD_STRUCTURE;
    XrHirArrayInfo info={0};XrXirStatus status=xr_hir_array_view(types,type,control,&info,reason);
    if(status!=XR_XIR_OK)return status;
    uint32_t key=(uint32_t)type-XR_XIR_CONSTRUCTED_TYPE_BASE;
    if(key<body->array_map_capacity&&body->array_map[key]) {
        *out=(XrHirArrayShape){body,body->array_map[key]-1};return XR_XIR_OK;
    }
    uint32_t *map=body->array_map,capacity=body->array_map_capacity;
    if(key>=capacity) {
        capacity=types->count;if(key>=capacity||!capacity||sizeof(*map)>SIZE_MAX/capacity)return XR_XIR_BAD_STRUCTURE;
        map=NULL;status=xir_compile_resource_status(xr_compile_resources_alloc(body->resources,(size_t)capacity*sizeof(*map),(void **)&map));
        if(status==XR_XIR_OK)status=body_bytes(map,NULL,(size_t)capacity*sizeof(*map),control);
        if(status==XR_XIR_OK)status=body_bytes(map,body->array_map,(size_t)body->array_map_capacity*sizeof(*map),control);
    }
    HirBodyVector staged=body->arrays;
    if(status==XR_XIR_OK)status=body_vector_stage(body,body->arrays,sizeof(info),control,&staged);
    if(status==XR_XIR_OK)status=body_poll(control);
    if(status!=XR_XIR_OK) {
        if(map!=body->array_map)xr_compile_resources_free(map);
        if(staged.items!=body->arrays.items)xr_compile_resources_free(staged.items);return status;
    }
    body_vector_commit(&body->arrays,staged);
    if(map!=body->array_map){xr_compile_resources_free(body->array_map);body->array_map=map;body->array_map_capacity=capacity;}
    uint32_t index=body->arrays.count++;((XrHirArrayInfo *)body->arrays.items)[index]=info;
    body->array_map[key]=index+1;*out=(XrHirArrayShape){body,index};return XR_XIR_OK;
}
XR_FUNC XrXirStatus xr_hir_body_array_primitive(XrHirBody *body,const XrHirArrayPrimitiveInput *input,
    XrCompileRequestControl *control,XrHirArrayAction *out,const char **reason) {
    if(reason)*reason=NULL;
    if(!body||body->ended||body->array_sealed||!input||input->types!=body->types||!out||
        (input->count&&!input->values)||body_tuple_origin(body,input->scope,input->origin)!=XR_XIR_OK)return XR_XIR_BAD_STRUCTURE;
    XrHirArrayInfo info={0};if(!xr_hir_body_array_info(body,input->shape,&info))return XR_XIR_BAD_STRUCTURE;
    XrXirType result=info.type;uint32_t count=input->count;
    switch(input->operation) {
    case XR_XIR_ARRAY_NEW:break;
    case XR_XIR_ARRAY_REPEAT:if(count!=2||input->values[0].type!=XR_XIR_I64||input->values[1].type!=info.element)return XR_XIR_BAD_TYPE;break;
    case XR_XIR_ARRAY_WITH_CAPACITY:if(count!=1||input->values[0].type!=XR_XIR_I64)return XR_XIR_BAD_TYPE;break;
    case XR_XIR_ARRAY_LEN:case XR_XIR_ARRAY_CAPACITY:
        if(count!=1||input->values[0].type!=info.type)return XR_XIR_BAD_TYPE;result=XR_XIR_I64;break;
    case XR_XIR_ARRAY_GET:
        if(count!=2||input->values[0].type!=info.type||input->values[1].type!=XR_XIR_I64)return XR_XIR_BAD_TYPE;result=info.element;break;
    default:return XR_XIR_BAD_STRUCTURE;
    }
    XrHirArrayValue *values=NULL;XrXirStatus status=body_poll(control);
    if(count&&sizeof(*values)>SIZE_MAX/count)return XR_XIR_OVERFLOW;
    if(status==XR_XIR_OK&&count)status=xir_compile_resource_status(xr_compile_resources_alloc(body->resources,(size_t)count*sizeof(*values),(void **)&values));
    for(uint32_t p=0;status==XR_XIR_OK&&p<count;++p) {
        if(!(p%64))status=body_poll(control);if(status!=XR_XIR_OK)break;
        if(input->values[p].value==UINT32_MAX)status=XR_XIR_BAD_STRUCTURE;
        if(input->operation==XR_XIR_ARRAY_NEW&&input->values[p].type!=info.element)
            status=body_array_failure(reason,"Array literal elements require one exact type");
        values[p]=input->values[p];
    }
    XrHirBodyUse use={0};XrHirBodyInput typed={.origin=input->origin,.rule=XR_HIR_BODY_PROJECTION,.source=info.type,.target=result};
    if(status==XR_XIR_OK)status=xr_hir_body_check(body,body->types,&typed,control,&use,reason);
    if(status!=XR_XIR_OK){xr_compile_resources_free(values);return status;}
    HirBodyUse *row=&body->uses[use.index];row->array=input->shape;row->array_arguments=values;row->array_argument_count=count;
    row->required_instruction=true;row->action.operation=input->operation;
    *out=(XrHirArrayAction){use,input->shape,result,input->operation};return XR_XIR_OK;
}
static XrXirStatus body_array_project(const XrHirBody *body,const HirBodyUse *row,
    const XrHirBodyProjection *projection,XrCompileRequestControl *control) {
    if(!projection->has_instruction||!body->declarations.functions||!*body->declarations.functions||
        projection->function>=*body->declarations.function_count)return XR_XIR_BAD_STRUCTURE;
    XrXirFunction function=(*body->declarations.functions)[projection->function];
    if(projection->instruction>=function.instruction_count||!function.instructions)return XR_XIR_BAD_STRUCTURE;
    XrXirInstruction actual=function.instructions[projection->instruction];
    if(!row->array_emitted||projection->source_value!=row->array_result||
        !projection->source_value_map||row->array_result>=projection->source_value_count||
        projection->source_value_map[row->array_result]!=projection->value||
        actual.op!=row->action.operation||actual.type!=row->action.target||!projection->has_value||
        projection->value!=function.parameter_count+projection->instruction)return XR_XIR_BAD_STRUCTURE;
    bool table=xr_xir_op_uses_operand_table(actual.op);
    if(table&&(actual.args[1]!=row->array_argument_count||actual.args[0]>function.operand_count||
        actual.args[1]>function.operand_count-actual.args[0]||(actual.args[1]&&!function.operands)))return XR_XIR_BAD_STRUCTURE;
    if(table&&(projection->source_argument_count!=row->array_argument_count||
        (row->array_argument_count&&!projection->source_arguments)))return XR_XIR_BAD_STRUCTURE;
    for(uint32_t p=0;p<row->array_argument_count;++p) {
        if(!(p%64)){XrXirStatus status=body_poll(control);if(status!=XR_XIR_OK)return status;}
        XrHirArrayValue value=row->array_arguments[p];
        if(!projection->source_value_map||value.value>=projection->source_value_count)return XR_XIR_BAD_STRUCTURE;
        uint32_t mapped=projection->source_value_map[value.value],operand=table?function.operands[actual.args[0]+p]:actual.args[p];
        uint32_t source=table?projection->source_arguments[p]:projection->source_operands[p];
        if(source!=value.value||mapped!=operand||!body_tuple_value_type(&function,operand,value.type))return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}
XR_FUNC XrXirStatus xr_hir_body_array_literal(XrHirBody *body,const XrHirArrayLiteralInput *input,
    XrCompileRequestControl *control,XrHirArrayLiteral *out,const char **reason) {
    if(reason)*reason=NULL;
    if(!body||body->ended||body->array_sealed||!input||input->types!=body->types||!out||
        body_tuple_origin(body,input->scope,input->origin)!=XR_XIR_OK)return XR_XIR_BAD_STRUCTURE;
    XrXirType element=XR_XIR_UNIT;
    if(input->has_expected) {
        if(!xr_xir_type_is_array(body->types,input->expected))
            return body_array_failure(reason,"Array literal cannot satisfy a non-Array context");
        element=xr_xir_array_element(body->types,input->expected);
    } else if(!input->count)return body_array_failure(reason,"empty Array literal needs an element context");
    uint32_t count=input->count;XrHirArrayValue *values=NULL;
    if(count&&sizeof(*values)>SIZE_MAX/count)return XR_XIR_OVERFLOW;
    XrXirStatus status=body_poll(control);
    if(status==XR_XIR_OK&&count)status=xir_compile_resource_status(xr_compile_resources_alloc(body->resources,(size_t)count*sizeof(*values),(void **)&values));
    if(status==XR_XIR_OK)status=body_bytes(values,NULL,(size_t)count*sizeof(*values),control);
    HirBodyVector staged=body->array_literals;
    if(status==XR_XIR_OK)status=body_vector_stage(body,body->array_literals,sizeof(HirBodyArrayLiteral),control,&staged);
    if(status!=XR_XIR_OK){xr_compile_resources_free(values);if(staged.items!=body->array_literals.items)xr_compile_resources_free(staged.items);return status;}
    body_vector_commit(&body->array_literals,staged);uint32_t index=body->array_literals.count++;
    ((HirBodyArrayLiteral *)body->array_literals.items)[index]=(HirBodyArrayLiteral){.scope=input->scope,.origin=input->origin,.values=values,
        .element=element,.count=count,.present=input->has_expected,.inferred=input->inferred};
    *out=(XrHirArrayLiteral){body,index};return XR_XIR_OK;
}
XR_FUNC bool xr_hir_body_array_expected(const XrHirBody *body,XrHirArrayLiteral handle,XrHirArrayExpected *out) {
    if(!body||body->ended||handle.owner!=body||handle.index>=body->array_literals.count||!out)return false;
    HirBodyArrayLiteral row=((HirBodyArrayLiteral *)body->array_literals.items)[handle.index];
    *out=(XrHirArrayExpected){row.element,row.present,row.inferred};return true;
}
XR_FUNC XrXirStatus xr_hir_body_array_append(XrHirBody *body,XrHirArrayLiteral handle,uint32_t ordinal,
    XrHirArrayValue value,XrCompileRequestControl *control,const char **reason) {
    if(reason)*reason=NULL;
    if(!body||body->ended||body->array_sealed||handle.owner!=body||handle.index>=body->array_literals.count)return XR_XIR_BAD_STRUCTURE;
    HirBodyArrayLiteral *row=&((HirBodyArrayLiteral *)body->array_literals.items)[handle.index];
    if(row->complete||ordinal!=row->received||ordinal>=row->count||value.value==UINT32_MAX)return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=body_poll(control);if(status!=XR_XIR_OK)return status;
    if(row->present&&value.type!=row->element)return body_array_failure(reason,"Array literal elements require one exact type");
    if(!row->present){row->element=value.type;row->present=true;row->inferred=true;}
    row->values[row->received++]=value;return XR_XIR_OK;
}
XR_FUNC XrXirStatus xr_hir_body_array_literal_finish(XrHirBody *body,XrHirArrayLiteral handle,XrHirArrayShape shape,
    XrCompileRequestControl *control,XrHirArrayAction *out,const char **reason) {
    if(!body||body->ended||handle.owner!=body||handle.index>=body->array_literals.count)return XR_XIR_BAD_STRUCTURE;
    HirBodyArrayLiteral row=((HirBodyArrayLiteral *)body->array_literals.items)[handle.index];XrHirArrayInfo info={0};
    if(row.complete||row.received!=row.count||!row.present||!xr_hir_body_array_info(body,shape,&info)||info.element!=row.element)return XR_XIR_BAD_STRUCTURE;
    XrHirArrayPrimitiveInput input={.types=body->types,.scope=row.scope,.origin=row.origin,.shape=shape,.operation=XR_XIR_ARRAY_NEW,.values=row.values,.count=row.count};
    XrXirStatus status=xr_hir_body_array_primitive(body,&input,control,out,reason);
    if(status==XR_XIR_OK)((HirBodyArrayLiteral *)body->array_literals.items)[handle.index].complete=true;
    return status;
}
