/* Complete Source receiving rechecks the witness on every failure prefix. */
static void source_failures(const XrXirCompileContext *compile) {
    size_t live=source_fixture_compile_live,bytes=source_fixture_compile_bytes;
    XrXirSourceResult result={0};source_fixture_compile_attempts=0;
    CHECK(source(compile,"source-fault.xr",producer,XR_XIR_LIBRARY,NULL,NULL,&result)==XR_XIR_OK);
    size_t sites=source_fixture_compile_attempts;CHECK(sites);
    fprintf(stderr,"Hashable Source allocation sweep sites=%zu\n",sites);fflush(stderr);
    xr_xir_compile_source_result_free(&result);
    CHECK(source_fixture_compile_live==live && source_fixture_compile_bytes==bytes);
    for(size_t point=0;point<sites;++point) {
        source_fixture_compile_attempts=0;source_fixture_compile_fail_at=point;source_fixture_compile_injected=false;
        XrXirStatus status=source(compile,"source-fault.xr",producer,XR_XIR_LIBRARY,NULL,NULL,&result);
        if(status!=XR_XIR_OUT_OF_MEMORY || !source_fixture_compile_injected || result.checked || result.snapshot)
            fprintf(stderr,"Hashable Source allocation failed point=%zu/%zu status=%u injected=%u\n",
                point,sites,(unsigned)status,source_fixture_compile_injected?1u:0u);
        CHECK(status==XR_XIR_OUT_OF_MEMORY && source_fixture_compile_injected);
        CHECK(!result.checked && !result.snapshot);
        xr_xir_compile_source_result_free(&result);
        CHECK(source_fixture_compile_live==live && source_fixture_compile_bytes==bytes);
    }
    source_fixture_compile_fail_at=SIZE_MAX;
    Cancel cancellation={0,SIZE_MAX};XrCompileRequestControl control={0};
    XrCompileRequestOptions options={.cancel_poll=cancelled,.cancel_context=&cancellation};
    CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
    CHECK(source(compile,"source-cancel.xr",producer,XR_XIR_LIBRARY,NULL,&control,&result)==XR_XIR_OK);
    size_t polls=cancellation.polls;CHECK(polls);xr_xir_compile_source_result_free(&result);
    fprintf(stderr,"Hashable Source cancellation sweep polls=%zu\n",polls);fflush(stderr);
    for(size_t point=1;point<=polls;++point) {
        cancellation=(Cancel){0,point};control=(XrCompileRequestControl){0};
        CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
        XrXirStatus status=source(compile,"source-cancel.xr",producer,XR_XIR_LIBRARY,NULL,&control,&result);
        if(status!=XR_XIR_CANCELLED || result.checked || result.snapshot)
            fprintf(stderr,"Hashable Source cancellation failed point=%zu/%zu status=%u\n",point,polls,(unsigned)status);
        CHECK(status==XR_XIR_CANCELLED);
        CHECK(!result.checked && !result.snapshot);
        xr_xir_compile_source_result_free(&result);
        CHECK(source_fixture_compile_live==live && source_fixture_compile_bytes==bytes);
    }
    printf("Hashable complete Source FI=%zu cancel-prefixes=%zu\n",sites,polls);
}
