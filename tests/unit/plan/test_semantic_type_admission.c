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

    printf("Semantic type admission tests passed\n");
    return 0;
}
