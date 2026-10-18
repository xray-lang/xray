/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * comptime_instances.h - Real syntax occurrences close through ordinary generics
 */
#include "xir/xxir_source_comptime_instances_internal.h"
static const char *comptime_instances_text(const char *kind) {
    if (!strcmp(kind,"array")) return "fn make<T>()->Array<T>{return comptime []}\nexport fn run()->i64{return len(make<i8>())+len(make<string>())+42}\n";
    if (!strcmp(kind,"optional")) return "fn make<T>()->T?{return comptime null}\nexport fn run()->i64{const a=make<i8>();const b=make<string>();return 42}\n";
    if (!strcmp(kind,"tuple")) return "fn make<T>()->(T?,Array<T>){return comptime (null,[])}\nexport fn run()->i64{const a=make<i8>();const b=make<string>();return 42}\n";
    if (!strcmp(kind,"scalar")) return "fn make<T>()->i64{return comptime {const v:Array<T>=[];len(v)+42}}\nexport fn run()->i64{return make<i8>()+make<string>()-42}\n";
    if (!strcmp(kind,"call")) return "fn take<T>(v:Array<T>)->i64{return len(v)+42}\nfn make<T>()->i64{return comptime take<T>([])}\nexport fn run()->i64{return make<i8>()+make<string>()-42}\n";
    if (!strcmp(kind,"multi")) return "fn make<A,B>()->(Array<A>,Array<B>){return comptime ([],[])}\nexport fn run()->i64{const a=make<i8,string>();const b=make<string,i8>();return 42}\n";
    if (!strcmp(kind,"method")) return "struct Box<T>{private v:T?;static make()->Array<T>{return comptime []}}\nexport fn run()->i64{return len(Box<i8>.make())+len(Box<string>.make())+42}\n";
    if (!strcmp(kind,"multiple")) return "fn make<T>()->i64{const a:Array<T>=comptime [];const b:T?=comptime null;return 42}\nexport fn run()->i64{return make<i8>()+make<string>()-42}\n";
    if (!strcmp(kind,"nested")) return "fn make<T>()->Array<T>{return comptime []}\nfn relay<U>()->i64{return len(make<Array<Array<U>>>())+42}\nexport fn run()->i64{return relay<i8>()+relay<string>()-42}\n";
    if (!strcmp(kind,"private")) return "struct Box<T>{private v:T?;static empty()->Box<T>{return Box<T>{v:null}};static make()->Box<T>{return comptime Box<T>.empty()}}\nexport fn run()->i64{const a=Box<i8>.make();const b=Box<string>.make();return 42}\n";
    if (!strcmp(kind,"unit")) return "fn make<T>(){comptime {const v:Array<T>=[];}}\nexport fn run()->i64{make<i8>();make<string>();return 42}\n";
    CHECK(false);return NULL;
}
static XrXirStatus comptime_instances_operation(XrXirCompileContext *context,const char *text,
    XirSourceComptimeInstances *output,XrXirCtfeLimits limits,uint32_t depth,bool public_source) {
    XrCompilerSession *session=NULL;XrCompilerSessionStatus created=xr_compile_session_new(context->resources,&session);
    if (created!=XR_COMPILER_SESSION_OK) return created==XR_COMPILER_SESSION_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:
        created==XR_COMPILER_SESSION_BUDGET?XR_XIR_BUDGET:XR_XIR_BAD_STRUCTURE;
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"comptime-instances",NULL};
    XrXirSourceRequest source={session,NULL,&authority,context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceText input={NULL,text,strlen(text)};XrXirSourceDiagnostic diagnostic={0};
    XirSourceComptimeRequest request={&source,&input,NULL,0,{XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},limits,depth};
    XrXirSourceResult unpublished={0};
    XrXirStatus status=public_source?xr_xir_compile_source_check_text(&source,&input,&unpublished,&diagnostic,NULL):
        xr_xir_compile_source_comptime_instances(&request,output,&diagnostic);
    if (status!=XR_XIR_OK && status!=XR_XIR_OUT_OF_MEMORY && status!=XR_XIR_BUDGET)
        fprintf(stderr,"Typed comptime Source=%u %d:%d %s\n",status,diagnostic.line,diagnostic.column,diagnostic.message);
    if (public_source && status==XR_XIR_OK) {
        CHECK(unpublished.checked && unpublished.snapshot);xr_xir_compile_source_result_free(&unpublished);
    } else CHECK(!unpublished.checked && !unpublished.snapshot);
    xr_compile_session_free(session);return status;
}
static XrXirCtfeLimits comptime_instances_limits(void) {
    return (XrXirCtfeLimits){XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,XR_XIR_CTFE_MAX_OUTPUT_BYTES};
}
static void comptime_instances_normal(const char *kind) {
    XrXirCompileContext context;context_new(&context);XirSourceComptimeInstances result={0};
    const char *text=comptime_instances_text(kind);
    XrXirStatus status=comptime_instances_operation(&context,text,&result,comptime_instances_limits(),XR_XIR_CTFE_MAX_CALL_DEPTH,false);
    CHECK(status==XR_XIR_OK && result.owner && result.count==(!strcmp(kind,"multiple")?4u:2u));
    XirCtfeInstanceView views[4]={0};XirSourceComptimeBinding bindings[4]={0};XrXirValue values[4]={0};
    CHECK(xr_xir_compile_source_comptime_instances_view(&result,views,bindings,result.count)==XR_XIR_CTFE_OK);
    for (uint32_t i=0;i<result.count;++i) {
        CHECK(bindings[i].helper==views[i].definition_function && bindings[i].callee==views[i].instance_function);
        CHECK(views[i].definition->module.generics[bindings[i].helper].parameter_count==(!strcmp(kind,"multi")?2u:1u));
        const XrXirInstruction *op=&views[i].checked->module.functions[bindings[i].function].instructions[bindings[i].instruction];
        CHECK((op->op==XR_XIR_CALL || op->op==XR_XIR_INVOKE) && op->immediate==bindings[i].callee);
        CHECK(xr_xir_compile_type_substitution_matches_between(&context,views[i].checked->module.types,views[i].lowered->module.types,
            NULL,0,op->type,(XrXirType)views[i].value->type)==XR_XIR_OK);
        if ((uint32_t)op->type!=views[i].value->type)
            fprintf(stderr,"Typed comptime cross-pool result Checked=%u Lowered=%u structural-match\n",op->type,views[i].value->type);
        if (!strcmp(kind,"nested")) CHECK(instance_argument_absent(views[i].checked,views[i].checked->module.provenance->origins[bindings[i].callee].arguments[0]));
        if (!strcmp(kind,"scalar")) CHECK(views[i].value->type==XR_XIR_I64 && views[i].value->payload==42);
        CHECK(xr_xir_value_copy(views[i].value,&values[i])==XR_XIR_VALUE_OK);
    }
    CHECK(comptime_instances_operation(&context,text,NULL,comptime_instances_limits(),XR_XIR_CTFE_MAX_CALL_DEPTH,true)==XR_XIR_OK);
    xr_compile_resources_release(context.resources);memset(views,0,sizeof(views));memset(bindings,0,sizeof(bindings));
    CHECK(xr_xir_compile_source_comptime_instances_view(&result,views,bindings,result.count)==XR_XIR_CTFE_OK);
    uint32_t count=result.count;xr_xir_compile_source_comptime_instances_free(&result);CHECK(!result.owner && !result.count);
    size_t attempts=source_fixture_compile_attempts;source_fixture_compile_fail_at=attempts;
    for (uint32_t i=0;i<count;++i) { CHECK(xr_xir_value_valid(&values[i]));xr_xir_value_drop(&values[i]); }
    CHECK(source_fixture_compile_attempts==attempts);source_fixture_compile_fail_at=SIZE_MAX;
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    fprintf(stderr,"Typed comptime %s actual syntax/closed-call bindings=%u two substitutions producer-dead physical0 public receiving checked\n",kind,count);
}
static void comptime_instances_controls(void) {
    XrXirCompileContext context;context_new(&context);XirSourceComptimeInstances result={0};
    CHECK(xr_xir_compile_source_comptime_instances(NULL,&result,NULL)==XR_XIR_BAD_STRUCTURE && !result.owner);
    XrXirCtfeLimits original=comptime_instances_limits();const char *text=comptime_instances_text("scalar");
    for (unsigned axis=0;axis<4;++axis) {
        XrXirCtfeLimits limits=original;uint32_t depth=XR_XIR_CTFE_MAX_CALL_DEPTH;
        if (!axis) limits.steps=0;else if (axis==1) limits.frame_bytes=0;else if (axis==2) limits.output_bytes=0;else depth=0;
        XrCompileResourceStats before=stats(&context);
        CHECK(comptime_instances_operation(&context,text,&result,limits,depth,false)==XR_XIR_BUDGET && !result.owner && !result.count);
        CHECK(stats(&context).live_bytes==before.live_bytes);
    }
    const char *capture="fn make<T>(value:T)->T{return comptime value}\nexport fn run()->i64{return make<i64>(42)}\n";
    CHECK(comptime_instances_operation(&context,capture,&result,original,XR_XIR_CTFE_MAX_CALL_DEPTH,false)==XR_XIR_BAD_VALUE && !result.owner);
    CHECK(comptime_instances_operation(&context,text,&result,original,XR_XIR_CTFE_MAX_CALL_DEPTH,false)==XR_XIR_OK);
    XirCtfeInstanceView views[2]={0};XirSourceComptimeBinding bindings[2]={0};bindings[1].helper=1;
    CHECK(xr_xir_compile_source_comptime_instances_view(&result,views,bindings,2)==XR_XIR_CTFE_BAD_ARGUMENT && !views[0].checked && bindings[1].helper==1);
    memset(bindings,0,sizeof(bindings));CHECK(xr_xir_compile_source_comptime_instances_view(&result,views,bindings,2)==XR_XIR_CTFE_OK);
    XrXirType *argument=(XrXirType *)views[0].checked->module.provenance->origins[bindings[0].callee].arguments;
    XrXirType saved=*argument;*argument=saved==XR_XIR_I8?XR_XIR_STRING:XR_XIR_I8;
    memset(views,0,sizeof(views));memset(bindings,0,sizeof(bindings));
    CHECK(xr_xir_compile_source_comptime_instances_view(&result,views,bindings,2)==XR_XIR_CTFE_BAD_ARTIFACT && !views[0].checked && !views[1].checked);
    *argument=saved;CHECK(xr_xir_compile_source_comptime_instances_view(&result,views,bindings,2)==XR_XIR_CTFE_OK);
    XrCompileResourceStats used=stats(&context);CHECK(xr_compile_resources_work(context.resources,caps.work-used.work)==XR_COMPILE_RESOURCE_OK);
    memset(views,0,sizeof(views));memset(bindings,0,sizeof(bindings));
    CHECK(xr_xir_compile_source_comptime_instances_view(&result,views,bindings,2)==XR_XIR_CTFE_BUDGET && !views[0].checked);
    size_t attempts=source_fixture_compile_attempts;xr_xir_compile_source_comptime_instances_free(&result);xr_compile_resources_release(context.resources);
    CHECK(source_fixture_compile_attempts==attempts && !source_fixture_compile_live && !source_fixture_compile_bytes);
}
