/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xenum.c - Enum object implementation
 */

#include "xenum.h"
#include "../../base/xchecks.h"
#include "../xisolate_api.h"
#include "../../base/xmalloc.h"
#include "../object/xstring.h"
#include "../symbol/xsymbol_table.h"
#include "../gc/xgc_internal.h"
#include "xclass.h"
#include "xclass_system.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ========== Enum Creation ========== */

XrEnumValue* xr_enum_value_new(XrayIsolate *X, const char *enum_name,
                               const char *member_name, XrValue raw_value,
                               uint32_t index) {
    XR_DCHECK(X != NULL, "enum_value_new: NULL isolate");
    XR_DCHECK(enum_name != NULL, "enum_value_new: NULL enum_name");
    XrEnumValue *enum_val = (XrEnumValue*)xr_gc_alloc(xr_isolate_get_gc(X), sizeof(XrEnumValue), XR_TENUM_VALUE);
    if (!enum_val) return NULL;

    enum_val->enum_name = xr_strdup(enum_name);
    enum_val->member_name = xr_strdup(member_name);
    enum_val->raw_value = raw_value;
    enum_val->member_index = index;

    return enum_val;
}

XrEnumType* xr_enum_type_new(XrayIsolate *X, const char *name, int base_type,
                             char **member_names, XrValue *member_values,
                             int count) {
    XrEnumType *enum_type = (XrEnumType*)xr_gc_alloc(xr_isolate_get_gc(X), sizeof(XrEnumType), XR_TENUM_TYPE);
    if (!enum_type) return NULL;

    XrayCoreClasses *core = xr_isolate_get_core_classes(X);
    XrClass *enum_base = core ? core->enumClass : NULL;
    XrClass *enum_class = xr_class_new(X, name, enum_base);
    enum_type->enum_class = enum_class;

    enum_type->name = xr_strdup(name);
    enum_type->base_type = base_type;
    enum_type->member_count = count;

    enum_type->symbol_to_index = NULL;
    enum_type->symbol_map_capacity = 0;
    enum_type->is_contiguous_int = false;
    enum_type->min_int_value = 0;
    enum_type->value_to_index = NULL;
    enum_type->value_map_range = 0;

    enum_type->members = (struct XrEnumMember*)xr_malloc(
        sizeof(*enum_type->members) * count);

    for (int i = 0; i < count; i++) {
        enum_type->members[i].name = xr_strdup(member_names[i]);
        enum_type->members[i].symbol = -1;
        enum_type->members[i].value = member_values[i];
        enum_type->members[i].instance = xr_enum_value_new(
            X, name, member_names[i], member_values[i], i);
    }

    // Initialize symbol mapping for O(1) lookup
    xr_enum_type_init_symbols(enum_type, X);

    /* Build reverse lookup structure for int enums.
     * Three-tier strategy using array/hash split:
     *   Tier 1 (contiguous): values are min,min+1,...,min+N-1 → direct index
     *   Tier 2 (sparse array): range <= count*4 → offset array, O(1)
     *            (each sparse slot = 4 bytes vs 8-byte XrValue scan)
     *   Tier 3 (fallback): linear scan O(n) */
    if (base_type == XR_TINT && count > 0) {
        int64_t min_val = XR_TO_INT(member_values[0]);
        int64_t max_val = min_val;
        bool contiguous = true;

        for (int i = 0; i < count; i++) {
            int64_t v = XR_TO_INT(member_values[i]);
            if (v < min_val) min_val = v;
            if (v > max_val) max_val = v;
            if (v != XR_TO_INT(member_values[0]) + i) contiguous = false;
        }

        enum_type->min_int_value = min_val;

        if (contiguous) {
            // Tier 1: perfect contiguous sequence
            enum_type->is_contiguous_int = true;
        } else {
            int64_t range = max_val - min_val + 1;
            // Tier 2: sparse array if range is bounded (Lua-inspired threshold)
            if (range > 0 && range <= (int64_t)count * 4 && range <= 1024) {
                int r = (int)range;
                enum_type->value_map_range = r;
                enum_type->value_to_index = (int*)xr_malloc(sizeof(int) * r);
                for (int i = 0; i < r; i++) {
                    enum_type->value_to_index[i] = -1;
                }
                for (int i = 0; i < count; i++) {
                    int slot = (int)(XR_TO_INT(member_values[i]) - min_val);
                    enum_type->value_to_index[slot] = i;
                }
            }
            // Tier 3: no optimization, linear scan at runtime
        }
    }

    return enum_type;
}

/* ========== Enum Access ========== */

/* ========== Symbol Mapping ========== */

