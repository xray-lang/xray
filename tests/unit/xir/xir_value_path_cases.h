/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_value_path_cases.h - Nested ownership, publication and rollback checks
 */
#ifndef XIR_VALUE_PATH_CASES_H
#define XIR_VALUE_PATH_CASES_H
static XrXirTypeArena *value_path_arena(XrXirDomain *domain) {
    (void)domain;
    const XrXirNominalFieldIdentity inner_fields[] = {
        {{"n", 1}, XR_XIR_FIELD_MUTABLE}, {{"text", 4}, XR_XIR_FIELD_MUTABLE}};
    const XrXirNominalFieldIdentity outer_fields[] = {
        {{"items", 5}, XR_XIR_FIELD_MUTABLE}, {{"sibling", 7}, 0}};
    const XrXirNominalIdentity identities[] = {
        {{"test", 4}, {"Inner", 5}, 1, 0, inner_fields, 2, XR_XIR_NOMINAL_STRUCT, NULL, 0, 0,{0}},
        {{"test", 4}, {"Outer", 5}, 1, 0, outer_fields, 2, XR_XIR_NOMINAL_STRUCT, NULL, 0, 0,{0}}};
    const XrXirNominalTable table = {NULL, 2, identities};
    const XrXirType inner[] = {XR_XIR_I64, XR_XIR_STRING}, outer[] = {(XrXirType)257, XR_XIR_I64};
    const XrXirTypeNode nodes[] = {
        {.kind = XR_XIR_TYPE_NOMINAL, .nominal = {0, NULL, 0, inner, 2}},
        {.kind = XR_XIR_TYPE_ARRAY, .element = (XrXirType)256},
        {.kind = XR_XIR_TYPE_NOMINAL, .nominal = {1, NULL, 0, outer, 2}},
        {.kind = XR_XIR_TYPE_ARRAY, .element = (XrXirType)258}};
    const XrXirTypes types = {nodes, 4, &table, NULL};
    XrCompileResourceLimits limits = value_compile_limits(65536, 65536, 10000);
    XrXirTypeArena *arena = NULL;
    CHECK(value_compile_arena(&types, 100, limits, &arena) == XR_XIR_VALUE_OK);
    return arena;
}
static XrXirValue value_path_root(XrXirValueAdmission *admission, bool boxed) {
    XrXirValue fields[] = {{XR_XIR_I64, 0, 11}, {0}}, inner = {0}, array = {0}, outer = {0}, root = {0};
    admission->work = 100000;
    CHECK(xr_xir_string_new(admission->domain, "stable", 6, &fields[1]) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_struct_new((XrXirType)256, fields, 2, admission, &inner) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_array_new((XrXirType)257, &inner, 1, admission, &array) == XR_XIR_VALUE_OK);
    xr_xir_value_drop(&inner); xr_xir_value_drop(&fields[1]);
    fields[0] = array; fields[1] = (XrXirValue){XR_XIR_I64, 0, 37};
    CHECK(xr_xir_struct_new((XrXirType)258, fields, 2, admission, &outer) == XR_XIR_VALUE_OK);
    xr_xir_value_drop(&array);
    if (boxed) return outer;
    CHECK(xr_xir_array_new((XrXirType)259, &outer, 1, admission, &root) == XR_XIR_VALUE_OK);
    xr_xir_value_drop(&outer); return root;
}
static const XrXirValuePathStep value_path_steps[] = {
    {XR_XIR_PATH_INDEX, (XrXirType)259, 0}, {XR_XIR_PATH_FIELD, (XrXirType)258, 0},
    {XR_XIR_PATH_INDEX, (XrXirType)257, 0}, {XR_XIR_PATH_FIELD, (XrXirType)256, 0}};
