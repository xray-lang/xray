/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_plain_ref_aggregate_cases.inc.c - Exact pointer-free aggregate borrow tests
 */

#include "../../../src/plan/target/xr_target_verify.h"
#include "../../../src/plan/format/xr_xtp_schema.h"
#include "../../../src/plan/format/xr_xtp_internal.h"
#include "../../../src/base/xsha256.h"
#include "../../../src/plan/semantic/xr_semantic_plan_internal.h"

static const char *plain_ref_aggregate_source =
    "struct Lanes { data: [u64; 4] }\n"
    "@noinline\n"
    "fn pick(view: ref Lanes) -> u64 { return view.data[1] }\n"
    "var lanes = Lanes{data: [1, 2, 3, 4]}\n"
    "print(pick(ref lanes))\n";

static void plain_ref_target_rehash(XrTargetPlan *target) {
    for (uint32_t i = 0; i < target->calls_count; i++)
        xr_target_call_compute_fingerprint(target, i, &target->calls[i].fingerprint);
    xr_target_plan_compute_fingerprint(target, &target->fingerprint);
}

static void plain_ref_artifact_section(uint8_t *bytes, XrXtpSectionKind kind,
    const void *rows, uint32_t count) {
    uint8_t *entry = bytes + XR_XTP_HEADER_SIZE + ((size_t) kind - 1u) * XR_XTP_DIRECTORY_ENTRY_SIZE;
    size_t offset = (size_t) xr_xtp_take_u64(entry + 8);
    size_t length = (size_t) xr_xtp_take_u64(entry + 16);
    TEST_REQUIRE(xr_xtp_take_u64(entry + 24) == count &&
        xr_xtp_encode_rows(kind, rows, count, bytes + offset), "independent wire mutation is canonical");
    xr_sha256(bytes + offset, length, entry + 40);
}

static void plain_ref_artifact_rejected(XrTargetPlan *target, XrTargetProfile *profile,
    const XrSemanticPlan *semantic, const uint8_t *baseline, size_t size) {
    uint8_t *bytes = (uint8_t *) xr_malloc(size);
    TEST_REQUIRE(bytes != NULL, "serialized negative copy allocated");
    memcpy(bytes, baseline, size);
    plain_ref_artifact_section(bytes, XR_XTP_SECTION_CALLS, target->calls, target->calls_count);
    plain_ref_artifact_section(bytes, XR_XTP_SECTION_CALL_ARGUMENTS, target->call_arguments,
        target->call_arguments_count);
    plain_ref_artifact_section(bytes, XR_XTP_SECTION_SLOTS, target->slots, target->slots_count);
    plain_ref_artifact_section(bytes, XR_XTP_SECTION_MACHINE_REPS, target->machine_reps,
        target->machine_reps_count);
    plain_ref_artifact_section(bytes, XR_XTP_SECTION_LAYOUTS, target->layouts, target->layouts_count);
    plain_ref_artifact_section(bytes, XR_XTP_SECTION_FIELDS, target->fields, target->fields_count);
    memcpy(bytes + 168, target->fingerprint.bytes, XR_FINGERPRINT_BYTES);
    memset(bytes + XR_XTP_FULL_DIGEST_OFFSET, 0, XR_FINGERPRINT_BYTES);
    xr_sha256(bytes, size, bytes + XR_XTP_FULL_DIGEST_OFFSET);
    char error[512] = {0};
    XrXtpCandidate *candidate = NULL;
    XrTargetPlan *published = NULL;
    TEST_REQUIRE(xr_xtp_decode_candidate(bytes, size, &candidate, error, sizeof(error)),
        "valid section and artifact digests reach the independent physical reader");
    TEST_REQUIRE(!xr_xtp_materialize_target_plan(candidate, semantic, profile, &published,
        error, sizeof(error)) && published == NULL,
        "valid-rehash serialized authority cannot publish a forged aggregate borrow");
    xr_xtp_candidate_release(candidate);
    xr_free(bytes);
}

