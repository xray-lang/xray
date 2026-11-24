/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_string_backing_ops.c - Shared text algorithms with real slot owners
 *
 * KEY CONCEPT:
 *   Admitted sources need no UTF-8 revalidation. Complete derived bytes and
 *   cached scalar counts precede publication and release of the old owner.
 */
#include "xnative_string_backing_ops.h"
#include "xnative_array_backing.h"
#include "xnative_place.h"
#include "xray_native_values.h"
#include "base/xchecks.h"
#include "shared/xr_string_core.h"
#include "shared/xr_string_parse_core.h"
#include "shared/xr_text_kernel.h"
#include "shared/xr_float_to_decimal.h"
#include <limits.h>

XRAY_API bool xrt_native_string_try_parse_i64(const XrtStringBacking *string, int64_t *output) {
    XR_DCHECK(string && output, "integer conversion requires a live String and scalar destination");
    XrStringParseIntResult parsed = xr_string_parse_int64(string->bytes, string->length);
    if (parsed.failure != XR_NUMBER_PARSE_OK) return false;
    *output = parsed.value;
    return true;
}
XRAY_API bool xrt_native_string_try_parse_f64(const XrtStringBacking *string, double *output) {
    XR_DCHECK(string && output, "float conversion requires a live String and scalar destination");
    XrStringParseFloatResult parsed = xr_string_parse_float64(string->bytes, string->length);
    if (parsed.failure != XR_NUMBER_PARSE_OK) return false;
    *output = parsed.value;
    return true;
}

