/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_callable52_source_facts.c - Owned typed Source callable and body authority
 */

#include "../test_framework.h"
#include "xray_vm.h"
#include "target_profile_test_fixture.h"
#include "runtime/xisolate_api.h"
#include "ir/xi.h"
#include "ir/xi_core_api.h"
#include "toolchain/xcompiler_session.h"
#include "plan/semantic/xr_semantic_plan_internal.h"
#include "plan/semantic/xr_semantic_callable_key_shape.h"
#include "plan/semantic/xr_semantic_function_callable_shape.h"
#include "plan/semantic/xr_semantic_constructor_callable_shape.h"
#include "plan/semantic/xr_semantic_coroutine_module_shape.h"
#include "plan/format/xr_xsm_schema.h"
#include "base/xmalloc.h"
#include "module/xmodule_identity.h"
#include "runtime/value/xchunk.h"

TEST(typed_source_lambda_has_owned_complete_callable_fact) {
    XrVMConfig config = {0};
    XrVMRuntime *isolate = xray_vm_new_full(&config);
    ASSERT_NOT_NULL(isolate);
    XrCompilerSession *session = xr_compiler_session_current_for_isolate(isolate);
    XrTargetProfile *profile = xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    ASSERT_NOT_NULL(profile);
    ASSERT_TRUE(xr_compiler_session_set_target_profile(session, profile));
    xr_target_profile_free(profile);
    const XrModuleIdentityAuthority authority = {
        .kind = XR_MODULE_IDENTITY_MEMORY, .namespace_id = "callable52-source-fact-v1"};
    XrProto *proto = xr_compile_source_with_path(session,
        "const cb = (lane: i64) -> lane\nvar answer = cb(3)\n", NULL, &authority);
    ASSERT_NOT_NULL(proto);
    XiFunc *root = (XiFunc *) proto->xi_func;
    ASSERT_NOT_NULL(root);
    ASSERT_TRUE(root->semantic_snapshot_detached);
    ASSERT_TRUE(PROTO_PROTO_COUNT(proto) == 1);
    XiFunc *lambda = (XiFunc *) PROTO_PROTO(proto, 0)->xi_func;
    ASSERT_NOT_NULL(lambda);
    const XrType *callable = lambda->source_callable_type;
    ASSERT_NOT_NULL(callable);
    ASSERT_TRUE(xi_func_arena_contains(root, callable, sizeof(*callable)));
    ASSERT_EQ_INT(callable->kind, XR_KIND_FUNCTION);
    ASSERT_EQ_INT(callable->function.type_param_count, 0);
    ASSERT_EQ_INT(callable->function.receiver_mode, XR_PARAM_READ);
    ASSERT_EQ_INT(callable->function.param_count, 1);
    ASSERT_EQ_INT(callable->function.min_params, 1);
    ASSERT_EQ_INT(callable->function.throw_effect, XR_FN_EFFECT_NO_THROW);
    ASSERT_TRUE(!callable->function.is_c_abi && !callable->function.is_variadic);
    ASSERT_EQ_INT(callable->function.view_origin_count, 0);
    ASSERT_TRUE(!callable->function.view_origin_was_elided);
    ASSERT_TRUE(lambda->analyzer_effect_complete && lambda->error_effect_nothrow);
    ASSERT_EQ_INT(lambda->effect_unknown_reasons, 0);
    ASSERT_EQ_INT(lambda->unknown_semantic_effects, 0);
    const XrSemanticPlan *plan = root->semantic_plan;
    ASSERT_NOT_NULL(plan);
    const XrSemanticFunctionRecord *fn = xr_semantic_plan_function(plan, 1);
    ASSERT_NOT_NULL(fn);
    ASSERT_TRUE(fn->effect_complete == 1u);
    ASSERT_EQ_INT(fn->unknown_semantic_effects, 0);
    ASSERT_EQ_INT(fn->effect_unknown_reasons, 0);
    ASSERT_TRUE(fn->callable_type < xr_semantic_plan_type_count(plan));
    const XrSemanticTypeRecord *type = xr_semantic_plan_type(plan, fn->callable_type);
    XrSemanticCallableKeyShape shape = {0};
    ASSERT_TRUE(xr_semantic_callable_key_parse(type->canonical_key, &shape));
    ASSERT_EQ_INT(shape.parameter_count, 1);
    ASSERT_EQ_INT(shape.receiver_mode, XR_PARAM_READ);
    ASSERT_EQ_INT(shape.generic_parameter_count, 0);
    ASSERT_EQ_INT(shape.throw_effect, XR_FN_EFFECT_NO_THROW);
    ASSERT_TRUE(strstr(fn->canonical_key, "function-v4:") == fn->canonical_key);
    ASSERT_TRUE(strstr(fn->canonical_key, ":callable=") != NULL);
    ASSERT_TRUE(xr_semantic_function_callable_shape_is_exact(plan, fn));
    XrSemanticFunctionRecord counterfeit = *fn;
    counterfeit.callable_type = fn->return_type;
    ASSERT_TRUE(!xr_semantic_function_callable_shape_is_exact(plan, &counterfeit));
    counterfeit = *fn;
    counterfeit.parameter_count = 0;
    ASSERT_TRUE(!xr_semantic_function_callable_shape_is_exact(plan, &counterfeit));
    counterfeit = *fn;
    counterfeit.unknown_semantic_effects = 1;
    ASSERT_TRUE(!xr_semantic_function_callable_shape_is_exact(plan, &counterfeit));
    counterfeit = *fn;
    counterfeit.effect_unknown_reasons = 1;
    ASSERT_TRUE(!xr_semantic_function_callable_shape_is_exact(plan, &counterfeit));
    counterfeit = *fn;
    counterfeit.effect_complete = 2;
    ASSERT_TRUE(!xr_semantic_function_callable_shape_is_exact(plan, &counterfeit));
    for (uint32_t op = 0; op < xr_semantic_plan_operation_count(plan); op++) {
        const XrSemanticOperationRecord *row = xr_semantic_plan_operation(plan, op);
        if (row->function == 1 || row->callable_function == 1)
            printf("body-op index=%u fn=%u opcode=%u effects=%u result=%u type=%u callable=%u imm=%lld nargs=%u flags=%u\n",
                op, row->function, row->opcode, row->effects, row->result_value,
                row->result_type, row->callable_function, (long long) row->semantic_immediate,
                row->operand_count, row->flags);
    }
    for (uint32_t b = fn->block_begin; b < fn->block_begin + fn->block_count; b++) {
        const XrSemanticBlockRecord *row = xr_semantic_plan_block(plan, b);
        printf("body-block index=%u kind=%u operations=%u+%u control=%u\n",
            b, row->kind, row->operation_begin, row->operation_count, row->control_value);
    }
    uint8_t *wire = NULL;
    size_t size = 0;
    char error[512] = {0};
    ASSERT_TRUE(xr_xsm_encode(plan, &wire, &size, error, sizeof(error)));
    XrSemanticPlan *decoded = NULL;
    ASSERT_TRUE(xr_xsm_decode(wire, size, &decoded, error, sizeof(error)));
    ASSERT_EQ_INT(xr_semantic_plan_function(decoded, 1)->callable_type, fn->callable_type);
    xr_semantic_plan_free(decoded);
    decoded = NULL;
    ASSERT_EQ_INT(wire[8], 52);
    wire[8] = 51;
    ASSERT_TRUE(!xr_xsm_decode(wire, size, &decoded, error, sizeof(error)));
    ASSERT_TRUE(decoded == NULL && strstr(error, "XR_ARTIFACT_2000") != NULL);
    wire[8] = 52;
    ASSERT_TRUE(xr_xsm_decode(wire, size, &decoded, error, sizeof(error)));
    xr_semantic_plan_free(decoded);
    xr_free(wire);
    printf("layout XiFunc=%zu FunctionRecord=%zu schema=%u wire=%zu\n",
        sizeof(XiFunc), sizeof(XrSemanticFunctionRecord), XR_SEMANTIC_SCHEMA_VERSION, size);
    xr_free_code(isolate, proto);
    xray_vm_delete(isolate);
}

