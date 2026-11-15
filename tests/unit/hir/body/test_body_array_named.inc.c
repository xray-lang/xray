/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * Reordered native arguments retain their real declaration and producer owners. */
static void array_named_binding_receivers(void) {
    XrCompileResources *resources=NULL;XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrXirTypeNode node={.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64};XrXirTypes types={.nodes=&node,.count=1};
    XrHirBody *body=NULL;CHECK(xr_hir_body_new(resources,&types,&body)==XR_XIR_OK);
    XrHirBodyScope scope={0};CHECK(xr_hir_body_scope(body,scope,(XrHirBodyOrigin){0},NULL,&scope)==XR_XIR_OK);
    const XrNativeTypeDeclaration *owner=xr_native_declaration_by_id(XR_NATIVE_DECLARATION_ARRAY);
    const XrNativeMemberDeclaration *member=xr_native_declaration_member(owner,"reduce");CHECK(member);
    XrXirType parameters[]={XR_XIR_UNIT,XR_XIR_UNIT};
    XrHirSignatureInput input={.types=&types,.scope=scope,.family=XR_HIR_CALL_NATIVE_CALLBACK,.count=2,
        .native_parameters=parameters,.collection_type=(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE,
        .collection_value=99,.native_member=member,.callback_error=XR_XIR_NEVER};
    XrHirBodySignature signature=binding_signature(body,input);XrHirCallBindingError error={0};
    XrHirNativeCallbackShape shape={0};
    CHECK(xr_hir_body_callback_context(body,signature,input.collection_type,99,&shape)==XR_XIR_BAD_STRUCTURE);
    CHECK(xr_hir_body_array_accumulator(body,signature,XR_XIR_UNIT,NULL)==XR_XIR_BAD_TYPE);
    CHECK(xr_hir_body_array_accumulator(body,signature,(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+1),NULL)==XR_XIR_BAD_TYPE);
    size_t live=physical_live,bytes=physical_bytes;
    for(size_t point=0;point<2;++point) {
        physical_fail=physical_attempts+point;
        CHECK(xr_hir_body_call_bind_open(body,signature,NULL,&error)==XR_XIR_OUT_OF_MEMORY);physical_fail=SIZE_MAX;
        CHECK(physical_live==live&&physical_bytes==bytes);
    }
    Cancel cancellation={0,SIZE_MAX};XrCompileRequestOptions options={.cancel_poll=cancel,.cancel_context=&cancellation};
    XrCompileRequestControl control={0};CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_hir_body_call_bind_open(body,signature,&control,&error)==XR_XIR_OK);
    size_t polls=cancellation.polls;CHECK(polls>0);
    for(size_t point=1;point<=polls;++point) {
        signature=binding_signature(body,input);live=physical_live;bytes=physical_bytes;
        cancellation=(Cancel){0,point};control=(XrCompileRequestControl){0};
        CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
        CHECK(xr_hir_body_call_bind_open(body,signature,&control,&error)==XR_XIR_CANCELLED);
        CHECK(physical_live==live&&physical_bytes==bytes);
        CHECK(xr_hir_body_call_bind_open(body,signature,NULL,&error)==XR_XIR_OK);
    }
    uint32_t formal=UINT32_MAX;
    CHECK(xr_hir_body_call_bind_argument(body,signature,"initial",7,1,false,NULL,&formal,&error)==XR_XIR_OK&&formal==1);
    CHECK(xr_hir_body_call_bind_argument(body,signature,"callback",8,1,false,NULL,&formal,&error)==XR_XIR_OK&&formal==0);
    XrHirCallBinding binding={0};CHECK(xr_hir_body_call_bind_seal(body,signature,NULL,&binding,&error)==XR_XIR_OK);
    CHECK(binding.argument_formals[0]==1&&binding.argument_formals[1]==0);
    cancellation=(Cancel){0,1};control=(XrCompileRequestControl){0};
    CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_hir_body_array_accumulator(body,signature,XR_XIR_I64,&control)==XR_XIR_CANCELLED);
    CHECK(xr_hir_body_array_accumulator(body,signature,XR_XIR_I64,NULL)==XR_XIR_OK);
    CHECK(xr_hir_body_array_accumulator(body,signature,XR_XIR_STRING,NULL)==XR_XIR_BAD_STRUCTURE);
    CHECK(xr_hir_body_callback_context(body,signature,input.collection_type,99,&shape)==XR_XIR_OK&&
        shape.parameters[0]==XR_XIR_I64&&shape.result==XR_XIR_I64);
    xr_hir_body_free(body);xr_compile_resources_release(resources);CHECK(!physical_live&&!physical_bytes);
}
static void array_named_receivers(void) {
    array_named_binding_receivers();
    XrCompileResources *resources=NULL;XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext compile={.resources=resources,.limits=xr_xir_compile_default_limits()};
    const char original[]="fn initial()->i64{return 10}\nfn addition()->fn(i64,i64)->i64{return (a,b)=>a+b}\n"
        "fn main(){const xs=[1,2,3];print(xs.reduce(initial:initial(),callback:addition()));"
        "const ys=xs.map(callback:(item,index)=>item+index);print(ys[0])}\nmain()\n";
    char source[sizeof(original)];memcpy(source,original,sizeof(source));
    XrXirSourceResult result={0};size_t first=physical_attempts;
    CHECK(collection_source_receive(&compile,source,NULL,&result)==XR_XIR_OK);
    size_t sites=physical_attempts-first;CHECK(sites>0);memset(source,0xa5,sizeof(source));
    CHECK(xr_xir_compile_artifact_verify(result.checked,NULL)==XR_XIR_OK);
    collection_source_retire(&result);size_t live=physical_live,bytes=physical_bytes;
    memcpy(source,original,sizeof(source));
    for(size_t point=0;point<sites;++point) {
        physical_fail=physical_attempts+point;
        XrXirStatus status=collection_source_receive(&compile,source,NULL,&result);physical_fail=SIZE_MAX;
        if(status!=XR_XIR_OUT_OF_MEMORY)fprintf(stderr,"Array named fault %zu/%zu status %u\n",point,sites,(unsigned)status);
        CHECK(status==XR_XIR_OUT_OF_MEMORY&&!result.checked&&!result.snapshot);
        CHECK(physical_live==live&&physical_bytes==bytes);
    }
    Cancel cancellation={0,SIZE_MAX};XrCompileRequestOptions options={.cancel_poll=cancel,.cancel_context=&cancellation};
    XrCompileRequestControl control={0};CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
    CHECK(collection_source_receive(&compile,source,&control,&result)==XR_XIR_OK);
    size_t polls=cancellation.polls;collection_source_retire(&result);CHECK(polls>0);
    for(size_t point=1;point<=polls;++point) {
        cancellation=(Cancel){0,point};control=(XrCompileRequestControl){0};
        CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
        CHECK(collection_source_receive(&compile,source,&control,&result)==XR_XIR_CANCELLED&&!result.checked&&!result.snapshot);
        CHECK(physical_live==live&&physical_bytes==bytes);
    }
    xr_compile_resources_release(resources);CHECK(!physical_live&&!physical_bytes);
}
