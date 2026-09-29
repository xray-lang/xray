/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_value.c - Atomic value ownership and failure-atomic UTF-8 mutation
 *
 * KEY CONCEPT:
 *   Strings retain only their allocation domain; code and execution may die first.
 */
#include "xxir_value.h"
#include "xxir_value_internal.h"
#include "xxir_type_arena.h"
#include "xxir_types.h"
#include "xxir_array.h"
#include "xxir_struct.h"
#include "xxir_enum.h"
#include "xxir_error.h"
#include "xxir_panic.h"
#include "../base/xmalloc.h"
#include "../base/xchecks.h"
#include "../shared/xr_utf8_core.h"
#include "../shared/xr_string_core.h"
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

struct XrXirDomain {
    _Atomic(uint32_t) references;
    atomic_bool locked;
    uint64_t limit;
    XrXirDomainStats stats;
};
typedef struct XirAtomicI64 {
    XirObject object;
    _Atomic(int64_t) value;
} XirAtomicI64;
typedef struct XirFunction {
    XirObject object;
    XrXirFunctionBinding binding;
} XirFunction;
typedef struct XirCell {
    XirObject object;
    XrXirValue value;
} XirCell;
typedef struct XirArray {
    XirObject object;
    unsigned char *data;
    size_t length, capacity;
    XrXirType element;
    uint32_t stride;
} XirArray;
typedef struct XirString {
    XirObject object;
    char *bytes;
    size_t length, runes, capacity;
} XirString;
typedef struct XirPanicInfo {
    XirObject object;
    XrXirFaultDetail detail;
} XirPanicInfo;

_Static_assert(sizeof(void *) == sizeof(int64_t), "XIR pointer payload width");
_Static_assert(sizeof(XrXirFaultDetail) == 24 && _Alignof(XrXirFaultDetail) == 8,
               "XIR bounds detail layout");


