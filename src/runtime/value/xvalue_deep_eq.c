/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvalue_deep_eq.c - Deep value equality.
 */

#include "xvalue.h"
#include "xvalue_hash.h"
#include "../class/xclass.h"
#include "../class/xinstance.h"
#include "../mem/xobj_header.h"
#include "../object/xarray.h"
#include "../object/xjson.h"
#include "../object/xmap.h"
#include "../object/xset.h"
#include "../object/xstring.h"
#include "../object/xtuple.h"
#include "../../shared/xr_deep_equality.h"
#include <stdint.h>
#include <string.h>

static bool vm_deep_describe(void *adapter, XrValue value, XrDeepEqualityNode *out) {
    (void) adapter;
    if (!out)
        return false;
    *out = (XrDeepEqualityNode){
        .kind = XR_DEEP_EQUALITY_IDENTITY,
        .identity = value.ptr,
    };
    if (!XR_IS_PTR(value) || !value.ptr)
        return true;
    switch (value.heap_type) {
        case XR_TSTRING:
            out->kind = XR_DEEP_EQUALITY_STRING;
            return true;
        case XR_TARRAY: {
            XrArray *array = xr_value_to_array(value);
            if (!array || array->length < 0)
                return false;
            out->kind = XR_DEEP_EQUALITY_ARRAY;
            out->nominal_identity = array->elem_type;
            out->logical_count = out->iteration_extent = (uint32_t) array->length;
            return true;
        }
        case XR_TMAP: {
            XrMap *map = xr_value_to_map(value);
            if (!map)
                return false;
            out->kind = XR_DEEP_EQUALITY_MAP;
            out->logical_count = map->count;
            out->iteration_extent = map->nentries;
            return true;
        }
        case XR_TSET: {
            XrSet *set = xr_value_to_set(value);
            if (!set)
                return false;
            out->kind = XR_DEEP_EQUALITY_SET;
            out->logical_count = set->count;
            out->iteration_extent = set->nentries;
            return true;
        }
        case XR_TINSTANCE:
            break;
        default:
            return true;
    }
    XrInstance *instance = xr_value_to_instance(value);
    XrClass *klass = instance ? instance->klass : NULL;
    if (!klass)
        return false;
    if (klass->builtin_kind == XR_BK_TUPLE) {
        out->kind = XR_DEEP_EQUALITY_TUPLE;
        out->logical_count = out->iteration_extent = xr_class_instance_field_count(klass);
    } else if (klass->builtin_kind == XR_BK_ADT_ENUM) {
        XrEnumAggregateValue *aggregate = xr_value_to_enum_aggregate(value);
        XrEnumType *enum_type = aggregate ? aggregate->enum_type : NULL;
        if (!aggregate || !enum_type)
            return false;
        out->kind = XR_DEEP_EQUALITY_ENUM;
        out->nominal_identity = enum_type->layout && enum_type->layout->layout_id
                                    ? enum_type->layout->layout_id
                                    : (uint64_t) (uintptr_t) enum_type;
        out->ordinal = aggregate->member_index;
        out->logical_count = out->iteration_extent = aggregate->payload_count;
    } else if (klass->builtin_kind == XR_BK_STRUCT_OBJECT) {
        out->kind = XR_DEEP_EQUALITY_STRUCT_OBJECT;
        out->nominal_identity = xr_class_object_domain(klass);
        out->logical_count = out->iteration_extent = klass->field_count;
    } else if ((klass->flags & XR_CLASS_DERIVE_EQ) != 0) {
        out->kind = XR_DEEP_EQUALITY_DERIVED_INSTANCE;
        out->nominal_identity = (uint64_t) (uintptr_t) klass;
        out->logical_count = out->iteration_extent = xr_class_instance_field_count(klass);
    }
    return true;
}

static bool vm_deep_fallback_equal(void *adapter, XrValue left, XrValue right) {
    bool key_equivalence = adapter != NULL;
    return key_equivalence ? xr_value_key_eq(left, right) : xr_value_eq(left, right);
}

static bool vm_deep_string_equal(void *adapter, XrValue left, XrValue right) {
    (void) adapter;
    XrString *a = XR_IS_STRING(left) ? XR_TO_STRING(left) : NULL;
    XrString *b = XR_IS_STRING(right) ? XR_TO_STRING(right) : NULL;
    return a && b && (a == b || (a->length == b->length &&
                                 memcmp(a->data, b->data, a->length) == 0));
}