static void plain_ref_target_rejected(XrTargetPlan *target, XrTargetProfile *profile,
                                      const XrSemanticPlan *semantic) {
    char error[512] = {0};
    plain_ref_target_rehash(target);
    TEST_REQUIRE(xr_target_plan_fingerprint_is_intact(target), "negative Target digest is valid");
    TEST_REQUIRE(!xr_target_plan_verify(target, error, sizeof(error)),
                 "independent Target reader rejects the rehashed forged borrow");
    uint8_t *bytes = NULL;
    size_t size = 0;
    TEST_REQUIRE(!xr_xtp_encode_plan(target, &bytes, &size, error, sizeof(error)) && !bytes,
                 "artifact publication independently rejects malformed authority");
    XrAotRefinementPlan *refinement = NULL;
    XrAotRefinementDiagnostic diag = {0};
    XiRepPolicy policy = xi_rep_policy_native_boundary();
    TEST_REQUIRE(!xr_aot_representation_refinement_build_from_authority(target, semantic,
        &policy, &refinement, &diag) && !refinement,
        "representation admission rejects the same rehashed forged authority");
    XrCEmissionPlan *emission = NULL;
    TEST_REQUIRE(!xr_c_emission_plan_build(target, semantic, xr_target_profile_fingerprint(profile),
        &emission, error, sizeof(error)) && !emission,
        "C emission independently rejects forged physical authority before publication");
}

static uint64_t plain_ref_expected_layout_key(const XrTargetProfile *profile) {
    const XrTargetMachineFacts *facts = xr_target_profile_machine_facts(profile);
    const char *names[] = {"data"};
    XrAggregateLayout layout = {0};
    layout.target_abi_hash = facts->data_layout.stable_hash;
    layout.nominal_name = "Lanes";
    layout.kind = XR_AGG_LAYOUT_STRUCT;
    layout.total_size = 32;
    layout.alignment = 8;
    layout.field_count = 1;
    layout.field_names = names;
    layout.fields[0].offset = 0;
    layout.fields[0].size = 32;
    layout.fields[0].native_type = XR_NATIVE_ARRAY;
    layout.fields[0].elem_native_type = XR_NATIVE_U64;
    layout.fields[0].elem_count = 4;
    return xr_aggregate_layout_stable_key(&layout);
}

static void plain_ref_source_load_mutations(XrTargetPlan *target, XrTargetProfile *profile) {
    XrSemanticPlan *semantic = (XrSemanticPlan *) xr_target_plan_semantic_plan(target);
    TEST_REQUIRE(semantic != NULL, "test mutations identify the exact primary Source owner");
    const XrTargetCallArgumentRecord *argument = &target->call_arguments[0];
    const XrSemanticParameterRecord *parameter = &semantic->parameters[argument->callee_parameter];
    XrFingerprint source_fingerprint = semantic->fingerprint;
    XrFingerprint target_semantic_fingerprint = target->semantic_fingerprint;
    XrFingerprint target_fingerprint = target->fingerprint;
    TEST_REQUIRE(target->module_partitions_count == 0 && target->semantic_module_count == 0,
        "fixture uses the exact single-module Target representation");
    XrTargetCallRecord original_call = target->calls[argument->call];
    uint32_t loads[2] = {UINT32_MAX, UINT32_MAX};
    for (uint32_t i = 0; i < semantic->operation_count; i++) {
        const XrSemanticOperationRecord *operation = &semantic->operations[i];
        if (operation->opcode != XI_PLACE_LOAD || operation->operand_count != 1)
            continue;
        uint32_t value = semantic->operands[operation->operand_begin].value;
        if (value == parameter->value)
            loads[0] = i;
        else if (value == argument->semantic_value)
            loads[1] = i;
    }
    TEST_REQUIRE(loads[0] != UINT32_MAX && loads[1] != UINT32_MAX,
        "callee load and caller copyback are independent Source records");
    for (uint32_t side = 0; side < 2; side++) {
        XrSemanticOperationRecord *load = &semantic->operations[loads[side]];
        XrSemanticOperandRecord *operand = &semantic->operands[load->operand_begin];
        XrSemanticOperationRecord saved_load = *load;
        XrSemanticOperandRecord saved_operand = *operand;
        for (uint32_t mutation = 0; mutation < 8; mutation++) {
            switch (mutation) {
                case 0: load->result_type = UINT32_MAX; break;
                case 1: load->operand_count = 2; break;
                case 2: operand->type = UINT32_MAX; break;
                case 3: operand->lifetime = XI_PLACE_LIFETIME_CALL_BOUND; break;
                case 4: operand->origin = XI_PLACE_ORIGIN_STACK_LOCAL; break;
                case 5: operand->access = XR_CALL_ARG_REF; break;
                case 6: load->function = saved_load.function == 0 ? 1 : 0; break;
                case 7: load->result_ownership = XI_GEN_RESULT_OWNERSHIP_OWNED; break;
                default: abort();
            }
            xr_semantic_plan_compute_fingerprint(semantic, &semantic->fingerprint);
            target->semantic_fingerprint = semantic->fingerprint;
            plain_ref_target_rejected(target, profile, semantic);
            *load = saved_load;
            *operand = saved_operand;
            semantic->fingerprint = source_fingerprint;
            target->semantic_fingerprint = target_semantic_fingerprint;
            target->calls[argument->call] = original_call;
            target->fingerprint = target_fingerprint;
        }
    }
}

