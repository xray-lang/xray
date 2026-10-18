/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_limits.h - Invalid and exhausted requests preserve the original owner
 */
static void source_receiving_limits(void) {
    SourceReceivingFixture fixture=source_receiving_fixture("scalar",0);
    for (unsigned axis=0;axis<5;++axis) {
        XrXirCompileContext context;context_new(&context);XrCompilerSession *session=NULL;
        CHECK(xr_compile_session_new(context.resources,&session)==XR_COMPILER_SESSION_OK);
        XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"instance-constant",NULL};
        XrXirSourceRequest source={session,NULL,&authority,&context,NULL,NULL,XR_XIR_PROGRAM,NULL};
        XrXirSourceText text={NULL,instance_source_text(),strlen(instance_source_text())};
        XirSourceInstanceSelection selection={fixture.definition,fixture.functions[0]};
        XirSourceInstanceRequest original={.source=&source,.text=&text,.selections=&selection,.count=1,
            .target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},
            .limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,XR_XIR_CTFE_MAX_OUTPUT_BYTES},.call_depth=XR_XIR_CTFE_MAX_CALL_DEPTH};
        XirSourceInstanceRequest request=original;XirSourceInstanceConstants result={0};XrXirSourceDiagnostic diagnostic={0};
        if (!axis) request.limits.steps=0;else if (axis==1) request.limits.frame_bytes=0;
        else if (axis==2) request.limits.output_bytes=0;else if (axis==3) request.call_depth=0;
        else request.target.abi_version=XR_XIR_VALUE_ABI_VERSION+1;
        XrXirStatus expected=axis==4?XR_XIR_BAD_TYPE:XR_XIR_BUDGET;
        for (unsigned retry=0;retry<2;++retry) {
            CHECK(xr_xir_compile_source_instance_constants(&request,&result,&diagnostic)==expected && diagnostic.status==expected);
            CHECK(!result.source.checked && !result.source.snapshot && !result.functions && !result.count);
        }
        CHECK(xr_xir_compile_source_instance_constants(&original,&result,&diagnostic)==XR_XIR_OK && result.count==1);
        xr_xir_compile_source_instance_constants_free(&result);xr_compile_session_free(session);xr_compile_resources_release(context.resources);
        CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    }
    XrXirCompileContext context;context_new(&context);XrCompilerSession *session=NULL;
    CHECK(xr_compile_session_new(context.resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"instance-constant",NULL};
    XrXirSourceRequest source={session,NULL,&authority,&context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceText text={NULL,instance_source_text(),strlen(instance_source_text())};
    XirSourceInstanceSelection selection={fixture.definition,fixture.functions[0]};
    XirSourceInstanceRequest original={.source=&source,.text=&text,.selections=&selection,.count=1,
        .target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},
        .limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,XR_XIR_CTFE_MAX_OUTPUT_BYTES},.call_depth=XR_XIR_CTFE_MAX_CALL_DEPTH};
    XirSourceInstanceConstants result={0};XrXirSourceDiagnostic diagnostic={0};
    CHECK(xr_xir_compile_source_instance_constants(NULL,&result,&diagnostic)==XR_XIR_BAD_STRUCTURE);
    XirSourceInstanceRequest request=original;request.count=0;
    CHECK(xr_xir_compile_source_instance_constants(&request,&result,&diagnostic)==XR_XIR_BAD_STRUCTURE);
    request=original;request.limits.steps=XR_XIR_CTFE_MAX_STEPS+1;
    CHECK(xr_xir_compile_source_instance_constants(&request,&result,&diagnostic)==XR_XIR_BAD_STRUCTURE);
    request=original;request.overlay_count=1;
    CHECK(xr_xir_compile_source_instance_constants(&request,&result,&diagnostic)==XR_XIR_BAD_STRUCTURE);
    XirSourceInstanceConstants occupied={.count=1};
    CHECK(xr_xir_compile_source_instance_constants(&original,&occupied,&diagnostic)==XR_XIR_BAD_STRUCTURE && occupied.count==1);
    uint32_t definition=selection.definition;selection.definition=UINT32_MAX;
    CHECK(xr_xir_compile_source_instance_constants(&original,&result,&diagnostic)==XR_XIR_BAD_STRUCTURE && !result.source.checked);
    selection.definition=definition;uint32_t function=selection.function;selection.function=UINT32_MAX;
    CHECK(xr_xir_compile_source_instance_constants(&original,&result,&diagnostic)==XR_XIR_BAD_VALUE && !result.functions);
    selection.function=function;source.linkage_kind=XR_XIR_LIBRARY;
    CHECK(xr_xir_compile_source_instance_constants(&original,&result,&diagnostic)==XR_XIR_BAD_TYPE && !result.source.checked);
    source.linkage_kind=XR_XIR_PROGRAM;
    XrCompileResourceStats used=stats(&context);
    CHECK(xr_compile_resources_work(context.resources,caps.work-used.work)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_xir_compile_source_instance_constants(&original,&result,&diagnostic)==XR_XIR_BUDGET && !result.source.checked && !result.functions);
    CHECK(xr_xir_compile_source_instance_constants(&original,&result,&diagnostic)==XR_XIR_BUDGET && !result.source.checked && !result.functions);
    size_t attempts=source_fixture_compile_attempts;xr_compile_session_free(session);xr_compile_resources_release(context.resources);
    CHECK(source_fixture_compile_attempts==attempts && !source_fixture_compile_live && !source_fixture_compile_bytes);
}
