/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * Explicit provenance survives overwriting spreads and activation retirement. */
typedef struct NativeMapLiteralCase {
    unsigned mode;
    char key[1536],path[768];
    XrXirDomain *pin;
    uint64_t baseline;
    bool completed;
} NativeMapLiteralCase;
static void native_map_literal_root(void *opaque) {
    NativeMapLiteralCase *context=opaque;
    XrtValueRoot *root=xrt_value_root_current(1);
    context->pin=xrt_value_root_domain(root,1);CHECK(xr_xir_domain_retain(context->pin));
    context->baseline=xr_xir_domain_stats(context->pin).live_bytes;
    XrtString *x=xrt_string_new(context->mode==3?context->key:"x",context->mode==3?sizeof(context->key):1,1);
    XrtString *y=xrt_string_new("y",1,1);
    XrtString *keys[]={x,y};int64_t values[]={8,9};
    XrtMap *spread=xrt_map_string_i64_new(keys,values,2,1);
    XrtMapLiteralOrigin origins[]={{0,10,3},{0,11,7},{0,12,9}};
    XrtMapLiteralInput inputs[]={{{.key=x},1},{{.key=y},2},{{.key=x},3}};
    XrtMapLiteral literal={context->path,sizeof(context->path),origins,context->mode==0?2:3};
    if(context->mode==1 || context->mode==2) {
        origins[1].kind=1;inputs[1].source.spread=spread;
        if(context->mode==1)inputs[2].source.key=y;
    } else if(context->mode==4)origins[1].kind=2;
    else if(context->mode==5){origins[1].kind=1;inputs[1].source.spread=(const XrtMap *)x;}
    else if(context->mode==6)literal.path_length=0;
    XrtMap *map=xrt_map_string_i64_literal(&literal,inputs,1);
    CHECK(xrt_map_length(map,1)==2);
    native_map_value(map,"x",1,context->mode==1?8:1);
    native_map_value(map,"y",1,context->mode==1?3:2);
    xrt_map_drop(&map);xrt_map_drop(&spread);xrt_string_drop(&x);xrt_string_drop(&y);
    context->completed=true;
}
static void native_map_literal_release(NativeMapLiteralCase *context,XrtExecutionFailure *failure) {
    xrt_execution_failure_drop(failure);
    if(context->pin) {
        CHECK(xr_xir_domain_stats(context->pin).live_bytes==context->baseline);
        xr_xir_domain_drop(context->pin);context->pin=NULL;
    }
#if defined(XR_NATIVE_ARRAY_ALLOCATIONS)
    CHECK(!native_array_live && !native_array_bytes);
#endif
}
static void native_map_literal_cases(XrtLocationOwner *locations) {
    for(unsigned mode=0;mode<7;++mode) {
        NativeMapLiteralCase context={.mode=mode};
        memset(context.path,'p',sizeof(context.path));
        memset(context.key,'k',sizeof(context.key));
        context.key[7]=0;context.key[8]='"';context.key[9]='\\';context.key[10]='\n';
        XrtValueRoot root={0};XrtExecutionFailure failure={0};
#if defined(XR_NATIVE_ARRAY_ALLOCATIONS)
        size_t start=native_array_calls;
#endif
        XrtExecutionStatus status=xrt_run_embedded_owned(locations,&failure,native_map_literal_root,&context,&root);
        CHECK(xrt_value_root_empty(&root));
        if(mode<2)CHECK(status==XRT_EXECUTION_COMPLETED && context.completed);
        else if(mode<4) {
            CHECK(status==XRT_EXECUTION_FAILURE && !context.completed && failure.kind==XRT_FAILURE_MESSAGE_PANIC &&
                failure.detail.message.code==434 && failure.detail.message.source_id==1);
            const char *message=NULL;size_t length=0;
            CHECK(xr_xir_string_view(&failure.detail.message.message,&message,&length));
            char expected[4096];size_t cursor=0;
            const char *prefix=mode==2?"duplicate key \"x\" in Map literal (entries at ":"duplicate key \"";
            memcpy(expected,prefix,strlen(prefix));cursor=strlen(prefix);
            if(mode==3) {
                memcpy(expected+cursor,context.key,7);cursor+=7;
                const char escape[]="\\u0000\\\"\\\\\\n";
                memcpy(expected+cursor,escape,sizeof(escape)-1);cursor+=sizeof(escape)-1;
                memcpy(expected+cursor,context.key+11,sizeof(context.key)-11);cursor+=sizeof(context.key)-11;
                const char suffix[]="\" in Map literal (entries at ";
                memcpy(expected+cursor,suffix,sizeof(suffix)-1);cursor+=sizeof(suffix)-1;
            }
            memcpy(expected+cursor,context.path,sizeof(context.path));cursor+=sizeof(context.path);
            memcpy(expected+cursor,":10:3 and ",10);cursor+=10;
            memcpy(expected+cursor,context.path,sizeof(context.path));cursor+=sizeof(context.path);
            memcpy(expected+cursor,":12:9)",6);cursor+=6;
            memset(context.key,0xa5,sizeof(context.key));memset(context.path,0xa5,sizeof(context.path));
            CHECK(length==cursor && !memcmp(message,expected,length));
        } else CHECK(status==XRT_EXECUTION_FAILURE && failure.kind==XRT_FAILURE_VALUE &&
            failure.detail.value.status==XR_XIR_VALUE_BAD_ARGUMENT);
#if defined(XR_NATIVE_ARRAY_ALLOCATIONS)
        size_t sites=native_array_calls-start;
#endif
        native_map_literal_release(&context,&failure);
#if defined(XR_NATIVE_ARRAY_ALLOCATIONS)
        if(mode>=4)continue;
        CHECK(sites>0);
        memset(context.path,'p',sizeof(context.path));memset(context.key,'k',sizeof(context.key));
        context.key[7]=0;context.key[8]='"';context.key[9]='\\';context.key[10]='\n';
        for(size_t point=0;point<sites;++point) {
            context.completed=false;
            native_array_fail_at=native_array_calls+point;native_array_injected=false;
            CHECK(xrt_run_embedded_owned(locations,&failure,native_map_literal_root,&context,&root)==XRT_EXECUTION_FAILURE);
            native_array_fail_at=SIZE_MAX;
            CHECK(native_array_injected && !context.completed && xrt_value_root_empty(&root) &&
                failure.kind==XRT_FAILURE_VALUE && failure.detail.value.status==XR_XIR_VALUE_OOM);
            native_map_literal_release(&context,&failure);
        }
#endif
    }
    puts("Map literal provenance, full panic ownership and every allocation prefix passed");
}
