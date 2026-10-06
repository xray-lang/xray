/* Same owner and ledger across Source, codec, specialization, lower and emit. */
#ifndef GO_BOOL_SOURCE_PIPELINE_H
#define GO_BOOL_SOURCE_PIPELINE_H
static XrXirArtifact *go_bool_source_lower(const XrXirCompileContext *context, uint32_t entries[3]) {
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(context->resources, &session) == XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, XR_GO_BOOL_FIXTURES};
    XrXirSourceRequest request = {session, XR_GO_BOOL_FIXTURES "/bool_native.xr", &authority,
        context, XR_SOURCE_STDLIB, NULL, XR_XIR_PROGRAM, NULL};
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic source_diagnostic = {0};
    XrXirDiagnostic diagnostic = {0}; char *failure = NULL;
    XrXirArtifact *decoded = NULL, *closed = NULL, *lowered = NULL;
    XrXirCheckedPacket packet = {0};
    XrXirStatus status = xr_xir_compile_source_check(&request, &result, &source_diagnostic, &failure);
    xr_compile_session_free(session);
    if (status != XR_XIR_OK) fprintf(stderr, "go_bool check=%u %s\n", status, source_diagnostic.message);
    CHECK(status == XR_XIR_OK && result.checked && !failure);
    CHECK(xr_xir_compile_checked_write(result.checked, &packet, &diagnostic) == XR_XIR_OK);
    xr_xir_compile_source_result_free(&result);
    CHECK(xr_xir_compile_checked_read(context, packet.bytes, packet.length, &decoded, &diagnostic) == XR_XIR_OK);
    memset(packet.bytes, 0xa5, packet.length); xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(decoded, &closed, &diagnostic) == XR_XIR_OK);
    xr_xir_compile_artifact_free(decoded);
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    status = xr_xir_compile_lower(closed, &target, &lowered, &diagnostic);
    xr_xir_compile_artifact_free(closed);
    if (status != XR_XIR_OK) fprintf(stderr, "go_bool lower=%u f=%u op=%u\n", status,
        diagnostic.function, diagnostic.instruction);
    CHECK(status == XR_XIR_OK);
    const XrXirModule *module = xr_xir_compile_artifact_module(lowered);
    const char *names[3] = {"main", "escaped", "parameter"}; unsigned matches[3] = {0};
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *function = &module->functions[f];
        const XrXirFunctionIdentity *identity = &module->declarations->functions[f];
        if (!identity->exported || identity->module != module->declarations->root_module || identity->nominal_owner) continue;
        for (unsigned n = 0; n < 3; ++n) if (function->name_length == strlen(names[n]) &&
            !memcmp(function->name, names[n], function->name_length)) {
            CHECK(function->parameter_count == (n == 2 ? 1u : 0u)); entries[n] = f; ++matches[n];
        }
    }
    CHECK(matches[0] == 1 && matches[1] == 1 && matches[2] == 1); return lowered;
}
#endif