void xr_enum_type_init_symbols(XrEnumType *enum_type, void *isolate) {
    XR_DCHECK(enum_type != NULL, "enum_type_init_symbols: NULL enum_type");
    XR_DCHECK(isolate != NULL, "enum_type_init_symbols: NULL isolate");
    XrayIsolate *X = (XrayIsolate*)isolate;
    XrSymbolTable *sym_table = (XrSymbolTable*)xr_isolate_get_symbol_table(X);
    if (!sym_table) return;

    // Find max symbol ID to size the direct-mapped array
    int max_symbol = 0;
    for (uint32_t i = 0; i < enum_type->member_count; i++) {
        SymbolId sid = xr_symbol_register_in_table(sym_table, enum_type->members[i].name);
        enum_type->members[i].symbol = sid;
        if (sid > max_symbol) max_symbol = sid;
    }

    // Build symbol_to_index direct-mapped array
    int capacity = max_symbol + 1;
    enum_type->symbol_map_capacity = capacity;
    enum_type->symbol_to_index = (int*)xr_malloc(sizeof(int) * capacity);
    for (int i = 0; i < capacity; i++) {
        enum_type->symbol_to_index[i] = -1;
    }
    for (uint32_t i = 0; i < enum_type->member_count; i++) {
        int sid = enum_type->members[i].symbol;
        if (sid >= 0 && sid < capacity) {
            enum_type->symbol_to_index[sid] = (int)i;
        }
    }
}

/* ========== Access ========== */

XrEnumValue* xr_enum_get_member(XrEnumType *enum_type, const char *member_name) {
    XR_DCHECK(enum_type != NULL, "enum_get_member: NULL enum_type");
    XR_DCHECK(member_name != NULL, "enum_get_member: NULL member_name");
    for (uint32_t i = 0; i < enum_type->member_count; i++) {
        if (strcmp(enum_type->members[i].name, member_name) == 0) {
            return enum_type->members[i].instance;
        }
    }
    return NULL;
}

// O(1) symbol-based member lookup
XrEnumValue* xr_enum_get_member_by_symbol(XrEnumType *enum_type, int symbol) {
    XR_DCHECK(enum_type != NULL, "enum_get_member_by_symbol: NULL enum_type");
    if (symbol >= 0 && symbol < enum_type->symbol_map_capacity &&
        enum_type->symbol_to_index != NULL) {
        int idx = enum_type->symbol_to_index[symbol];
        if (idx >= 0) {
            return enum_type->members[idx].instance;
        }
    }
    return NULL;
}

XrEnumValue* xr_enum_from_value(XrEnumType *enum_type, XrValue value) {
    XR_DCHECK(enum_type != NULL, "enum_from_value: NULL enum_type");
    if (XR_IS_INT(value)) {
        int64_t v = XR_TO_INT(value);
        int64_t offset = v - enum_type->min_int_value;

        // Tier 1: contiguous int — direct index
        if (enum_type->is_contiguous_int) {
            if (offset >= 0 && offset < (int64_t)enum_type->member_count) {
                return enum_type->members[offset].instance;
            }
            return NULL;
        }

        // Tier 2: sparse array — O(1) with bounded waste
        if (enum_type->value_to_index != NULL) {
            if (offset >= 0 && offset < enum_type->value_map_range) {
                int idx = enum_type->value_to_index[offset];
                if (idx >= 0) {
                    return enum_type->members[idx].instance;
                }
            }
            return NULL;
        }
    }

    // Tier 3: linear scan (non-int enums or unbounded int range)
    for (uint32_t i = 0; i < enum_type->member_count; i++) {
        XrValue member_val = enum_type->members[i].value;

        bool equals = false;
        if (XR_IS_INT(value) && XR_IS_INT(member_val)) {
            equals = (XR_TO_INT(value) == XR_TO_INT(member_val));
        } else if (XR_IS_STRING(value) && XR_IS_STRING(member_val)) {
            // Cannot rely on pointer equality: user input may carry a
            // non-interned XrString even though enum members are interned.
            equals = xr_string_equal(XR_TO_STRING(value), XR_TO_STRING(member_val));
        } else if (XR_IS_FLOAT(value) && XR_IS_FLOAT(member_val)) {
            equals = (XR_TO_FLOAT(value) == XR_TO_FLOAT(member_val));
        } else if (XR_IS_BOOL(value) && XR_IS_BOOL(member_val)) {
            equals = (XR_TO_BOOL(value) == XR_TO_BOOL(member_val));
        }

        if (equals) {
            return enum_type->members[i].instance;
        }
    }

    return NULL;
}

const char* xr_enum_value_name(XrEnumValue *enum_val) {
    XR_DCHECK(enum_val != NULL, "enum_value_name: NULL enum_val");
    return enum_val->member_name;
}

/* ========== Cleanup ========== */

void xr_enum_value_free(XrEnumValue *enum_val) {
    if (!enum_val) return;
    xr_free(enum_val->enum_name);
    xr_free(enum_val->member_name);
    xr_free(enum_val);
}

void xr_enum_type_free(XrEnumType *enum_type) {
    if (!enum_type) return;
    for (uint32_t i = 0; i < enum_type->member_count; i++) {
        xr_free(enum_type->members[i].name);
        xr_enum_value_free(enum_type->members[i].instance);
    }
    xr_free(enum_type->members);
    xr_free(enum_type->symbol_to_index);
    xr_free(enum_type->value_to_index);
    xr_free(enum_type->name);
    xr_free(enum_type);
}

