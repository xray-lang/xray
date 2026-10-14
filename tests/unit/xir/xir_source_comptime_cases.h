/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_comptime_cases.h - Independent Source constant and refusal controls
 */
#ifndef XIR_SOURCE_COMPTIME_CASES_H
#define XIR_SOURCE_COMPTIME_CASES_H
static void source_comptime_test_answer(const char *text,XrXirType type,int64_t expected) {
    XrXirCompileContext context={0};
    CHECK(xr_compile_resources_new(&source_comptime_test_limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();XrCompileResourceStats baseline=source_comptime_test_stats(&context);
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=source_comptime_test_build(&context,text,&source,&diagnostic);
    if (status!=XR_XIR_OK) fprintf(stderr,"Source comptime status%u %d:%d %s\n",(unsigned)status,diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==XR_XIR_OK && source.checked && source.snapshot);
    const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(source.snapshot);
    CHECK(view && view->complete && view->module_count && view->declaration_count && view->expression_count);
    const XrXirSourceSyntaxView *syntax=xr_xir_compile_source_snapshot_syntax(source.snapshot);
    CHECK(syntax && syntax->declaration_count==view->declaration_count && syntax->reference_count==view->reference_count);
    bool helper=false;
    for (uint32_t d=0;d<view->declaration_count;++d) {
        const char *name=view->declarations[d].name;
        if (!name || strncmp(name,"$comptime",9)) continue;
        const XrXirSourceDeclarationSyntax *selection=&syntax->declarations[d];
        CHECK(selection->role==XR_XIR_SOURCE_SYNTAX_CLOSURE);
        CHECK(!selection->name.line && !selection->name.column && !selection->name.end_line && !selection->name.end_column);
        helper=true;
    }
    CHECK(helper);
    CHECK(xr_xir_compile_artifact_verify(source.checked,NULL)==XR_XIR_OK);
    source_comptime_test_fold(xr_xir_compile_artifact_module(source.checked),expected,type);
    XrXirCheckedPacket packet={0};XrXirArtifact *received=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&source);
    CHECK(xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&received,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_artifact_verify(received,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_specialize(received,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(received);
    CHECK(xr_xir_compile_artifact_verify(closed,NULL)==XR_XIR_OK);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    CHECK(xr_xir_compile_artifact_verify(lowered,NULL)==XR_XIR_OK);
    uint32_t f=source_comptime_test_function(xr_xir_compile_artifact_module(lowered),"run");
    XrXirRunContext run={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,0,0,0,0};XrXirValue value={0};
    CHECK(xr_xir_compile_vm_run(lowered,f,&run,NULL,0,&value)==XR_XIR_RUN_OK);
    CHECK(value.type==(uint32_t)type && !value.reserved && value.payload==expected);
    CHECK(!run.live_bytes && run.allocations==run.frees);
    xr_xir_compile_artifact_free(lowered);XrCompileResourceStats spent=source_comptime_test_stats(&context);
    CHECK(spent.live_bytes==baseline.live_bytes);source_comptime_test_monotonic(baseline,spent);
    CHECK(spent.allocated_bytes<=source_comptime_test_limits.allocated_bytes && spent.peak_bytes<=source_comptime_test_limits.live_bytes &&
        spent.work<=source_comptime_test_limits.work);
    xr_compile_resources_release(context.resources);CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
}
static void source_comptime_test_semantics(void) {
    source_comptime_test_answer("export fn run()->i64 { return comptime (6*7) }\n",XR_XIR_I64,42);
    source_comptime_test_answer(source_comptime_test_text,XR_XIR_I64,42);
    source_comptime_test_answer("fn cell()->i64 { var n=1; n=2; return n }\nexport fn run()->i64 { return comptime cell() }\n",XR_XIR_I64,2);
    source_comptime_test_answer("fn leaf(x:i64)->i64 { return x+1 }\nfn idle()->i64 { print(1); return 1 }\n"
        "export fn run()->i64 { return comptime leaf(41) }\n",XR_XIR_I64,42);
    source_comptime_test_answer("fn equal(x:i64)->bool { return x==41 }\nexport fn run()->bool { return comptime equal(41) }\n",XR_XIR_BOOL,1);
    source_comptime_test_answer("fn equal(x:i64)->bool { return x==41 }\nexport fn run()->i64 { return (comptime equal(41)) ? 42 : 0 }\n",XR_XIR_I64,42);
    source_comptime_test_answer("fn loop(flag:bool)->i64 { while(flag) {}; return 1 }\n"
        "export fn run()->i64 { return comptime loop(false) }\n",XR_XIR_I64,1);
    source_comptime_test_answer("fn recurse(n:i64)->i64 { return n==0 ? 41 : recurse(n-1) }\n"
        "export fn run()->i64 { return comptime recurse(3) }\n",XR_XIR_I64,41);
    source_comptime_test_answer("fn leaf(x:i64)->i64 { return x+1 }\nexport fn run()->i64 { return comptime { const n=40; leaf(n) } }\n",XR_XIR_I64,41);
    source_comptime_test_answer("export fn run()->i8 { return comptime (6+1) }\n",XR_XIR_I8,7);
    source_comptime_test_answer("export fn run()->i64 { return comptime (comptime (6*7)) }\n",XR_XIR_I64,42);
    source_comptime_test_answer("fn id<T>(x:T)->T { return x }\nexport fn run()->i64 { return comptime id<i64>(41) }\n",XR_XIR_I64,41);
    source_comptime_test_answer("fn id<T>(x:T)->T { return x }\nexport fn run()->i8 { return comptime id<i8>(7) }\n",XR_XIR_I8,7);
}
static void source_comptime_test_refuse(const char *text,XrXirStatus expected) {
    XrXirCompileContext context={0};
    CHECK(xr_compile_resources_new(&source_comptime_test_limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();XrCompileResourceStats baseline=source_comptime_test_stats(&context);
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus actual=source_comptime_test_build(&context,text,&source,&diagnostic);
    if (actual!=expected) fprintf(stderr,"Source comptime reject got%u want%u %d:%d %s\nInput: %s\n",(unsigned)actual,(unsigned)expected,
        diagnostic.line,diagnostic.column,diagnostic.message,text);
    CHECK(actual==expected && diagnostic.status==expected && !source.checked && !source.snapshot);
    XrCompileResourceStats after=source_comptime_test_stats(&context);
    CHECK(after.live_bytes==baseline.live_bytes);source_comptime_test_monotonic(baseline,after);
    xr_compile_resources_release(context.resources);CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
}
static void source_comptime_test_occupied(void) {
    XrXirCompileContext context={0};
    CHECK(xr_compile_resources_new(&source_comptime_test_limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();XrCompileResourceStats baseline=source_comptime_test_stats(&context);
    XrXirSourceResult output={0};XrXirSourceDiagnostic diagnostic={0};
    CHECK(source_comptime_test_build(&context,source_comptime_test_text,&output,&diagnostic)==XR_XIR_OK);
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context.resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"source-comptime",NULL};
    XrXirSourceRequest request={session,NULL,&authority,&context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceText text={NULL,source_comptime_test_text,sizeof(source_comptime_test_text)-1};
    XrXirSourceResult original=output;XrCompileResourceStats before=source_comptime_test_stats(&context);
    size_t attempts=source_fixture_compile_attempts;
    CHECK(xr_xir_compile_source_check_text(&request,&text,&output,&diagnostic,NULL)==XR_XIR_BAD_STRUCTURE);
    CHECK(output.checked==original.checked && output.snapshot==original.snapshot);
    XrCompileResourceStats after=source_comptime_test_stats(&context);
    CHECK(!memcmp(&before,&after,sizeof(before)) && attempts==source_fixture_compile_attempts);
    xr_compile_session_free(session);xr_xir_compile_source_result_free(&output);
    CHECK(source_comptime_test_stats(&context).live_bytes==baseline.live_bytes);
    xr_compile_resources_release(context.resources);CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
}
static void source_comptime_test_string_result(const char *text) {
    XrXirCompileContext context={0};
    CHECK(xr_compile_resources_new(&source_comptime_test_limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();XrCompileResourceStats baseline=source_comptime_test_stats(&context);
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    CHECK(source_comptime_test_build(&context,text,&source,&diagnostic)==XR_XIR_OK);
    CHECK(source.checked && source.snapshot && xr_xir_compile_artifact_verify(source.checked,NULL)==XR_XIR_OK);
    XrXirCheckedPacket packet={0};XrXirArtifact *received=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);xr_xir_compile_source_result_free(&source);
    CHECK(xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&received,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(received,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(received);
    CHECK(xr_xir_compile_checked_write(closed,&packet,NULL)==XR_XIR_OK);
    uint8_t identity[32]={0};CHECK(xr_compile_resources_work(context.resources,packet.length)==XR_COMPILE_RESOURCE_OK);
    xr_sha256(packet.bytes,packet.length,identity);xr_xir_compile_checked_packet_free(&packet);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    XrXirCtfeCallRequest request={.evaluation={.artifact=lowered,
        .function=source_comptime_test_function(xr_xir_compile_artifact_module(lowered),"run"),.target=target,
        .limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,7}},.call_depth=XR_XIR_CTFE_MAX_CALL_DEPTH};
    memcpy(request.evaluation.checked_identity,identity,sizeof(identity));XrXirValue result={0};
    CHECK(xr_xir_compile_ctfe_calls(&request,&result)==XR_XIR_CTFE_OK);xr_xir_compile_artifact_free(lowered);
    const char *bytes=NULL;size_t length=0;
    CHECK(xr_xir_string_view(&result,&bytes,&length) && length==7 && !memcmp(bytes,"managed",7));
    xr_xir_value_drop(&result);CHECK(source_comptime_test_stats(&context).live_bytes==baseline.live_bytes);
    xr_compile_resources_release(context.resources);CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
}
static void source_comptime_test_rejections(void) {
    source_comptime_test_refuse("fn io()->i64 { print(1); return 1 }\nexport fn run()->i64 { return comptime io() }\n",XR_XIR_BAD_TYPE);
    source_comptime_test_refuse("var n=1\nfn state()->i64 { return n }\nexport fn run()->i64 { return comptime state() }\n",XR_XIR_BAD_TYPE);
    source_comptime_test_refuse("fn indirect()->i64 { const f=fn(x:i64)->i64 { return x+1 }; return f(40) }\n"
        "export fn run()->i64 { return comptime indirect() }\n",XR_XIR_BAD_TYPE);
    source_comptime_test_refuse("export fn run(x:i64)->i64 { return comptime x }\n",XR_XIR_BAD_VALUE);
    source_comptime_test_refuse("fn recurse()->i64 { return recurse() }\nexport fn run()->i64 { return comptime recurse() }\n",XR_XIR_BUDGET);
    source_comptime_test_refuse("fn loop(flag:bool)->i64 { while(flag) {}; return 1 }\n"
        "export fn run()->i64 { return comptime loop(true) }\n",XR_XIR_BUDGET);
    source_comptime_test_refuse("fn divide()->i64 { return 41/0 }\nexport fn run()->i64 { return comptime divide() }\n",XR_XIR_BAD_TYPE);
    source_comptime_test_refuse("fn deferred()->i64 { defer { print(1) }; return 1 }\nexport fn run()->i64 { return comptime deferred() }\n",XR_XIR_BAD_TYPE);
    source_comptime_test_refuse("export fn run()->i64 { return comptime true }\n",XR_XIR_BAD_TYPE);
    source_comptime_test_string_result("export fn run()->string { return comptime \"managed\" }\n");
    source_comptime_test_occupied();
}
#endif // XIR_SOURCE_COMPTIME_CASES_H