static XrProto *compile_callable_body_probe(const char *source, XrVMRuntime **out) {
    XrVMConfig config = {0};
    XrVMRuntime *isolate = xray_vm_new_full(&config);
    if (!isolate)
        return NULL;
    XrCompilerSession *session = xr_compiler_session_current_for_isolate(isolate);
    XrTargetProfile *profile = xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    if (!profile || !xr_compiler_session_set_target_profile(session, profile)) {
        xr_target_profile_free(profile);
        xray_vm_delete(isolate);
        return NULL;
    }
    xr_target_profile_free(profile);
    const XrModuleIdentityAuthority authority = {
        .kind = XR_MODULE_IDENTITY_MEMORY, .namespace_id = "callable52-body-proof-v1"};
    XrProto *proto = xr_compile_source_with_path(session, source, NULL, &authority);
    if (!proto) {
        xray_vm_delete(isolate);
        return NULL;
    }
    *out = isolate;
    return proto;
}

TEST(real_source_scalar_body_proves_ctor_callback_without_effect_bit_authority) {
    XrVMRuntime *actual_isolate = NULL, *formal_isolate = NULL;
    XrProto *actual_proto = compile_callable_body_probe(
        "const cb = (lane: i64) -> lane\nvar answer = cb(3)\n", &actual_isolate);
    XrProto *formal_proto = compile_callable_body_probe(
        "fn accept(cb: fn(i64) -> i64) { }\n", &formal_isolate);
    ASSERT_NOT_NULL(actual_proto);
    ASSERT_NOT_NULL(formal_proto);
    const XrSemanticPlan *actual = ((XiFunc *) actual_proto->xi_func)->semantic_plan;
    const XrSemanticPlan *formal = ((XiFunc *) formal_proto->xi_func)->semantic_plan;
    const XrSemanticFunctionRecord *formal_function = xr_semantic_plan_function(formal, 1);
    ASSERT_NOT_NULL(formal_function);
    const XrSemanticParameterRecord *parameter =
        xr_semantic_plan_parameter(formal, formal_function->parameter_begin);
    ASSERT_NOT_NULL(parameter);
    const XrSemanticTypeRecord *formal_type = xr_semantic_plan_type(formal, parameter->type);
    XrSemanticCallableKeyShape shape = {0};
    ASSERT_TRUE(formal_type && xr_semantic_callable_key_parse(formal_type->canonical_key, &shape));
    ASSERT_EQ_INT(shape.throw_effect, XR_FN_EFFECT_POLY);
    ASSERT_TRUE(xr_semantic_source_callable_pure_body_is_exact(actual, 1));
    const XrSemanticOperationRecord *closure = xr_semantic_plan_operation(actual, 0);
    ASSERT_TRUE(xr_semantic_constructor_callback_admits(actual, closure, formal, parameter->type));
    XrSemanticOperationRecord fake_closure = *closure;
    fake_closure.callable_function = 0;
    ASSERT_TRUE(!xr_semantic_constructor_callback_admits(actual, &fake_closure, formal, parameter->type));
    fake_closure = *closure;
    fake_closure.operand_count = 1;
    ASSERT_TRUE(!xr_semantic_constructor_callback_admits(actual, &fake_closure, formal, parameter->type));
    XrSemanticPlan counterfeit = *actual;
    XrSemanticOperationRecord *ops = (XrSemanticOperationRecord *) xr_malloc(
        actual->operation_count * sizeof(*ops));
    ASSERT_NOT_NULL(ops);
    memcpy(ops, actual->operations, actual->operation_count * sizeof(*ops));
    counterfeit.operations = ops;
    const XrSemanticFunctionRecord *fn = xr_semantic_plan_function(actual, 1);
    const XrSemanticBlockRecord *block = xr_semantic_plan_block(actual, fn->block_begin);
    ops[block->operation_begin].effects = XI_EFFECT_MAY_THROW;
    ASSERT_TRUE(!xr_semantic_source_callable_pure_body_is_exact(&counterfeit, 1));
    ASSERT_TRUE(!xr_semantic_constructor_callback_admits(
        &counterfeit, closure, formal, parameter->type));
    ops[block->operation_begin] = actual->operations[block->operation_begin];
    ops[block->operation_begin].opcode = XI_CALL;
    ASSERT_TRUE(!xr_semantic_source_callable_pure_body_is_exact(&counterfeit, 1));
    ops[block->operation_begin] = actual->operations[block->operation_begin];
    ASSERT_TRUE(xr_semantic_source_callable_pure_body_is_exact(&counterfeit, 1));
    xr_free(ops);
    xr_free_code(actual_isolate, actual_proto);
    xr_free_code(formal_isolate, formal_proto);
    xray_vm_delete(actual_isolate);
    xray_vm_delete(formal_isolate);
}


