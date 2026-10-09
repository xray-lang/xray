/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_ctfe_leaf_chain.h - Fallible same-owner Source scalar producer
 *
 * KEY CONCEPT:
 *   The complete producer uses its caller's original finite ledger and publishes
 *   only after both packet round trips and the full Lowered verification succeed.
 */
#ifndef XIR_CTFE_LEAF_CHAIN_H
#define XIR_CTFE_LEAF_CHAIN_H
typedef enum CtfeChainPhase {
    CTFE_CHAIN_LEDGER, CTFE_CHAIN_SESSION, CTFE_CHAIN_SOURCE,
    CTFE_CHAIN_WRITE1, CTFE_CHAIN_READ1, CTFE_CHAIN_VERIFY1,
    CTFE_CHAIN_WRITE2, CTFE_CHAIN_READ2, CTFE_CHAIN_VERIFY2,
    CTFE_CHAIN_SPECIALIZE, CTFE_CHAIN_CLOSED_VERIFY, CTFE_CHAIN_CLOSED_WRITE,
    CTFE_CHAIN_HASH, CTFE_CHAIN_LOWER, CTFE_CHAIN_LOWER_VERIFY,
    CTFE_CHAIN_EVALUATE, CTFE_CHAIN_CLEANUP, CTFE_CHAIN_PHASE_COUNT
} CtfeChainPhase;
static const char *const ctfe_chain_names[CTFE_CHAIN_PHASE_COUNT] = {
    "ledger", "session", "Source", "write1", "read1", "verify1",
    "write2", "read2", "verify2", "specialize", "closed verify", "closed write",
    "closed hash", "lower", "Lowered verify", "CTFE", "cleanup"
};
typedef struct CtfeChainTrace {
    size_t allocations[CTFE_CHAIN_PHASE_COUNT], start;
    uint32_t visited;
    CtfeChainPhase phase;
    bool active;
} CtfeChainTrace;
static void ctfe_chain_mark(CtfeChainTrace *trace, CtfeChainPhase phase) {
    CHECK((unsigned)phase < CTFE_CHAIN_PHASE_COUNT);
    source_fixture_compile_phase = ctfe_chain_names[phase];
    if (!trace) return;
    if (trace->active) trace->allocations[trace->phase] += source_fixture_compile_attempts - trace->start;
    trace->phase = phase; trace->start = source_fixture_compile_attempts;
    trace->active = true; trace->visited |= UINT32_C(1) << phase;
}
static void ctfe_chain_trace_end(CtfeChainTrace *trace) {
    if (!trace || !trace->active) return;
    trace->allocations[trace->phase] += source_fixture_compile_attempts - trace->start;
    trace->active = false;
}
static XrXirStatus ctfe_session_status(XrCompilerSessionStatus status) {
    if (status == XR_COMPILER_SESSION_OK) return XR_XIR_OK;
    if (status == XR_COMPILER_SESSION_BUDGET) return XR_XIR_BUDGET;
    if (status == XR_COMPILER_SESSION_OUT_OF_MEMORY) return XR_XIR_OUT_OF_MEMORY;
    return XR_XIR_BAD_STRUCTURE;
}
static XrXirStatus ctfe_fixture_build(const XrXirCompileContext *context,
    CtfeFixture *output, CtfeChainTrace *trace) {
    if (!context || !context->resources || !output || output->context.resources || output->lowered)
        return XR_XIR_BAD_STRUCTURE;
    CtfeFixture result = {0}; result.context = *context;
    CHECK(xr_compile_resources_stats(context->resources, &result.owner_baseline) == XR_COMPILE_RESOURCE_OK);
    XrCompilerSession *session = NULL;
    XrXirArtifact *checked = NULL, *copy = NULL, *closed = NULL;
    XrXirCheckedPacket packet = {0}; XrXirSourceResult source = {0};
    XrXirSourceDiagnostic diagnostic = {0}; XrXirStatus status;
    char producer[sizeof(ctfe_source)]; memcpy(producer, ctfe_source, sizeof(producer));
    char memory_name[] = "closed-scalar-leaf";
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_MEMORY, memory_name, NULL};
    XrXirSourceRequest request = {NULL, NULL, &authority, context, NULL, NULL, XR_XIR_PROGRAM, NULL};
    XrXirSourceText text = {NULL, producer, sizeof(producer)-1};
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    ctfe_chain_mark(trace, CTFE_CHAIN_SESSION);
    status = ctfe_session_status(xr_compile_session_new(context->resources, &session));
    if (status != XR_XIR_OK) { CHECK(!session); goto done; }
    CHECK(session && xr_compile_session_resources(session) == context->resources);
    request.session = session;
    ctfe_chain_mark(trace, CTFE_CHAIN_SOURCE);
    status = xr_xir_compile_source_check_text(&request, &text, &source, &diagnostic, NULL);
    xr_compile_session_free(session); session = NULL;
    memset(producer, 0xa5, sizeof(producer)); memset(memory_name, 0xa5, sizeof(memory_name));
    authority = (XrModuleIdentityAuthority){0}; request = (XrXirSourceRequest){0};
    text = (XrXirSourceText){0};
    if (status != XR_XIR_OK) {
        if (status != XR_XIR_BUDGET && status != XR_XIR_OUT_OF_MEMORY)
            fprintf(stderr, "Source CTFE producer status%u at%d:%d %s\n", (unsigned)status,
                diagnostic.line, diagnostic.column, diagnostic.message);
        CHECK(!source.checked && !source.snapshot); goto done;
    }
    CHECK(source.checked && source.snapshot);
    CHECK(xr_xir_compile_artifact_context(source.checked)->resources == context->resources);
    checked = source.checked; source.checked = NULL; xr_xir_compile_source_result_free(&source);
    for (uint32_t pass = 0; pass < 2; ++pass) {
        ctfe_chain_mark(trace, pass ? CTFE_CHAIN_WRITE2 : CTFE_CHAIN_WRITE1);
        status = xr_xir_compile_checked_write(checked, &packet, NULL);
        if (status != XR_XIR_OK) { CHECK(!packet.bytes && !packet.length); goto done; }
        xr_xir_compile_artifact_free(checked); checked = NULL;
        ctfe_chain_mark(trace, pass ? CTFE_CHAIN_READ2 : CTFE_CHAIN_READ1);
        status = xr_xir_compile_checked_read(context, packet.bytes, packet.length, &copy, NULL);
        memset(packet.bytes, 0xa5, packet.length); xr_xir_compile_checked_packet_free(&packet);
        if (status != XR_XIR_OK) { CHECK(!copy); goto done; }
        CHECK(copy && xr_xir_compile_artifact_context(copy)->resources == context->resources);
        ctfe_chain_mark(trace, pass ? CTFE_CHAIN_VERIFY2 : CTFE_CHAIN_VERIFY1);
        status = xr_xir_compile_artifact_verify(copy, NULL);
        if (status != XR_XIR_OK) goto done;
        checked = copy; copy = NULL;
    }
    ctfe_chain_mark(trace, CTFE_CHAIN_SPECIALIZE);
    status = xr_xir_compile_specialize(checked, &closed, NULL);
    xr_xir_compile_artifact_free(checked); checked = NULL;
    if (status != XR_XIR_OK) { CHECK(!closed); goto done; }
    CHECK(closed && xr_xir_compile_artifact_context(closed)->resources == context->resources);
    ctfe_chain_mark(trace, CTFE_CHAIN_CLOSED_VERIFY);
    status = xr_xir_compile_artifact_verify(closed, NULL);
    if (status != XR_XIR_OK) goto done;
    ctfe_chain_mark(trace, CTFE_CHAIN_CLOSED_WRITE);
    status = xr_xir_compile_checked_write(closed, &packet, NULL);
    if (status != XR_XIR_OK) { CHECK(!packet.bytes && !packet.length); goto done; }
    ctfe_chain_mark(trace, CTFE_CHAIN_HASH);
    if (xr_compile_resources_work(context->resources, packet.length) != XR_COMPILE_RESOURCE_OK) {
        status = XR_XIR_BUDGET; goto done;
    }
    xr_sha256(packet.bytes, packet.length, result.identity);
    memset(packet.bytes, 0xa5, packet.length); xr_xir_compile_checked_packet_free(&packet);
    ctfe_chain_mark(trace, CTFE_CHAIN_LOWER);
    status = xr_xir_compile_lower(closed, &target, &result.lowered, NULL);
    xr_xir_compile_artifact_free(closed); closed = NULL;
    if (status != XR_XIR_OK) { CHECK(!result.lowered); goto done; }
    CHECK(result.lowered && xr_xir_compile_artifact_context(result.lowered)->resources == context->resources);
    ctfe_chain_mark(trace, CTFE_CHAIN_LOWER_VERIFY);
    status = xr_xir_compile_artifact_verify(result.lowered, NULL);
    if (status == XR_XIR_OK) { *output = result; result.lowered = NULL; }
done:
    ctfe_chain_mark(trace, CTFE_CHAIN_CLEANUP);
    xr_compile_session_free(session); xr_xir_compile_source_result_free(&source);
    xr_xir_compile_checked_packet_free(&packet);
    xr_xir_compile_artifact_free(checked); xr_xir_compile_artifact_free(copy);
    xr_xir_compile_artifact_free(closed); xr_xir_compile_artifact_free(result.lowered);
    return status;
}
#endif // XIR_CTFE_LEAF_CHAIN_H
