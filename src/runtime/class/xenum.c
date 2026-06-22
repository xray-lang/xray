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
#include "../core/xr_runtime_core.h"
#include "../../base/xmalloc.h"
#include "../object/xstring.h"
#include "../symbol/xsymbol_table.h"
#include "../mem/xfixed_heap.h"
#include "xclass.h"
#include "xinstance.h"
#include "xreflect_registry.h"
#include "xclass_system.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ========== Enum Creation ========== */

XrEnumValue *xr_enum_value_new(XrayIsolate *X, const char *enum_name, const char *member_name,
                               XrValue raw_value, uint32_t index) {
    XR_DCHECK(X != NULL, "enum_value_new: NULL isolate");
    XR_DCHECK(enum_name != NULL, "enum_value_new: NULL enum_name");
    XrayCoreClasses *core = xr_isolate_get_core_classes(X);
    XrEnumValue *enum_val = (XrEnumValue *) xr_fixed_heap_alloc(xr_isolate_get_fixed_heap(X),
                                                                sizeof(XrEnumValue), XR_TINSTANCE);
    if (!enum_val)
        return NULL;
    enum_val->klass = (core && core->enumValueClass) ? core->enumValueClass : NULL;

    // Names are interned via the isolate's symbol table, so the pointer
    // is stable for the life of the isolate and must not be freed.
    enum_val->enum_name = xr_symbol_intern(X, enum_name);
    enum_val->member_name = xr_symbol_intern(X, member_name);
    enum_val->raw_value = raw_value;
    enum_val->member_index = index;
    enum_val->parent_type = NULL;  // Set by xr_enum_type_new after member creation

    return enum_val;
}

XrEnumValue *xr_enum_value_new_core(XrRuntimeCore *core, const char *enum_name,
                                    const char *member_name, XrValue raw_value, uint32_t index) {
    XR_DCHECK(core != NULL, "enum_value_new_core: NULL core");
    XR_DCHECK(enum_name != NULL, "enum_value_new_core: NULL enum_name");
    XR_DCHECK(member_name != NULL, "enum_value_new_core: NULL member_name");

    XrEnumValue *enum_val =
        (XrEnumValue *) xr_fixed_heap_alloc(&core->fixed_heap, sizeof(XrEnumValue), XR_TINSTANCE);
    if (!enum_val)
        return NULL;
    enum_val->klass = NULL;
    enum_val->enum_name = enum_name;
    enum_val->member_name = member_name;
    enum_val->raw_value = raw_value;
    enum_val->member_index = index;
    enum_val->parent_type = NULL;
    return enum_val;
}

static XrClass *xr_enum_minimal_adt_class_new(const char *name, uint16_t field_count) {
    XrClass *cls = (XrClass *) xr_calloc(1, sizeof(XrClass));
    if (!cls)
        return NULL;
    cls->name = name;
    cls->display_name = name;
    cls->field_count = field_count;
    cls->own_field_count = field_count;
    cls->builtin_kind = XR_BK_ADT_ENUM;
    cls->flags = XR_CLASS_BUILTIN | XR_CLASS_FINAL | XR_CLASS_INITIALIZED;
    return cls;
}