static void domain_lock(XrXirDomain *domain) {
    while (atomic_exchange_explicit(&domain->locked, true, memory_order_acquire)) { }
}
static void domain_unlock(XrXirDomain *domain) {
    atomic_store_explicit(&domain->locked, false, memory_order_release);
}
static bool unit_value(const XrXirValue *value) {
    return value && !value->type && !value->reserved && !value->payload;
}
static XirObject *object_pointer(const XrXirValue *value) {
    XirObject *string;
    memcpy(&string, &value->payload, sizeof(string));
    return string;
}
static XirString *string_pointer(const XrXirValue *value) {
    return (XirString *) object_pointer(value);
}
static XrXirValue string_value(XirString *string) {
    XrXirValue value = {XR_XIR_STRING, 0, 0};
    memcpy(&value.payload, &string, sizeof(string));
    return value;
}
/* Built-in carriers own no type arena and no nested values. */
static bool arena_free_carrier(XrXirType type) {
    return type == XR_XIR_STRING || type == XR_XIR_ATOMIC_I64 || type == XR_XIR_PANIC_INFO;
}
static bool owned_carrier_type(XrXirType type) {
    return arena_free_carrier(type) || type == XR_XIR_ERROR ||
        ((uint32_t) type >= XR_XIR_CONSTRUCTED_TYPE_BASE &&
         (uint32_t) type < XR_XIR_CONSTRUCTED_TYPE_LIMIT);
}
static bool array_layout(const XrXirTypeArena *arena, XrXirType type,
                         XrXirType *element, uint32_t *stride) {
    const XrXirTypes *types = xr_xir_type_arena_types(arena);
    const XrXirTypeNode *node = xr_xir_type_node(types, type);
    XrXirLayout layout = {0};
    if (!node || node->kind != XR_XIR_TYPE_ARRAY || node->parameter_span ||
        !xr_xir_type_arena_layout(arena, node->element, &layout) ||
        layout.alignment > _Alignof(int64_t)) return false;
    *element = node->element; *stride = layout.size;
    return true;
}
static bool array_storage_valid(const XirArray *array) {
    XrXirType element = XR_XIR_UNIT; uint32_t stride = 0;
    return array_layout(array->object.arena, array->object.type, &element, &stride) &&
        array->element == element && array->stride == stride && array->length <= array->capacity &&
        array->capacity <= INT64_MAX && (!stride || array->capacity <= SIZE_MAX / stride) &&
        ((array->capacity != 0 && stride != 0) == (array->data != NULL));
}
static XrXirValue storage_leaf_value(XrXirType type, const unsigned char *bytes, uint32_t size) {
    XR_CHECK(bytes && size && size <= sizeof(uint64_t), "compact leaf needs exact payload storage");
    XrXirValue value = {(uint32_t)type, 0, 0};
    uint64_t bits = 0;
    memcpy(&bits, bytes, size);
    if (xr_xir_integer_signed(type) && size < sizeof(bits) &&
        (bits & (UINT64_C(1) << (size * 8 - 1)))) bits |= UINT64_MAX << (size * 8);
    memcpy(&value.payload, &bits, sizeof(bits));
    return value;
}
static bool value_header_valid(const XrXirValue *value) {
    if (!value || value->reserved) return false;
    XrXirType type = (XrXirType) value->type;
    if (unit_value(value)) return true;
    if (xr_xir_integer_payload_valid(type, value->payload) ||
        xr_xir_float_payload_valid(type, value->payload) ||
        (type == XR_XIR_BOOL && (value->payload == 0 || value->payload == 1))) return true;
    if (!owned_carrier_type(type)) return false;
    XirObject *object = object_pointer(value);
    if (!object || (type != XR_XIR_ERROR && object->type != type) || !object->domain ||
        !atomic_load_explicit(&object->references, memory_order_relaxed)) return false;
    if (type == XR_XIR_ERROR) {
        if (!xr_xir_type_is_enum(xr_xir_type_arena_types(object->arena), object->type)) return false;
        type = object->type;
    }
    if (type == XR_XIR_PANIC_INFO)
        return !object->arena && !object->kind && xr_xir_fault_panic_valid(((XirPanicInfo *) object)->detail);
    if (arena_free_carrier(type))
        return !object->arena && !object->kind;
    const XrXirTypeNode *node = xr_xir_type_node(xr_xir_type_arena_types(object->arena), type);
    return node && !node->parameter_span && node->kind == object->kind &&
        (node->kind == XR_XIR_TYPE_CALLABLE || node->kind == XR_XIR_TYPE_CELL || node->kind == XR_XIR_TYPE_ARRAY ||
         node->kind == XR_XIR_TYPE_NOMINAL);
}
XR_FUNC bool xr_xir_value_valid(const XrXirValue *value) {
    if (!value_header_valid(value)) return false;
    if (!owned_carrier_type((XrXirType) value->type)) return true;
    XirObject *object = object_pointer(value);
    if (object->kind == XR_XIR_TYPE_NOMINAL) {
        const XirNominalValue *record = (const XirNominalValue *) object;
        const XrXirTypes *types = xr_xir_type_arena_types(object->arena);
        const XrXirTypeNode *node = xr_xir_type_node(types, object->type);
        const XrXirNominalIdentity *identity = &types->nominals->identities[node->nominal.declaration];
        uint32_t begin = 0, count = node->nominal.field_count;
        if (identity->kind == XR_XIR_NOMINAL_ENUM) {
            if (record->variant >= identity->variant_count) return false;
            begin = identity->variants[record->variant].field_begin;
            count = identity->variants[record->variant].field_count;
            if (!count) return !record->count && !record->fields &&
                record == xr_xir_type_arena_empty_variant(object->arena, object->type, record->variant);
        } else if (identity->kind != XR_XIR_NOMINAL_STRUCT || record->variant) return false;
        if (record->count != count || record->fields != (const XrXirValue *) (record + 1)) return false;
        for (uint32_t i = 0; i < count; ++i)
            if (record->fields[i].type != (uint32_t) node->nominal.fields[begin + i] || !value_header_valid(&record->fields[i])) return false;
        return true;
    }
    if (object->kind == XR_XIR_TYPE_ARRAY) return array_storage_valid((XirArray *) object);
    if (object->kind == XR_XIR_TYPE_CALLABLE) {
        const XrXirFunctionBinding *binding = &((XirFunction *) object)->binding;
        return binding->owner && binding->release && binding->capture_count <= 65536 &&
            ((binding->capture_count != 0) == (binding->captures != NULL));
    }
    if (object->kind == XR_XIR_TYPE_CELL) {
        const XrXirValue *content = &((XirCell *) object)->value;
        XrXirType element = xr_xir_cell_element(xr_xir_type_arena_types(object->arena), object->type);
        if (!element || content->type != (uint32_t) element || !value_header_valid(content)) return false;
        if (owned_carrier_type(element) && !arena_free_carrier(element)) {
            XirObject *child = object_pointer(content);
            return child->arena == object->arena && child->kind != XR_XIR_TYPE_CELL && xr_xir_value_valid(content);
        }
    }
    return true;
}
XR_FUNC const XrXirTypeArena *xr_xir_value_arena(const XrXirValue *value) {
    return xr_xir_value_valid(value) && owned_carrier_type((XrXirType) value->type) ?
        object_pointer(value)->arena : NULL;
}
XR_FUNC bool xr_xir_value_argument(const XrXirValue *value, const XrXirTypeArena *arena, XrXirType type) {
    if (!value || value->type != (uint32_t) type || !xr_xir_value_valid(value)) return false;
    if (type == XR_XIR_ERROR || (uint32_t) type >= XR_XIR_CONSTRUCTED_TYPE_BASE)
        return arena && object_pointer(value)->arena == arena;
    return type != XR_XIR_UNIT;
}
XR_FUNC XrXirValueStatus xr_xir_domain_new(uint64_t limit, XrXirDomain **output) {
    if (!output) return XR_XIR_VALUE_BAD_ARGUMENT;
    *output = NULL;
    if (limit < sizeof(XrXirDomain)) return XR_XIR_VALUE_LIMIT;
    XrXirDomain *domain = xr_malloc(sizeof(*domain));
    if (!domain) return XR_XIR_VALUE_OOM;
    atomic_init(&domain->references, 1);
    atomic_init(&domain->locked, false);
    domain->limit = limit;
    domain->stats = (XrXirDomainStats) {sizeof(*domain), sizeof(*domain), 1, 0, 0};
    *output = domain;
    return XR_XIR_VALUE_OK;
}
XR_FUNC bool xr_xir_domain_retain(XrXirDomain *domain) {
    return domain && xr_xir_reference_retain(&domain->references);
}
XR_FUNC void xr_xir_domain_drop(XrXirDomain *domain) {
    if (domain && xr_xir_reference_release(&domain->references)) {
        XR_CHECK(domain->stats.live_bytes == sizeof(*domain) &&
                 domain->stats.allocations == domain->stats.frees + 1,
                 "domain still owns storage");
        xr_free(domain);
    }
}
XR_FUNC XrXirDomainStats xr_xir_domain_stats(XrXirDomain *domain) {
    if (!domain) return (XrXirDomainStats) {0};
    domain_lock(domain);
    XrXirDomainStats stats = domain->stats;
    domain_unlock(domain);
    return stats;
}
XR_FUNC void *xr_xir_domain_allocate(XrXirDomain *domain, size_t bytes, XrXirValueStatus *status) {
    domain_lock(domain);
    if (bytes > domain->limit - domain->stats.live_bytes || domain->stats.allocations == UINT64_MAX) {
        domain_unlock(domain);
        *status = XR_XIR_VALUE_LIMIT;
        return NULL;
    }
    void *memory = xr_malloc(bytes);
    if (memory) {
        domain->stats.live_bytes += bytes;
        if (domain->stats.live_bytes > domain->stats.peak_bytes)
            domain->stats.peak_bytes = domain->stats.live_bytes;
        ++domain->stats.allocations;
    } else *status = XR_XIR_VALUE_OOM;
    domain_unlock(domain);
    return memory;
}
XR_FUNC void xr_xir_domain_deallocate(XrXirDomain *domain, void *memory, size_t bytes) {
    domain_lock(domain);
    XR_CHECK(memory && domain->stats.live_bytes >= bytes &&
             domain->stats.allocations > domain->stats.frees, "invalid string storage release");
    xr_free(memory);
    domain->stats.live_bytes -= bytes;
    ++domain->stats.frees;
    domain_unlock(domain);
}
static XrXirValueStatus string_resize(XirString *string, size_t capacity) {
    XrXirDomain *domain = string->object.domain;
    domain_lock(domain);
    size_t extra = capacity - string->capacity;
    if (extra > domain->limit - domain->stats.live_bytes || domain->stats.reallocations == UINT64_MAX) {
        domain_unlock(domain);
        return XR_XIR_VALUE_LIMIT;
    }
    char *replacement = xr_realloc(string->bytes, capacity);
    if (!replacement) {
        domain_unlock(domain);
        return XR_XIR_VALUE_OOM;
    }
    string->bytes = replacement;
    string->capacity = capacity;
    domain->stats.live_bytes += extra;
    if (domain->stats.live_bytes > domain->stats.peak_bytes)
        domain->stats.peak_bytes = domain->stats.live_bytes;
    ++domain->stats.reallocations;
    domain_unlock(domain);
    return XR_XIR_VALUE_OK;
}
static size_t string_capacity(size_t required) {
    size_t capacity = 16;
    while (capacity < required && capacity <= SIZE_MAX / 2) capacity *= 2;
    return capacity < required ? required : capacity;
}
static XrXirValueStatus string_allocate(XrXirDomain *domain, size_t length, XirString **output) {
    *output = NULL;
    if (length == SIZE_MAX) return XR_XIR_VALUE_LIMIT;
    if (!xr_xir_reference_retain(&domain->references)) return XR_XIR_VALUE_REFCOUNT_LIMIT;
    XrXirValueStatus status = XR_XIR_VALUE_OK;
    XirString *string = xr_xir_domain_allocate(domain, sizeof(*string), &status);
    if (string) {
        string->capacity = string_capacity(length + 1);
        string->bytes = xr_xir_domain_allocate(domain, string->capacity, &status);
        if (string->bytes) {
            atomic_init(&string->object.references, 1);
            string->object.domain = domain;
            string->object.type = XR_XIR_STRING;
            string->object.arena = NULL; string->object.kind = 0;
            string->length = length;
            string->runes = 0;
            string->bytes[length] = 0;
            *output = string;
            return XR_XIR_VALUE_OK;
        }
        xr_xir_domain_deallocate(domain, string, sizeof(*string));
    }
    xr_xir_domain_drop(domain);
    return status;
}
XR_FUNC XrXirValueStatus xr_xir_string_new(XrXirDomain *domain, const char *bytes,
                                  size_t length, XrXirValue *output) {
    if (!domain || !unit_value(output) || (length && !bytes)) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (length == SIZE_MAX) return XR_XIR_VALUE_LIMIT;
    XrUtf8ScanResult scan = xr_utf8_core_scan_strict((const uint8_t *) bytes, length);
    if (scan.error != XR_UTF8_OK) return XR_XIR_VALUE_BAD_UTF8;
    XirString *string = NULL;
    XrXirValueStatus status = string_allocate(domain, length, &string);
    if (status != XR_XIR_VALUE_OK) return status;
    if (length) memcpy(string->bytes, bytes, length);
    string->runes = scan.rune_count;
    *output = string_value(string);
    return XR_XIR_VALUE_OK;
}
static bool empty_enum_object(const XirObject *object) {
    return object->kind == XR_XIR_TYPE_NOMINAL &&
        !((const XirNominalValue *) object)->count &&
        xr_xir_type_is_enum(xr_xir_type_arena_types(object->arena), object->type);
}
XR_FUNC XrXirValueStatus xr_xir_value_copy(const XrXirValue *source, XrXirValue *output) {
    if (!unit_value(output) || !source ||
        !xr_xir_value_valid(source))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    if (owned_carrier_type((XrXirType) source->type)) {
        XirObject *object = object_pointer(source);
        if (!(empty_enum_object(object) ? xr_xir_type_arena_retain(object->arena) :
            xr_xir_reference_retain(&object->references))) return XR_XIR_VALUE_REFCOUNT_LIMIT;
    }
    *output = *source;
    return XR_XIR_VALUE_OK;
}
static void queue_release(const XrXirValue *value, XirObject **pending) {
    XR_CHECK(xr_xir_value_valid(value), "invalid nested owned value release");
    if (!owned_carrier_type((XrXirType) value->type)) return;
    XirObject *object = object_pointer(value);
    if (empty_enum_object(object)) { xr_xir_type_arena_drop(object->arena); return; }
    if (xr_xir_reference_release(&object->references)) {
        object->release_next = *pending; *pending = object;
    }
}
#include "xxir_storage_cursor.inc.c"
static uint32_t array_release_depth(const XrXirTypeArena *arena, XrXirType element) {
    const XrXirStorageLayout *layout = xr_xir_type_arena_storage(arena, element);
    return layout ? layout->owned_depth : 0;
}
static uint64_t array_header_bytes(const XrXirTypeArena *arena, XrXirType element) {
    _Static_assert(_Alignof(XirArray) >= _Alignof(StorageFrame), "array tail aligns release frames");
    return sizeof(XirArray) + (uint64_t)array_release_depth(arena, element) * sizeof(StorageFrame);
}
static bool array_owned_elements(const XirArray *array) {
    if (xr_xir_type_is_nominal(xr_xir_type_arena_types(array->object.arena), array->element))
        return array_release_depth(array->object.arena, array->element) != 0;
    return owned_carrier_type(array->element);
}
static void release_pending(XirObject *pending) {
    while (pending) {
        XirObject *object = pending; pending = object->release_next;
        XrXirDomain *domain = object->domain;
        XrXirTypeArena *arena = object->arena;
        if (object->type == XR_XIR_STRING) {
            XirString *string = (XirString *) object;
            xr_xir_domain_deallocate(domain, string->bytes, string->capacity);
            xr_xir_domain_deallocate(domain, string, sizeof(*string));
        } else if (object->kind == XR_XIR_TYPE_CELL) {
            XirCell *cell = (XirCell *) object;
            queue_release(&cell->value, &pending);
            xr_xir_domain_deallocate(domain, cell, sizeof(*cell));
        } else if (object->kind == XR_XIR_TYPE_CALLABLE) {
            XirFunction *function = (XirFunction *) object;
            XrXirFunctionBinding binding = function->binding;
            for (uint32_t i = 0; i < binding.capture_count; ++i)
                queue_release(&binding.captures[i], &pending);
            xr_xir_domain_deallocate(domain, function, sizeof(*function) +
                (size_t) binding.capture_count * sizeof(XrXirValue));
            binding.release(binding.owner);
        } else if (object->kind == XR_XIR_TYPE_NOMINAL) {
            XirNominalValue *record = (XirNominalValue *) object;
            for (uint32_t i = record->count; i; --i) queue_release(&record->fields[i - 1], &pending);
            xr_xir_domain_deallocate(domain, record, sizeof(*record) + (size_t) record->count * sizeof(XrXirValue));
        } else if (object->kind == XR_XIR_TYPE_ARRAY) {
            XirArray *array = (XirArray *) object;
            uint32_t depth = array_release_depth(arena, array->element);
            for (size_t i = 0; array_owned_elements(array) && i < array->length; ++i) {
                StorageCursor cursor = {0};
                StorageSpan span = {array->element, array->data ? array->data + i * array->stride : NULL};
                XR_CHECK(storage_cursor_init(arena, span, depth ? (StorageFrame *)(array + 1) : NULL,
                    depth, true, &cursor) == XR_XIR_VALUE_OK,
                    "array release requires its sealed storage layout");
                storage_queue_release(&cursor, UINT64_MAX, &pending);
            }
            if (array->data) xr_xir_domain_deallocate(domain, array->data, array->capacity * array->stride);
            xr_xir_domain_deallocate(domain, array, (size_t)array_header_bytes(arena, array->element));
        } else if (object->type == XR_XIR_PANIC_INFO) {
            xr_xir_domain_deallocate(domain, object, sizeof(XirPanicInfo));
        } else {
            XR_CHECK(object->type == XR_XIR_ATOMIC_I64, "unknown owned value kind");
            xr_xir_domain_deallocate(domain, object, sizeof(XirAtomicI64));
        }
        xr_xir_type_arena_drop(arena);
        xr_xir_domain_drop(domain);
    }
}
XR_FUNC void xr_xir_value_drop(XrXirValue *value) {
    if (!value) return;
    XR_CHECK(xr_xir_value_valid(value), "invalid owned value release");
    XirObject *pending = NULL;
    queue_release(value, &pending);
    *value = (XrXirValue) {0};
    release_pending(pending);
}
XR_FUNC XrXirValueStatus xr_xir_string_append(XrXirValue *destination, const XrXirValue *suffix) {
    if (!xr_xir_value_argument(destination, NULL, XR_XIR_STRING) ||
        !xr_xir_value_argument(suffix, NULL, XR_XIR_STRING)) return XR_XIR_VALUE_BAD_ARGUMENT;
    XirString *left = string_pointer(destination), *right = string_pointer(suffix);
    if (!right->length) return XR_XIR_VALUE_OK;
    if (right->length >= SIZE_MAX - left->length) return XR_XIR_VALUE_LIMIT;
    size_t length = left->length + right->length, runes = left->runes + right->runes;
    if (atomic_load_explicit(&left->object.references, memory_order_acquire) == 1) {
        if (length + 1 > left->capacity) {
            XrXirValueStatus status = string_resize(left, string_capacity(length + 1));
            if (status != XR_XIR_VALUE_OK) return status;
        }
        memmove(left->bytes + left->length, right->bytes, right->length);
        left->length = length;
        left->runes = runes;
        left->bytes[length] = 0;
        return XR_XIR_VALUE_OK;
    }
    XirString *replacement = NULL;
    XrXirValueStatus status = string_allocate(left->object.domain, length, &replacement);
    if (status != XR_XIR_VALUE_OK) return status;
    memcpy(replacement->bytes, left->bytes, left->length);
    memcpy(replacement->bytes + left->length, right->bytes, right->length);
    replacement->runes = runes;
    xr_xir_value_drop(destination);
    *destination = string_value(replacement);
    return XR_XIR_VALUE_OK;
}
XR_FUNC bool xr_xir_string_view(const XrXirValue *value, const char **bytes, size_t *length) {
    if (!bytes || !length || !xr_xir_value_argument(value, NULL, XR_XIR_STRING)) return false;
    XirString *string = string_pointer(value);
    *bytes = string->bytes;
    *length = string->length;
    return true;
}
XR_FUNC XrXirValueStatus xr_xir_string_length(const XrXirValue *value, int64_t *length) {
    size_t count;
    if (!length || !xr_xir_string_runes(value, &count)) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (count > INT64_MAX) return XR_XIR_VALUE_LIMIT;
    *length = (int64_t) count;
    return XR_XIR_VALUE_OK;
}
XR_FUNC bool xr_xir_string_equal(const XrXirValue *left, const XrXirValue *right, bool *equal) {
    if (!equal || !xr_xir_value_argument(left, NULL, XR_XIR_STRING) ||
        !xr_xir_value_argument(right, NULL, XR_XIR_STRING)) return false;
    const XirString *a = string_pointer(left), *b = string_pointer(right);
    *equal = a == b || (a->length == b->length && (!a->length || !memcmp(a->bytes, b->bytes, a->length)));
    return true;
}
XR_FUNC bool xr_xir_string_contains(const XrXirValue *value, const XrXirValue *pattern, bool *result) {
    if (!result || !xr_xir_value_argument(value, NULL, XR_XIR_STRING) ||
        !xr_xir_value_argument(pattern, NULL, XR_XIR_STRING)) return false;
    const XirString *text = string_pointer(value), *search = string_pointer(pattern);
    *result = xr_string_core_contains(text->bytes, text->length, search->bytes, search->length);
    return true;
}
XR_FUNC bool xr_xir_string_starts_with(const XrXirValue *value, const XrXirValue *pattern, bool *result) {
    if (!result || !xr_xir_value_argument(value, NULL, XR_XIR_STRING) ||
        !xr_xir_value_argument(pattern, NULL, XR_XIR_STRING)) return false;
    const XirString *text = string_pointer(value), *search = string_pointer(pattern);
    *result = xr_string_core_starts_with(text->bytes, text->length, search->bytes, search->length);
    return true;
}
XR_FUNC bool xr_xir_string_ends_with(const XrXirValue *value, const XrXirValue *pattern, bool *result) {
    if (!result || !xr_xir_value_argument(value, NULL, XR_XIR_STRING) ||
        !xr_xir_value_argument(pattern, NULL, XR_XIR_STRING)) return false;
    const XirString *text = string_pointer(value), *search = string_pointer(pattern);
    *result = xr_string_core_ends_with(text->bytes, text->length, search->bytes, search->length);
    return true;
}
static XrXirValueStatus string_search_arguments(const XrXirValue *value,
    const XrXirValue *pattern, int64_t *result) {
    if (!result || !xr_xir_value_argument(value, NULL, XR_XIR_STRING) ||
        !xr_xir_value_argument(pattern, NULL, XR_XIR_STRING)) return XR_XIR_VALUE_BAD_ARGUMENT;
    const XirString *text = string_pointer(value), *search = string_pointer(pattern);
    if (text->runes > INT64_MAX || text->length > PTRDIFF_MAX || search->length > PTRDIFF_MAX)
        return XR_XIR_VALUE_LIMIT;
    return XR_XIR_VALUE_OK;
}
XR_FUNC XrXirValueStatus xr_xir_string_index_of(const XrXirValue *value,
    const XrXirValue *pattern, int64_t start, int64_t *result) {
    XrXirValueStatus status = string_search_arguments(value, pattern, result);
    if (status != XR_XIR_VALUE_OK) return status;
    const XirString *text = string_pointer(value), *search = string_pointer(pattern);
    if (start < 0 || (uint64_t) start > text->runes) return XR_XIR_VALUE_BOUNDS;
    if (!search->length) { *result = start; return XR_XIR_VALUE_OK; }
    if ((uint64_t) start == text->runes) { *result = -1; return XR_XIR_VALUE_OK; }
    size_t offset = (size_t) start;
    bool ascii = text->length == text->runes;
    if (!ascii && !xr_string_core_utf8_rune_at(text->bytes, text->length, (size_t) start, NULL, &offset))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    ptrdiff_t found = xr_string_core_index_of(text->bytes + offset, text->length - offset,
        search->bytes, search->length);
    *result = found < 0 ? -1 : start + (int64_t) (ascii ? (size_t) found :
        xr_string_core_utf8_rune_count(text->bytes + offset, (size_t) found));
    return XR_XIR_VALUE_OK;
}
XR_FUNC XrXirValueStatus xr_xir_string_last_index_of(const XrXirValue *value,
    const XrXirValue *pattern, int64_t *result) {
    XrXirValueStatus status = string_search_arguments(value, pattern, result);
    if (status != XR_XIR_VALUE_OK) return status;
    const XirString *text = string_pointer(value), *search = string_pointer(pattern);
    if (!search->length) { *result = (int64_t) text->runes; return XR_XIR_VALUE_OK; }
    ptrdiff_t found = xr_string_core_last_index_of(text->bytes, text->length, search->bytes, search->length);
    *result = found < 0 ? -1 : (text->length == text->runes ? (int64_t) found :
        (int64_t) xr_string_core_utf8_rune_count(text->bytes, (size_t) found));
    return XR_XIR_VALUE_OK;
}
XR_FUNC bool xr_xir_string_runes(const XrXirValue *value, size_t *count) {
    if (!count || !xr_xir_value_argument(value, NULL, XR_XIR_STRING)) return false;
    *count = string_pointer(value)->runes;
    return true;
}

