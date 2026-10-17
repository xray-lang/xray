/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#ifndef LSP_PARSE_POSITION_CASES_H
#define LSP_PARSE_POSITION_CASES_H
static void diagnostic_positions(void) {
    const struct {const char *text;size_t length;int line,column;XrModuleStatus status;uint32_t row,units;} cases[]={
        {"",0,1,1,XR_MODULE_OK,0,0},{"a",1,1,2,XR_MODULE_OK,0,1},
        {"a\nb",3,2,2,XR_MODULE_OK,1,1},{"a\r\nb",4,2,2,XR_MODULE_OK,1,1},
        {"a\rb",3,1,4,XR_MODULE_OK,1,1},
        {"\xf0\x9f\x98\x80",4,1,5,XR_MODULE_OK,0,2},
        {"a\xf0\x9f\x98\x80" "b",6,1,6,XR_MODULE_OK,0,3},
        {"\xf0\x9f\x98\x80",4,1,2,XR_MODULE_INVALID,0,0},
        {"\xf0\x9f\x98\x80",4,1,3,XR_MODULE_INVALID,0,0},
        {"\xf0\x9f\x98\x80",4,1,4,XR_MODULE_INVALID,0,0},
        {"\xffx",2,1,2,XR_MODULE_INVALID,0,0},{"a\0b",3,1,3,XR_MODULE_INVALID,0,0},
        {"abc",3,1,5,XR_MODULE_INVALID,0,0},{"a\r\nb",4,1,3,XR_MODULE_INVALID,0,0},
        {"a",1,0,1,XR_MODULE_INVALID,0,0},{"a",1,1,0,XR_MODULE_INVALID,0,0}
    };
    const XrCompileResourceLimits limits={UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(128000000)};
    for(size_t i=0;i<sizeof(cases)/sizeof(*cases);++i) {
        XrCompileResources *resources=sdk_ledger(&limits);XrCompileResourceStats before=sdk_stats(resources);
        XrModuleSourcePosition point={99,77};
        CHECK(xr_compile_module_source_position(resources,cases[i].text,cases[i].length,cases[i].line,cases[i].column,&point)==cases[i].status);
        if(cases[i].status==XR_MODULE_OK)CHECK(point.line==cases[i].row&&point.column==cases[i].units);
        else CHECK(point.line==99&&point.column==77);
        XrCompileResourceStats after=sdk_stats(resources);
        CHECK(before.allocated_bytes==after.allocated_bytes&&before.live_bytes==after.live_bytes);
        xr_compile_resources_release(resources);CHECK(!runtime_live&&!runtime_bytes);
    }
    for(int delta=-1;delta<=1;++delta) {
        /* Ledger construction spends one work unit; projection spends six. */
        XrCompileResourceLimits cut=limits;cut.work=(uint64_t)(7+delta);
        XrCompileResources *resources=sdk_ledger(&cut);XrModuleSourcePosition point={99,77};
        XrCompileResourceStats before=sdk_stats(resources);
        XrModuleStatus status=xr_compile_module_source_position(resources,"a\xf0\x9f\x98\x80" "b",6,1,6,&point);
        CHECK(status==(delta<0?XR_MODULE_BUDGET:XR_MODULE_OK));
        if(delta<0)CHECK(point.line==99&&point.column==77);else CHECK(point.line==0&&point.column==3);
        XrCompileResourceStats after=sdk_stats(resources);
        CHECK(before.allocated_bytes==after.allocated_bytes&&before.live_bytes==after.live_bytes);
        xr_compile_resources_release(resources);CHECK(!runtime_live&&!runtime_bytes);
    }
    fprintf(stderr,"PARSE_POSITION cases=16 work_cuts=3 no_allocation=1 failure_output_preserved=1 physical0/0\n");
}
static void diagnostic_occupied(const DiagnosticCase *input) {
    XrCompileResources *resources=sdk_ledger(&input->limits);XrCompilerSession *session=NULL;
    CHECK(xr_compile_session_new(resources,&session)==XR_COMPILER_SESSION_OK);
    XrXirCompileContext context={resources,xr_xir_compile_default_limits()};
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,input->directory};
    XrXirSourceRequest request={session,input->root_path,&authority,&context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrModuleOverlayInput overlay={authority,"root.xr",input->root_path,root_source,sizeof(root_source)-1};
    for(unsigned field=0;field<5;++field) {
        XrXirSourceFailure failure={0};
        if(!field)failure.path=(char *)"occupied";
        else if(field==1)failure.origin=XR_XIR_SOURCE_DIAGNOSTIC_PARSE;
        else if(field==2)failure.has_utf16_position=true;
        else if(field==3)failure.utf16_line=1;else failure.utf16_column=1;
        XrXirSourceFailure saved=failure;XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
        XrCompileResourceStats before=sdk_stats(resources);size_t attempts=runtime_attempts;
        CHECK(xr_xir_compile_source_check_overlay(&request,&overlay,1,&result,&diagnostic,&failure)==XR_XIR_BAD_STRUCTURE);
        CHECK(!result.checked&&!result.snapshot&&runtime_attempts==attempts);
        CHECK(failure.path==saved.path&&failure.origin==saved.origin&&failure.has_utf16_position==saved.has_utf16_position&&
            failure.utf16_line==saved.utf16_line&&failure.utf16_column==saved.utf16_column);
        XrCompileResourceStats after=sdk_stats(resources);
        CHECK(before.allocated_bytes==after.allocated_bytes&&before.live_bytes==after.live_bytes&&before.work==after.work);
    }
    xr_compile_session_free(session);xr_compile_resources_release(resources);CHECK(!runtime_live&&!runtime_bytes);
    fprintf(stderr,"PARSE_OCCUPIED fields=5 before_allocation=1 output_preserved=1 physical0/0\n");
}
#endif // LSP_PARSE_POSITION_CASES_H
