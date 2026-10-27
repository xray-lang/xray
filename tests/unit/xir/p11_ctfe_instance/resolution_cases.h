/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * resolution_cases.h - Missing Source keys become ordinary verified providers
 */
#include "xir/xxir_comptime_resolve_internal.h"
static XirComptimeResolveRequest resolution_request(const XrXirArtifact *source) {
    return (XirComptimeResolveRequest){source,{XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},
        {XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,XR_XIR_CTFE_MAX_OUTPUT_BYTES},XR_XIR_CTFE_MAX_CALL_DEPTH,NULL};
}
static XrXirStatus resolution_changed_source(XrXirCompileContext *context,XrXirArtifact **output,unsigned mode) {
    bool trap=mode!=0;XrXirArtifact *source=NULL;XrXirStatus status=pending_source(context,"scalar",mode==1?1:0,&source);
    XrXirFunction *functions=NULL;XrXirInstruction *ops=NULL;
    if (status==XR_XIR_OK) {
        uint32_t helper=source->construction->comptime.sites[0].helper;
        functions=xir_compile_copy(context,source->module.functions,(size_t)source->module.function_count*sizeof(*functions),&status);
        if (status==XR_XIR_OK) ops=xir_compile_copy(context,functions[helper].instructions,(size_t)functions[helper].instruction_count*sizeof(*ops),&status);
        if (status==XR_XIR_OK) {
            functions[helper].instructions=ops;bool constant=false,add=false;
            for (uint32_t i=0;i<functions[helper].instruction_count;++i) {
                if (ops[i].op==XR_XIR_CONST_INT && ops[i].immediate==42) { ops[i].immediate=trap?0:99;constant=true; }
                if (ops[i].op==XR_XIR_ADD_INT) { if (trap) ops[i].op=XR_XIR_DIV_INT;add=true; }
            }
            CHECK(constant && add);XrXirModule changed=source->module;changed.functions=functions;
            status=xr_xir_compile_recheck_v2(context,&changed,source->construction,output,NULL);
        }
    }
    xr_compile_resources_free(ops);xr_compile_resources_free(functions);xr_xir_compile_artifact_free(source);return status;
}
static XrXirStatus resolution_input(XrXirCompileContext *context,const char *kind,XrXirArtifact **output) {
    if (!strcmp(kind,"preserve") || !strcmp(kind,"known-trap") || !strcmp(kind,"bad-trap"))
        return resolution_changed_source(context,output,!strcmp(kind,"preserve")?0:!strcmp(kind,"known-trap")?1:2);
    return pending_source(context,kind,!strcmp(kind,"nested-open")?0:2,output);
}
static XrXirStatus resolution_operation(XrXirCompileContext *context,const char *kind,XrXirArtifact **output) {
    XrXirArtifact *source=NULL;XrXirStatus status=resolution_input(context,kind,&source);
    if (status==XR_XIR_OK) {
        XirComptimeResolveRequest request=resolution_request(source);XirComptimeResolveFailure failure={0};XrXirDiagnostic diagnostic={0};
        status=xr_xir_compile_comptime_resolve(&request,output,&failure,&diagnostic);
        if (status!=XR_XIR_OK && status!=XR_XIR_BUDGET && status!=XR_XIR_OUT_OF_MEMORY)
            fprintf(stderr,"Resolution %s status%u f%u i%u CTFE%u local%u\n",kind,status,diagnostic.function,diagnostic.instruction,failure.status,failure.root_local);
    }
    xr_xir_compile_artifact_free(source);return status;
}
static void resolution_normal(const char *kind) {
    XrXirCompileContext context;context_new(&context);XrXirArtifact *source=NULL,*received=NULL,*closed=NULL,*lowered=NULL;
    XrXirStatus status=resolution_operation(&context,kind,&source);
    if (status!=XR_XIR_OK) fprintf(stderr,"Resolution %s final status%u\n",kind,status);
    CHECK(status==XR_XIR_OK && source && !source->evaluation_only);
    XirComptimePending *pending=NULL;CHECK(xr_xir_compile_comptime_pending(source,&pending,NULL)==XR_XIR_OK && !pending);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(source,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(source);CHECK(xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&received,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(received,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(received);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK && !lowered->evaluation_only);
    xr_xir_compile_artifact_free(closed);uint32_t run=named(&lowered->module,"run");XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK && !lowered);
    xr_compile_resources_release(context.resources);
    for (unsigned i=0;i<2;++i) {
        XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_start(instance,run,NULL,0)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
        XrXirValue result={0};CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED &&
            result.type==XR_XIR_I64 && result.payload==(!strcmp(kind,"preserve")?99u:42u));
        xr_xir_value_drop(&result);CHECK(xr_xir_instance_stop(instance)==XR_XIR_CALL_READY && xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    }
    size_t attempts=source_fixture_compile_attempts;source_fixture_compile_fail_at=attempts;
    xr_xir_compile_program_drop(program);CHECK(source_fixture_compile_attempts==attempts);
    source_fixture_compile_fail_at=SIZE_MAX;CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    fprintf(stderr,"Resolution %s no AST, all actual missing keys, original providers, ordinary Source packet/two runtime Instances producer-dead physical0\n",kind);
}