XR_FUNC void xr_xir_owned_slot_clear(void *frame, uint32_t offset) {
    XrXirValue old = {XR_XIR_STRING, 0, 0};
    memcpy(&old.payload, (char *) frame + offset, sizeof(old.payload));
    if (old.payload) {
        old.type = (uint32_t) object_pointer(&old)->type;
        xr_xir_value_drop(&old);
    }
    memset((char *) frame + offset, 0, sizeof(old.payload));
}
XR_FUNC XrXirValueStatus xr_xir_owned_slot_copy(void *frame, uint32_t offset, const XrXirTypeArena *arena, XrXirType type, int64_t payload) {
    if (!xr_xir_type_is_owned(xr_xir_type_arena_types(arena), type)) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValue borrowed = {(uint32_t) type, 0, payload}, owned = {0};
    if (!xr_xir_value_argument(&borrowed, arena, type)) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValueStatus status = xr_xir_value_copy(&borrowed, &owned);
    if (status != XR_XIR_VALUE_OK) return status;
    xr_xir_owned_slot_clear(frame, offset);
    memcpy((char *) frame + offset, &owned.payload, sizeof(owned.payload));
    return XR_XIR_VALUE_OK;
}
XR_FUNC XrXirValueStatus xr_xir_string_slot_concat(void *frame, uint32_t offset, int64_t left, int64_t right) {
    XrXirValue borrowed = {XR_XIR_STRING, 0, left}, suffix = {XR_XIR_STRING, 0, right}, owned = {0};
    XrXirValueStatus status = xr_xir_value_copy(&borrowed, &owned);
    if (status == XR_XIR_VALUE_OK) status = xr_xir_string_append(&owned, &suffix);
    if (status != XR_XIR_VALUE_OK) {
        xr_xir_value_drop(&owned);
        return status;
    }
    xr_xir_owned_slot_clear(frame, offset);
    memcpy((char *) frame + offset, &owned.payload, sizeof(owned.payload));
    return XR_XIR_VALUE_OK;
}

