#include "core/xr_core_spec_gen.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                   \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

static void test_registry_identity_and_lookup(void) {
    size_t index;

    CHECK(XR_CORE_SPEC_EPOCH == 1u);
    CHECK(XR_CORE_SPEC_OPERATION_COUNT == 47u);
    CHECK(XR_CORE_SPEC_FEATURE_COUNT == 1u);
    CHECK(strlen(XR_CORE_SPEC_SEMANTIC_SHA256) == 64u);

    for (index = 0; index < XR_CORE_SPEC_OPERATION_COUNT; ++index) {
        const XrCoreOperationSpec *operation = &xr_core_operation_specs[index];
        CHECK(operation->stable_id != 0u);
        CHECK(operation->spelling != NULL);
        CHECK(operation->operation_class != NULL);
        CHECK(operation->feature != NULL);
        CHECK(operation->spec_oracle_status == XR_CORE_COVERAGE_COMPLETE);
        CHECK(operation->decoder_status == XR_CORE_COVERAGE_COMPLETE);
        CHECK(operation->verifier_status == XR_CORE_COVERAGE_COMPLETE);
        CHECK(operation->evaluator_status == XR_CORE_COVERAGE_COMPLETE);
        CHECK(operation->vm_status == XR_CORE_COVERAGE_COMPLETE);
        CHECK(operation->aot_status == XR_CORE_COVERAGE_COMPLETE);
        CHECK(xr_core_spec_operation_by_id(operation->stable_id) == operation);
        CHECK(xr_core_spec_operation_by_spelling(operation->spelling) == operation);
        if (index > 0u)
            CHECK(xr_core_operation_specs[index - 1u].stable_id < operation->stable_id);
    }

    CHECK(xr_core_spec_operation_by_id(0u) == NULL);
    CHECK(xr_core_spec_operation_by_id(15u) == NULL);
    CHECK(xr_core_spec_operation_by_id(UINT16_MAX) == NULL);
    CHECK(xr_core_spec_operation_by_spelling(NULL) == NULL);
    CHECK(xr_core_spec_operation_by_spelling("core.unknown") == NULL);
    CHECK(xr_core_spec_feature_active(XR_CORE_FEATURE_CORE_BASE));
    CHECK(!xr_core_spec_feature_active(0u));
    CHECK(!xr_core_spec_feature_active(UINT16_MAX));
}

static void check_target_query(uint16_t operation_id, uint8_t result_type,
                               uint32_t capability_mask, const char *spelling,
                               const char *profile_dependency) {
    const XrCoreOperationSpec *operation = xr_core_spec_operation_by_id(operation_id);

    CHECK(operation != NULL);
    if (!operation)
        return;
    CHECK(strcmp(operation->spelling, spelling) == 0);
    CHECK(strcmp(operation->operation_class, "target-query") == 0);
    CHECK(operation->operand_arity == 0u);
    CHECK(operation->result_type == result_type);
    CHECK(operation->effect_mask ==
          (XR_CORE_EFFECT_TARGET_QUERY | XR_CORE_EFFECT_TRAP));
    CHECK(operation->capability_mask == capability_mask);
    CHECK(strcmp(operation->profile_dependency, profile_dependency) == 0);
    CHECK(strcmp(operation->materialization, "profile-query") == 0);
}

