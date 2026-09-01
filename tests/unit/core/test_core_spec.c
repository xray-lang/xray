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
    CHECK(XR_CORE_SPEC_OPERATION_COUNT == 15u);
    CHECK(XR_CORE_SPEC_FEATURE_COUNT == 1u);
    CHECK(strlen(XR_CORE_SPEC_REGISTRY_SHA256) == 64u);

    for (index = 0; index < XR_CORE_SPEC_OPERATION_COUNT; ++index) {
        const XrCoreOperationSpec *operation = &xr_core_operation_specs[index];
        CHECK(operation->stable_id != 0u);
        CHECK(operation->spelling != NULL);
        CHECK(operation->operation_class != NULL);
        CHECK(operation->feature != NULL);
        CHECK(operation->spec_oracle_status == XR_CORE_COVERAGE_COMPLETE);
        CHECK(operation->decoder_status == XR_CORE_COVERAGE_NOT_YET_ACTIVE);
        CHECK(operation->verifier_status == XR_CORE_COVERAGE_NOT_YET_ACTIVE);
        CHECK(operation->evaluator_status == XR_CORE_COVERAGE_NOT_YET_ACTIVE);
        CHECK(operation->vm_status == XR_CORE_COVERAGE_NOT_YET_ACTIVE);
        CHECK(operation->aot_status == XR_CORE_COVERAGE_NOT_YET_ACTIVE);
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

static void test_walking_skeleton_metadata(void) {
    const XrCoreOperationSpec *constant =
        xr_core_spec_operation_by_id(XR_CORE_OP_CORE_CONSTANT_I64);
    const XrCoreOperationSpec *add = xr_core_spec_operation_by_spelling("core.add.i64");
    const XrCoreOperationSpec *branch = xr_core_spec_operation_by_spelling("core.branch");
    const XrCoreOperationSpec *call =
        xr_core_spec_operation_by_id(XR_CORE_OP_CORE_CALL_SEALED_DIRECT);
    const XrCoreOperationSpec *target =
        xr_core_spec_operation_by_id(XR_CORE_OP_CORE_TARGET_POINTER_WIDTH);

    CHECK(constant != NULL);
    CHECK(constant->operand_arity == 0u);
    CHECK(constant->result_type == XR_CORE_TYPE_I64);

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
    CHECK(target->result_type == XR_CORE_TYPE_U32);
    CHECK(target->capability_mask == UINT32_C(1));
    CHECK(strcmp(target->profile_dependency, "pointer_width") == 0);
}

int main(void) {
    test_registry_identity_and_lookup();
    test_walking_skeleton_metadata();

    if (failures != 0) {
        fprintf(stderr, "CoreSpec metadata tests failed: %d\n", failures);
        return 1;
    }
    printf("CoreSpec metadata tests passed (%u operations)\n",
           (unsigned) XR_CORE_SPEC_OPERATION_COUNT);
    return 0;
}