XR_FUNC XrXirValueStatus xr_xir_atomic_i64_new(XrXirDomain *domain, int64_t initial, XrXirValue *output) {
    if (!domain || !unit_value(output)) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!xr_xir_reference_retain(&domain->references)) return XR_XIR_VALUE_REFCOUNT_LIMIT;
    XrXirValueStatus status = XR_XIR_VALUE_OK;
    XirAtomicI64 *cell = xr_xir_domain_allocate(domain, sizeof(*cell), &status);
    if (!cell) { xr_xir_domain_drop(domain); return status; }
    atomic_init(&cell->object.references, 1);
    cell->object.domain = domain;
    cell->object.type = XR_XIR_ATOMIC_I64;
    cell->object.arena = NULL; cell->object.kind = 0;
    atomic_init(&cell->value, initial);
    output->type = XR_XIR_ATOMIC_I64;
    memcpy(&output->payload, &cell, sizeof(cell));
    return XR_XIR_VALUE_OK;
}
XR_FUNC bool xr_xir_atomic_i64_load(const XrXirValue *value, int64_t *output) {
    if (!output || !xr_xir_value_argument(value, NULL, XR_XIR_ATOMIC_I64)) return false;
    XirAtomicI64 *cell = (XirAtomicI64 *) object_pointer(value);
    *output = atomic_load_explicit(&cell->value, memory_order_seq_cst);
    return true;
}
XR_FUNC bool xr_xir_atomic_i64_fetch_add(const XrXirValue *value, int64_t delta, int64_t *previous) {
    if (!previous || !xr_xir_value_argument(value, NULL, XR_XIR_ATOMIC_I64)) return false;
    XirAtomicI64 *cell = (XirAtomicI64 *) object_pointer(value);
    *previous = atomic_fetch_add_explicit(&cell->value, delta, memory_order_seq_cst);
    return true;
}
XR_FUNC void xr_xir_owned_slot_move(void *frame, uint32_t offset, XrXirValue *owned) {
    XR_CHECK(owned && owned_carrier_type((XrXirType) owned->type) &&
             xr_xir_value_valid(owned), "moving non-owned frame value");
    xr_xir_owned_slot_clear(frame, offset);
    memcpy((char *) frame + offset, &owned->payload, sizeof(owned->payload));
    *owned = (XrXirValue) {0};
}


