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
#include "../core/xr_exec_context.h"
#include "../../base/xmalloc.h"
#include "../object/xstring.h"
#include "../symbol/xsymbol_table.h"
#include "../mem/xalloc_unified.h"
#include "../mem/xcoro_heap.h"
#include "../mem/xfixed_heap.h"
#include "xclass.h"
#include "xinstance.h"
#include "xtype_registry.h"
#include "xclass_system.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>

/* ========== Enum Creation ========== */

XrEnumCtor *xr_enum_ctor_new(XrVMRuntime *X, const char *enum_name, const char *member_name,
                             uint32_t index) {
    XR_DCHECK(X != NULL, "enum_ctor_new: NULL isolate");
    XR_DCHECK(enum_name != NULL, "enum_ctor_new: NULL enum_name");
    XrEnumCtor *enum_val = (XrEnumCtor *) xr_fixed_heap_alloc(xr_isolate_get_fixed_heap(X),
                                                              sizeof(XrEnumCtor), XR_TENUM_CTOR);
    if (!enum_val)
        return NULL;

    // Names are interned via the isolate's symbol table, so the pointer
    // is stable for the life of the isolate and must not be freed.
    enum_val->enum_name = xr_symbol_intern(X, enum_name);
    enum_val->member_name = xr_symbol_intern(X, member_name);
    enum_val->member_index = index;
    enum_val->layout_id = 0;
    enum_val->parent_type = NULL;  // Set by xr_enum_type_new after member creation

    return enum_val;
}

XrEnumCtor *xr_enum_ctor_new_core(XrRuntimeCore *core, const char *enum_name,
                                  const char *member_name, uint32_t index) {
    XR_DCHECK(core != NULL, "enum_ctor_new_core: NULL core");
    XR_DCHECK(enum_name != NULL, "enum_ctor_new_core: NULL enum_name");
    XR_DCHECK(member_name != NULL, "enum_ctor_new_core: NULL member_name");

    XrEnumCtor *enum_val =
        (XrEnumCtor *) xr_fixed_heap_alloc(&core->fixed_heap, sizeof(XrEnumCtor), XR_TENUM_CTOR);
    if (!enum_val)
        return NULL;
    enum_val->enum_name = enum_name;
    enum_val->member_name = member_name;
    enum_val->member_index = index;
    enum_val->layout_id = 0;
    enum_val->parent_type = NULL;
    return enum_val;
}

/* Configure a class as the ADT aggregate class for enum_type. Must complete
 * before the class is published via enum_type->enum_class: constructors on
 * other threads read the published class without synchronization. */
static void xr_enum_adt_class_configure(XrClass *cls, XrEnumType *enum_type) {
    cls->field_count = 0;
    cls->own_field_count = 0;
    cls->builtin_kind = XR_BK_ADT_ENUM;
    cls->builtin_data = enum_type;
}

static XrClass *xr_enum_minimal_adt_class_new(XrEnumType *enum_type) {
    XrClass *cls = (XrClass *) xr_calloc(1, sizeof(XrClass));
    if (!cls)
        return NULL;
    cls->name = enum_type->name;
    cls->display_name = enum_type->name;
    cls->flags = XR_CLASS_BUILTIN | XR_CLASS_FINAL | XR_CLASS_INITIALIZED;
    xr_enum_adt_class_configure(cls, enum_type);
    return cls;
}

static XrEnumLayout *xr_enum_layout_from_members(const char *nominal_owner, const char *name,
                                                 const struct XrEnumMember *members, int count) {
    if (!nominal_owner || !name || !members || count <= 0)
        return NULL;

    const char **names = (const char **) xr_malloc(sizeof(*names) * (size_t) count);
    if (!names)
        return NULL;
    for (int i = 0; i < count; i++)
        names[i] = members[i].name;

    XrEnumLayout *layout =
        xr_enum_layout_new(nominal_owner, name, names, (uint32_t) count);
    xr_free(names);
    return layout;
}