static void plain_ref_emission_mutations(XrCEmissionPlan *emission, XrTargetPlan *target,
    XrTargetProfile *profile, uint32_t parameter_value, const XaotBundle *bundle) {
    char error[512] = {0};
    const XrSemanticPlan *semantic = xr_target_plan_semantic_plan(target);
    uint32_t value_index = UINT32_MAX, abi_index = UINT32_MAX;
    for (uint32_t i = 0; i < emission->value_count; i++)
        if (emission->values[i].semantic_value == parameter_value)
            value_index = i;
    for (uint32_t i = 0; i < emission->function_abi_count; i++)
        if (emission->function_abis[i].semantic_value == parameter_value)
            abi_index = i;
    TEST_REQUIRE(value_index != UINT32_MAX && abi_index != UINT32_MAX &&
                 emission->call_argument_count == 1, "all borrow projection rows exist");
    XrCValueEmissionView *value = &emission->values[value_index];
    XrCFunctionAbiEmissionView *abi = &emission->function_abis[abi_index];
    XrCValueEmissionView saved_value = *value;
    XrCFunctionAbiEmissionView saved_abi = *abi;
    XrCCallArgumentEmissionView saved_argument = emission->call_arguments[0];
    XrFingerprint saved_fingerprint = emission->fingerprint;
    char forged_type[40];
    memcpy(forged_type, value->c_type, strlen(value->c_type) + 1u);
    forged_type[15] = forged_type[15] == '0' ? '1' : '0';
    for (uint32_t mutation = 0; mutation < 12; mutation++) {
        switch (mutation) {
            case 0: value->c_type = forged_type; break;
            case 1: value->target_register_rep = UINT16_MAX; break;
            case 2: value->semantic_value = UINT32_MAX; break;
            case 3: value->memory_size++; break;
            case 4: abi->c_type = forged_type; break;
            case 5: abi->pointee_c_type = "xrt_struct_abi_0000000000000000"; break;
            case 6: abi->slot_class = XR_C_ABI_SLOT_VALUE; break;
            case 7: abi->pointee_rep = XR_C_VALUE_REP_TAGGED; break;
            case 8: emission->call_arguments[0].c_type = forged_type; break;
            case 9: emission->call_arguments[0].callee_parameter = UINT32_MAX; break;
            case 10: emission->call_arguments[0].semantic_operand = UINT32_MAX; break;
            case 11: emission->call_arguments[0].caller_memory_kind = XR_MACHINE_REP_RAW_PTR; break;
            default: abort();
        }
        xr_c_emission_plan_compute_fingerprint(emission, &emission->fingerprint);
        TEST_REQUIRE(!xr_c_emission_plan_verify(emission, target, semantic,
            xr_target_profile_fingerprint(profile), error, sizeof(error)),
            "valid-rehash C emission rows cannot forge the pointee hash, value, geometry or ABI");
        if (bundle) {
            XiCgenCtx *consumer = xi_cgen_ctx_new();
            const XrCEmissionPlan *plans[] = {emission};
            TEST_REQUIRE(consumer && xi_cgen_ctx_set_aot_bundle(consumer, bundle) &&
                !xi_cgen_ctx_set_value_emission_plans(consumer, plans, 1) &&
                xi_cgen_has_error(consumer),
                "real CGen registry refuses every forged type, value or ABI projection");
            xi_cgen_ctx_free(consumer);
        }
        *value = saved_value;
        *abi = saved_abi;
        emission->call_arguments[0] = saved_argument;
        emission->fingerprint = saved_fingerprint;
    }
    TEST_REQUIRE(xr_c_emission_plan_verify(emission, target, semantic,
        xr_target_profile_fingerprint(profile), error, sizeof(error)), "restored C authority verifies");
}

