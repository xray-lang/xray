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
    XrXirCheckedPacket packet = {0}; XrXirArtifact *copy = NULL, *closed = NULL, *lowered = NULL;
    CHECK(xr_xir_checked_write(checked, NULL, &packet, NULL) == XR_XIR_OK);
    CHECK(xr_xir_checked_read(packet.bytes, packet.length, NULL, &copy, NULL) == XR_XIR_OK);
    xr_xir_checked_packet_free(&packet);
    CHECK(xr_xir_specialize(copy, NULL, &closed, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(copy);
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_lower(closed, &target, NULL, &lowered, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(closed);
    const char *names[] = {"boundUse", "staticUse", "callbackUse"};
    const int64_t expected[] = {11, 13, 36}; uint32_t entries[3] = {UINT32_MAX, UINT32_MAX, UINT32_MAX};
    const XrXirModule *module = xr_xir_artifact_module(lowered);
    for (uint32_t f = 0; f < module->function_count; ++f)
        for (uint32_t i = 0; i < 3; ++i)
            if (strlen(names[i]) == module->functions[f].name_length &&
                !memcmp(names[i], module->functions[f].name, module->functions[f].name_length)) entries[i] = f;
    if (output) {
        XrXirCSource source = {0};
        CHECK(xr_xir_emit_c(lowered, "method_source", 4194304, &source) == XR_XIR_OK);
        CHECK(!strstr(source.text, "({") && !strstr(source.text, "xr_xir_vm"));
        FILE *file = fopen(output, "ab"); CHECK(file);
        CHECK(fwrite(source.text, 1, source.length, file) == source.length);
        CHECK(fprintf(file, "\nconst uint32_t method_source_selected_entries[3] = {%uu,%uu,%uu};\n",
            entries[0], entries[1], entries[2]) > 0);
        CHECK(fclose(file) == 0); xr_xir_c_source_free(&source);
    }
    XrXirProgram *program = NULL;
    CHECK(xr_xir_vm_program_take(&lowered, (XrXirProgramBudget){33554432, 64000000}, &program) == XR_XIR_OK);
    XrXirInstanceConfig config = xr_xir_instance_defaults(); XrXirInstance *instance = NULL;
    CHECK(xr_xir_instance_new(program, &config, &instance) == XR_XIR_CALL_READY);
    for (uint32_t i = 0; i < 3; ++i) {
        CHECK(entries[i] != UINT32_MAX);
        CHECK(xr_xir_instance_start(instance, entries[i], NULL, 0) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_poll(instance).outcome.status == XR_XIR_CALL_RETURNED);
        XrXirValue value = {0};
        CHECK(xr_xir_instance_take_result(instance, &value) == XR_XIR_CALL_RETURNED);
        CHECK(value.type == XR_XIR_I64 && value.payload == expected[i]); xr_xir_value_drop(&value);
    }
    CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY); xr_xir_program_drop(program);
}
static void source_method_promise_cases(const XrXirSourceRequest *request, const char *output) {
    XrXirSourceRequest probe = *request;
    probe.entry_path = XR_EFFECT_FIXTURES "/methods.xr"; probe.declarations = NULL;
    XrXirSourceResult baseline = {0};
    CHECK(xr_xir_source_check(&probe, &baseline, NULL) == XR_XIR_OK);
    const XrXirDeclarations *decls = xr_xir_artifact_module(baseline.checked)->declarations;
    XrXirLiteral module = {decls->modules[decls->root_module].name, decls->modules[decls->root_module].name_length};
    XrXirSourcePromise records[] = {
        {module, {"get", 3}, XR_XIR_FUNCTION_NO_SUSPEND, 0, {"Box", 3}},
        {module, {"identity", 8}, XR_XIR_FUNCTION_NO_SUSPEND, 0, {"Box", 3}},
        {module, {"invoke", 6}, XR_XIR_FUNCTION_NO_SUSPEND, 1, {0}},
        {module, {"invokeStatic", 12}, XR_XIR_FUNCTION_NO_SUSPEND, 1, {0}},
        {module, {"call", 4}, XR_XIR_FUNCTION_NO_SUSPEND, 1, {"Box", 3}},
        {module, {"callStatic", 10}, XR_XIR_FUNCTION_NO_SUSPEND, 1, {"Box", 3}},
        {module, {"call", 4}, XR_XIR_FUNCTION_NO_SUSPEND, 0, {"Box", 3}},
        {module, {"callStatic", 10}, XR_XIR_FUNCTION_NO_SUSPEND, 0, {"Box", 3}},
        {module, {"sleeper", 7}, XR_XIR_FUNCTION_NO_SUSPEND, 0, {"Box", 3}}};
    XrXirSourcePromises table = {records, 8}; probe.declarations = &table;
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_source_check(&probe, &result, &diagnostic);
    if (status != XR_XIR_OK) fprintf(stderr, "method promise: %s\n", diagnostic.message);
    CHECK(status == XR_XIR_OK); source_method_promise_execute(result.checked, output);
    xr_xir_source_result_free(&result);
    XrXirSourcePromise original = records[0];
    for (uint32_t i = 0; i < 4; ++i) {
        records[0] = original;
        if (i == 0) records[0].owner = (XrXirLiteral){0};
        if (i == 1) records[0].owner = (XrXirLiteral){"Missing", 7};
        if (i == 2) records[0].owner = (XrXirLiteral){"Other", 5};
        if (i == 3) records[0].parameter = 1;
        memset(&diagnostic, 0, sizeof(diagnostic));
        CHECK(xr_xir_source_check(&probe, &result, &diagnostic) != XR_XIR_OK);
        CHECK(strstr(diagnostic.message, i < 2 ? "target is missing" :
            i == 2 ? "explicit target promise" : "parameter is missing"));
        CHECK(!result.checked); xr_xir_source_result_free(&result);
    }
    records[0] = original; table.count = 9;
    memset(&diagnostic, 0, sizeof(diagnostic));
    CHECK(xr_xir_source_check(&probe, &result, &diagnostic) == XR_XIR_BAD_TYPE);
    CHECK(!result.checked && strstr(diagnostic.message, "declared no_suspend"));
    xr_xir_source_result_free(&result);
    records[8] = original;
    memset(&diagnostic, 0, sizeof(diagnostic));
    CHECK(xr_xir_source_check(&probe, &result, &diagnostic) == XR_XIR_BAD_STRUCTURE);
    CHECK(!result.checked && strstr(diagnostic.message, "duplicate"));
    xr_xir_source_result_free(&result);
    xr_xir_source_result_free(&baseline);
}
#endif // XIR_SOURCE_METHOD_PROMISES_H
