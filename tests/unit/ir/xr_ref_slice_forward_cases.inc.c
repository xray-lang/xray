/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_ref_slice_forward_cases.inc.c - Exact borrowed descriptor forwarding tests
 */

/* Exact ref Slice forwarding shares a parameter descriptor place. A descriptor
 * borrow does not grant permission to write the borrowed elements. */
#include "../../../src/aot/refine/xr_aot_scalar_ref_v1.h"
#include "../../../src/aot/emit_c/xr_c_scalar_ref_projection.h"
#include "../../../src/plan/semantic/xr_semantic_plan_internal.h"
#include "../../../src/plan/target/xr_target_verify.h"

typedef struct RefSliceForwardFixture {
    XiFunc *ir;
    XrTargetProfile *profile;
    XrTargetPlan *target;
    XrAotScalarRefV1Scope scope;
    uint32_t argument_index;
} RefSliceForwardFixture;

static RefSliceForwardFixture ref_slice_forward_fixture(void) {
    const char *source =
        "fn inspect(src: ref Slice<u8>) -> i64 { return (src[0] as i64) + len(src) }\n"
        "fn forward(src: ref Slice<u8>) -> i64 { return inspect(ref src) }\n"
        "fn twice(src: ref Slice<u8>) -> i64 { return forward(ref src) }\n"
        "fn probe() -> i64 {\n"
        "    var a: Array<u8> = [7 as u8, 8 as u8]\n"
        "    var s: Slice<u8> = a[:]\n"
        "    return twice(ref s)\n"
        "}\n"
        "print(probe())\n";
    RefSliceForwardFixture fixture = {.argument_index = UINT32_MAX};
    fixture.ir = compile_to_ir_with_module_graph_config(source, xi_pipeline_aot_config());
    TEST_REQUIRE(fixture.ir && fixture.ir->semantic_plan,
                 "read-only forwarded Slice source has frozen semantic authority");
    fixture.profile = xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    char error[512] = {0};
    TEST_REQUIRE(fixture.profile && xr_target_plan_build(fixture.ir->semantic_plan, fixture.profile,
                 &fixture.target, error, sizeof(error)), "forwarding has verified Target authority");
    TEST_REQUIRE(xr_aot_scalar_ref_v1_scope_init(&fixture.scope, fixture.ir->semantic_plan,
                 fixture.target), "forwarding scope uses intact frozen Target authority");
    uint32_t forwarded = 0, local_address = 0;
    for (uint32_t i = 0; i < fixture.target->call_arguments_count; i++) {
        const XrTargetCallArgumentRecord *argument = &fixture.target->call_arguments[i];
        const XrTargetCallRecord *call = &fixture.target->calls[argument->call];
        const XrSemanticOperandRecord *operand =
            &fixture.ir->semantic_plan->operands[argument->semantic_operand];
        if (operand->parameter_mode != XR_PARAM_REF)
            continue;
        TEST_REQUIRE(xr_aot_scalar_ref_v1_call_use_in_scope(&fixture.scope,
            call->semantic_operation, (uint16_t) (argument->ordinal + 1),
            argument->semantic_value) == XR_AOT_SCALAR_REF_V1_EXACT,
            "every nested and local ref Slice call is independently exact");
        if (operand->origin == XI_PLACE_ORIGIN_PARAM) {
            forwarded++;
            fixture.argument_index = i;
            const XrTargetSlotRecord *slot = &fixture.target->slots[argument->caller_slot];
            const XrTargetMachineRepRecord *rep =
                &fixture.target->machine_reps[argument->memory_rep];
            TEST_REQUIRE(slot->role == XR_TARGET_SLOT_PARAMETER &&
                         slot->semantic_value == argument->semantic_value &&
                         slot->root_kind == XR_TARGET_ROOT_VIEW_OWNER &&
                         slot->ownership == XR_TARGET_OWNERSHIP_BORROWED &&
                         rep->kind == XR_MACHINE_REP_VIEW &&
                         rep->root_kind == XR_TARGET_ROOT_VIEW_OWNER &&
                         rep->ownership == XR_TARGET_OWNERSHIP_BORROWED &&
                         slot->size == rep->memory_size && slot->align == rep->memory_align,
                         "forwarding retains borrowed descriptor parameter storage");
        } else {
            TEST_REQUIRE(operand->origin == XI_PLACE_ORIGIN_STACK_LOCAL,
                         "outer descriptor call has stack-local place origin");
            local_address++;
        }
    }
    TEST_REQUIRE(forwarded == 2 && local_address == 1 && fixture.argument_index != UINT32_MAX,
                 "fixture includes two parameter hops and one exact local address");
    return fixture;
}