static int64_t value_path_number(XrXirValue *root, const XrXirValuePath *path,
    XrXirValueAdmission *admission) {
    XrXirValuePlace place = {(XrXirType)root->type, &root->payload};
    XrXirValue result = {0}; XrXirFaultDetail fault = {0}; admission->work = 100000;
    CHECK(xr_xir_value_path_read(&place, path, admission, &result, &fault) == XR_XIR_VALUE_OK);
    CHECK(result.type == XR_XIR_I64 && !fault.code); return result.payload;
}
static void value_path_failures(XrXirValue *root, const XrXirValuePath *path,
    XrXirValueAdmission *admission) {
    XrXirValue copy = {0}, replacement = {XR_XIR_I64, 0, 99};
    XrXirValuePlace place = {(XrXirType)root->type, &copy.payload}; XrXirFaultDetail fault = {0};
    CHECK(xr_xir_value_copy(root, &copy) == XR_XIR_VALUE_OK);
    admission->work = 100000; size_t begin = calls;
    CHECK(xr_xir_value_path_write(&place, path, &replacement, admission, &fault) == XR_XIR_VALUE_OK);
    uint64_t work = 100000 - admission->work; size_t sites = calls - begin;
    CHECK(sites >= 4 && value_path_number(&copy, path, admission) == 99);
    CHECK(value_path_number(root, path, admission) == 11); xr_xir_value_drop(&copy);
    size_t blocks = live; uint64_t bytes = admission->domain->stats.live_bytes;
    for (size_t i = 0; i < sites; ++i) {
        CHECK(xr_xir_value_copy(root, &copy) == XR_XIR_VALUE_OK);
        admission->work = 100000; fail_at = calls + i;
        CHECK(xr_xir_value_path_write(&place, path, &replacement, admission, &fault) == XR_XIR_VALUE_OOM);
        fail_at = SIZE_MAX;
        CHECK(copy.payload == root->payload && admission->scratch_bytes == 65536);
        CHECK(live == blocks && admission->domain->stats.live_bytes == bytes);
        CHECK(value_path_number(root, path, admission) == 11); xr_xir_value_drop(&copy);
    }
    for (uint64_t i = 0; i < work; ++i) {
        CHECK(xr_xir_value_copy(root, &copy) == XR_XIR_VALUE_OK); admission->work = i;
        CHECK(xr_xir_value_path_write(&place, path, &replacement, admission, &fault) == XR_XIR_VALUE_LIMIT);
        CHECK(copy.payload == root->payload && admission->scratch_bytes == 65536);
        CHECK(live == blocks && admission->domain->stats.live_bytes == bytes);
        xr_xir_value_drop(&copy);
    }
    CHECK(xr_xir_value_copy(root, &copy) == XR_XIR_VALUE_OK);
    admission->work = 100000; admission->scratch_bytes = 0;
    CHECK(xr_xir_value_path_write(&place, path, &replacement, admission, &fault) == XR_XIR_VALUE_LIMIT);
    CHECK(copy.payload == root->payload && !admission->scratch_bytes);
    admission->scratch_bytes = 65536; xr_xir_value_drop(&copy);
    CHECK(live == blocks && admission->domain->stats.live_bytes == bytes);
    printf("Nested path rollback: %zu allocation sites, %llu work boundaries\n", sites, (unsigned long long)work);
}
static void value_path_saturation(XrXirValue *root, const XrXirValuePath *path,
    XrXirValueAdmission *admission) {
    XrXirValuePathStep steps[4]; memcpy(steps, path->steps, path->count * sizeof(*steps));
    steps[path->count - 1].selector = 1;
    XrXirValuePath string_path = {steps, path->count};
    XrXirValuePlace original = {(XrXirType)root->type, &root->payload};
    XrXirValue text = {0}, copy = {0}, replacement = {XR_XIR_I64, 0, 98};
    XrXirFaultDetail fault = {0}; admission->work = 100000;
    CHECK(xr_xir_value_path_read(&original, &string_path, admission, &text, &fault) == XR_XIR_VALUE_OK);
    XirObject *object = object_pointer(&text); uint32_t refs = atomic_load(&object->references);
    CHECK(xr_xir_value_copy(root, &copy) == XR_XIR_VALUE_OK);
    XrXirValuePlace place = {(XrXirType)root->type, &copy.payload};
    size_t blocks = live; uint64_t bytes = admission->domain->stats.live_bytes;
    atomic_store(&object->references, UINT32_MAX);
    CHECK(xr_xir_value_path_write(&place, path, &replacement, admission, &fault) == XR_XIR_VALUE_REFCOUNT_LIMIT);
    CHECK(copy.payload == root->payload && atomic_load(&object->references) == UINT32_MAX);
    CHECK(live == blocks && admission->domain->stats.live_bytes == bytes && admission->scratch_bytes == 65536);
    atomic_store(&object->references, refs); xr_xir_value_drop(&copy); xr_xir_value_drop(&text);
}
static void value_path_deep(void) {
    CHECK(!live); XrXirValue root = struct_deep_value(), copy = {0};
    XirObject *object = object_pointer(&root);
    XrXirValueAdmission admission = {object->arena, object->domain, NULL, NULL, 1000000, 65536};
    XrXirValuePathStep steps[160];
    for (uint32_t i = 0; i < 160; ++i)
        steps[i] = (XrXirValuePathStep){XR_XIR_PATH_FIELD, (XrXirType)(256 + i), 0};
    XrXirValuePath path = {steps, 160};
    XrXirValuePlace place = {(XrXirType)root.type, &root.payload};
    XrXirValue replacement = {XR_XIR_I64, 0, 92}; XrXirFaultDetail fault = {0};
    int64_t identity = root.payload;
    CHECK(xr_xir_value_path_write(&place, &path, &replacement, &admission, &fault) == XR_XIR_VALUE_OK);
    CHECK(root.payload == identity && value_path_number(&root, &path, &admission) == 92);
    CHECK(xr_xir_value_copy(&root, &copy) == XR_XIR_VALUE_OK);
    replacement.payload = 103; admission.work = 1000000;
    CHECK(xr_xir_value_path_write(&place, &path, &replacement, &admission, &fault) == XR_XIR_VALUE_OK);
    CHECK(1000000 - admission.work < 160 * 12);
    CHECK(root.payload != identity && value_path_number(&root, &path, &admission) == 103);
    CHECK(value_path_number(&copy, &path, &admission) == 92);
    xr_xir_value_drop(&copy); xr_xir_value_drop(&root); CHECK(!live);
}
static void value_path_append(XrXirValueAdmission *admission, bool boxed) {
    XrXirValue root = value_path_root(admission, boxed), copy = {0}, item = {0}, observed = {0};
    XrXirValuePath path = {value_path_steps + (boxed ? 1 : 0), boxed ? 1 : 2};
    XrXirValuePath element = {path.steps, path.count + 1};
    XrXirValuePlace place = {(XrXirType)root.type, &root.payload}, target = {place.type, &copy.payload};
    XrXirFaultDetail fault = {0};
    CHECK(xr_xir_value_path_read(&place, &element, admission, &item, &fault) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_copy(&root, &copy) == XR_XIR_VALUE_OK);
    admission->work = 100000; size_t begin = calls;
    CHECK(xr_xir_value_path_push(&target, &path, &item, admission, &fault) == XR_XIR_VALUE_OK);
    size_t sites = calls - begin; uint64_t work = 100000 - admission->work;
    CHECK(xr_xir_value_path_read(&target, &path, admission, &observed, &fault) == XR_XIR_VALUE_OK);
    CHECK(((XirArray *)object_pointer(&observed))->length == 2);
    xr_xir_value_drop(&observed); xr_xir_value_drop(&copy);
    size_t blocks = live; uint64_t bytes = admission->domain->stats.live_bytes;
    for (size_t i = 0; i < sites; ++i) {
        CHECK(xr_xir_value_copy(&root, &copy) == XR_XIR_VALUE_OK);
        admission->work = 100000; fail_at = calls + i;
        CHECK(xr_xir_value_path_push(&target, &path, &item, admission, &fault) == XR_XIR_VALUE_OOM);
        fail_at = SIZE_MAX;
        CHECK(copy.payload == root.payload && admission->scratch_bytes == 65536);
        CHECK(live == blocks && admission->domain->stats.live_bytes == bytes); xr_xir_value_drop(&copy);
    }
    for (uint64_t i = 0; i < work; ++i) {
        CHECK(xr_xir_value_copy(&root, &copy) == XR_XIR_VALUE_OK); admission->work = i;
        CHECK(xr_xir_value_path_push(&target, &path, &item, admission, &fault) == XR_XIR_VALUE_LIMIT);
        CHECK(copy.payload == root.payload && admission->scratch_bytes == 65536);
        CHECK(live == blocks && admission->domain->stats.live_bytes == bytes); xr_xir_value_drop(&copy);
    }
    admission->work = 100000;
    CHECK(xr_xir_value_path_read(&place, &path, admission, &observed, &fault) == XR_XIR_VALUE_OK);
    CHECK(((XirArray *)object_pointer(&observed))->length == 1);
    int64_t old_array = observed.payload, old_root = root.payload;
    xr_xir_value_drop(&observed);
    XrXirDomain *receiving = NULL, *original = admission->domain;
    CHECK(xr_xir_domain_new(65536, &receiving) == XR_XIR_VALUE_OK);
    admission->domain = receiving;
    for (unsigned i = 0; i < 3; ++i)
        CHECK(xr_xir_value_path_push(&place, &path, &item, admission, &fault) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_path_read(&place, &path, admission, &observed, &fault) == XR_XIR_VALUE_OK);
    XirArray *before_growth = (XirArray *)object_pointer(&observed);
    CHECK(before_growth->length == 4 && before_growth->stride == 16 && before_growth->capacity == 4);
    unsigned char saved[64]; memcpy(saved, before_growth->data, sizeof(saved));
    xr_xir_value_drop(&observed);
    uint64_t source_bytes = original->stats.live_bytes, receiving_bytes = receiving->stats.live_bytes;
    blocks = live;
    for (size_t i = 0;; ++i) {
        CHECK(i < 32); admission->work = 100000; fail_at = calls + i;
        XrXirValueStatus status = xr_xir_value_path_push(&place, &path, &item, admission, &fault);
        fail_at = SIZE_MAX;
        if (status == XR_XIR_VALUE_OK) break;
        CHECK(status == XR_XIR_VALUE_OOM && root.payload == old_root);
        CHECK(before_growth->length == 4 && before_growth->capacity == 4);
        CHECK(!memcmp(saved, before_growth->data, sizeof(saved)));
        CHECK(original->stats.live_bytes == source_bytes && receiving->stats.live_bytes == receiving_bytes);
        CHECK(live == blocks && admission->scratch_bytes == 65536);
    }
    CHECK(root.payload == old_root);
    CHECK(xr_xir_value_path_read(&place, &path, admission, &observed, &fault) == XR_XIR_VALUE_OK);
    XirArray *array = (XirArray *)object_pointer(&observed);
    CHECK(array->length == 5 && observed.payload != old_array && array->object.domain == receiving);
    xr_xir_value_drop(&observed); xr_xir_value_drop(&item); xr_xir_value_drop(&root);
    CHECK(receiving->stats.live_bytes == sizeof(*receiving));
    xr_xir_domain_drop(receiving); admission->domain = original;
    printf("Nested append rollback: %zu allocation sites, %llu work boundaries\n", sites, (unsigned long long)work);
}
static void value_path_empty(void) {
    CHECK(!live); XrXirDomain *domain = NULL;
    CHECK(xr_xir_domain_new(65536, &domain) == XR_XIR_VALUE_OK);
    XrXirTypeArena *arena = storage_cursor_arena(domain);
    XrXirValueAdmission admission = {arena, domain, NULL, NULL, 100000, 65536};
    XrXirValue empty = {0}, root = {0}, copy = {0}, output = {0};
    CHECK(xr_xir_struct_new((XrXirType)258, NULL, 0, &admission, &empty) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_array_new((XrXirType)261, &empty, 1, &admission, &root) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_copy(&root, &copy) == XR_XIR_VALUE_OK);
    XrXirValuePlace place = {(XrXirType)261, &copy.payload};
    XrXirValuePathStep step = {XR_XIR_PATH_INDEX, (XrXirType)261, 0};
    XrXirValuePath path = {&step, 1}, root_path = {NULL, 0}; XrXirFaultDetail fault = {0};
    CHECK(xr_xir_value_path_write(&place, &path, &empty, &admission, &fault) == XR_XIR_VALUE_OK);
    CHECK(copy.payload != root.payload);
    CHECK(xr_xir_value_path_read(&place, &path, &admission, &output, &fault) == XR_XIR_VALUE_OK);
    CHECK(output.type == 258 && xr_xir_value_valid(&output)); xr_xir_value_drop(&output);
    for (unsigned i = 0; i < 5; ++i)
        CHECK(xr_xir_value_path_push(&place, &root_path, &empty, &admission, &fault) == XR_XIR_VALUE_OK);
    XirArray *array = (XirArray *)object_pointer(&copy);
    CHECK(array->length == 6 && !array->stride && !array->data);
    CHECK(((XirArray *)object_pointer(&root))->length == 1);
    xr_xir_value_drop(&empty); xr_xir_value_drop(&copy); xr_xir_value_drop(&root);
    xr_xir_compile_type_arena_drop(arena); xr_xir_domain_drop(domain); CHECK(!live);
}
static void value_path_cases(void) {
    CHECK(!live); XrXirDomain *domain = NULL;
    CHECK(xr_xir_domain_new(65536, &domain) == XR_XIR_VALUE_OK);
    XrXirTypeArena *arena = value_path_arena(domain);
    XrXirValueAdmission admission = {arena, domain, NULL, NULL, 100000, 65536};
    for (unsigned boxed = 0; boxed < 2; ++boxed) {
        XrXirValue root = value_path_root(&admission, boxed != 0), inner = {0}, output = {0};
        XrXirValuePath path = {value_path_steps + boxed, 4 - boxed};
        XrXirValuePlace place = {(XrXirType)root.type, &root.payload}; XrXirFaultDetail fault = {0};
        value_path_failures(&root, &path, &admission);
        value_path_saturation(&root, &path, &admission);
        XrXirValuePath prefix = {path.steps, path.count - 2};
        admission.work = 100000;
        CHECK(xr_xir_value_path_read(&place, &prefix, &admission, &inner, &fault) == XR_XIR_VALUE_OK);
        int64_t old_root = root.payload, old_inner = inner.payload;
        XrXirValue replacement = {XR_XIR_I64, 0, 73};
        CHECK(xr_xir_value_path_write(&place, &path, &replacement, &admission, &fault) == XR_XIR_VALUE_OK);
        CHECK(root.payload == old_root && value_path_number(&root, &path, &admission) == 73);
        XrXirValuePath suffix = {value_path_steps + 2, 2};
        CHECK(value_path_number(&inner, &suffix, &admission) == 11);
        xr_xir_value_drop(&inner);
        CHECK(xr_xir_value_path_read(&place, &prefix, &admission, &inner, &fault) == XR_XIR_VALUE_OK);
        CHECK(inner.payload != old_inner); old_inner = inner.payload; xr_xir_value_drop(&inner);
        replacement.payload = 84; admission.work = 100000;
        CHECK(xr_xir_value_path_write(&place, &path, &replacement, &admission, &fault) == XR_XIR_VALUE_OK);
        CHECK(root.payload == old_root && value_path_number(&root, &path, &admission) == 84);
        CHECK(xr_xir_value_path_read(&place, &prefix, &admission, &inner, &fault) == XR_XIR_VALUE_OK);
        CHECK(inner.payload == old_inner); xr_xir_value_drop(&inner);
        XrXirValuePathStep wrong_type[4]; memcpy(wrong_type, path.steps, path.count * sizeof(*wrong_type));
        wrong_type[path.count - 1].container = XR_XIR_I64;
        XrXirValuePath wrong = {wrong_type, path.count};
        CHECK(xr_xir_value_path_write(&place, &wrong, &replacement, &admission, &fault) == XR_XIR_VALUE_BAD_ARGUMENT);
        CHECK(value_path_number(&root, &path, &admission) == 84);
        XrXirValuePathStep invalid[4]; memcpy(invalid, path.steps, path.count * sizeof(*invalid));
        XrXirValuePath bad = {invalid, path.count}; invalid[path.count - 2].selector = -1;
        CHECK(xr_xir_value_path_write(&place, &bad, &replacement, &admission, &fault) == XR_XIR_VALUE_BOUNDS);
        CHECK(fault.code == 430 && fault.index == -1 && fault.length == 1);
        bad.count = 2 - boxed; invalid[1 - boxed].selector = 1;
        CHECK(xr_xir_value_path_write(&place, &bad, &replacement, &admission, &fault) == XR_XIR_VALUE_BAD_ARGUMENT);
        CHECK(!fault.code && root.payload == old_root);
        CHECK(xr_xir_value_path_read(&place, &bad, &admission, &output, &fault) == XR_XIR_VALUE_OK);
        CHECK(output.type == XR_XIR_I64 && output.payload == 37);
        xr_xir_value_drop(&root);
        value_path_append(&admission, boxed != 0);
    }
    xr_xir_compile_type_arena_drop(arena); CHECK(domain->stats.live_bytes == sizeof(*domain));
    xr_xir_domain_drop(domain); CHECK(!live);
    value_path_deep();
    value_path_empty();
}
#endif // XIR_VALUE_PATH_CASES_H
