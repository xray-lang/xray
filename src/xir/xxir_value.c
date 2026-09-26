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
#include "../base/xmalloc.h"
#include "../base/xchecks.h"
#include "../shared/xr_utf8_core.h"
#include <stdatomic.h>
#include <string.h>

struct XrXirDomain {
    _Atomic(uint32_t) references;
    atomic_bool locked;
    uint64_t limit;
    XrXirDomainStats stats;
};
typedef struct XirString {
    _Atomic(uint32_t) references;
    XrXirDomain *domain;
    char *bytes;
    size_t length, runes, capacity;
} XirString;

_Static_assert(sizeof(void *) == sizeof(int64_t), "XIR pointer payload width");

static void domain_lock(XrXirDomain *domain) {
    while (atomic_exchange_explicit(&domain->locked, true, memory_order_acquire)) { }
}
static void domain_unlock(XrXirDomain *domain) {
    atomic_store_explicit(&domain->locked, false, memory_order_release);
}
static bool reference_retain(_Atomic(uint32_t) *references) {
    uint32_t count = atomic_load_explicit(references, memory_order_relaxed);
    for (;;) {
        if (!count || count == UINT32_MAX) return false;
        if (atomic_compare_exchange_weak_explicit(references, &count, count + 1,
                memory_order_relaxed, memory_order_relaxed)) return true;
    }
}
static bool reference_release(_Atomic(uint32_t) *references) {
    uint32_t count = atomic_load_explicit(references, memory_order_relaxed);
    for (;;) {
        XR_CHECK(count, "reference count underflow");
        if (atomic_compare_exchange_weak_explicit(references, &count, count - 1,
                memory_order_acq_rel, memory_order_relaxed)) return count == 1;
    }
}
static bool unit_value(const XrXirValue *value) {
    return value && !value->type && !value->reserved && !value->payload;
}
static XirString *string_pointer(const XrXirValue *value) {
    XirString *string;
    memcpy(&string, &value->payload, sizeof(string));
    return string;
}
static XrXirValue string_value(XirString *string) {
    XrXirValue value = {XR_XIR_STRING, 0, 0};
    memcpy(&value.payload, &string, sizeof(string));
    return value;
}
bool xr_xir_value_argument(const XrXirValue *value, XrXirType type) {
    if (!value || value->reserved || value->type != (uint32_t) type) return false;
    return type == XR_XIR_I64 ||
        (type == XR_XIR_BOOL && (value->payload == 0 || value->payload == 1)) ||
        (type == XR_XIR_STRING && string_pointer(value) != NULL);
}
XrXirValueStatus xr_xir_domain_new(uint64_t limit, XrXirDomain **output) {
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
void xr_xir_domain_drop(XrXirDomain *domain) {
    if (domain && reference_release(&domain->references)) {
        XR_CHECK(domain->stats.live_bytes == sizeof(*domain) &&
                 domain->stats.allocations == domain->stats.frees + 1,
                 "domain still owns storage");
        xr_free(domain);
    }
}
XrXirDomainStats xr_xir_domain_stats(XrXirDomain *domain) {
    if (!domain) return (XrXirDomainStats) {0};
    domain_lock(domain);
    XrXirDomainStats stats = domain->stats;
    domain_unlock(domain);
    return stats;
}
static void *domain_allocate(XrXirDomain *domain, size_t bytes, XrXirValueStatus *status) {
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
static void domain_deallocate(XrXirDomain *domain, void *memory, size_t bytes) {
    domain_lock(domain);
    XR_CHECK(memory && domain->stats.live_bytes >= bytes &&
             domain->stats.allocations > domain->stats.frees, "invalid string storage release");
    xr_free(memory);
    domain->stats.live_bytes -= bytes;
    ++domain->stats.frees;
    domain_unlock(domain);
}
static XrXirValueStatus string_resize(XirString *string, size_t capacity) {
    XrXirDomain *domain = string->domain;
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
    if (!reference_retain(&domain->references)) return XR_XIR_VALUE_REFCOUNT_LIMIT;
    XrXirValueStatus status = XR_XIR_VALUE_OK;
    XirString *string = domain_allocate(domain, sizeof(*string), &status);
    if (string) {
        string->capacity = string_capacity(length + 1);
        string->bytes = domain_allocate(domain, string->capacity, &status);
        if (string->bytes) {
            atomic_init(&string->references, 1);
            string->domain = domain;
            string->length = length;
            string->runes = 0;
            string->bytes[length] = 0;
            *output = string;
            return XR_XIR_VALUE_OK;
        }
        domain_deallocate(domain, string, sizeof(*string));
    }
    xr_xir_domain_drop(domain);
    return status;
}
XrXirValueStatus xr_xir_string_new(XrXirDomain *domain, const char *bytes,
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
XrXirValueStatus xr_xir_value_copy(const XrXirValue *source, XrXirValue *output) {
    if (!unit_value(output) || !source ||
        (!unit_value(source) && !xr_xir_value_argument(source, (XrXirType) source->type)))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    if (source->type == XR_XIR_STRING && !reference_retain(&string_pointer(source)->references))
        return XR_XIR_VALUE_REFCOUNT_LIMIT;
    *output = *source;
    return XR_XIR_VALUE_OK;
}
void xr_xir_value_drop(XrXirValue *value) {
    if (!value) return;
    XR_CHECK(unit_value(value) || xr_xir_value_argument(value, (XrXirType) value->type),
             "invalid owned value release");
    if (value->type == XR_XIR_STRING) {
        XirString *string = string_pointer(value);
        if (reference_release(&string->references)) {
            XrXirDomain *domain = string->domain;
            domain_deallocate(domain, string->bytes, string->capacity);
            domain_deallocate(domain, string, sizeof(*string));
            xr_xir_domain_drop(domain);
        }
    }
    *value = (XrXirValue) {0};
}
XrXirValueStatus xr_xir_string_append(XrXirValue *destination, const XrXirValue *suffix) {
    if (!xr_xir_value_argument(destination, XR_XIR_STRING) ||
        !xr_xir_value_argument(suffix, XR_XIR_STRING)) return XR_XIR_VALUE_BAD_ARGUMENT;
    XirString *left = string_pointer(destination), *right = string_pointer(suffix);
    if (!right->length) return XR_XIR_VALUE_OK;
    if (right->length >= SIZE_MAX - left->length) return XR_XIR_VALUE_LIMIT;
    size_t length = left->length + right->length, runes = left->runes + right->runes;
    if (atomic_load_explicit(&left->references, memory_order_acquire) == 1) {
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
    XrXirValueStatus status = string_allocate(left->domain, length, &replacement);
    if (status != XR_XIR_VALUE_OK) return status;
    memcpy(replacement->bytes, left->bytes, left->length);
    memcpy(replacement->bytes + left->length, right->bytes, right->length);
    replacement->runes = runes;
    xr_xir_value_drop(destination);
    *destination = string_value(replacement);
    return XR_XIR_VALUE_OK;
}
bool xr_xir_string_view(const XrXirValue *value, const char **bytes, size_t *length) {
    if (!bytes || !length || !xr_xir_value_argument(value, XR_XIR_STRING)) return false;
    XirString *string = string_pointer(value);
    *bytes = string->bytes;
    *length = string->length;
    return true;
}
bool xr_xir_string_runes(const XrXirValue *value, size_t *count) {
    if (!count || !xr_xir_value_argument(value, XR_XIR_STRING)) return false;
    *count = string_pointer(value)->runes;
    return true;
}

void xr_xir_string_slot_clear(void *frame, uint32_t offset) {
    XrXirValue old = {XR_XIR_STRING, 0, 0};
    memcpy(&old.payload, (char *) frame + offset, sizeof(old.payload));
    if (old.payload) xr_xir_value_drop(&old);
    memset((char *) frame + offset, 0, sizeof(old.payload));
}
XrXirValueStatus xr_xir_string_slot_copy(void *frame, uint32_t offset, int64_t payload) {
    XrXirValue borrowed = {XR_XIR_STRING, 0, payload}, owned = {0};
    XrXirValueStatus status = xr_xir_value_copy(&borrowed, &owned);
    if (status != XR_XIR_VALUE_OK) return status;
    xr_xir_string_slot_clear(frame, offset);
    memcpy((char *) frame + offset, &owned.payload, sizeof(owned.payload));
    return XR_XIR_VALUE_OK;
}
XrXirValueStatus xr_xir_string_slot_concat(void *frame, uint32_t offset, int64_t left, int64_t right) {
    XrXirValue borrowed = {XR_XIR_STRING, 0, left}, suffix = {XR_XIR_STRING, 0, right}, owned = {0};
    XrXirValueStatus status = xr_xir_value_copy(&borrowed, &owned);
    if (status == XR_XIR_VALUE_OK) status = xr_xir_string_append(&owned, &suffix);
    if (status != XR_XIR_VALUE_OK) {
        xr_xir_value_drop(&owned);
        return status;
    }
    xr_xir_string_slot_clear(frame, offset);
    memcpy((char *) frame + offset, &owned.payload, sizeof(owned.payload));
    return XR_XIR_VALUE_OK;
}