static XirObject *constructed_allocate(XrXirDomain *domain, XrXirTypeArena *arena,
    XrXirType type, size_t bytes, XrXirValueStatus *status) {
    if (!xr_xir_domain_retain(domain)) { *status = XR_XIR_VALUE_REFCOUNT_LIMIT; return NULL; }
    if (!xr_xir_type_arena_retain(arena)) {
        xr_xir_domain_drop(domain); *status = XR_XIR_VALUE_REFCOUNT_LIMIT; return NULL;
    }
    XirObject *object = xr_xir_domain_allocate(domain, bytes, status);
    if (!object) {
        xr_xir_type_arena_drop(arena); xr_xir_domain_drop(domain); return NULL;
    }
    const XrXirTypeNode *node = xr_xir_type_node(xr_xir_type_arena_types(arena), type);
    XR_CHECK(node, "constructed allocation requires an exact runtime descriptor");
    atomic_init(&object->references, 1);
    object->domain = domain; object->arena = arena; object->type = type; object->kind = node->kind;
    object->release_next = NULL;
    return object;
}
static void constructed_discard(XirObject *object, size_t bytes) {
    XrXirDomain *domain = object->domain;
    XrXirTypeArena *arena = object->arena;
    xr_xir_domain_deallocate(domain, object, bytes);
    xr_xir_type_arena_drop(arena); xr_xir_domain_drop(domain);
}
static bool admission_owner(const XrXirValueAdmission *admission,
                             const XrXirTypeArena *arena, XrXirDomain *domain) {
    return admission && arena && domain && admission->arena == arena && admission->domain == domain;
}
#include "xxir_value_admission.inc.c"
XR_FUNC XrXirValueStatus xr_xir_function_new(XrXirDomain *domain, XrXirTypeArena *arena,
    XrXirType type, const XrXirFunctionBinding *binding, XrXirValueAdmission *admission,
    XrXirValue *output) {
    if (!admission_owner(admission, arena, domain) || !unit_value(output) ||
        !xr_xir_type_is_callable(xr_xir_type_arena_types(arena), type) ||
        !binding || !binding->owner || !binding->release || binding->capture_count > 65536 ||
        (binding->capture_count && !binding->captures)) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!admission->work) return XR_XIR_VALUE_LIMIT;
    --admission->work;
    if (!admission->function) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValueStatus admitted = admission->function(admission->context, binding, type, &admission->work);
    if (admitted != XR_XIR_VALUE_OK) return admitted;
    for (uint32_t i = 0; i < binding->capture_count; ++i) {
        XrXirValueStatus status = xr_xir_value_admit(&binding->captures[i],
            (XrXirType) binding->captures[i].type, admission);
        if (status != XR_XIR_VALUE_OK) return status;
    }
    XrXirValueStatus status = XR_XIR_VALUE_OK;
    size_t bytes = sizeof(XirFunction) + (size_t) binding->capture_count * sizeof(XrXirValue);
    XirFunction *function = (XirFunction *) constructed_allocate(domain, arena, type, bytes, &status);
    if (!function) return status;
    function->binding = *binding;
    XrXirValue *captures = (XrXirValue *) (function + 1);
    function->binding.captures = binding->capture_count ? captures : NULL;
    memset(captures, 0, (size_t) binding->capture_count * sizeof(*captures));
    for (uint32_t i = 0; i < binding->capture_count; ++i) {
        status = xr_xir_value_copy(&binding->captures[i], &captures[i]);
        if (status != XR_XIR_VALUE_OK) {
            while (i) xr_xir_value_drop(&captures[--i]);
            constructed_discard(&function->object, bytes); return status;
        }
    }
    output->type = (uint32_t) type;
    memcpy(&output->payload, &function, sizeof(function));
    return XR_XIR_VALUE_OK;
}
XR_FUNC const XrXirFunctionBinding *xr_xir_function_binding(const XrXirValue *value) {
    if (!xr_xir_value_valid(value) || !owned_carrier_type((XrXirType) value->type)) return NULL;
    XirObject *object = object_pointer(value);
    return object->kind == XR_XIR_TYPE_CALLABLE ? &((XirFunction *) object)->binding : NULL;
}
XR_FUNC XrXirValueStatus xr_xir_cell_new(XrXirDomain *domain, XrXirTypeArena *arena,
    XrXirType type, const XrXirValue *initial, XrXirValueAdmission *admission,
    XrXirValue *output) {
    if (!admission_owner(admission, arena, domain) || !unit_value(output) ||
        !xr_xir_type_is_cell(xr_xir_type_arena_types(arena), type)) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirType element = xr_xir_cell_element(xr_xir_type_arena_types(arena), type);
    XrXirValueStatus status = xr_xir_value_admit(initial, element, admission);
    if (status != XR_XIR_VALUE_OK) return status;
    XirCell *cell = (XirCell *) constructed_allocate(domain, arena, type, sizeof(*cell), &status);
    if (!cell) return status;
    cell->value = (XrXirValue) {0};
    status = xr_xir_value_copy(initial, &cell->value);
    if (status != XR_XIR_VALUE_OK) { constructed_discard(&cell->object, sizeof(*cell)); return status; }
    output->type = (uint32_t) type;
    memcpy(&output->payload, &cell, sizeof(cell));
    return XR_XIR_VALUE_OK;
}
XR_FUNC bool xr_xir_cell_in_domain(const XrXirValue *cell, XrXirDomain *domain) {
    if (!xr_xir_value_valid(cell) || !owned_carrier_type((XrXirType) cell->type)) return false;
    XirObject *object = object_pointer(cell);
    return object->kind == XR_XIR_TYPE_CELL && object->domain == domain;
}
XR_FUNC XrXirValueStatus xr_xir_cell_read(const XrXirValue *cell, XrXirValue *output) {
    if (!xr_xir_value_valid(cell) || !owned_carrier_type((XrXirType) cell->type) ||
        object_pointer(cell)->kind != XR_XIR_TYPE_CELL) return XR_XIR_VALUE_BAD_ARGUMENT;
    return xr_xir_value_copy(&((XirCell *) object_pointer(cell))->value, output);
}
XR_FUNC XrXirValueStatus xr_xir_cell_write(const XrXirValue *cell, const XrXirValue *value,
                                         XrXirValueAdmission *admission) {
    if (!xr_xir_value_valid(cell) || !owned_carrier_type((XrXirType) cell->type) ||
        object_pointer(cell)->kind != XR_XIR_TYPE_CELL) return XR_XIR_VALUE_BAD_ARGUMENT;
    XirCell *target = (XirCell *) object_pointer(cell);
    if (!admission_owner(admission, target->object.arena, target->object.domain)) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirType element = xr_xir_cell_element(xr_xir_type_arena_types(target->object.arena), target->object.type);
    XrXirValueStatus status = xr_xir_value_admit(value, element, admission);
    if (status != XR_XIR_VALUE_OK) return status;
    XrXirValue replacement = {0};
    status = xr_xir_value_copy(value, &replacement);
    if (status != XR_XIR_VALUE_OK) return status;
    XrXirValue previous = target->value; target->value = replacement;
    xr_xir_value_drop(&previous); return XR_XIR_VALUE_OK;
}

