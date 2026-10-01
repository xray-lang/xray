/* A frozen yieldable call must carry exactly its registry capability set. */
static void test_native_yieldable_capability_authority(void) {
    XrFunctionParam parameters[] = {{.type = &stub_int, .mode = XR_PARAM_READ}};
    XrType function_type = {
        .kind = XR_KIND_FUNCTION, .id = 803, .frozen = true,
        .function = {.params = parameters, .param_count = 1, .min_params = 1, .return_type = &stub_unit,
                     .throw_effect = XR_FN_EFFECT_NO_THROW},
    };
    XiFunc *function = xi_func_new("native_timer_capability", &stub_unit);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiImportRef imported = {
        .module_path = "time", .member_name = "__sleep", .resolved_mod_index = -1,
        .resolved_shared_slot = -1, .resolved_export_slot = -1, .resolution_attempted = true,
    };
    XiValue *callee = xi_value_new(function, entry, XI_IMPORT_REF, &function_type, 0);
    XiValue *milliseconds = xi_const_int(function, entry, 1, &stub_int);
    XiBlock *suspend = xi_block_new(function);
    REQUIRE(suspend != NULL);
    XiValue *call = xi_value_new(function, suspend, XI_CALL, &stub_unit, 2);
    REQUIRE(callee && milliseconds && call);
    callee->aux = &imported;
    call->args[0] = callee;
    call->args[1] = milliseconds;
    XiBlock *resume = xi_block_new(function);
    REQUIRE(resume != NULL);
    xi_block_set_jump(entry, suspend);
    xi_block_set_jump(suspend, resume);
    xi_block_set_return(resume, NULL);
    XiCoroSuspendPoint point = {.state_id = 1, .op = call, .kind = XI_CORO_SUSP_CALL};
    XiCoroPlan coroutine = {.is_coroutine = true, .nstates = 1, .points = &point};
    function->coro_plan = &coroutine;
    function->stage = XI_STAGE_OPTIMIZED;
    XrSemanticPlan *semantic = NULL;
    char error[512] = {0};
    bool semantic_built = build_target_unit_fixture_semantic(function, &semantic, error, sizeof(error));
    if (!semantic_built)
        fprintf(stderr, "native timer capability SemanticPlan failed: %s\n", error);
    REQUIRE(semantic_built && semantic);
    xi_func_free(function);
    XrTargetProfile *profile = build_profile(0);
    XrTargetPlan *target = NULL;
    bool built = xr_target_plan_build(semantic, profile, &target, error, sizeof(error));
    if (!built)
        fprintf(stderr, "native timer capability TargetPlan failed: %s\n", error);
    REQUIRE(built && target && xr_target_plan_verify(target, error, sizeof(error)));
    const XrStdlibDefEntry *authority = xr_stdlib_metadata_unique_func("time", "__sleep");
    REQUIRE(authority && authority->runtime_capabilities == (XR_CAP_COROUTINE | XR_CAP_TIMER));
    REQUIRE(target->calls_count == 1 &&
            target->calls[0].target_kind == XR_TARGET_CALL_TARGET_NATIVE_YIELDABLE &&
            target->calls[0].runtime_capabilities == authority->runtime_capabilities);
    XrTargetCallRecord saved = target->calls[0];
    XrFingerprint saved_fingerprint = target->fingerprint;
    const uint32_t mutations[] = {0, XR_CAP_COROUTINE, XR_CAP_TIMER,
                                  authority->runtime_capabilities | XR_CAP_NETPOLL};
    for (uint32_t i = 0; i < sizeof(mutations) / sizeof(mutations[0]); i++) {
        target->calls[0].runtime_capabilities = mutations[i];
        xr_target_call_compute_fingerprint(target, 0, &target->calls[0].fingerprint);
        xr_target_plan_compute_fingerprint(target, &target->fingerprint);
        expect_verify_failure(target, "XR_TARGET_1003");
        target->calls[0] = saved;
        target->fingerprint = saved_fingerprint;
    }
    target->calls[0].identity.bytes[0] ^= 1u;
    xr_target_call_compute_fingerprint(target, 0, &target->calls[0].fingerprint);
    xr_target_plan_compute_fingerprint(target, &target->fingerprint);
    expect_verify_failure(target, "XR_TARGET_1003");
    target->calls[0] = saved;
    target->fingerprint = saved_fingerprint;
    REQUIRE(xr_target_plan_verify(target, error, sizeof(error)));
    xr_target_plan_free(target);
    xr_target_profile_free(profile);
    xr_semantic_plan_free(semantic);
}