static bool vm_deep_sequence_element(void *adapter, XrValue sequence, uint32_t index,
                                     XrValue *out) {
    (void) adapter;
    if (!out)
        return false;
    if (XR_IS_ARRAY(sequence)) {
        XrArray *array = xr_value_to_array(sequence);
        if (!array || index >= (uint32_t) array->length)
            return false;
        *out = xr_array_get_element(array, (int) index);
        return true;
    }
    XrInstance *instance = xr_value_to_instance(sequence);
    if (!instance || !instance->klass)
        return false;
    if (instance->klass->builtin_kind == XR_BK_ADT_ENUM) {
        XrEnumAggregateValue *aggregate = xr_value_to_enum_aggregate(sequence);
        if (!aggregate || index >= aggregate->payload_count)
            return false;
        *out = aggregate->payloads[index];
        return true;
    }
    uint32_t count = xr_class_instance_field_count(instance->klass);
    if (index >= count)
        return false;
    *out = instance->fields[index];
    return true;
}

static bool vm_deep_map_entry(void *adapter, XrValue value, uint32_t slot, bool *present,
                              XrValue *key, XrValue *entry_value) {
    (void) adapter;
    XrMap *map = xr_value_to_map(value);
    if (!map || !present || !key || !entry_value || slot >= map->nentries)
        return false;
    XrMapEntry *entry = xr_map_entry(map, slot);
    *present = !XR_MAP_ENTRY_EMPTY(entry);
    if (*present) {
        *key = entry->key;
        *entry_value = entry->value;
    }
    return true;
}

static bool vm_deep_map_find(void *adapter, XrValue value, XrValue key, bool *found,
                             XrValue *entry_value) {
    (void) adapter;
    XrMap *map = xr_value_to_map(value);
    if (!map || !found || !entry_value)
        return false;
    *entry_value = xr_map_get(map, key, found);
    return true;
}

static bool vm_deep_set_entry(void *adapter, XrValue value, uint32_t slot, bool *present,
                              XrValue *entry_value) {
    (void) adapter;
    XrSet *set = xr_value_to_set(value);
    if (!set || !present || !entry_value || slot >= set->nentries)
        return false;
    XrSetEntry *entry = xr_set_entry(set, slot);
    *present = !XR_SET_ENTRY_EMPTY(entry);
    if (*present)
        *entry_value = entry->value;
    return true;
}

static bool vm_deep_set_contains(void *adapter, XrValue value, XrValue entry_value,
                                 bool *contains) {
    (void) adapter;
    XrSet *set = xr_value_to_set(value);
    if (!set || !contains)
        return false;
    *contains = xr_set_has(set, entry_value);
    return true;
}

static bool vm_deep_struct_field_pair(void *adapter, XrValue left, XrValue right,
                                      uint32_t left_ordinal, XrValue *left_value,
                                      XrValue *right_value) {
    (void) adapter;
    XrObjectInstance *a = xr_value_to_object_instance(left);
    XrObjectInstance *b = xr_value_to_object_instance(right);
    if (!a || !b || !a->klass || !b->klass || left_ordinal >= a->klass->field_count ||
        !left_value || !right_value)
        return false;
    int symbol = a->klass->fields[left_ordinal].symbol;
    int right_index = xr_class_lookup_field(b->klass, symbol);
    if (right_index < 0)
        return false;
    *left_value = xr_instance_get_dynamic_field(a, (uint16_t) left_ordinal);
    *right_value = xr_instance_get_dynamic_field(b, (uint16_t) right_index);
    return true;
}

static const XrDeepEqualityOps vm_deep_equality_ops = {
    .describe = vm_deep_describe,
    .fallback_equal = vm_deep_fallback_equal,
    .string_equal = vm_deep_string_equal,
    .sequence_element = vm_deep_sequence_element,
    .map_entry = vm_deep_map_entry,
    .map_find_key_equivalent = vm_deep_map_find,
    .set_entry = vm_deep_set_entry,
    .set_contains_key_equivalent = vm_deep_set_contains,
    .struct_field_pair = vm_deep_struct_field_pair,
};

bool xr_value_deep_key_eq(XrValue left, XrValue right) {
    uint8_t key_equivalence = 1;
    return xr_deep_equality_apply(&vm_deep_equality_ops, &key_equivalence, left, right);
}

bool xr_value_deep_eq(XrValue left, XrValue right) {
    return xr_deep_equality_apply(&vm_deep_equality_ops, NULL, left, right);
}