#include "xxir_storage_pack.inc.c"
#include "xxir_storage_unpack.inc.c"
#include "xxir_array_prepare.inc.c"
static XrXirValue array_value(XirArray *array) {
    XrXirValue value = {(uint32_t) array->object.type, 0, 0};
    memcpy(&value.payload, &array, sizeof(array));
    return value;
}

static bool array_capacity(size_t required, uint32_t stride, size_t *output) {
    size_t maximum = stride ? SIZE_MAX / stride : SIZE_MAX;
    if (maximum > (uint64_t) INT64_MAX) maximum = (size_t) INT64_MAX;
    if (required > maximum) return false;
    size_t capacity = required ? 4 : 0;
    while (capacity < required) {
        if (capacity > maximum / 2) { capacity = required; break; }
        capacity *= 2;
    }
    *output = capacity;
    return true;
}

static XrXirValueStatus array_allocate(XrXirType type, size_t capacity,
    XrXirValueAdmission *admission, XirArray **output) {
    XrXirType element = XR_XIR_UNIT; uint32_t stride = 0;
    if (!admission || !admission->domain ||
        !array_layout(admission->arena, type, &element, &stride)) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (capacity > (uint64_t) INT64_MAX || (stride && capacity > SIZE_MAX / stride)) return XR_XIR_VALUE_LIMIT;
    uint64_t header = array_header_bytes(admission->arena, element);
    if (header > SIZE_MAX) return XR_XIR_VALUE_LIMIT;
    XrXirValueStatus status = XR_XIR_VALUE_OK;
    XirArray *array = (XirArray *) constructed_allocate(admission->domain,
        (XrXirTypeArena *) admission->arena, type, (size_t)header, &status);
    if (!array) return status;
    array->data = NULL; array->length = 0; array->capacity = capacity;
    array->element = element; array->stride = stride;
    if (capacity && stride) {
        array->data = xr_xir_domain_allocate(admission->domain, capacity * stride, &status);
        if (!array->data) { constructed_discard(&array->object, (size_t)header); return status; }
    }
    *output = array;
    return XR_XIR_VALUE_OK;
}

