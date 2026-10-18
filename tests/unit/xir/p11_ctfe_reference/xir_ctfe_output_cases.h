/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_ctfe_output_cases.h - Original multi-byte result vectors and caller bounds
 */
#ifndef XIR_CTFE_OUTPUT_CASES_H
#define XIR_CTFE_OUTPUT_CASES_H
static void reference_output_value(const XrXirValue *value,bool fibonacci) {
    if (fibonacci) {
        const int64_t expected[]={0,1,1,2,3,5,8,13,21,34};int64_t length=0;
        XrXirValueAdmission admission={.arena=xr_xir_value_arena(value),.work=UINT64_MAX,.scratch_bytes=65536};
        CHECK(xr_xir_array_len(value,&admission,&length)==XR_XIR_VALUE_OK && length==10);
        for (int64_t i=0;i<length;++i) {
            XrXirValue item={0};XrXirFaultDetail fault={0};
            CHECK(xr_xir_array_get(value,i,&admission,&item,&fault)==XR_XIR_VALUE_OK);
            CHECK(item.type==XR_XIR_I8 && !item.reserved && item.payload==expected[i]);xr_xir_value_drop(&item);
        }
    } else {
        for (uint32_t i=0;i<2;++i) {
            XrXirValue item={0};CHECK(xr_xir_tuple_get(value,i,&item)==XR_XIR_VALUE_OK);
            CHECK(item.type==XR_XIR_I64 && !item.reserved && item.payload==21);xr_xir_value_drop(&item);
        }
    }
}
static void reference_output_fault(const XrXirCheckedPacket *wire,const uint8_t *identity,size_t fault,size_t sites,bool fibonacci,uint64_t logical) {
    size_t held=source_fixture_compile_live,bytes=source_fixture_compile_bytes;
    XrXirCompileContext context={0};CHECK(xr_compile_resources_new(&reference_limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();XrXirArtifact *closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_read(&context,wire->bytes,wire->length,&closed,NULL)==XR_XIR_OK);
    uint8_t digest[32]={0};CHECK(xr_compile_resources_work(context.resources,wire->length)==XR_COMPILE_RESOURCE_OK);
    xr_sha256(wire->bytes,wire->length,digest);CHECK(!memcmp(digest,identity,32));
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    XrXirCtfeRequest request={.artifact=lowered,.function=reference_run_function(xr_xir_compile_artifact_module(lowered)),.target=target,
        .limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,logical}};
    memcpy(request.checked_identity,identity,32);XrXirValue output={0};XrCompileResources *same=context.resources;
    XrCompileResourceStats baseline=reference_stats(&context);size_t start=source_fixture_compile_attempts;
    source_fixture_compile_injected=false;source_fixture_compile_fail_at=start+fault;
    CHECK(reference_text_evaluate(&request,true,&output)==XR_XIR_CTFE_OUT_OF_MEMORY);
    CHECK(source_fixture_compile_injected && source_fixture_compile_attempts==start+fault+1 && !output.type && !output.payload);
    CHECK(reference_stats(&context).live_bytes==baseline.live_bytes);source_fixture_compile_fail_at=SIZE_MAX;
    XrCompileResourceStats failed=reference_stats(&context);start=source_fixture_compile_attempts;
    CHECK(reference_text_evaluate(&request,true,&output)==XR_XIR_CTFE_OK);reference_output_value(&output,fibonacci);
    CHECK(source_fixture_compile_attempts-start==sites);xr_xir_value_drop(&output);XrCompileResourceStats after=reference_stats(&context);
    CHECK(context.resources==same && after.live_bytes==baseline.live_bytes && after.work>=failed.work && after.allocated_bytes>=failed.allocated_bytes);
    CHECK(after.work<=reference_limits.work && after.allocated_bytes<=reference_limits.allocated_bytes && after.peak_bytes<=reference_limits.live_bytes);
    xr_xir_compile_artifact_free(lowered);xr_compile_resources_release(context.resources);
    CHECK(source_fixture_compile_live==held && source_fixture_compile_bytes==bytes);
}
static void reference_output(const XrXirCompileContext *context,XrXirSourceResult *source,const CtfeReferenceCase *selected) {
    bool fibonacci=strstr(selected->name,"fibonacci")!=NULL,direct=strstr(selected->name,"direct")!=NULL;
    XrXirCheckedPacket wire={0};
    if (direct) {
        XrXirArtifact *closed=NULL;CHECK(xr_xir_compile_specialize(source->checked,&closed,NULL)==XR_XIR_OK);
        CHECK(xr_xir_compile_checked_write(closed,&wire,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    }
    uint8_t identity[32]={0};XrXirArtifact *lowered=reference_lower(context,source,identity);XrXirValue output={0};
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);uint32_t run=reference_run_function(module);
    if (!direct) for (uint32_t i=0;i<module->functions[run].instruction_count;++i)
        CHECK(module->functions[run].instructions[i].op!=XR_XIR_CALL && module->functions[run].instructions[i].op!=XR_XIR_CALL_INDIRECT);
    XrXirCtfeRequest request={.artifact=lowered,.function=run,.target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},
        .limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,8}};
    memcpy(request.checked_identity,identity,32);XrCompileResourceStats baseline=reference_stats(context);
    CHECK(reference_text_evaluate(&request,true,&output)==XR_XIR_CTFE_OUTPUT_LIMIT && !output.type && !output.payload);
    request.limits.output_bytes=(uint64_t)selected->value;
    size_t start=source_fixture_compile_attempts;XrXirCtfeStatus status=reference_text_evaluate(&request,true,&output);
    size_t sites=source_fixture_compile_attempts-start;CHECK(sites);
    fprintf(stderr,"CTFE original output vector %s logical=%lld status=%u\n",selected->name,(long long)selected->value,status);
    CHECK(status==XR_XIR_CTFE_OK);reference_output_value(&output,fibonacci);xr_xir_value_drop(&output);
    if (direct) {
        for (size_t fault=0;fault<sites;++fault) reference_output_fault(&wire,identity,fault,sites,fibonacci,(uint64_t)selected->value);
        fprintf(stderr,"CTFE original output %s FI=%zu complete per-job same-owner retry, all actual values preserved physical0\n",selected->name,sites);
    }
    request.limits.output_bytes=(uint64_t)selected->value-1;
    CHECK(reference_text_evaluate(&request,true,&output)==XR_XIR_CTFE_OUTPUT_LIMIT && !output.type && !output.payload);
    request.limits.output_bytes=XR_XIR_CTFE_MAX_OUTPUT_BYTES;
    CHECK(XR_XIR_CTFE_OUTPUT_POLICY_VERSION==2 && request.limits.output_bytes==UINT64_C(8388608));
    CHECK(reference_text_evaluate(&request,true,&output)==XR_XIR_CTFE_OK);reference_output_value(&output,fibonacci);xr_xir_value_drop(&output);
    request.limits.output_bytes=XR_XIR_CTFE_MAX_OUTPUT_BYTES+1;XrCompileResourceStats before=reference_stats(context);
    CHECK(reference_text_evaluate(&request,false,&output)==XR_XIR_CTFE_BAD_ARGUMENT && !output.type && !output.payload);
    CHECK(reference_text_evaluate(&request,true,&output)==XR_XIR_CTFE_BAD_ARGUMENT && !output.type && !output.payload);
    XrCompileResourceStats after=reference_stats(context);CHECK(!memcmp(&before,&after,sizeof(before)));
    request.limits.output_bytes=(uint64_t)selected->value;request.limits.steps=1;
    CHECK(reference_text_evaluate(&request,true,&output)==XR_XIR_CTFE_STEP_LIMIT && !output.type && !output.payload);
    request.limits.steps=XR_XIR_CTFE_MAX_STEPS;request.limits.frame_bytes=0;
    CHECK(reference_text_evaluate(&request,true,&output)==XR_XIR_CTFE_FRAME_LIMIT && !output.type && !output.payload);
    request.limits.frame_bytes=XR_XIR_CTFE_MAX_FRAME_BYTES;
    if (direct) {
        XrXirCtfeCallRequest shallow={request,1};
        CHECK(xr_xir_compile_ctfe_calls(&shallow,&output)==XR_XIR_CTFE_DEPTH_LIMIT && !output.type && !output.payload);
    }
    CHECK(reference_stats(context).live_bytes==baseline.live_bytes);xr_xir_compile_artifact_free(lowered);xr_xir_compile_checked_packet_free(&wire);
}
static void reference_output_last_owner(const CtfeReferenceCase *selected) {
    XrXirCompileContext context={0};CHECK(xr_compile_resources_new(&reference_limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    CHECK(reference_source(&context,selected->name,&source,&diagnostic)==XR_XIR_OK);
    uint8_t identity[32]={0};XrXirArtifact *lowered=reference_lower(&context,&source,identity);xr_xir_compile_source_result_free(&source);
    XrXirCtfeRequest request={.artifact=lowered,.function=reference_run_function(xr_xir_compile_artifact_module(lowered)),
        .target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},.limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,(uint64_t)selected->value}};
    memcpy(request.checked_identity,identity,32);XrXirValue output={0};CHECK(reference_text_evaluate(&request,true,&output)==XR_XIR_CTFE_OK);
    xr_xir_compile_artifact_free(lowered);xr_compile_resources_release(context.resources);reference_output_value(&output,strstr(selected->name,"fibonacci")!=NULL);
    size_t attempts=source_fixture_compile_attempts;source_fixture_compile_fail_at=attempts;xr_xir_value_drop(&output);source_fixture_compile_fail_at=SIZE_MAX;
    CHECK(source_fixture_compile_attempts==attempts && !source_fixture_compile_live && !source_fixture_compile_bytes);
    fprintf(stderr,"CTFE original output %s survives Source/Artifact/ledger caller death; final drop allocates nothing physical0\n",selected->name);
}
#endif // XIR_CTFE_OUTPUT_CASES_H
