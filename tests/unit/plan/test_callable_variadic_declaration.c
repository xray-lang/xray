/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_callable_variadic_declaration.c - Exact declared rest and packed body join
 */

#include "../test_framework.h"
#include "api/xisolate_profile.h"
#include "xray_vm.h"
#include "runtime/xisolate_api.h"
#include "toolchain/xcompiler_session.h"
#include "ir/xi.h"
#include "ir/xi_core_api.h"
#include "plan/semantic/xr_semantic_plan_internal.h"
#include "plan/semantic/xr_semantic_function_callable_shape.h"
#include "plan/semantic/xr_semantic_constructor_callable_shape.h"
#include "plan/ownership/xr_ownership_certificate_internal.h"
#include "plan/format/xr_xsm_schema.h"
#include "base/xmalloc.h"
#include "module/xmodule_identity.h"
#include "runtime/value/xchunk.h"

static XrVMRuntime *variadic_isolate(void) {
    return xr_isolate_profile_new(XR_ISOLATE_PROFILE_RUN);
}

static XrProto *variadic_source(XrVMRuntime *isolate) {
    const XrModuleIdentityAuthority authority = {
        .kind=XR_MODULE_IDENTITY_MEMORY, .namespace_id="exact-variadic-declaration"};
    const char *source =
        "fn first(...args: f64) -> f64 {\n"
        "  if (len(args) == 0) { return -1.0 }\n"
        "  return args[0]\n}\n"
        "fn prefixed(seed: f64, ...args: f64) -> f64 {\n"
        "  if (len(args) == 0) { return seed }\n"
        "  return seed + args[0]\n}\n"
        "assert(first() == -1.0)\n"
        "assert(first(8.0, 2.0) == 8.0)\n"
        "assert(prefixed(3.0) == 3.0)\n"
        "assert(prefixed(3.0, 4.0, 5.0) == 7.0)\n";
    XrCompilerSession *session = xr_compiler_session_current_for_isolate(isolate);
    XrCompilerSessionOperationScope scope;
    if (!xr_compiler_session_operation_begin(session, &scope)) return NULL;
    XrProto *proto = xr_compile_source_with_path(session, source, authority.namespace_id, &authority);
    if (!proto) {
        (void) xr_compiler_session_operation_fail(&scope, XR_COMPILER_SESSION_OPERATION_FATAL);
        return NULL;
    }
    if (!xr_compiler_session_operation_succeed(&scope)) {
        xr_free_code(isolate, proto);
        return NULL;
    }
    return proto;
}

static XrSemanticFunctionRecord *variadic_function(XrSemanticPlan *plan, const char *name) {
    for (uint32_t f=0; f<plan->function_count; ++f)
        if (strcmp(plan->functions[f].name,name)==0) return &plan->functions[f];
    return NULL;
}

XR_FUNC bool probe_unchecked_xsm_encode(const XrSemanticPlan *plan, uint8_t **bytes,
                                       size_t *size, char *error, size_t error_size);

static bool variadic_rehashed_rejection(XrSemanticPlan *plan, const char *label,
                                        const char *expected_error) {
    XrFingerprint fingerprint = plan->fingerprint;
    XrFingerprint premise = plan->ownership->semantic_fingerprint;
    XrFingerprint certificate = plan->ownership->fingerprint;
    xr_semantic_plan_compute_fingerprint(plan, &plan->fingerprint);
    plan->ownership->semantic_fingerprint = plan->fingerprint;
    plan->ownership->fingerprint = plan->fingerprint;
    uint8_t *wire = NULL;
    size_t size = 0;
    char error[512] = {0};
    XrSemanticPlan *decoded = NULL;
    bool encoded = probe_unchecked_xsm_encode(plan, &wire, &size, error, sizeof(error));
    bool accepted = encoded && xr_xsm_decode(wire, size, &decoded, error, sizeof(error));
    bool rejected = encoded && !accepted && !decoded && strstr(error, expected_error);
    printf("rehashed %s: %s\n", label, error);
    xr_semantic_plan_free(decoded);
    xr_free(wire);
    plan->fingerprint = fingerprint;
    plan->ownership->semantic_fingerprint = premise;
    plan->ownership->fingerprint = certificate;
    return rejected;
}

TEST(real_source_empty_multiple_and_prefixed_rest_arguments) {
    XrVMRuntime *isolate=variadic_isolate(); ASSERT_NOT_NULL(isolate);
    XrProto *proto=variadic_source(isolate); ASSERT_NOT_NULL(proto);
    XrSemanticPlan *plan=((XiFunc *)proto->xi_func)->semantic_plan;
    XrSemanticFunctionRecord *first=variadic_function(plan,"first");
    XrSemanticFunctionRecord *prefixed=variadic_function(plan,"prefixed");
    ASSERT_NOT_NULL(first); ASSERT_NOT_NULL(prefixed);
    ASSERT_EQ_INT(first->parameter_count,1); ASSERT_EQ_INT(prefixed->parameter_count,2);
    ASSERT_TRUE(xr_semantic_function_callable_shape_is_exact(plan,first));
    ASSERT_TRUE(xr_semantic_function_callable_shape_is_exact(plan,prefixed));
    ASSERT_TRUE(!xr_semantic_source_callable_pure_body_is_exact(plan,
        (uint32_t)(first-plan->functions)));
    ASSERT_EQ_INT(xr_execute(isolate,proto),0);
    xr_free_code(isolate,proto); xray_vm_delete(isolate);
}

