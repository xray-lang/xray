/*
 * test_semantic_type_admission.c - Target-neutral parameter admission facts
 */

#include "../../../src/plan/semantic/xr_semantic_type_admission_shape.h"

#include <stdio.h>
#include <stdlib.h>

#define REQUIRE(condition)                                                                         \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "requirement failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

int main(void) {
    XrSemanticTypeRecord null_type = {.kind = XR_KIND_NULL};
    XrSemanticTypeRecord parameter = {
        .kind = XR_KIND_ARRAY,
        .flags = XR_SEM_TYPE_NULLABLE | XR_SEM_TYPE_REFERENCE_CAPABLE,
    };

    REQUIRE(xr_semantic_null_inhabits_parameter(&null_type, &parameter));

    parameter.flags = XR_SEM_TYPE_REFERENCE_CAPABLE;
    REQUIRE(!xr_semantic_null_inhabits_parameter(&null_type, &parameter));

    parameter.flags = XR_SEM_TYPE_NULLABLE | XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_VALUE;
    REQUIRE(!xr_semantic_null_inhabits_parameter(&null_type, &parameter));

    parameter.kind = XR_KIND_UNKNOWN;
    parameter.flags = XR_SEM_TYPE_NULLABLE | XR_SEM_TYPE_REFERENCE_CAPABLE;
    REQUIRE(!xr_semantic_null_inhabits_parameter(&null_type, &parameter));

    null_type.kind = XR_KIND_UNKNOWN;
    parameter.kind = XR_KIND_ARRAY;
    REQUIRE(!xr_semantic_null_inhabits_parameter(&null_type, &parameter));

    XrSemanticTypeRecord const_slice = {
        .id = {.bytes = {1}},
        .kind = XR_KIND_SLICE,
        .flags = XR_SEM_TYPE_CONST | XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_BORROW_VIEW,
        .canonical_key =
            "type-v3:26:0:0:0:1:0:0:0:0:255:0:;element:"
            "type-v3:0:0:0:0:0:0:0:0:0:2:0:",
    };
    XrSemanticTypeRecord slice_parameter = {
        .id = {.bytes = {2}},
        .kind = XR_KIND_SLICE,
        .flags = XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_BORROW_VIEW,
        .canonical_key =
            "type-v3:26:0:0:0:0:0:0:0:0:255:0:;element:"
            "type-v3:0:0:0:0:0:0:0:0:0:2:0:",
    };
    REQUIRE(xr_semantic_type_is_const_read_admission(&const_slice, &slice_parameter,
                                                     XR_PARAM_READ));
    REQUIRE(!xr_semantic_type_is_const_read_admission(&const_slice, &slice_parameter,
                                                      XR_PARAM_REF));
    REQUIRE(!xr_semantic_type_is_const_read_admission(&const_slice, &slice_parameter,
                                                      XR_PARAM_MOVE));

    XrSemanticTypeRecord different_element = const_slice;
    different_element.canonical_key =
        "type-v3:26:0:0:0:1:0:0:0:0:255:0:;element:"
        "type-v3:0:0:0:0:0:0:0:0:0:3:0:";
    REQUIRE(!xr_semantic_type_is_const_read_admission(&different_element, &slice_parameter,
                                                      XR_PARAM_READ));

    XrSemanticTypeRecord nullable_slice = const_slice;
    nullable_slice.flags |= XR_SEM_TYPE_NULLABLE;
    nullable_slice.canonical_key =
        "type-v3:26:0:0:1:1:0:0:0:0:255:0:;element:"
        "type-v3:0:0:0:0:0:0:0:0:0:2:0:";
    REQUIRE(!xr_semantic_type_is_const_read_admission(&nullable_slice, &slice_parameter,
                                                      XR_PARAM_READ));

    printf("Semantic type admission tests passed\n");
    return 0;
}
