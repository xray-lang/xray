/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * Actual tuple receivers preserve logical fields and physical operand order. */
static void tuple_receivers(void) {
    XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};XrCompileResources *resources=NULL;
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats initial={0},before={0},after={0};CHECK(xr_compile_resources_stats(resources,&initial)==XR_COMPILE_RESOURCE_OK);
    XrXirCallableParameter mixed[]={{XR_XIR_I64,0},{XR_XIR_UNIT,0},{XR_XIR_STRING,0}};
    XrXirCallableParameter units[]={{XR_XIR_UNIT,0},{XR_XIR_UNIT,0}};
    XrXirTypeNode nodes[]={{.kind=XR_XIR_TYPE_TUPLE,.parameters=mixed,.parameter_count=3},
        {.kind=XR_XIR_TYPE_TUPLE,.parameters=units,.parameter_count=2}};
    XrXirTypes types={.nodes=nodes,.count=2},foreign=types;
    XrXirType pair=(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE,all_unit=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+1);
    XrHirBody *body=NULL,*other=NULL;CHECK(xr_hir_body_new(resources,&types,&body)==XR_XIR_OK);
    CHECK(xr_compile_resources_stats(resources,&before)==XR_COMPILE_RESOURCE_OK);uint64_t body_frame=before.held_bytes;
    CHECK(xr_hir_body_new(resources,&types,&other)==XR_XIR_OK);
    XrHirTupleShape shape={0},zero={0},all={0},sentinel={other,77},output=sentinel;
    CHECK(xr_hir_body_tuple_receive(body,&foreign,pair,NULL,&output,NULL)==XR_XIR_BAD_STRUCTURE&&output.owner==sentinel.owner&&output.index==77);
    CHECK(xr_hir_body_tuple_receive(body,&types,pair,NULL,&shape,NULL)==XR_XIR_OK);
    CHECK(xr_hir_body_tuple_receive(body,&types,XR_XIR_UNIT,NULL,&zero,NULL)==XR_XIR_OK);
    CHECK(xr_hir_body_tuple_receive(body,&types,all_unit,NULL,&all,NULL)==XR_XIR_OK);
    XrHirTupleInfo info={0};CHECK(xr_hir_body_tuple_info(body,shape,&info));
    CHECK(info.count==3&&info.physical_count==2&&info.fields[1].physical==UINT32_MAX&&
        info.physical_fields[0]==0&&info.physical_fields[1]==2);
    CHECK(xr_hir_body_tuple_arity(body,zero,0,NULL)==XR_XIR_OK&&xr_hir_body_tuple_arity(body,all,0,NULL)==XR_XIR_BAD_TYPE);
    mixed[0].type=XR_XIR_BOOL;
    CHECK(xr_hir_body_tuple_info(body,shape,&info)&&info.fields[0].type==XR_XIR_I64);
    mixed[0].type=XR_XIR_I64;
    XrXirType parameters[]={XR_XIR_I64,XR_XIR_STRING};uint32_t operands[]={0,1};
    XrXirInstruction instructions[]={
        {XR_XIR_TUPLE_NEW,pair,{0,2},{0},0,{0}},
        {XR_XIR_TUPLE_FIELD,XR_XIR_I64,{2,0},{0},0,{0}},
        {XR_XIR_TUPLE_FIELD,XR_XIR_UNIT,{2,0},{0},1,{0}},
        {XR_XIR_TUPLE_NEW,all_unit,{0,0},{0},0,{0}}};
    XrXirFunction function={.parameters=parameters,.parameter_count=2,.instructions=instructions,.instruction_count=4,
        .operands=operands,.operand_count=2};XrXirFunction *functions=&function;
    XrXirFunctionIdentity identity={0},*identities=&identity;uint32_t one=1;
    XrHirBodyDeclarations declarations={.functions=&functions,.identities=&identities,.function_count=&one,.module_count=&one};
    CHECK(xr_hir_body_declarations(body,&declarations)==XR_XIR_OK);
    XrHirBodyScope scope={0};XrHirBodyOrigin origin={.function=0,.module=0,.node=41};
    CHECK(xr_hir_body_scope(body,(XrHirBodyScope){0},origin,NULL,&scope)==XR_XIR_OK);
    XrHirTupleExpected wanted[]={{XR_XIR_I64,true,false,false},{XR_XIR_UNIT,true,false,false},{XR_XIR_STRING,true,false,false}};
    XrHirTupleContextInput context_input={.types=&types,.fields=wanted,.count=3,.kind=XR_HIR_TUPLE_EXACT};
    XrHirTupleContext context={0};CHECK(xr_hir_body_tuple_context(body,&context_input,NULL,&context,NULL)==XR_XIR_OK);
    memset(wanted,0xa5,sizeof(wanted));XrHirTupleExpected expected={0};
    CHECK(xr_hir_body_tuple_expected(body,context,2,&expected)&&expected.present&&expected.type==XR_XIR_STRING);
    XrHirTupleValue values[]={{XR_XIR_I64,0},{XR_XIR_UNIT,UINT32_MAX},{XR_XIR_STRING,1}};
    XrHirTupleConstructInput construct={.types=&types,.scope=scope,.origin=origin,.shape=shape,.context=context,.values=values,.count=3};
    XrHirTupleAction action={0};
    construct.shape=sentinel;CHECK(xr_hir_body_tuple_construct(body,&construct,NULL,&action,NULL)==XR_XIR_BAD_STRUCTURE);construct.shape=shape;
    values[0].type=XR_XIR_BOOL;CHECK(xr_hir_body_tuple_construct(body,&construct,NULL,&action,NULL)==XR_XIR_BAD_TYPE);values[0].type=XR_XIR_I64;
    CHECK(xr_hir_body_tuple_construct(body,&construct,NULL,&action,NULL)==XR_XIR_OK&&action.operation==XR_XIR_TUPLE_NEW);
    CHECK(xr_hir_body_finish(body,NULL)==XR_XIR_BAD_STRUCTURE);
    XrHirBodyAction emitted={0};CHECK(xr_hir_body_consume(body,action.use,&emitted)==XR_XIR_OK&&emitted.target==pair);
    CHECK(xr_hir_body_emit(body,action.use,pair,2)==XR_XIR_OK);
    uint32_t value_map[]={0,1,2,3,4,5};
    XrHirBodyProjection projection={.function=0,.instruction=0,.type=pair,.operation=XR_XIR_TUPLE_NEW,.has_instruction=true,
        .source_arguments=operands,.source_argument_count=2,.source_value_map=value_map,.source_value_count=6};
    uint32_t reversed[]={1,0};projection.source_arguments=reversed;
    CHECK(xr_hir_body_project(body,action.use,&projection,NULL,NULL)==XR_XIR_BAD_STRUCTURE);projection.source_arguments=operands;
    parameters[0]=XR_XIR_BOOL;CHECK(xr_hir_body_project(body,action.use,&projection,NULL,NULL)==XR_XIR_BAD_STRUCTURE);parameters[0]=XR_XIR_I64;
    CHECK(xr_hir_body_project(body,action.use,&projection,NULL,NULL)==XR_XIR_OK);
    XrHirTupleFieldInput field={.types=&types,.scope=scope,.origin=origin,.shape=shape,.receiver_type=pair,.receiver=2,.field=0};
    CHECK(xr_hir_body_tuple_field(body,&field,NULL,&action,NULL)==XR_XIR_OK);
    CHECK(xr_hir_body_consume(body,action.use,&emitted)==XR_XIR_OK);
    projection=(XrHirBodyProjection){.function=0,.instruction=1,.type=XR_XIR_I64,.operation=XR_XIR_TUPLE_FIELD,
        .source_operands={2,0},.has_instruction=true,.source_value_map=value_map,.source_value_count=6};
    instructions[1].args[0]=3;CHECK(xr_hir_body_project(body,action.use,&projection,NULL,NULL)==XR_XIR_BAD_STRUCTURE);instructions[1].args[0]=2;
    CHECK(xr_hir_body_project(body,action.use,&projection,NULL,NULL)==XR_XIR_OK);
    field.field=1;CHECK(xr_hir_body_tuple_field(body,&field,NULL,&action,NULL)==XR_XIR_OK&&action.type==XR_XIR_UNIT);
    CHECK(xr_hir_body_consume(body,action.use,&emitted)==XR_XIR_OK);projection.instruction=2;projection.type=XR_XIR_UNIT;projection.immediate=1;
    CHECK(xr_hir_body_project(body,action.use,&projection,NULL,NULL)==XR_XIR_OK);
    field.field=3;CHECK(xr_hir_body_tuple_field(body,&field,NULL,&action,NULL)==XR_XIR_BAD_TYPE);
    field.field=0;field.origin.module=1;CHECK(xr_hir_body_tuple_field(body,&field,NULL,&action,NULL)==XR_XIR_BAD_STRUCTURE);
    XrHirTupleValue no_values[]={{XR_XIR_UNIT,UINT32_MAX},{XR_XIR_UNIT,UINT32_MAX}};
    construct=(XrHirTupleConstructInput){.types=&types,.scope=scope,.origin=origin,.shape=all,.values=no_values,.count=2};
    CHECK(xr_hir_body_tuple_construct(body,&construct,NULL,&action,NULL)==XR_XIR_OK&&action.operation==XR_XIR_TUPLE_NEW&&action.tuple.physical_count==0);
    CHECK(xr_hir_body_consume(body,action.use,&emitted)==XR_XIR_OK);
    projection=(XrHirBodyProjection){.function=0,.instruction=3,.type=all_unit,.operation=XR_XIR_TUPLE_NEW};
    CHECK(xr_hir_body_project(body,action.use,&projection,NULL,NULL)==XR_XIR_BAD_STRUCTURE);projection.has_instruction=true;
    CHECK(xr_hir_body_project(body,action.use,&projection,NULL,NULL)==XR_XIR_OK);
    construct.shape=zero;construct.values=NULL;construct.count=0;
    CHECK(xr_hir_body_tuple_construct(body,&construct,NULL,&action,NULL)==XR_XIR_OK&&action.operation==XR_XIR_INVALID);
    CHECK(xr_hir_body_consume(body,action.use,&emitted)==XR_XIR_OK);
    projection=(XrHirBodyProjection){.function=0,.type=XR_XIR_UNIT};
    CHECK(xr_hir_body_project(body,action.use,&projection,NULL,NULL)==XR_XIR_OK&&xr_hir_body_finish(body,NULL)==XR_XIR_OK);
    CHECK(xr_compile_resources_stats(resources,&before)==XR_COMPILE_RESOURCE_OK);
    size_t live=physical_live,bytes=physical_bytes;physical_fail=physical_attempts;
    CHECK(xr_hir_body_tuple_receive(other,&types,pair,NULL,&output,NULL)==XR_XIR_OUT_OF_MEMORY);physical_fail=SIZE_MAX;
    CHECK(output.owner==sentinel.owner&&output.index==77&&physical_live==live&&physical_bytes==bytes);
    Cancel cancellation={0,6};XrCompileRequestOptions options={.cancel_poll=cancel,.cancel_context=&cancellation};XrCompileRequestControl control={0};
    CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_hir_body_tuple_receive(other,&types,pair,&control,&output,NULL)==XR_XIR_CANCELLED);
    CHECK(physical_live==live&&physical_bytes==bytes&&xr_compile_resources_stats(resources,&after)==XR_COMPILE_RESOURCE_OK&&after.held_bytes==before.held_bytes);
    control=(XrCompileRequestControl){0};options=(XrCompileRequestOptions){.deadline_mode=XR_COMPILE_DEADLINE_AFTER};
    CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_hir_body_tuple_receive(other,&types,pair,&control,&output,NULL)==XR_XIR_DEADLINE);
    CHECK(physical_live==live&&physical_bytes==bytes&&output.owner==sentinel.owner&&output.index==77);
    control=(XrCompileRequestControl){0};options=(XrCompileRequestOptions){0};
    CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_hir_body_tuple_receive(other,&types,pair,&control,&output,NULL)==XR_XIR_OK);
    uint32_t count=xr_hir_body_use_count(body);xr_hir_body_end(body);
    XrHirTupleView view={0};CHECK(xr_hir_tuple_view(&types,pair,NULL,&view,NULL)==XR_XIR_OK&&xr_hir_body_use_count(body)==count);
    CHECK(!xr_hir_body_tuple_info(body,shape,&info)&&xr_hir_body_tuple_receive(body,&types,pair,NULL,&output,NULL)==XR_XIR_BAD_STRUCTURE);
    xr_hir_body_free(body);xr_hir_body_free(other);
    CHECK(xr_compile_resources_stats(resources,&after)==XR_COMPILE_RESOURCE_OK&&after.held_bytes==initial.held_bytes);
    xr_compile_resources_release(resources);CHECK(!physical_live&&!physical_bytes);
    limits=(XrCompileResourceLimits){XR_COMPILE_MEMORY_FINITE,body_frame,false};resources=NULL;body=NULL;
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_stats(resources,&initial)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_hir_body_new(resources,&types,&body)==XR_XIR_OK&&xr_compile_resources_stats(resources,&before)==XR_COMPILE_RESOURCE_OK);
    output=(XrHirTupleShape){NULL,77};live=physical_live;bytes=physical_bytes;
    CHECK(xr_hir_body_tuple_receive(body,&types,pair,NULL,&output,NULL)==XR_XIR_MEMORY_LIMIT);
    CHECK(output.owner==NULL&&output.index==77&&physical_live==live&&physical_bytes==bytes);
    CHECK(xr_compile_resources_stats(resources,&after)==XR_COMPILE_RESOURCE_OK&&after.held_bytes==before.held_bytes);
    xr_hir_body_end(body);xr_hir_body_free(body);
    CHECK(xr_compile_resources_stats(resources,&after)==XR_COMPILE_RESOURCE_OK&&after.held_bytes==initial.held_bytes);
    xr_compile_resources_release(resources);CHECK(!physical_live&&!physical_bytes);
}
