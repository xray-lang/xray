/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * Function declaration labels survive capture, but values never acquire them. */
static XrHirBodyUse declared_test_initialize(XrHirBody *body,const XrXirTypes *types,
    XrHirBodyBinding binding,XrHirBindingUseKind kind,uint32_t value) {
    XrHirBindingInput declaration={0};CHECK(xr_hir_body_binding_info(body,binding,&declaration));
    XrHirBindingUseInput input={.types=types,.scope=declaration.scope,.origin=declaration.origin,
        .kind=kind,.value=declaration.physical,.value_id=value};
    XrHirBindingAction action={0};XrHirBodyAction consumed={0};
    CHECK(xr_hir_body_binding_use(body,binding,&input,NULL,&action,NULL)==XR_XIR_OK);
    CHECK(xr_hir_body_consume(body,action.use,&consumed)==XR_XIR_OK);
    CHECK(xr_hir_body_emit(body,action.use,declaration.physical,value)==XR_XIR_OK);
    return action.use;
}
static void declared_capture_projection_receivers(void) {
    XrCompileResources *resources=NULL;XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrHirItems *items=NULL;CHECK(xr_hir_items_new(resources,&items)==XR_COMPILE_RESOURCE_OK);
    const uint32_t item_nodes[]={10,11,20};
    for(uint32_t n=0;n<3;++n) {
        uint32_t item=0;XrHirItemSpec spec={"$closure",0,0,item_nodes[n],1,1,XR_HIR_ITEM_NAMED_FUNCTION_SCOPE};
        CHECK(xr_hir_items_reserve(items,&spec,&item)==XR_COMPILE_RESOURCE_OK);
    }
    XrXirCallableParameter parameter={XR_XIR_I64,XR_PARAM_READ,0};
    XrXirTypeNode node={.kind=XR_XIR_TYPE_CALLABLE,.element=XR_XIR_NEVER,.parameters=&parameter,
        .parameter_count=1,.result=XR_XIR_I64};
    XrXirTypes types={.nodes=&node,.count=1};XrXirType callable=XR_XIR_CONSTRUCTED_TYPE_BASE;
    XrXirType ordinary[]={XR_XIR_I64},captured[]={callable,callable,XR_XIR_I64};
    XrXirFunction functions[4]={{.result=XR_XIR_UNIT,.error_type=XR_XIR_NEVER},
        {.parameters=captured,.parameter_count=3,.result=XR_XIR_I64,.error_type=XR_XIR_NEVER},
        {.parameters=ordinary,.parameter_count=1,.result=XR_XIR_I64,.error_type=XR_XIR_NEVER},
        {.parameters=ordinary,.parameter_count=1,.result=XR_XIR_I64,.error_type=XR_XIR_NEVER}};
    XrXirFunctionIdentity identities[4]={{0},{.capture_owner=1,.capture_count=2},{.capture_owner=1},{.capture_owner=1}};
    XrXirFunction *function_view=functions;XrXirFunctionIdentity *identity_view=identities;
    uint32_t count=4,modules=1;
    XrHirBodyDeclarations declarations={.functions=&function_view,.identities=&identity_view,
        .function_count=&count,.module_count=&modules,.items=&items};
    XrHirBody *body=NULL,*foreign=NULL;
    CHECK(xr_hir_body_new(resources,&types,&body)==XR_XIR_OK&&xr_hir_body_new(resources,&types,&foreign)==XR_XIR_OK);
    CHECK(xr_hir_body_declarations(body,&declarations)==XR_XIR_OK);
    XrHirBodyScope outer={0},receiver={0};
    CHECK(xr_hir_body_scope(body,outer,(XrHirBodyOrigin){.function=0,.node=1},NULL,&outer)==XR_XIR_OK);
    CHECK(xr_hir_body_scope(body,receiver,(XrHirBodyOrigin){.function=1,.node=20},NULL,&receiver)==XR_XIR_OK);
    XrHirBodyBinding sources[2]={{0},{0}};
    for(uint32_t p=0;p<2;++p) {
        XrHirBindingInput input={.types=&types,.key={0,10+p,10+p,1,0,UINT32_MAX},.scope=outer,
            .origin={.function=0,.node=10+p,.binding=10+p},.name=p?"second":"first",.name_length=p?6:5,
            .logical=callable,.physical=callable,.storage=XR_HIR_BODY_LOCAL,.mode=XR_PARAM_READ};
        CHECK(xr_hir_body_binding_declare(body,&input,NULL,&sources[p],NULL)==XR_XIR_OK);
        XrHirBodyUse use=declared_test_initialize(body,&types,sources[p],XR_HIR_BINDING_INITIALIZE,p);
        CHECK(xr_hir_body_binding_function(body,sources[p],use,p+2,NULL)==XR_XIR_OK);
        XrHirBodyProjection projection={.function=0,.instruction=p,.type=callable,.operation=XR_XIR_FUNCTION_REF,
            .immediate=p+2,.value=p,.source_value=p,.has_instruction=true,.has_value=true};
        CHECK(xr_hir_body_project(body,use,&projection,NULL,NULL)==XR_XIR_OK);
        XrHirBindingInput target=input;target.key.node=20;target.key.function=1;target.scope=receiver;
        target.origin.function=1;target.origin.node=20;target.location=p;
        XrHirBodyBinding capture={0};CHECK(xr_hir_body_binding_declare(body,&target,NULL,&capture,NULL)==XR_XIR_OK);
        use=declared_test_initialize(body,&types,capture,XR_HIR_BINDING_CAPTURE,p);
        CHECK(xr_hir_body_binding_capture_function(body,capture,(XrHirBodyBinding){foreign,0},use,NULL)==XR_XIR_BAD_STRUCTURE);
        CHECK(xr_hir_body_binding_capture_function(body,capture,sources[p],use,NULL)==XR_XIR_OK);
        projection=(XrHirBodyProjection){.function=1,.type=callable,.value=p,.source_value=p,.has_value=true};
        CHECK(xr_hir_body_project(body,use,&projection,NULL,NULL)==XR_XIR_OK);
    }
    XrHirBindingInput input={.types=&types,.key={0,20,20,1,0,UINT32_MAX},.scope=outer,
        .origin={.function=0,.node=20,.binding=20},.name="relay",.name_length=5,
        .logical=callable,.physical=callable,.storage=XR_HIR_BODY_LOCAL,.mode=XR_PARAM_READ};
    XrHirBodyBinding binding={0};CHECK(xr_hir_body_binding_declare(body,&input,NULL,&binding,NULL)==XR_XIR_OK);
    XrHirBodyUse use=declared_test_initialize(body,&types,binding,XR_HIR_BINDING_INITIALIZE,2);
    CHECK(xr_hir_body_binding_function(body,binding,use,1,NULL)==XR_XIR_OK);
    uint32_t captures[]={1,0};
    XrHirBodyProjection projection={.function=0,.instruction=2,.type=callable,.operation=XR_XIR_FUNCTION_REF,
        .immediate=1,.value=2,.source_value=2,.has_instruction=true,.has_value=true,
        .source_arguments=captures,.source_argument_count=2};
    CHECK(xr_hir_body_project(body,use,&projection,NULL,NULL)==XR_XIR_BAD_STRUCTURE);
    captures[0]=0;captures[1]=1;
    CHECK(xr_hir_body_project(body,use,&projection,NULL,NULL)==XR_XIR_OK);
    CHECK(xr_hir_body_finish(body,NULL)==XR_XIR_OK);
    xr_hir_body_free(foreign);xr_hir_body_free(body);xr_hir_items_free(items);
    xr_compile_resources_release(resources);CHECK(!physical_live&&!physical_bytes);
}

