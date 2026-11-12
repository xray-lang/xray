/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_signal_escaping_cases.h - Source-backed registration parameter ownership
 */
#include "xir_genuine_sys_path.h"

static XrXirStatus signal_contract_source(const char *input, XrXirSourceResult *output,
    XrXirSourceDiagnostic *diagnostic) {
    const char *stdlib = XR_ROOT_EFFECT_STDLIB;
    char path[2048];
    int length = snprintf(path, sizeof(path), "%s/sys/sys.xr", stdlib);
    CHECK(length > 0 && (size_t)length < sizeof(path));
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_STDLIB, "sys", stdlib};
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(effect_context.resources, &session) == XR_COMPILE_RESOURCE_OK);
    XrXirSourceRequest request = {session, path, &authority, &effect_context,
        stdlib, NULL, XR_XIR_PROGRAM, NULL};
    XrXirStatus status = xir_genuine_sys_path_check(&request, input, strlen(input), output, diagnostic);
    xr_compile_session_free(session);
    return status;
}

static void signal_escaping_contracts(void) {
    EffectMark before = effect_mark();
    const XrNativeCallableDeclaration *canonical = xr_native_callable_by_id(XR_NATIVE_CALLABLE_SYS_ON_SIGNAL);
    CHECK(canonical && canonical->member && canonical->member->parameter_count == 2);
    CHECK(!canonical->member->parameters[0].escaping && canonical->member->parameters[1].escaping);
    CHECK(strstr(canonical->member->signature, "handler: escaping fn(): ()"));
    static const char accepted[] =
        "export fn transfer(handler: escaping fn())->bool{return __onSignal(2,handler)}\n";
    XrXirSourceResult source = {0};
    XrXirSourceDiagnostic diagnostic = {0};
    XrXirStatus status = signal_contract_source(accepted, &source, &diagnostic);
    if (status != XR_XIR_OK) fprintf(stderr, "signal escaping Source%u %d:%d %s\n",
        status, diagnostic.line, diagnostic.column, diagnostic.message);
    CHECK(status == XR_XIR_OK && source.checked && source.snapshot);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(source.checked, &packet, NULL) == XR_XIR_OK);
    xr_xir_compile_source_result_free(&source);
    XrXirArtifact *received = NULL;
    CHECK(xr_xir_compile_checked_read(&effect_context, packet.bytes, packet.length, &received, NULL) == XR_XIR_OK);
    xr_xir_compile_checked_packet_free(&packet);
    const XrXirModule *module = xr_xir_compile_artifact_module(received);
    uint32_t native = root_find(module, "__onSignal"), transfer = root_find(module, "transfer");
    CHECK(native < module->function_count && transfer < module->function_count);
    CHECK(xir_native_callable_verify(module, native) == XR_XIR_OK);
    CHECK(module->functions[native].escaping_parameter_count == 1 &&
        module->functions[native].escaping_parameters[0] == 1);
    CHECK(module->functions[transfer].escaping_parameter_count == 1 &&
        module->functions[transfer].escaping_parameters[0] == 0);
    XrXirFunction *functions = xr_malloc((size_t)module->function_count * sizeof(*functions));
    CHECK(functions);
    memcpy(functions, module->functions, (size_t)module->function_count * sizeof(*functions));
    XrXirModule changed = *module;
    changed.functions = functions;
    const uint32_t forged[][2] = {{0, 0}, {1, 1}, {2, 0}};
    for (uint32_t p = 0; p < 3; ++p) {
        functions[native] = module->functions[native];
        functions[native].escaping_parameters = forged[p];
        functions[native].escaping_parameter_count = p == 1 ? 2 : 1;
        CHECK(xir_native_callable_verify(&changed, native) == XR_XIR_BAD_TYPE);
    }
    functions[native].escaping_parameter_count = 0;
    functions[native].escaping_parameters = NULL;
    CHECK(xir_native_callable_verify(&changed, native) == XR_XIR_BAD_TYPE);
    functions[native].escaping_parameter_count = 1;
    CHECK(xir_native_callable_verify(&changed, native) == XR_XIR_BAD_TYPE);
    functions[native] = module->functions[native];
    CHECK(xir_native_callable_verify(&changed, native) == XR_XIR_OK);
    uint32_t ordinary = root_find(module, "__threadYield");
    CHECK(ordinary < module->function_count);
    functions[ordinary].escaping_parameters = forged[0];
    functions[ordinary].escaping_parameter_count = 1;
    CHECK(xir_native_callable_verify(&changed, ordinary) == XR_XIR_BAD_TYPE);
    xr_free(functions);
    xr_xir_compile_artifact_free(received);

    const XrNativeTypeDeclaration *array = xr_native_declaration_by_id(XR_NATIVE_DECLARATION_ARRAY);
    const XrNativeMemberDeclaration *get = xr_native_declaration_member(array, "get");
    CHECK(array && get && get->parameter_count == 1);
    XrNativeMemberDeclaration *members = xr_malloc((size_t)array->member_count * sizeof(*members));
    CHECK(members);
    memcpy(members, array->members, (size_t)array->member_count * sizeof(*members));
    XrNativeParameter parameter = get->parameters[0];
    CHECK(!parameter.escaping);
    parameter.escaping = true;
    members[get - array->members].parameters = &parameter;
    XrNativeTypeDeclaration forged_array = *array;
    forged_array.members = members;
    CHECK(!xr_native_declaration_validate(&forged_array));
    xr_free(members);

    static const char rejected[] =
        "export fn transfer(handler: fn())->bool{return __onSignal(2,handler)}\n";
    status = signal_contract_source(rejected, &source, &diagnostic);
    CHECK(status == XR_XIR_BAD_TYPE && !source.checked && !source.snapshot);
    CHECK(strstr(diagnostic.message, "E0398") && strstr(diagnostic.message, "an escaping parameter"));
    xr_xir_compile_source_result_free(&source);
    effect_mark_check(before);
    puts("signal escaping: canonical metadata, cold Checked receipt and restricted forwarding rejection PASS");
}
