/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_method_promises.h - Nominal promise selection and receiver binding
 */
#ifndef XIR_SOURCE_METHOD_PROMISES_H
#define XIR_SOURCE_METHOD_PROMISES_H
static void source_method_promise_execute(XrXirArtifact *checked, const char *output) {
    const XrXirCompileContext context=*xr_xir_compile_artifact_context(checked);
    XrXirCheckedPacket packet = {0}; XrXirArtifact *copy = NULL, *closed = NULL, *lowered = NULL;
    CHECK(xr_xir_compile_checked_write(checked, &packet, NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_checked_read(&context, packet.bytes, packet.length, &copy, NULL) == XR_XIR_OK);
    xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(copy, &closed, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(copy);
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed, &target, &lowered, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);
    const char *names[] = {"boundUse", "staticUse", "callbackUse"};
    const int64_t expected[] = {11, 13, 36}; uint32_t entries[3] = {UINT32_MAX, UINT32_MAX, UINT32_MAX};
    const XrXirModule *module = xr_xir_compile_artifact_module(lowered);
    for (uint32_t f = 0; f < module->function_count; ++f)
        for (uint32_t i = 0; i < 3; ++i)
            if (strlen(names[i]) == module->functions[f].name_length &&
                !memcmp(names[i], module->functions[f].name, module->functions[f].name_length)) entries[i] = f;
    if (output) {
        XrXirCSource source = {0};
        CHECK(xr_xir_compile_emit_c(lowered, "method_source", 4194304, &source) == XR_XIR_OK);
        CHECK(!strstr(source.text, "({") && !strstr(source.text, "xr_xir_vm"));
        FILE *file = fopen(output, "ab"); CHECK(file);
        CHECK(fwrite(source.text, 1, source.length, file) == source.length);
        CHECK(fprintf(file, "\nconst uint32_t method_source_selected_entries[3] = {%uu,%uu,%uu};\n",
            entries[0], entries[1], entries[2]) > 0);
        CHECK(fclose(file) == 0); xr_xir_compile_c_source_free(&source);
    }
    XrXirProgram *program = NULL;
    CHECK(xr_xir_compile_vm_program_take(&lowered, &program) == XR_XIR_OK);
    XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY); XrXirInstance *instance = NULL;
    CHECK(xr_xir_instance_new(program, &config, &instance) == XR_XIR_CALL_READY);
    for (uint32_t i = 0; i < 3; ++i) {
        CHECK(entries[i] != UINT32_MAX);
        CHECK(xr_xir_instance_start(instance, entries[i], NULL, 0) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status == XR_XIR_CALL_RETURNED);
        XrXirValue value = {0};
        CHECK(xr_xir_instance_take_result(instance, &value) == XR_XIR_CALL_RETURNED);
        CHECK(value.type == XR_XIR_I64 && value.payload == expected[i]); xr_xir_value_drop(&value);
    }
    CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY); xr_xir_compile_program_drop(program);
}
static void source_method_promise_cases(const XrXirSourceRequest *request, const char *output) {
    XrXirSourceRequest probe = *request; char path[8192];
    source_fixture_path(request, "methods.xr", path); probe.entry_path = path;
    SourceTestDeclaration records[] = {
        {"get", "Box", NULL, true}, {"identity", "Box", NULL, true},
        {"invoke", NULL, "f", false}, {"invokeStatic", NULL, "f", false},
        {"call", "Box", "f", true}, {"callStatic", "Box", "f", true},
        {"sleeper", "Box", NULL, true}};
    source_manifest_write(&probe, "methods.xr", records, 6);
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0};
    XrXirStatus status = effect_execution_check(&probe, &result, &diagnostic);
    if (status != XR_XIR_OK) fprintf(stderr, "method promise: %s\n", diagnostic.message);
    CHECK(status == XR_XIR_OK); source_method_promise_execute(result.checked, output);
    xr_xir_compile_source_result_free(&result);
    SourceTestDeclaration original = records[0];
    for (uint32_t i = 0; i < 4; ++i) {
        records[0] = original;
        if (i == 0) records[0].owner = NULL;
        if (i == 1) records[0].owner = "Missing";
        if (i == 2) records[0].owner = "Other";
        if (i == 3) records[0].parameter = "this";
        source_manifest_write(&probe, "methods.xr", records, 6);
        memset(&diagnostic, 0, sizeof(diagnostic));
        CHECK(effect_execution_check(&probe, &result, &diagnostic) != XR_XIR_OK);
        CHECK(strstr(diagnostic.message, i < 2 ? "target is missing" :
            i == 2 ? "explicit target promise" : "parameter name is missing"));
        CHECK(!result.checked); xr_xir_compile_source_result_free(&result);
    }
    records[0] = original; source_manifest_write(&probe, "methods.xr", records, 7);
    memset(&diagnostic, 0, sizeof(diagnostic));
    CHECK(effect_execution_check(&probe, &result, &diagnostic) == XR_XIR_BAD_TYPE);
    CHECK(!result.checked && strstr(diagnostic.message, "declared no_suspend"));
    xr_xir_compile_source_result_free(&result);
    records[6] = original; source_manifest_write(&probe, "methods.xr", records, 7);
    memset(&diagnostic, 0, sizeof(diagnostic));
    CHECK(effect_execution_check(&probe, &result, &diagnostic) == XR_XIR_BAD_STRUCTURE);
    CHECK(!result.checked && strstr(diagnostic.message, "manifest admission failed"));
    xr_xir_compile_source_result_free(&result); source_manifest_raw(request, "");
}
#endif // XIR_SOURCE_METHOD_PROMISES_H