XR_FUNC void probe_callable52_graph_oom(const XrSemanticCoroutineModuleGraph *graph,
                                       uint32_t owner, uint32_t function, int expected);

TEST(source_callable_graph_retains_unknown_callback_suspension) {
    XrVMRuntime *isolate = NULL;
    XrProto *proto = compile_callable_body_probe(
        "fn leaf(x: i64) -> i64 { return x }\n"
        "fn invoke(cb: fn(i64) -> i64, x: i64) -> i64 { return cb(x) }\n", &isolate);
    ASSERT_NOT_NULL(proto);
    const XrSemanticPlan *plan = ((XiFunc *) proto->xi_func)->semantic_plan;
    const char *identity = xr_semantic_coroutine_module_identity(plan);
    ASSERT_NOT_NULL(identity);
    uint8_t used[1] = {0};
    XrSemanticCoroutineModule module = {plan, identity};
    XrSemanticCoroutineModuleGraph graph = {&module, 1, used};
    ASSERT_EQ_INT(xr_semantic_function_graph_suspendability(&graph, 0, 1), 0);
    ASSERT_TRUE(used[0] == 1);
    ASSERT_EQ_INT(xr_semantic_function_graph_suspendability(&graph, 0, 2), 1);
    probe_callable52_graph_oom(&graph, 0, 1, 0);
    probe_callable52_graph_oom(&graph, 0, 2, 1);
    const XrSemanticEntityRecord *module_entity = xr_semantic_plan_unique_module_entity(plan);
    XrSemanticDependencyRecord required = {
        .module = module_entity->id, .semantic_fingerprint = xr_semantic_plan_fingerprint(plan),
        .module_path = identity};
    ASSERT_EQ_INT(xr_semantic_coroutine_dependency_module(&graph, &required), 0);
    required.semantic_fingerprint.bytes[0] ^= 1;
    ASSERT_TRUE(xr_semantic_coroutine_dependency_module(&graph, &required) == XR_SEMANTIC_INDEX_NONE);
    required.semantic_fingerprint.bytes[0] ^= 1;
    required.module_path = "fake-source-module";
    ASSERT_TRUE(xr_semantic_coroutine_dependency_module(&graph, &required) == XR_SEMANTIC_INDEX_NONE);
    required.module_path = identity;
    module.identity = "fake-source-module";
    ASSERT_TRUE(xr_semantic_coroutine_dependency_module(&graph, &required) == XR_SEMANTIC_INDEX_NONE);
    module.identity = identity;
    XrSemanticCoroutineModule duplicated[2] = {module, module};
    graph.modules = duplicated;
    graph.count = 2;
    graph.used = NULL;
    ASSERT_TRUE(xr_semantic_coroutine_dependency_module(&graph, &required) == XR_SEMANTIC_INDEX_NONE);
    xr_free_code(isolate, proto);
    xray_vm_delete(isolate);
}

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("Callable 52 typed Source facts");
RUN_TEST(typed_source_lambda_has_owned_complete_callable_fact);
RUN_TEST(real_source_scalar_body_proves_ctor_callback_without_effect_bit_authority);
RUN_TEST(source_callable_graph_retains_unknown_callback_suspension);
TEST_MAIN_END()
