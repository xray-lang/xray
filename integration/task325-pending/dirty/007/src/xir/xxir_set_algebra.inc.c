/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_set_algebra.inc.c - Ordered private results from two admitted sets
 */
typedef enum SetAlgebra { SET_UNION, SET_INTERSECTION, SET_DIFFERENCE, SET_SYMMETRIC_DIFFERENCE } SetAlgebra;
static XrXirValueStatus set_pair(const XrXirValue *left, const XrXirValue *right,
    XrXirValueAdmission *admission, XirHash **a, XirHash **b) {
    if (!left || !right || left->type != right->type) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValueStatus status = hash_admit(left, XR_XIR_TYPE_SET, admission, a);
    if (status == XR_XIR_VALUE_OK) status = hash_admit(right, XR_XIR_TYPE_SET, admission, b);
    return status;
}
typedef struct SetSelection { const XirHash *source, *other; bool filtered, keep_present; } SetSelection;
static XrXirValueStatus set_select(const SetSelection *selection, XirHash *result,
    StoragePack *pack, XrXirValueAdmission *admission) {
    const XirHash *source = selection->source;
    XrXirLayout layout = {0};
    if (!xr_xir_compile_type_arena_layout(source->object.arena, source->key_type, &layout))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    for (size_t i = 0; i < source->length; ++i) {
        XrXirValueStatus status = hash_charge(admission, 1);
        if (status != XR_XIR_VALUE_OK) return status;
        if (selection->filtered) {
            XrXirValue key = storage_leaf_value(source->key_type, hash_span(source, i, false).bytes, layout.size);
            HashProbe probe = {0}; status = hash_probe(selection->other, &key, admission, &probe);
            if (status != XR_XIR_VALUE_OK) return status;
            if (probe.found != selection->keep_present) continue;
        }
        size_t bucket = 0, index = result->length;
        uint64_t hash = hash_entry_hash(source, i);
        status = hash_empty_bucket(result, hash, admission, &bucket);
        if (status == XR_XIR_VALUE_OK)
            status = storage_pack_copy(pack, hash_span(source, i, false).bytes,
                (unsigned char *)hash_span(result, index, false).bytes);
        if (status != XR_XIR_VALUE_OK) return status;
        memcpy(hash_entry(result, index), &hash, sizeof(hash));
        hash_buckets(result)[bucket] = index + 1; ++result->length;
    }
    return XR_XIR_VALUE_OK;
}
static XrXirValueStatus set_algebra(const XrXirValue *left, const XrXirValue *right,
    SetAlgebra operation, XrXirValueAdmission *admission, XrXirValue *output) {
    if (!unit_value(output) || !hash_admission_current(admission)) return XR_XIR_VALUE_BAD_ARGUMENT;
    xr_xir_value_graph_begin();
    XirHash *a = NULL, *b = NULL, *result = NULL;
    XrXirValueStatus status = set_pair(left, right, admission, &a, &b);
    size_t count = 0, capacity = 0;
    if (status == XR_XIR_VALUE_OK) {
        count = a->length;
        if (operation == SET_INTERSECTION && b->length < count) count = b->length;
        if (operation == SET_UNION || operation == SET_SYMMETRIC_DIFFERENCE) {
            if (b->length > SIZE_MAX - count) status = XR_XIR_VALUE_LIMIT;
            else count += b->length;
        }
        if (status == XR_XIR_VALUE_OK && !hash_capacity(count, a->stride, &capacity)) status = XR_XIR_VALUE_LIMIT;
    }
    if (status == XR_XIR_VALUE_OK) status = hash_allocate((XrXirType)left->type, capacity, admission, &result);
    XrXirValue owned = result ? hash_value(result) : (XrXirValue){0}; StoragePack pack = {0};
    if (status == XR_XIR_VALUE_OK) status = storage_pack_begin(admission, a->key_type, &pack);
    if (status == XR_XIR_VALUE_OK) {
        SetSelection first = {a, b, operation != SET_UNION, operation == SET_INTERSECTION};
        status = set_select(&first, result, &pack, admission);
        if (status == XR_XIR_VALUE_OK && (operation == SET_UNION || operation == SET_SYMMETRIC_DIFFERENCE)) {
            SetSelection second = {b, a, true, false};
            status = set_select(&second, result, &pack, admission);
        }
    }
    storage_pack_end(&pack);
    if (status == XR_XIR_VALUE_OK) { *output = owned; owned = (XrXirValue){0}; }
    xr_xir_value_drop(&owned); xr_xir_value_graph_end(); return status;
}
XR_FUNC XrXirValueStatus xr_xir_set_union(const XrXirValue *left, const XrXirValue *right,
    XrXirValueAdmission *admission, XrXirValue *output) {
    return set_algebra(left, right, SET_UNION, admission, output);
}
XR_FUNC XrXirValueStatus xr_xir_set_intersection(const XrXirValue *left, const XrXirValue *right,
    XrXirValueAdmission *admission, XrXirValue *output) {
    return set_algebra(left, right, SET_INTERSECTION, admission, output);
}
XR_FUNC XrXirValueStatus xr_xir_set_difference(const XrXirValue *left, const XrXirValue *right,
    XrXirValueAdmission *admission, XrXirValue *output) {
    return set_algebra(left, right, SET_DIFFERENCE, admission, output);
}
XR_FUNC XrXirValueStatus xr_xir_set_symmetric_difference(const XrXirValue *left, const XrXirValue *right,
    XrXirValueAdmission *admission, XrXirValue *output) {
    return set_algebra(left, right, SET_SYMMETRIC_DIFFERENCE, admission, output);
}
XR_FUNC XrXirValueStatus xr_xir_set_is_subset(const XrXirValue *left, const XrXirValue *right,
    XrXirValueAdmission *admission, bool *output) {
    if (!output || !hash_admission_current(admission)) return XR_XIR_VALUE_BAD_ARGUMENT;
    xr_xir_value_graph_begin();
    XirHash *a = NULL, *b = NULL;
    XrXirValueStatus status = set_pair(left, right, admission, &a, &b);
    bool subset = status == XR_XIR_VALUE_OK && a->length <= b->length;
    XrXirLayout layout = {0};
    if (subset && !xr_xir_compile_type_arena_layout(a->object.arena, a->key_type, &layout)) status = XR_XIR_VALUE_BAD_ARGUMENT;
    for (size_t i = 0; status == XR_XIR_VALUE_OK && subset && i < a->length; ++i) {
        XrXirValue key = storage_leaf_value(a->key_type, hash_span(a, i, false).bytes, layout.size);
        HashProbe probe = {0}; status = hash_probe(b, &key, admission, &probe);
        if (status == XR_XIR_VALUE_OK) subset = probe.found;
    }
    if (status == XR_XIR_VALUE_OK) *output = subset;
    xr_xir_value_graph_end(); return status;
}
XR_FUNC XrXirValueStatus xr_xir_set_is_superset(const XrXirValue *left, const XrXirValue *right,
    XrXirValueAdmission *admission, bool *output) {
    return xr_xir_set_is_subset(right, left, admission, output);
}