XrEnumType *xr_enum_type_new(XrVMRuntime *X, const char *nominal_owner, const char *name,
                             char **member_names, int count) {
    if (!X || !nominal_owner || !nominal_owner[0] || !name || !member_names || count <= 0)
        return NULL;
    XrayCoreClasses *core = xr_isolate_get_core_classes(X);
    XrEnumType *enum_type = (XrEnumType *) xr_fixed_heap_alloc(xr_isolate_get_fixed_heap(X),
                                                               sizeof(XrEnumType), XR_TENUM_TYPE);
    if (!enum_type)
        return NULL;
    enum_type->name = xr_symbol_intern(X, name);
    XrClass *enum_base = core ? core->enumClass : NULL;
    XrClass *enum_class = xr_class_new(X, name, enum_base);
    /* ADT-configure at creation, while registration is still single-threaded.
     * This keeps the enum_class invariant (published => fully configured) so
     * the concurrent construct path never has to write class fields. Doing it
     * unconditionally is safe: every runtime enum value is an ADT aggregate,
     * and fresh classes start with zero fields anyway. */
    if (enum_class)
        xr_enum_adt_class_configure(enum_class, enum_type);
    enum_type->enum_class = enum_class;
    enum_type->member_count = count;

    enum_type->symbol_to_index = NULL;
    enum_type->symbol_map_capacity = 0;
    enum_type->layout = NULL;
    enum_type->derive_flags = 0;
    enum_type->owns_enum_class = false;

    enum_type->members = (struct XrEnumMember *) xr_malloc(sizeof(*enum_type->members) * count);
    if (!enum_type->members) {
        return NULL;
    }

    for (int i = 0; i < count; i++) {
        enum_type->members[i].name = xr_symbol_intern(X, member_names[i]);
        enum_type->members[i].symbol = -1;
        enum_type->members[i].ctor = xr_enum_ctor_new(X, name, member_names[i], i);
        enum_type->members[i].value = NULL;
        if (enum_type->members[i].ctor)
            enum_type->members[i].ctor->parent_type = enum_type;
    }

    const char *owner = xr_symbol_intern(X, nominal_owner);
    enum_type->layout = xr_enum_layout_from_members(owner, enum_type->name, enum_type->members,
                                                    (int) enum_type->member_count);
    if (!enum_type->layout) {
        xr_free(enum_type->members);
        enum_type->members = NULL;
        return NULL;
    }
    for (uint32_t i = 0; i < enum_type->member_count; i++) {
        if (enum_type->members[i].ctor)
            enum_type->members[i].ctor->layout_id = enum_type->layout->layout_id;
    }

    // Initialize symbol mapping for O(1) lookup
    xr_enum_type_init_symbols(enum_type, X);

    return enum_type;
}

XrEnumType *xr_enum_type_new_core(XrRuntimeCore *core, const char *nominal_owner,
                                  const char *name, char **member_names, int count) {
    if (!core || !nominal_owner || !nominal_owner[0] || !name || !member_names || count <= 0)
        return NULL;

    XrEnumType *enum_type =
        (XrEnumType *) xr_fixed_heap_alloc(&core->fixed_heap, sizeof(XrEnumType), XR_TENUM_TYPE);
    if (!enum_type)
        return NULL;
    enum_type->enum_class = NULL;
    enum_type->name = name;
    enum_type->member_count = (uint32_t) count;
    enum_type->symbol_to_index = NULL;
    enum_type->symbol_map_capacity = 0;
    enum_type->layout = NULL;
    enum_type->derive_flags = 0;
    enum_type->owns_enum_class = false;

    enum_type->members = (struct XrEnumMember *) xr_malloc(sizeof(*enum_type->members) * count);
    if (!enum_type->members)
        return NULL;

    for (int i = 0; i < count; i++) {
        enum_type->members[i].name = member_names[i];
        enum_type->members[i].symbol = -1;
        enum_type->members[i].ctor =
            xr_enum_ctor_new_core(core, name, member_names[i], (uint32_t) i);
        enum_type->members[i].value = NULL;
        if (enum_type->members[i].ctor)
            enum_type->members[i].ctor->parent_type = enum_type;
    }

    enum_type->layout = xr_enum_layout_from_members(nominal_owner, enum_type->name,
                                                    enum_type->members,
                                                    (int) enum_type->member_count);
    if (!enum_type->layout) {
        xr_free(enum_type->members);
        enum_type->members = NULL;
        return NULL;
    }
    for (uint32_t i = 0; i < enum_type->member_count; i++) {
        if (enum_type->members[i].ctor)
            enum_type->members[i].ctor->layout_id = enum_type->layout->layout_id;
    }

    return enum_type;
}

