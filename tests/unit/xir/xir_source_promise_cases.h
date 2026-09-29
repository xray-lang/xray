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
    XrXirSourceRequest probe = *request;
    probe.entry_path = XR_EFFECT_FIXTURES "/default_selector.xr"; probe.declarations = NULL;
    XrXirSourceResult baseline = {0};
    CHECK(xr_xir_source_check(&probe, &baseline, NULL) == XR_XIR_OK);
    const XrXirDeclarations *decls = xr_xir_artifact_module(baseline.checked)->declarations;
    XrXirLiteral module = {decls->modules[decls->root_module].name, decls->modules[decls->root_module].name_length};
    XrXirSourcePromise item = {module, {"$argument_default", 17}, XR_XIR_FUNCTION_NO_SUSPEND, 0};
    XrXirSourcePromises table = {&item, 1}; probe.declarations = &table;
    for (uint32_t p = 0; p < 2; ++p) {
        item.parameter = p;
        XrXirSourceResult denied = {0}; XrXirSourceDiagnostic diagnostic = {0};
        CHECK(xr_xir_source_check(&probe, &denied, &diagnostic) == XR_XIR_BAD_STRUCTURE);
        CHECK(!denied.checked && strstr(diagnostic.message, "target is missing"));
        xr_xir_source_result_free(&denied);
    }
    xr_xir_source_result_free(&baseline);
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
    XrXirInstance *instance = NULL; XrXirInstanceConfig config = xr_xir_instance_defaults();
    CHECK(xr_xir_instance_new(program, &config, &instance) == XR_XIR_CALL_READY);
    for (unsigned i = 0; i < 2; ++i) {
        CHECK(xr_xir_instance_start(instance, i ? captured : entry, NULL, 0) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_poll(instance).outcome.status == XR_XIR_CALL_RETURNED);
        XrXirValue value = {0};
        CHECK(xr_xir_instance_take_result(instance, &value) == XR_XIR_CALL_RETURNED);
        CHECK(value.type == XR_XIR_I64 && value.payload == 7); xr_xir_value_drop(&value);
    }
    CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
    xr_xir_program_drop(program);
}
static void source_callable_promise_cases(XrXirSourceRequest *request, const XrXirArtifact *baseline, const char *output) {
    source_synthetic_promise_case(request);
    const XrXirDeclarations *decls = xr_xir_artifact_module(baseline)->declarations;
    XrXirLiteral module = {decls->modules[decls->root_module].name, decls->modules[decls->root_module].name_length};
    const char *reject[] = {"rejectYield", "rejectUnknown", "rejectGeneric", "rejectDefaultYield", "rejectDefaultName"};
    for (unsigned i = 0; i < 5; ++i) {
        XrXirSourcePromise item = {module, {reject[i], (uint32_t)strlen(reject[i])}, XR_XIR_FUNCTION_NO_SUSPEND, 1};
        XrXirSourcePromises table = {&item, 1}; request->declarations = &table;
        XrXirSourceResult denied = {0}; XrXirSourceDiagnostic diagnostic = {0};
        CHECK(xr_xir_source_check(request, &denied, &diagnostic) == XR_XIR_BAD_TYPE);
        CHECK(!denied.checked);
        if (i < 4) CHECK(strstr(diagnostic.message, "declared no_suspend"));
        xr_xir_source_result_free(&denied);
    }
    XrXirSourcePromise items[] = {
        {module, {"dynamic", 7}, XR_XIR_FUNCTION_NO_SUSPEND, 1},
        {module, {"dynamic", 7}, XR_XIR_FUNCTION_NO_SUSPEND, 0},
        {module, {"pure", 4}, XR_XIR_FUNCTION_NO_SUSPEND, 0},
        {module, {"weaken", 6}, XR_XIR_FUNCTION_NO_SUSPEND, 1},
        {module, {"literalDefault", 14}, XR_XIR_FUNCTION_NO_SUSPEND, 1},
        {module, {"namedDefault", 12}, XR_XIR_FUNCTION_NO_SUSPEND, 1},
        {module, {"effectDefault", 13}, XR_XIR_FUNCTION_NO_SUSPEND, 1},
        {module, {"dynamic", 7}, XR_XIR_FUNCTION_NO_SUSPEND, 1}};
    XrXirSourcePromises declarations = {items, 7}; request->declarations = &declarations;
    for (uint32_t i = 0; i < 4; ++i) {
        declarations.count = i == 0 ? 2 : i == 1 ? 8 : 7;
        items[0].parameter = i == 2 ? 2 : 1;
        if (i == 3) items[0].function = (XrXirLiteral){"parameterThrow", 14};
        XrXirSourceResult result = {0};
        CHECK(xr_xir_source_check(request, &result, NULL) == XR_XIR_BAD_TYPE);
        CHECK(!result.checked); xr_xir_source_result_free(&result);
    }
    items[0].function = (XrXirLiteral){"dynamic", 7};
    items[7] = (XrXirSourcePromise){module, {"effectDefaultUse", 16}, XR_XIR_FUNCTION_NO_SUSPEND, 0};
    declarations.count = 8;
    XrXirSourceResult rejected_default = {0}; XrXirSourceDiagnostic default_diagnostic = {0};
    CHECK(xr_xir_source_check(request, &rejected_default, &default_diagnostic) == XR_XIR_BAD_TYPE);
    CHECK(!rejected_default.checked && strstr(default_diagnostic.message, "declared no_suspend"));
    xr_xir_source_result_free(&rejected_default); declarations.count = 7;
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
    xr_xir_source_result_free(&result); request->declarations = NULL;
    memset(items, 0xCC, sizeof(items)); source_qualified_vm(checked, output);
}
#endif // XIR_SOURCE_PROMISE_CASES_H
