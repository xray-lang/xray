/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * program_cases.h - Public syntax publishes complete per-instance literal bindings
 */
#include "program_foreign.h"
static const char *program_text(const char *kind) {
    if (!strcmp(kind,"mixed")) return "fn make<T>()->i64{const n:i64=comptime 20+1;const a:Array<T>=comptime [];return n+len(a)}\n"
        "export fn run()->i64{return make<i8>()+make<string>()}\n";
    if (!strcmp(kind,"unused")) return "fn make<T>()->Array<T>{return comptime []}\nexport fn run()->i64{return 42}\n";
    if (!strcmp(kind,"nested-open")) return "fn make<T>()->i64{return comptime {const v:Array<T>=comptime [];len(v)+42}}\n"
        "export fn run()->i64{return make<i8>()+make<string>()-42}\n";
    if (!strcmp(kind,"nested-closed")) return "fn make<T>()->i64{return comptime {const n:i64=comptime 42;const v:Array<T>=[];len(v)+n}}\n"
        "export fn run()->i64{return make<i8>()+make<string>()-42}\n";
    return comptime_instances_text(kind);
}
static XrXirStatus program_source(XrXirCompileContext *context,const char *text,XrXirSourceResult *output,XrXirSourceDiagnostic *diagnostic) {
    XrCompilerSession *session=NULL;XrCompileResourceStatus created=xr_compile_session_new(context->resources,&session);
    if (created!=XR_COMPILE_RESOURCE_OK) return p11_resource_status(created);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"program-comptime",NULL};
    XrXirSourceRequest request={session,NULL,&authority,context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceText input={NULL,text,strlen(text)};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,output,diagnostic,NULL);
    xr_compile_session_free(session);return status;
}
#include "program_debug.h"
static void program_normal(const char *kind) {
    XrXirCompileContext context;context_new(&context);XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=!strcmp(kind,"foreign")?program_foreign_source(&context,&source,&diagnostic):
        program_source(&context,program_text(kind),&source,&diagnostic);
    if (status!=XR_XIR_OK) fprintf(stderr,"Public comptime %s status%u %d:%d %s\n",kind,status,diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==XR_XIR_OK && source.checked && source.snapshot);
    const XrXirComptimeTable *table=xr_xir_compile_construction_comptime(source.checked->construction);
    uint32_t sites=!strcmp(kind,"multiple") || !strcmp(kind,"nested-open")?2:1;
    uint32_t bindings=!strcmp(kind,"unused")?0:!strcmp(kind,"nested-open")?2:sites*2;
    CHECK(table->site_count==sites && table->binding_count==bindings);
    const XrXirConstruction *query=xr_xir_compile_source_snapshot_construction(source.snapshot);
    CHECK(xr_xir_compile_construction_comptime(query)->site_count==sites && xr_xir_compile_construction_comptime(query)->binding_count==bindings);
    CHECK(xr_xir_compile_artifact_verify(source.checked,NULL)==XR_XIR_OK);
    uint64_t debug_before[4],debug_after[4];program_debug_observe(source.snapshot,&source.checked->module,debug_before);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);
    XrXirSourceSnapshot *snapshot=source.snapshot;source.snapshot=NULL;
    xr_xir_compile_source_result_free(&source);XrXirArtifact *received=NULL,*closed=NULL,*lowered=NULL;
    program_debug_observe(snapshot,NULL,debug_after);
    CHECK(!memcmp(debug_before,debug_after,sizeof(debug_before)));xr_xir_compile_source_snapshot_free(snapshot);
    CHECK(xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&received,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(received,&closed,NULL, NULL)==XR_XIR_OK);
    CHECK(!xr_xir_compile_construction_comptime(closed->construction)->site_count);
    uint32_t edges=0;
    for (uint32_t f=0;f<closed->module.function_count;++f) {
        const XrXirOrigin *origin=&closed->module.provenance->origins[f];
        for (uint32_t s=0;s<sites;++s) {
            const XrXirComptimeSite *site=&received->construction->comptime.sites[s];
            if (origin->function!=site->caller) continue;
            const XrXirInstruction *op=&closed->module.functions[f].instructions[site->instruction];
            CHECK(op->op==XR_XIR_CALL || op->op==XR_XIR_INVOKE);
            uint32_t actual=closed->module.provenance->origins[op->immediate].function;bool found=false;
            for (uint32_t b=0;b<bindings;++b) if (received->construction->comptime.bindings[b].provider==actual) found=true;
            CHECK(found);++edges;
        }
    }
    CHECK(edges==(!strcmp(kind,"nested-open")?2:bindings));xr_xir_compile_artifact_free(received);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target, NULL,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    XrXirCtfeCallRequest call=import_request(lowered,"run",NULL);XrXirValue value={0};
    CHECK(xr_xir_compile_ctfe_calls(&call,&value)==XR_XIR_CTFE_OK && value.type==XR_XIR_I64 && value.payload==42);
    xr_xir_value_drop(&value);uint32_t run=named(&lowered->module,"run");XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK);xr_compile_resources_release(context.resources);
    for (unsigned i=0;i<2;++i) {
        XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_start(instance,run,NULL,0)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
        CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED && value.payload==42);
        xr_xir_value_drop(&value);CHECK(xr_xir_instance_stop(instance)==XR_XIR_CALL_READY && xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    }
    size_t attempts=source_fixture_compile_attempts;source_fixture_compile_fail_at=attempts;
    xr_xir_compile_program_drop(program);CHECK(source_fixture_compile_attempts==attempts);
    source_fixture_compile_fail_at=SIZE_MAX;CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    fprintf(stderr,"Public comptime %s sites%u bindings%u exact provider edges%u independent packet producer-dead two VM Instances42 physical0\n",kind,sites,bindings,edges);
}
static void program_controls(void) {
    static const char failure[]="fn good<T>()->Array<T>{return comptime []}\n"
        "fn bad<T>()->i64{return comptime {const v:Array<T>=[];1/len(v)}}\n"
        "export fn run()->i64{const a=good<i8>();return bad<string>()}\n";
    XrXirCompileContext context;context_new(&context);XrXirSourceResult output={0};XrXirSourceDiagnostic diagnostic={0};
    XrCompileResourceStats before=stats(&context);
    CHECK(program_source(&context,failure,&output,&diagnostic)==XR_XIR_BAD_TYPE);
    if (diagnostic.line!=2 || !strstr(diagnostic.message,"division by zero"))
        fprintf(stderr,"Public failed selection %d:%d %s\n",diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(!output.checked && !output.snapshot && diagnostic.line==2 && strstr(diagnostic.message,"division by zero"));
    CHECK(stats(&context).held_bytes==before.held_bytes);xr_compile_resources_release(context.resources);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    fprintf(stderr,"Public comptime actual failed generic selection maps to original second source line without execution retry physical0\n");
}