static void hir_scope_index_controls(void) {
    XrCompileResources *resources=NULL;
    XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrHirItems *items=NULL;
    CHECK(xr_hir_items_new(resources,&items)==XR_COMPILE_RESOURCE_OK);
    uint32_t parents[3];
    for(uint32_t module=0;module<3;++module) {
        XrHirItemSpec parent={"owner",module,0,1,1,1,XR_HIR_ITEM_DECLARATION};
        CHECK(xr_hir_items_reserve(items,&parent,&parents[module])==XR_COMPILE_RESOURCE_OK);
    }
    const XrHirItemKind kinds[]={XR_HIR_ITEM_NAMED_FUNCTION_SCOPE,XR_HIR_ITEM_PARAMETER_SCOPE,XR_HIR_ITEM_METHOD};
    uint32_t expected[48];
    for(uint32_t i=0;i<48;++i) {
        uint32_t module=i%3,node=100+i/3;
        XrHirItemKind kind=kinds[(i/3)%3];
        char name[24];
        CHECK(snprintf(name,sizeof(name),"scope_%u",i)>0);
        XrHirItemSpec spec={name,module,kind==XR_HIR_ITEM_METHOD?parents[module]+1:0,node,1,1,kind};
        if(i==4) {
            expected[i]=UINT32_MAX;
            physical_fail=physical_attempts;
            CHECK(xr_hir_items_reserve(items,&spec,&expected[i])==XR_COMPILE_RESOURCE_OUT_OF_MEMORY);
            physical_fail=SIZE_MAX;
            CHECK(expected[i]==UINT32_MAX&&!xr_hir_items_find_scope(items,module,node,&expected[i]));
            CHECK(expected[i]==UINT32_MAX&&xr_hir_items_named_function(items,0,100));
        }
        CHECK(xr_hir_items_reserve(items,&spec,&expected[i])==XR_COMPILE_RESOURCE_OK);
    }
    for(uint32_t i=0;i<48;++i) {
        uint32_t found=UINT32_MAX;
        CHECK(xr_hir_items_find_scope(items,i%3,100+i/3,&found)&&found==expected[i]);
    }
    XrHirItemSpec ordinary={"ordinary",0,0,100,1,1,XR_HIR_ITEM_DECLARATION};
    uint32_t found=UINT32_MAX;
    CHECK(xr_hir_items_reserve(items,&ordinary,&found)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_hir_items_find_scope(items,0,100,&found)&&found==expected[0]);
    XrHirItemSpec duplicate={"duplicate_scope",1,0,100,1,1,XR_HIR_ITEM_PARAMETER_SCOPE};
    CHECK(xr_hir_items_reserve(items,&duplicate,&found)==XR_COMPILE_RESOURCE_OK);
    found=UINT32_MAX;
    CHECK(!xr_hir_items_find_scope(items,1,100,&found)&&found==UINT32_MAX);
    CHECK(!xr_hir_items_named_function(items,1,100));
    CHECK(xr_hir_items_find_scope(items,2,100,&found)&&found==expected[2]);
    CHECK(!xr_hir_items_find_scope(items,3,100,&found)&&found==expected[2]);
    xr_hir_items_free(items);
    xr_compile_resources_release(resources);
    CHECK(!physical_live&&!physical_bytes);
}

