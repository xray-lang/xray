/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_ctfe_panic_abort_cases.h - Uncaught language panics and finite cleanup
 */
#ifndef XIR_CTFE_PANIC_ABORT_CASES_H
#define XIR_CTFE_PANIC_ABORT_CASES_H
static void reference_panic_uncaught_fault(const XrXirCheckedPacket *wire,const uint8_t *identity,size_t fault,size_t sites,XrXirCtfeStatus expected) {
    size_t held=source_fixture_compile_live,bytes=source_fixture_compile_bytes;
    XrXirCompileContext context={0};CHECK(xr_compile_resources_new(&reference_limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();XrXirArtifact *received=NULL,*closed=NULL,*lowered=NULL;XrXirCheckedPacket packet={0};
    CHECK(xr_xir_compile_checked_read(&context,wire->bytes,wire->length,&received,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_specialize(received,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(received);
    CHECK(xr_xir_compile_checked_write(closed,&packet,NULL)==XR_XIR_OK);uint8_t digest[32]={0};
    CHECK(xr_compile_resources_work(context.resources,packet.length)==XR_COMPILE_RESOURCE_OK);xr_sha256(packet.bytes,packet.length,digest);
    CHECK(!memcmp(digest,identity,sizeof(digest)));xr_xir_compile_checked_packet_free(&packet);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target, NULL,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    CHECK(xr_xir_compile_artifact_verify(lowered,NULL)==XR_XIR_OK);
    XrXirCtfeRequest request={.artifact=lowered,.function=reference_named(xr_xir_compile_artifact_module(lowered),"unhandled"),.target=target,
        .limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,8}};
    memcpy(request.checked_identity,identity,32);XrXirValue output={0};XrCompileResources *same=context.resources;
    XrCompileResourceStats baseline=reference_stats(&context);size_t start=source_fixture_compile_attempts;
    source_fixture_compile_injected=false;source_fixture_compile_fail_at=start+fault;
    CHECK(reference_text_evaluate(&request,true,&output)==XR_XIR_CTFE_OUT_OF_MEMORY);
    CHECK(source_fixture_compile_injected && source_fixture_compile_attempts==start+fault+1 && !output.type && !output.payload);
    CHECK(reference_stats(&context).live_bytes==baseline.live_bytes);source_fixture_compile_fail_at=SIZE_MAX;
    XrCompileResourceStats failed=reference_stats(&context);start=source_fixture_compile_attempts;
    CHECK(reference_text_evaluate(&request,true,&output)==expected && !output.type && !output.payload);
    CHECK(source_fixture_compile_attempts-start==sites);XrCompileResourceStats after=reference_stats(&context);
    CHECK(context.resources==same && after.live_bytes==baseline.live_bytes && after.work>=failed.work && after.allocated_bytes>=failed.allocated_bytes);
    CHECK(after.work<=reference_limits.work && after.allocated_bytes<=reference_limits.allocated_bytes && after.peak_bytes<=reference_limits.live_bytes);
    xr_xir_compile_artifact_free(lowered);xr_compile_resources_release(context.resources);
    CHECK(source_fixture_compile_live==held && source_fixture_compile_bytes==bytes);
}
static void reference_panic_abort(const XrXirCompileContext *context,XrXirSourceResult *source,bool escaped) {
    XrXirCtfeStatus expected=escaped?XR_XIR_CTFE_CLEANUP_PANIC:XR_XIR_CTFE_ASSERTION_FAILURE;
    XrXirCheckedPacket wire={0};CHECK(xr_xir_compile_checked_write(source->checked,&wire,NULL)==XR_XIR_OK);
    uint8_t identity[32]={0};XrXirArtifact *lowered=reference_lower(context,source,identity);XrXirValue output={0};
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    XrXirCtfeRequest request={.artifact=lowered,.function=reference_named(module,"unhandled"),
        .target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},.limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,8}};
    memcpy(request.checked_identity,identity,32);XrCompileResourceStats baseline=reference_stats(context);size_t start=source_fixture_compile_attempts;
    CHECK(reference_text_evaluate(&request,true,&output)==expected && !output.type && !output.payload);
    size_t sites=source_fixture_compile_attempts-start;CHECK(sites);
    for (size_t fault=0;fault<sites;++fault) reference_panic_uncaught_fault(&wire,identity,fault,sites,expected);
    if (!escaped) {
        request.function=reference_run_function(module);
        CHECK(reference_text_evaluate(&request,true,&output)==XR_XIR_CTFE_STEP_LIMIT && !output.type && !output.payload);
        CHECK(reference_stats(context).live_bytes==baseline.live_bytes);request.limits.steps=1;
        CHECK(reference_text_evaluate(&request,true,&output)==XR_XIR_CTFE_STEP_LIMIT && !output.type && !output.payload);
        CHECK(reference_stats(context).live_bytes==baseline.live_bytes);
        fprintf(stderr,"CTFE actual infinite panic cleanup preserves STEP_LIMIT before uncaught publication; retry step1 physical0\n");
    }
    xr_xir_compile_artifact_free(lowered);xr_xir_compile_checked_packet_free(&wire);
    fprintf(stderr,"CTFE uncaught or escaping cleanup panic: FI=%zu complete per-job same-owner retry status=%u/live; host remains live, physical0\n",sites,(unsigned)expected);
}
#endif // XIR_CTFE_PANIC_ABORT_CASES_H
