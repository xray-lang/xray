/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 */
#ifndef XIR_TASK_UNIT_NATIVE_PIPELINE_H
#define XIR_TASK_UNIT_NATIVE_PIPELINE_H
static void unit_native_shape(const XrXirModule *module) {
    unsigned tasks = 0; CHECK(module->types);
    for (uint32_t t = 0; t < module->types->count; ++t) {
        const XrXirTypeNode *node = &module->types->nodes[t];
        if (node->kind == XR_XIR_TYPE_TASK) { CHECK(node->element == XR_XIR_UNIT && !node->parameter_span); ++tasks; }
    }
    CHECK(tasks == 1);
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *function = &module->functions[f];
        for (uint32_t i = 0; i < function->instruction_count; ++i) if (function->instructions[i].op == XR_XIR_TASK_AWAIT) {
            const XrXirInstruction *op = &function->instructions[i];
            const XrXirTypeNode *node = xr_xir_type_node(module->types, xr_xir_operand_type(function, op->args[0]));
            CHECK(node && node->kind == XR_XIR_TYPE_TASK && node->element == XR_XIR_UNIT);
            const XrXirInstruction *first = &function->instructions[function->blocks[op->targets[0]].first];
            CHECK(first->op != XR_XIR_INVOKE_RESULT && first->op != XR_XIR_INVOKE_ERROR);
        }
    }
}
static XrXirArtifact *unit_native_lower(const UnitNativeOracle *oracle, uint32_t *entry) {
    const XrXirCompileContext *context = effects_source_owner(UINT64_C(67108864), UINT64_C(128000000));
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(context->resources, &session) == XR_COMPILER_SESSION_OK);
    char path[1024]; CHECK(snprintf(path, sizeof(path), "%s/%s.xr", XR_TASK_UNIT_FIXTURES, oracle->name) > 0);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, XR_TASK_UNIT_FIXTURES};
    XrXirSourceRequest request = {session, path, &authority, context, XR_SOURCE_STDLIB, NULL, XR_XIR_PROGRAM, NULL};
    XrXirSourceResult source = {0}; XrXirSourceDiagnostic source_diagnostic = {0}; char *failure = NULL;
    XrXirDiagnostic diagnostic = {0}; XrXirCheckedPacket packet = {0};
    XrXirArtifact *read = NULL, *closed = NULL, *lowered = NULL;
    XrXirStatus status = xr_xir_compile_source_check(&request, &source, &source_diagnostic, &failure);
    xr_compile_session_free(session);
    if (status != XR_XIR_OK) fprintf(stderr, "%s check=%u %s\n", oracle->name, status, source_diagnostic.message);
    CHECK(status == XR_XIR_OK && source.checked && !failure);
    unit_native_shape(xr_xir_compile_artifact_module(source.checked));
    CHECK(xr_xir_compile_checked_write(source.checked, &packet, &diagnostic) == XR_XIR_OK);
    xr_xir_compile_source_result_free(&source);
    CHECK(xr_xir_compile_checked_read(context, packet.bytes, packet.length, &read, &diagnostic) == XR_XIR_OK);
    memset(packet.bytes, 0xa5, packet.length); xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(read, &closed, &diagnostic) == XR_XIR_OK);
    xr_xir_compile_artifact_free(read);
    CHECK(xr_xir_compile_artifact_verify(closed, &diagnostic) == XR_XIR_OK);
    unit_native_shape(xr_xir_compile_artifact_module(closed));
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    status = xr_xir_compile_lower(closed, &target, &lowered, &diagnostic);
    xr_xir_compile_artifact_free(closed);
    if (status != XR_XIR_OK) fprintf(stderr, "%s lower=%u function=%u op=%u\n", oracle->name, status, diagnostic.function, diagnostic.instruction);
    CHECK(status == XR_XIR_OK && lowered);
    const XrXirModule *module = xr_xir_compile_artifact_module(lowered); unit_native_shape(module);
    unsigned matches = 0; size_t length = strlen(oracle->entry);
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *function = &module->functions[f];
        const XrXirFunctionIdentity *identity = &module->declarations->functions[f];
        if (identity->exported && !identity->nominal_owner && identity->module == module->declarations->root_module &&
            function->name_length == length && !memcmp(function->name, oracle->entry, length)) {
            CHECK(!function->parameter_count); *entry = f; ++matches;
        }
    }
    CHECK(matches == 1); return lowered;
}
#endif