static void declared_closure_receivers(void) {
    hir_scope_index_controls();
    declared_capture_projection_receivers();
    native_named_source_case("fn outer()->i64{const base=7;fn add(a:i64,b:i64=2)->i64{return base+a*10+b};return add(a:3)}\nprint(outer())\n");
    native_named_source_case("fn outer()->i64{const base=7;fn add(a:i64,b:i64)->i64{return base+a*10+b};return add(b:2,a:3)}\nprint(outer())\n");
    const char *positive[]={
        "fn outer()->i64{const base=7;fn add(a:i64,b:i64)->i64{return base+a*10+b};const relay=()=>add(b:2,a:3);return relay()};print(outer())",
        "fn outer(){const base=7;fn values(n:i64=2)->Iterator<i64>{yield base+n};const values_now=values()};outer()",
        "fn outer<T>(value:T)->T{fn choose(a:T,b:i64=2)->T{return a};return choose(a:value)};print(outer(3))",
        "fn outer()->i64{const base=7;fn add(n:i64,step:i64=1)->i64{if(n==0){return base};return add(n:n-step)};return add(n:3)};print(outer())",
        "fn outer()->i64{const base=7;fn add(a:i64,b:i64=2)->i64{return base+a*10+b};fn relay()->i64{return add(a:3)};return relay()};print(outer())",
        "fn outer()->i64{const base=7;fn add(a:i64,...tail:i64)->i64{return base+a+len(tail)};return add(a:3)};print(outer())",
        "fn outer()->i64{fn add(a:i64,b:i64)->i64{return a*10+b};return add(b:2,a:3)};print(outer())",
        "fn outer()->i64{const base=7;fn add(a:i64,b:i64)->i64{return base+a*10+b};fn relay()->i64{return add(b:2,a:3)};return relay()};print(outer())",
        "fn outer()->i64{const base=7;fn add(n:i64)->i64{if(n==0){return base};return add(n:n-1)};return add(n:3)};print(outer())",
        "fn outer()->i64{const base=7;fn add(a:i64,b:i64)->i64{return base+a*10+b};fn relay()->i64{fn leaf()->i64{return add(b:2,a:3)};return leaf()};return relay()};print(outer())"
    };
    const char *negative[]={
        "fn outer()->i64{const base=7;fn add(a:i64,b:i64)->i64{return base+a+b};const alias=add;return alias(a:1,b:2)};print(outer())",
        "fn outer()->i64{const base=7;fn add(a:i64,b:i64)->i64{return base+a+b};fn relay()->i64{const alias=add;return alias(a:1,b:2)};return relay()};print(outer())",
        "fn apply(callback:fn(i64)->i64)->i64{return callback(a:1)};print(apply((a:i64)=>a))",
        "fn outer()->i64{const base=7;const add=(a:i64,b:i64)=>base+a+b;return add(a:1,b:2)};print(outer())",
        "fn outer()->i64{const base=7;fn add(a:i64,b:i64)->i64{return base+a+b};return add(other:1,b:2)};print(outer())",
        "fn outer()->i64{const base=7;fn add(a:i64,b:i64)->i64{return base+a+b};return add(a:1,a:2)};print(outer())"
    };
    XrCompileResources *resources=NULL;XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext compile={.resources=resources,.limits=xr_xir_compile_default_limits()};
    for(size_t i=0;i<sizeof(positive)/sizeof(positive[0]);++i) {
        XrXirSourceResult result={0};
        CHECK(collection_source_receive(&compile,positive[i],NULL,&result)==XR_XIR_OK);
        CHECK(xr_xir_compile_artifact_verify(result.checked,NULL)==XR_XIR_OK);collection_source_retire(&result);
    }
    for(size_t i=0;i<sizeof(negative)/sizeof(negative[0]);++i) {
        XrXirSourceResult result={0};
        CHECK(collection_source_receive(&compile,negative[i],NULL,&result)!=XR_XIR_OK);
        CHECK(!result.checked&&!result.snapshot);collection_source_retire(&result);
    }
    xr_compile_resources_release(resources);CHECK(!physical_live&&!physical_bytes);
}