XR_FUNC XrXirValueStatus xr_xir_array_new(XrXirType type, const XrXirValue *values,
    size_t count, XrXirValueAdmission *admission, XrXirValue *output) {
    XrXirType element = XR_XIR_UNIT; uint32_t stride = 0;
    if (!unit_value(output) || !admission || !admission->domain || (count && !values) ||
        !array_layout(admission->arena, type, &element, &stride)) return XR_XIR_VALUE_BAD_ARGUMENT;
    size_t capacity = 0;
    if (!array_capacity(count, stride, &capacity) || !admission->work || count >= admission->work)
        return XR_XIR_VALUE_LIMIT;
    --admission->work;
    for (size_t i = 0; i < count; ++i) {
        XrXirValueStatus status = xr_xir_value_admit(&values[i], element, admission);
        if (status != XR_XIR_VALUE_OK) return status;
    }
    XirArray *array = NULL;
    XrXirValueStatus status = array_allocate(type, capacity, admission, &array);
    if (status != XR_XIR_VALUE_OK) return status;
    XrXirValue result = array_value(array);
    StoragePack pack = {0};
    status = storage_pack_begin(admission, element, &pack);
    if (status != XR_XIR_VALUE_OK) { xr_xir_value_drop(&result); return status; }
    for (size_t i = 0; i < count; ++i) {
        status = storage_pack_value(&pack, &values[i], array_slot(array, i));
        if (status != XR_XIR_VALUE_OK) {
            storage_pack_end(&pack); xr_xir_value_drop(&result); return status;
        }
        ++array->length;
    }
    storage_pack_end(&pack);
    *output = result;
    return XR_XIR_VALUE_OK;
}

XR_FUNC XrXirValueStatus xr_xir_array_len(const XrXirValue *array,
    XrXirValueAdmission *admission, int64_t *output) {
    if (!array || !admission || !output) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!admission->work) return XR_XIR_VALUE_LIMIT;
    --admission->work;
    if (!xr_xir_value_argument(array, admission->arena, (XrXirType) array->type) ||
        !xr_xir_type_is_array(xr_xir_type_arena_types(admission->arena), (XrXirType) array->type))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    *output = (int64_t) ((XirArray *) object_pointer(array))->length;
    return XR_XIR_VALUE_OK;
}

XR_FUNC XrXirValueStatus xr_xir_array_get(const XrXirValue *value, int64_t index,
    XrXirValueAdmission *admission, XrXirValue *output, XrXirFaultDetail *fault) {
    if (fault) *fault = (XrXirFaultDetail) {0};
    if (!value || !fault || !unit_value(output) || !admission ||
        !xr_xir_type_is_array(xr_xir_type_arena_types(admission->arena), (XrXirType) value->type))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValueStatus status = xr_xir_value_admit(value, (XrXirType) value->type, admission);
    if (status != XR_XIR_VALUE_OK) return status;
    XirArray *array = (XirArray *) object_pointer(value);
    if (index < 0 || (uint64_t) index >= array->length) {
        *fault = (XrXirFaultDetail) {430, 0, index, (int64_t) array->length};
        return XR_XIR_VALUE_BOUNDS;
    }
    StorageSpan span = {array->element, array_slot(array, (size_t)index)};
    return storage_unpack(span, admission, output);
}