static XrtManagedStatus string_prepare(XrtManagedFrame *frame, size_t slot,
    const XrtBackingTypes *types, size_t length, size_t runes, XrtStringBacking **output) {
    if (!types || !types->string) return XRT_MANAGED_ARGUMENT;
    if (length > SIZE_MAX - sizeof(XrtStringBacking) - 1) return XRT_MANAGED_OVERFLOW;
    XrtStringBacking *string = NULL;
    XrtManagedStatus status = xrt_string_backing_prepare(frame, slot, types->string,
        sizeof(XrtStringBacking) + length + 1, &string);
    if (status == XRT_MANAGED_OK) {
        XR_CHECK(string->capacity == length + 1, "derived bytes use the actual pending extent");
        string->length = length;
        string->runes = runes;
        string->bytes[length] = 0;
        *output = string;
    }
    return status;
}
static XrtManagedStatus string_finish(XrtManagedFrame *frame, size_t slot) {
    XrtManagedStatus status = xrt_managed_slot_commit(frame, slot);
    if (status != XRT_MANAGED_OK)
        XR_CHECK(xrt_managed_slot_abort(frame, slot) == XRT_MANAGED_OK, "abort only this operation's prepared token");
    return status;
}
static XrtManagedStatus string_bytes_create(XrtManagedFrame *frame, size_t slot,
    const XrtBackingTypes *types, const char *bytes, size_t length, size_t runes) {
    XrtStringBacking *string = NULL;
    XrtManagedStatus status = string_prepare(frame, slot, types, length, runes, &string);
    if (status != XRT_MANAGED_OK) return status;
    if (length) memcpy(string->bytes, bytes, length);
    return string_finish(frame, slot);
}
static XrtManagedStatus search_extent(const XrtStringBacking *text, const XrtStringBacking *search) {
    if (!text || !search) return XRT_MANAGED_ARGUMENT;
    if (text->runes > INT64_MAX || text->length > PTRDIFF_MAX || search->length > PTRDIFF_MAX)
        return XRT_MANAGED_OVERFLOW;
    return XRT_MANAGED_OK;
}
static size_t scalar_offset(const XrtStringBacking *text, size_t index) {
    if (text->length == text->runes || index == text->runes) return index == text->runes ? text->length : index;
    size_t offset = 0;
    bool found = xr_string_core_utf8_rune_at(text->bytes, text->length, index, NULL, &offset);
    XR_CHECK(found, "an admitted scalar index has a byte boundary");
    return offset;
}
XR_FUNCDEF XrtManagedStatus xrt_string_backing_counts(const XrtStringBacking *text, size_t *bytes, size_t *runes) {
    if (!text || !bytes || !runes) return XRT_MANAGED_ARGUMENT;
    *bytes = text->length;
    *runes = text->runes;
    return XRT_MANAGED_OK;
}
XR_FUNCDEF XrtManagedStatus xrt_string_backing_compare(const XrtStringBacking *left, const XrtStringBacking *right, int *order) {
    if (!left || !right || !order) return XRT_MANAGED_ARGUMENT;
    *order = xr_text_compare((const uint8_t *)left->bytes, left->length, (const uint8_t *)right->bytes, right->length);
    return XRT_MANAGED_OK;
}
XR_FUNCDEF XrtManagedStatus xrt_string_backing_test(const XrtStringBacking *text, const XrtStringBacking *pattern,
    XrtStringTest test, bool *output) {
    if (!text || !pattern || !output) return XRT_MANAGED_ARGUMENT;
    bool result;
    switch (test) {
        case XRT_STRING_CONTAINS:
            if (text->length > PTRDIFF_MAX || pattern->length > PTRDIFF_MAX) return XRT_MANAGED_OVERFLOW;
            result = xr_string_core_contains(text->bytes, text->length, pattern->bytes, pattern->length); break;
        case XRT_STRING_STARTS_WITH:
            result = xr_string_core_starts_with(text->bytes, text->length, pattern->bytes, pattern->length); break;
        case XRT_STRING_ENDS_WITH:
            result = xr_string_core_ends_with(text->bytes, text->length, pattern->bytes, pattern->length); break;
        default: return XRT_MANAGED_ARGUMENT;
    }
    *output = result;
    return XRT_MANAGED_OK;
}
XR_FUNCDEF XrtManagedStatus xrt_string_backing_index_of(const XrtStringBacking *text, const XrtStringBacking *search,
    int64_t start, int64_t *output) {
    if (!output) return XRT_MANAGED_ARGUMENT;
    XrtManagedStatus status = search_extent(text, search);
    if (status != XRT_MANAGED_OK) return status;
    if (start < 0 || (uint64_t)start > text->runes) return XRT_MANAGED_BOUNDS;
    if (!search->length) { *output = start; return XRT_MANAGED_OK; }
    size_t offset = scalar_offset(text, (size_t)start);
    ptrdiff_t found = xr_string_core_index_of(text->bytes + offset, text->length - offset, search->bytes, search->length);
    *output = found < 0 ? -1 : start + (int64_t)xr_text_scalar_count((const uint8_t *)text->bytes + offset, (size_t)found);
    return XRT_MANAGED_OK;
}
XR_FUNCDEF XrtManagedStatus xrt_string_backing_last_index_of(const XrtStringBacking *text,
    const XrtStringBacking *search, int64_t *output) {
    if (!output) return XRT_MANAGED_ARGUMENT;
    XrtManagedStatus status = search_extent(text, search);
    if (status != XRT_MANAGED_OK) return status;
    ptrdiff_t found = xr_string_core_last_index_of(text->bytes, text->length, search->bytes, search->length);
    *output = found < 0 ? -1 : (int64_t)xr_text_scalar_count((const uint8_t *)text->bytes, (size_t)found);
    return XRT_MANAGED_OK;
}
XR_FUNCDEF XrtManagedStatus xrt_string_backing_rune_at(const XrtStringBacking *text, int64_t index, uint32_t *output) {
    if (!text || !output) return XRT_MANAGED_ARGUMENT;
    if (index < 0 || (uint64_t)index >= text->runes) return XRT_MANAGED_BOUNDS;
    uint32_t rune = 0;
    bool found = xr_string_core_utf8_rune_at(text->bytes, text->length, (size_t)index, &rune, NULL);
    XR_CHECK(found, "a cached admitted scalar exists");
    *output = rune;
    return XRT_MANAGED_OK;
}
XR_FUNCDEF XrtManagedStatus xrt_string_backing_rune_next(const XrtStringBacking *text, size_t *cursor,
    uint32_t *rune, bool *has) {
    if (!text || !cursor || !rune || !has) return XRT_MANAGED_ARGUMENT;
    if (*cursor > text->length) return XRT_MANAGED_BOUNDS;
    if (*cursor == text->length) { *has = false; return XRT_MANAGED_OK; }
    if (((uint8_t)text->bytes[*cursor] & UINT8_C(0xc0)) == UINT8_C(0x80)) return XRT_MANAGED_BOUNDS;
    uint32_t value = 0;
    size_t width = (size_t)xr_string_core_utf8_decode(text->bytes + *cursor, text->length - *cursor, &value);
    XR_CHECK(width && width <= text->length - *cursor, "an admitted cursor advances by one complete scalar");
    *cursor += width;
    *rune = value;
    *has = true;
    return XRT_MANAGED_OK;
}
XR_FUNCDEF XrtManagedStatus xrt_string_backing_copy_into(XrtManagedFrame *frame, size_t slot, const XrtStringBacking *source) {
    if (!source) return XRT_MANAGED_ARGUMENT;
    XrtManagedHeader *owned = (XrtManagedHeader *)&source->managed;
    if (!xrt_managed_retain(owned)) return XRT_MANAGED_REFCOUNT;
    XrtManagedStatus status = xrt_managed_slot_replace(frame, slot, &owned);
    xrt_managed_drop(owned);
    return status;
}
XR_FUNCDEF XrtManagedStatus xrt_string_backing_slice_create(XrtManagedFrame *frame, size_t slot,
    const XrtBackingTypes *types, const XrtStringBacking *text, int64_t start, int64_t end) {
    if (!text || !types || !types->string) return XRT_MANAGED_ARGUMENT;
    if (start < 0 || end < start || (uint64_t)end > text->runes) return XRT_MANAGED_BOUNDS;
    size_t first = scalar_offset(text, (size_t)start), last = scalar_offset(text, (size_t)end);
    return string_bytes_create(frame, slot, types, text->bytes + first, last - first, (size_t)(end - start));
}
XR_FUNCDEF XrtManagedStatus xrt_string_backing_repeat_create(XrtManagedFrame *frame, size_t slot,
    const XrtBackingTypes *types, const XrtStringBacking *text, int64_t count) {
    if (!text || !types || !types->string) return XRT_MANAGED_ARGUMENT;
    XrStringCoreRepeatPlan plan = xr_string_core_repeat_plan(text->bytes, text->length, count);
    if (plan.kind == XR_STRING_CORE_REPEAT_INVALID) return XRT_MANAGED_OVERFLOW;
    if (plan.kind == XR_STRING_CORE_REPEAT_ORIGINAL) return xrt_string_backing_copy_into(frame, slot, text);
    size_t runes = 0;
    if (count > 0 && text->runes) {
        if ((uint64_t)count > SIZE_MAX / text->runes) return XRT_MANAGED_OVERFLOW;
        runes = text->runes * (size_t)count;
    }
    XrtStringBacking *string = NULL;
    XrtManagedStatus status = string_prepare(frame, slot, types, plan.len, runes, &string);
    if (status != XRT_MANAGED_OK) return status;
    XR_CHECK(xr_string_core_repeat_write(string->bytes, text->bytes, text->length, count) == plan.len,
        "the shared repetition plan and writer agree");
    return string_finish(frame, slot);
}
XR_FUNCDEF XrtManagedStatus xrt_string_backing_replace_create(XrtManagedFrame *frame, size_t slot,
    const XrtBackingTypes *types, const XrtStringBacking *text, XrtStringReplacement request) {
    if (!types || !types->string || !request.replacement) return XRT_MANAGED_ARGUMENT;
    XrtManagedStatus status = search_extent(text, request.search);
    if (status != XRT_MANAGED_OK) return status;
    const XrtStringBacking *search = request.search, *replacement = request.replacement;
    XrStringCoreReplacePlan plan = xr_string_core_replace_plan(text->bytes, text->length,
        search->bytes, search->length, replacement->bytes, replacement->length, request.all);
    if (plan.kind == XR_STRING_CORE_REPLACE_INVALID) return XRT_MANAGED_OVERFLOW;
    if (plan.kind == XR_STRING_CORE_REPLACE_ORIGINAL) return xrt_string_backing_copy_into(frame, slot, text);
    XR_CHECK(search->runes && plan.count <= text->runes / search->runes, "nonoverlapping matches consume admitted scalars");
    size_t runes = text->runes - plan.count * search->runes;
    if (replacement->runes && plan.count > (SIZE_MAX - runes) / replacement->runes) return XRT_MANAGED_OVERFLOW;
    runes += plan.count * replacement->runes;
    XrtStringBacking *string = NULL;
    status = string_prepare(frame, slot, types, plan.len, runes, &string);
    if (status != XRT_MANAGED_OK) return status;
    XR_CHECK(xr_string_core_replace_write(string->bytes, text->bytes, text->length, search->bytes,
        search->length, replacement->bytes, replacement->length, plan, request.all) == plan.len,
        "the shared replacement plan and writer agree");
    return string_finish(frame, slot);
}
XR_FUNCDEF XrtManagedStatus xrt_string_backing_copy_bytes_create(XrtManagedFrame *frame, size_t slot,
    const XrtBackingTypes *types, const XrtStringBacking *text) {
    if (!text) return XRT_MANAGED_ARGUMENT;
    return xrt_array_backing_u8_create(frame, slot, types, (const uint8_t *)text->bytes, text->length, text->length);
}
XR_FUNCDEF XrtStringSplitResult xrt_string_backing_split_create(XrtManagedFrame *frame, size_t slot,
    const XrtBackingTypes *types, const XrtStringBacking *text, const XrtStringBacking *separator, size_t workspace) {
    XrtStringSplitResult result = {XRT_MANAGED_ARGUMENT, false};
    if (!types || !types->string || !types->arrays[XRT_BACKING_ARRAY_STRING] || !text || !separator || slot == workspace)
        return result;
    XrtManagedHeader *borrowed = NULL;
    result.status = xrt_managed_slot_borrow(frame, workspace, &borrowed);
    if (result.status != XRT_MANAGED_OK || borrowed) { result.status = XRT_MANAGED_ARGUMENT; return result; }
    if (!separator->length) return (XrtStringSplitResult){XRT_MANAGED_OK, true};
    result.status = search_extent(text, separator);
    if (result.status != XRT_MANAGED_OK) return result;
    XrStringCoreSplitPlan plan = xr_string_core_split_plan(text->bytes, text->length, separator->bytes, separator->length);
    if (plan.kind == XR_STRING_CORE_SPLIT_INVALID || plan.count > (SIZE_MAX - sizeof(XrtArrayBacking)) / sizeof(borrowed))
        return (XrtStringSplitResult){XRT_MANAGED_OVERFLOW, false};
    result.status = xrt_managed_slot_prepare(frame, slot, types->arrays[XRT_BACKING_ARRAY_STRING],
        sizeof(XrtArrayBacking) + plan.count * sizeof(borrowed), &borrowed);
    if (result.status != XRT_MANAGED_OK) return result;
    XrtArrayBacking *array = (XrtArrayBacking *)borrowed;
    size_t position = 0;
    for (size_t i = 0; i < plan.count; ++i) {
        ptrdiff_t found = xr_string_core_index_of(text->bytes + position, text->length - position,
            separator->bytes, separator->length);
        size_t length = found < 0 ? text->length - position : (size_t)found;
        size_t runes = xr_text_scalar_count((const uint8_t *)text->bytes + position, length);
        result.status = string_bytes_create(frame, workspace, types, text->bytes + position, length, runes);
        if (result.status != XRT_MANAGED_OK) {
            XR_CHECK(xrt_managed_slot_abort(frame, slot) == XRT_MANAGED_OK, "partial split owns its Array token");
            return result;
        }
        XrtManagedHeader *owned = NULL;
        XR_CHECK(xrt_managed_slot_take(frame, workspace, &owned) == XRT_MANAGED_OK,
            "the exclusive split workspace moves one complete owner");
        memcpy(array->elements + i * sizeof(owned), &owned, sizeof(owned));
        array->length = i + 1;
        if (found >= 0) position += length + separator->length;
    }
    result.status = string_finish(frame, slot);
    return result;
}
XR_FUNCDEF XrtManagedStatus xrt_string_backing_try_append_exclusive(XrtStringBacking *text,
    const XrtStringBacking *suffix, bool *appended) {
    if (!text || !suffix || !appended) return XRT_MANAGED_ARGUMENT;
    if (suffix->length > SIZE_MAX - text->length || suffix->runes > SIZE_MAX - text->runes)
        return XRT_MANAGED_OVERFLOW;
    size_t length = text->length + suffix->length, runes = text->runes + suffix->runes;
    if (length == SIZE_MAX) return XRT_MANAGED_OVERFLOW;
    if (length >= text->capacity || !xr_signed_rc_unique(&text->managed.references)) {
        *appended = false;
        return XRT_MANAGED_OK;
    }
    if (suffix->length) {
        memmove(text->bytes + text->length, suffix->bytes, suffix->length);
        atomic_store_explicit(&text->cached_hash, 0, memory_order_relaxed);
    }
    text->bytes[length] = 0;
    text->length = length;
    text->runes = runes;
    *appended = true;
    return XRT_MANAGED_OK;
}
typedef struct StringAppendTransaction {
    const XrtStringBacking *suffix;
    XrtStringBacking *source;
    bool appended;
} StringAppendTransaction;
static XrtManagedStatus string_append_apply(XrtManagedHeader *source, void *context) {
    StringAppendTransaction *transaction = context;
    transaction->source = (XrtStringBacking *)source;
    if (transaction->source->length >= transaction->source->capacity ||
        transaction->source->runes > transaction->source->length ||
        transaction->suffix->length >= transaction->suffix->capacity ||
        transaction->suffix->runes > transaction->suffix->length) return XRT_MANAGED_ARGUMENT;
    return xrt_string_backing_try_append_exclusive(transaction->source,
        transaction->suffix, &transaction->appended);
}
static bool string_append_edit(XrtManagedHeader *source, const void *suffix) {
    StringAppendTransaction transaction = {.suffix=suffix};
    return string_append_apply(source, &transaction) == XRT_MANAGED_OK && transaction.appended;
}
static size_t string_fill_window(XrtStringBacking *text, const XrtStringBacking *suffix, size_t count) {
    if (!text || !suffix || text == suffix || text->length >= text->capacity || text->runes > text->length ||
        !suffix->length || suffix->length >= suffix->capacity || suffix->runes > suffix->length ||
        xr_signed_rc_read(&text->managed.references) != 1) return 0;
    size_t steps = (text->capacity - text->length - 1) / suffix->length;
    if (steps > count) steps = count;
    if (!steps) return 0;
    char *next = text->bytes + text->length;
    if (suffix->length == 1) memset(next, (unsigned char)suffix->bytes[0], steps);
    else for (size_t n = 0; n < steps; ++n) {
        memcpy(next, suffix->bytes, suffix->length);
        next += suffix->length;
    }
    text->length += steps * suffix->length;
    text->runes += steps * suffix->runes;
    text->bytes[text->length] = 0;
    atomic_store_explicit(&text->cached_hash, 0, memory_order_relaxed);
    return steps;
}