static void test_operation_metadata(void) {
    const XrCoreOperationSpec *constant =
        xr_core_spec_operation_by_id(XR_CORE_OP_CORE_CONSTANT_I64);
    const XrCoreOperationSpec *target_enum_constant =
        xr_core_spec_operation_by_id(XR_CORE_OP_CORE_CONSTANT_TARGET_ENUM);
    const XrCoreOperationSpec *add = xr_core_spec_operation_by_spelling("core.add.i64");
    const XrCoreOperationSpec *target_enum_compare =
        xr_core_spec_operation_by_id(XR_CORE_OP_CORE_COMPARE_TARGET_ENUM);
    const XrCoreOperationSpec *branch = xr_core_spec_operation_by_spelling("core.branch");
    const XrCoreOperationSpec *call =
        xr_core_spec_operation_by_id(XR_CORE_OP_CORE_CALL_SEALED_DIRECT);
    const XrCoreOperationSpec *target =
        xr_core_spec_operation_by_id(XR_CORE_OP_CORE_TARGET_POINTER_WIDTH);
    const XrCoreOperationSpec *aggregate =
        xr_core_spec_operation_by_id(XR_CORE_OP_CORE_AGGREGATE_CONSTRUCT);
    const XrCoreOperationSpec *variant =
        xr_core_spec_operation_by_id(XR_CORE_OP_CORE_VARIANT_PROJECT);
    const XrCoreOperationSpec *provider =
        xr_core_spec_operation_by_id(XR_CORE_OP_CORE_PROVIDER_CALL);
    const XrCoreOperationSpec *yield =
        xr_core_spec_operation_by_id(XR_CORE_OP_CORE_COROUTINE_YIELD);
    const XrCoreOperationSpec *coroutine_call =
        xr_core_spec_operation_by_id(XR_CORE_OP_CORE_COROUTINE_CALL_SEALED);

    CHECK(constant != NULL);
    CHECK(constant->operand_arity == 0u);
    CHECK(constant->result_type == XR_CORE_TYPE_I64);

    CHECK(target_enum_constant != NULL);
    CHECK(target_enum_constant->operand_arity == 0u);
    CHECK(target_enum_constant->result_type == XR_CORE_TYPE_TYPE_VARIABLE);
    CHECK(target_enum_constant->effect_mask == UINT32_C(0));
    CHECK(target_enum_constant->capability_mask == UINT32_C(0));
    CHECK(strcmp(target_enum_constant->spelling, "core.constant.target_enum") == 0);

    CHECK(target_enum_compare != NULL);
    CHECK(target_enum_compare->operand_arity == 2u);
    CHECK(target_enum_compare->result_type == XR_CORE_TYPE_BOOL);
    CHECK(target_enum_compare->effect_mask == UINT32_C(0));
    CHECK(target_enum_compare->capability_mask == UINT32_C(0));
    CHECK(strcmp(target_enum_compare->spelling, "core.compare.target_enum") == 0);

    CHECK(add != NULL);
    CHECK(add->operand_arity == 2u);
    CHECK(add->result_type == XR_CORE_TYPE_I64);
    CHECK(add->effect_mask == UINT32_C(1));

    CHECK(branch != NULL);
    CHECK(branch->operand_arity == XR_CORE_SPEC_VARIADIC_ARITY);
    CHECK(branch->result_type == XR_CORE_TYPE_VOID);

    CHECK(call != NULL);
    CHECK(call->operand_arity == XR_CORE_SPEC_VARIADIC_ARITY);
    CHECK(call->result_type == XR_CORE_TYPE_TYPE_VARIABLE);
    CHECK(call->effect_mask == UINT32_C(4));

    CHECK(target != NULL);
    CHECK(target->result_type == XR_CORE_TYPE_U16);
    CHECK(target->capability_mask == UINT32_C(1));
    CHECK(strcmp(target->profile_dependency, "pointer_width") == 0);

    check_target_query(XR_CORE_OP_CORE_TARGET_POINTER_WIDTH, XR_CORE_TYPE_U16,
                       XR_CORE_CAPABILITY_PROFILE_POINTER_WIDTH,
                       "core.target.pointer_width", "pointer_width");
    check_target_query(XR_CORE_OP_CORE_TARGET_OPERATING_SYSTEM, XR_CORE_TYPE_TARGET_OS,
                       XR_CORE_CAPABILITY_PROFILE_OPERATING_SYSTEM,
                       "core.target.operating_system", "operating_system");
    check_target_query(XR_CORE_OP_CORE_TARGET_ARCHITECTURE, XR_CORE_TYPE_TARGET_ARCH,
                       XR_CORE_CAPABILITY_PROFILE_ARCHITECTURE,
                       "core.target.architecture", "architecture");
    check_target_query(XR_CORE_OP_CORE_TARGET_NATIVE_ABI, XR_CORE_TYPE_TARGET_ABI,
                       XR_CORE_CAPABILITY_PROFILE_NATIVE_ABI,
                       "core.target.native_abi", "native_abi");
    check_target_query(XR_CORE_OP_CORE_TARGET_ENDIANNESS, XR_CORE_TYPE_TARGET_ENDIAN,
                       XR_CORE_CAPABILITY_PROFILE_ENDIANNESS,
                       "core.target.endianness", "endianness");

    CHECK(aggregate != NULL);
    CHECK(aggregate->operand_arity == XR_CORE_SPEC_VARIADIC_ARITY);
    CHECK(aggregate->result_type == XR_CORE_TYPE_TYPE_VARIABLE);

    CHECK(variant != NULL);
    CHECK(variant->operand_arity == 1u);
    CHECK(variant->result_type == XR_CORE_TYPE_TYPE_VARIABLE);
    CHECK(variant->effect_mask == UINT32_C(1));

    CHECK(provider != NULL);
    CHECK(provider->operand_arity == 1u);
    CHECK(provider->result_type == XR_CORE_TYPE_I64);
    CHECK(provider->effect_mask ==
          (XR_CORE_EFFECT_TRAP | XR_CORE_EFFECT_CALL | XR_CORE_EFFECT_PROVIDER_CALL));
    CHECK(provider->capability_mask == XR_CORE_CAPABILITY_PROVIDER_BINDING);
    CHECK(yield != NULL);
    CHECK(strcmp(yield->spelling, "core.coroutine.yield") == 0);
    CHECK(strcmp(yield->operation_class, "coroutine-terminator") == 0);
    CHECK(yield->operand_arity == XR_CORE_SPEC_VARIADIC_ARITY);
    CHECK(yield->result_type == XR_CORE_TYPE_VOID);
    CHECK(yield->successor_mask == UINT8_C(16));
    CHECK(yield->effect_mask == XR_CORE_EFFECT_SUSPEND);
    CHECK(yield->capability_mask == XR_CORE_CAPABILITY_RUNTIME_COOPERATIVE_YIELD);
    CHECK(strcmp(yield->profile_dependency, "scheduler-yield") == 0);
    CHECK(strcmp(yield->materialization, "logical-coroutine-control") == 0);
    CHECK(coroutine_call != NULL);
    CHECK(strcmp(coroutine_call->spelling, "core.coroutine.call.sealed") == 0);
    CHECK(strcmp(coroutine_call->operation_class, "coroutine-call-terminator") == 0);
    CHECK(coroutine_call->operand_arity == XR_CORE_SPEC_VARIADIC_ARITY);
    CHECK(coroutine_call->result_type == XR_CORE_TYPE_VOID);
    CHECK(coroutine_call->successor_mask == UINT8_C(17));
    CHECK(coroutine_call->effect_mask ==
          (XR_CORE_EFFECT_CALL | XR_CORE_EFFECT_SUSPEND));
    CHECK(coroutine_call->capability_mask ==
          XR_CORE_CAPABILITY_RUNTIME_COOPERATIVE_YIELD);
    CHECK(strcmp(coroutine_call->profile_dependency, "scheduler-yield") == 0);
    CHECK(strcmp(coroutine_call->materialization,
                 "logical-child-coroutine-control") == 0);
}

int main(void) {
    test_registry_identity_and_lookup();
    test_operation_metadata();

    if (failures != 0) {
        fprintf(stderr, "CoreSpec metadata tests failed: %d\n", failures);
        return 1;
    }
    printf("CoreSpec metadata tests passed (%u operations)\n",
           (unsigned) XR_CORE_SPEC_OPERATION_COUNT);
    return 0;
}