static XrXirValueStatus array_grow(XirArray *array, size_t capacity) {
    XrXirDomain *domain = array->object.domain;
    XrXirValueStatus status = XR_XIR_VALUE_OK;
    size_t bytes = capacity * array->stride;
    if (!array->stride) { array->capacity = capacity; return XR_XIR_VALUE_OK; }
    if (!array->data) {
        unsigned char *data = xr_xir_domain_allocate(domain, bytes, &status);
        if (!data) return status;
        array->data = data; array->capacity = capacity;
        return XR_XIR_VALUE_OK;
    }
    domain_lock(domain);
    size_t extra = bytes - array->capacity * array->stride;
    if (extra > domain->limit - domain->stats.live_bytes || domain->stats.reallocations == UINT64_MAX) {
        domain_unlock(domain); return XR_XIR_VALUE_LIMIT;
    }
    unsigned char *replacement = xr_realloc(array->data, bytes);
    if (!replacement) { domain_unlock(domain); return XR_XIR_VALUE_OOM; }
    array->data = replacement; array->capacity = capacity;
    domain->stats.live_bytes += extra;
    if (domain->stats.live_bytes > domain->stats.peak_bytes) domain->stats.peak_bytes = domain->stats.live_bytes;
    ++domain->stats.reallocations;
    domain_unlock(domain);
    return XR_XIR_VALUE_OK;
}

static XrXirValueStatus array_mutate(const XrXirValuePlace *place, int64_t index,
    const XrXirValue *element, bool append, XrXirValueAdmission *admission, XrXirFaultDetail *fault) {
    if (fault) *fault = (XrXirFaultDetail) {0};
    if (!place || !place->payload || !admission || !admission->domain || (!append && !fault) ||
        !xr_xir_type_is_array(xr_xir_type_arena_types(admission->arena), place->type)) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValue current = {(uint32_t) place->type, 0, 0};
    memcpy(&current.payload, place->payload, sizeof(current.payload));
    XrXirValueStatus status = xr_xir_value_admit(&current, place->type, admission);
    if (status != XR_XIR_VALUE_OK) return status;
    XirArray *array = (XirArray *) object_pointer(&current);
    status = xr_xir_value_admit(element, array->element, admission);
    if (status != XR_XIR_VALUE_OK) return status;
    if (!append && (index < 0 || (uint64_t) index >= array->length)) {
        *fault = (XrXirFaultDetail) {430, 0, index, (int64_t) array->length};
        return XR_XIR_VALUE_BOUNDS;
    }
    if (append && array->length == (uint64_t) INT64_MAX) return XR_XIR_VALUE_LIMIT;
    size_t length = array->length + (append ? 1u : 0u), capacity = 0;
    if (!array_capacity(length, array->stride, &capacity)) return XR_XIR_VALUE_LIMIT;
    ArrayPrepared prepared = {0};
    status = array_prepared_begin(&prepared, element, array->stride, admission);
    if (status != XR_XIR_VALUE_OK) return status;
    bool unique = atomic_load_explicit(&array->object.references, memory_order_acquire) == 1;
    bool grow = length > array->capacity;
    if (!unique || grow) {
        if (admission->work < array->length) {
            array_prepared_end(&prepared); return XR_XIR_VALUE_LIMIT;
        }
        admission->work -= array->length;
    }
    if (unique && (!grow || array->object.domain == admission->domain)) {
        if (grow) status = array_grow(array, capacity);
        if (status != XR_XIR_VALUE_OK) { array_prepared_end(&prepared); return status; }
        size_t offset = append ? array->length : (size_t) index;
        XirObject *pending = NULL;
        if (!append) {
            StorageCursor cursor = {0};
            XR_CHECK(storage_cursor_init(admission->arena, (StorageSpan){array->element, array_slot(array, offset)},
                (StorageFrame *)prepared.pack.frames, prepared.pack.capacity, true, &cursor) == XR_XIR_VALUE_OK,
                "array overwrite retains reserved ownership traversal");
            storage_queue_release(&cursor, UINT64_MAX, &pending);
        }
        array_prepared_transfer(&prepared, array, offset); array->length = length;
        release_pending(pending); array_prepared_end(&prepared);
        return XR_XIR_VALUE_OK;
    }
    XirArray *replacement = NULL;
    status = array_allocate(place->type, capacity, admission, &replacement);
    if (status != XR_XIR_VALUE_OK) { array_prepared_end(&prepared); return status; }
    XrXirValue result = array_value(replacement);
    if (unique) {
        if (array->length && array->stride) memcpy(replacement->data, array->data, array->length * array->stride);
        replacement->length = array->length;
    } else {
        for (size_t i = 0; i < array->length; ++i) {
            if (!append && i == (size_t) index) array_prepared_transfer(&prepared, replacement, i);
            else {
                status = storage_pack_copy(&prepared.pack, array_slot(array, i), array_slot(replacement, i));
                if (status != XR_XIR_VALUE_OK) {
                    xr_xir_value_drop(&result); array_prepared_end(&prepared); return status;
                }
            }
            ++replacement->length;
        }
    }
    if (append) {
        array_prepared_transfer(&prepared, replacement, replacement->length); ++replacement->length;
    }
    memcpy(place->payload, &result.payload, sizeof(result.payload));
    if (unique) array->length = 0;
    xr_xir_value_drop(&current);
    array_prepared_end(&prepared);
    return XR_XIR_VALUE_OK;
}

XR_FUNC XrXirValueStatus xr_xir_array_set(const XrXirValuePlace *place, int64_t index,
    const XrXirValue *element, XrXirValueAdmission *admission, XrXirFaultDetail *fault) {
    return array_mutate(place, index, element, false, admission, fault);
}
XR_FUNC XrXirValueStatus xr_xir_array_push(const XrXirValuePlace *place,
    const XrXirValue *element, XrXirValueAdmission *admission) {
    return array_mutate(place, 0, element, true, admission, NULL);
}
XR_FUNC XrXirValueStatus xr_xir_cell_value_place(const XrXirValue *cell,
    XrXirValueAdmission *admission, XrXirValuePlace *output) {
    if (!output || !cell || !admission || !admission->arena || !admission->domain)
        return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!admission->work) return XR_XIR_VALUE_LIMIT;
    --admission->work;
    if (!xr_xir_value_argument(cell, admission->arena, (XrXirType) cell->type) ||
        !xr_xir_cell_in_domain(cell, admission->domain)) return XR_XIR_VALUE_BAD_ARGUMENT;
    XirCell *holder = (XirCell *) object_pointer(cell);
    XrXirType type = (XrXirType) holder->value.type;
    const XrXirTypes *types = xr_xir_type_arena_types(admission->arena);
    if (!xr_xir_type_is_array(types, type) && !xr_xir_type_is_nominal(types, type)) return XR_XIR_VALUE_BAD_ARGUMENT;
    *output = (XrXirValuePlace) {type, &holder->value.payload};
    return XR_XIR_VALUE_OK;
}

#include "xxir_struct_value.inc.c"
#include "xxir_value_path.inc.c"
#include "xxir_enum_value.inc.c"
#include "xxir_error_value.inc.c"
#include "xxir_panic_value.inc.c"
