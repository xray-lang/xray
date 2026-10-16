#define main materialization_original_main
#include "../p11_materialization/test_materialization.c"
#undef main

static XrXirStatus library_operation(const XrXirCompileContext *context,bool fresh,XrXirSourceResult *output) {
    XrXirSourceResult source={0};XrXirCheckedPacket packet={0};XrXirLibraryCatalog *catalog=NULL;
    XrCompilerSession *session=NULL;XrXirStatus status=source_operation(context,3,XR_XIR_LIBRARY,&source);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,MATERIALIZATION_ROOT};
    XrXirLibraryModuleInput bindings[2]={0};const char *names[2]={"root.xr","private.xr"};
    if (status!=XR_XIR_OK) goto done;
    const XrXirDeclarations *declarations=xr_xir_compile_artifact_module(source.checked)->declarations;
    CHECK(declarations->module_count==2);
    for (uint32_t m=0;m<2;++m) {
        bool found=false;
        for (uint32_t n=0;n<2;++n) {
            char *canonical=NULL;
            XrModuleStatus identity=xr_compile_module_identity_from_logical(context->resources,&authority,names[n],&canonical);
            if (identity!=XR_MODULE_OK) {
                status=identity==XR_MODULE_BUDGET?XR_XIR_BUDGET:identity==XR_MODULE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BAD_STRUCTURE;
                xr_compile_resources_free(canonical);goto done;
            }
            if (declarations->modules[m].name_length==strlen(canonical) &&
                !memcmp(declarations->modules[m].name,canonical,strlen(canonical))) {
                CHECK(!found);bindings[m]=(XrXirLibraryModuleInput){authority,names[n]};found=true;
            }
            xr_compile_resources_free(canonical);
        }
        CHECK(found);
    }
    status=xr_xir_compile_checked_write(source.checked,&packet,NULL);
    if (status!=XR_XIR_OK) goto done;
    xr_xir_compile_source_result_free(&source);
    XrXirLibraryInput input={packet.bytes,packet.length,{0},bindings,2};xr_sha256(input.packet,input.length,input.sha256);
    status=xr_xir_compile_library_catalog_new_v2(context,&input,1,&catalog);
    if (status!=XR_XIR_OK) goto done;
    xr_xir_compile_checked_packet_free(&packet);memset(bindings,0xa5,sizeof(bindings));
    XrCompilerSessionStatus created=xr_compile_session_new(context->resources,&session);
    if (created!=XR_COMPILER_SESSION_OK) {
        status=created==XR_COMPILER_SESSION_BUDGET?XR_XIR_BUDGET:created==XR_COMPILER_SESSION_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BAD_STRUCTURE;
        goto done;
    }
    XrXirSourceRequest request={session,MATERIALIZATION_ROOT "/consumer.xr",&authority,context,NULL,NULL,XR_XIR_PROGRAM,catalog};
    char text[256];
    CHECK(snprintf(text,sizeof(text),"import {run} from \"./root\";import {Outer} from \"./private\";export fn answer()->i64{%s}",
        fresh?"const value=comptime Outer.outer();return value.get()":"return run()")>0);
    XrXirSourceText source_text={"consumer.xr",text,strlen(text)};XrXirSourceDiagnostic diagnostic={0};
    status=xr_xir_compile_source_check_text(&request,&source_text,output,&diagnostic,NULL);
    memset(text,0xa5,sizeof(text));
done:
    xr_compile_session_free(session);xr_xir_compile_library_catalog_free(catalog);
    xr_xir_compile_checked_packet_free(&packet);xr_xir_compile_source_result_free(&source);
    return status;
}

static void library_faults(bool fresh,size_t first,size_t maximum) {
    XrXirCompileContext context;context_new(&context);XrXirSourceResult output={0};
    size_t start=source_fixture_compile_attempts;
    CHECK(library_operation(&context,fresh,&output)==XR_XIR_OK && output.checked && output.snapshot);
    size_t sites=source_fixture_compile_attempts-start;
    CHECK(xr_xir_compile_construction_materialization_count(output.checked->construction)==(fresh?4u:2u));
    XrCompileResourceStats used=stats(&context);
    xr_xir_compile_source_result_free(&output);xr_compile_resources_release(context.resources);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    fprintf(stderr,"Library whole Source fresh=%u census=%zu allocated=%llu peak=%llu work=%llu\n",fresh,sites,
        (unsigned long long)used.allocated_bytes,(unsigned long long)used.peak_bytes,(unsigned long long)used.work);
    CHECK(first<=sites);
    size_t end=maximum<sites-first?first+maximum:sites;
    for (size_t fault=first;fault<end;++fault) {
        context_new(&context);XrCompileResources *same=context.resources;XrCompileResourceStats baseline=stats(&context);
        start=source_fixture_compile_attempts;source_fixture_compile_injected=false;source_fixture_compile_fail_at=start+fault;
        XrXirStatus status=library_operation(&context,fresh,&output);
        if (status!=XR_XIR_OUT_OF_MEMORY || source_fixture_compile_attempts!=start+fault+1)
            fprintf(stderr,"Library FI first-error mismatch ordinal=%zu status=%u attempts=%zu expected=%zu injected=%u\n",fault,status,source_fixture_compile_attempts-start,fault+1,source_fixture_compile_injected);
        CHECK(status==XR_XIR_OUT_OF_MEMORY && source_fixture_compile_injected && !output.checked && !output.snapshot);
        CHECK(source_fixture_compile_attempts==start+fault+1 && stats(&context).live_bytes==baseline.live_bytes);
        XrCompileResourceStats failed=stats(&context);source_fixture_compile_fail_at=SIZE_MAX;
        CHECK(library_operation(&context,fresh,&output)==XR_XIR_OK && output.checked && output.snapshot);
        xr_xir_compile_source_result_free(&output);
        CHECK(context.resources==same && stats(&context).live_bytes==baseline.live_bytes && stats(&context).work>=failed.work);
        xr_compile_resources_release(context.resources);CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
        if (!(fault%128) || fault+1==end) fprintf(stderr,"Library whole Source fresh=%u FI=%zu/%zu same-owner retry/live physical0\n",fresh,fault+1,sites);
    }
    fprintf(stderr,"Library probe completed interval [%zu,%zu) of actual %zu; full qualification only if entire [0,census) terminal\n",first,end,sites);
}

int main(int argc,char **argv) {
    CHECK(argc==4);bool fresh=!strcmp(argv[1],"fresh");CHECK(fresh || !strcmp(argv[1],"import"));
    library_faults(fresh,(size_t)strtoull(argv[2],NULL,10),(size_t)strtoull(argv[3],NULL,10));
    return 0;
}
