/* Independent receiving/import failure atomicity for the folded JSON domain.
 * Included by test_json_domain.c after the controlled check_script helper.
 */
static void json_catalog_failures(const XrXirLibraryInput *input) {
    const size_t live=source_fixture_compile_live,bytes=source_fixture_compile_bytes;
    size_t sites=0;
    for(size_t p=0;p<=sites;++p) {
        source_fixture_compile_attempts=0;source_fixture_compile_fail_at=p?p-1:SIZE_MAX;
        source_fixture_compile_injected=false;
        XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,true};
        XrXirCompileContext context={.limits=xr_xir_compile_default_limits()};
        XrXirLibraryCatalog *catalog=NULL;
        XrXirStatus status=xir_compile_resource_status(xr_compile_resources_new(&limits,&context.resources));
        if(status==XR_XIR_OK)status=xr_xir_compile_library_catalog_new_v2(&context,input,1,&catalog);
        if(!p) {
            if(status!=XR_XIR_OK)fprintf(stderr,"JSON measured Catalog status=%u\n",(unsigned)status);
            CHECK(status==XR_XIR_OK && catalog);sites=source_fixture_compile_attempts;CHECK(sites);
        }
        else {CHECK(status==XR_XIR_OUT_OF_MEMORY && source_fixture_compile_injected && !catalog);}
        xr_xir_compile_library_catalog_free(catalog);
        if(context.resources)xr_compile_resources_release(context.resources);
        CHECK(source_fixture_compile_live==live && source_fixture_compile_bytes==bytes);
    }
    source_fixture_compile_fail_at=SIZE_MAX;
    printf("Folded JSON Catalog FI: receiving=%zu\n",sites);
}
static void json_consumer_failures(const XrXirLibraryCatalog *catalog,const char *text) {
    const size_t live=source_fixture_compile_live,bytes=source_fixture_compile_bytes;
    const XrXirCompileContext *context=xr_xir_compile_library_catalog_context(catalog);
    CHECK(context && context->resources);
    size_t sites=0;
    for(size_t p=0;p<=sites;++p) {
        source_fixture_compile_attempts=0;source_fixture_compile_fail_at=p?p-1:SIZE_MAX;
        source_fixture_compile_injected=false;
        XrXirSourceResult result={0};
        XrXirStatus status=check_script(context,"consumer.xr",text,XR_XIR_PROGRAM,catalog,NULL,&result);
        if(!p) {CHECK(status==XR_XIR_OK && result.checked);sites=source_fixture_compile_attempts;CHECK(sites);}
        else {CHECK(status==XR_XIR_OUT_OF_MEMORY && source_fixture_compile_injected && !result.checked && !result.snapshot);}
        xr_xir_compile_source_result_free(&result);
        CHECK(source_fixture_compile_live==live && source_fixture_compile_bytes==bytes);
    }
    source_fixture_compile_fail_at=SIZE_MAX;
    size_t polls=0;
    for(size_t p=0;p<=polls;++p) {
        JsonColdCancel state={0,p?p:SIZE_MAX};XrCompileRequestControl control={0};
        XrCompileRequestOptions options={.cancel_poll=json_cold_cancel,.cancel_context=&state};
        CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
        XrXirSourceResult result={0};
        XrXirStatus status=check_script(context,"consumer.xr",text,XR_XIR_PROGRAM,catalog,&control,&result);
        if(!p) {CHECK(status==XR_XIR_OK && result.checked);polls=state.polls;CHECK(polls);}
        else {CHECK(status==XR_XIR_CANCELLED && !result.checked && !result.snapshot);}
        xr_xir_compile_source_result_free(&result);
        CHECK(source_fixture_compile_live==live && source_fixture_compile_bytes==bytes);
    }
    printf("Folded JSON consumer import: FI=%zu cancellation-prefixes=%zu\n",sites,polls);
}