static void ref_slice_expect_call_rejected(const RefSliceForwardFixture *fixture) {
    const XrTargetCallArgumentRecord *argument =
        &fixture->target->call_arguments[fixture->argument_index];
    const XrTargetCallRecord *call = &fixture->target->calls[argument->call];
    TEST_REQUIRE(xr_aot_scalar_ref_v1_call_use_in_scope(&fixture->scope, call->semantic_operation,
        (uint16_t) (argument->ordinal + 1), argument->semantic_value) !=
        XR_AOT_SCALAR_REF_V1_EXACT, "malformed forwarding claim fails closed");
}

static void ref_slice_target_mutations(RefSliceForwardFixture *fixture) {
    XrTargetCallArgumentRecord *argument = &fixture->target->call_arguments[fixture->argument_index];
    XrTargetCallArgumentRecord saved = *argument;
    XrTargetCallRecord *call = &fixture->target->calls[argument->call];
    XrTargetCallRecord saved_call = *call;
    XrTargetSlotRecord *slot = &fixture->target->slots[argument->caller_slot];
    XrTargetSlotRecord saved_slot = *slot;
    XrFingerprint saved_fingerprint = fixture->target->fingerprint;
    for (uint32_t mutation = 0; mutation < 12; mutation++) {
        switch (mutation) {
            case 0: argument->identity.bytes[0] ^= 1; break;
            case 1: argument->caller_slot = argument->callee_slot; break;
            case 2: argument->mode = XR_TARGET_CALL_VALUE; break;
            case 3: argument->ownership = XR_TARGET_CALL_MOVE; break;
            case 4: argument->transfer_mode = XR_TRANSFER_MOVE; break;
            case 5: call->caller_function = call->callee_function; break;
            case 6: call->callee_function = call->caller_function; break;
            case 7: argument->callee_parameter = UINT32_MAX; break;
            case 8: argument->semantic_value = UINT32_MAX; break;
            case 9: slot->root_kind = XR_TARGET_ROOT_NONE; break;
            case 10: slot->ownership = XR_TARGET_OWNERSHIP_TRIVIAL; break;
            case 11: slot->size++; break;
            default: abort();
        }
        ref_slice_expect_call_rejected(fixture);
        xr_target_call_compute_fingerprint(fixture->target, saved.call, &call->fingerprint);
        xr_target_plan_compute_fingerprint(fixture->target, &fixture->target->fingerprint);
        XrCScalarRefProjection projection = {0};
        TEST_REQUIRE(xr_c_scalar_ref_project_argument(fixture->target, argument, &projection) !=
                     XR_C_SCALAR_REF_EXACT, "C argument projection rejects rehashed malformed rows");
        *argument = saved;
        *call = saved_call;
        *slot = saved_slot;
        fixture->target->fingerprint = saved_fingerprint;
    }
}

static void ref_slice_semantic_mutations(RefSliceForwardFixture *fixture) {
    XrSemanticPlan *semantic = fixture->ir->semantic_plan;
    const XrTargetCallArgumentRecord *argument =
        &fixture->target->call_arguments[fixture->argument_index];
    XrSemanticOperandRecord *operand = &semantic->operands[argument->semantic_operand];
    XrSemanticOperandRecord saved_operand = *operand;
    XrSemanticParameterRecord *parameter = NULL;
    for (uint32_t i = 0; i < semantic->parameter_count; i++)
        if (semantic->parameters[i].value == argument->semantic_value)
            parameter = &semantic->parameters[i];
    TEST_REQUIRE(parameter != NULL, "forwarded value names its exact source parameter");
    XrSemanticParameterRecord saved_parameter = *parameter;
    for (uint32_t mutation = 0; mutation < 12; mutation++) {
        switch (mutation) {
            case 0: operand->origin = XI_PLACE_ORIGIN_STACK_LOCAL; break;
            case 1: operand->type = UINT32_MAX; break;
            case 2: operand->parameter_mode = XR_PARAM_READ; break;
            case 3: operand->ownership_action = XR_SEM_OPERAND_CONSUME; break;
            case 4: operand->transfer_mode = XR_TRANSFER_MOVE; break;
            case 5: operand->flags &= ~XR_SEM_OPERAND_ADDRESSABLE; break;
            case 6: parameter->function = UINT32_MAX; break;
            case 7: parameter->type = UINT32_MAX; break;
            case 8: parameter->mode = XR_PARAM_READ; break;
            case 9: parameter->ownership = XI_OWN_OWNED; break;
            case 10: parameter->ordinal = UINT16_MAX; break;
            case 11: parameter->value = UINT32_MAX; break;
            default: abort();
        }
        ref_slice_expect_call_rejected(fixture);
        *operand = saved_operand;
        *parameter = saved_parameter;
    }
    uint32_t saved_index = fixture->argument_index;
    for (uint32_t i = 0; i < fixture->target->call_arguments_count; i++) {
        const XrTargetCallArgumentRecord *candidate = &fixture->target->call_arguments[i];
        XrSemanticOperandRecord *local = &semantic->operands[candidate->semantic_operand];
        if (local->origin != XI_PLACE_ORIGIN_STACK_LOCAL || local->parameter_mode != XR_PARAM_REF)
            continue;
        fixture->argument_index = i;
        uint8_t origin = local->origin;
        local->origin = XI_PLACE_ORIGIN_PARAM;
        ref_slice_expect_call_rejected(fixture);
        local->origin = origin;
    }
    fixture->argument_index = saved_index;
}

