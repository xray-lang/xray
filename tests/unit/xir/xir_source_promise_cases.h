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
#include "xir/xxir_effect_contract_internal.h"
static void source_same_bound_barrier(const XrXirModule *module,uint32_t function,uint32_t instruction) {
    const XrXirProvenance *source=module->provenance;
    CHECK(source && source->kind==XR_XIR_EVIDENCE_TEMPLATE && source->contracts && source->contract_count==module->function_count);
    const XrXirFunction *body=&module->functions[function];const XrXirInstruction *op=&body->instructions[instruction];
    CHECK(op->op==XR_XIR_FUNCTION_WEAKEN && xr_xir_operand_type(body,op->args[0])==op->type);
    const XrXirTypeNode *type=xr_xir_callable_signature(module->types,op->type);
    CHECK(type && xr_xir_callable_flags_valid(type->flags));
    const XrXirFunctionEffectContract *contract=&source->contracts[function];uint32_t records=0;
    for (uint32_t v=0;v<contract->value_count;++v) {
        const XrXirRootValueIdentity *value=&contract->values[v];
        if (value->instruction!=instruction) continue;
        CHECK(value->mode==XR_XIR_EFFECT_VALUE_FIXED && value->declared_type==op->type);++records;
    }
    CHECK(records==1);
}
static void source_synthetic_promise_case(const XrXirSourceRequest *request) {
    XrXirSourceRequest probe = *request; char path[8192];
    source_fixture_path(request, "default_selector.xr", path); probe.entry_path = path;
    SourceTestDeclaration item = {"$argument_default", NULL, NULL, true};
    for (uint32_t p = 0; p < 2; ++p) {
        item.parameter = p ? "f" : NULL;
        source_manifest_write(&probe, "default_selector.xr", &item, 1);
        XrXirSourceResult denied = {0}; XrXirSourceDiagnostic diagnostic = {0};
        CHECK(effect_execution_check(&probe, &denied, &diagnostic) == XR_XIR_BAD_STRUCTURE);
        CHECK(!denied.checked && strstr(diagnostic.message, "target is missing"));
        xr_xir_compile_source_result_free(&denied);
    }
    source_manifest_raw(request, "");
}
static void source_qualified_vm(XrXirArtifact *checked, const char *output, bool strict) {
    const XrXirCompileContext context=*xr_xir_compile_artifact_context(checked);
    const XrXirModule *input = xr_xir_compile_artifact_module(checked);
    uint32_t weakenings = 0, root_only = 0, same_bound = 0, captured_bound = 0, qualified = 0;
    for (uint32_t f = 0; f < input->function_count; ++f) {
        const XrXirFunction *function = &input->functions[f];
        const char *callbacks[] = {"dynamic", "weaken", "literalDefault", "namedDefault", "effectDefault"};
        for (uint32_t c = 0; c < 5; ++c) {
            if (function->name_length != strlen(callbacks[c]) ||
                memcmp(function->name, callbacks[c], function->name_length)) continue;
            CHECK(function->parameter_count == 1);
            const XrXirTypeNode *parameter = xr_xir_callable_signature(input->types, function->parameters[0]);
            CHECK(parameter && parameter->flags == (XR_XIR_CALLABLE_ROOT_UNRESOLVED | XR_XIR_CALLABLE_NO_SUSPEND));
            ++qualified;
        }
        for (uint32_t i = 0; i < function->instruction_count; ++i) {
            const XrXirInstruction *instruction = &function->instructions[i];
            if (instruction->op != XR_XIR_FUNCTION_WEAKEN) continue;
            uint32_t value = instruction->args[0];
            CHECK(value < function->parameter_count + function->instruction_count);
            XrXirType type = value < function->parameter_count ? function->parameters[value] :
                function->instructions[value - function->parameter_count].type;
            const XrXirTypeNode *from = xr_xir_callable_signature(input->types, type);
            const XrXirTypeNode *to = xr_xir_callable_signature(input->types, instruction->type);
            CHECK(from && to);
            if (type==instruction->type) {
                source_same_bound_barrier(input,f,i);++same_bound;
                if (function->name_length==11 && !memcmp(function->name,"capturedUse",11)) {
                    CHECK(from->flags==(XR_XIR_CALLABLE_ROOT_UNRESOLVED|XR_XIR_CALLABLE_NO_SUSPEND));
                    ++captured_bound;
                }
            } else if ((from->flags & XR_XIR_CALLABLE_NO_SUSPEND) && !(to->flags & XR_XIR_CALLABLE_NO_SUSPEND))
                ++weakenings;
            else {
                CHECK((from->flags & XR_XIR_CALLABLE_NO_SUSPEND) == (to->flags & XR_XIR_CALLABLE_NO_SUSPEND));
                CHECK((from->flags & XR_XIR_CALLABLE_ROOT_MASK) == XR_XIR_CALLABLE_ROOT_NONE);
                CHECK((to->flags & XR_XIR_CALLABLE_ROOT_MASK) == XR_XIR_CALLABLE_ROOT_UNRESOLVED);
                ++root_only;
            }
        }
    }
    if (strict) CHECK(weakenings==1);
    else CHECK(qualified==5 && !weakenings && root_only>0 && same_bound>0 && captured_bound==2);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(checked, &packet, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked); checked = NULL;
    CHECK(xr_xir_compile_checked_read(&context, packet.bytes, packet.length, &checked, NULL) == XR_XIR_OK);
    xr_xir_compile_checked_packet_free(&packet);
    XrXirArtifact *closed = NULL, *lowered = NULL;
    CHECK(xr_xir_compile_specialize(checked, &closed, NULL, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_lower(closed,&target, NULL,&lowered,&diagnostic);
    if (status!=XR_XIR_OK) {
        XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(context.resources,&stats)==XR_COMPILE_RESOURCE_OK);
        fprintf(stderr,"qualified lower strict=%u status=%u at=%u:%u:%u reason=%u work=%llu allocated=%llu peak=%llu live=%llu\n",
            strict,status,diagnostic.function,diagnostic.block,diagnostic.instruction,diagnostic.reason,
            (unsigned long long)stats.work,(unsigned long long)stats.allocated_bytes,
            (unsigned long long)stats.peak_bytes,(unsigned long long)stats.live_bytes);
    }
    CHECK(status==XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);
    const XrXirModule *module = xr_xir_compile_artifact_module(lowered);
    uint32_t entry = UINT32_MAX, captured = UINT32_MAX;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        if (module->functions[f].name_length == 11 && !memcmp(module->functions[f].name, "promisedUse", 11)) entry = f;
        if (module->functions[f].name_length == 11 && !memcmp(module->functions[f].name, "capturedUse", 11)) captured = f;
    }
    CHECK(entry != UINT32_MAX && captured != UINT32_MAX);
    XrXirCSource source = {0};
    XrXirStatus emit_status=xr_xir_compile_emit_c(lowered,"effect_source",4194304,&source);
    if(emit_status!=XR_XIR_OK) {
        XrCompileResourceStats stats={0};
        CHECK(xr_compile_resources_stats(context.resources,&stats)==XR_COMPILE_RESOURCE_OK);
        fprintf(stderr,"Effects C emission status%u work%llu\n",emit_status,(unsigned long long)stats.work);
    }
    CHECK(emit_status==XR_XIR_OK);
    CHECK(!strstr(source.text, "({") && !strstr(source.text, "xr_xir_vm"));
    if (output) {
        FILE *file = fopen(output, "wb"); CHECK(file);
        CHECK(fwrite(source.text, 1, source.length, file) == source.length);
        CHECK(fprintf(file, "\nconst uint32_t effect_source_entry = %uu;\n", entry) > 0);
        CHECK(fprintf(file, "const uint32_t effect_source_captured = %uu;\n", captured) > 0);
        CHECK(fclose(file) == 0);
    }
    xr_xir_compile_c_source_free(&source);
    XrXirProgram *program = NULL;
    CHECK(xr_xir_compile_vm_program_take(&lowered, &program) == XR_XIR_OK);
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
    xr_xir_compile_program_drop(program);
}
static void source_forward_rejections(const XrXirSourceRequest *request) {
    for (uint32_t mode=0;mode<2;++mode) {
        FILE *file=fopen(request->entry_path,"wb");CHECK(file);
        CHECK(fputs("fn pure()->i64{return 7}\n"
            "fn erase(f:fn()->i64)->fn()->i64{return f}\n"
            "fn forward(f:fn()->i64)->fn()->i64{return erase(f)}\n"
            "fn use()->i64{const callback=forward(pure);return callback()}\n",file)>=0);
        if (!mode) CHECK(fputs("const worker=go use()\n",file)>=0);
        CHECK(fclose(file)==0);
        SourceTestDeclaration declared[]={{"pure",NULL,NULL,true},{mode?"erase":"forward",NULL,"f",false}};
        source_manifest_write(request,"qualified_boundary.xr",declared,2);
        XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
        CHECK(effect_execution_check(request,&result,&diagnostic)==XR_XIR_BAD_TYPE);
        CHECK(!result.checked && !result.snapshot && diagnostic.status==XR_XIR_BAD_TYPE);
        if (!mode) CHECK(strstr(diagnostic.message,"GO target lacks proof for worker execution"));
        xr_xir_compile_source_result_free(&result);
    }
}
static void source_fixed_promise_case(const XrXirSourceRequest *request, bool forwarding) {
    char path[8192];source_fixture_path(request,"qualified_boundary.xr",path);
    FILE *file=fopen(path,"wb");CHECK(file);
    CHECK(fputs("fn pure()->i64{return 7}\n"
        "fn erase(f:fn()->i64)->fn()->i64{return f}\n",file)>=0);
    if (forwarding) CHECK(fputs("fn forward(f:fn()->i64)->fn()->i64{return erase(f)}\n",file)>=0);
    CHECK(fputs(forwarding ?
        "export fn promisedUse()->i64{const callback=forward(pure);return callback()}\n" :
        "export fn promisedUse()->i64{const callback=erase(pure);return callback()}\n",file)>=0);
    CHECK(fputs("export fn capturedUse()->i64{return promisedUse()}\n",file)>=0 && fclose(file)==0);
    XrXirSourceRequest probe=*request;probe.entry_path=path;
    SourceTestDeclaration declared[]={{"pure",NULL,NULL,true},{forwarding?"forward":"erase",NULL,"f",false}};
    source_manifest_write(&probe,"qualified_boundary.xr",declared,2);
    XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=effect_execution_check(&probe,&result,&diagnostic);
    if (status!=XR_XIR_OK) fprintf(stderr,"fixed callable boundary status=%u at=%d:%d %s\n",
        status,diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==XR_XIR_OK);
    XrXirArtifact *checked=result.checked;result.checked=NULL;xr_xir_compile_source_result_free(&result);
    source_qualified_vm(checked,NULL,true);
    if (forwarding) source_forward_rejections(&probe);
    CHECK(xr_test_unlink(path)==0);source_manifest_raw(request,"");
}
static void source_callable_promise_cases(XrXirSourceRequest *request, const char *output) {
    source_synthetic_promise_case(request);
    const char *reject[] = {"rejectYield", "rejectUnknown", "rejectGeneric", "rejectDefaultYield", "rejectDefaultName"};
    for (unsigned i = 0; i < 5; ++i) {
        SourceTestDeclaration item = {reject[i], NULL, "f", false};
        source_manifest_write(request, "root.xr", &item, 1);
        XrXirSourceResult denied = {0}; XrXirSourceDiagnostic diagnostic = {0};
        CHECK(effect_execution_check(request, &denied, &diagnostic) == XR_XIR_BAD_TYPE);
        CHECK(!denied.checked);
        if (i < 4) CHECK(strstr(diagnostic.message, "declared no_suspend"));
        xr_xir_compile_source_result_free(&denied);
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
        CHECK(effect_execution_check(request, &result, NULL) == (i == 1 ? XR_XIR_BAD_STRUCTURE : XR_XIR_BAD_TYPE));
        CHECK(!result.checked); xr_xir_compile_source_result_free(&result);
    }
    items[0] = (SourceTestDeclaration){"dynamic", NULL, "f", true};
    items[6] = (SourceTestDeclaration){"effectDefaultUse", NULL, NULL, true};
    source_manifest_write(request, "root.xr", items, 7);
    XrXirSourceResult rejected_default = {0}; XrXirSourceDiagnostic default_diagnostic = {0};
    CHECK(effect_execution_check(request, &rejected_default, &default_diagnostic) == XR_XIR_BAD_TYPE);
    CHECK(!rejected_default.checked && strstr(default_diagnostic.message, "declared no_suspend"));
    xr_xir_compile_source_result_free(&rejected_default);
    source_manifest_write(request, "root.xr", items, 6);
    XrXirSourceResult result = {0};
    CHECK(effect_execution_check(request, &result, NULL) == XR_XIR_OK);
    const XrXirModule *checked_module = xr_xir_compile_artifact_module(result.checked);
    unsigned qualified_defaults = 0;
    for (uint32_t f = 0; f < checked_module->function_count; ++f) {
        const XrXirFunction *function = &checked_module->functions[f];
        if (function->name_length != 17 || memcmp(function->name, "$argument_default", 17)) continue;
        const XrXirTypeNode *type = xr_xir_callable_signature(checked_module->types, function->result);
        if (type && type->flags == (XR_XIR_CALLABLE_ROOT_UNRESOLVED | XR_XIR_CALLABLE_NO_SUSPEND)) {
            CHECK(!checked_module->declarations->functions[f].promises); ++qualified_defaults;
        }
    }
    CHECK(qualified_defaults == 3);
    const XrXirSourceView *view = xr_xir_compile_source_snapshot_view(result.snapshot);
    bool found = false;
    for (uint32_t d = 0; d < view->declaration_count; ++d) {
        const XrXirSourceDeclaration *decl = &view->declarations[d];
        if (decl->kind != XR_XIR_SOURCE_FUNCTION || strcmp(decl->name, "dynamic")) continue;
        CHECK(decl->parameter_count == 1);
        const XrXirTypeNode *type = xr_xir_callable_signature(view->types, decl->parameters[0].type);
        CHECK(type && type->flags == (XR_XIR_CALLABLE_ROOT_UNRESOLVED | XR_XIR_CALLABLE_NO_SUSPEND)); found = true;
    }
    CHECK(found);
    XrXirArtifact *checked = result.checked; result.checked = NULL;
    xr_xir_compile_source_result_free(&result); source_manifest_raw(request, "");
    memset(items, 0xCC, sizeof(items)); source_qualified_vm(checked, output, false);
    source_fixed_promise_case(request,false);
    source_fixed_promise_case(request,true);
}
#endif // XIR_SOURCE_PROMISE_CASES_H
