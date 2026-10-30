/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * Tuple metadata and executable uses share one ordered logical shape. */
static XrXirStatus body_tuple_failure(const char **reason,XrXirStatus status,const char *message) {
    if(reason)*reason=message;return status;
}
static bool body_tuple_type_known(const XrXirTypes *types,XrXirType type) {
    uint32_t id=(uint32_t)type;
    return id<=XR_XIR_NEVER?(id!=4):id>=XR_XIR_TYPE_PARAMETER_BASE?
        id<XR_XIR_TYPE_PARAMETER_LIMIT:xr_xir_type_node(types,type)!=NULL;
}
XR_FUNC XrHirTupleKind xr_hir_tuple_kind(const XrXirTypes *types,XrXirType type) {
    return !types?XR_HIR_TUPLE_OTHER:type==XR_XIR_UNIT?XR_HIR_TUPLE_UNIT:
        xr_xir_tuple_signature(types,type)?XR_HIR_TUPLE_ORDERED:XR_HIR_TUPLE_OTHER;
}
XR_FUNC XrXirStatus xr_hir_tuple_view(const XrXirTypes *types,XrXirType type,
    XrCompileRequestControl *control,XrHirTupleView *out,const char **reason) {
    if(reason)*reason=NULL;
    if(!types||!out)return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=body_poll(control);if(status!=XR_XIR_OK)return status;
    XrHirTupleView view={.owner=types,.type=type};
    if(type!=XR_XIR_UNIT) {
        const XrXirTypeNode *node=xr_xir_tuple_signature(types,type);
        if(!node||!node->parameter_count||!node->parameters)
            return body_tuple_failure(reason,XR_XIR_BAD_TYPE,"Tuple requires a statically known ordered shape");
        view.fields=node->parameters;view.count=node->parameter_count;
        for(uint32_t p=0;p<view.count;++p) {
            if(!(p%64)){status=body_poll(control);if(status!=XR_XIR_OK)return status;}
            if(view.fields[p].mode||!body_tuple_type_known(types,view.fields[p].type))return XR_XIR_BAD_TYPE;
            if(view.fields[p].type!=XR_XIR_UNIT)++view.physical_count;
        }
    }
    status=body_poll(control);if(status==XR_XIR_OK)*out=view;return status;
}
XR_FUNC XrXirStatus xr_hir_tuple_type(const XrHirTuplePool *pool,const XrXirCallableParameter *fields,
    uint32_t count,XrXirType *out,const char **reason) {
    if(reason)*reason=NULL;
    if(!pool||!pool->types||!pool->compile||!pool->compile->resources||!out||(count&&!fields))return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=body_poll(pool->control);if(status!=XR_XIR_OK)return status;
    if(!count){*out=XR_XIR_UNIT;return XR_XIR_OK;}
    if(!pool->intern||sizeof(*fields)>SIZE_MAX/count)return XR_XIR_BAD_STRUCTURE;
    XrXirCallableParameter *copy=NULL;
    status=xir_compile_resource_status(xr_compile_resources_alloc(pool->compile->resources,(size_t)count*sizeof(*copy),(void **)&copy));
    if(status==XR_XIR_OK)status=body_bytes(copy,fields,(size_t)count*sizeof(*copy),pool->control);
    for(uint32_t p=0;status==XR_XIR_OK&&p<count;++p) {
        if(!(p%64))status=body_poll(pool->control);
        if(status==XR_XIR_OK&&(copy[p].mode||!body_tuple_type_known(pool->types,copy[p].type)))status=XR_XIR_BAD_TYPE;
    }
    XrXirType type=XR_XIR_UNIT;
    if(status==XR_XIR_OK) {
        XrXirTypeNode node={.kind=XR_XIR_TYPE_TUPLE,.parameters=copy,.parameter_count=count};
        status=pool->intern(pool->intern_owner,&node,&type);
    }
    XrHirTupleView received={0};
    if(status==XR_XIR_OK)status=xr_hir_tuple_view(pool->types,type,pool->control,&received,reason);
    if(status==XR_XIR_OK&&received.count!=count)status=XR_XIR_BAD_STRUCTURE;
    for(uint32_t p=0;status==XR_XIR_OK&&p<count;++p) {
        if(!(p%64))status=body_poll(pool->control);
        if(status==XR_XIR_OK&&received.fields[p].type!=copy[p].type)status=XR_XIR_BAD_STRUCTURE;
    }
    xr_compile_resources_free(copy);if(status==XR_XIR_OK)*out=type;return status;
}
XR_FUNC XrXirStatus xr_hir_tuple_field_type(const XrXirTypes *types,XrXirType type,uint32_t field,
    XrCompileRequestControl *control,XrXirType *out,const char **reason) {
    XrHirTupleView view={0};if(!out)return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=xr_hir_tuple_view(types,type,control,&view,reason);if(status!=XR_XIR_OK)return status;
    if(field>=view.count)return body_tuple_failure(reason,XR_XIR_BAD_TYPE,"Tuple field index exceeds its ordered arity");
    *out=view.fields[field].type;return XR_XIR_OK;
}
XR_FUNC XrXirStatus xr_hir_tuple_arity(const XrXirTypes *types,XrXirType type,uint32_t count,
    XrCompileRequestControl *control,const char **reason) {
    XrHirTupleView view={0};XrXirStatus status=xr_hir_tuple_view(types,type,control,&view,reason);
    if(status!=XR_XIR_OK)return status;
    return view.count==count?XR_XIR_OK:body_tuple_failure(reason,XR_XIR_BAD_TYPE,"Tuple requires its exact ordered arity");
}
XR_FUNC XrXirStatus xr_hir_tuple_index(const char *name,size_t length,
    XrCompileRequestControl *control,uint32_t *out,const char **reason) {
    if(reason)*reason=NULL;
    if(!name||!out)return XR_XIR_BAD_STRUCTURE;
    if(!length)return body_tuple_failure(reason,XR_XIR_BAD_TYPE,"Tuple field index is empty");
    uint32_t index=0;
    for(size_t p=0;p<length;++p) {
        if(!(p%64)){XrXirStatus status=body_poll(control);if(status!=XR_XIR_OK)return status;}
        unsigned char digit=(unsigned char)name[p];
        if(digit<'0'||digit>'9'||index>(UINT32_MAX-(digit-'0'))/10)
            return body_tuple_failure(reason,XR_XIR_BAD_TYPE,"Tuple field requires a bounded decimal index");
        index=index*10+(digit-'0');
    }
    XrXirStatus status=body_poll(control);if(status==XR_XIR_OK)*out=index;return status;
}
XR_FUNC bool xr_hir_body_tuple_info(const XrHirBody *body,XrHirTupleShape shape,XrHirTupleInfo *out) {
    if(!body||body->ended||shape.owner!=body||shape.index>=body->tuples.count||!out)return false;
    HirBodyTuple tuple=((HirBodyTuple *)body->tuples.items)[shape.index];
    *out=(XrHirTupleInfo){tuple.type,tuple.fields,tuple.physical_fields,tuple.count,tuple.physical_count};return true;
}
XR_FUNC XrXirStatus xr_hir_body_tuple_receive(XrHirBody *body,const XrXirTypes *types,XrXirType type,
    XrCompileRequestControl *control,XrHirTupleShape *out,const char **reason) {
    if(reason)*reason=NULL;
    if(!body||body->ended||types!=body->types||!out)return XR_XIR_BAD_STRUCTURE;
    uint32_t key=type==XR_XIR_UNIT?0:(uint32_t)type-XR_XIR_CONSTRUCTED_TYPE_BASE+1;
    if(type!=XR_XIR_UNIT&&!xr_xir_tuple_signature(types,type))return XR_XIR_BAD_TYPE;
    XrXirStatus status=body_poll(control);if(status!=XR_XIR_OK)return status;
    if(key<body->tuple_map_capacity&&body->tuple_map[key]){
        *out=(XrHirTupleShape){body,body->tuple_map[key]-1};return XR_XIR_OK;
    }
    XrHirTupleView view={0};status=xr_hir_tuple_view(types,type,control,&view,reason);if(status!=XR_XIR_OK)return status;
    HirBodyTuple tuple={.type=type,.count=view.count,.physical_count=view.physical_count};
    XR_DCHECK(tuple.physical_count<=tuple.count,"tuple payload count cannot exceed its logical arity");
    size_t field_bytes=(size_t)view.count*sizeof(*tuple.fields),map_bytes=(size_t)view.physical_count*sizeof(*tuple.physical_fields);
    if(view.count&&field_bytes/view.count!=sizeof(*tuple.fields))return XR_XIR_OVERFLOW;
    if(map_bytes>SIZE_MAX-field_bytes)return XR_XIR_OVERFLOW;
    if(field_bytes+map_bytes)status=xir_compile_resource_status(xr_compile_resources_alloc(body->resources,field_bytes+map_bytes,(void **)&tuple.fields));
    if(status==XR_XIR_OK&&tuple.fields)tuple.physical_fields=(uint32_t *)((unsigned char *)tuple.fields+field_bytes);
    for(uint32_t p=0,a=0;status==XR_XIR_OK&&p<view.count;++p) {
        if(!(p%64))status=body_poll(control);if(status!=XR_XIR_OK)break;
        bool unit=view.fields[p].type==XR_XIR_UNIT;
        tuple.fields[p]=(XrHirTupleField){view.fields[p].type,unit?UINT32_MAX:a};
        if(!unit)tuple.physical_fields[a++]=p;
    }
    uint32_t *map=body->tuple_map,capacity=body->tuple_map_capacity;
    if(status==XR_XIR_OK&&key>=capacity) {
        capacity=types->count+1;if(capacity<=key)status=XR_XIR_BAD_STRUCTURE;
        if(status==XR_XIR_OK){map=NULL;status=xir_compile_resource_status(xr_compile_resources_alloc(body->resources,(size_t)capacity*sizeof(*map),(void **)&map));}
        if(status==XR_XIR_OK)status=body_bytes(map,NULL,(size_t)capacity*sizeof(*map),control);
        if(status==XR_XIR_OK)status=body_bytes(map,body->tuple_map,(size_t)body->tuple_map_capacity*sizeof(*map),control);
    }
    HirBodyVector staged=body->tuples;
    if(status==XR_XIR_OK)status=body_vector_stage(body,body->tuples,sizeof(tuple),control,&staged);
    if(status==XR_XIR_OK)status=body_poll(control);
    if(status!=XR_XIR_OK) {
        if(map!=body->tuple_map)xr_compile_resources_free(map);
        if(staged.items!=body->tuples.items)xr_compile_resources_free(staged.items);
        xr_compile_resources_free(tuple.fields);return status;
    }
    body_vector_commit(&body->tuples,staged);
    XR_DCHECK(body->tuples.count<body->tuples.capacity,"received tuple publication has reserved storage");
    if(map!=body->tuple_map){xr_compile_resources_free(body->tuple_map);body->tuple_map=map;body->tuple_map_capacity=capacity;}
    uint32_t index=body->tuples.count;((HirBodyTuple *)body->tuples.items)[index]=tuple;
    body->tuple_map[key]=++body->tuples.count;*out=(XrHirTupleShape){body,index};return XR_XIR_OK;
}
XR_FUNC XrXirStatus xr_hir_body_tuple_context(XrHirBody *body,const XrHirTupleContextInput *input,
    XrCompileRequestControl *control,XrHirTupleContext *out,const char **reason) {
    if(reason)*reason=NULL;
    if(!body||body->ended||!input||input->types!=body->types||!out||
        (input->kind!=XR_HIR_TUPLE_EXACT&&input->kind!=XR_HIR_TUPLE_PREFIX))return XR_XIR_BAD_STRUCTURE;
    XrHirTupleView view={0};uint32_t count=input->count;
    XrXirStatus status=body_poll(control);if(status!=XR_XIR_OK)return status;
    if(input->from_type){
        status=xr_hir_tuple_view(body->types,input->type,control,&view,reason);if(status!=XR_XIR_OK)return status;
        if(input->count!=UINT32_MAX&&input->count!=view.count)
            return body_tuple_failure(reason,XR_XIR_BAD_TYPE,"Tuple literal requires an exact ordered arity context");
        count=view.count;
    }
    else if(count&&!input->fields)return XR_XIR_BAD_STRUCTURE;
    XrHirTupleExpected *fields=NULL;
    if(count)status=xir_compile_resource_status(xr_compile_resources_alloc(body->resources,(size_t)count*sizeof(*fields),(void **)&fields));
    for(uint32_t p=0;status==XR_XIR_OK&&p<count;++p) {
        if(!(p%64))status=body_poll(control);if(status!=XR_XIR_OK)break;
        fields[p]=input->from_type?(XrHirTupleExpected){view.fields[p].type,true,false,input->inferred}:input->fields[p];
        if(fields[p].present&&!body_tuple_type_known(body->types,fields[p].type))status=XR_XIR_BAD_TYPE;
    }
    HirBodyVector staged=body->tuple_contexts;
    if(status==XR_XIR_OK)status=body_vector_stage(body,body->tuple_contexts,sizeof(HirBodyTupleContext),control,&staged);
    if(status==XR_XIR_OK)status=body_poll(control);
    if(status!=XR_XIR_OK){xr_compile_resources_free(fields);if(staged.items!=body->tuple_contexts.items)xr_compile_resources_free(staged.items);return status;}
    body_vector_commit(&body->tuple_contexts,staged);
    XR_DCHECK(body->tuple_contexts.count<body->tuple_contexts.capacity,"tuple context publication has reserved storage");
    uint32_t index=body->tuple_contexts.count++;
    ((HirBodyTupleContext *)body->tuple_contexts.items)[index]=(HirBodyTupleContext){fields,count,input->kind};
    *out=(XrHirTupleContext){body,index};return XR_XIR_OK;
}
XR_FUNC bool xr_hir_body_tuple_expected(const XrHirBody *body,XrHirTupleContext context,uint32_t index,XrHirTupleExpected *out) {
    if(!body||body->ended||context.owner!=body||context.index>=body->tuple_contexts.count||!out)return false;
    HirBodyTupleContext row=((HirBodyTupleContext *)body->tuple_contexts.items)[context.index];
    if(index>=row.count)return false;*out=row.fields[index];return true;
}
XR_FUNC bool xr_hir_body_tuple_context_size(const XrHirBody *body,XrHirTupleContext context,uint32_t *out) {
    if(!body||body->ended||context.owner!=body||context.index>=body->tuple_contexts.count||!out)return false;
    *out=((HirBodyTupleContext *)body->tuple_contexts.items)[context.index].count;return true;
}
XR_FUNC XrXirStatus xr_hir_body_tuple_context_slice(XrHirBody *body,XrHirTupleContext context,
    uint32_t offset,uint32_t count,XrCompileRequestControl *control,XrHirTupleContext *out) {
    if(!body||body->ended||context.owner!=body||context.index>=body->tuple_contexts.count)return XR_XIR_BAD_STRUCTURE;
    HirBodyTupleContext row=((HirBodyTupleContext *)body->tuple_contexts.items)[context.index];
    if(offset>row.count||count>row.count-offset)return XR_XIR_BAD_STRUCTURE;
    XrHirTupleContextInput input={.types=body->types,.fields=count?row.fields+offset:NULL,.count=count,.kind=XR_HIR_TUPLE_PREFIX};
    return xr_hir_body_tuple_context(body,&input,control,out,NULL);
}
XR_FUNC XrXirStatus xr_hir_body_tuple_arity(const XrHirBody *body,XrHirTupleShape shape,uint32_t count,const char **reason) {
    XrHirTupleInfo tuple={0};
    if(!xr_hir_body_tuple_info(body,shape,&tuple))return XR_XIR_BAD_STRUCTURE;
    return tuple.count==count?XR_XIR_OK:body_tuple_failure(reason,XR_XIR_BAD_TYPE,"Tuple requires its exact ordered arity");
}
static XrXirStatus body_tuple_origin(const XrHirBody *body,XrHirBodyScope scope,XrHirBodyOrigin origin) {
    if(scope.owner!=body||scope.index>=body->scopes.count)return XR_XIR_BAD_STRUCTURE;
    HirBodyScope row=((HirBodyScope *)body->scopes.items)[scope.index];
    return row.origin.function==origin.function&&row.origin.module==origin.module?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
}
XR_FUNC XrXirStatus xr_hir_body_tuple_construct(XrHirBody *body,const XrHirTupleConstructInput *input,
    XrCompileRequestControl *control,XrHirTupleAction *out,const char **reason) {
    if(reason)*reason=NULL;
    if(!body||body->ended||!input||input->types!=body->types||!out)return XR_XIR_BAD_STRUCTURE;
    XrHirTupleInfo tuple={0};
    if(!xr_hir_body_tuple_info(body,input->shape,&tuple)||(input->count&&!input->values)||
        body_tuple_origin(body,input->scope,input->origin)!=XR_XIR_OK)return XR_XIR_BAD_STRUCTURE;
    if(tuple.count!=input->count)return body_tuple_failure(reason,XR_XIR_BAD_TYPE,"Tuple literal requires an exact ordered arity context");
    HirBodyTupleContext *context=NULL;
    if(input->context.owner) {
        if(input->context.owner!=body||input->context.index>=body->tuple_contexts.count)return XR_XIR_BAD_STRUCTURE;
        context=&((HirBodyTupleContext *)body->tuple_contexts.items)[input->context.index];
        if(input->context_offset>context->count||input->count>context->count-input->context_offset||
            (context->kind==XR_HIR_TUPLE_EXACT&&input->count!=context->count-input->context_offset))
            return body_tuple_failure(reason,XR_XIR_BAD_TYPE,"Tuple literal requires an exact ordered arity context");
    }
    XrXirStatus status=body_poll(control);
    for(uint32_t p=0;status==XR_XIR_OK&&p<tuple.count;++p) {
        if(!(p%64))status=body_poll(control);if(status!=XR_XIR_OK)break;
        if(input->values[p].type!=tuple.fields[p].type)return XR_XIR_BAD_TYPE;
        if(context&&context->fields[input->context_offset+p].present) {
            XrHirTupleExpected expected=context->fields[input->context_offset+p];
            if(expected.infer_result) {
                if(!input->conversion||!input->conversion->compile||input->conversion->compile->resources!=body->resources||
                    input->conversion->types!=body->types)return XR_XIR_BAD_STRUCTURE;
                XrHirConversion converted={0};XrHirConversionInput conversion={.actual=input->values[p].type,
                    .expected=expected.type,.has_expected=true,.infer_result=true,.inferred=expected.inferred};
                status=xr_hir_conversion_check(input->conversion,&conversion,&converted,reason);
                if(status!=XR_XIR_OK)return status;
                if(converted.target!=input->values[p].type)return XR_XIR_BAD_TYPE;
            } else if(input->values[p].type!=expected.type)
                return body_tuple_failure(reason,XR_XIR_BAD_TYPE,"Tuple field does not satisfy its ordered context");
        }
    }
    uint32_t *arguments=NULL;
    if(status==XR_XIR_OK&&tuple.physical_count)status=xir_compile_resource_status(xr_compile_resources_alloc(body->resources,
        (size_t)tuple.physical_count*sizeof(*arguments),(void **)&arguments));
    for(uint32_t p=0;status==XR_XIR_OK&&p<tuple.physical_count;++p) {
        if(!(p%64))status=body_poll(control);if(status!=XR_XIR_OK)break;
        arguments[p]=input->values[tuple.physical_fields[p]].value;
        if(arguments[p]==UINT32_MAX)status=XR_XIR_BAD_STRUCTURE;
    }
    XrHirBodyUse use={0};XrHirBodyInput typed={.origin=input->origin,.rule=XR_HIR_BODY_PROJECTION,.target=tuple.type};
    if(status==XR_XIR_OK)status=xr_hir_body_check(body,body->types,&typed,control,&use,reason);
    if(status!=XR_XIR_OK){xr_compile_resources_free(arguments);return status;}
    HirBodyUse *row=&body->uses[use.index];row->tuple=input->shape;row->tuple_construct=true;
    row->tuple_arguments=arguments;row->tuple_argument_count=tuple.physical_count;
    row->required_instruction=tuple.count!=0;row->action.operation=tuple.count?XR_XIR_TUPLE_NEW:XR_XIR_INVALID;
    *out=(XrHirTupleAction){use,input->shape,tuple,tuple.type,row->action.operation,0};return XR_XIR_OK;
}
XR_FUNC XrXirStatus xr_hir_body_tuple_field(XrHirBody *body,const XrHirTupleFieldInput *input,
    XrCompileRequestControl *control,XrHirTupleAction *out,const char **reason) {
    if(reason)*reason=NULL;
    if(!body||body->ended||!input||input->types!=body->types||!out)return XR_XIR_BAD_STRUCTURE;
    XrHirTupleInfo tuple={0};
    if(!xr_hir_body_tuple_info(body,input->shape,&tuple)||tuple.type!=input->receiver_type||input->receiver==UINT32_MAX||
        body_tuple_origin(body,input->scope,input->origin)!=XR_XIR_OK)return XR_XIR_BAD_STRUCTURE;
    if(input->field>=tuple.count)return body_tuple_failure(reason,XR_XIR_BAD_TYPE,"Tuple field index exceeds its ordered arity");
    XrHirBodyInput typed={.origin=input->origin,.rule=XR_HIR_BODY_PROJECTION,.source=tuple.type,.target=tuple.fields[input->field].type};
    XrHirBodyUse use={0};XrXirStatus status=xr_hir_body_check(body,body->types,&typed,control,&use,reason);if(status!=XR_XIR_OK)return status;
    HirBodyUse *row=&body->uses[use.index];row->tuple=input->shape;row->action.operation=XR_XIR_TUPLE_FIELD;
    row->required_instruction=true;row->has_operand=true;row->source_operand=input->receiver;
    row->has_field=true;row->field=input->field;
    *out=(XrHirTupleAction){use,input->shape,tuple,typed.target,XR_XIR_TUPLE_FIELD,input->field};return XR_XIR_OK;
}
static bool body_tuple_value_type(const XrXirFunction *function,uint32_t value,XrXirType type) {
    if(value<function->parameter_count)return function->parameters&&function->parameters[value]==type;
    value-=function->parameter_count;
    return value<function->instruction_count&&function->instructions&&function->instructions[value].type==type;
}
static XrXirStatus body_tuple_project(const XrHirBody *body,const HirBodyUse *row,
    const XrHirBodyProjection *projection,XrCompileRequestControl *control) {
    XrHirTupleInfo tuple={0};if(!xr_hir_body_tuple_info(body,row->tuple,&tuple))return XR_XIR_BAD_STRUCTURE;
    if(row->tuple_construct&&!tuple.count)return !projection->has_instruction&&projection->type==XR_XIR_UNIT?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
    if(!projection->has_instruction||!body->declarations.functions||!*body->declarations.functions||
        projection->function>=*body->declarations.function_count)return XR_XIR_BAD_STRUCTURE;
    const XrXirFunction *function=&(*body->declarations.functions)[projection->function];
    if(projection->instruction>=function->instruction_count||!function->instructions)return XR_XIR_BAD_STRUCTURE;
    XrXirInstruction actual=function->instructions[projection->instruction];
    if(actual.op!=row->action.operation||actual.type!=row->action.target)return XR_XIR_BAD_STRUCTURE;
    if(row->tuple_construct) {
        if(projection->source_argument_count!=row->tuple_argument_count||
            actual.args[1]!=tuple.physical_count||actual.args[0]>function->operand_count||
            actual.args[1]>function->operand_count-actual.args[0]||
            (tuple.physical_count&&(!projection->source_arguments||!function->operands)))return XR_XIR_BAD_STRUCTURE;
        for(uint32_t p=0;p<tuple.physical_count;++p) {
            if(!(p%64)){XrXirStatus status=body_poll(control);if(status!=XR_XIR_OK)return status;}
            uint32_t source=row->tuple_arguments[p];
            if(projection->source_arguments[p]!=source||source>=projection->source_value_count||!projection->source_value_map||
                projection->source_value_map[source]!=function->operands[actual.args[0]+p]||
                !body_tuple_value_type(function,function->operands[actual.args[0]+p],tuple.fields[tuple.physical_fields[p]].type))return XR_XIR_BAD_STRUCTURE;
        }
    } else if(actual.immediate!=(int64_t)row->field||row->source_operand>=projection->source_value_count||!projection->source_value_map||
        actual.args[0]!=projection->source_value_map[row->source_operand]||!body_tuple_value_type(function,actual.args[0],tuple.type))return XR_XIR_BAD_STRUCTURE;
    return body_poll(control);
}
