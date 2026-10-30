/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * Malformed receiver records cannot acquire declaration or call authority. */
static void place_receivers(void) {
    XrCompileResources *resources=NULL;XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrXirType object=(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE,nullable=(XrXirType)(object+1);
    XrXirNominalField fields[]={{{"value",5},XR_XIR_I64,XR_XIR_FIELD_MUTABLE},{{"fixed",5},XR_XIR_I64,0}};
    XrXirNominalDeclaration nominal={.fields=fields,.field_count=2,.kind=XR_XIR_NOMINAL_CLASS};
    XrXirNominalTable nominals={.declarations=&nominal,.count=1};
    XrXirTypeNode nodes[]={{.kind=XR_XIR_TYPE_NOMINAL,.nominal={.declaration=0}},
        {.kind=XR_XIR_TYPE_NULLABLE,.element=object}};
    XrXirTypes types={.nodes=nodes,.count=2,.nominals=&nominals};
    XrXirFunction functions[2]={{0},{0}},*function_view=functions;
    XrXirFunctionIdentity identities[2]={{0},{0}},*identity_view=identities;
    uint32_t function_count=2,module_count=1;
    XrHirBodyDeclarations declarations={.functions=&function_view,.identities=&identity_view,
        .function_count=&function_count,.module_count=&module_count};
    XrHirBody *body=NULL;CHECK(xr_hir_body_new(resources,&types,&body)==XR_XIR_OK);
    CHECK(xr_hir_body_declarations(body,&declarations)==XR_XIR_OK);
    XrHirBodyScope root={0},scope={0},sibling={0},foreign={0};
    CHECK(xr_hir_body_scope(body,root,(XrHirBodyOrigin){.function=0,.node=1},NULL,&root)==XR_XIR_OK);
    CHECK(xr_hir_body_scope(body,root,(XrHirBodyOrigin){.function=0,.node=2},NULL,&scope)==XR_XIR_OK);
    CHECK(xr_hir_body_scope(body,root,(XrHirBodyOrigin){.function=0,.node=3},NULL,&sibling)==XR_XIR_OK);
    CHECK(xr_hir_body_scope(body,foreign,(XrHirBodyOrigin){.function=1},NULL,&foreign)==XR_XIR_OK);
    XrHirBindingInput declaration={.types=&types,.scope=scope,.key={0,11,10,0,0,UINT32_MAX},
        .origin={.function=0,.node=10,.binding=11},.name="object",.name_length=6,
        .logical=nullable,.physical=nullable,.storage=XR_HIR_BODY_LOCAL};
    XrHirBodyBinding binding={0};CHECK(xr_hir_body_binding_declare(body,&declaration,NULL,&binding,NULL)==XR_XIR_OK);
    XrHirPlaceAction not_ready={.location=123};
    XrHirPlaceInput early={.types=&types,.scope=scope,.origin={.function=0},.root=binding,
        .kind=XR_HIR_PLACE_ROOT,.type=object};
    CHECK(xr_hir_body_place(body,&early,NULL,&not_ready,NULL)==XR_XIR_BAD_STRUCTURE&&not_ready.location==123&&!not_ready.use.owner);
    XrHirBindingUseInput initialize={.types=&types,.scope=scope,.origin={.function=0},
        .kind=XR_HIR_BINDING_INITIALIZE,.value=nullable,.value_id=50};XrHirBindingAction initialization;
    CHECK(xr_hir_body_binding_use(body,binding,&initialize,NULL,&initialization,NULL)==XR_XIR_OK);
    XrHirBodyAction consumed;CHECK(xr_hir_body_consume(body,initialization.use,&consumed)==XR_XIR_OK);
    CHECK(xr_hir_body_emit(body,initialization.use,nullable,50)==XR_XIR_OK);
    CHECK(xr_hir_body_project(body,initialization.use,&(XrHirBodyProjection){.function=0,.type=nullable,
        .has_value=true,.value=50,.source_value=50},NULL,NULL)==XR_XIR_OK);
    XrHirPlaceInput input={.types=&types,.scope=scope,.origin={.function=0},.root=binding,
        .kind=XR_HIR_PLACE_ROOT,.type=XR_XIR_I64};XrHirPlaceAction place={0};
    CHECK(xr_hir_body_place(body,&input,NULL,&place,NULL)==XR_XIR_BAD_TYPE&&!place.use.owner);
    input.type=object;input.scope=sibling;
    CHECK(xr_hir_body_place(body,&input,NULL,&place,NULL)==XR_XIR_BAD_STRUCTURE);
    input.scope=foreign;input.origin.function=1;
    CHECK(xr_hir_body_place(body,&input,NULL,&place,NULL)==XR_XIR_BAD_STRUCTURE);
    input.scope=scope;input.origin.function=0;
    CHECK(xr_hir_body_place(body,&input,NULL,&place,NULL)==XR_XIR_OK&&place.object_root);
    CHECK(xr_hir_body_consume(body,place.use,&consumed)==XR_XIR_OK&&consumed.operation==XR_XIR_OBJECT_PLACE);
    CHECK(xr_hir_body_emit(body,place.use,object,51)==XR_XIR_OK);
    CHECK(xr_hir_body_project(body,place.use,&(XrHirBodyProjection){.function=0,.type=object,
        .operation=XR_XIR_OBJECT_PLACE,.has_instruction=true,.has_value=true,.value=51,.source_value=51},NULL,NULL)==XR_XIR_OK);
    XrHirPlaceInput field={.types=&types,.scope=scope,.origin={.function=0},.kind=XR_HIR_PLACE_FIELD,
        .parent=place.place,.field=0,.access=1,.type=XR_XIR_STRING};XrHirPlaceAction selected={0};
    CHECK(xr_hir_body_place(body,&field,NULL,&selected,NULL)==XR_XIR_BAD_TYPE&&!selected.use.owner);
    field.type=XR_XIR_I64;field.field=1;
    CHECK(xr_hir_body_place(body,&field,NULL,&selected,NULL)==XR_XIR_BAD_TYPE);
    field.field=0;CHECK(xr_hir_body_place(body,&field,NULL,&selected,NULL)==XR_XIR_OK&&selected.type==XR_XIR_I64);
    CHECK(xr_hir_body_consume(body,selected.use,&consumed)==XR_XIR_OK);
    CHECK(xr_hir_body_emit(body,selected.use,XR_XIR_I64,52)==XR_XIR_OK);
    XrHirBodyProjection projection={.function=0,.type=XR_XIR_I64,.operation=XR_XIR_FIELD_PLACE,
        .source_operands={99,0},.has_instruction=true,.has_value=true,.value=52,.source_value=52};
    CHECK(xr_hir_body_project(body,selected.use,&projection,NULL,NULL)==XR_XIR_BAD_STRUCTURE);
    projection.source_operands[0]=51;CHECK(xr_hir_body_project(body,selected.use,&projection,NULL,NULL)==XR_XIR_OK);
    CHECK(xr_hir_body_finish(body,NULL)==XR_XIR_OK);
    xr_hir_body_end(body);xr_hir_body_free(body);xr_compile_resources_release(resources);
    CHECK(!physical_live&&!physical_bytes);
}
static void contextual_call_receivers(void) {
    XrCompileResources *resources=NULL;XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrXirCallableParameter integer={XR_XIR_I64,XR_PARAM_READ},string={XR_XIR_STRING,XR_PARAM_READ};
    XrXirTypeNode nodes[]={
        {.kind=XR_XIR_TYPE_CALLABLE,.element=XR_XIR_NEVER,.parameters=&integer,.parameter_count=1,.result=XR_XIR_UNIT},
        {.kind=XR_XIR_TYPE_CALLABLE,.element=XR_XIR_NEVER,.parameters=&integer,.parameter_count=1,.result=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_CALLABLE,.element=XR_XIR_NEVER,.parameters=&string,.parameter_count=1,.result=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64}};
    XrXirTypes types={.nodes=nodes,.count=4};
    XrHirBody *body=NULL;CHECK(xr_hir_body_new(resources,&types,&body)==XR_XIR_OK);
    XrHirBodyScope scope={0};CHECK(xr_hir_body_scope(body,scope,(XrHirBodyOrigin){0},NULL,&scope)==XR_XIR_OK);
    XrXirType context=(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE;
    const XrNativeTypeDeclaration *native=xr_native_declaration_by_id(XR_NATIVE_DECLARATION_ARRAY);
    const XrNativeMemberDeclaration *member=xr_native_declaration_member(native,"map");CHECK(member);
    XrNativeMemberDeclaration fabricated=*member;
    XrHirSignatureInput declaration={.types=&types,.scope=scope,.family=XR_HIR_CALL_ARRAY,
        .native_parameters=&context,.count=1,.array=(XrXirType)(context+3),.array_value=99,
        .callback_error=XR_XIR_NEVER,.array_member=&fabricated};XrHirBodySignature signature={0};
    CHECK(xr_hir_body_signature(body,&declaration,NULL,&signature,NULL)==XR_XIR_BAD_STRUCTURE&&!signature.owner);
    declaration.array_member=member;
    CHECK(xr_hir_body_signature(body,&declaration,NULL,&signature,NULL)==XR_XIR_OK);
    XrHirArrayCallbackShape shape={0};
    CHECK(xr_hir_body_array_context(body,signature,declaration.array,98,&shape)==XR_XIR_BAD_STRUCTURE);
    CHECK(xr_hir_body_array_context(body,signature,declaration.array,99,&shape)==XR_XIR_OK&&
        shape.minimum==1&&shape.maximum==2&&!shape.has_result&&shape.parameters[0]==XR_XIR_I64);
    XrHirCallUseInput input={.types=&types,.scope=scope,.kind=XR_HIR_CALL_ACCESS,.access=XR_CALL_ARG_REF};
    XrHirBodyUse use={0};XrHirBodyAction action;
    CHECK(xr_hir_body_call_argument(body,signature,&input,NULL,&use,&action)==XR_XIR_BAD_TYPE);
    input.access=XR_CALL_ARG_PLAIN;
    CHECK(xr_hir_body_call_argument(body,signature,&input,NULL,&use,&action)==XR_XIR_OK);
    owned_marker(body,use);
    input.kind=XR_HIR_CALL_VALUE;input.type=(XrXirType)(context+1);input.value=40;
    CHECK(xr_hir_body_call_argument(body,signature,&input,NULL,&use,&action)==XR_XIR_OK);
    owned_projection(body,use,(XrHirBodyProjection){.function=0,.type=input.type,.source_value=40,.has_value=true,.value=40});
    input.kind=XR_HIR_CALL_FINAL;input.type=(XrXirType)(context+2);
    CHECK(xr_hir_body_call_argument(body,signature,&input,NULL,&use,&action)==XR_XIR_BAD_STRUCTURE);
    input.type=(XrXirType)(context+1);
    CHECK(xr_hir_body_call_argument(body,signature,&input,NULL,&use,&action)==XR_XIR_OK&&action.target==input.type);
    owned_projection(body,use,(XrHirBodyProjection){.function=0,.type=input.type,.source_value=40,.has_value=true,.value=40});
    CHECK(xr_hir_body_call_finish(body,signature,&(XrXirInstruction){.op=XR_XIR_INVALID,.type=XR_XIR_UNIT},
        NULL,&use,&action)==XR_XIR_OK&&!use.owner);
    CHECK(xr_hir_body_finish(body,NULL)==XR_XIR_OK);
    CHECK(xr_hir_body_signature(body,&declaration,NULL,&signature,NULL)==XR_XIR_OK);
    input.kind=XR_HIR_CALL_ACCESS;input.access=XR_CALL_ARG_PLAIN;
    CHECK(xr_hir_body_call_argument(body,signature,&input,NULL,&use,&action)==XR_XIR_OK);owned_marker(body,use);
    input.kind=XR_HIR_CALL_VALUE;input.type=(XrXirType)(context+2);input.value=41;
    CHECK(xr_hir_body_call_argument(body,signature,&input,NULL,&use,&action)==XR_XIR_OK);
    owned_projection(body,use,(XrHirBodyProjection){.function=0,.type=input.type,.source_value=41,.has_value=true,.value=41});
    input.kind=XR_HIR_CALL_FINAL;
    uint32_t before=xr_hir_body_use_count(body);
    CHECK(xr_hir_body_call_argument(body,signature,&input,NULL,&use,&action)==XR_XIR_BAD_TYPE&&xr_hir_body_use_count(body)==before);
    CHECK(xr_hir_body_finish(body,NULL)==XR_XIR_BAD_STRUCTURE);
    xr_hir_body_end(body);xr_hir_body_free(body);xr_compile_resources_release(resources);
    CHECK(!physical_live&&!physical_bytes);
}