bool xr_enum_type_ensure_adt_class(XrEnumType *enum_type) {
    if (!enum_type)
        return false;
    /* Hot path: constructors call this on every aggregate build, racing
     * across workers. A published class is fully configured and immutable,
     * so an acquire load is the whole fast path — no writes. */
    if (atomic_load_explicit(&enum_type->enum_class, memory_order_acquire))
        return true;
    /* Slow path: only core-created enums (enum_class starts NULL) whose ADT
     * class was not set up during registration reach this, possibly from
     * several workers at once. Build a fully configured class, then
     * CAS-publish it; losers discard theirs and use the winner's. */
    XrClass *fresh = xr_enum_minimal_adt_class_new(enum_type);
    if (!fresh)
        return false;
    XrClass *expected = NULL;
    if (!atomic_compare_exchange_strong_explicit(&enum_type->enum_class, &expected, fresh,
                                                 memory_order_acq_rel, memory_order_acquire)) {
        xr_free(fresh);
        return true;
    }
    /* Winner: mark ownership for the destroy hook. Runs at teardown after
     * workers join, so a plain store is ordered by the join. */
    enum_type->owns_enum_class = true;
    return true;
}

bool xr_enum_type_set_adt_payloads(XrEnumType *enum_type, const int *payload_counts, int count) {
    if (!enum_type || !payload_counts || count <= 0 || (uint32_t) count != enum_type->member_count)
        return false;

    if (!xr_enum_layout_set_payload_counts(enum_type->layout, payload_counts, (uint32_t) count))
        return false;

    for (uint32_t i = 0; i < enum_type->member_count; i++) {
        if (enum_type->members[i].ctor)
            enum_type->members[i].ctor->layout_id = enum_type->layout->layout_id;
    }

    if (xr_enum_type_has_payloads(enum_type))
        return xr_enum_type_ensure_adt_class(enum_type);
    return true;
}

bool xr_enum_type_has_payloads(const XrEnumType *enum_type) {
    return enum_type && enum_type->layout && !enum_type->layout->is_zero_payload;
}

int xr_enum_type_payload_count(const XrEnumType *enum_type, uint32_t member_index) {
    if (!enum_type)
        return 0;
    return xr_enum_layout_payload_count(enum_type->layout, member_index);
}

uint16_t xr_enum_type_max_payload(const XrEnumType *enum_type) {
    return enum_type ? xr_enum_layout_max_payload(enum_type->layout) : 0;
}

const char *xr_enum_type_member_name(const XrEnumType *enum_type, uint32_t member_index) {
    const XrEnumVariantLayout *variant =
        enum_type ? xr_enum_layout_variant(enum_type->layout, member_index) : NULL;
    if (variant && variant->name)
        return variant->name;
    if (enum_type && member_index < enum_type->member_count && enum_type->members)
        return enum_type->members[member_index].name;
    return NULL;
}

/* ========== Enum Access ========== */

/* ========== Symbol Mapping ========== */