XRAY_API size_t xrt_native_string_append_window(XrtManagedFrame *frame, size_t owner,
    size_t suffix, size_t count, const size_t *empty_slots, size_t empty_count) {
    XrtManagedSlotLocation text, piece;
    if (!count || (empty_count && !empty_slots) || owner == suffix ||
        xrt_managed_slot_location(frame, owner, &text) != XRT_MANAGED_OK ||
        xrt_managed_slot_location(frame, suffix, &piece) != XRT_MANAGED_OK ||
        xrt_managed_domain_enter(text) != XRT_MANAGED_OK) return 0;
    size_t done = 0;
    bool ready = !text.pending->value && !text.pending->allocation_bytes &&
        !piece.pending->value && !piece.pending->allocation_bytes;
    for (size_t n = 0; ready && n < empty_count; ++n) {
        XrtManagedSlotLocation empty;
        ready = xrt_managed_slot_location(frame, empty_slots[n], &empty) == XRT_MANAGED_OK &&
            !*empty.owner && !empty.pending->value && !empty.pending->allocation_bytes;
    }
    if (ready) done = string_fill_window((XrtStringBacking *)*text.owner, (const XrtStringBacking *)*piece.owner, count);
    xrt_managed_domain_leave(text);
    return done;
}

static XrtManagedStatus string_append_grow(XrtPlaceReplacement *reservation,
    const XrtBackingTypes *types, const StringAppendTransaction *transaction, bool allow_resize) {
    const XrtStringBacking *left = transaction->source, *right = transaction->suffix;
    if (right->length > SIZE_MAX - left->length || right->runes > SIZE_MAX - left->runes)
        return XRT_MANAGED_OVERFLOW;
    size_t prefix = left->length, length = prefix + right->length, runes = left->runes + right->runes;
    size_t maximum = SIZE_MAX - sizeof(XrtStringBacking);
    if (length >= maximum) return XRT_MANAGED_OVERFLOW;
    size_t required = length + 1, capacity = left->capacity;
    if (capacity < 64) capacity = 64;
    if (capacity > maximum) return XRT_MANAGED_OVERFLOW;
    while (capacity < required) {
        if (capacity > maximum / 2) { capacity = required; break; }
        capacity *= 2;
    }
    XrtManagedHeader *storage = NULL;
    size_t bytes = sizeof(XrtStringBacking) + capacity;
    XrtManagedStatus status = XRT_MANAGED_OK;
    /* A self suffix still borrows the old address. Only a distinct suffix
     * survives a successful resize of a consumed source. */
    if (allow_resize && left != right && required > left->capacity)
        status = xrt_managed_slot_reserved_grow_scalar_tail(reservation, types->string, bytes, &storage);
    if (status != XRT_MANAGED_OK) return status;
    bool resized = storage != NULL;
    /* The initialized byte span ends at the terminator. Future appends
     * write their own suffix before making that part of the tail readable. */
    if (!resized) status = xrt_managed_slot_reserved_prepare_scalar_tail(reservation, types->string, bytes, &storage);
    if (status != XRT_MANAGED_OK) return status;
    XrtStringBacking *value = (XrtStringBacking *)storage;
    /* A newly allocated or resized object has no readers until publication. */
    atomic_init(&value->cached_hash, 0);
    XR_CHECK(value->capacity == capacity, "String growth charges its actual complete capacity");
    if (!resized && prefix) memcpy(value->bytes, left->bytes, prefix);
    if (right->length) memcpy(value->bytes + prefix, right->bytes, right->length);
    value->bytes[length] = 0;
    value->length = length;
    value->runes = runes;
    return resized ? XRT_MANAGED_OK : xrt_managed_slot_reserved_commit(reservation);
}
XR_FUNCDEF XrtManagedStatus xrt_string_backing_append_owned(XrtManagedFrame *frame,
    const XrtBackingTypes *types, const XrtStringAppendOwned *input) {
    if (!frame || !types || !types->string || !input || !input->suffix) return XRT_MANAGED_ARGUMENT;
    if (xrt_managed_slot_try_edit_move(frame, input->source_slot, input->result_slot,
        input->temporary_slot, string_append_edit, input->suffix)) return XRT_MANAGED_OK;

    XrtPlaceReplacement reservation = {0};
    XrtPlace source = {.frame=frame, .slot=input->source_slot, .nulls=XRT_PLACE_NONNULL};
    XrtManagedStatus status = xrt_place_replacement_begin(source, frame, input->temporary_slot, &reservation);
    if (status != XRT_MANAGED_OK) return status;
    status = xrt_managed_slot_reserved_result_check(&reservation, input->result_slot);
    StringAppendTransaction transaction = {.suffix=input->suffix};
    if (status == XRT_MANAGED_OK)
        status = xrt_managed_slot_reserved_apply(&reservation, string_append_apply, &transaction);
    if (status == XRT_MANAGED_OK && !transaction.appended)
        status = string_append_grow(&reservation, types, &transaction, true);
    if (status == XRT_MANAGED_OK) {
        status = xrt_managed_slot_reserved_consume_publish(&reservation, input->result_slot);
        XR_CHECK(status == XRT_MANAGED_OK, "prechecked stable String slots cannot reject completed publication");
    }
    XR_CHECK(xrt_place_replacement_end(&reservation) == XRT_MANAGED_OK,
        "String failure and success release one registered ownership transaction");
    return status;
}
XR_FUNCDEF XrtManagedStatus xrt_string_backing_append_place(XrtPlace binding, XrtManagedFrame *frame,
    size_t workspace, const XrtBackingTypes *types, const XrtStringBacking *suffix) {
    if (!types || !types->string || !suffix) return XRT_MANAGED_ARGUMENT;
    XrtPlaceReplacement reservation = {0};
    XrtManagedStatus status = xrt_place_replacement_begin(binding, frame, workspace, &reservation);
    if (status != XRT_MANAGED_OK) return status;
    StringAppendTransaction transaction = {.suffix=suffix};
    status = xrt_managed_slot_reserved_apply(&reservation, string_append_apply, &transaction);
    XrtManagedHeader *suffix_pin = NULL;
    if (status == XRT_MANAGED_OK && !transaction.appended) {
        /* The allocation-free attempt made no change on a miss. Only growth
         * can invoke a provider; keep its distinct suffix alive first without
         * making the actual left binding look shared. Self borrows use the
         * source reference already held by this guarded binding. */
        if (suffix != transaction.source) {
            suffix_pin = (XrtManagedHeader *)&suffix->managed;
            if (!xrt_managed_retain(suffix_pin)) { suffix_pin = NULL; status = XRT_MANAGED_REFCOUNT; }
        }
        if (status == XRT_MANAGED_OK) status = string_append_grow(&reservation, types, &transaction, false);
        if (status == XRT_MANAGED_OK) status = xrt_place_replacement_publish(&reservation);
    }
    xrt_managed_drop(suffix_pin);
    XR_CHECK(xrt_place_replacement_end(&reservation) == XRT_MANAGED_OK,
        "String binding retirement finishes before its actual domains leave");
    return status;
}
XR_FUNCDEF XrtManagedStatus xrt_string_backing_i64_create(XrtManagedFrame *frame, size_t slot,
    const XrtBackingTypes *types, int64_t value) {
    uint8_t bytes[XR_TEXT_I64_DISPLAY_MAX];
    size_t length = xr_text_display_i64(value, bytes);
    return string_bytes_create(frame, slot, types, (const char *)bytes, length, length);
}
XR_FUNCDEF XrtManagedStatus xrt_string_backing_u64_create(XrtManagedFrame *frame, size_t slot,
    const XrtBackingTypes *types, uint64_t value) {
    uint8_t bytes[XR_TEXT_U64_DISPLAY_MAX];
    size_t length = xr_text_display_u64(value, bytes);
    return string_bytes_create(frame, slot, types, (const char *)bytes, length, length);
}
XR_FUNCDEF XrtManagedStatus xrt_string_backing_f64_create(XrtManagedFrame *frame, size_t slot,
    const XrtBackingTypes *types, uint64_t bits) {
    char bytes[32]; size_t length = 0;
    XR_CHECK(xr_float_to_decimal(64, bits, bytes, sizeof(bytes), &length), "every binary64 pattern has canonical text");
    return string_bytes_create(frame, slot, types, bytes, length, length);
}
XR_FUNCDEF XrtManagedStatus xrt_string_backing_bool_create(XrtManagedFrame *frame, size_t slot,
    const XrtBackingTypes *types, bool value) {
    return string_bytes_create(frame, slot, types, value ? "true" : "false", value ? 4 : 5, value ? 4 : 5);
}
XR_FUNCDEF XrtManagedStatus xrt_string_backing_rune_create(XrtManagedFrame *frame, size_t slot,
    const XrtBackingTypes *types, uint32_t value) {
    uint8_t bytes[XR_TEXT_RUNE_UTF8_MAX];
    size_t length = xr_text_encode_rune(value, bytes);
    if (!length) return XRT_MANAGED_ARGUMENT;
    return string_bytes_create(frame, slot, types, (const char *)bytes, length, 1);
}
static XrtManagedStatus pod_scalar_display(XrtManagedFrame *frame, size_t slot,
    const XrtBackingTypes *types, XrPodScalar scalar, const void *value) {
    int64_t signed_value = 0;
    uint64_t unsigned_value = 0;
    uint32_t float_width = 0;
    switch (scalar) {
    case XR_POD_UNIT: return string_bytes_create(frame, slot, types, "()", 2, 2);
    case XR_POD_BOOL: {
        uint8_t byte = 0; memcpy(&byte, value, sizeof(byte));
        if (byte > 1) return XRT_MANAGED_ARGUMENT;
        return xrt_string_backing_bool_create(frame, slot, types, byte != 0);
    }
#define POD_SIGNED(kind, type) case kind: { type bits; memcpy(&bits, value, sizeof(bits)); signed_value = bits; break; }
    POD_SIGNED(XR_POD_I8, int8_t)
    POD_SIGNED(XR_POD_I16, int16_t)
    POD_SIGNED(XR_POD_I32, int32_t)
    POD_SIGNED(XR_POD_I64, int64_t)
#undef POD_SIGNED
#define POD_UNSIGNED(kind, type) case kind: { type bits; memcpy(&bits, value, sizeof(bits)); unsigned_value = bits; break; }
    POD_UNSIGNED(XR_POD_U8, uint8_t)
    POD_UNSIGNED(XR_POD_U16, uint16_t)
    POD_UNSIGNED(XR_POD_U32, uint32_t)
    POD_UNSIGNED(XR_POD_U64, uint64_t)
#undef POD_UNSIGNED
    case XR_POD_F32: { uint32_t bits; memcpy(&bits, value, sizeof(bits)); unsigned_value = bits; float_width = 32; break; }
    case XR_POD_F64: memcpy(&unsigned_value, value, sizeof(unsigned_value)); float_width = 64; break;
    default: return XRT_MANAGED_ARGUMENT;
    }
    if (float_width) {
        char bytes[32]; size_t length = 0;
        XR_CHECK(xr_float_to_decimal(float_width, unsigned_value, bytes, sizeof(bytes), &length),
            "every admitted POD float bit pattern has canonical text");
        return string_bytes_create(frame, slot, types, bytes, length, length);
    }
    if (scalar >= XR_POD_I8 && scalar <= XR_POD_I64)
        return xrt_string_backing_i64_create(frame, slot, types, signed_value);
    return xrt_string_backing_u64_create(frame, slot, types, unsigned_value);
}
XR_FUNCDEF XrtManagedStatus xrt_string_backing_pod_display_create(XrtManagedFrame *frame, size_t slot,
    const XrtBackingTypes *types, const XrtPodDisplay *description, const void *value) {
    XrPodLayout scalar = {0};
    if (!description || !xr_pod_scalar_layout(description->scalar, &scalar)) return XRT_MANAGED_ARGUMENT;
    uint64_t size = scalar.size + (uint64_t)description->nullable_depth * scalar.alignment;
    uint64_t fields = (uint64_t)scalar.scalar_count + description->nullable_depth;
    if (size != description->physical.size || scalar.alignment != description->physical.alignment ||
        fields != description->physical.scalar_count || (size && !value)) return XRT_MANAGED_ARGUMENT;
    const unsigned char *bytes = value;
    for (uint32_t depth = 0; depth < description->nullable_depth; ++depth) {
        if (*bytes > 1) return XRT_MANAGED_ARGUMENT;
        if (!*bytes) return string_bytes_create(frame, slot, types, "null", 4, 4);
        bytes += scalar.alignment;
    }
    return pod_scalar_display(frame, slot, types, description->scalar, bytes);
}