TEST(frozen_rest_shape_rejects_wrong_pack_element_position_and_modes) {
    XrVMRuntime *isolate=variadic_isolate(); ASSERT_NOT_NULL(isolate);
    XrProto *proto=variadic_source(isolate); ASSERT_NOT_NULL(proto);
    XrSemanticPlan *plan=((XiFunc *)proto->xi_func)->semantic_plan;
    XrSemanticFunctionRecord *function=variadic_function(plan,"prefixed");
    ASSERT_NOT_NULL(function);
    XrSemanticParameterRecord *ordinary=&plan->parameters[function->parameter_begin];
    XrSemanticParameterRecord *rest=ordinary+1;
    XrSemanticTypeRecord *packed=&plan->types[rest->type];
    XrSemanticParameterRecord old_ordinary=*ordinary,old_rest=*rest;
    XrSemanticTypeRecord old_packed=*packed;
    ASSERT_TRUE(xr_semantic_function_callable_shape_is_exact(plan,function));
    rest->flags &= (uint8_t)~XR_SEM_PARAMETER_VARIADIC;
    ASSERT_TRUE(!xr_semantic_function_callable_shape_is_exact(plan,function)); *rest=old_rest;
    rest->flags |= XR_SEM_PARAMETER_REQUIRED;
    ASSERT_TRUE(!xr_semantic_function_callable_shape_is_exact(plan,function)); *rest=old_rest;
    ordinary->flags |= XR_SEM_PARAMETER_VARIADIC;
    ASSERT_TRUE(!xr_semantic_function_callable_shape_is_exact(plan,function)); *ordinary=old_ordinary;
    rest->mode=XR_PARAM_REF;
    ASSERT_TRUE(!xr_semantic_function_callable_shape_is_exact(plan,function)); *rest=old_rest;
    rest->type=ordinary->type;
    ASSERT_TRUE(!xr_semantic_function_callable_shape_is_exact(plan,function)); *rest=old_rest;
    ordinary->type=rest->type;
    ASSERT_TRUE(!xr_semantic_function_callable_shape_is_exact(plan,function)); *ordinary=old_ordinary;
    packed->kind=XR_KIND_SLICE;
    ASSERT_TRUE(!xr_semantic_function_callable_shape_is_exact(plan,function)); *packed=old_packed;
    packed->flags |= XR_SEM_TYPE_NULLABLE;
    ASSERT_TRUE(!xr_semantic_function_callable_shape_is_exact(plan,function)); *packed=old_packed;
    packed->child_count=0;
    ASSERT_TRUE(!xr_semantic_function_callable_shape_is_exact(plan,function)); *packed=old_packed;
    uint32_t child=plan->type_children[packed->child_begin];
    uint32_t wrong=XR_SEMANTIC_INDEX_NONE;
    for(uint32_t t=0;t<plan->type_count;++t)
        if(plan->types[t].kind==XR_KIND_INT){wrong=t;break;}
    ASSERT_TRUE(wrong<plan->type_count);
    plan->type_children[packed->child_begin]=wrong;
    ASSERT_TRUE(!xr_semantic_function_callable_shape_is_exact(plan,function));
    plan->type_children[packed->child_begin]=child;
    ASSERT_TRUE(xr_semantic_function_callable_shape_is_exact(plan,function));
    xr_free_code(isolate,proto); xray_vm_delete(isolate);
}

TEST(real_source_wire_roundtrip_and_rehashed_rest_rejections) {
    XrVMRuntime *isolate = variadic_isolate(); ASSERT_NOT_NULL(isolate);
    XrProto *proto = variadic_source(isolate); ASSERT_NOT_NULL(proto);
    XrSemanticPlan *plan = ((XiFunc *) proto->xi_func)->semantic_plan;
    XrSemanticFunctionRecord *function = variadic_function(plan, "prefixed");
    ASSERT_NOT_NULL(function);
    uint8_t *wire = NULL;
    size_t size = 0;
    char error[512] = {0};
    XrSemanticPlan *decoded = NULL;
    ASSERT_TRUE(xr_xsm_encode(plan, &wire, &size, error, sizeof(error)));
    ASSERT_TRUE(xr_xsm_decode(wire, size, &decoded, error, sizeof(error)));
    xr_semantic_plan_free(decoded); xr_free(wire);
    XrSemanticParameterRecord *rest = &plan->parameters[function->parameter_begin + 1u];
    XrSemanticParameterRecord saved = *rest;
    rest->flags &= (uint8_t) ~XR_SEM_PARAMETER_VARIADIC;
    ASSERT_TRUE(variadic_rehashed_rejection(plan, "missing rest flag", "XR_SEM_0013")); *rest = saved;
    rest->flags |= XR_SEM_PARAMETER_REQUIRED;
    ASSERT_TRUE(variadic_rehashed_rejection(plan, "required rest slot", "XR_SEM_0013")); *rest = saved;
    rest->mode = XR_PARAM_REF;
    ASSERT_TRUE(variadic_rehashed_rejection(plan, "wrong rest mode", "XR_SEM_0002")); *rest = saved;
    rest->type = plan->parameters[function->parameter_begin].type;
    ASSERT_TRUE(variadic_rehashed_rejection(plan, "unpacked rest slot", "XR_SEM_0002")); *rest = saved;
    wire = NULL; decoded = NULL;
    ASSERT_TRUE(xr_xsm_encode(plan, &wire, &size, error, sizeof(error)));
    ASSERT_TRUE(xr_xsm_decode(wire, size, &decoded, error, sizeof(error)));
    xr_semantic_plan_free(decoded); xr_free(wire);
    xr_free_code(isolate, proto); xray_vm_delete(isolate);
}

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("Source callable declared rest and packed frame");
RUN_TEST(real_source_empty_multiple_and_prefixed_rest_arguments);
RUN_TEST(frozen_rest_shape_rejects_wrong_pack_element_position_and_modes);
RUN_TEST(real_source_wire_roundtrip_and_rehashed_rest_rejections);
TEST_MAIN_END()