void xr_enum_type_init_symbols(XrEnumType *enum_type, void *isolate) {
    XR_DCHECK(enum_type != NULL, "enum_type_init_symbols: NULL enum_type");
    XR_DCHECK(isolate != NULL, "enum_type_init_symbols: NULL isolate");
    XrVMRuntime *X = (XrVMRuntime *) isolate;
    XrSymbolTable *sym_table = (XrSymbolTable *) xr_isolate_get_symbol_table(X);
    if (!sym_table)
        return;

    // Find max symbol ID to size the direct-mapped array
    int max_symbol = 0;
    for (uint32_t i = 0; i < enum_type->member_count; i++) {
        SymbolId sid = xr_symbol_register_in_table(sym_table, enum_type->members[i].name);
        enum_type->members[i].symbol = sid;
        xr_enum_layout_set_variant_symbol(enum_type->layout, i, sid);
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
XrEnumCtor *xr_enum_get_member_by_symbol(XrEnumType *enum_type, int symbol) {
    XR_DCHECK(enum_type != NULL, "enum_get_member_by_symbol: NULL enum_type");
    int idx = xr_enum_type_find_member_index_by_symbol(enum_type, symbol);
    if (idx >= 0)
        return enum_type->members[idx].ctor;
    return NULL;
}

int xr_enum_type_find_member_index_by_symbol(XrEnumType *enum_type, int symbol) {
    if (!enum_type || symbol < 0 || symbol >= enum_type->symbol_map_capacity ||
        !enum_type->symbol_to_index)
        return -1;
    int idx = enum_type->symbol_to_index[symbol];
    return (idx >= 0 && (uint32_t) idx < enum_type->member_count) ? idx : -1;
}

const char *xr_enum_ctor_name(XrEnumCtor *enum_val) {
    XR_DCHECK(enum_val != NULL, "enum_value_name: NULL enum_val");
    if (enum_val->parent_type) {
        const char *name = xr_enum_type_member_name(enum_val->parent_type, enum_val->member_index);
        if (name)
            return name;
    }
    return enum_val->member_name;
}

/* ========== Destroy Hooks ========== */

/* ========== ADT Variant Construction ========== */

size_t xr_enum_aggregate_size(uint32_t payload_count) {
    return sizeof(XrEnumAggregateValue) + (size_t) payload_count * sizeof(XrValue);
}

void xr_enum_aggregate_init_inplace(XrEnumAggregateValue *value, XrEnumType *enum_type,
                                    uint32_t member_index, const XrValue *payloads,
                                    uint32_t payload_count) {
    if (!value)
        return;
    value->klass =
        enum_type ? atomic_load_explicit(&enum_type->enum_class, memory_order_acquire) : NULL;
    value->enum_type = enum_type;
    value->member_index = member_index;
    value->payload_count = payload_count;
    for (uint32_t i = 0; i < payload_count; i++)
        value->payloads[i] = payloads ? payloads[i] : XR_NULL_VAL;
}

XR_FUNC XrEnumAggregateValue *xr_enum_adt_construct_in(XrAllocationContext *alloc,
                                                       XrEnumType *enum_type, uint32_t member_index,
                                                       XrValue *args, int nargs) {
    XR_DCHECK(alloc != NULL, "adt_construct_in: NULL allocation context");
    XR_DCHECK(enum_type != NULL, "adt_construct: NULL enum_type");
    XR_DCHECK(member_index < enum_type->member_count, "adt_construct: member_index out of bounds");
    if (!xr_enum_type_ensure_adt_class(enum_type))
        return NULL;

    int expected_payload = xr_enum_type_payload_count(enum_type, member_index);
    int actual_payload = nargs < expected_payload ? nargs : expected_payload;

    XrValue *payloads = NULL;
    XrValue local_payloads[8];
    if (expected_payload > 0) {
        payloads = expected_payload <= (int) (sizeof(local_payloads) / sizeof(local_payloads[0]))
                       ? local_payloads
                       : (XrValue *) xr_malloc((size_t) expected_payload * sizeof(XrValue));
        if (!payloads)
            return NULL;
        for (int i = 0; i < actual_payload; i++)
            payloads[i] = args ? args[i] : XR_NULL_VAL;
        for (int i = actual_payload; i < expected_payload; i++)
            payloads[i] = XR_NULL_VAL;
    }

    size_t size = xr_enum_aggregate_size((uint32_t) expected_payload);
    XrEnumAggregateValue *value =
        (XrEnumAggregateValue *) xr_alloc_context_new_object(alloc, size, XR_TINSTANCE);
    if (!value) {
        if (payloads && payloads != local_payloads)
            xr_free(payloads);
        return NULL;
    }

    xr_obj_header_init_type(&value->hdr, XR_TINSTANCE);
    xr_enum_aggregate_init_inplace(value, enum_type, member_index, payloads,
                                   (uint32_t) expected_payload);
    if (payloads && payloads != local_payloads)
        xr_free(payloads);

    return value;
}

XrEnumAggregateValue *xr_enum_zero_payload_value(XrVMRuntime *X, XrEnumType *enum_type,
                                                 uint32_t member_index) {
    if (!X || !enum_type || member_index >= enum_type->member_count)
        return NULL;
    if (xr_enum_type_payload_count(enum_type, member_index) != 0)
        return NULL;
    /* Fast path: the canonical value is published once and immutable. */
    XrEnumAggregateValue *value =
        atomic_load_explicit(&enum_type->members[member_index].value, memory_order_acquire);
    if (value)
        return value;
    /* First touch can race across workers. Serialize on metadata_lock: the
     * loser must not build a duplicate, and module_alloc is not a concurrent
     * allocator. Cold path — one contended pass per member ever. */
    XrRuntimeCore *core = xr_isolate_get_runtime_core(X);
    xr_amutex_lock(&core->metadata_lock);
    value = atomic_load_explicit(&enum_type->members[member_index].value, memory_order_acquire);
    if (value) {
        xr_amutex_unlock(&core->metadata_lock);
        return value;
    }
    value = xr_enum_adt_construct_in(&core->module_alloc, enum_type, member_index, NULL, 0);
    if (value) {
        /* A unit variant carries no payload and never changes, and this one
         * value is cached on the enum type for the life of the module. That
         * makes it an immortal shared constant, not a coroutine-local object:
         * mark it so, or crossing an execution boundary -- a task's error
         * payload, a channel send -- fails the publish check that requires
         * shared or transferred storage. Sticky RC keeps every retain/release
         * on it a no-op, which is what an immortal value wants on both the
         * atomic and the thread-local fast path. */
        XR_OBJ_SET_STORAGE(&value->hdr, XR_OBJ_STORAGE_SHARED);
        atomic_store_explicit(&value->hdr.refcount, XR_RC_STICKY, memory_order_relaxed);
        /* Publish only after the value is fully marked; concurrent fast-path
         * readers synchronize on this release store. */
        atomic_store_explicit(&enum_type->members[member_index].value, value, memory_order_release);
    }
    xr_amutex_unlock(&core->metadata_lock);
    return value;
}

XR_FUNC XrEnumAggregateValue *xr_enum_adt_construct(XrVMRuntime *X, XrEnumType *enum_type,
                                                    uint32_t member_index, XrValue *args,
                                                    int nargs) {
    XrAllocationContext *alloc = xr_alloc_context_current();
    return alloc && alloc->core == xr_isolate_get_runtime_core(X)
               ? xr_enum_adt_construct_in(alloc, enum_type, member_index, args, nargs)
               : NULL;
}

XR_FUNC XrEnumAggregateValue *xr_enum_adt_construct_storage(XrVMRuntime *X, XrEnumType *enum_type,
                                                            uint32_t member_index, XrValue *args,
                                                            int nargs, uint8_t storage_mode) {
    if (storage_mode == XR_OBJ_STORAGE_NORMAL)
        return xr_enum_adt_construct(X, enum_type, member_index, args, nargs);
    XrRuntimeCore *core = X ? xr_isolate_get_runtime_core(X) : NULL;
    if (!core)
        return NULL;
    XrAllocationContext alloc;
    xr_alloc_context_init(&alloc, core,
                          storage_mode == XR_OBJ_STORAGE_TRANSFER ? XR_STORAGE_TRANSFERABLE
                                                                  : XR_STORAGE_SYNC_SHARED);
    return xr_enum_adt_construct_in(&alloc, enum_type, member_index, args, nargs);
}

bool xr_value_is_enum_aggregate(XrValue value) {
    if (!XR_IS_PTR(value) || !value.ptr || value.heap_type != XR_TINSTANCE)
        return false;
    XrEnumAggregateValue *agg = (XrEnumAggregateValue *) value.ptr;
    return agg->klass && agg->klass->builtin_kind == XR_BK_ADT_ENUM;
}

XrEnumAggregateValue *xr_value_to_enum_aggregate(XrValue value) {
    return xr_value_is_enum_aggregate(value) ? (XrEnumAggregateValue *) value.ptr : NULL;
}

XrEnumType *xr_enum_aggregate_type(const XrEnumAggregateValue *value) {
    if (!value)
        return NULL;
    if (value->enum_type)
        return value->enum_type;
    return value->klass ? (XrEnumType *) value->klass->builtin_data : NULL;
}

const char *xr_enum_aggregate_member_name(const XrEnumAggregateValue *value) {
    XrEnumType *type = xr_enum_aggregate_type(value);
    return type ? xr_enum_type_member_name(type, value->member_index) : NULL;
}

XrValue xr_enum_aggregate_field_get(const XrEnumAggregateValue *value, int64_t index) {
    if (!value)
        return XR_NULL_VAL;
    if (index == 0)
        return xr_int((int64_t) value->member_index);
    if (index > 0 && (uint32_t) index <= value->payload_count)
        return value->payloads[index - 1];
    return XR_NULL_VAL;
}

XrValue xr_enum_aggregate_payload_get(const XrEnumAggregateValue *value, uint32_t index) {
    if (!value || index >= value->payload_count)
        return XR_NULL_VAL;
    return value->payloads[index];
}

/* Release malloc-backed side resources owned by the enum constructor metadata.
 * The body itself lives on the isolate fixed_heap list and is freed by
 * xr_fixed_heap_cleanup; this hook must NOT call xr_free(obj). */
void xr_obj_destroy_enum_ctor(XrObjHeader *obj, XrCoroHeap *owner_heap) {
    (void) owner_heap;
    if (!obj)
        return;
    // enum_name / member_name are interned (symbol table owns them).
    // XrEnumCtor currently has no malloc-backed side tables, so this
    // is a no-op — kept registered to make the owner contract explicit.
}

/* Release malloc-backed side resources owned by the enum type. The
 * member constructors are individually owned by the fixed_heap list, so this
 * hook only frees the type's own side arrays and the members[] table. */
void xr_obj_destroy_enum_type(XrObjHeader *obj, XrCoroHeap *owner_heap) {
    (void) owner_heap;
    if (!obj)
        return;
    XrEnumType *enum_type = (XrEnumType *) obj;
    if (enum_type->members) {
        // members[].ctor bodies are freed by fixed_heap cleanup; this
        // table only stores pointers, so freeing the table is enough.
        xr_free(enum_type->members);
        enum_type->members = NULL;
    }
    if (enum_type->symbol_to_index) {
        xr_free(enum_type->symbol_to_index);
        enum_type->symbol_to_index = NULL;
    }
    if (enum_type->layout) {
        xr_enum_layout_free(enum_type->layout);
        enum_type->layout = NULL;
    }
    if (enum_type->owns_enum_class && enum_type->enum_class) {
        xr_free(enum_type->enum_class);
        enum_type->enum_class = NULL;
        enum_type->owns_enum_class = false;
    }
    // enum_type->name is interned; not owned.
}
