/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * backing_array_inline_cases.h - Shared row layouts and independent owners
 */
typedef struct InlineArrayRow {
    XrtManagedHeader *left;
    int64_t number;
    XrtManagedHeader *right;
} InlineArrayRow;

static XrtValuePlan *inline_array_leaf(XrtBillingOwner *billing, XrtValueKind kind,
    size_t size, size_t alignment, bool owned) {
    size_t offset = 0;
    XrtValuePlanSpec shape = {.kind = kind, .size = size, .alignment = alignment,
        .owned_offsets = owned ? &offset : NULL, .owned_count = owned ? 1 : 0};
    XrtValuePlan *plan = NULL;
    OK(xrt_value_plan_new(billing, &shape, &plan));
    return plan;
}

static XrtValuePlan *inline_array_element(XrtBillingOwner *billing, XrtValuePlan *text, XrtValuePlan *number) {
    const size_t owners[] = {offsetof(InlineArrayRow, left), offsetof(InlineArrayRow, right)};
    const XrtValueSlotSpec fields[] = {
        {text, owners[0], NULL, 0}, {number, offsetof(InlineArrayRow, number), NULL, 0},
        {text, owners[1], NULL, 0}
    };
    XrtValuePlanSpec shape = {.kind = XRT_VALUE_TUPLE, .size = sizeof(InlineArrayRow),
        .alignment = _Alignof(InlineArrayRow), .owned_offsets = owners, .owned_count = 2,
        .fields = fields, .field_count = 3};
    XrtValuePlan *plan = NULL;
    OK(xrt_value_plan_new(billing, &shape, &plan));
    return plan;
}

static void inline_array_rows(void) {
    Fixture fixture;
    fixture_init(&fixture, SIZE_MAX);
    fixture.audit.allocation_fill = 0xa5;
    XrtValuePlan *text = inline_array_leaf(fixture.billing, XRT_VALUE_STRING,
        sizeof(XrtManagedHeader *), _Alignof(XrtManagedHeader *), true);
    XrtValuePlan *integer = inline_array_leaf(fixture.billing, XRT_VALUE_I64, sizeof(int64_t), _Alignof(int64_t), false);
    XrtValuePlan *decimal = inline_array_leaf(fixture.billing, XRT_VALUE_F64, sizeof(double), _Alignof(double), false);
    XrtValuePlan *element = inline_array_element(fixture.billing, text, integer);
    XrtValuePlan *unrelated = inline_array_element(fixture.billing, text, decimal);
    xrt_value_plan_drop(text);
    xrt_value_plan_drop(integer);
    xrt_value_plan_drop(decimal);

    XrtManagedOps *ops = NULL;
    XrtBillingStats before = xrt_billing_owner_stats(fixture.billing);
    for (size_t allowed = 0; allowed < 2; ++allowed) {
        xrt_billing_owner_fail_after(fixture.billing, allowed);
        CHECK(xrt_array_backing_inline_bind(fixture.billing, element, &ops) == XRT_MANAGED_OOM && !ops);
        same_bill(fixture.billing, before);
    }
    xrt_billing_owner_fail_after(fixture.billing, SIZE_MAX);
    OK(xrt_array_backing_inline_bind(fixture.billing, element, &ops));
    CHECK(ops);
    OK(xrt_string_backing_create(fixture.frame, 0, &fixture.types, "alpha", 5, 0));
    OK(xrt_string_backing_create(fixture.frame, 1, &fixture.types, "omega", 5, 0));
    XrtManagedHeader *first = borrow(&fixture, 0);
    XrtManagedHeader *second = borrow(&fixture, 1);
    const InlineArrayRow input[] = {{first, 10, first}, {first, 20, second}};
    OK(xrt_array_backing_inline_create(fixture.frame, 2, ops, input, 1, 2));
    OK(xrt_array_backing_inline_create(fixture.frame, 3, ops, NULL, 0, 0));
    XrtManagedHeader *old = borrow(&fixture, 2);
    const void *row = NULL;
    CHECK(xrt_array_backing_inline_at((const XrtArrayBacking *)borrow(&fixture, 3), element, 0, &row) ==
        XRT_MANAGED_BOUNDS && !row);
    before = xrt_billing_owner_stats(fixture.billing);
    BAD(xrt_array_backing_inline_create(fixture.frame, 2, fixture.types.arrays[XRT_BACKING_ARRAY_I64], input, 2, 3));
    BAD(xrt_array_backing_inline_create(fixture.frame, 2, ops, input, 2, 1));
    CHECK(xrt_array_backing_inline_create(fixture.frame, 2, ops, NULL, 0, SIZE_MAX) == XRT_MANAGED_OVERFLOW);
    xrt_billing_owner_fail_after(fixture.billing, 0);
    CHECK(xrt_array_backing_inline_create(fixture.frame, 2, ops, input, 2, 3) == XRT_MANAGED_OOM);
    xrt_billing_owner_fail_after(fixture.billing, SIZE_MAX);
    const int32_t first_refs = atomic_load(&first->references);
    const int32_t second_refs = atomic_exchange(&second->references, INT32_MAX);
    CHECK(xrt_array_backing_inline_create(fixture.frame, 2, ops, input, 2, 3) == XRT_MANAGED_REFCOUNT);
    CHECK(atomic_load(&first->references) == first_refs && atomic_load(&second->references) == INT32_MAX);
    atomic_store(&second->references, second_refs);
    CHECK(borrow(&fixture, 2) == old);
    same_bill(fixture.billing, before);

    OK(xrt_array_backing_inline_create(fixture.frame, 2, ops, input, 2, 3));
    XrtManagedHeader *owner = NULL;
    OK(xrt_managed_slot_take(fixture.frame, 2, &owner));
    const XrtArrayBacking *array = (const XrtArrayBacking *)owner;
    CHECK(array->length == 2 && array->capacity == 3);
    BAD(xrt_array_backing_inline_at(array, unrelated, 1, &row));
    CHECK(xrt_array_backing_inline_at(array, element, -1, &row) == XRT_MANAGED_BOUNDS);
    CHECK(xrt_array_backing_inline_at(array, element, 2, &row) == XRT_MANAGED_BOUNDS && !row);
    xrt_value_plan_drop(unrelated);
    xrt_managed_ops_drop(ops);
    xrt_value_plan_drop(element);
    fixture_retire(&fixture);
    CHECK(!atomic_load(&fixture.audit.finalized));

    /* The live Array now provides the only plan lease and the source Strings. */
    OK(xrt_array_backing_inline_at(array, element, 1, &row));
    CHECK(row == array->elements + sizeof(InlineArrayRow));
    InlineArrayRow copied = {0};
    CHECK(xrt_value_plan_retain(element));
    OK(xrt_value_copy_empty(element, row, &copied));
    xrt_managed_drop(owner);
    const XrtStringBacking *left = (const XrtStringBacking *)copied.left;
    const XrtStringBacking *right = (const XrtStringBacking *)copied.right;
    CHECK(copied.number == 20 && left->length == 5 && !memcmp(left->bytes, "alpha", 5));
    CHECK(right->length == 5 && !memcmp(right->bytes, "omega", 5));
    xrt_value_drop(element, &copied);
    xrt_value_plan_drop(element);
    CHECK(atomic_load(&fixture.audit.finalized) == 1);
}
