/* Finite held-memory admission uses charged absolute peaks, not turnover. */
static bool owner_record_transaction(const char *path,const XrCompileResourceLimits *limits,XrCompileResourceStats *stats) {
    XrCompileResources *resources=sdk_ledger(limits);XrXirInvocation owner={0};owner.context.resources=resources;owner.io=xr_compile_io_policy(resources);owner.limits.files=16;
    XtcXirFileLease *lease=NULL;bool okay=invocation_target(&owner,xtc_xir_file_lease_open(resources,path,&lease));
    if(okay)okay=invocation_file_add(&owner,XR_XIR_INVOCATION_GENERATED,XR_XIR_INVOCATION_SOURCE,&lease);
    CHECK(xr_compile_resources_stats(resources,stats)==XR_COMPILE_RESOURCE_OK);
    if(okay)CHECK(owner.facts.file_count==1 && !lease);else CHECK(owner.status==XR_XIR_INVOCATION_MEMORY_LIMIT && !owner.facts.file_count);
    for(uint32_t i=0;i<owner.facts.file_count;++i)xtc_xir_file_lease_free(owner.files[i].lease);
    xtc_xir_file_lease_free(lease);xr_compile_resources_free(owner.files);xr_compile_resources_release(resources);CHECK(!runtime_live && !runtime_bytes);return okay;
}
static void owner_record_boundaries(const char *path) {
    XrCompileResourceStats baseline;CHECK(owner_record_transaction(path,&sdk_unlimited,&baseline));CHECK(baseline.peak_bytes>sizeof(XrCompileResources));
    for(unsigned minus=0;minus<2;++minus) {
        XrCompileResourceLimits limits={.memory_mode=XR_COMPILE_MEMORY_FINITE,.memory_limit=baseline.peak_bytes-minus,.profile_allocations=true};XrCompileResourceStats actual;
        CHECK(owner_record_transaction(path,&limits,&actual)==!minus);CHECK(actual.peak_bytes<=limits.memory_limit);
    }
    XrCompileResourceStats restored;CHECK(owner_record_transaction(path,&sdk_unlimited,&restored));
    printf("real file record finite charged peak=%llu exact/minus1 typed MEMORY_LIMIT, ordinary recovery, physical zero PASS\n",(unsigned long long)baseline.peak_bytes);
}
/* These are explicit executable leases and bounded PE facts, not loaded images. */
static bool owner_provider_transaction(const char *compiler,const char *linker,const XrCompileResourceLimits *limits,XrCompileResourceStats *stats) {
    XrCompileResources *resources=sdk_ledger(limits);XtcXirFileLease *leases[2]={NULL,NULL};const char *paths[2]={compiler,linker};bool okay=true;
    for(unsigned i=0;i<2 && okay;++i) {
        void *bytes=NULL;size_t length=0;XrXirTargetStatus status=xtc_xir_file_lease_open(resources,paths[i],&leases[i]);
        if(status==XR_XIR_TARGET_OK)status=xtc_xir_file_lease_read(leases[i],64u*1024u*1024u,&bytes,&length);
        if(status!=XR_XIR_TARGET_OK) {CHECK(status==XR_XIR_TARGET_MEMORY_LIMIT);okay=false;}
        else {XtcXirPeVersion version={0};CHECK(xtc_xir_pe_version_parse(bytes,length,&version)==XTC_XIR_PE_VERSION_OK && version.file_text[0]);}
        xr_compile_resources_free(bytes);
    }
    CHECK(xr_compile_resources_stats(resources,stats)==XR_COMPILE_RESOURCE_OK);xr_compile_resources_release(resources);
    for(unsigned i=0;i<2;++i)if(leases[i]) {CHECK(xtc_xir_file_lease_facts(leases[i])->path[0]);xtc_xir_file_lease_free(leases[i]);}
    CHECK(!runtime_live && !runtime_bytes);return okay;
}
static int owner_provider_suite(const char *compiler,const char *linker) {
    DWORD before=0,after=0;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));XrCompileResourceStats baseline;
    CHECK(owner_provider_transaction(compiler,linker,&sdk_unlimited,&baseline));CHECK(baseline.peak_bytes>sizeof(XrCompileResources));
    for(unsigned minus=0;minus<2;++minus) {XrCompileResourceLimits limits={.memory_mode=XR_COMPILE_MEMORY_FINITE,.memory_limit=baseline.peak_bytes-minus,.profile_allocations=true};XrCompileResourceStats actual;CHECK(owner_provider_transaction(compiler,linker,&limits,&actual)==!minus);CHECK(actual.peak_bytes<=limits.memory_limit);}
    XrCompileResourceStats restored;CHECK(owner_provider_transaction(compiler,linker,&sdk_unlimited,&restored));
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after) && after==before);puts("two explicit provider PE leases, finite held exact/minus1, ledger retirement, physical/handles restored PASS");return 0;
}
