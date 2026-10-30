/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * Private owner boundaries supplement, and never replace, full Source gates. */
static void owned_marker(XrHirBody *body,XrHirBodyUse use) {
    XrHirBodyAction action;
    CHECK(xr_hir_body_consume(body,use,&action)==XR_XIR_OK);
    CHECK(xr_hir_body_retire(body,use)==XR_XIR_OK);
}
static void owned_projection(XrHirBody *body,XrHirBodyUse use,XrHirBodyProjection projection) {
    XrHirBodyAction action;CHECK(xr_hir_body_consume(body,use,&action)==XR_XIR_OK);
    CHECK(xr_hir_body_project(body,use,&projection,NULL,NULL)==XR_XIR_OK);
}
static void declaration_owners(void) {
    XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};XrCompileResources *resources=NULL;
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrXirType cell=(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE;
    XrXirTypeNode nodes[]={{.kind=XR_XIR_TYPE_CELL,.element=XR_XIR_I64},{.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64}};
    XrXirTypes types={.nodes=nodes,.count=2},foreign=types;
    XrXirType formals[]={cell,XR_XIR_I64};
    XrXirFunction functions[2]={{.parameters=formals,.parameter_count=2,.result=XR_XIR_I64},{.result=XR_XIR_UNIT}};
    XrXirFunctionIdentity identities[2]={{0},{0}};
    XrXirFunction *function_view=functions;XrXirFunctionIdentity *identity_view=identities;
    uint32_t function_count=2,module_count=1,slot_count=0;XrXirSlot *slots=NULL;
    XrXirDefaultTable defaults={0};XrXirInterfaceTable interfaces={0};
    XrHirBodyDeclarations views={&function_view,&identity_view,&function_count,&module_count,&defaults,&interfaces,&slots,&slot_count};
    XrHirBody *body=NULL,*other=NULL;
    CHECK(xr_hir_body_new(resources,&types,&body)==XR_XIR_OK&&xr_hir_body_new(resources,&types,&other)==XR_XIR_OK);
    CHECK(xr_hir_body_declarations(body,&views)==XR_XIR_OK&&xr_hir_body_declarations(other,&views)==XR_XIR_OK);
    XrHirBodyScope root={0},scope={0},sibling={0},foreign_scope={0};
    CHECK(xr_hir_body_scope(body,root,(XrHirBodyOrigin){.function=0,.module=0,.node=1},NULL,&root)==XR_XIR_OK);
    CHECK(xr_hir_body_scope(body,root,(XrHirBodyOrigin){.function=0,.module=0,.node=2},NULL,&scope)==XR_XIR_OK);
    CHECK(xr_hir_body_scope(body,root,(XrHirBodyOrigin){.function=0,.module=0,.node=3},NULL,&sibling)==XR_XIR_OK);
    CHECK(xr_hir_body_scope(other,foreign_scope,(XrHirBodyOrigin){.function=0},NULL,&foreign_scope)==XR_XIR_OK);
    char *caller_name=body_allocate(8);CHECK(caller_name);memcpy(caller_name,"counter",8);
    XrHirBindingInput declaration={.types=&types,.key={0,11,10,1,0,UINT32_MAX},.scope=scope,
        .origin={.function=0,.module=0,.node=10,.binding=11},.name=caller_name,.name_length=7,
        .logical=XR_XIR_I64,.physical=cell,.storage=XR_HIR_BODY_LOCAL,.mode=XR_PARAM_READ,.rebindable=true,.location=UINT32_MAX};
    XrHirBodyBinding binding={0},sentinel={other,123},out=sentinel;
    XrHirBindingInput invalid=declaration;invalid.types=&foreign;
    CHECK(xr_hir_body_binding_declare(body,&invalid,NULL,&out,NULL)==XR_XIR_BAD_STRUCTURE&&out.owner==sentinel.owner&&out.index==123);
    invalid=declaration;invalid.scope=foreign_scope;
    CHECK(xr_hir_body_binding_declare(body,&invalid,NULL,&out,NULL)==XR_XIR_BAD_STRUCTURE);
    invalid=declaration;invalid.origin.function=1;
    CHECK(xr_hir_body_binding_declare(body,&invalid,NULL,&out,NULL)==XR_XIR_BAD_STRUCTURE);
    invalid=declaration;invalid.key.declaration=12;
    CHECK(xr_hir_body_binding_declare(body,&invalid,NULL,&out,NULL)==XR_XIR_BAD_STRUCTURE);
    CHECK(xr_hir_body_binding_declare(body,&declaration,NULL,&binding,NULL)==XR_XIR_OK);
    memset(caller_name,0xa5,8);body_release(caller_name);
    XrHirBindingInput received={0};CHECK(xr_hir_body_binding_info(body,binding,&received)&&!strcmp(received.name,"counter")&&!received.types);
    XrHirBindingUseInput use={.types=&types,.scope=scope,.origin={.function=0},.kind=XR_HIR_BINDING_INITIALIZE,.value=XR_XIR_I64};
    XrHirBindingAction initialization;
    CHECK(xr_hir_body_binding_use(body,binding,&use,NULL,&initialization,NULL)==XR_XIR_OK);
    XrHirBodyAction action;CHECK(xr_hir_body_consume(body,initialization.use,&action)==XR_XIR_OK&&action.target==cell);
    CHECK(xr_hir_body_emit(body,initialization.use,cell,20)==XR_XIR_OK);
    XrHirBodyProjection projection={.function=0,.instruction=0,.block=0,.value=20,.source_value=20,.type=cell,
        .operation=XR_XIR_SLOT_LOAD,.has_instruction=true,.has_value=true};
    CHECK(xr_hir_body_project(body,initialization.use,&projection,NULL,NULL)==XR_XIR_BAD_STRUCTURE);
    projection.operation=XR_XIR_CELL_NEW;CHECK(xr_hir_body_project(body,initialization.use,&projection,NULL,NULL)==XR_XIR_OK);
    use.kind=XR_HIR_BINDING_READ;use.value=XR_XIR_UNIT;use.scope=sibling;
    XrHirBindingAction read;
    CHECK(xr_hir_body_binding_use(body,binding,&use,NULL,&read,NULL)==XR_XIR_BAD_STRUCTURE);
    use.scope=scope;use.types=&foreign;
    CHECK(xr_hir_body_binding_use(body,binding,&use,NULL,&read,NULL)==XR_XIR_BAD_STRUCTURE);
    use.types=&types;CHECK(xr_hir_body_binding_use(body,binding,&use,NULL,&read,NULL)==XR_XIR_OK&&read.location==20);
    owned_projection(body,read.use,(XrHirBodyProjection){.function=0,.instruction=1,.block=0,.type=XR_XIR_I64,
        .operation=XR_XIR_CELL_READ,.source_operands={20,0},.has_instruction=true,.has_value=true,.value=21,.source_value=21});
    use.kind=XR_HIR_BINDING_WRITE;use.value=XR_XIR_STRING;
    CHECK(xr_hir_body_binding_use(body,binding,&use,NULL,&read,NULL)==XR_XIR_BAD_TYPE);
    XrHirSignatureInput signature_input={.types=&types,.scope=scope,.origin={.function=0},
        .family=XR_HIR_CALL_FUNCTION,.function=0,.count=2};XrHirBodySignature signature={0};
    CHECK(xr_hir_body_signature(body,&signature_input,NULL,&signature,NULL)==XR_XIR_OK);
    XrHirCallUseInput argument={.types=&types,.scope=scope,.origin={.function=0},.kind=XR_HIR_CALL_FINAL,
        .formal=0,.argument=0,.type=cell,.value=20};XrHirBodyUse argument_use={0};
    CHECK(xr_hir_body_call_argument(body,signature,&argument,NULL,&argument_use,&action)==XR_XIR_BAD_STRUCTURE);
    argument.kind=XR_HIR_CALL_ACCESS;argument.access=XR_CALL_ARG_PLAIN;
    CHECK(xr_hir_body_call_argument(body,signature,&argument,NULL,&argument_use,&action)==XR_XIR_BAD_TYPE);
    argument.access=XR_CALL_ARG_REF;
    CHECK(xr_hir_body_call_argument(body,signature,&argument,NULL,&argument_use,&action)==XR_XIR_OK&&action.reference);
    owned_marker(body,argument_use);
    argument.formal=1;argument.argument=1;argument.access=XR_CALL_ARG_PLAIN;
    CHECK(xr_hir_body_call_argument(body,signature,&argument,NULL,&argument_use,&action)==XR_XIR_OK&&!action.reference);
    owned_marker(body,argument_use);
    argument.kind=XR_HIR_CALL_FINAL;argument.formal=0;argument.argument=0;argument.type=cell;argument.value=20;
    CHECK(xr_hir_body_call_argument(body,signature,&argument,NULL,&argument_use,&action)==XR_XIR_OK);
    owned_projection(body,argument_use,(XrHirBodyProjection){.function=0,.type=cell,.has_value=true,.value=20,.source_value=20});
    argument.formal=1;argument.argument=1;argument.type=XR_XIR_I64;argument.value=7;
    argument.type=XR_XIR_STRING;
    CHECK(xr_hir_body_call_argument(body,signature,&argument,NULL,&argument_use,&action)==XR_XIR_BAD_TYPE);
    argument.type=XR_XIR_I64;
    CHECK(xr_hir_body_call_argument(body,signature,&argument,NULL,&argument_use,&action)==XR_XIR_OK);
    owned_projection(body,argument_use,(XrHirBodyProjection){.function=0,.formal=1,.argument=1,.type=XR_XIR_I64,.has_value=true,.value=7,.source_value=7});
    XrXirInstruction call={.op=XR_XIR_CALL,.type=XR_XIR_I64,.immediate=0};XrHirBodyUse call_use={0};
    CHECK(xr_hir_body_call_finish(body,signature,&call,NULL,&call_use,&action)==XR_XIR_OK&&action.argument_count==2);
    CHECK(xr_hir_body_consume(body,call_use,&action)==XR_XIR_OK);
    uint32_t actuals[]={20,8};projection=(XrHirBodyProjection){.function=0,.instruction=2,.block=0,
        .type=XR_XIR_I64,.operation=XR_XIR_CALL,.call=call,.has_call=true,.has_instruction=true,.source_arguments=actuals,.source_argument_count=2};
    CHECK(xr_hir_body_project(body,call_use,&projection,NULL,NULL)==XR_XIR_BAD_STRUCTURE);
    actuals[1]=7;projection.call.immediate=1;
    CHECK(xr_hir_body_project(body,call_use,&projection,NULL,NULL)==XR_XIR_BAD_STRUCTURE);
    projection.call.immediate=0;CHECK(xr_hir_body_project(body,call_use,&projection,NULL,NULL)==XR_XIR_OK);
    CHECK(xr_hir_body_finish(body,NULL)==XR_XIR_OK);
    XrCompileResourceStats before={0},after={0};CHECK(xr_compile_resources_stats(resources,&before)==XR_COMPILE_RESOURCE_OK);
    size_t count=physical_live,bytes=physical_bytes,attempts=physical_attempts;
    declaration.key.declaration=12;declaration.origin.binding=12;declaration.name="rollback";declaration.name_length=8;
    physical_fail=physical_attempts;
    CHECK(xr_hir_body_binding_declare(body,&declaration,NULL,&out,NULL)==XR_XIR_OUT_OF_MEMORY);physical_fail=SIZE_MAX;
    CHECK(physical_live==count&&physical_bytes==bytes&&physical_attempts>attempts);
    Cancel cancellation={0,3};XrCompileRequestOptions options={.cancel_poll=cancel,.cancel_context=&cancellation};XrCompileRequestControl control={0};
    CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
    attempts=physical_attempts;
    CHECK(xr_hir_body_binding_declare(body,&declaration,&control,&out,NULL)==XR_XIR_CANCELLED&&physical_attempts>attempts);
    CHECK(xr_compile_resources_stats(resources,&after)==XR_COMPILE_RESOURCE_OK&&after.held_bytes==before.held_bytes);
    CHECK(physical_live==count&&physical_bytes==bytes);
    CHECK(xr_hir_body_binding_declare(body,&declaration,NULL,&out,NULL)==XR_XIR_OK);
    xr_hir_body_end(body);memset(&types,0xa5,sizeof(types));
    CHECK(xr_hir_body_binding_info(body,binding,&received)&&!strcmp(received.name,"counter"));
    CHECK(xr_hir_body_binding_use(body,binding,&use,NULL,&read,NULL)==XR_XIR_BAD_STRUCTURE);
    xr_hir_body_free(body);xr_hir_body_free(other);xr_compile_resources_release(resources);
    CHECK(!physical_live&&!physical_bytes);
}