TEST(cgen_plain_ref_aggregate_authority) {
    XiFunc *producer = compile_to_ir(plain_ref_aggregate_source);
    TEST_REQUIRE(producer && producer->semantic_plan, "exact Source fixture is frozen");
    require_detached_semantic_snapshot(producer);
    XrTargetProfile *profile = xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    XrTargetPlan *target = NULL;
    char error[512] = {0};
    TEST_REQUIRE(profile && xr_target_plan_build(producer->semantic_plan, profile, &target,
        error, sizeof(error)), "plain aggregate has exact physical authority");
    test_func_free(producer);
    producer = NULL;
    const XrSemanticPlan *semantic = xr_target_plan_semantic_plan(target);
    TEST_REQUIRE(semantic && xr_target_plan_verify(target, error, sizeof(error)),
                 "Target reader succeeds after the Xi and analyzer producers are destroyed");
    TEST_REQUIRE(target->call_arguments_count == 1, "fixture has exactly one ref boundary");
    XrTargetCallArgumentRecord *argument = &target->call_arguments[0];
    uint32_t parameter_value = target->slots[argument->callee_slot].semantic_value;
    XrCAggregateProjection projection = {0};
    TEST_REQUIRE(xr_c_plain_ref_aggregate_argument_projection(target, argument, &projection) &&
        projection.abi_key == plain_ref_expected_layout_key(profile),
        "fixed-array element fingerprint agrees with independent explicit runtime layout");
    XrAotRefinementPlan *refinement = NULL;
    XrAotRefinementDiagnostic diag = {0};
    XiRepPolicy policy = xi_rep_policy_native_boundary();
    bool refined = xr_aot_representation_refinement_build_from_authority(target, semantic,
        &policy, &refinement, &diag);
    if (!refined)
        fprintf(stderr, "plain-ref refinement refused code=%u value=%u operation=%u\n",
            (unsigned) diag.issue, diag.semantic_value, diag.semantic_operation);
    TEST_REQUIRE(refined && refinement, "parameter, local address, load and call replay independently");
    xr_aot_refinement_plan_free(refinement);
    XrCEmissionPlan *emission = NULL;
    TEST_REQUIRE(xr_c_emission_plan_build(target, semantic, xr_target_profile_fingerprint(profile),
        &emission, error, sizeof(error)), "receiving emission plan owns the exact type names");
    plain_ref_emission_mutations(emission, target, profile, parameter_value, NULL);
    plain_ref_source_load_mutations(target, profile);
    XrTargetCallArgumentRecord saved_argument = *argument;
    XrTargetSlotRecord *slot = &target->slots[argument->callee_slot];
    XrTargetSlotRecord saved_slot = *slot;
    XrTargetMachineRepRecord *rep = &target->machine_reps[argument->callee_register_rep];
    XrTargetMachineRepRecord saved_rep = *rep;
    XrTargetLayoutRecord *layout = &target->layouts[projection.layout];
    XrTargetLayoutRecord saved_layout = *layout;
    XrTargetFieldRecord *field = &target->fields[layout->field_begin];
    XrTargetFieldRecord saved_field = *field;
    XrTargetLayoutRecord *lanes = &target->layouts[target->machine_reps[field->memory_rep].detail];
    XrTargetMachineRepRecord *lane_rep = &target->machine_reps[target->fields[lanes->field_begin].memory_rep];
    XrTargetMachineRepRecord saved_lane_rep = *lane_rep;
    XrTargetCallRecord saved_call = target->calls[argument->call];
    XrFingerprint saved_fingerprint = target->fingerprint;
    uint8_t *baseline = NULL;
    size_t baseline_size = 0;
    TEST_REQUIRE(xr_xtp_encode_plan(target, &baseline, &baseline_size, error, sizeof(error)),
        "correct artifact is the independent mutation baseline");
    for (uint32_t mutation = 0; mutation < 20; mutation++) {
        switch (mutation) {
            case 0: argument->caller_slot = argument->callee_slot; break;
            case 1: argument->callee_slot = argument->caller_slot; break;
            case 2: argument->callee_parameter = UINT32_MAX; break;
            case 3: argument->mode = XR_TARGET_CALL_VALUE; break;
            case 4: argument->ownership = XR_TARGET_CALL_MOVE; break;
            case 5: argument->transfer_mode = XR_TRANSFER_MOVE; break;
            case 6: argument->semantic_operand++; break;
            case 7: target->calls[argument->call].callee_function = target->calls[argument->call].caller_function; break;
            case 8: slot->root_kind = XR_TARGET_ROOT_OBJECT; break;
            case 9: slot->ownership = XR_TARGET_OWNERSHIP_TRIVIAL; break;
            case 10: slot->size++; break;
            case 11: rep->kind = XR_MACHINE_REP_DYN_VALUE; break;
            case 12: rep->ownership = XR_TARGET_OWNERSHIP_TRIVIAL; break;
            case 13: layout->fixed_prefix_size++; break;
            case 14: field->semantic_field++; break;
            case 15: field->offset++; break;
            case 16: lane_rep->signedness = XR_TARGET_SIGN_SIGNED; break;
            case 17: lane_rep->null_encoding = XR_TARGET_NULL_ZERO; break;
            case 18: lane_rep->register_bits++; break;
            case 19: lane_rep->detail++; break;
            default: abort();
        }
        plain_ref_target_rejected(target, profile, semantic);
        plain_ref_artifact_rejected(target, profile, semantic, baseline, baseline_size);
        *argument = saved_argument;
        *slot = saved_slot;
        *rep = saved_rep;
        *layout = saved_layout;
        *field = saved_field;
        *lane_rep = saved_lane_rep;
        target->calls[argument->call] = saved_call;
        target->fingerprint = saved_fingerprint;
    }
    xr_xtp_encoded_free(baseline);
    uint8_t *encoded = NULL;
    size_t encoded_size = 0;
    XrXtpCandidate *candidate = NULL;
    XrTargetPlan *decoded = NULL;
    TEST_REQUIRE(xr_xtp_encode_plan(target, &encoded, &encoded_size, error, sizeof(error)) &&
        xr_xtp_decode_candidate(encoded, encoded_size, &candidate, error, sizeof(error)) &&
        xr_xtp_materialize_target_plan(candidate, semantic, profile, &decoded, error, sizeof(error)),
        "independent artifact reader reconstructs the exact borrow after producer destruction");
    XrCAggregateProjection decoded_projection = {0};
    TEST_REQUIRE(xr_c_plain_ref_aggregate_argument_projection(decoded, &decoded->call_arguments[0],
        &decoded_projection) && decoded_projection.abi_key == projection.abi_key &&
        strcmp(decoded_projection.c_type, projection.c_type) == 0,
        "artifact projection preserves the independent fixed-array fingerprint");
    xr_target_plan_free(decoded);
    xr_xtp_candidate_release(candidate);
    xr_xtp_encoded_free(encoded);
    xr_c_emission_plan_free(emission);
    xr_target_plan_free(target);
    xr_target_profile_free(profile);
}

