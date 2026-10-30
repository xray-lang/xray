/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_ctfe_calls_chain.h - Fallible same-owner Source scalar producer
 *
 * KEY CONCEPT:
 *   The complete producer uses its caller's original finite ledger and publishes
 *   only after both packet round trips and the full Lowered verification succeed.
 */
#ifndef XIR_CTFE_CALLS_CHAIN_H
#define XIR_CTFE_CALLS_CHAIN_H
static const char ctfe_calls_source[] =
    "fn leaf(x:i64)->i64 { return x+1 }\n"
    "fn bool_local(value:i64)->bool { return value==40 && value<42 }\n"
    "export fn logic(value:i64)->i64 { return bool_local(value) ? 42 : 1 }\n"
    "fn middle(x:i64)->i64 { return leaf(x)+1 }\n"
    "export fn answer()->i64 { return middle(40) }\n"
    "export fn recurse(n:i64)->i64 { return n==0 ? 41 : recurse(n-1) }\n"
    "export fn endless()->i64 { return endless() }\n"
    "fn quotient(x:i64)->i64 { return 41/x }\n"
    "export fn division()->i64 { return quotient(0) }\n"
    "fn io()->i64 { print(1); return 1 }\n"
    "export fn unsupported()->i64 { return io() }\n"
    "export fn deferred()->i64 { defer { print(1) }; return 1 }\n"
    "export fn managed()->string { return \"owned\" }\n"
    "export fn choose(flag:bool)->i64 { return flag ? middle(40) : leaf(0) }\n"
    "export fn loop(flag:bool)->i64 { while(flag) {}; return 1 }\n"
    "export fn compare(flag:bool,x:i64,y:i64)->i64 { return flag!=false ? (x==y ? 41 : 0) : (x!=y ? 41 : 0) }\n"
    "export fn unsigned_equal(x:u64,y:u64)->i64 { return x==y ? 41 : 0 }\n"
    "export fn managed_equal(value:string)->i64 { return value==value ? 41 : 0 }\n";

static XrXirStatus ctfe_calls_fixture_build(const XrXirCompileContext *context,
    CtfeFixture *output, CtfeChainTrace *trace) {
    if (!context || !context->resources || !output || output->context.resources || output->lowered)
        return XR_XIR_BAD_STRUCTURE;
    CtfeFixture result = {0}; result.context = *context;
    CHECK(xr_compile_resources_stats(context->resources, &result.owner_baseline) == XR_COMPILE_RESOURCE_OK);
    XrCompilerSession *session = NULL;
    XrXirArtifact *checked = NULL, *copy = NULL, *closed = NULL;
    XrXirCheckedPacket packet = {0}; XrXirSourceResult source = {0};
    XrXirSourceDiagnostic diagnostic = {0}; XrXirStatus status;
    char producer[sizeof(ctfe_calls_source)]; memcpy(producer, ctfe_calls_source, sizeof(producer));
    char memory_name[] = "closed-scalar-calls";
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
    status = xr_xir_compile_specialize(checked, &closed, NULL, NULL);
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
    status = xr_xir_compile_lower(closed, &target, NULL, &result.lowered, NULL);
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
#endif // XIR_CTFE_CALLS_CHAIN_H