XrEnumType *xr_enum_type_new(XrayIsolate *X, const char *name, int base_type, char **member_names,
                             XrValue *member_values, int count) {
    XrayCoreClasses *core = xr_isolate_get_core_classes(X);
    XrEnumType *enum_type = (XrEnumType *) xr_fixed_heap_alloc(xr_isolate_get_fixed_heap(X),
                                                               sizeof(XrEnumType), XR_TINSTANCE);
    if (!enum_type)
        return NULL;
    enum_type->klass = (core && core->enumTypeClass) ? core->enumTypeClass : NULL;
    XrClass *enum_base = core ? core->enumClass : NULL;
    // xr_class_new -> builder finalize registers the class with the
    // reflection type registry automatically, so the enum is visible
    // through Type.getTypeByName without any follow-up call here.
    XrClass *enum_class = xr_class_new(X, name, enum_base);
    enum_type->enum_class = enum_class;

    enum_type->name = xr_symbol_intern(X, name);
    enum_type->base_type = base_type;
    enum_type->member_count = count;

    enum_type->symbol_to_index = NULL;
    enum_type->symbol_map_capacity = 0;
    enum_type->is_contiguous_int = false;
    enum_type->min_int_value = 0;
    enum_type->value_to_index = NULL;
    enum_type->value_map_range = 0;
    enum_type->is_adt = false;
    enum_type->max_payload = 0;
    enum_type->payload_counts = NULL;
    enum_type->owns_enum_class = false;

    enum_type->members = (struct XrEnumMember *) xr_malloc(sizeof(*enum_type->members) * count);
    if (!enum_type->members) {
        xr_free(enum_type);
        return NULL;
    }

    for (int i = 0; i < count; i++) {
        enum_type->members[i].name = xr_symbol_intern(X, member_names[i]);
        enum_type->members[i].symbol = -1;
        enum_type->members[i].value = member_values[i];
        enum_type->members[i].instance =
            xr_enum_value_new(X, name, member_names[i], member_values[i], i);
        if (enum_type->members[i].instance)
            enum_type->members[i].instance->parent_type = enum_type;
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
            if (v < min_val)
                min_val = v;
            if (v > max_val)
                max_val = v;
            if (v != XR_TO_INT(member_values[0]) + i)
                contiguous = false;
        }

        enum_type->min_int_value = min_val;

        if (contiguous) {
            // Tier 1: perfect contiguous sequence
            enum_type->is_contiguous_int = true;
        } else {
            int64_t range = max_val - min_val + 1;
            // Tier 2: sparse array if range is bounded (Lua-inspired threshold)
            if (range > 0 && range <= (int64_t) count * 4 && range <= 1024) {
                int r = (int) range;
                enum_type->value_map_range = r;
                enum_type->value_to_index = (int *) xr_malloc(sizeof(int) * r);
                if (!enum_type->value_to_index) {
                    enum_type->value_map_range = 0;
                    return enum_type;
                }
                for (int i = 0; i < r; i++) {
                    enum_type->value_to_index[i] = -1;
                }
                for (int i = 0; i < count; i++) {
                    int slot = (int) (XR_TO_INT(member_values[i]) - min_val);
                    enum_type->value_to_index[slot] = i;
                }
            }
            // Tier 3: no optimization, linear scan at runtime
        }
    }

    return enum_type;
}

XrEnumType *xr_enum_type_new_core(XrRuntimeCore *core, const char *name, int base_type,
                                  char **member_names, XrValue *member_values, int count) {
    if (!core || !name || !member_names || !member_values || count <= 0)
        return NULL;

    XrEnumType *enum_type =
        (XrEnumType *) xr_fixed_heap_alloc(&core->fixed_heap, sizeof(XrEnumType), XR_TINSTANCE);
    if (!enum_type)
        return NULL;
    enum_type->klass = NULL;
    enum_type->enum_class = NULL;
    enum_type->name = name;
    enum_type->base_type = base_type;
    enum_type->member_count = (uint32_t) count;
    enum_type->symbol_to_index = NULL;
    enum_type->symbol_map_capacity = 0;
    enum_type->is_contiguous_int = false;
    enum_type->min_int_value = 0;
    enum_type->value_to_index = NULL;
    enum_type->value_map_range = 0;
    enum_type->is_adt = false;
    enum_type->max_payload = 0;
    enum_type->payload_counts = NULL;
    enum_type->owns_enum_class = false;

    enum_type->members = (struct XrEnumMember *) xr_malloc(sizeof(*enum_type->members) * count);
    if (!enum_type->members)
        return NULL;

    for (int i = 0; i < count; i++) {
        enum_type->members[i].name = member_names[i];
        enum_type->members[i].symbol = -1;
        enum_type->members[i].value = member_values[i];
        enum_type->members[i].instance =
            xr_enum_value_new_core(core, name, member_names[i], member_values[i], (uint32_t) i);
        if (enum_type->members[i].instance)
            enum_type->members[i].instance->parent_type = enum_type;
    }

    if (base_type == XR_TINT && count > 0) {
        int64_t min_val = XR_TO_INT(member_values[0]);
        int64_t max_val = min_val;
        bool contiguous = true;

        for (int i = 0; i < count; i++) {
            int64_t v = XR_TO_INT(member_values[i]);
            if (v < min_val)
                min_val = v;
            if (v > max_val)
                max_val = v;
            if (v != XR_TO_INT(member_values[0]) + i)
                contiguous = false;
        }

        enum_type->min_int_value = min_val;
        if (contiguous) {
            enum_type->is_contiguous_int = true;
        } else {
            int64_t range = max_val - min_val + 1;
            if (range > 0 && range <= (int64_t) count * 4 && range <= INT32_MAX) {
                enum_type->value_map_range = (int) range;
                enum_type->value_to_index = (int *) xr_malloc(sizeof(int) * (size_t) range);
                if (enum_type->value_to_index) {
                    for (int64_t i = 0; i < range; i++)
                        enum_type->value_to_index[i] = -1;
                    for (int i = 0; i < count; i++) {
                        int64_t slot = XR_TO_INT(member_values[i]) - min_val;
                        enum_type->value_to_index[slot] = i;
                    }
                } else {
                    enum_type->value_map_range = 0;
                }
            }
        }
    }

    return enum_type;
}

