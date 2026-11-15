/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * Each call owns a snapshot of its real declaration's ordered formals. */
static XrXirStatus body_native_member(uint32_t identity,const XrNativeMemberDeclaration *input,
    XrCompileRequestControl *control,const XrNativeMemberDeclaration **output) {
    const XrNativeTypeDeclaration *owner=xr_native_declaration_by_id(identity);
    for(uint32_t m=0;owner&&m<owner->member_count;++m) {
        XrXirStatus status=body_poll(control);if(status!=XR_XIR_OK)return status;
        if(&owner->members[m]==input) {
            if(!input->is_public||!input->is_method||(input->parameter_count&&!input->parameters))return XR_XIR_BAD_STRUCTURE;
            *output=input;return XR_XIR_OK;
        }
    }
    return XR_XIR_BAD_STRUCTURE;
}
static uint32_t body_callback_owner(const XrXirTypes *types,XrXirType type) {
    return xr_xir_type_is_array(types,type)?XR_NATIVE_DECLARATION_ARRAY:
        xr_xir_type_is_map(types,type)?XR_NATIVE_DECLARATION_MAP:
        xr_xir_type_is_set(types,type)?XR_NATIVE_DECLARATION_SET:0;
}
static XrXirStatus body_native_callback_contract(XrHirBody *body,const XrHirSignatureInput *input,
    XrCompileRequestControl *control,XrHirNativeCallbackShape *output) {
    uint32_t owner=body_callback_owner(body->types,input->collection_type);
    if(!owner||input->callback_error!=XR_XIR_NEVER)return XR_XIR_BAD_TYPE;
    const XrNativeTypeDeclaration *native=xr_native_declaration_by_id(owner);
    const XrNativeMemberDeclaration *member=NULL;
    for(uint32_t m=0;native&&m<native->member_count;++m) {
        XrXirStatus status=body_poll(control);if(status!=XR_XIR_OK)return status;
        if(&native->members[m]==input->native_member){member=&native->members[m];break;}
    }
    if(!member||!member->is_public||!member->is_method||member->is_static||input->count!=member->parameter_count)
        return XR_XIR_BAD_STRUCTURE;
    if(owner!=XR_NATIVE_DECLARATION_ARRAY) {
        if(member->operation!=XR_NATIVE_OPERATION_HASH_FOR_EACH||input->count!=1)return XR_XIR_BAD_STRUCTURE;
        const XrXirTypeNode *type=xr_xir_type_node(body->types,input->collection_type);
        uint32_t arity=owner==XR_NATIVE_DECLARATION_MAP?2u:1u;
        *output=(XrHirNativeCallbackShape){{type->element,type->result},XR_XIR_UNIT,XR_XIR_NEVER,arity,arity,true};
        return XR_XIR_OK;
    }
    XrXirType element=xr_xir_array_element(body->types,input->collection_type);
    XrHirNativeCallbackShape shape={{element,XR_XIR_I64},XR_XIR_BOOL,XR_XIR_NEVER,1,1,true};
    switch(member->operation) {
    case XR_NATIVE_OPERATION_ARRAY_MAP:shape.has_result=false;shape.maximum=2;break;
    case XR_NATIVE_OPERATION_ARRAY_FILTER:shape.maximum=2;break;
    case XR_NATIVE_OPERATION_ARRAY_FOR_EACH:shape.result=XR_XIR_UNIT;shape.maximum=2;break;
    case XR_NATIVE_OPERATION_ARRAY_REDUCE:
        shape=(XrHirNativeCallbackShape){{input->accumulator,element},input->accumulator,XR_XIR_NEVER,2,2,true};break;
    case XR_NATIVE_OPERATION_ARRAY_FIND:case XR_NATIVE_OPERATION_ARRAY_FIND_INDEX:
    case XR_NATIVE_OPERATION_ARRAY_EVERY:case XR_NATIVE_OPERATION_ARRAY_SOME:break;
    default:return XR_XIR_BAD_STRUCTURE;
    }
    *output=shape;return XR_XIR_OK;
}
XR_FUNC XrXirStatus xr_hir_body_signature(XrHirBody *body,const XrHirSignatureInput *input,
    XrCompileRequestControl *control,XrHirBodySignature *output,const char **reason) {
    if(reason)*reason=NULL;
    if(!body||body->ended||!input||input->types!=body->types||!output||input->receiver_count>1)return XR_XIR_BAD_STRUCTURE;
    if(input->scope.owner!=body||input->scope.index>=body->scopes.count)return XR_XIR_BAD_STRUCTURE;
    HirBodyScope scope=((HirBodyScope *)body->scopes.items)[input->scope.index];
    if(scope.origin.function!=input->origin.function||scope.origin.module!=input->origin.module)return XR_XIR_BAD_STRUCTURE;
    if(input->declaration_binding.owner) {
        if(input->family!=XR_HIR_CALL_INDIRECT||input->declaration_binding.owner!=body||
            input->declaration_binding.index>=body->binding_count)return XR_XIR_BAD_STRUCTURE;
        HirBodyBinding binding=body->bindings[input->declaration_binding.index];
        if(!binding.materialized||!binding.declared_function||binding.location!=input->callee_value||
            binding.declaration.logical!=input->callable||binding.declaration.origin.function!=input->origin.function||
            binding.declaration.origin.module!=input->origin.module)return XR_XIR_BAD_STRUCTURE;
        XrXirStatus status=body_scope_contains(body,binding.declaration.scope,input->scope,control);
        if(status!=XR_XIR_OK)return status;
    }
    const XrXirType *function_parameters=NULL;
    XrXirFunction function_declaration={0};
    const XrXirCallableParameter *callable_parameters=NULL;
    uint32_t count=input->count;
    const XrNativeMemberDeclaration *native=NULL;
    uint32_t native_owner=input->family==XR_HIR_CALL_NATIVE_CALLBACK?
        body_callback_owner(body->types,input->collection_type):input->native_owner;
    if(input->family==XR_HIR_CALL_NATIVE_CALLBACK&&(!native_owner||(input->native_owner&&input->native_owner!=native_owner)))
        return XR_XIR_BAD_STRUCTURE;
    if(input->native_member) {
        XrXirStatus status=body_native_member(native_owner,input->native_member,control,&native);
        if(status!=XR_XIR_OK)return status;
        if(native->parameter_count!=count ||
            (input->family!=XR_HIR_CALL_NATIVE&&input->family!=XR_HIR_CALL_NATIVE_CALLBACK&&input->family!=XR_HIR_CALL_VARIADIC_READ))
            return XR_XIR_BAD_STRUCTURE;
    } else if(input->native_owner)return XR_XIR_BAD_STRUCTURE;
    XrHirNativeCallbackShape callback={0};
    if(input->family==XR_HIR_CALL_NATIVE_CALLBACK) {
        XrXirStatus status=body_native_callback_contract(body,input,control,&callback);if(status!=XR_XIR_OK)return status;
    }
    if(input->family==XR_HIR_CALL_FUNCTION||input->family==XR_HIR_CALL_CONSTRUCTOR) {
        if(!body->declarations.functions||!*body->declarations.functions||
            input->function>=*body->declarations.function_count)return XR_XIR_BAD_STRUCTURE;
        XrXirFunction declaration=(*body->declarations.functions)[input->function];
        function_declaration=declaration;
        if(count!=declaration.parameter_count)return XR_XIR_BAD_STRUCTURE;
        const XrXirFunctionIdentity *identities=*body->declarations.identities;
        if(input->family==XR_HIR_CALL_FUNCTION) {
            uint32_t receiver=identities[input->function].method_kind==XR_XIR_READ_METHOD?1u:0u;
            if(input->callable!=XR_XIR_UNIT) {
                const XrXirTypeNode *callable=xr_xir_callable_signature(body->types,input->callable);
                if(receiver||input->receiver_count||!callable||input->parameter_offset>count||
                    callable->parameter_count!=count-input->parameter_offset)return XR_XIR_BAD_STRUCTURE;
            } else if(input->receiver_count!=receiver||input->parameter_offset!=receiver)return XR_XIR_BAD_STRUCTURE;
        } else if(input->receiver_count||input->parameter_offset)return XR_XIR_BAD_STRUCTURE;
        function_parameters=declaration.parameters;
        if(count&&!function_parameters)return XR_XIR_BAD_STRUCTURE;
    } else if(input->family==XR_HIR_CALL_INDIRECT||input->family==XR_HIR_CALL_REQUIREMENT) {
        if(input->family==XR_HIR_CALL_REQUIREMENT) {
            const XrXirInterfaceTable *interfaces=body->declarations.interfaces;
            if(!interfaces||input->interface_declaration>=interfaces->count)return XR_XIR_BAD_STRUCTURE;
            const XrXirInterfaceDeclaration *owner=&interfaces->declarations[input->interface_declaration];
            if(input->interface_member>=owner->method_count||owner->methods[input->interface_member].signature!=input->callable)
                return XR_XIR_BAD_STRUCTURE;
        }
        const XrXirTypeNode *type=xr_xir_callable_signature(body->types,input->callable);
        if(!type||type->parameter_count!=count)return XR_XIR_BAD_TYPE;
        callable_parameters=type->parameters;
    } else if(input->family==XR_HIR_CALL_NATIVE||input->family==XR_HIR_CALL_VARIADIC_READ||input->family==XR_HIR_CALL_NATIVE_CALLBACK) {
        if(count&&!input->native_parameters)return XR_XIR_BAD_STRUCTURE;
        function_parameters=input->native_parameters;
    } else return XR_XIR_BAD_STRUCTURE;
    if(input->family==XR_HIR_CALL_CONSTRUCTOR)for(uint32_t a=0;a<input->constructor_argument_count;++a) {
        if(!(a%64)){XrXirStatus status=body_poll(control);if(status!=XR_XIR_OK)return status;}
        if(input->constructor_accesses&&input->constructor_accesses[a]!=XR_CALL_ARG_PLAIN) {
            if (reason)
                *reason = "constructor argument requires a value";
            return XR_XIR_BAD_TYPE;
        }
    }
    if(count&&(sizeof(XrXirCallableParameter)+2)>SIZE_MAX/count)return XR_XIR_OVERFLOW;
    XrXirCallableParameter *parameters=NULL;XrXirStatus status=body_poll(control);
    if(status==XR_XIR_OK&&count)status=xir_compile_resource_status(xr_compile_resources_alloc(body->resources,
        (size_t)count*(sizeof(*parameters)+2),(void **)&parameters));
    if(status==XR_XIR_OK)status=body_bytes(parameters,NULL,(size_t)count*(sizeof(*parameters)+2),control);
    for(uint32_t p=0;status==XR_XIR_OK&&p<count;++p) {
        if (!(p % 64))
            status = body_poll(control);
        if (status != XR_XIR_OK)
            break;
        if(input->family==XR_HIR_CALL_NATIVE_CALLBACK&&!p) {
            /* This formal is an owned shape, not a provisional Unit type. */
            parameters[p]=(XrXirCallableParameter){(XrXirType)UINT32_MAX,XR_PARAM_READ,0};continue;
        }
        if(callable_parameters) {
            parameters[p]=callable_parameters[p];
            if(!xr_xir_callable_parameter_storage_valid(body->types,&parameters[p]))status=XR_XIR_BAD_TYPE;
        } else {
            XrXirType type=function_parameters[p];
            /* Direct REF is the verified declaration's existing physical Cell
             * ABI. Actual argument Cells never create this authority. */
            XrParamMode mode=input->family==XR_HIR_CALL_FUNCTION||input->family==XR_HIR_CALL_CONSTRUCTOR?
                (XrParamMode)xr_xir_declared_parameter_mode(body->types,type,function_declaration.consume_parameters,
                    function_declaration.consume_parameter_count,p):XR_PARAM_READ;
            parameters[p]=(XrXirCallableParameter){type,mode,
                (input->family==XR_HIR_CALL_FUNCTION||input->family==XR_HIR_CALL_CONSTRUCTOR) &&
                xr_xir_function_parameter_escaping(&function_declaration,p)};
            if(mode==XR_PARAM_READ&&xr_xir_type_is_cell(body->types,type))
                status=input->family==XR_HIR_CALL_VARIADIC_READ?XR_XIR_BAD_STRUCTURE:XR_XIR_BAD_TYPE;
        }
    }
    uint32_t default_owner = input->function;
    if(status==XR_XIR_OK)status=body_call_default_declaration(body,input,control,&default_owner);
    if(status==XR_XIR_OK)status=body_vector_reserve(body,&body->signatures,sizeof(HirBodySignature),control);
    if(status!=XR_XIR_OK) {
        xr_compile_resources_free(parameters);
        if(reason)*reason=input->family==XR_HIR_CALL_NATIVE?"native READ arguments cannot grant a ref contract":
            input->family==XR_HIR_CALL_VARIADIC_READ?"native variadic READ parameter is inconsistent":
            input->family==XR_HIR_CALL_INDIRECT?"indirect parameter storage and mode disagree":"call parameter storage and mode disagree";
        return status;
    }
    HirBodySignature row={.declaration=*input,.parameters=parameters,.callback=callback,.accesses=count?(unsigned char *)(parameters+count):NULL,.first_argument=UINT32_MAX,.last_argument=UINT32_MAX};row.declaration.native_parameters=NULL;row.declaration.types=NULL;
    row.default_owner=default_owner;
    if(input->declaration_binding.owner)
        row.default_parameter_offset=(*body->declarations.identities)[default_owner].capture_count;
    row.native_optional=count?row.accesses+count:NULL;
    if(native) {
        row.native_member=native->id;row.native_owner=native_owner;
        for(uint32_t p=0;p<count;++p)row.native_optional[p]=native->parameters[p].optional?1:0;
        row.native_variadic=count&&native->parameters[count-1].variadic;
    }
    row.declaration.constructor_accesses=NULL;row.declaration.constructor_argument_count=0;row.declaration.native_member=NULL;
    ((HirBodySignature *)body->signatures.items)[body->signatures.count]=row;
    *output=(XrHirBodySignature){body,body->signatures.count++};return XR_XIR_OK;
}
XR_FUNC XrXirStatus xr_hir_body_callback_context(const XrHirBody *body,XrHirBodySignature signature,XrXirType type,uint32_t receiver,XrHirNativeCallbackShape *output) {
    if(!body||body->ended||signature.owner!=body||signature.index>=body->signatures.count||!output)return XR_XIR_BAD_STRUCTURE;
    HirBodySignature row=((HirBodySignature *)body->signatures.items)[signature.index];
    if(row.declaration.family!=XR_HIR_CALL_NATIVE_CALLBACK||row.finished||type!=row.declaration.collection_type||receiver!=row.declaration.collection_value)return XR_XIR_BAD_STRUCTURE;
    if(row.declaration.count==2&&row.declaration.accumulator==XR_XIR_UNIT)return XR_XIR_BAD_STRUCTURE;
    *output=row.callback;return XR_XIR_OK;
}
XR_FUNC XrXirStatus xr_hir_body_array_accumulator(XrHirBody *body,XrHirBodySignature signature,
    XrXirType type,XrCompileRequestControl *control) {
    if(!body||body->ended||signature.owner!=body||signature.index>=body->signatures.count)
        return XR_XIR_BAD_STRUCTURE;
    HirBodySignature *row=&((HirBodySignature *)body->signatures.items)[signature.index];
    if(row->declaration.family!=XR_HIR_CALL_NATIVE_CALLBACK||row->finished||row->access_count||row->argument_count||
        row->declaration.count!=2||row->declaration.accumulator!=XR_XIR_UNIT||
        row->callback.parameters[0]!=XR_XIR_UNIT)return XR_XIR_BAD_STRUCTURE;
    XrXirCallableParameter parameter={type,XR_PARAM_READ,0};
    if(type==XR_XIR_UNIT||!xr_xir_callable_parameter_storage_valid(body->types,&parameter))return XR_XIR_BAD_TYPE;
    XrXirStatus status=body_poll(control);if(status!=XR_XIR_OK)return status;
    XrXirCompileContext context={body->resources,xr_xir_compile_default_limits()};
    status=xr_xir_compile_type_expression_shape(&context,body->types,type,xr_xir_type_span(body->types,type));
    if(status!=XR_XIR_OK)return status;
    status=body_poll(control);if(status!=XR_XIR_OK)return status;
    row->declaration.accumulator=type;row->parameters[1]=parameter;
    row->callback.parameters[0]=type;row->callback.result=type;
    return XR_XIR_OK;
}
static XrXirStatus body_native_callback(const XrHirBody *body,const HirBodySignature *call,
    XrXirType type,XrCompileRequestControl *control,const char **reason) {
    const XrXirTypeNode *actual=xr_xir_callable_signature(body->types,type);
    XrHirNativeCallbackShape shape=call->callback;
    if(!actual||(actual->flags&XR_XIR_CALLABLE_REST)||actual->parameter_count<shape.minimum||actual->parameter_count>shape.maximum) {
        if (reason)
            *reason = "collection callback requires a function of the declared shape";
        return XR_XIR_BAD_TYPE;
    }
    if(!xr_xir_error_bound_accepts(body->types,actual->element,shape.error)) {
        if (reason)
            *reason = "collection callback error exceeds its declaration";
        return XR_XIR_BAD_TYPE;
    }
    for(uint32_t p=0;p<actual->parameter_count;++p) {
        XrXirStatus status=body_poll(control);if(status!=XR_XIR_OK)return status;
        if(actual->parameters[p].type!=shape.parameters[p]||actual->parameters[p].mode!=XR_PARAM_READ) {
            if (reason)
                *reason = "collection callback parameter type differs from the element";
            return XR_XIR_BAD_TYPE;
        }
    }
    if(shape.has_result&&actual->result!=shape.result) {
        if (reason)
            *reason = "collection callback result differs from its declared result";
        return XR_XIR_BAD_TYPE;
    }
    return XR_XIR_OK;
}
XR_FUNC XrXirStatus xr_hir_body_call_argument_formals(const XrHirBody *body,XrHirBodySignature signature,
    XrCompileRequestControl *control,uint32_t *formals,uint32_t count) {
    if(!body||body->ended||signature.owner!=body||signature.index>=body->signatures.count||count>65536||(!formals&&count))
        return XR_XIR_BAD_STRUCTURE;
    for(uint32_t a=0;a<count;++a) {
        if(!(a%64)){XrXirStatus status=body_poll(control);if(status!=XR_XIR_OK)return status;}
        formals[a]=UINT32_MAX;
    }
    HirBodySignature call=((HirBodySignature *)body->signatures.items)[signature.index];
    uint32_t selected=call.first_argument;
    for(uint32_t p=0;p<call.argument_count;++p) {
        if(!(p%64)){XrXirStatus status=body_poll(control);if(status!=XR_XIR_OK)return status;}
        if(selected>=body->count)return XR_XIR_BAD_STRUCTURE;
        HirBodyUse use=body->uses[selected];
        if(use.signature.owner!=body||use.signature.index!=signature.index)return XR_XIR_BAD_STRUCTURE;
        uint32_t argument=use.action.argument;
        uint32_t formal=call.declaration.family==XR_HIR_CALL_VARIADIC_READ?0:use.action.formal;
        if(!use.defaulted&&argument<count&&formal!=UINT32_MAX) {
            if(formals[argument]==UINT32_MAX)formals[argument]=formal;
            else if(formals[argument]!=formal)formals[argument]=UINT32_MAX-1;
        }
        selected=use.next_argument;
    }
    for(uint32_t a=0;a<count;++a) {
        if(!(a%64)){XrXirStatus status=body_poll(control);if(status!=XR_XIR_OK)return status;}
        if(formals[a]==UINT32_MAX-1)formals[a]=UINT32_MAX;
    }
    return body_poll(control);
}
static XrXirStatus body_call_final_type(XrHirBody *body,const XrHirCallUseInput *input,XrXirCallableParameter formal,XrCompileRequestControl *control) {
    XrXirType expected=input->check_type?input->expected:formal.type;
    XrXirStatus status=body_substitution(body,input->substitution,input->substitution_count,formal.type,expected,control);
    if(status!=XR_XIR_OK||expected==input->type)return status;
    if(formal.mode==XR_PARAM_REF)return XR_XIR_BAD_TYPE;
    XrXirCompileContext context={body->resources,xr_xir_compile_default_limits()};
    return xr_xir_compile_callable_weakening(&context,body->types,input->type,expected);
}
XR_FUNC XrXirStatus xr_hir_body_call_argument(XrHirBody *body,XrHirBodySignature signature,
    const XrHirCallUseInput *input,XrCompileRequestControl *control,XrHirBodyUse *use,XrHirBodyAction *output) {
    if(!body||body->ended||!input||input->types!=body->types||!use||!output||signature.owner!=body||signature.index>=body->signatures.count)
        return XR_XIR_BAD_STRUCTURE;
    if(input->reason)*input->reason=NULL;
    if(input->defaulted&&input->kind!=XR_HIR_CALL_FINAL)return XR_XIR_BAD_STRUCTURE;
    HirBodySignature declaration=((HirBodySignature *)body->signatures.items)[signature.index];
    if(declaration.finished||input->origin.function!=declaration.declaration.origin.function)return XR_XIR_BAD_STRUCTURE;
    if(declaration.declaration.family==XR_HIR_CALL_NATIVE_CALLBACK&&declaration.declaration.count==2&&
        declaration.declaration.accumulator==XR_XIR_UNIT)return XR_XIR_BAD_STRUCTURE;
    if(declaration.binding_open && !declaration.binding_sealed && input->kind!=XR_HIR_CALL_PLAN)
        return XR_XIR_BAD_STRUCTURE;
    bool receiver=input->kind==XR_HIR_CALL_FINAL&&input->formal==UINT32_MAX&&declaration.declaration.family==XR_HIR_CALL_REQUIREMENT;
    XrXirStatus scope_status=body_scope_contains(body,declaration.declaration.scope,input->scope,control);
    if(scope_status!=XR_XIR_OK)return scope_status;
    uint32_t parameter=declaration.declaration.family==XR_HIR_CALL_VARIADIC_READ?0:input->formal;
    if(!receiver&&parameter>=declaration.declaration.count)return XR_XIR_BAD_STRUCTURE;
    if(declaration.binding_sealed&&!receiver&&input->kind!=XR_HIR_CALL_PLAN) {
        bool rest=body_call_binding_rest(body,&declaration)&&parameter+1==declaration.declaration.count;
        bool implicit=parameter<declaration.declaration.parameter_offset;
        if(input->defaulted) {
            if(implicit||declaration.formal_arguments[parameter]!=UINT32_MAX)return XR_XIR_BAD_STRUCTURE;
        } else if(!implicit&&!rest&&
            (input->argument>=declaration.binding_count||declaration.argument_formals[input->argument]!=parameter))
            return XR_XIR_BAD_STRUCTURE;
    }
    XrXirCallableParameter formal=receiver?(XrXirCallableParameter){declaration.declaration.receiver_type,XR_PARAM_READ,0}:declaration.parameters[parameter];
    bool callback=declaration.declaration.family==XR_HIR_CALL_NATIVE_CALLBACK&&!parameter;
    if(callback&&input->kind==XR_HIR_CALL_VALUE&&declaration.callback_received)return XR_XIR_BAD_STRUCTURE;
    if((input->kind==XR_HIR_CALL_VALUE||input->kind==XR_HIR_CALL_EXACT||input->kind==XR_HIR_CALL_FINAL)&&
        input->type==XR_XIR_UNIT&&declaration.declaration.family!=XR_HIR_CALL_REQUIREMENT) {
        if (input->reason)
            *input->reason = "unit argument is not admitted";
        return XR_XIR_BAD_TYPE;
    }
    bool callback_final=callback&&input->kind==XR_HIR_CALL_FINAL;
    if(callback_final) {
        if(!declaration.callback_received||declaration.callback_use>=body->count)return XR_XIR_BAD_STRUCTURE;
        HirBodyUse received=body->uses[declaration.callback_use];
        if(received.source_value!=input->value||received.action.target!=input->type)return XR_XIR_BAD_STRUCTURE;
        XrXirStatus status=body_native_callback(body,&declaration,input->type,control,input->reason);if(status!=XR_XIR_OK)return status;
        formal.type=input->type;
    } else if(callback&&!declaration.callback_ready)formal.type=XR_XIR_UNIT;
    XrHirBodyAction action={.source=formal.type,.target=formal.type,.formal=input->formal,.argument=input->argument,
        .receiver_count=declaration.declaration.receiver_count,.reference=formal.mode==XR_PARAM_REF,
        .consuming=formal.mode==XR_PARAM_MOVE};
    if(callback_final)action.argument_count=xr_xir_callable_signature(body->types,input->type)->parameter_count;
    if(input->spread&&action.reference)return XR_XIR_BAD_TYPE;
    if(input->kind==XR_HIR_CALL_PLAN) {
        if(callback&&declaration.callback_ready)action.argument_count=xr_xir_callable_signature(body->types,formal.type)->parameter_count;
        *output=action;*use=(XrHirBodyUse){0};return body_poll(control);
    }
    XrHirTupleShape spread_shape={0};
    if(input->kind==XR_HIR_CALL_SPREAD) {
        XrXirStatus status=xr_hir_body_tuple_receive(body,body->types,input->type,control,&spread_shape,input->reason);
        if(status!=XR_XIR_OK)return status;
        XrHirTupleInfo tuple={0};if(!xr_hir_body_tuple_info(body,spread_shape,&tuple))return XR_XIR_BAD_STRUCTURE;
        if(input->spread_field>=tuple.count||action.reference)return XR_XIR_BAD_TYPE;
        action.target=tuple.fields[input->spread_field].type;action.operation=XR_XIR_TUPLE_FIELD;
        action.field=input->spread_field;
    } else if(input->kind==XR_HIR_CALL_ACCESS) {
        if(formal.mode==XR_PARAM_MOVE) {
            if(input->access!=XR_CALL_ARG_PLAIN&&input->access!=XR_CALL_ARG_MOVE)return XR_XIR_BAD_TYPE;
        } else if(input->access!=(action.reference?XR_CALL_ARG_REF:XR_CALL_ARG_PLAIN))return XR_XIR_BAD_TYPE;
    } else if(input->kind==XR_HIR_CALL_VALUE||input->kind==XR_HIR_CALL_EXACT||input->kind==XR_HIR_CALL_FINAL) {
        if(input->kind==XR_HIR_CALL_EXACT&&input->type!=formal.type)return XR_XIR_BAD_TYPE;
        if(input->kind==XR_HIR_CALL_FINAL) {
            if(receiver&&input->type!=formal.type)return XR_XIR_BAD_TYPE;
            uint32_t ordinal=declaration.argument_count;
            if(declaration.declaration.family==XR_HIR_CALL_NATIVE&&declaration.binding_sealed) {
                uint32_t selected=0;
                for(ordinal=0;ordinal<declaration.declaration.count;++ordinal) {
                    if(!(ordinal%64)){XrXirStatus status=body_poll(control);if(status!=XR_XIR_OK)return status;}
                    if(declaration.formal_arguments[ordinal]==UINT32_MAX)continue;
                    if(selected++==declaration.argument_count)break;
                }
                if(ordinal==declaration.declaration.count)return XR_XIR_BAD_STRUCTURE;
            }
            if(declaration.declaration.family==XR_HIR_CALL_REQUIREMENT) {
                if((!ordinal)!=receiver||(!receiver&&input->formal!=ordinal-1))return XR_XIR_BAD_STRUCTURE;
            } else if(input->formal!=ordinal)return XR_XIR_BAD_STRUCTURE;
            bool implicit_receiver=receiver||(input->formal<declaration.declaration.parameter_offset&&input->argument==UINT32_MAX);
            if(!implicit_receiver&&!input->defaulted) {
                if(input->formal<declaration.declaration.parameter_offset||!(declaration.accesses[parameter]&1))return XR_XIR_BAD_STRUCTURE;
                if(declaration.binding_sealed) {
                    bool rest=body_call_binding_rest(body,&declaration)&&parameter+1==declaration.declaration.count;
                    if(!rest&&(input->argument>=declaration.binding_count||
                        declaration.argument_formals[input->argument]!=parameter||
                        declaration.formal_arguments[parameter]!=input->argument))return XR_XIR_BAD_STRUCTURE;
                } else if(input->argument!=input->formal-declaration.declaration.parameter_offset||
                    input->argument>=declaration.access_count)return XR_XIR_BAD_STRUCTURE;
            }
            if(!receiver&&!callback) {
                XrXirStatus status=body_call_final_type(body,input,formal,control);if(status!=XR_XIR_OK)return status;
            }
            if(input->defaulted) {
                if(declaration.declaration.family!=XR_HIR_CALL_FUNCTION&&
                    declaration.declaration.family!=XR_HIR_CALL_CONSTRUCTOR&&!declaration.declaration.declaration_binding.owner)return XR_XIR_BAD_STRUCTURE;
                const XrXirDefaultTable *defaults=body->declarations.defaults;bool found=false;
                if(!defaults)return XR_XIR_BAD_STRUCTURE;
                for(uint32_t d=0;d<defaults->count;++d) {
                    if(!(d%64)){XrXirStatus status=body_poll(control);if(status!=XR_XIR_OK)return status;}
                    XrXirDefaultBinding binding=defaults->records[d];
                    if(binding.owner_kind==XR_XIR_DEFAULT_PARAMETER&&binding.owner==declaration.default_owner&&
                        binding.ordinal==input->formal+declaration.default_parameter_offset&&binding.function==input->default_function){found=true;break;}
                }
                if(!found)return XR_XIR_BAD_STRUCTURE;
            }
        }
        action.target=input->type;
    } else return XR_XIR_BAD_STRUCTURE;
    XrHirBodyInput checked={.origin=input->origin,.definition=declaration.declaration.origin,
        .rule=XR_HIR_BODY_IDENTITY,.source=input->type,.target=action.target,
        .formal=input->formal,.receiver_count=action.receiver_count};
    XrXirType *default_types=NULL;
    XrXirStatus status=input->defaulted?body_default_types_stage(body,&declaration,input,control,&default_types):XR_XIR_OK;
    if(status!=XR_XIR_OK)return status;
    status=xr_hir_body_check(body,body->types,&checked,control,use,NULL);
    if(status!=XR_XIR_OK){xr_compile_resources_free(default_types);return status;}
    if(input->defaulted&&!declaration.default_types_ready) {
        HirBodySignature *call=&((HirBodySignature *)body->signatures.items)[signature.index];
        call->default_types=default_types;call->default_type_count=input->substitution_count;call->default_types_ready=true;
    }
    body->uses[use->index].action=action;
    body->uses[use->index].has_source=input->kind==XR_HIR_CALL_VALUE||input->kind==XR_HIR_CALL_EXACT||input->kind==XR_HIR_CALL_FINAL;
    body->uses[use->index].signature=signature;
    body->uses[use->index].next_argument=UINT32_MAX;
    body->uses[use->index].defaulted=input->defaulted;
    body->uses[use->index].default_value=input->default_value;
    body->uses[use->index].default_call=input->default_call;
    body->uses[use->index].default_conversion=input->default_conversion;
    if(callback&&input->kind==XR_HIR_CALL_VALUE) {
        HirBodySignature *call=&((HirBodySignature *)body->signatures.items)[signature.index];
        call->callback_received=true;call->callback_use=use->index;
    }
    if(callback_final) {
        HirBodySignature *call=&((HirBodySignature *)body->signatures.items)[signature.index];
        call->parameters[parameter].type=input->type;call->callback_ready=true;
    }
    if(input->kind==XR_HIR_CALL_SPREAD) {
        body->uses[use->index].tuple=spread_shape;
        body->uses[use->index].required_instruction=true;
        body->uses[use->index].has_field=true;body->uses[use->index].field=input->spread_field;
        body->uses[use->index].has_operand=true;body->uses[use->index].source_operand=input->value;
    }
    if(input->kind==XR_HIR_CALL_ACCESS) {
        HirBodySignature *call=&((HirBodySignature *)body->signatures.items)[signature.index];
        call->accesses[parameter]|=1;
        if(input->argument>=call->access_count)call->access_count=input->argument+1;
    }
    if(input->kind==XR_HIR_CALL_FINAL) {
        HirBodySignature *call=&((HirBodySignature *)body->signatures.items)[signature.index];
        if(call->last_argument!=UINT32_MAX)body->uses[call->last_argument].next_argument=use->index;
        else call->first_argument=use->index;
        call->last_argument=use->index;++call->argument_count;
    }
    body->uses[use->index].source_value=input->value;*output=action;return XR_XIR_OK;
}
XR_FUNC XrXirStatus xr_hir_body_call_scope(XrHirBody *body,XrHirBodySignature signature,
    XrHirScopeArgument group,XrCompileRequestControl *control) {
    if(!body||body->ended||signature.owner!=body||signature.index>=body->signatures.count||group.value==UINT32_MAX)
        return XR_XIR_BAD_STRUCTURE;
    HirBodySignature row=((HirBodySignature *)body->signatures.items)[signature.index];
    const XrXirTypeNode *type=xr_xir_type_node(body->types,group.type);
    if(row.finished||row.group.type!=XR_XIR_UNIT||row.declaration.family!=XR_HIR_CALL_FUNCTION||
        !type||type->kind!=XR_XIR_TYPE_TASK||type->element!=XR_XIR_UNIT)return XR_XIR_BAD_TYPE;
    XrXirStatus status=body_poll(control);if(status!=XR_XIR_OK)return status;
    ((HirBodySignature *)body->signatures.items)[signature.index].group=group;return XR_XIR_OK;
}
#include "xhir_body_class_call.inc.c"
XR_FUNC XrXirStatus xr_hir_body_call_finish(XrHirBody *body,XrHirBodySignature signature,
    const XrXirInstruction *instruction,XrCompileRequestControl *control,XrHirBodyUse *use,XrHirBodyAction *output) {
    if(!body||body->ended||signature.owner!=body||signature.index>=body->signatures.count||!instruction||!use||!output)
        return XR_XIR_BAD_STRUCTURE;
    HirBodySignature row=((HirBodySignature *)body->signatures.items)[signature.index];
    bool native=row.declaration.family==XR_HIR_CALL_NATIVE||row.declaration.family==XR_HIR_CALL_VARIADIC_READ||row.declaration.family==XR_HIR_CALL_NATIVE_CALLBACK;
    uint32_t expected=row.declaration.count+(row.declaration.family==XR_HIR_CALL_REQUIREMENT?1u:0u);
    if(row.finished||(row.binding_open&&!row.binding_sealed)||((!native||row.declaration.family==XR_HIR_CALL_NATIVE_CALLBACK)&&row.argument_count!=expected)||
        (native&&row.declaration.family!=XR_HIR_CALL_VARIADIC_READ&&row.argument_count>expected))return XR_XIR_BAD_STRUCTURE;
    if(native&&row.binding_sealed&&row.argument_count!=row.binding_count)return XR_XIR_BAD_STRUCTURE;
    if(native&&row.native_member&&!row.binding_sealed&&row.declaration.family!=XR_HIR_CALL_VARIADIC_READ) {
        for(uint32_t p=row.argument_count;p<expected;++p) {
            if(!(p%64)){XrXirStatus status=body_poll(control);if(status!=XR_XIR_OK)return status;}
            if(!row.native_optional[p])return XR_XIR_BAD_STRUCTURE;
        }
    }
    XrXirOp operation=instruction->op;
    bool scoped=row.group.type!=XR_XIR_UNIT;
    bool method=body_class_call_bound(&row);
    if((operation==XR_XIR_SCOPED_GO)!=scoped||
        (row.declaration.family==XR_HIR_CALL_FUNCTION&&row.declaration.callable!=XR_XIR_UNIT&&
         !method&&operation!=XR_XIR_GO&&operation!=XR_XIR_SCOPED_GO))
        return XR_XIR_BAD_STRUCTURE;
    if(native) {
        if(operation!=XR_XIR_INVALID)return XR_XIR_BAD_STRUCTURE;
        XrXirStatus status=body_poll(control);if(status!=XR_XIR_OK)return status;
        ((HirBodySignature *)body->signatures.items)[signature.index].finished=true;
        *use=(XrHirBodyUse){0};*output=(XrHirBodyAction){.target=instruction->type};return XR_XIR_OK;
    }
    if(row.declaration.family==XR_HIR_CALL_INDIRECT||method) {
        if(operation!=XR_XIR_CALL_INDIRECT||instruction->immediate!=row.declaration.callee_value)return XR_XIR_BAD_STRUCTURE;
    } else if(row.declaration.family==XR_HIR_CALL_REQUIREMENT) {
        if(operation!=XR_XIR_CALL_REQUIREMENT||instruction->targets[0]!=row.declaration.interface_declaration||
            instruction->targets[1]!=row.declaration.interface_member)return XR_XIR_BAD_STRUCTURE;
    } else if((operation!=XR_XIR_CALL&&operation!=XR_XIR_GO&&operation!=XR_XIR_SCOPED_GO&&operation!=XR_XIR_GENERATOR_NEW)||
        instruction->immediate!=row.declaration.function)return XR_XIR_BAD_STRUCTURE;
    if(row.argument_count>UINT32_MAX-(scoped?1u:0u))return XR_XIR_OVERFLOW;
    XrHirBodyInput input={.origin=row.declaration.origin,.definition=row.declaration.origin,
        .rule=XR_HIR_BODY_IDENTITY,.source=instruction->type,.target=instruction->type};
    XrXirStatus status=xr_hir_body_check(body,body->types,&input,control,use,NULL);if(status!=XR_XIR_OK)return status;
    HirBodyUse *record=&body->uses[use->index];record->action.operation=operation;
    record->required_instruction=true;record->is_call=true;record->signature=signature;
    record->action.argument_count=row.argument_count+(scoped?1u:0u)-(method?1u:0u);
    ((HirBodySignature *)body->signatures.items)[signature.index].finished=true;
    *output=record->action;return XR_XIR_OK;
}
