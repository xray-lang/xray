/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
static const XrCompileResourceLimits entry_text_limits={UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(128000000)};
static XrManifestStatus entry_text_run(const XrCompileResourceLimits *limits,size_t failure,
    XrCompileResourceStats *measurement) {
    reset(failure);XrCompileResources *resources=NULL;
    XrCompileResourceStatus opened=xr_compile_resources_new(limits,&resources);
    if(opened!=XR_COMPILE_RESOURCE_OK)return opened==XR_COMPILE_RESOURCE_BUDGET?XR_MANIFEST_BUDGET:XR_MANIFEST_OUT_OF_MEMORY;
    XrXirCompileContext context={resources,xr_xir_compile_default_limits()};
    char path[4096],text[]="export fn entry()->i64 { return 41 }\n";
    CHECK(snprintf(path,sizeof(path),"%s\\sub\\not-on-disk\\main.xr",root_path)>0);
    XrCliGraphEntryInput input={XR_CLI_GRAPH_ENTRY_TEXT,path,text,sizeof(text)-1};
    XrCliGraphAuthority *authority=NULL;
    XrManifestStatus status=xr_cli_compile_graph_authority_open_input(&context,&input,NULL,&parse_limits,&authority,NULL);
    memset(path,'?',sizeof(path));memset(text,'?',sizeof(text));memset(&input,0,sizeof(input));
    if(measurement)*measurement=stats(resources);
    xr_compile_resources_release(resources);
    if(status==XR_MANIFEST_OK) {
        const XrModuleIdentityAuthority *identity=xr_cli_compile_graph_authority_entry(authority);
        CHECK(identity->kind==XR_MODULE_IDENTITY_PROJECT&&!strcmp(identity->namespace_id,"owner-test"));
        CHECK(xr_cli_compile_graph_authority_project(authority)->native_plan);
        const XrLockfile *lock=xr_cli_compile_graph_authority_lockfile(authority);
        CHECK(lock&&lock->package_count==1&&!strcmp(lock->packages[0].name,"owner/dep")&&
            !strcmp(lock->packages[0].version,"1.2.3"));
        const XrModuleOverlayInput *entry=xr_cli_compile_graph_authority_overlay(authority);
        CHECK(entry&&!strcmp(entry->logical_path,"sub/not-on-disk/main.xr")&&
            !strcmp(entry->text,"export fn entry()->i64 { return 41 }\n")&&entry->length==37);
        CHECK(!strcmp(entry->source_path,xr_cli_compile_graph_authority_source_path(authority)));
    } else CHECK(!authority);
    xr_cli_compile_graph_authority_close(authority);CHECK(!physical_live&&!block_count);return status;
}
static void entry_text_failure_matrix(void) {
    XrCompileResourceStats baseline={0};
    CHECK(entry_text_run(&entry_text_limits,SIZE_MAX,&baseline)==XR_MANIFEST_OK);size_t sites=attempts;CHECK(sites);
    for(size_t site=0;site<sites;++site) {
        CHECK(entry_text_run(&entry_text_limits,site,NULL)==XR_MANIFEST_OUT_OF_MEMORY);
        CHECK(attempts==site+1&&!physical_live&&!block_count);
    }
    for(unsigned axis=0;axis<3;++axis)for(int delta=-1;delta<=1;++delta) {
        XrCompileResourceLimits limits={baseline.allocated_bytes,baseline.peak_bytes,baseline.work};
        uint64_t *bound=axis==0?&limits.allocated_bytes:axis==1?&limits.live_bytes:&limits.work;
        CHECK(*bound&&*bound<UINT64_MAX);*bound=(uint64_t)((int64_t)*bound+delta);
        CHECK(entry_text_run(&limits,SIZE_MAX,NULL)==(delta<0?XR_MANIFEST_BUDGET:XR_MANIFEST_OK));
    }
    printf("TEXT project authority: real fresh FI sites=%zu, three axes, physical=0/0\n",sites);
}
static void entry_text_boundaries(const char *script_path) {
    reset(SIZE_MAX);XrCompileResources *resources=NULL;
    CHECK(xr_compile_resources_new(&entry_text_limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext context={resources,xr_xir_compile_default_limits()};XrCliGraphAuthority *authority=NULL;
    char path[4096];const char text[]="export fn value()->i64 { return 1 }\n";
    XrCliGraphEntryInput input={XR_CLI_GRAPH_ENTRY_TEXT,path,text,sizeof(text)-1};
    const char *invalid[]={"sub/../escape.xr","sub/./bad.xr","sub/CON.xr","sub/bad. ","sub/name:stream.xr","sub//bad.xr"};
    for(size_t i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) {
        CHECK(snprintf(path,sizeof(path),"%s/%s",root_path,invalid[i])>0);
        CHECK(xr_cli_compile_graph_authority_open_input(&context,&input,NULL,&parse_limits,&authority,NULL)==XR_MANIFEST_BAD_ARGUMENT&&!authority);
    }
    CHECK(snprintf(path,sizeof(path),"%s\\bad\\missing.xr",root_path)>0);
    CHECK(xr_cli_compile_graph_authority_open_input(&context,&input,NULL,&parse_limits,&authority,NULL)==XR_MANIFEST_INVALID&&!authority);
    CHECK(snprintf(path,sizeof(path),"%s\\package\\missing\\entry.xr",root_path)>0);
    CHECK(xr_cli_compile_graph_authority_open_input(&context,&input,NULL,&parse_limits,&authority,NULL)==XR_MANIFEST_OK);
    CHECK(xr_cli_compile_graph_authority_entry(authority)->kind==XR_MODULE_IDENTITY_PACKAGE&&
        !strcmp(xr_cli_compile_graph_authority_entry(authority)->namespace_id,"owner/package@1.2.3"));
    CHECK(!xr_cli_compile_graph_authority_lockfile(authority));
    CHECK(!strcmp(xr_cli_compile_graph_authority_overlay(authority)->logical_path,"missing/entry.xr"));
    XrCliGraphAuthority *occupied=authority;size_t before=attempts;
    CHECK(xr_cli_compile_graph_authority_open_input(&context,&input,NULL,&parse_limits,&occupied,NULL)==XR_MANIFEST_BAD_ARGUMENT&&occupied==authority&&before==attempts);
    xr_cli_compile_graph_authority_close(authority);authority=NULL;
    CHECK(strlen(script_path)<sizeof(path));strcpy(path,script_path);
    char *slash=strrchr(path,'\\');CHECK(slash && (size_t)(slash-path)+sizeof("new-sub\\entry.xr")<sizeof(path));strcpy(slash+1,"new-sub\\entry.xr");
    CHECK(xr_cli_compile_graph_authority_open_input(&context,&input,NULL,&parse_limits,&authority,NULL)==XR_MANIFEST_OK);
    CHECK(xr_cli_compile_graph_authority_entry(authority)->kind==XR_MODULE_IDENTITY_SCRIPT&&
        !xr_cli_compile_graph_authority_project(authority));
    CHECK(!strcmp(xr_cli_compile_graph_authority_overlay(authority)->logical_path,"entry.xr"));
    xr_cli_compile_graph_authority_close(authority);authority=NULL;
    CHECK(snprintf(path,sizeof(path),"%s\\sub\\text.xr",root_path)>0);
    const char nul_text[]={'x',0,'y'};input.text=nul_text;input.length=sizeof(nul_text);
    CHECK(xr_cli_compile_graph_authority_open_input(&context,&input,NULL,&parse_limits,&authority,NULL)==XR_MANIFEST_INVALID&&!authority);
    input.text=text;input.length=SIZE_MAX;
    CHECK(xr_cli_compile_graph_authority_open_input(&context,&input,NULL,&parse_limits,&authority,NULL)==XR_MANIFEST_BUDGET&&!authority);
    input.length=sizeof(text)-1;
    XrXirLibraryCatalog *catalog=real_catalog(&context);
    CHECK(snprintf(path,sizeof(path),"%s\\sub\\text.xr",root_path)>0);
    CHECK(xr_cli_compile_graph_authority_open_input(&context,&input,catalog,&parse_limits,&authority,NULL)==XR_MANIFEST_OK);
    CHECK(xr_cli_compile_graph_authority_catalog(authority)==catalog);
    xr_cli_compile_graph_authority_close(authority);authority=NULL;
    XrCompileResources *foreign=NULL;CHECK(xr_compile_resources_new(&entry_text_limits,&foreign)==XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext wrong={foreign,xr_xir_compile_default_limits()};before=attempts;
    CHECK(xr_cli_compile_graph_authority_open_input(&wrong,&input,catalog,&parse_limits,&authority,NULL)==XR_MANIFEST_BAD_ARGUMENT&&!authority&&before==attempts);
    xr_compile_resources_release(foreign);xr_xir_compile_library_catalog_free(catalog);
    xr_compile_resources_release(resources);CHECK(!physical_live&&!block_count);
}
