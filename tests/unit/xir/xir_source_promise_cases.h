/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_promise_cases.h - Qualified source callbacks through owned execution
 */
#ifndef XIR_SOURCE_PROMISE_CASES_H
#define XIR_SOURCE_PROMISE_CASES_H
static void source_synthetic_promise_case(const XrXirSourceRequest *request) {
    XrXirSourceRequest probe = *request; char path[8192];
    source_fixture_path(request, "default_selector.xr", path); probe.entry_path = path;
    SourceTestDeclaration item = {"$argument_default", NULL, NULL, true};
    for (uint32_t p = 0; p < 2; ++p) {
        item.parameter = p ? "f" : NULL;
        source_manifest_write(&probe, "default_selector.xr", &item, 1);
        XrXirSourceResult denied = {0}; XrXirSourceDiagnostic diagnostic = {0};
        CHECK(xr_xir_source_check(&probe, &denied, &diagnostic) == XR_XIR_BAD_STRUCTURE);
        CHECK(!denied.checked && strstr(diagnostic.message, "target is missing"));
        xr_xir_source_result_free(&denied);
    }
    source_manifest_raw(request, "");
}
static void source_qualified_vm(XrXirArtifact *checked, const char *output) {
    const XrXirModule *input = xr_xir_artifact_module(checked);
    uint32_t weakenings = 0;
    for (uint32_t f = 0; f < input->function_count; ++f)
        for (uint32_t i = 0; i < input->functions[f].instruction_count; ++i)
            if (input->functions[f].instructions[i].op == XR_XIR_FUNCTION_WEAKEN) ++weakenings;
    CHECK(weakenings == 1);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_checked_write(checked, NULL, &packet, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(checked); checked = NULL;
    CHECK(xr_xir_checked_read(packet.bytes, packet.length, NULL, &checked, NULL) == XR_XIR_OK);
    xr_xir_checked_packet_free(&packet);
    XrXirArtifact *closed = NULL, *lowered = NULL;
    CHECK(xr_xir_specialize(checked, NULL, &closed, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(checked);
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_lower(closed, &target, NULL, &lowered, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(closed);
    const XrXirModule *module = xr_xir_artifact_module(lowered);
    uint32_t entry = UINT32_MAX, captured = UINT32_MAX;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        if (module->functions[f].name_length == 11 && !memcmp(module->functions[f].name, "promisedUse", 11)) entry = f;
        if (module->functions[f].name_length == 11 && !memcmp(module->functions[f].name, "capturedUse", 11)) captured = f;
    }
    CHECK(entry != UINT32_MAX && captured != UINT32_MAX);
    XrXirCSource source = {0};
    CHECK(xr_xir_emit_c(lowered, "effect_source", 4194304, &source) == XR_XIR_OK);
    CHECK(!strstr(source.text, "({") && !strstr(source.text, "xr_xir_vm"));
    if (output) {
        FILE *file = fopen(output, "wb"); CHECK(file);
        CHECK(fwrite(source.text, 1, source.length, file) == source.length);
        CHECK(fprintf(file, "\nconst uint32_t effect_source_entry = %uu;\n", entry) > 0);
        CHECK(fprintf(file, "const uint32_t effect_source_captured = %uu;\n", captured) > 0);
        CHECK(fclose(file) == 0);
    }
    xr_xir_c_source_free(&source);
    XrXirProgram *program = NULL;
    CHECK(xr_xir_vm_program_take(&lowered, (XrXirProgramBudget){33554432, 64000000}, &program) == XR_XIR_OK);
    CHECK(!lowered);
    XrXirInstance *instance = NULL; XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_new(program, &config, &instance) == XR_XIR_CALL_READY);
    for (unsigned i = 0; i < 2; ++i) {
        CHECK(xr_xir_instance_start(instance, i ? captured : entry, NULL, 0) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status == XR_XIR_CALL_RETURNED);
        XrXirValue value = {0};
        CHECK(xr_xir_instance_take_result(instance, &value) == XR_XIR_CALL_RETURNED);
        CHECK(value.type == XR_XIR_I64 && value.payload == 7); xr_xir_value_drop(&value);
    }
    CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
    xr_xir_program_drop(program);
}
static void source_callable_promise_cases(XrXirSourceRequest *request, const char *output) {
    source_synthetic_promise_case(request);
    const char *reject[] = {"rejectYield", "rejectUnknown", "rejectGeneric", "rejectDefaultYield", "rejectDefaultName"};
    for (unsigned i = 0; i < 5; ++i) {
        SourceTestDeclaration item = {reject[i], NULL, "f", false};
        source_manifest_write(request, "root.xr", &item, 1);
        XrXirSourceResult denied = {0}; XrXirSourceDiagnostic diagnostic = {0};
        CHECK(xr_xir_source_check(request, &denied, &diagnostic) == XR_XIR_BAD_TYPE);
        CHECK(!denied.checked);
        if (i < 4) CHECK(strstr(diagnostic.message, "declared no_suspend"));
        xr_xir_source_result_free(&denied);
    }
    SourceTestDeclaration items[] = {
        {"dynamic", NULL, "f", true}, {"pure", NULL, NULL, true},
        {"weaken", NULL, "f", false}, {"literalDefault", NULL, "f", false},
        {"namedDefault", NULL, "f", false}, {"effectDefault", NULL, "f", false},
        {"dynamic", NULL, "f", true}};
    for (uint32_t i = 0; i < 4; ++i) {
        items[0].parameter = i == 2 ? "missing" : "f";
        if (i == 3) { items[0].name = "parameterThrow"; items[0].parameter = "e"; }
        source_manifest_write(request, "root.xr", items, i == 0 ? 1 : i == 1 ? 7 : 6);
        XrXirSourceResult result = {0};
        CHECK(xr_xir_source_check(request, &result, NULL) == (i == 1 ? XR_XIR_BAD_STRUCTURE : XR_XIR_BAD_TYPE));
        CHECK(!result.checked); xr_xir_source_result_free(&result);
    }
    items[0] = (SourceTestDeclaration){"dynamic", NULL, "f", true};
    items[6] = (SourceTestDeclaration){"effectDefaultUse", NULL, NULL, true};
    source_manifest_write(request, "root.xr", items, 7);
    XrXirSourceResult rejected_default = {0}; XrXirSourceDiagnostic default_diagnostic = {0};
    CHECK(xr_xir_source_check(request, &rejected_default, &default_diagnostic) == XR_XIR_BAD_TYPE);
    CHECK(!rejected_default.checked && strstr(default_diagnostic.message, "declared no_suspend"));
    xr_xir_source_result_free(&rejected_default);
    source_manifest_write(request, "root.xr", items, 6);
    XrXirSourceResult result = {0};
    CHECK(xr_xir_source_check(request, &result, NULL) == XR_XIR_OK);
    const XrXirModule *checked_module = xr_xir_artifact_module(result.checked);
    unsigned qualified_defaults = 0;
    for (uint32_t f = 0; f < checked_module->function_count; ++f) {
        const XrXirFunction *function = &checked_module->functions[f];
        if (function->name_length != 17 || memcmp(function->name, "$argument_default", 17)) continue;
        const XrXirTypeNode *type = xr_xir_callable_signature(checked_module->types, function->result);
        if (type && type->flags == XR_XIR_CALLABLE_NO_SUSPEND) {
            CHECK(!checked_module->declarations->functions[f].promises); ++qualified_defaults;
        }
    }
    CHECK(qualified_defaults == 3);
    const XrXirSourceView *view = xr_xir_source_snapshot_view(result.snapshot);
    bool found = false;
    for (uint32_t d = 0; d < view->declaration_count; ++d) {
        const XrXirSourceDeclaration *decl = &view->declarations[d];
        if (decl->kind != XR_XIR_SOURCE_FUNCTION || strcmp(decl->name, "dynamic")) continue;
        CHECK(decl->parameter_count == 1);
        const XrXirTypeNode *type = xr_xir_callable_signature(view->types, decl->parameters[0].type);
        CHECK(type && type->flags == XR_XIR_CALLABLE_NO_SUSPEND); found = true;
    }
    CHECK(found);
    XrXirArtifact *checked = result.checked; result.checked = NULL;
    xr_xir_source_result_free(&result); source_manifest_raw(request, "");
    memset(items, 0xCC, sizeof(items)); source_qualified_vm(checked, output);
}
#endif // XIR_SOURCE_PROMISE_CASES_H
