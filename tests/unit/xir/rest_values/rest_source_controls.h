/* Rejections and finite request failures keep outputs empty and owners balanced. */
#ifndef XR_REST_SOURCE_CONTROLS_H
#define XR_REST_SOURCE_CONTROLS_H
static void rest_source_refusals(XrXirCompileContext *context) {
    static const char *const cases[]={
        "fn f(xs:Array<i64>)->i64{return len(xs)}\nf(1,2)\n",
        "fn f(...xs:i64)->i64{return len(xs)}\nconst g:fn(Array<i64>)->i64=f\n",
        "fn f(xs:Array<i64>)->i64{return len(xs)}\nconst g:fn(...i64)->i64=f\n",
        "fn f(x:i64,...xs:i64)->i64{return x}\nf(...[1])\n",
        "fn f(...xs:i64)->i64{return len(xs)}\nf(...[1],2)\n",
        "fn f(...xs:i64)->i64{return len(xs)}\nf(...[1],...[2])\n",
        "fn f(x:i64)->i64{return x}\nf(...[1])\n",
        "fn f(...xs:i64)->i64{return len(xs)}\nvar a=[1,2]\nf(...a[:])\n",
        "fn f(...xs:i64){xs[0]=0}\n",
        "fn f(...xs:i64=1){}\n",
        "fn f(...xs:i64,x:i64){}\n",
        "fn f(...xs:i64,...ys:i64){}\n",
        "const f=fn(...xs:i64)->i64{return len(xs)}\n",
        "struct S{f(...xs:i64)->i64{return len(xs)}}\n",
        "class S{init(...xs:i64){}}\n",
        "fn f(...xs:i64)->i64{return len(xs)}\nf(...[\"bad\"])\n",
        "fn f(x:i64=40,...xs:i64)->i64{return x}\nconst g:fn(i64,...i64)->i64=f\ng()\n",
        "fn f(...xs:Slice<i64>){}\n"
        ,"fn f(seed:ref i64){fn local(...xs:i64)->Iterator<i64>{yield seed};const g=local}\n"
        ,"fn f(...xs:ref i64){}\n"
    };
    uint64_t before=held(context->resources);
    for(size_t i=0;i<sizeof(cases)/sizeof(*cases);++i) {
        XrXirSourceResult output={0};XrXirStatus status=source(context,cases[i],&output,NULL);
        CHECK((status==XR_XIR_BAD_TYPE||status==XR_XIR_BAD_STRUCTURE)&&!output.checked&&!output.snapshot);
        CHECK(held(context->resources)==before);
    }
    printf("Rest Source refusals %zu preserve empty output and held baseline\n",sizeof(cases)/sizeof(*cases));
}
typedef struct RestCancel {unsigned calls,stop;} RestCancel;
static bool rest_cancel(const void *context) {RestCancel *state=(RestCancel *)context;return ++state->calls==state->stop;}
static void rest_request_failures(XrXirCompileContext *context,const char *text) {
    uint64_t before=held(context->resources);
    for(unsigned i=0;i<3;++i) {
        RestCancel state={0,i?2048:64};XrCompileRequestControl control={0};
        XrCompileRequestOptions options={.cancel_poll=i<2?rest_cancel:NULL,.cancel_context=i<2?&state:NULL,
            .deadline_mode=i==2?XR_COMPILE_DEADLINE_AFTER:XR_COMPILE_DEADLINE_NONE};
        CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
        XrXirSourceResult output={0};XrXirStatus status=source(context,text,&output,&control);
        CHECK(status==(i==2?XR_XIR_DEADLINE:XR_XIR_CANCELLED)&&!output.checked&&!output.snapshot);
        if(i<2)CHECK(state.calls==state.stop);
        CHECK(held(context->resources)==before);
    }
    XrXirCompileContext finite={.limits=xr_xir_compile_default_limits()};
    const XrCompileResourceLimits limits={XR_COMPILE_MEMORY_FINITE,16384,true};
    CHECK(xr_compile_resources_new(&limits,&finite.resources)==XR_COMPILE_RESOURCE_OK);
    uint64_t finite_before=held(finite.resources);XrXirSourceResult refused={0};
    CHECK(source(&finite,text,&refused,NULL)==XR_XIR_MEMORY_LIMIT&&!refused.checked&&!refused.snapshot);
    CHECK(held(finite.resources)==finite_before);xr_compile_resources_release(finite.resources);
    puts("Rest cancellation/deadline/finite memory failures retire without publication");
}
#endif
