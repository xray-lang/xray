/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * Each call owns a snapshot of its real declaration's ordered formals. */
static XrXirStatus body_array_contract(XrHirBody *body,const XrHirSignatureInput *input,
    XrCompileRequestControl *control,XrHirArrayCallbackShape *output) {
    if(!xr_xir_type_is_array(body->types,input->array)||input->callback_error!=XR_XIR_NEVER)return XR_XIR_BAD_TYPE;
    const XrNativeTypeDeclaration *native=xr_native_declaration_by_id(XR_NATIVE_DECLARATION_ARRAY);
    const XrNativeMemberDeclaration *member=NULL;
    for(uint32_t m=0;native&&m<native->member_count;++m) {
        XrXirStatus status=body_poll(control);if(status!=XR_XIR_OK)return status;
        if(&native->members[m]==input->array_member){member=&native->members[m];break;}
    }
    if(!member||!member->is_public||!member->is_method||member->is_static||input->count!=member->parameter_count)
        return XR_XIR_BAD_STRUCTURE;
    XrXirType element=xr_xir_array_element(body->types,input->array);
    XrHirArrayCallbackShape shape={{element,XR_XIR_I64},XR_XIR_BOOL,XR_XIR_NEVER,1,1,true};
    switch(member->operation) {
    case XR_NATIVE_OPERATION_ARRAY_MAP:shape.has_result=false;shape.maximum=2;break;
    case XR_NATIVE_OPERATION_ARRAY_FILTER:shape.maximum=2;break;
    case XR_NATIVE_OPERATION_ARRAY_FOR_EACH:shape.result=XR_XIR_UNIT;shape.maximum=2;break;
    case XR_NATIVE_OPERATION_ARRAY_REDUCE:
        if(input->accumulator==XR_XIR_UNIT)return XR_XIR_BAD_TYPE;
        shape=(XrHirArrayCallbackShape){{input->accumulator,element},input->accumulator,XR_XIR_NEVER,2,2,true};break;
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
    const XrXirType *function_parameters=NULL;
    const XrXirCallableParameter *callable_parameters=NULL;
    uint32_t count=input->count;
    XrHirArrayCallbackShape callback={0};
    if(input->family==XR_HIR_CALL_ARRAY) {
        XrXirStatus status=body_array_contract(body,input,control,&callback);if(status!=XR_XIR_OK)return status;
    }
    if(input->family==XR_HIR_CALL_FUNCTION||input->family==XR_HIR_CALL_CONSTRUCTOR) {
        if(!body->declarations.functions||!*body->declarations.functions||
            input->function>=*body->declarations.function_count)return XR_XIR_BAD_STRUCTURE;
        XrXirFunction declaration=(*body->declarations.functions)[input->function];
        if(count!=declaration.parameter_count)return XR_XIR_BAD_STRUCTURE;
        const XrXirFunctionIdentity *identities=*body->declarations.identities;
        if(input->family==XR_HIR_CALL_FUNCTION) {
            uint32_t receiver=identities[input->function].method_kind==XR_XIR_READ_METHOD?1u:0u;
            if(input->receiver_count!=receiver||input->parameter_offset!=receiver)return XR_XIR_BAD_STRUCTURE;
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
    } else if(input->family==XR_HIR_CALL_NATIVE||input->family==XR_HIR_CALL_VARIADIC_READ||input->family==XR_HIR_CALL_ARRAY) {
        if(count&&!input->native_parameters)return XR_XIR_BAD_STRUCTURE;
        function_parameters=input->native_parameters;
    } else return XR_XIR_BAD_STRUCTURE;
    if(input->family==XR_HIR_CALL_CONSTRUCTOR)for(uint32_t a=0;a<input->constructor_argument_count;++a) {
        if(!(a%64)){XrXirStatus status=body_poll(control);if(status!=XR_XIR_OK)return status;}
        if(input->constructor_accesses&&input->constructor_accesses[a]!=XR_CALL_ARG_PLAIN) {
            if(reason)*reason="constructor argument requires a value";return XR_XIR_BAD_TYPE;
        }
    }
    if(count&&(sizeof(XrXirCallableParameter)+1)>SIZE_MAX/count)return XR_XIR_OVERFLOW;
    XrXirCallableParameter *parameters=NULL;XrXirStatus status=body_poll(control);
    if(status==XR_XIR_OK&&count)status=xir_compile_resource_status(xr_compile_resources_alloc(body->resources,
        (size_t)count*(sizeof(*parameters)+1),(void **)&parameters));
    if(status==XR_XIR_OK)status=body_bytes(parameters,NULL,(size_t)count*(sizeof(*parameters)+1),control);
    for(uint32_t p=0;status==XR_XIR_OK&&p<count;++p) {
        if(!(p%64))status=body_poll(control);if(status!=XR_XIR_OK)break;
        if(input->family==XR_HIR_CALL_ARRAY&&!p) {
            /* This formal is an owned shape, not a provisional Unit type. */
            parameters[p]=(XrXirCallableParameter){(XrXirType)UINT32_MAX,XR_PARAM_READ};continue;
        }
        if(callable_parameters) {
            parameters[p]=callable_parameters[p];
            if(!xr_xir_callable_parameter_storage_valid(body->types,&parameters[p]))status=XR_XIR_BAD_TYPE;
        } else {
            XrXirType type=function_parameters[p];
            /* Direct REF is the verified declaration's existing physical Cell
             * ABI. Actual argument Cells never create this authority. */
            XrParamMode mode=input->family==XR_HIR_CALL_FUNCTION&&xr_xir_type_is_cell(body->types,type)?XR_PARAM_REF:XR_PARAM_READ;
            parameters[p]=(XrXirCallableParameter){type,mode};
            if(mode==XR_PARAM_READ&&xr_xir_type_is_cell(body->types,type))
                status=input->family==XR_HIR_CALL_VARIADIC_READ?XR_XIR_BAD_STRUCTURE:XR_XIR_BAD_TYPE;
        }
    }
    if(status==XR_XIR_OK)status=body_vector_reserve(body,&body->signatures,sizeof(HirBodySignature),control);
    if(status!=XR_XIR_OK) {
        xr_compile_resources_free(parameters);
        if(reason)*reason=input->family==XR_HIR_CALL_NATIVE?"native READ arguments cannot grant a ref contract":
            input->family==XR_HIR_CALL_VARIADIC_READ?"native variadic READ parameter is inconsistent":
            input->family==XR_HIR_CALL_INDIRECT?"indirect parameter storage and mode disagree":"call parameter storage and mode disagree";
        return status;
    }
    HirBodySignature row={.declaration=*input,.parameters=parameters,.callback=callback,.accesses=count?(unsigned char *)(parameters+count):NULL,.first_argument=UINT32_MAX,.last_argument=UINT32_MAX};row.declaration.native_parameters=NULL;row.declaration.types=NULL;
    row.declaration.constructor_accesses=NULL;row.declaration.constructor_argument_count=0;row.declaration.array_member=NULL;
    ((HirBodySignature *)body->signatures.items)[body->signatures.count]=row;
    *output=(XrHirBodySignature){body,body->signatures.count++};return XR_XIR_OK;
}
XR_FUNC XrXirStatus xr_hir_body_array_context(const XrHirBody *body,XrHirBodySignature signature,XrXirType type,uint32_t receiver,XrHirArrayCallbackShape *output) {
    if(!body||body->ended||signature.owner!=body||signature.index>=body->signatures.count||!output)return XR_XIR_BAD_STRUCTURE;
    HirBodySignature row=((HirBodySignature *)body->signatures.items)[signature.index];
    if(row.declaration.family!=XR_HIR_CALL_ARRAY||row.finished||type!=row.declaration.array||receiver!=row.declaration.array_value)return XR_XIR_BAD_STRUCTURE;
    *output=row.callback;return XR_XIR_OK;
}
static XrXirStatus body_array_callback(const XrHirBody *body,const HirBodySignature *call,
    XrXirType type,XrCompileRequestControl *control,const char **reason) {
    const XrXirTypeNode *actual=xr_xir_callable_signature(body->types,type);
    XrHirArrayCallbackShape shape=call->callback;
    if(!actual||actual->parameter_count<shape.minimum||actual->parameter_count>shape.maximum) {
        if(reason)*reason="Array callback requires a function of the declared shape";return XR_XIR_BAD_TYPE;
    }
    if(!xr_xir_error_bound_accepts(body->types,actual->element,shape.error)) {
        if(reason)*reason="Array callback error exceeds its declaration";return XR_XIR_BAD_TYPE;
    }
    for(uint32_t p=0;p<actual->parameter_count;++p) {
        XrXirStatus status=body_poll(control);if(status!=XR_XIR_OK)return status;
        if(actual->parameters[p].type!=shape.parameters[p]||actual->parameters[p].mode!=XR_PARAM_READ) {
            if(reason)*reason="Array callback parameter type differs from the element";return XR_XIR_BAD_TYPE;
        }
    }
    if(shape.has_result&&actual->result!=shape.result) {
        if(reason)*reason="Array callback result differs from its declared result";return XR_XIR_BAD_TYPE;
    }
    return XR_XIR_OK;
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
    bool receiver=input->kind==XR_HIR_CALL_FINAL&&input->formal==UINT32_MAX&&declaration.declaration.family==XR_HIR_CALL_REQUIREMENT;
    XrXirStatus scope_status=body_scope_contains(body,declaration.declaration.scope,input->scope,control);
    if(scope_status!=XR_XIR_OK)return scope_status;
    uint32_t parameter=declaration.declaration.family==XR_HIR_CALL_VARIADIC_READ?0:input->formal;
    if(!receiver&&parameter>=declaration.declaration.count)return XR_XIR_BAD_STRUCTURE;
    XrXirCallableParameter formal=receiver?(XrXirCallableParameter){declaration.declaration.receiver_type,XR_PARAM_READ}:declaration.parameters[parameter];
    bool callback=declaration.declaration.family==XR_HIR_CALL_ARRAY&&!parameter;
    if(callback&&input->kind==XR_HIR_CALL_VALUE&&declaration.callback_received)return XR_XIR_BAD_STRUCTURE;
    if((input->kind==XR_HIR_CALL_VALUE||input->kind==XR_HIR_CALL_EXACT||input->kind==XR_HIR_CALL_FINAL)&&
        input->type==XR_XIR_UNIT&&declaration.declaration.family!=XR_HIR_CALL_REQUIREMENT) {
        if(input->reason)*input->reason="unit argument is not admitted";return XR_XIR_BAD_TYPE;
    }
    bool callback_final=callback&&input->kind==XR_HIR_CALL_FINAL;
    if(callback_final) {
        if(!declaration.callback_received||declaration.callback_use>=body->count)return XR_XIR_BAD_STRUCTURE;
        HirBodyUse received=body->uses[declaration.callback_use];
        if(received.source_value!=input->value||received.action.target!=input->type)return XR_XIR_BAD_STRUCTURE;
        XrXirStatus status=body_array_callback(body,&declaration,input->type,control,input->reason);if(status!=XR_XIR_OK)return status;
        formal.type=input->type;
    } else if(callback&&!declaration.callback_ready)formal.type=XR_XIR_UNIT;
    XrHirBodyAction action={.source=formal.type,.target=formal.type,.formal=input->formal,.argument=input->argument,
        .receiver_count=declaration.declaration.receiver_count,.reference=formal.mode==XR_PARAM_REF};
    if(callback_final)action.argument_count=xr_xir_callable_signature(body->types,input->type)->parameter_count;
    if(input->spread&&action.reference)return XR_XIR_BAD_TYPE;
    if(input->kind==XR_HIR_CALL_PLAN) {
        if(callback&&declaration.callback_ready)action.argument_count=xr_xir_callable_signature(body->types,formal.type)->parameter_count;
        *output=action;*use=(XrHirBodyUse){0};return body_poll(control);
    }
    if(input->kind==XR_HIR_CALL_SPREAD) {
        const XrXirTypeNode *tuple=xr_xir_tuple_signature(body->types,input->type);
        if(!tuple||input->spread_field>=tuple->parameter_count||action.reference)return XR_XIR_BAD_TYPE;
        action.target=tuple->parameters[input->spread_field].type;action.operation=XR_XIR_TUPLE_FIELD;
        action.field=input->spread_field;
    } else if(input->kind==XR_HIR_CALL_ACCESS) {
        if(input->access!=(action.reference?XR_CALL_ARG_REF:XR_CALL_ARG_PLAIN))return XR_XIR_BAD_TYPE;
    } else if(input->kind==XR_HIR_CALL_VALUE||input->kind==XR_HIR_CALL_EXACT||input->kind==XR_HIR_CALL_FINAL) {
        if(input->kind==XR_HIR_CALL_EXACT&&input->type!=formal.type)return XR_XIR_BAD_TYPE;
        if(input->kind==XR_HIR_CALL_FINAL) {
            if(receiver&&input->type!=formal.type)return XR_XIR_BAD_TYPE;
            uint32_t ordinal=declaration.argument_count;
            if(declaration.declaration.family==XR_HIR_CALL_REQUIREMENT) {
                if((!ordinal)!=receiver||(!receiver&&input->formal!=ordinal-1))return XR_XIR_BAD_STRUCTURE;
            } else if(input->formal!=ordinal)return XR_XIR_BAD_STRUCTURE;
            bool implicit_receiver=receiver||(input->formal<declaration.declaration.parameter_offset&&input->argument==UINT32_MAX);
            if(!implicit_receiver&&!input->defaulted) {
                if(input->formal<declaration.declaration.parameter_offset||
                    input->argument!=input->formal-declaration.declaration.parameter_offset||
                    input->argument>=declaration.access_count||!(declaration.accesses[parameter]&1))return XR_XIR_BAD_STRUCTURE;
            }
            if(!receiver&&!callback) {
                XrXirStatus status=body_call_final_type(body,input,formal,control);if(status!=XR_XIR_OK)return status;
            }
            if(input->defaulted) {
                if(declaration.declaration.family!=XR_HIR_CALL_FUNCTION&&
                    declaration.declaration.family!=XR_HIR_CALL_CONSTRUCTOR)return XR_XIR_BAD_STRUCTURE;
                const XrXirDefaultTable *defaults=body->declarations.defaults;bool found=false;
                if(!defaults)return XR_XIR_BAD_STRUCTURE;
                for(uint32_t d=0;d<defaults->count;++d) {
                    if(!(d%64)){XrXirStatus status=body_poll(control);if(status!=XR_XIR_OK)return status;}
                    XrXirDefaultBinding binding=defaults->records[d];
                    if(binding.owner_kind==XR_XIR_DEFAULT_PARAMETER&&binding.owner==declaration.declaration.function&&
                        binding.ordinal==input->formal&&binding.function==input->default_function){found=true;break;}
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
        body->uses[use->index].required_instruction=true;
        body->uses[use->index].has_field=true;body->uses[use->index].field=input->spread_field;
        body->uses[use->index].has_operand=true;body->uses[use->index].source_operand=input->value;
    }
    if(input->kind==XR_HIR_CALL_ACCESS) {
        HirBodySignature *call=&((HirBodySignature *)body->signatures.items)[signature.index];
        call->accesses[parameter]|=1;++call->access_count;
    }
    if(input->kind==XR_HIR_CALL_FINAL) {
        HirBodySignature *call=&((HirBodySignature *)body->signatures.items)[signature.index];
        if(call->last_argument!=UINT32_MAX)body->uses[call->last_argument].next_argument=use->index;
        else call->first_argument=use->index;
        call->last_argument=use->index;++call->argument_count;
    }
    body->uses[use->index].source_value=input->value;*output=action;return XR_XIR_OK;
}
XR_FUNC XrXirStatus xr_hir_body_call_finish(XrHirBody *body,XrHirBodySignature signature,
    const XrXirInstruction *instruction,XrCompileRequestControl *control,XrHirBodyUse *use,XrHirBodyAction *output) {
    if(!body||body->ended||signature.owner!=body||signature.index>=body->signatures.count||!instruction||!use||!output)
        return XR_XIR_BAD_STRUCTURE;
    HirBodySignature row=((HirBodySignature *)body->signatures.items)[signature.index];
    bool native=row.declaration.family==XR_HIR_CALL_NATIVE||row.declaration.family==XR_HIR_CALL_VARIADIC_READ||row.declaration.family==XR_HIR_CALL_ARRAY;
    uint32_t expected=row.declaration.count+(row.declaration.family==XR_HIR_CALL_REQUIREMENT?1u:0u);
    if(row.finished||((!native||row.declaration.family==XR_HIR_CALL_ARRAY)&&row.argument_count!=expected)||
        (native&&row.declaration.family!=XR_HIR_CALL_VARIADIC_READ&&row.argument_count>expected))return XR_XIR_BAD_STRUCTURE;
    XrXirOp operation=instruction->op;
    if(native) {
        if(operation!=XR_XIR_INVALID)return XR_XIR_BAD_STRUCTURE;
        XrXirStatus status=body_poll(control);if(status!=XR_XIR_OK)return status;
        ((HirBodySignature *)body->signatures.items)[signature.index].finished=true;
        *use=(XrHirBodyUse){0};*output=(XrHirBodyAction){.target=instruction->type};return XR_XIR_OK;
    }
    if(row.declaration.family==XR_HIR_CALL_INDIRECT) {
        if(operation!=XR_XIR_CALL_INDIRECT||instruction->immediate!=row.declaration.callee_value)return XR_XIR_BAD_STRUCTURE;
    } else if(row.declaration.family==XR_HIR_CALL_REQUIREMENT) {
        if(operation!=XR_XIR_CALL_REQUIREMENT||instruction->targets[0]!=row.declaration.interface_declaration||
            instruction->targets[1]!=row.declaration.interface_member)return XR_XIR_BAD_STRUCTURE;
    } else if((operation!=XR_XIR_CALL&&operation!=XR_XIR_GO&&operation!=XR_XIR_GENERATOR_NEW)||
        instruction->immediate!=row.declaration.function)return XR_XIR_BAD_STRUCTURE;
    XrHirBodyInput input={.origin=row.declaration.origin,.definition=row.declaration.origin,
        .rule=XR_HIR_BODY_IDENTITY,.source=instruction->type,.target=instruction->type};
    XrXirStatus status=xr_hir_body_check(body,body->types,&input,control,use,NULL);if(status!=XR_XIR_OK)return status;
    HirBodyUse *record=&body->uses[use->index];record->action.operation=operation;
    record->required_instruction=true;record->is_call=true;record->signature=signature;
    record->action.argument_count=row.argument_count;
    ((HirBodySignature *)body->signatures.items)[signature.index].finished=true;
    *output=record->action;return XR_XIR_OK;
}