static void ref_slice_projection_mutations(const RefSliceForwardFixture *fixture) {
    XrCScalarRefProjection projection = {0};
    const XrTargetCallArgumentRecord *argument =
        &fixture->target->call_arguments[fixture->argument_index];
    TEST_REQUIRE(xr_c_scalar_ref_project_argument(fixture->target, argument, &projection) ==
                 XR_C_SCALAR_REF_EXACT, "forwarded descriptor has exact C projection");
    XrCCallArgumentEmissionView call = projection.call_argument;
    XrCFunctionAbiEmissionView abis[2] = {
        {.semantic_function = projection.function_abi.semantic_function,
         .ordinal = 0, .boundary_kind = XR_C_ABI_BOUNDARY_NATIVE},
        projection.function_abi,
    };
    abis[1].boundary_kind = XR_C_ABI_BOUNDARY_NATIVE;
    XrCFunctionAbiEmissionView saved_abi = abis[1];
    TEST_REQUIRE(xr_c_scalar_ref_projection_views_are_exact(&projection, &call, 1, abis, 2),
                 "forwarded C argument and borrowed pointer ABI agree");
    for (uint32_t mutation = 0; mutation < 8; mutation++) {
        switch (mutation) {
            case 0: call.semantic_value = UINT32_MAX; break;
            case 1: call.mode = XR_TARGET_CALL_VALUE; break;
            case 2: call.c_type = "xr_span_t"; break;
            case 3: abis[1].semantic_value = UINT32_MAX; break;
            case 4: abis[1].slot_class = XR_C_ABI_SLOT_INVALID; break;
            case 5: abis[1].pointee_rep = XR_C_VALUE_REP_I64; break;
            case 6: abis[1].c_type = "void *"; break;
            case 7: abis[1].boundary_kind = XR_C_ABI_BOUNDARY_INVALID; break;
            default: abort();
        }
        TEST_REQUIRE(!xr_c_scalar_ref_projection_views_are_exact(&projection, &call, 1, abis, 2),
                     "C call and signature mutations fail independent view verification");
        call = projection.call_argument;
        abis[1] = saved_abi;
    }
}

TEST(cgen_ref_slice_forward_read_authority) {
    RefSliceForwardFixture fixture = ref_slice_forward_fixture();
    XiRepPolicy policy = xi_rep_policy_native_boundary();
    XrAotRefinementDiagnostic diagnostic = {0};
    XrAotRefinementPlan *refinement = NULL;
    TEST_REQUIRE(xr_aot_representation_refinement_build_from_authority(fixture.target,
        fixture.ir->semantic_plan, &policy, &refinement, &diagnostic),
        "read-only parameter forwarding builds complete representation authority");
    xr_aot_refinement_plan_free(refinement);
    ref_slice_target_mutations(&fixture);
    ref_slice_semantic_mutations(&fixture);
    ref_slice_projection_mutations(&fixture);
    char error[512] = {0};
    TEST_REQUIRE(xr_target_plan_verify(fixture.target, error, sizeof(error)),
                 "mutation probes restored the independently verified target");
    bool had_error = false;
    char *code = generate_c_with_status(fixture.ir, "ref_slice_forward", &had_error);
    TEST_REQUIRE(code && !had_error && strstr(code, "xr_span_t *"),
                 "read-only multi-hop forwarding emits borrowed descriptor ABI");
    if (g_channel_send_c_output) {
        FILE *output = fopen(g_channel_send_c_output, "wb");
        TEST_REQUIRE(output != NULL, "read-only Slice forwarding C output opens");
        size_t length = strlen(code);
        TEST_REQUIRE(fwrite(code, 1, length, output) == length,
                     "read-only Slice forwarding C output is complete");
        TEST_REQUIRE(fclose(output) == 0, "read-only Slice forwarding C output closes");
    }
    xr_free(code);
    xr_target_plan_free(fixture.target);
    xr_target_profile_free(fixture.profile);
    test_func_free(fixture.ir);
}