TEST(cgen_plain_ref_aggregate_consumer_authority) {
    XiFunc *backend = compile_to_ir(plain_ref_aggregate_source);
    TEST_REQUIRE(backend && test_prepare_backend_ir(backend), "real backend receiver is prepared");
    XiModule *module = xi_module_new("test.xr", "plain_ref", backend);
    TEST_REQUIRE(module && xi_module_set_identity(module,
        "memory-module-v1:id=18:xi-cgen-fixture-v1"), "real module has exact Source identity");
    XiModule *modules[] = {module};
    TestAotPlan prepared;
    test_aot_plan_prepare(&prepared, modules, 1, 0);
    TEST_REQUIRE(prepared.nemission_plans == 1 && prepared.bundle.program_target_plan,
        "prepared call closure has one receiving C authority");
    XrTargetPlan *target = prepared.bundle.program_target_plan;
    const XrTargetCallArgumentRecord *argument = &target->call_arguments[0];
    XrCEmissionPlan *emission = (XrCEmissionPlan *) prepared.emission_plans[0];
    plain_ref_emission_mutations(emission, target, (XrTargetProfile *) xr_target_plan_profile(target),
        target->slots[argument->callee_slot].semantic_value, &prepared.bundle);
    XiCgenCtx *consumer = xi_cgen_ctx_new();
    TEST_REQUIRE(consumer && xi_cgen_ctx_set_aot_bundle(consumer, &prepared.bundle) &&
        xi_cgen_ctx_set_value_emission_plans(consumer, prepared.emission_plans, 1),
        "restored exact immutable projection is admitted by the real CGen registry");
    char *code = NULL;
    size_t size = 0;
    FILE *stream = xr_open_memstream(&code, &size);
    TEST_REQUIRE(stream != NULL, "real consumer output stream allocated");
    xi_cgen_program(consumer, stream, module);
    TEST_REQUIRE(xr_close_memstream(stream, &code, &size) == 0 && code &&
        !xi_cgen_has_error(consumer) && contains(code, "->data[v"),
        "real consumer emits the restored exact aggregate boundary");
    xr_free(code);
    xi_cgen_ctx_free(consumer);
    test_aot_plan_free(&prepared);
    module->init = NULL;
    backend->module = NULL;
    xi_module_free(module);
    test_func_free(backend);
}
