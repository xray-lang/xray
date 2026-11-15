/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * Native optional holes and final operands share one declaration-owned map. */
static void native_named_binding_receivers(void) {
    XrCompileResources *resources=NULL;XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrXirTypes types={0};XrHirBody *body=NULL;CHECK(xr_hir_body_new(resources,&types,&body)==XR_XIR_OK);
    XrHirBodyScope scope={0};CHECK(xr_hir_body_scope(body,scope,(XrHirBodyOrigin){0},NULL,&scope)==XR_XIR_OK);
    const XrNativeTypeDeclaration *owner=xr_native_declaration_by_id(XR_NATIVE_DECLARATION_ARRAY);
    const XrNativeMemberDeclaration *member=xr_native_declaration_member(owner,"fill");CHECK(member);
    XrXirType parameters[]={XR_XIR_I64,XR_XIR_I64,XR_XIR_I64};
    XrHirSignatureInput input={.types=&types,.scope=scope,.family=XR_HIR_CALL_NATIVE,.count=3,
        .native_parameters=parameters,.native_owner=XR_NATIVE_DECLARATION_ARRAY,.native_member=member};
    XrNativeMemberDeclaration fabricated=*member;input.native_member=&fabricated;
    XrHirBodySignature signature={0};
    CHECK(xr_hir_body_signature(body,&input,NULL,&signature,NULL)==XR_XIR_BAD_STRUCTURE&&!signature.owner);
    input.native_member=member;input.native_owner=XR_NATIVE_DECLARATION_STRING;
    CHECK(xr_hir_body_signature(body,&input,NULL,&signature,NULL)==XR_XIR_BAD_STRUCTURE&&!signature.owner);
    input.native_owner=XR_NATIVE_DECLARATION_ARRAY;input.count=2;
    CHECK(xr_hir_body_signature(body,&input,NULL,&signature,NULL)==XR_XIR_BAD_STRUCTURE&&!signature.owner);
    input.count=3;signature=binding_signature(body,input);memset(parameters,0xa5,sizeof(parameters));
    XrHirCallBindingError error={0};CHECK(xr_hir_body_call_bind_open(body,signature,NULL,&error)==XR_XIR_OK);
    uint32_t formal=UINT32_MAX;
    CHECK(xr_hir_body_call_bind_argument(body,signature,"end",3,1,false,NULL,&formal,&error)==XR_XIR_OK&&formal==2);
    CHECK(xr_hir_body_call_bind_argument(body,signature,"value",5,1,false,NULL,&formal,&error)==XR_XIR_OK&&formal==0);
    XrHirCallBinding binding={0};CHECK(xr_hir_body_call_bind_seal(body,signature,NULL,&binding,&error)==XR_XIR_OK);
    CHECK(binding.argument_formals[0]==2&&binding.argument_formals[1]==0&&binding.formal_arguments[1]==UINT32_MAX);
    XrHirBodyUse use={0};XrHirBodyAction action={0};XrXirInstruction finish={.op=XR_XIR_INVALID,.type=XR_XIR_UNIT};
    CHECK(xr_hir_body_call_finish(body,signature,&finish,NULL,&use,&action)==XR_XIR_BAD_STRUCTURE);
    for(uint32_t a=0;a<2;++a) {
        uint32_t p=binding.argument_formals[a];
        XrHirCallUseInput argument={.types=&types,.scope=scope,.kind=XR_HIR_CALL_ACCESS,
            .formal=p,.argument=a,.access=XR_CALL_ARG_PLAIN};
        CHECK(xr_hir_body_call_argument(body,signature,&argument,NULL,&use,&action)==XR_XIR_OK);owned_marker(body,use);
        argument.kind=XR_HIR_CALL_VALUE;argument.type=XR_XIR_I64;argument.value=p+10;
        CHECK(xr_hir_body_call_argument(body,signature,&argument,NULL,&use,&action)==XR_XIR_OK);
        owned_projection(body,use,(XrHirBodyProjection){.function=0,.formal=p,.argument=a,
            .type=XR_XIR_I64,.source_value=p+10,.has_value=true,.value=p+10});
    }
    XrHirCallUseInput final={.types=&types,.scope=scope,.kind=XR_HIR_CALL_FINAL,.formal=2,.argument=0,
        .type=XR_XIR_I64,.value=12,.check_type=true,.expected=XR_XIR_I64};
    CHECK(xr_hir_body_call_argument(body,signature,&final,NULL,&use,&action)==XR_XIR_BAD_STRUCTURE);
    for(uint32_t p=0;p<3;p+=2) {
        final.formal=p;final.argument=binding.formal_arguments[p];final.value=p+10;
        CHECK(xr_hir_body_call_argument(body,signature,&final,NULL,&use,&action)==XR_XIR_OK);
        owned_projection(body,use,(XrHirBodyProjection){.function=0,.formal=p,.argument=binding.formal_arguments[p],
            .type=XR_XIR_I64,.source_value=p+10,.has_value=true,.value=p+10});
    }
    CHECK(xr_hir_body_call_finish(body,signature,&finish,NULL,&use,&action)==XR_XIR_OK);
    CHECK(xr_hir_body_finish(body,NULL)==XR_XIR_OK);
    parameters[0]=parameters[1]=parameters[2]=XR_XIR_I64;signature=binding_signature(body,input);
    CHECK(xr_hir_body_call_finish(body,signature,&finish,NULL,&use,&action)==XR_XIR_BAD_STRUCTURE);
    xr_hir_body_free(body);xr_compile_resources_release(resources);CHECK(!physical_live&&!physical_bytes);
}
static void native_collection_callback_receivers(void) {
    XrCompileResources *resources=NULL;XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrXirTypeNode nodes[]={{.kind=XR_XIR_TYPE_MAP,.element=XR_XIR_STRING,.result=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_SET,.element=XR_XIR_I64}};XrXirTypes types={.nodes=nodes,.count=2};
    XrHirBody *body=NULL;CHECK(xr_hir_body_new(resources,&types,&body)==XR_XIR_OK);
    XrHirBodyScope scope={0};CHECK(xr_hir_body_scope(body,scope,(XrHirBodyOrigin){0},NULL,&scope)==XR_XIR_OK);
    for(unsigned mode=0;mode<2;++mode) {
        uint32_t owner=mode?XR_NATIVE_DECLARATION_SET:XR_NATIVE_DECLARATION_MAP;
        const XrNativeMemberDeclaration *member=xr_native_declaration_member(xr_native_declaration_by_id(owner),"forEach");CHECK(member);
        XrXirType parameter=XR_XIR_UNIT;
        XrHirSignatureInput input={.types=&types,.scope=scope,.family=XR_HIR_CALL_NATIVE_CALLBACK,.count=1,
            .native_parameters=&parameter,.native_owner=owner,.native_member=member,
            .collection_type=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+mode),.collection_value=42,.callback_error=XR_XIR_NEVER};
        input.native_owner=XR_NATIVE_DECLARATION_ARRAY;XrHirBodySignature rejected={0};
        CHECK(xr_hir_body_signature(body,&input,NULL,&rejected,NULL)==XR_XIR_BAD_STRUCTURE&&!rejected.owner);
        input.native_owner=owner;XrHirBodySignature signature=binding_signature(body,input);
        XrHirNativeCallbackShape shape={0};
        CHECK(xr_hir_body_callback_context(body,signature,input.collection_type,41,&shape)==XR_XIR_BAD_STRUCTURE);
        CHECK(xr_hir_body_callback_context(body,signature,input.collection_type,42,&shape)==XR_XIR_OK);
        CHECK(shape.minimum==(mode?1u:2u)&&shape.maximum==shape.minimum&&shape.has_result&&
            shape.result==XR_XIR_UNIT&&shape.error==XR_XIR_NEVER&&shape.parameters[0]==(mode?XR_XIR_I64:XR_XIR_STRING));
        if(!mode)CHECK(shape.parameters[1]==XR_XIR_I64);
        CHECK(xr_hir_body_array_accumulator(body,signature,XR_XIR_I64,NULL)==XR_XIR_BAD_STRUCTURE);
    }
    xr_hir_body_free(body);xr_compile_resources_release(resources);CHECK(!physical_live&&!physical_bytes);
}
static void native_named_source_case(const char *original) {
    XrCompileResources *resources=NULL;XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext compile={.resources=resources,.limits=xr_xir_compile_default_limits()};
    size_t length=strlen(original)+1;char source[512];CHECK(length<=sizeof(source));memcpy(source,original,length);
    XrXirSourceResult result={0};size_t first=physical_attempts;
    CHECK(collection_source_receive(&compile,source,NULL,&result)==XR_XIR_OK);
    size_t sites=physical_attempts-first;CHECK(sites>0);memset(source,0xa5,sizeof(source));
    CHECK(xr_xir_compile_artifact_verify(result.checked,NULL)==XR_XIR_OK);
    collection_source_retire(&result);size_t live=physical_live,bytes=physical_bytes;
    memcpy(source,original,length);
    for(size_t point=0;point<sites;++point) {
        physical_fail=physical_attempts+point;
        XrXirStatus status=collection_source_receive(&compile,source,NULL,&result);physical_fail=SIZE_MAX;
        if(status!=XR_XIR_OUT_OF_MEMORY)fprintf(stderr,"Native named fault %zu/%zu status %u\n",point,sites,(unsigned)status);
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
static void native_named_source_receivers(void) {
    native_named_binding_receivers();native_collection_callback_receivers();
    native_named_source_case("fn main(){var xs=[1,2,3];xs.fill(end:2,value:9);xs.set(value:8,index:0);print(xs[0])}\nmain()\n");
    native_named_source_case("fn main(){var xs={\"a\":1};xs.set(value:2,key:\"b\");xs.forEach(callback:(key,value)=>print(value))}\nmain()\n");
    native_named_source_case("fn main(){const xs=Set([1,2]);xs.forEach(callback:(value)=>print(value))}\nmain()\n");
}
