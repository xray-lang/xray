/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_weaken_authority_cases.h - Captured private bindings retain revocable authority
 */
#ifndef XIR_WEAKEN_AUTHORITY_CASES_H
#define XIR_WEAKEN_AUTHORITY_CASES_H
typedef struct WeakenWitness {
    const XrXirValue *probe;
    XrXirCallStatus status;
    unsigned releases, calls, conversions;
} WeakenWitness;
static XrXirAction weaken_authority_resume(XrXirCallView *view) {
    WeakenWitness *w = (WeakenWitness *)view->environment;
    FunctionCaseFrame *frame = view->state;
    uint32_t entry = xr_xir_call_current_entry(view->activation);
    XrXirCallStatus status = XR_XIR_CALL_READY;
    if (entry == 1) return (XrXirAction){XR_XIR_ACTION_RETURN, 0, NULL, 0, {XR_XIR_I64, 0, 0}, {0}, 0};
    if (entry == 2) {
        XrXirValue capture = {0};
        status = xr_xir_instance_literal(view, 0, &capture);
        if (status == XR_XIR_CALL_READY)
            status = xr_xir_instance_function(view, (XrXirType)257, 3, &capture, 1, &frame->owned);
        xr_xir_value_drop(&capture);
    } else if (entry == 3) {
        ++w->calls;
        CHECK(xr_xir_value_copy(&view->arguments[0], &frame->owned) == XR_XIR_VALUE_OK);
    } else if (entry == 4) {
        const XrXirFunctionBinding *binding = xr_xir_function_binding(&view->arguments[0]);
        CHECK(binding && binding->capture_count == 1);
        XrXirValue denied = {0};
        CHECK(xr_xir_instance_function(view, (XrXirType)256, 3, binding->captures, 1, &denied) == XR_XIR_CALL_BAD_ARGUMENT);
        CHECK(!denied.type);
        ++w->conversions;
        status = xr_xir_instance_weaken_function(view, (XrXirType)256, &view->arguments[0], &frame->owned);
    } else if (entry == 6) {
        w->status = xr_xir_instance_weaken_function(view, (XrXirType)256, w->probe, &frame->owned);
        CHECK(!frame->owned.type);
    }
    if (status != XR_XIR_CALL_READY) return function_case_fault(status);
    return (XrXirAction){XR_XIR_ACTION_RETURN, 0, NULL, 0, frame->owned, {0}, 0};
}
static void weaken_authority_cases(void) {
    WeakenWitness witness = {0}; uint32_t dependency = 1;
    XrXirSourceModule modules[] = {{"root", 4, &dependency, 1, 0}, {"library", 7, NULL, 0, 5}};
    XrXirFunctionIdentity ids[7] = {{0}};
    ids[1].exported = ids[2].exported = ids[4].exported = ids[6].exported = 1;
    ids[2].module = ids[3].module = ids[5].module = 1;
    ids[3].promises = XR_XIR_FUNCTION_NO_SUSPEND;
    XrXirLiteral literal = {"captured authority", 18};
    XrXirDeclarations declarations = {modules, 2, ids, NULL, 0, &literal, 1, 0, 1, NULL};
    XrXirTypeNode nodes[] = {
        {XR_XIR_TYPE_CALLABLE, XR_XIR_UNIT, NULL, 0, XR_XIR_STRING, 0, 0, {0}},
        {XR_XIR_TYPE_CALLABLE, XR_XIR_UNIT, NULL, 0, XR_XIR_STRING, XR_XIR_CALLABLE_NO_SUSPEND, 0, {0}}};
    XrXirTypes types = {nodes, 2, NULL, NULL};
    XrXirType capture = XR_XIR_STRING, strong_type = (XrXirType)257;
    XrXirCallEntry entries[7];
    for (unsigned f = 0; f < 7; ++f)
        entries[f] = (XrXirCallEntry){XR_XIR_CALL_ABI_VERSION, f == 3 ? &capture : f == 4 ? &strong_type : NULL,
            f == 3 || f == 4 ? 1u : 0u,
            f == 1 ? XR_XIR_I64 : f == 2 ? strong_type : f == 3 ? XR_XIR_STRING : f == 4 ? (XrXirType)256 : XR_XIR_UNIT,
            sizeof(FunctionCaseFrame), weaken_authority_resume, function_case_cleanup, &witness, 0, 0};
    XrXirProgramSpec spec = {XR_XIR_PROGRAM_ABI_VERSION, {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION},
        entries, 7, &declarations, {&witness.releases, function_case_release}, &types, {0}};
    XrXirArtifact *proof = NULL; CHECK(native_metadata_fixture(&spec, &proof) == XR_XIR_OK);
    spec.proof = xr_xir_program_proof(proof);
    XrXirProgram *program = NULL;
    CHECK(xr_xir_program_seal(&spec, (XrXirProgramBudget){2097152, 16000000}, &program) == XR_XIR_OK);
    xr_xir_artifact_free(proof);
    XrXirInstance *instance = NULL, *other = NULL; XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_new(program, &config, &instance) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_new(program, &config, &other) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance, 2, NULL, 0) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll(instance).outcome.status == XR_XIR_CALL_RETURNED);
    XrXirValue strong = {0}, weak = {0}, result = {0};
    CHECK(xr_xir_instance_take_result(instance, &strong) == XR_XIR_CALL_RETURNED);
    witness.probe = &strong;
    CHECK(xr_xir_instance_start(other, 6, NULL, 0) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll(other).outcome.status == XR_XIR_CALL_RETURNED);
    CHECK(witness.status == XR_XIR_CALL_BAD_ARGUMENT && !witness.calls && !witness.conversions);
    CHECK(xr_xir_instance_start(instance, 4, &strong, 1) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll(instance).outcome.status == XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_take_result(instance, &weak) == XR_XIR_CALL_RETURNED);
    CHECK(strong.type == 257 && weak.type == 256 && strong.payload != weak.payload);
    const XrXirFunctionBinding *a = xr_xir_function_binding(&strong), *b = xr_xir_function_binding(&weak);
    CHECK(a && b && a->owner == b->owner && a->entry == b->entry && a->capture_count == 1 && b->capture_count == 1);
    CHECK(a->captures[0].payload == b->captures[0].payload);
    CHECK(xr_xir_instance_start_function(instance, &weak, NULL, 0) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll(instance).outcome.status == XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_take_result(instance, &result) == XR_XIR_CALL_RETURNED);
    CHECK(witness.calls == 1 && witness.conversions == 1);
    CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(other, 6, NULL, 0) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll(other).outcome.status == XR_XIR_CALL_RETURNED);
    CHECK(witness.status == XR_XIR_CALL_BAD_ARGUMENT && witness.calls == 1 && witness.conversions == 1);
    CHECK(xr_xir_instance_free(other) == XR_XIR_CALL_READY);
    xr_xir_program_drop(program); CHECK(!witness.releases);
    xr_xir_value_drop(&strong); CHECK(!witness.releases);
    xr_xir_value_drop(&weak); CHECK(witness.releases == 1);
    const char *bytes = NULL; size_t length = 0;
    CHECK(xr_xir_string_view(&result, &bytes, &length) && length == 18 && !memcmp(bytes, "captured authority", 18));
    xr_xir_value_drop(&result);
}
#endif // XIR_WEAKEN_AUTHORITY_CASES_H
