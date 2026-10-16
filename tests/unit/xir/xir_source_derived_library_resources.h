/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_derived_library_resources.h - Finite cold library receiving
 */
#ifndef XIR_SOURCE_DERIVED_LIBRARY_RESOURCES_H
#define XIR_SOURCE_DERIVED_LIBRARY_RESOURCES_H
#ifndef XR_DERIVED_LIBRARY_VARIANT
#define XR_DERIVED_LIBRARY_VARIANT 0
#endif
typedef struct LibraryResourceInput {
    XrXirCheckedPacket packet;
    XrXirLibraryModuleInput modules[2];
    uint8_t digest[32];
} LibraryResourceInput;
typedef struct LibraryResourceReport {
    XrXirStatus status;
    XrCompileResourceStats spent;
    size_t sites;
    bool owner;
} LibraryResourceReport;
static XrCompileResourceStats library_resource_stats(const XrXirCompileContext *context) {
    XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(context->resources,&stats)==XR_COMPILE_RESOURCE_OK);return stats;
}
static void library_resource_monotonic(XrCompileResourceStats a,XrCompileResourceStats b) {
    CHECK(b.allocated_bytes>=a.allocated_bytes && b.work>=a.work && b.allocation_count>=a.allocation_count && b.peak_bytes>=a.peak_bytes);
}
static XrXirStatus library_resource_whole(const XrXirCompileContext *context,const LibraryResourceInput *input,const char **stage) {
    XrXirLibraryInput packet={input->packet.bytes,input->packet.length,{0},(XrXirLibraryModuleInput *)input->modules,2};
    memcpy(packet.sha256,input->digest,32);XrXirLibraryCatalog *catalog=NULL;XrCompilerSession *session=NULL;
    XrXirSourceResult source={0};XrXirCheckedPacket written={0};XrXirArtifact *received=NULL;
    *stage="catalog";XrXirStatus status=xr_xir_compile_library_catalog_new_v2(context,&packet,1,&catalog);
    if(status!=XR_XIR_OK){CHECK(!catalog);goto done;}
    *stage="session";XrCompilerSessionStatus opened=xr_compile_session_new(context->resources,&session);
    if(opened!=XR_COMPILER_SESSION_OK) {
        CHECK(!session);status=opened==XR_COMPILER_SESSION_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:
            opened==XR_COMPILER_SESSION_BUDGET?XR_XIR_BUDGET:XR_XIR_BAD_STRUCTURE;goto done;
    }
    char path[2048],logical[64];int n=snprintf(logical,sizeof(logical),"downstream-%u.xr",(unsigned)XR_DERIVED_LIBRARY_VARIANT);
    CHECK(n>0 && n<(int)sizeof(logical));n=snprintf(path,sizeof(path),"%s/%s",XR_DERIVED_LIBRARY_WORK,logical);CHECK(n>0 && n<(int)sizeof(path));
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_DERIVED_LIBRARY_WORK};
    XrXirSourceRequest request={session,path,&authority,context,NULL,NULL,XR_XIR_PROGRAM,catalog,NULL};
    const char *text=XR_DERIVED_LIBRARY_VARIANT?downstream_local_source:downstream_source;
    XrXirSourceText bytes={logical,text,strlen(text)};XrXirSourceDiagnostic diagnostic={0};
    *stage="Source";status=xr_xir_compile_source_check_text(&request,&bytes,&source,&diagnostic,NULL);
    if(status!=XR_XIR_OK){CHECK(!source.checked && !source.snapshot);goto done;}
    CHECK(source.checked && source.snapshot);*stage="write";status=xr_xir_compile_checked_write(source.checked,&written,NULL);
    xr_xir_compile_source_result_free(&source);
    if(status==XR_XIR_OK){*stage="read";status=xr_xir_compile_checked_read(context,written.bytes,written.length,&received,NULL);}
    if(written.bytes)memset(written.bytes,0xa5,written.length);xr_xir_compile_checked_packet_free(&written);
    if(status==XR_XIR_OK){CHECK(received);*stage="verify";status=xr_xir_compile_artifact_verify(received,NULL);}
