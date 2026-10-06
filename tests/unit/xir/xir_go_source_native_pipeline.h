/* Owned Source pipeline; decoded input and every derived stage share one ledger. */
static XrXirArtifact *source17_lower(const XrXirCompileContext *context,
    const char *name, uint32_t *entry) {
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(context->resources, &session) == XR_COMPILER_SESSION_OK);
    char path[1024];
    CHECK(snprintf(path, sizeof(path), "%s/%s.xr", XR_GO_EXECUTION_FIXTURES, name) > 0);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, XR_GO_EXECUTION_FIXTURES};
    XrXirSourceRequest request = {session, path, &authority, context, XR_SOURCE_STDLIB, NULL, XR_XIR_PROGRAM, NULL};
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic source_diagnostic = {0};
    XrXirDiagnostic diagnostic = {0}; char *failure = NULL;
    XrXirArtifact *decoded = NULL, *closed = NULL, *lowered = NULL;
    XrXirCheckedPacket packet = {0};
    XrXirStatus status = xr_xir_compile_source_check(&request, &result, &source_diagnostic, &failure);
    xr_compile_session_free(session);
    if (status != XR_XIR_OK) fprintf(stderr, "%s check=%u %s\n", name, status, source_diagnostic.message);
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
    if (status != XR_XIR_OK) fprintf(stderr, "%s lower=%u f=%u op=%u\n", name,
        status, diagnostic.function, diagnostic.instruction);
    CHECK(status == XR_XIR_OK);
    const XrXirModule *module = xr_xir_compile_artifact_module(lowered);
    unsigned matches = 0;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *function = &module->functions[f];
        const XrXirFunctionIdentity *identity = &module->declarations->functions[f];
        if (function->name_length == 4 && !memcmp(function->name, "main", 4) && identity->exported &&
            identity->module == module->declarations->root_module && !identity->nominal_owner) {
            CHECK(!function->parameter_count); *entry = f; ++matches;
        }
    }
    CHECK(matches == 1);
    return lowered;
}
