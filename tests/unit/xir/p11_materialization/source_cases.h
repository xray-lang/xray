/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_cases.h - Complete Source receiving and Checked library relocation
 */
static void source_faults(unsigned mode) {
    XrXirCompileContext context;context_new(&context);XrXirSourceResult output={0};
    size_t start=source_fixture_compile_attempts;
    CHECK(source_operation(&context,mode,XR_XIR_PROGRAM,&output)==XR_XIR_OK);
    size_t sites=source_fixture_compile_attempts-start;CHECK(sites);
    xr_xir_compile_source_result_free(&output);xr_compile_resources_release(context.resources);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    fprintf(stderr,"Private materialization whole Source mode=%u actual census=%zu\n",mode,sites);
    for (size_t fault=0;fault<sites;++fault) {
        context_new(&context);XrCompileResources *same=context.resources;
        XrCompileResourceStats baseline=stats(&context);start=source_fixture_compile_attempts;
        source_fixture_compile_injected=false;source_fixture_compile_fail_at=start+fault;
        CHECK(source_operation(&context,mode,XR_XIR_PROGRAM,&output)==XR_XIR_OUT_OF_MEMORY);
        CHECK(!output.checked && !output.snapshot && source_fixture_compile_injected);
        CHECK(source_fixture_compile_attempts==start+fault+1 && stats(&context).held_bytes==baseline.held_bytes);
        source_fixture_compile_fail_at=SIZE_MAX;
        CHECK(source_operation(&context,mode,XR_XIR_PROGRAM,&output)==XR_XIR_OK && output.checked);
        xr_xir_compile_source_result_free(&output);
        CHECK(context.resources==same && stats(&context).held_bytes==baseline.held_bytes);
        xr_compile_resources_release(context.resources);
        CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
        if (!(fault%128) || fault+1==sites) fprintf(stderr,"Private materialization whole Source FI=%zu/%zu same-owner retry/live\n",fault+1,sites);
    }
}
static void source_library_case(bool fresh) {
    XrXirCompileContext context;context_new(&context);XrXirSourceResult source={0};
    CHECK(source_operation(&context,3,XR_XIR_LIBRARY,&source)==XR_XIR_OK);
    const XrXirDeclarations *declarations=xr_xir_compile_artifact_module(source.checked)->declarations;
    CHECK(declarations->module_count==2);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,MATERIALIZATION_ROOT};
    XrXirLibraryModuleInput bindings[2]={0};const char *names[2]={"root.xr","private.xr"};
    for (uint32_t m=0;m<2;++m) {
        bool found=false;
        for (uint32_t n=0;n<2;++n) {
            char *canonical=NULL;
            CHECK(xr_compile_module_identity_from_logical(context.resources,&authority,names[n],&canonical)==XR_MODULE_OK);
            if (declarations->modules[m].name_length==strlen(canonical) &&
                !memcmp(declarations->modules[m].name,canonical,strlen(canonical))) {
                CHECK(!found);bindings[m]=(XrXirLibraryModuleInput){authority,names[n]};found=true;
            }
            xr_compile_resources_free(canonical);
        }
        CHECK(found);
    }
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&source);
    XrXirLibraryInput input={packet.bytes,packet.length,{0},bindings,2};xr_sha256(input.packet,input.length,input.sha256);
    XrXirLibraryCatalog *catalog=NULL;
    XrXirStatus status=xr_xir_compile_library_catalog_new_v2(&context,&input,1,&catalog);
    if (status!=XR_XIR_OK) fprintf(stderr,"Materializer library catalog status=%u\n",(unsigned)status);
    CHECK(status==XR_XIR_OK && catalog);xr_xir_compile_checked_packet_free(&packet);memset(bindings,0xa5,sizeof(bindings));
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context.resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrXirSourceRequest request={session,MATERIALIZATION_ROOT "/consumer.xr",&authority,&context,NULL,NULL,XR_XIR_PROGRAM,catalog};
    char text[256];
    CHECK(snprintf(text,sizeof(text),"import {run} from \"./root\";import {Outer} from \"./private\";export fn answer()->i64{%s}",
        fresh?"const value=comptime Outer.outer();return value.get()":"return run()")>0);
    XrXirSourceText source_text={"consumer.xr",text,strlen(text)};XrXirSourceDiagnostic diagnostic={0};
    status=xr_xir_compile_source_check_text(&request,&source_text,&source,&diagnostic,NULL);
    if (status!=XR_XIR_OK) fprintf(stderr,"Materializer library receiving status=%u %s\n",(unsigned)status,diagnostic.message);
    CHECK(status==XR_XIR_OK && source.checked);
    xr_compile_session_free(session);xr_xir_compile_library_catalog_free(catalog);memset(text,0xa5,sizeof(text));
    CHECK(xr_xir_compile_construction_materialization_count(source.checked->construction)==(fresh?4u:2u));
    XrXirArtifact *closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_specialize(source.checked,&closed,NULL, NULL)==XR_XIR_OK);xr_xir_compile_source_result_free(&source);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target, NULL,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    xr_compile_resources_release(context.resources);execute(&lowered,"answer",fresh?4:2);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    fprintf(stderr,"Checked library private materializer edges relocated, fresh declaration-born helpers=%u, caller packets/catalog dead; two Instance42\n",fresh?2:0);
}
