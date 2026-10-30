/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * Owning nominal receivers bind real producer identities and emitted recipes. */
static XrXirStatus nominal_resolve(void *owner,const XrXirType *arguments,uint32_t count,XrXirType type,XrXirType *out) {
    (void)owner;(void)arguments;if(count)return XR_XIR_BAD_STRUCTURE;*out=type;return XR_XIR_OK;
}
static void nominal_call_receivers(XrXirType result,XrXirOp operation,unsigned failure) {
    XrCompileResources *resources=NULL;XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext compile={.resources=resources,.limits=xr_xir_compile_default_limits()};
    XrXirType type=(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE;
    XrXirNominalField field={.name={"value",5},.type=result};
    XrXirNominalDeclaration nominal={.module={"m",1},.name={"LogicalUnit",11},.fields=&field,.field_count=1,.kind=XR_XIR_NOMINAL_STRUCT};
    XrXirNominalTable table={.declarations=&nominal,.count=1};
    XrXirTypeNode node={.kind=XR_XIR_TYPE_NOMINAL,.nominal={.declaration=0}};
    XrXirTypes types={.nodes=&node,.count=1,.nominals=&table};
    XrXirInstruction instructions[]={{.op=operation,.type=result,.immediate=1},{.op=operation,.type=result,.immediate=1}};
    XrXirFunction functions[]={{.result=XR_XIR_UNIT,.instructions=instructions,.instruction_count=2},{.result=result}},*function_view=functions;
    XrXirFunctionIdentity identities[]={{0},{.nominal_owner=1,.method_kind=XR_XIR_MEMBER_HELPER}},*identity_view=identities;
    XrXirSourceModule module={.name="m",.name_length=1},*module_view=&module;XrXirGeneric generics[2]={{0}},*generic_view=generics;
    uint32_t function_count=2,module_count=1,origin=11,map[]={0,1};
    XrHirBodyDeclarations declarations={.functions=&function_view,.identities=&identity_view,.function_count=&function_count,.module_count=&module_count};
    XrHirBody *body=NULL;CHECK(xr_hir_body_new(resources,&types,&body)==XR_XIR_OK);
    CHECK(xr_hir_body_declarations(body,&declarations)==XR_XIR_OK&&xr_hir_body_nominal_modules(body,&module_view,&generic_view)==XR_XIR_OK);
    XrHirNominalDeclarationInput declaration={.types=&types,.source_declaration=10,.field_initializers=&origin};
    CHECK(xr_hir_body_nominal_declare(body,&declaration,NULL,NULL)==XR_XIR_OK);
    XrHirNominalFunctionSource producer={1,11};XrHirNominalFunction source={0};
    CHECK(xr_hir_body_nominal_function_source(body,&producer,NULL,&source)==XR_XIR_OK);
    XrHirNominalFunctionInput binding={.function=1,.kind=XR_HIR_NOMINAL_FIELD_DEFAULT,.source=source};
    CHECK(xr_hir_body_nominal_bind(body,&binding,NULL)==XR_XIR_OK);
    XrHirNominalShape shape={0};CHECK(xr_hir_body_nominal_receive(body,&types,type,NULL,&shape,NULL)==XR_XIR_OK);
    XrHirBodyScope scope={0};CHECK(xr_hir_body_scope(body,scope,(XrHirBodyOrigin){0},NULL,&scope)==XR_XIR_OK);
    XrHirNominalPool pool={.compile=&compile,.types=&types,.resolve=nominal_resolve};
    XrHirNominalUseInput input={.pool=&pool,.shape=shape,.scope=scope,.type=type};
    for(uint32_t at=0;at<2;++at) {
        XrHirNominalAction action={0};CHECK(xr_hir_body_nominal_helper(body,&input,NULL,&action,NULL)==XR_XIR_OK&&action.type==result);
        XrHirNominalCallEmission emission={instructions[at],at};
        CHECK(xr_hir_body_nominal_call_emitted(body,action.use,&emission)==XR_XIR_OK);
        XrHirBodyAction typed;CHECK(xr_hir_body_consume(body,action.use,&typed)==XR_XIR_OK);
        uint32_t value=failure==1?1-at:at;
        CHECK(xr_hir_body_emit(body,action.use,result,result==XR_XIR_UNIT?UINT32_MAX:value)==XR_XIR_OK);
        XrHirBodyProjection projection={.function=0,.instruction=1-at,.type=result,.operation=operation,
            .has_instruction=true,.has_call=true,.call=instructions[at],.source_value_map=map,.source_value_count=2,
            .has_value=result!=XR_XIR_UNIT,.source_value=value,.value=value};
        CHECK(xr_hir_body_project(body,action.use,&projection,NULL,NULL)==XR_XIR_BAD_STRUCTURE);
        projection.instruction=at;
        if(failure) {
            CHECK(xr_hir_body_project(body,action.use,&projection,NULL,NULL)==XR_XIR_BAD_STRUCTURE);
            xr_hir_body_end(body);xr_hir_body_free(body);xr_compile_resources_release(resources);CHECK(!physical_live&&!physical_bytes);return;
        }
        if(result==XR_XIR_UNIT) {
            projection.has_value=true;CHECK(xr_hir_body_project(body,action.use,&projection,NULL,NULL)==XR_XIR_BAD_STRUCTURE);projection.has_value=false;
        }
        CHECK(xr_hir_body_project(body,action.use,&projection,NULL,NULL)==XR_XIR_OK);
        if(result==XR_XIR_UNIT) {
            XrHirNominalPlan plan={0};XrHirNominalPlanInput planned={.shape=shape,.scope=scope,.kind=XR_HIR_NOMINAL_STRUCT_LITERAL};
            CHECK(xr_hir_body_nominal_begin(body,&planned,NULL,&plan,NULL)==XR_XIR_OK);
            XrHirNominalValue logical={.type=XR_XIR_UNIT,.value=UINT32_MAX,.producer=action.use};
            CHECK(xr_hir_body_nominal_supply(body,plan,&pool,0,logical,NULL)==XR_XIR_OK);
            CHECK(xr_hir_body_nominal_complete(body,plan,&pool,&action,NULL)==XR_XIR_OK&&action.count==1);
            /* Only the discarded result is retired. The actual Unit effect
             * call above still has its unique canonical instruction. */
            CHECK(xr_hir_body_consume(body,action.use,&typed)==XR_XIR_OK&&xr_hir_body_retire(body,action.use)==XR_XIR_OK);
        }
    }
    CHECK(xr_hir_body_finish(body,NULL)==XR_XIR_OK);xr_hir_body_end(body);xr_hir_body_free(body);xr_compile_resources_release(resources);
    CHECK(!physical_live&&!physical_bytes);
}
static void nominal_resource_receivers(void) {
    XrCompileResources *resources=NULL;XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    char long_name[5001];memset(long_name,'a',5000);long_name[5000]=0;char names[129][16];XrXirNominalField fields[130];
    fields[0]=(XrXirNominalField){{long_name,5000},XR_XIR_I64,0};
    for(uint32_t f=1;f<130;++f){int n=snprintf(names[f-1],16,"field%u",f);CHECK(n>0&&n<16);fields[f]=(XrXirNominalField){{names[f-1],(uint32_t)n},XR_XIR_I64,0};}
    XrXirNominalDeclaration nominal={.module={"m",1},.name={"Wide",4},.fields=fields,.field_count=130,.kind=XR_XIR_NOMINAL_STRUCT};
    XrXirNominalTable table={.declarations=&nominal,.count=1};XrXirTypes types={.nominals=&table};
    XrXirFunction function={0},*function_view=&function;XrXirFunctionIdentity identity={0},*identity_view=&identity;
    uint32_t one=1;XrXirSourceModule module={.name="m",.name_length=1},*module_view=&module;XrXirGeneric generic={0},*generic_view=&generic;
    XrHirBodyDeclarations declarations={.functions=&function_view,.identities=&identity_view,.function_count=&one,.module_count=&one};
    XrHirBody *body=NULL;CHECK(xr_hir_body_new(resources,&types,&body)==XR_XIR_OK);
    CHECK(xr_hir_body_declarations(body,&declarations)==XR_XIR_OK&&xr_hir_body_nominal_modules(body,&module_view,&generic_view)==XR_XIR_OK);
    XrHirNominalDeclarationInput input={.types=&types,.source_declaration=1};
    XrCompileResourceStats before={0},after={0};CHECK(xr_compile_resources_stats(resources,&before)==XR_COMPILE_RESOURCE_OK);
    size_t live=physical_live,bytes=physical_bytes;physical_fail=physical_attempts+5;
    CHECK(xr_hir_body_nominal_declare(body,&input,NULL,NULL)==XR_XIR_OUT_OF_MEMORY);physical_fail=SIZE_MAX;
    CHECK(physical_live==live&&physical_bytes==bytes&&xr_compile_resources_stats(resources,&after)==XR_COMPILE_RESOURCE_OK&&after.held_bytes==before.held_bytes);
    Cancel cancellation={0,30};XrCompileRequestOptions options={.cancel_poll=cancel,.cancel_context=&cancellation};XrCompileRequestControl control={0};
    CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_hir_body_nominal_declare(body,&input,&control,NULL)==XR_XIR_CANCELLED);
    CHECK(physical_live==live&&physical_bytes==bytes&&xr_compile_resources_stats(resources,&after)==XR_COMPILE_RESOURCE_OK&&after.held_bytes==before.held_bytes);
    CHECK(xr_hir_body_nominal_declare(body,&input,NULL,NULL)==XR_XIR_OK);
    uint32_t added=77,failed=UINT32_MAX;CHECK(xr_hir_body_nominal_schedule(body,1,2,NULL,&added,&failed)==XR_XIR_OK&&added==1);
    xr_hir_body_end(body);xr_hir_body_free(body);xr_compile_resources_release(resources);CHECK(!physical_live&&!physical_bytes);
}
static void nominal_literal_receivers(XrHirBody *body,const XrHirNominalPool *pool,XrHirNominalShape shape) {
    XrHirNominalInfo info={0};CHECK(xr_hir_body_nominal_info(body,shape,&info));XrXirType pair=info.type;
    XrHirNominalValue values[]={{.type=XR_XIR_I64,.value=0},{.type=XR_XIR_I64,.value=1}};XrHirNominalAction action={0};
    uint32_t operands[]={0,1},map[]={0,1,2,3,4,5,6};
    XrHirBodyScope scope={0};CHECK(xr_hir_body_scope(body,scope,(XrHirBodyOrigin){0},NULL,&scope)==XR_XIR_OK);
    XrHirNominalPlan plan={0};XrHirNominalPlanInput planned={.shape=shape,.scope=scope,.count=2,.kind=(XrHirNominalPlanKind)99};
    CHECK(xr_hir_body_nominal_begin(body,&planned,NULL,&plan,NULL)==XR_XIR_BAD_STRUCTURE);
    planned.kind=XR_HIR_NOMINAL_STRUCT_LITERAL;CHECK(xr_hir_body_nominal_begin(body,&planned,NULL,&plan,NULL)==XR_XIR_OK);
    CHECK(xr_hir_body_finish(body,NULL)==XR_XIR_BAD_STRUCTURE);uint32_t selected=77;
    CHECK(xr_hir_body_nominal_select(body,plan,(XrXirLiteral){"left",4},NULL,&selected,NULL)==XR_XIR_OK&&selected==0);
    CHECK(xr_hir_body_nominal_supply(body,plan,pool,selected,(XrHirNominalValue){.type=XR_XIR_BOOL},NULL)==XR_XIR_BAD_TYPE);
    CHECK(xr_hir_body_nominal_supply(body,plan,pool,selected,values[0],NULL)==XR_XIR_OK);
    CHECK(xr_hir_body_nominal_select(body,plan,(XrXirLiteral){"left",4},NULL,&selected,NULL)==XR_XIR_BAD_TYPE);
    CHECK(xr_hir_body_nominal_select(body,plan,(XrXirLiteral){"right",5},NULL,&selected,NULL)==XR_XIR_OK&&selected==1);
    CHECK(xr_hir_body_nominal_supply(body,plan,pool,selected,values[1],NULL)==XR_XIR_OK);
    CHECK(xr_hir_body_nominal_complete(body,plan,pool,&action,NULL)==XR_XIR_OK);
    XrHirBodyAction typed;CHECK(xr_hir_body_consume(body,action.use,&typed)==XR_XIR_OK&&xr_hir_body_emit(body,action.use,pair,2)==XR_XIR_OK);
    XrHirBodyProjection projection=(XrHirBodyProjection){.function=0,.instruction=0,.type=pair,.operation=XR_XIR_STRUCT_NEW,.has_instruction=true,
        .source_arguments=operands,.source_argument_count=2,.source_value_map=map,.source_value_count=7};
    CHECK(xr_hir_body_project(body,action.use,&projection,NULL,NULL)==XR_XIR_OK&&xr_hir_body_finish(body,NULL)==XR_XIR_OK);
}
static void nominal_receivers(void) {
    XrCompileResources *resources=NULL;XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats initial={0},before={0},after={0};CHECK(xr_compile_resources_stats(resources,&initial)==XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext compile={.resources=resources,.limits=xr_xir_compile_default_limits()};
    XrXirType pair=(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE;
    XrXirNominalField fields[]={{{"left",4},XR_XIR_I64,0},{{"right",5},XR_XIR_I64,0}};
    XrXirNominalDeclaration nominal={.module={"m",1},.name={"Pair",4},.fields=fields,.field_count=2,.kind=XR_XIR_NOMINAL_STRUCT};
    XrXirNominalTable table={.declarations=&nominal,.count=1};
    XrXirTypeNode node={.kind=XR_XIR_TYPE_NOMINAL,.nominal={.declaration=0}};
    XrXirTypes types={.nodes=&node,.count=1,.nominals=&table},foreign=types;
    uint32_t origins[]={11,12},operands[]={0,1},map[]={0,1,2,3,4,5,6};XrXirType parameters[]={XR_XIR_I64,XR_XIR_I64};
    XrXirInstruction instructions[]={
        {.op=XR_XIR_STRUCT_NEW,.type=pair,.args={0,2}},
        {.op=XR_XIR_STRUCT_GET,.type=XR_XIR_I64,.args={2,0}},
        {.op=XR_XIR_CALL,.type=XR_XIR_I64,.immediate=1},
        {.op=XR_XIR_INVOKE,.type=XR_XIR_I64,.immediate=2},
        {.op=XR_XIR_INVOKE_RESULT,.type=XR_XIR_I64,.immediate=3}};
    XrXirFunction functions[]={
        {.parameters=parameters,.parameter_count=2,.instructions=instructions,.instruction_count=5,.operands=operands,.operand_count=2},
        {.result=XR_XIR_I64},{.result=XR_XIR_I64},{.result=pair},{.result=pair}};
    XrXirFunctionIdentity identities[]={{0},{.nominal_owner=1,.method_kind=XR_XIR_MEMBER_HELPER},
        {.nominal_owner=1,.method_kind=XR_XIR_MEMBER_HELPER},{.nominal_owner=1,.method_kind=XR_XIR_CONSTRUCTOR},
        {.nominal_owner=1,.method_kind=XR_XIR_CONSTRUCTOR}};
    XrXirFunction *function_view=functions;XrXirFunctionIdentity *identity_view=identities;
    XrXirSourceModule module={.name="m",.name_length=1},*module_view=&module;XrXirGeneric generics[5]={{0}},*generic_view=generics;
    uint32_t function_count=5,module_count=1;XrHirBodyDeclarations declarations={.functions=&function_view,.identities=&identity_view,
        .function_count=&function_count,.module_count=&module_count};XrHirBody *body=NULL,*other=NULL;
    CHECK(xr_hir_body_new(resources,&types,&body)==XR_XIR_OK&&xr_hir_body_new(resources,&types,&other)==XR_XIR_OK);
    CHECK(xr_hir_body_nominal_modules(body,&module_view,&generic_view)==XR_XIR_BAD_STRUCTURE);
    CHECK(xr_hir_body_declarations(body,&declarations)==XR_XIR_OK&&xr_hir_body_declarations(other,&declarations)==XR_XIR_OK);
    CHECK(xr_hir_body_nominal_modules(body,&module_view,&generic_view)==XR_XIR_OK&&xr_hir_body_nominal_modules(other,&module_view,&generic_view)==XR_XIR_OK);
    XrHirNominalDeclarationInput declaration={.types=&types,.declaration=0,.source_declaration=10,.field_initializers=origins};
    declaration.types=&foreign;CHECK(xr_hir_body_nominal_declare(body,&declaration,NULL,NULL)==XR_XIR_BAD_STRUCTURE);declaration.types=&types;
    CHECK(xr_hir_body_nominal_declare(body,&declaration,NULL,NULL)==XR_XIR_OK);
    CHECK(xr_hir_body_nominal_declare(body,&declaration,NULL,NULL)==XR_XIR_BAD_STRUCTURE);
    origins[0]=99;uint32_t added=77,failed=UINT32_MAX;
    CHECK(xr_hir_body_nominal_schedule(body,5,6,NULL,&added,&failed)==XR_XIR_OK&&added==1);
    XrHirNominalFunctionInput binding={.declaration=0,.function=3,.field=UINT32_MAX,.kind=(XrHirNominalFunctionKind)99};
    CHECK(xr_hir_body_nominal_bind(body,&binding,NULL)==XR_XIR_BAD_STRUCTURE);
    binding.kind=XR_HIR_NOMINAL_EXPLICIT;CHECK(xr_hir_body_nominal_bind(body,&binding,NULL)==XR_XIR_BAD_STRUCTURE);
    binding.kind=XR_HIR_NOMINAL_IMPLICIT;CHECK(xr_hir_body_nominal_bind(body,&binding,NULL)==XR_XIR_OK);
    XrHirNominalFunctionSource producer={1,11};XrHirNominalFunction first={0},second={0},sentinel={other,77},handle=sentinel;
    producer.declaration=99;CHECK(xr_hir_body_nominal_function_source(body,&producer,NULL,&handle)==XR_XIR_BAD_STRUCTURE&&handle.owner==other&&handle.index==77);
    producer.declaration=11;CHECK(xr_hir_body_nominal_function_source(body,&producer,NULL,&first)==XR_XIR_OK);
    producer.function=2;CHECK(xr_hir_body_nominal_function_source(body,&producer,NULL,&handle)==XR_XIR_BAD_STRUCTURE);
    producer.declaration=12;CHECK(xr_hir_body_nominal_function_source(body,&producer,NULL,&second)==XR_XIR_OK);
    binding=(XrHirNominalFunctionInput){.declaration=0,.function=2,.field=0,.kind=XR_HIR_NOMINAL_FIELD_DEFAULT,.source=first};
    CHECK(xr_hir_body_nominal_bind(body,&binding,NULL)==XR_XIR_BAD_STRUCTURE);
    binding.function=1;binding.field=1;CHECK(xr_hir_body_nominal_bind(body,&binding,NULL)==XR_XIR_BAD_STRUCTURE);
    binding.field=0;CHECK(xr_hir_body_nominal_bind(body,&binding,NULL)==XR_XIR_OK);
    binding.function=2;binding.field=1;binding.source=second;CHECK(xr_hir_body_nominal_bind(body,&binding,NULL)==XR_XIR_OK);
    XrHirNominalDefaults defaults={0};CHECK(xr_hir_body_nominal_defaults(body,0,&defaults)&&defaults.implicit_initializer==3&&defaults.fields[0]==1&&defaults.fields[1]==2);
    XrHirNominalShape shape={0},shape_out={other,77};
    CHECK(xr_hir_body_nominal_receive(body,&foreign,pair,NULL,&shape_out,NULL)==XR_XIR_BAD_STRUCTURE&&shape_out.owner==other&&shape_out.index==77);
    CHECK(xr_hir_body_nominal_receive(body,&types,pair,NULL,&shape,NULL)==XR_XIR_OK);
    XrHirNominalInfo info={0};CHECK(xr_hir_body_nominal_info(body,shape,&info)&&info.count==2);
    fields[0].name=(XrXirLiteral){"poison",6};CHECK(info.fields[0].name.length==4&&!memcmp(info.fields[0].name.bytes,"left",4));fields[0].name=(XrXirLiteral){"left",4};
    XrHirNominalPool pool={.compile=&compile,.types=&types,.resolve=nominal_resolve};XrHirBodyScope scope={0};
    CHECK(xr_hir_body_scope(body,scope,(XrHirBodyOrigin){0},NULL,&scope)==XR_XIR_OK);
    XrHirNominalValue values[]={{.type=XR_XIR_I64,.value=0},{.type=XR_XIR_I64,.value=1}};
    XrHirNominalUseInput input={.pool=&pool,.shape=shape,.scope=scope,.type=pair,.values=values,.count=2};XrHirNominalAction action={0};
    pool.types=&foreign;CHECK(xr_hir_body_nominal_construct(body,&input,NULL,&action,NULL)==XR_XIR_BAD_STRUCTURE);pool.types=&types;
    input.count=1;CHECK(xr_hir_body_nominal_construct(body,&input,NULL,&action,NULL)==XR_XIR_BAD_TYPE);input.count=2;
    values[0].type=XR_XIR_BOOL;CHECK(xr_hir_body_nominal_construct(body,&input,NULL,&action,NULL)==XR_XIR_BAD_TYPE);values[0].type=XR_XIR_I64;
    CHECK(xr_hir_body_nominal_construct(body,&input,NULL,&action,NULL)==XR_XIR_OK);
    XrHirBodyAction typed;CHECK(xr_hir_body_consume(body,action.use,&typed)==XR_XIR_OK&&xr_hir_body_emit(body,action.use,pair,2)==XR_XIR_OK);
    XrHirBodyProjection projection={.function=0,.instruction=0,.type=pair,.operation=XR_XIR_STRUCT_NEW,.has_instruction=true,
        .source_arguments=operands,.source_argument_count=2,.source_value_map=map,.source_value_count=7};
    operands[0]=1;CHECK(xr_hir_body_project(body,action.use,&projection,NULL,NULL)==XR_XIR_BAD_STRUCTURE);operands[0]=0;
    CHECK(xr_hir_body_project(body,action.use,&projection,NULL,NULL)==XR_XIR_OK);
    input.receiver=2;input.field=0;CHECK(xr_hir_body_nominal_get(body,&input,NULL,&action,NULL)==XR_XIR_OK);
    CHECK(xr_hir_body_consume(body,action.use,&typed)==XR_XIR_OK);
    projection=(XrHirBodyProjection){.function=0,.instruction=1,.type=XR_XIR_I64,.operation=XR_XIR_STRUCT_GET,
        .has_instruction=true,.source_operands={2,0},.source_value_map=map,.source_value_count=7};
    instructions[1].immediate=1;CHECK(xr_hir_body_project(body,action.use,&projection,NULL,NULL)==XR_XIR_BAD_STRUCTURE);instructions[1].immediate=0;
    CHECK(xr_hir_body_project(body,action.use,&projection,NULL,NULL)==XR_XIR_OK);
    CHECK(xr_hir_body_nominal_helper(body,&input,NULL,&action,NULL)==XR_XIR_OK&&action.function==1);
    XrHirNominalCallEmission emitted={instructions[2],4};
    CHECK(xr_hir_body_nominal_call_emitted(body,action.use,&emitted)==XR_XIR_OK);
    CHECK(xr_hir_body_consume(body,action.use,&typed)==XR_XIR_OK&&xr_hir_body_emit(body,action.use,XR_XIR_I64,4)==XR_XIR_OK);
    projection=(XrHirBodyProjection){.function=0,.instruction=2,.type=XR_XIR_I64,.operation=XR_XIR_CALL,.has_instruction=true,
        .call=instructions[2],.has_call=true,.has_value=true,.value=4,.source_value=4,.source_value_map=map,.source_value_count=7};
    instructions[2].immediate=2;CHECK(xr_hir_body_project(body,action.use,&projection,NULL,NULL)==XR_XIR_BAD_STRUCTURE);instructions[2].immediate=1;
    CHECK(xr_hir_body_project(body,action.use,&projection,NULL,NULL)==XR_XIR_OK);
    input.field=1;CHECK(xr_hir_body_nominal_helper(body,&input,NULL,&action,NULL)==XR_XIR_OK&&action.function==2);
    emitted=(XrHirNominalCallEmission){instructions[3],5};CHECK(xr_hir_body_nominal_call_emitted(body,action.use,&emitted)==XR_XIR_OK);
    CHECK(xr_hir_body_consume(body,action.use,&typed)==XR_XIR_OK&&xr_hir_body_emit(body,action.use,XR_XIR_I64,6)==XR_XIR_OK);
    projection=(XrHirBodyProjection){.function=0,.instruction=4,.type=XR_XIR_I64,.operation=XR_XIR_INVOKE_RESULT,.has_instruction=true,
        .call=instructions[3],.has_call=true,.has_value=true,.value=6,.source_value=6,.source_value_map=map,.source_value_count=7};
    XrXirInstruction saved=instructions[2];instructions[2]=instructions[3];instructions[4].immediate=2;
    CHECK(xr_hir_body_project(body,action.use,&projection,NULL,NULL)==XR_XIR_BAD_STRUCTURE);instructions[4].immediate=3;instructions[2]=saved;
    projection.value=4;CHECK(xr_hir_body_project(body,action.use,&projection,NULL,NULL)==XR_XIR_BAD_STRUCTURE);projection.value=6;
    CHECK(xr_hir_body_project(body,action.use,&projection,NULL,NULL)==XR_XIR_OK&&xr_hir_body_finish(body,NULL)==XR_XIR_OK);
    CHECK(xr_hir_body_nominal_helper(body,&input,NULL,&action,NULL)==XR_XIR_BAD_STRUCTURE);
    XrXirDeclarations checked_declarations={.modules=&module,.module_count=1};XrXirModule definition={.stage=XR_XIR_CHECKED,.types=&types,.declarations=&checked_declarations};
    XrXirTypeNode wrong=node;wrong.kind=XR_XIR_TYPE_ARRAY;XrXirTypes unrelated=types;unrelated.nodes=&wrong;definition.types=&unrelated;
    CHECK(xr_hir_body_nominal_checked(other,&definition,NULL)==XR_XIR_BAD_STRUCTURE);definition.types=&types;
    CHECK(xr_hir_body_nominal_checked(other,&definition,NULL)==XR_XIR_OK);
    CHECK(!xr_hir_body_nominal_defaults(other,0,&defaults)&&!xr_hir_body_nominal_defaultable(other,pair));
    CHECK(xr_hir_body_nominal_schedule(other,5,6,NULL,&added,&failed)==XR_XIR_BAD_STRUCTURE&&xr_hir_body_nominal_bind(other,&binding,NULL)==XR_XIR_BAD_STRUCTURE);
    CHECK(xr_compile_resources_stats(resources,&before)==XR_COMPILE_RESOURCE_OK);size_t live=physical_live,bytes=physical_bytes;
    physical_fail=physical_attempts;CHECK(xr_hir_body_nominal_receive(other,&types,pair,NULL,&shape_out,NULL)==XR_XIR_OUT_OF_MEMORY);physical_fail=SIZE_MAX;
    CHECK(shape_out.owner==other&&shape_out.index==77&&physical_live==live&&physical_bytes==bytes);
    Cancel cancellation={0,4};XrCompileRequestOptions options={.cancel_poll=cancel,.cancel_context=&cancellation};XrCompileRequestControl control={0};
    CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_hir_body_nominal_receive(other,&types,pair,&control,&shape_out,NULL)==XR_XIR_CANCELLED);
    CHECK(physical_live==live&&physical_bytes==bytes&&xr_compile_resources_stats(resources,&after)==XR_COMPILE_RESOURCE_OK&&after.held_bytes==before.held_bytes);
    CHECK(xr_hir_body_nominal_receive(other,&types,pair,NULL,&shape_out,NULL)==XR_XIR_OK);
    nominal_literal_receivers(other,&pool,shape_out);
    xr_hir_body_end(body);CHECK(!xr_hir_body_nominal_info(body,shape,&info));xr_hir_body_free(body);xr_hir_body_end(other);xr_hir_body_free(other);
    CHECK(xr_compile_resources_stats(resources,&after)==XR_COMPILE_RESOURCE_OK&&after.held_bytes==initial.held_bytes);
    xr_compile_resources_release(resources);CHECK(!physical_live&&!physical_bytes);
    nominal_call_receivers(XR_XIR_UNIT,XR_XIR_INVOKE,0);nominal_call_receivers(XR_XIR_I64,XR_XIR_CALL,0);
    nominal_call_receivers(XR_XIR_I64,XR_XIR_CALL,1);nominal_call_receivers(XR_XIR_I64,XR_XIR_INVOKE,2);nominal_resource_receivers();
}