bool xr_enum_type_ensure_adt_class(XrEnumType *enum_type) {
    if (!enum_type)
        return false;
    uint16_t field_count =
        (uint16_t) (1 + (enum_type->max_payload > 0 ? enum_type->max_payload : 0));
    if (enum_type->enum_class) {
        enum_type->enum_class->field_count = field_count;
        enum_type->enum_class->own_field_count = field_count;
        enum_type->enum_class->builtin_kind = XR_BK_ADT_ENUM;
        return true;
    }
    enum_type->enum_class = xr_enum_minimal_adt_class_new(enum_type->name, field_count);
    enum_type->owns_enum_class = enum_type->enum_class != NULL;
    return enum_type->enum_class != NULL;
}

bool xr_enum_type_set_adt_payloads(XrEnumType *enum_type, const int *payload_counts, int count) {
    if (!enum_type || !payload_counts || count <= 0 || (uint32_t) count != enum_type->member_count)
        return false;

    int *payload_copy = (int *) xr_calloc((size_t) count, sizeof(int));
    if (!payload_copy)
        return false;

    int max_payload = 0;
    for (int i = 0; i < count; i++) {
        payload_copy[i] = payload_counts[i];
        if (payload_counts[i] > max_payload)
            max_payload = payload_counts[i];
    }

    if (enum_type->payload_counts)
        xr_free(enum_type->payload_counts);
    enum_type->payload_counts = payload_copy;
    enum_type->is_adt = true;
    enum_type->max_payload = max_payload;
    if (max_payload > 0)
        return xr_enum_type_ensure_adt_class(enum_type);
    return true;
}

/* ========== Enum Access ========== */

/* ========== Symbol Mapping ========== */

void xr_enum_type_init_symbols(XrEnumType *enum_type, void *isolate) {
    XR_DCHECK(enum_type != NULL, "enum_type_init_symbols: NULL enum_type");
    XR_DCHECK(isolate != NULL, "enum_type_init_symbols: NULL isolate");
    XrayIsolate *X = (XrayIsolate *) isolate;
    XrSymbolTable *sym_table = (XrSymbolTable *) xr_isolate_get_symbol_table(X);
    if (!sym_table)
        return;

    // Find max symbol ID to size the direct-mapped array
    int max_symbol = 0;
    for (uint32_t i = 0; i < enum_type->member_count; i++) {
        SymbolId sid = xr_symbol_register_in_table(sym_table, enum_type->members[i].name);
        enum_type->members[i].symbol = sid;
        if (sid > max_symbol)
            max_symbol = sid;
    }

    // Build symbol_to_index direct-mapped array
    int capacity = max_symbol + 1;
    enum_type->symbol_map_capacity = capacity;
    enum_type->symbol_to_index = (int *) xr_malloc(sizeof(int) * capacity);
    if (!enum_type->symbol_to_index) {
        enum_type->symbol_map_capacity = 0;
        return;
    }
    for (int i = 0; i < capacity; i++) {
        enum_type->symbol_to_index[i] = -1;
    }
    for (uint32_t i = 0; i < enum_type->member_count; i++) {
        int sid = enum_type->members[i].symbol;
        if (sid >= 0 && sid < capacity) {
            enum_type->symbol_to_index[sid] = (int) i;
        }
    }
}

