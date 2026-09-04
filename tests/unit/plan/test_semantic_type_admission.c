/*
 * test_semantic_type_admission.c - Target-neutral parameter admission facts
 */

#include "../../../src/plan/semantic/xr_semantic_type_admission_shape.h"

#include <stdio.h>
#include <stdlib.h>

#define REQUIRE(condition)                                                                         \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "requirement failed at %s:%d: %s\n", __FILE__, __LINE__, #condition);  \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

typedef struct TestSemanticPlan {
    const XrSemanticTypeRecord *types;
    uint32_t type_count;
    const uint32_t *children;
    uint32_t child_count;
} TestSemanticPlan;

bool xr_stable_id_equal(XrStableId left, XrStableId right) {
    return memcmp(left.bytes, right.bytes, sizeof(left.bytes)) == 0;
}

size_t xr_semantic_plan_type_count(const XrSemanticPlan *plan) {
    return ((const TestSemanticPlan *) plan)->type_count;
}

const XrSemanticTypeRecord *xr_semantic_plan_type(const XrSemanticPlan *plan, uint32_t index) {
    const TestSemanticPlan *fixture = (const TestSemanticPlan *) plan;
    return index < fixture->type_count ? &fixture->types[index] : NULL;
}

const uint32_t *xr_semantic_plan_type_children(const XrSemanticPlan *plan, uint32_t *count) {
    const TestSemanticPlan *fixture = (const TestSemanticPlan *) plan;
    if (count)
        *count = fixture->child_count;
    return fixture->children;
}

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
        .canonical_key = "type-v3:26:0:0:0:1:0:0:0:0:255:0:;element:"
                         "type-v3:0:0:0:0:0:0:0:0:0:2:0:",
    };
    XrSemanticTypeRecord slice_parameter = {
        .id = {.bytes = {2}},
        .kind = XR_KIND_SLICE,
        .flags = XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_BORROW_VIEW,
        .canonical_key = "type-v3:26:0:0:0:0:0:0:0:0:255:0:;element:"
                         "type-v3:0:0:0:0:0:0:0:0:0:2:0:",
    };
    REQUIRE(
        xr_semantic_type_is_const_read_admission(&const_slice, &slice_parameter, XR_PARAM_READ));
    REQUIRE(
        !xr_semantic_type_is_const_read_admission(&const_slice, &slice_parameter, XR_PARAM_REF));
    REQUIRE(
        !xr_semantic_type_is_const_read_admission(&const_slice, &slice_parameter, XR_PARAM_MOVE));

    XrSemanticTypeRecord different_element = const_slice;
    different_element.canonical_key = "type-v3:26:0:0:0:1:0:0:0:0:255:0:;element:"
                                      "type-v3:0:0:0:0:0:0:0:0:0:3:0:";
    REQUIRE(!xr_semantic_type_is_const_read_admission(&different_element, &slice_parameter,
                                                      XR_PARAM_READ));

    XrSemanticTypeRecord nullable_slice = const_slice;
    nullable_slice.flags |= XR_SEM_TYPE_NULLABLE;
    nullable_slice.canonical_key = "type-v3:26:0:0:1:1:0:0:0:0:255:0:;element:"
                                   "type-v3:0:0:0:0:0:0:0:0:0:2:0:";
    REQUIRE(!xr_semantic_type_is_const_read_admission(&nullable_slice, &slice_parameter,
                                                      XR_PARAM_READ));

    XrSemanticTypeRecord union_members[] = {
        {
            .id = {.bytes = {3}},
            .kind = XR_KIND_INSTANCE,
            .flags = XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT,
            .canonical_key = "type-v3:11:0:0:0:0:0:0:0:0:255:0:;named:7:NetConn[0]",
        },
        {
            .id = {.bytes = {4}},
            .kind = XR_KIND_INSTANCE,
            .flags = XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT,
            .canonical_key = "type-v3:11:0:0:0:0:0:0:0:0:255:0:;named:11:NetListener[0]",
        },
    };
    const uint32_t union_children[] = {0, 1};
    const TestSemanticPlan callee = {
        .types = union_members,
        .type_count = 2,
        .children = union_children,
        .child_count = 2,
    };
    XrSemanticTypeRecord handle_union = {
        .id = {.bytes = {5}},
        .kind = XR_KIND_UNION,
        .child_begin = 0,
        .child_count = 2,
    };
    XrSemanticTypeRecord const_listener = union_members[1];
    const_listener.id.bytes[0] = 6;
    const_listener.flags |= XR_SEM_TYPE_CONST;
    const_listener.canonical_key = "type-v3:11:0:0:0:1:0:0:0:0:255:0:;named:11:NetListener[0]";

    REQUIRE(xr_semantic_parameter_type_admits_argument(
        (const XrSemanticPlan *) &callee, &handle_union, &const_listener, XR_PARAM_READ));
    REQUIRE(!xr_semantic_parameter_type_admits_argument(
        (const XrSemanticPlan *) &callee, &handle_union, &const_listener, XR_PARAM_REF));
    REQUIRE(!xr_semantic_parameter_type_admits_argument(
        (const XrSemanticPlan *) &callee, &handle_union, &different_element, XR_PARAM_READ));

    printf("Semantic type admission tests passed\n");
    return 0;
}
