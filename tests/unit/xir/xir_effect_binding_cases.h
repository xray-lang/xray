/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_effect_binding_cases.h - Promise admission before native target effects
 */
#ifndef XIR_EFFECT_BINDING_CASES_H
#define XIR_EFFECT_BINDING_CASES_H
typedef struct EffectBindingWitness {
    XrXirValue strong, ordinary;
    XrXirCallStatus binding_status;
    uint32_t target_calls, consumer_calls, releases;
} EffectBindingWitness;
static XrXirAction effect_binding_resume(XrXirCallView *view) {
    EffectBindingWitness *w = (EffectBindingWitness *)view->environment;
    uint32_t entry = xr_xir_call_current_entry(view->activation);
    FunctionCaseFrame *frame = view->state;
    XrXirValue value = {0};
    if (entry == 1 || entry == 3 || entry == 6) {
        if (entry != 1) ++w->target_calls;
        value = (XrXirValue){XR_XIR_I64, 0, 42};
    } else if (entry == 2 || entry == 5) {
        w->binding_status = xr_xir_instance_function(view, (XrXirType)(entry == 2 ? 257 : 256),
            3, NULL, 0, entry == 2 ? &w->strong : &w->ordinary);
    } else if (entry == 4) {
        if (!frame->phase++) {
            ++w->consumer_calls;
            uint32_t target = UINT32_MAX;
            XrXirCallStatus status = xr_xir_instance_resolve_function(view, &view->arguments[0], &target);
            if (status != XR_XIR_CALL_READY) return function_case_fault(status);
            return (XrXirAction){XR_XIR_ACTION_CALL, target, NULL, 0, view->arguments[0], {0}, 0};
        }
        value = view->inbox.value;
    }
    return (XrXirAction){XR_XIR_ACTION_RETURN, 0, NULL, 0, value, {0}, 0};
}
static void effect_binding_cases(void) {
    for (unsigned promised = 0; promised < 2; ++promised) {
        EffectBindingWitness witness = {0};
        XrXirSourceModule source = {"root", 4, NULL, 0, 0};
        XrXirFunctionIdentity ids[7] = {{0}};
        ids[1].exported = ids[2].exported = ids[4].exported = ids[5].exported = 1;
        ids[3].promises = promised ? XR_XIR_FUNCTION_NO_SUSPEND : 0;
        XrXirDeclarations declarations = {&source, 1, ids, NULL, 0, NULL, 0, 0, 1, NULL};
        XrXirTypeNode nodes[2] = {{0}, {0}};
        for (unsigned n = 0; n < 2; ++n) {
            nodes[n].kind = XR_XIR_TYPE_CALLABLE; nodes[n].result = XR_XIR_I64;
            nodes[n].flags = XR_XIR_CALLABLE_ROOT_UNRESOLVED;
        }
        nodes[1].flags |= XR_XIR_CALLABLE_NO_SUSPEND;
        XrXirTypes types = {nodes, 2, NULL, NULL};
        XrXirType qualified = (XrXirType)257;
        XrXirCallEntry entries[7];
        for (unsigned f = 0; f < 7; ++f)
            entries[f] = (XrXirCallEntry){XR_XIR_CALL_ABI_VERSION, f == 4 ? &qualified : NULL,
                f == 4 ? 1u : 0u, f == 1 || f == 3 || f == 4 || f == 6 ? XR_XIR_I64 : XR_XIR_UNIT,
                sizeof(FunctionCaseFrame), effect_binding_resume, function_case_cleanup, &witness, 0, 0};
        XrXirProgramSpec spec = {XR_XIR_PROGRAM_ABI_VERSION, {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION},
            entries, 7, &declarations, {&witness.releases, function_case_release}, &types, {0}};
        NativeFixtureOwner compiler = {0}; CHECK(native_fixture_owner_new(&compiler) == XR_XIR_OK);
        XrXirArtifact *proof = NULL;
        CHECK(native_metadata_fixture(&compiler.context, &spec, &proof) == XR_XIR_OK);
        spec.proof = xr_xir_compile_program_proof(proof);
        XrXirProgram *program = NULL;
        CHECK(xr_xir_compile_program_seal(xr_xir_compile_artifact_context(proof), &spec, &program) == XR_XIR_OK);
        xr_xir_compile_artifact_free(proof); proof = NULL; native_fixture_owner_free(&compiler);
        XrXirInstance *instance = NULL;
        XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_new(program, &config, &instance) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_start(instance, 2, NULL, 0) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status == XR_XIR_CALL_RETURNED);
        XrXirValue result = {0};
        CHECK(xr_xir_instance_take_result(instance, &result) == XR_XIR_CALL_RETURNED);
        CHECK(witness.binding_status == (promised ? XR_XIR_CALL_READY : XR_XIR_CALL_BAD_ARGUMENT));
        CHECK(!witness.target_calls && !witness.consumer_calls);
        CHECK(xr_xir_instance_start(instance, 5, NULL, 0) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status == XR_XIR_CALL_RETURNED);
        CHECK(xr_xir_instance_take_result(instance, &result) == XR_XIR_CALL_RETURNED);
        CHECK(witness.binding_status == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_start(instance, 4, &witness.ordinary, 1) == XR_XIR_CALL_BAD_ARGUMENT);
        CHECK(!witness.target_calls && !witness.consumer_calls);
        if (promised) {
            XrXirFunctionBinding *binding = (XrXirFunctionBinding *)xr_xir_function_binding(&witness.strong);
            CHECK(binding); binding->entry = 6;
            CHECK(xr_xir_instance_start_function(instance, &witness.strong, NULL, 0) == XR_XIR_CALL_BAD_ARGUMENT);
            CHECK(xr_xir_instance_start(instance, 4, &witness.strong, 1) == XR_XIR_CALL_BAD_ARGUMENT);
            CHECK(!witness.target_calls && !witness.consumer_calls);
            binding->entry = 3;
            CHECK(xr_xir_instance_start(instance, 4, &witness.strong, 1) == XR_XIR_CALL_READY);
            CHECK(xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status == XR_XIR_CALL_RETURNED);
            CHECK(xr_xir_instance_take_result(instance, &result) == XR_XIR_CALL_RETURNED);
            CHECK(result.type == XR_XIR_I64 && result.payload == 42);
            CHECK(witness.target_calls == 1 && witness.consumer_calls == 1);
        }
        xr_xir_value_drop(&result);
        CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
        xr_xir_compile_program_drop(program); CHECK(!witness.releases);
        xr_xir_value_drop(&witness.strong); xr_xir_value_drop(&witness.ordinary);
        CHECK(witness.releases == 1);
    }
}
#endif // XIR_EFFECT_BINDING_CASES_H