/* ========== Access ========== */

// O(1) symbol-based member lookup. There used to be a by-name variant
// as well, but it had no call sites outside its own header declaration
// -- every real lookup goes through xr_symbol_lookup_in_table first,
// yielding a SymbolId that this function consumes directly.
XrEnumValue *xr_enum_get_member_by_symbol(XrEnumType *enum_type, int symbol) {
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

XrEnumValue *xr_enum_from_value(XrEnumType *enum_type, XrValue value) {
    XR_DCHECK(enum_type != NULL, "enum_from_value: NULL enum_type");
    if (XR_IS_INT(value)) {
        int64_t v = XR_TO_INT(value);
        int64_t offset = v - enum_type->min_int_value;

        // Tier 1: contiguous int — direct index
        if (enum_type->is_contiguous_int) {
            if (offset >= 0 && offset < (int64_t) enum_type->member_count) {
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

const char *xr_enum_value_name(XrEnumValue *enum_val) {
    XR_DCHECK(enum_val != NULL, "enum_value_name: NULL enum_val");
    return enum_val->member_name;
}

/* ========== Destroy Hooks ========== */

/* ========== ADT Variant Construction ========== */

XR_FUNC XrInstance *xr_enum_adt_construct_core(XrRuntimeCore *core, struct XrCoroutine *coro,
                                               XrEnumType *enum_type, uint32_t member_index,
                                               XrValue *args, int nargs) {
    XR_DCHECK(core != NULL || coro != NULL, "adt_construct_core: NULL owner");
    XR_DCHECK(enum_type != NULL, "adt_construct: NULL enum_type");
    XR_DCHECK(enum_type->is_adt, "adt_construct: enum is not ADT");
    XR_DCHECK(member_index < enum_type->member_count, "adt_construct: member_index out of bounds");

    int expected_payload = enum_type->payload_counts[member_index];
    int actual_payload = nargs < expected_payload ? nargs : expected_payload;

    /* Instance layout: field[0] = tag (int), field[1..N] = payload.
     * Total fields = 1 + max_payload (uniform across all variants for
     * consistent field_count on the shared class). */
    XrClass *klass = enum_type->enum_class;
    XR_DCHECK(klass != NULL, "adt_construct: NULL enum_class");

    XrInstance *inst = xr_instance_new_core(core, coro, klass);
    if (!inst)
        return NULL;

    /* field[0] = XrEnumValue* for the variant (carries name, tag, parent) */
    XrEnumValue *variant = enum_type->members[member_index].instance;
    XR_DCHECK(variant != NULL, "adt_construct: NULL variant instance");
    inst->fields[0] = XR_FROM_PTR(variant);

    /* field[1..payload_count] = args */
    for (int i = 0; i < actual_payload; i++) {
        inst->fields[1 + i] = args[i];
    }
    /* Zero remaining payload slots (when variant has fewer than max_payload) */
    for (int i = actual_payload; i < enum_type->max_payload; i++) {
        inst->fields[1 + i] = xr_null();
    }

    return inst;
}

XR_FUNC XrInstance *xr_enum_adt_construct(XrayIsolate *X, XrEnumType *enum_type,
                                          uint32_t member_index, XrValue *args, int nargs) {
    return xr_enum_adt_construct_core(xr_isolate_get_runtime_core(X), xr_current_coro(X), enum_type,
                                      member_index, args, nargs);
}

/* Release malloc-backed side resources owned by the enum value.
 * The body itself lives on the isolate fixed_heap list and is freed by
 * xr_fixed_heap_cleanup; this hook must NOT call xr_free(obj). */
void xr_obj_destroy_enum_value(XrObjHeader *obj, XrCoroHeap *owner_heap) {
    (void) owner_heap;
    if (!obj)
        return;
    // enum_name / member_name are interned (symbol table owns them).
    // XrEnumValue currently has no malloc-backed side tables, so this
    // is a no-op — kept registered to make the owner contract explicit.
}

/* Release malloc-backed side resources owned by the enum type. The
 * member instances are individually owned by the fixed_heap list, so this
 * hook only frees the type's own side arrays and the members[] table. */
void xr_obj_destroy_enum_type(XrObjHeader *obj, XrCoroHeap *owner_heap) {
    (void) owner_heap;
    if (!obj)
        return;
    XrEnumType *enum_type = (XrEnumType *) obj;
    if (enum_type->members) {
        // members[].instance bodies are freed by fixed_heap cleanup; this
        // table only stores pointers, so freeing the table is enough.
        xr_free(enum_type->members);
        enum_type->members = NULL;
    }
    if (enum_type->symbol_to_index) {
        xr_free(enum_type->symbol_to_index);
        enum_type->symbol_to_index = NULL;
    }
    if (enum_type->value_to_index) {
        xr_free(enum_type->value_to_index);
        enum_type->value_to_index = NULL;
    }
    if (enum_type->payload_counts) {
        xr_free(enum_type->payload_counts);
        enum_type->payload_counts = NULL;
    }
    if (enum_type->owns_enum_class && enum_type->enum_class) {
        xr_free(enum_type->enum_class);
        enum_type->enum_class = NULL;
        enum_type->owns_enum_class = false;
    }
    // enum_type->name is interned; not owned.
}

/* ========== Native Body Descriptors ========== */

// EnumValue body: everything after klass pointer.
// body_size = offsetof(XrEnumValue, member_index) + sizeof(uint32_t) - offsetof(XrEnumValue,
// enum_name) Simpler: sizeof(XrEnumValue) - sizeof(XrObjHeader) - sizeof(XrClass*)

static void enum_value_body_destroy(void *body) {
    (void) body;
    // No-op: interned names are not owned, raw_value is a tagged value.
}

static XrNativeBodyDesc enum_value_body_desc = {
    .body_size = sizeof(XrEnumValue) - sizeof(XrObjHeader) - sizeof(XrClass *),
    .body_align = _Alignof(const char *),
    .copy_policy = XR_NATIVE_BODY_COPY_FORBID,
    .destroy = enum_value_body_destroy,
    .deep_copy = NULL,
    .to_shared = NULL,
};

static void enum_type_body_destroy(void *body) {
    // body points to 'name' field (native body start = offsetof(XrEnumType, name)).
    // Recover the enclosing struct pointer.
    XrEnumType *et = (XrEnumType *) ((uint8_t *) body - offsetof(XrEnumType, name));
    if (et->members) {
        xr_free(et->members);
        et->members = NULL;
    }
    if (et->symbol_to_index) {
        xr_free(et->symbol_to_index);
        et->symbol_to_index = NULL;
    }
    if (et->value_to_index) {
        xr_free(et->value_to_index);
        et->value_to_index = NULL;
    }
}

static XrNativeBodyDesc enum_type_body_desc = {
    .body_size = sizeof(XrEnumType) - sizeof(XrObjHeader) - sizeof(XrClass *),
    .body_align = _Alignof(const char *),
    .copy_policy = XR_NATIVE_BODY_COPY_FORBID,
    .destroy = enum_type_body_destroy,
    .deep_copy = NULL,
    .to_shared = NULL,
};

XrNativeBodyDesc *xr_enum_value_native_body_desc(void) {
    return &enum_value_body_desc;
}

XrNativeBodyDesc *xr_enum_type_native_body_desc(void) {
    return &enum_type_body_desc;
}