done:
    xr_xir_compile_artifact_free(received);xr_xir_compile_checked_packet_free(&written);
    xr_xir_compile_source_result_free(&source);xr_compile_session_free(session);xr_xir_compile_library_catalog_free(catalog);return status;
}
static bool library_resource_trial(const LibraryResourceInput *input,const XrCompileResourceLimits *caps,
    size_t fault,bool retry,LibraryResourceReport *output) {
    size_t held=source_fixture_compile_live,bytes=source_fixture_compile_bytes,start=source_fixture_compile_attempts;
    size_t runtime_held=runtime_live,runtime_held_bytes=runtime_bytes;
    LibraryResourceReport report={0};XrXirCompileContext context={0};bool retry_ok=true;
    source_fixture_compile_injected=false;source_fixture_compile_fail_at=fault==SIZE_MAX?SIZE_MAX:start+fault;
    XrCompileResourceStatus opened=xr_compile_resources_new(caps,&context.resources);
    if(opened!=XR_COMPILE_RESOURCE_OK) {
        CHECK(opened==XR_COMPILE_RESOURCE_OUT_OF_MEMORY && !context.resources);report.status=XR_XIR_OUT_OF_MEMORY;goto done;
    }
    context.limits=xr_xir_compile_default_limits();report.owner=true;XrCompileResourceStats baseline=library_resource_stats(&context);
    const char *stage=NULL;report.status=library_resource_whole(&context,input,&stage);report.sites=source_fixture_compile_attempts-start;
    report.spent=library_resource_stats(&context);library_resource_monotonic(baseline,report.spent);
    CHECK(report.spent.live_bytes==baseline.live_bytes && report.spent.allocated_bytes<=caps->allocated_bytes &&
        report.spent.peak_bytes<=caps->live_bytes && report.spent.work<=caps->work);source_fixture_compile_fail_at=SIZE_MAX;
    if(retry) {
        CHECK(report.status==XR_XIR_OUT_OF_MEMORY || report.status==XR_XIR_BUDGET);
        XrCompileResources *same=context.resources;const char *again_stage=NULL;
        XrXirStatus again=library_resource_whole(&context,input,&again_stage);XrCompileResourceStats after=library_resource_stats(&context);
        library_resource_monotonic(report.spent,after);
        CHECK(context.resources==same && after.live_bytes==baseline.live_bytes && after.allocated_bytes<=caps->allocated_bytes &&
            after.peak_bytes<=caps->live_bytes && after.work<=caps->work);
        retry_ok=again==(report.status==XR_XIR_OUT_OF_MEMORY?XR_XIR_OK:XR_XIR_BUDGET);
        if(!retry_ok)fprintf(stderr,"Cold library retry variant%u fault%zu first%u/%s again%u/%s spent=%llu/%llu/%llu after=%llu/%llu/%llu caps=%llu/%llu/%llu\n",
            (unsigned)XR_DERIVED_LIBRARY_VARIANT,fault,(unsigned)report.status,stage,(unsigned)again,again_stage,
            (unsigned long long)report.spent.allocated_bytes,(unsigned long long)report.spent.peak_bytes,(unsigned long long)report.spent.work,
            (unsigned long long)after.allocated_bytes,(unsigned long long)after.peak_bytes,(unsigned long long)after.work,
            (unsigned long long)caps->allocated_bytes,(unsigned long long)caps->live_bytes,(unsigned long long)caps->work);
    }
    xr_compile_resources_release(context.resources);
done:
    if(!report.sites)report.sites=source_fixture_compile_attempts-start;source_fixture_compile_fail_at=SIZE_MAX;
    CHECK(source_fixture_compile_live==held && source_fixture_compile_bytes==bytes && runtime_live==runtime_held && runtime_bytes==runtime_held_bytes);
    *output=report;
    if(!retry_ok)fprintf(stderr,"Cold library failed retry cleaned original physical baseline compiler=%zu/%zu runtime=%zu/%zu\n",held,bytes,runtime_held,runtime_held_bytes);
    return retry_ok;
}
static void library_resource_controls(void) {
    XrXirCompileContext transport=library_context();LibraryResourceInput input={0};input.packet=library_fixture(&transport,input.modules);
    bool successful=true;
    xr_sha256(input.packet.bytes,input.packet.length,input.digest);LibraryResourceReport first={0},second={0};
    CHECK(library_resource_trial(&input,&library_caps,SIZE_MAX,false,&first));CHECK(library_resource_trial(&input,&library_caps,SIZE_MAX,false,&second));
    CHECK(first.status==XR_XIR_OK && second.status==XR_XIR_OK && first.sites==second.sites && first.sites>2 &&
        !memcmp(&first.spent,&second.spent,sizeof(first.spent)));
    fprintf(stderr,"Cold library variant%u census%zu allocated=%llu peak=%llu work=%llu\n",(unsigned)XR_DERIVED_LIBRARY_VARIANT,first.sites,
        (unsigned long long)first.spent.allocated_bytes,(unsigned long long)first.spent.peak_bytes,(unsigned long long)first.spent.work);fflush(stderr);
    for(unsigned axis=0;axis<3;++axis)for(unsigned less=0;less<2;++less) {
        XrCompileResourceLimits caps=library_caps;uint64_t exact=axis==0?first.spent.allocated_bytes:axis==1?first.spent.peak_bytes:first.spent.work;CHECK(exact>1);
        if(!axis)caps.allocated_bytes=exact-less;else if(axis==1)caps.live_bytes=exact-less;else caps.work=exact-less;
        LibraryResourceReport trial={0};
        if(!library_resource_trial(&input,&caps,SIZE_MAX,less!=0,&trial)){successful=false;goto done;}
        CHECK(trial.status==(less?XR_XIR_BUDGET:XR_XIR_OK));
        if(!less)CHECK(trial.sites==first.sites && !memcmp(&trial.spent,&first.spent,sizeof(first.spent)));
    }
    size_t samples[]={0,first.sites/2,first.sites-1};
    for(unsigned i=0;i<3;++i) {
        LibraryResourceReport trial={0};
        if(!library_resource_trial(&input,&library_caps,samples[i],true,&trial)){successful=false;goto done;}
        CHECK(trial.status==XR_XIR_OUT_OF_MEMORY && source_fixture_compile_injected && trial.sites==samples[i]+1);
    }
done:
    memset(input.packet.bytes,0xa5,input.packet.length);xr_xir_compile_checked_packet_free(&input.packet);xr_compile_resources_release(transport.resources);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes && !runtime_live && !runtime_bytes);
    if(!successful)fprintf(stderr,"Cold library failed control released input and trial owners: compiler0/0 runtime0/0\n");
    CHECK(successful);
}
static size_t library_resource_number(const char *text) {
    CHECK(text && *text>='0' && *text<='9');errno=0;char *end=NULL;unsigned long long value=strtoull(text,&end,10);
    CHECK(!errno && end && !*end && value<=SIZE_MAX);return (size_t)value;
}
static void library_resource_bindings(const XrXirCheckedPacket *packet,XrXirLibraryModuleInput bindings[2]) {
    XrXirCompileContext preparation=library_context();XrXirArtifact *artifact=NULL;
    CHECK(xr_xir_compile_checked_read(&preparation,packet->bytes,packet->length,&artifact,NULL)==XR_XIR_OK);
    const XrXirModule *module=xr_xir_compile_artifact_module(artifact);CHECK(module->declarations && module->declarations->module_count==2);
    const char *paths[]={"derived_library.xr","constructor_names.xr"};XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_DERIVED_LIBRARY_WORK};
    memset(bindings,0,2*sizeof(*bindings));
    for(unsigned p=0;p<2;++p) {
        char *canonical=NULL;CHECK(xr_compile_module_identity_from_logical(preparation.resources,&authority,paths[p],&canonical)==XR_MODULE_OK);unsigned matched=0;
        for(unsigned i=0;i<2;++i)if(module->declarations->modules[i].name_length==strlen(canonical) &&
            !memcmp(module->declarations->modules[i].name,canonical,strlen(canonical))) {
            CHECK(!bindings[i].logical_path);bindings[i]=(XrXirLibraryModuleInput){authority,paths[p]};++matched;
        }
        CHECK(matched==1);xr_compile_resources_free(canonical);
    }
    xr_xir_compile_artifact_free(artifact);xr_compile_resources_release(preparation.resources);
}
static int library_resource_partition(char **argv) {
    size_t first=library_resource_number(argv[3]),end=library_resource_number(argv[4]),sites=library_resource_number(argv[5]);
    CHECK(first<=end && end<=sites && (sites || (!first && !end)));FILE *file=fopen(argv[2],"rb");
    CHECK(file && !fseek(file,0,SEEK_END));long length=ftell(file);CHECK(length>0 && length<=67108864 && !fseek(file,0,SEEK_SET));
    LibraryResourceInput input={0};input.packet.bytes=xr_malloc((size_t)length);input.packet.length=(size_t)length;
    CHECK(input.packet.bytes && fread(input.packet.bytes,1,(size_t)length,file)==(size_t)length && !fclose(file));
    library_resource_bindings(&input.packet,input.modules);xr_sha256(input.packet.bytes,input.packet.length,input.digest);
    FILE *report=fopen(argv[6],"wb");CHECK(report);
    CHECK(fprintf(report,"{\"event\":\"input\",\"first\":%zu,\"end\":%zu,\"sites\":%zu,\"packet_sha256\":\"",first,end,sites)>0);
    for(unsigned i=0;i<32;++i)CHECK(fprintf(report,"%02x",(unsigned)input.digest[i])==2);
    CHECK(fprintf(report,"\",\"caps\":[67108864,8388608,128000000]}\n")>0);
    if(!sites) {
        LibraryResourceReport census={0};CHECK(library_resource_trial(&input,&library_caps,SIZE_MAX,false,&census));CHECK(census.status==XR_XIR_OK && census.owner && census.sites>2);
        CHECK(fprintf(report,"{\"event\":\"census\",\"sites\":%zu,\"allocated\":%llu,\"peak\":%llu,\"work\":%llu}\n",census.sites,
            (unsigned long long)census.spent.allocated_bytes,(unsigned long long)census.spent.peak_bytes,(unsigned long long)census.spent.work)>0);
    }else for(size_t fault=first;fault<end;++fault) {
        LibraryResourceReport trial={0};
        if(!library_resource_trial(&input,&library_caps,fault,true,&trial)) {
            memset(input.packet.bytes,0xa5,input.packet.length);xr_free(input.packet.bytes);
            CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes && !runtime_live && !runtime_bytes);
            CHECK(!fclose(report));fprintf(stderr,"Cold library failed partition released input and trial owners: compiler0/0 runtime0/0\n");
            CHECK(false);
        }
        CHECK(trial.status==XR_XIR_OUT_OF_MEMORY && source_fixture_compile_injected && trial.sites==fault+1);
        CHECK(fprintf(report,"{\"event\":\"fault\",\"ordinal\":%zu,\"attempts\":%zu,\"owner\":%u}\n",fault,trial.sites,(unsigned)trial.owner)>0);
    }
    memset(input.packet.bytes,0xa5,input.packet.length);xr_free(input.packet.bytes);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes && !runtime_live && !runtime_bytes);
    CHECK(fprintf(report,"{\"event\":\"complete\",\"points\":%zu,\"compiler_blocks\":0,\"compiler_bytes\":0}\n",end-first)>0 && !fclose(report));return 0;
}
static int library_resource_main(int argc,char **argv) {
    if(argc==7 && !strcmp(argv[1],"--compiler-fi"))return library_resource_partition(argv);
    if(argc==2 && !strcmp(argv[1],"--compiler-controls")){library_resource_controls();return 0;}
    CHECK(argc==3 && !strcmp(argv[1],"--emit-generator"));XrXirCompileContext transport=library_context();
    XrXirLibraryModuleInput bindings[2];XrXirCheckedPacket packet=library_fixture(&transport,bindings);
    FILE *file=fopen(argv[2],"wb");CHECK(file && fwrite(packet.bytes,1,packet.length,file)==packet.length && !fclose(file));
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);xr_compile_resources_release(transport.resources);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes && !runtime_live && !runtime_bytes);return 0;
}
#endif // XIR_SOURCE_DERIVED_LIBRARY_RESOURCES_H
