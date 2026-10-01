/* DNS offload consumes one exact fresh native yieldable call, never a spelling. */
static void test_native_resolve_call_authority(void) {
    XrType result_type = {.kind = XR_KIND_ARRAY, .id = 804, .frozen = true,
                          .scalar_rep = XR_SCALAR_REP_NONE,
                          .container = {.element_type = &stub_exact_string}};
    XrFunctionParam parameters[] = {{.type = &stub_exact_string, .mode = XR_PARAM_READ}};
    XrType function_type = {.kind = XR_KIND_FUNCTION, .id = 805, .frozen = true,
                           .scalar_rep = XR_SCALAR_REP_NONE,
                           .function = {.params = parameters, .param_count = 1, .min_params = 1,
                                        .return_type = &result_type,
                                        .throw_effect = XR_FN_EFFECT_MAY_THROW}};
    XiFunc *function = xi_func_new("native_resolve_authority", &stub_unit);
    REQUIRE(function != NULL);
    XiBlock *before = xi_block_new(function), *suspend = xi_block_new(function);
    XiBlock *resume = xi_block_new(function);
    REQUIRE(before && suspend && resume);
    XiImportRef imported = {.module_path = "net", .member_name = "__resolveAll",
                            .resolved_mod_index = -1, .resolved_shared_slot = -1,
                            .resolved_export_slot = -1, .resolution_attempted = true};
    XiValue *callee = xi_value_new(function, before, XI_IMPORT_REF, &function_type, 0);
    XiValue *hostname = xi_const_str(function, before, "127.0.0.1", &stub_exact_string);
    XiValue *call = xi_value_new(function, suspend, XI_CALL, &result_type, 2);
    REQUIRE(callee && hostname && call);
    callee->aux = &imported;
    call->args[0] = callee;
    call->args[1] = hostname;
    call->call_return_ownership = (XiReturnOwnership) {
        .kind = XI_RETURN_OWNERSHIP_OWNED, .param_index = -1, .complete = true};
    XiCallPlan call_plan = {.verified = true, .nargs = 1};
    XiCallArgPlan argument = {.param_mode = XR_PARAM_READ, .access = XR_CALL_ARG_PLAIN,
                             .origin_var_id = XI_NO_VAR_ID};
    call_plan.args = &argument;
    call->call_plan = &call_plan;
    XiValue *release = xi_value_new(function, resume, XI_RELEASE, &stub_unit, 1);
    REQUIRE(release != NULL);
    release->args[0] = call;
    release->flags |= XI_FLAG_SIDE_EFFECT;
    xi_block_set_jump(before, suspend);
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
        fprintf(stderr, "DNS SemanticPlan failed: %s\n", error);
    REQUIRE(semantic_built && semantic);
    xi_func_free(function);
    XrTargetProfile *profile = build_profile(0);
    XrTargetPlan *target = NULL;
    bool built = xr_target_plan_build(semantic, profile, &target, error, sizeof(error));
    if (!built)
        fprintf(stderr, "DNS TargetPlan failed: %s\n", error);
    REQUIRE(built && target && xr_target_plan_verify(target, error, sizeof(error)));
    const XrStdlibDefEntry *authority = xr_stdlib_metadata_unique_func("net", "__resolveAll");
    REQUIRE(authority && authority->runtime_capabilities == (XR_CAP_COROUTINE | XR_CAP_NETPOLL) &&
            target->calls_count == 1 && target->coroutines_count == 1 &&
            target->calls[0].runtime_capabilities == authority->runtime_capabilities &&
            target->calls[0].result_ownership == XR_TARGET_CALL_RETURN_OWNED);
    XrTargetCallRecord saved = target->calls[0];
    XrFingerprint saved_fingerprint = target->fingerprint;
    for (unsigned mutation = 0; mutation < 9; mutation++) {
        XrTargetCallRecord *row = &target->calls[0];
        switch (mutation) {
            case 0: row->runtime_capabilities = 0; break;
            case 1: row->runtime_capabilities = XR_CAP_COROUTINE; break;
            case 2: row->runtime_capabilities |= XR_CAP_TIMER; break;
            case 3: row->result_ownership = XR_TARGET_CALL_BORROW; break;
            case 4: row->target_kind = XR_TARGET_CALL_TARGET_NATIVE_DIRECT; break;
            case 5: row->calling_convention = XR_TARGET_CALL_CONVENTION_NATIVE_DIRECT; break;
            case 6: row->source_dependency = 0; break;
            case 7: row->source_callee_identity.bytes[0] ^= 1u; break;
            default: row->semantic_call_target = XR_SEMANTIC_INDEX_NONE; break;
        }
        xr_target_call_compute_fingerprint(target, 0, &row->fingerprint);
        xr_target_plan_compute_fingerprint(target, &target->fingerprint);
        expect_verify_failure(target, "XR_TARGET_1003");
        *row = saved;
        target->fingerprint = saved_fingerprint;
    }
    REQUIRE(xr_target_plan_verify(target, error, sizeof(error)));
    xr_target_plan_free(target);
    xr_target_profile_free(profile);
    xr_semantic_plan_free(semantic);
}
