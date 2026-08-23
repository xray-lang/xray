/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt_assertion.h - Hosted AOT execution of typed assertion plans
 */

#ifndef XRT_ASSERTION_H
#define XRT_ASSERTION_H

#include "../shared/xr_assertion_core.h"
#include "../shared/xr_deep_equality.h"

typedef struct XrtAssertionText {
    const char *bytes;
    XrValue owned;
} XrtAssertionText;

#ifndef XRT_ASSERTION_FORMAT_STEP
#define XRT_ASSERTION_FORMAT_STEP(step) ((void) (step))
#endif

static inline bool xrt_assertion_deep_describe(void *adapter, XrValue value,
                                               XrDeepEqualityNode *out) {
    (void) adapter;
    if (!out)
        return false;
    *out = (XrDeepEqualityNode){
        .kind = XR_DEEP_EQUALITY_IDENTITY,
        .identity = value.ptr,
    };
    uint32_t kind = xrt_value_kind(value);
    kind = kind == XR_TAG_STR_ARC ? XR_TAG_STR : kind;
    switch (kind) {
        case XR_TAG_STR:
            out->kind = XR_DEEP_EQUALITY_STRING;
            return value.ptr != NULL;
        case XR_TAG_ARRAY: {
            const xrt_array_t *array = (const xrt_array_t *) value.ptr;
            if (!array || array->length < 0 || (uint64_t) array->length > UINT32_MAX)
                return false;
            out->kind = XR_DEEP_EQUALITY_ARRAY;
            out->nominal_identity = array->elem_type;
            out->logical_count = out->iteration_extent = (uint32_t) array->length;
            return true;
        }
        case XR_TAG_TUPLE: {
            const xrt_tuple_t *tuple = (const xrt_tuple_t *) value.ptr;
            if (!tuple || tuple->len < 0 || (uint64_t) tuple->len > UINT32_MAX)
                return false;
            out->kind = XR_DEEP_EQUALITY_TUPLE;
            out->logical_count = out->iteration_extent = (uint32_t) tuple->len;
            return true;
        }
        case XR_TAG_MAP: {
            const xrt_map_t *map = (const xrt_map_t *) value.ptr;
            int64_t length = map ? xrt_map_len(map) : -1;
            if (!map || length < 0 || (uint64_t) length > UINT32_MAX)
                return false;
            if (map->class_name) {
                out->kind = XR_DEEP_EQUALITY_IDENTITY;
                return true;
            }
            out->kind = XR_DEEP_EQUALITY_MAP;
            out->logical_count = (uint32_t) length;
            out->iteration_extent = xrt_map_is_boolmap(map)
                                        ? 2u
                                        : (xrt_map_is_typed(map) ? (uint32_t) map->cap
                                                                 : map->nentries);
            return true;
        }
        case XR_TAG_SET: {
            const xrt_set_t *set = (const xrt_set_t *) value.ptr;
            int64_t length = set ? xrt_set_len(set) : -1;
            if (!set || length < 0 || (uint64_t) length > UINT32_MAX)
                return false;
            out->kind = XR_DEEP_EQUALITY_SET;
            out->logical_count = (uint32_t) length;
            out->iteration_extent = xrt_set_is_typed(set) ? (uint32_t) set->cap : set->nentries;
            return true;
        }
        case XR_TAG_ENUM: {
            uint32_t member_index = 0;
            uint32_t layout_id = 0;
            if (!xrt_enum_key_parts(value, NULL, NULL, &member_index, &layout_id))
                return false;
            const XrAotEnumBox *box = xrt_enum_payload_box(value);
            out->kind = XR_DEEP_EQUALITY_ENUM;
            out->nominal_identity = layout_id ? layout_id : (uint64_t) (uintptr_t) value.ptr;
            out->ordinal = member_index;
            out->logical_count = out->iteration_extent = box ? box->payload_count : 0;
            return true;
        }
        case XR_TAG_PTR:
            break;
        default:
            return true;
    }
    if (xrt_is_struct_object_value(value)) {
        xrt_object_t *object = (xrt_object_t *) value.ptr;
        int64_t count = xrt_object_field_count(object);
        if (count < 0 || (uint64_t) count > UINT32_MAX)
            return false;
        out->kind = XR_DEEP_EQUALITY_STRUCT_OBJECT;
        out->nominal_identity = xrt_object_domain(object);
        out->logical_count = out->iteration_extent = (uint32_t) count;
        return true;
    }
    if (value.heap_type == XR_TINSTANCE && value.ptr) {
        uint16_t type_id = xrt_aot_class_type_id((const XrObjHeader *) value.ptr);
        const XrtTypeDeriveInfo *derive = xrt_type_derive_info(type_id);
        if (type_id && derive && (derive->derive_flags & XR_DERIVE_EQ) != 0) {
            out->kind = XR_DEEP_EQUALITY_DERIVED_INSTANCE;
            out->nominal_identity = type_id;
            out->logical_count = out->iteration_extent = derive->inspect_field_count;
        }
    }
    return true;
}

static inline bool xrt_assertion_deep_fallback_equal(void *adapter, XrValue left,
                                                     XrValue right) {
    (void) adapter;
    return xrt_eq(left, right) != 0;
}

static inline bool xrt_assertion_deep_string_equal(void *adapter, XrValue left, XrValue right) {
    (void) adapter;
    if (!XR_IS_STR(left) || !XR_IS_STR(right))
        return false;
    int64_t length = xr_str_len(left);
    return length == xr_str_len(right) && length >= 0 && xr_str_data(left) && xr_str_data(right) &&
           memcmp(xr_str_data(left), xr_str_data(right), (size_t) length) == 0;
}

static inline bool xrt_assertion_deep_sequence_element(void *adapter, XrValue sequence,
                                                       uint32_t index, XrValue *out) {
    (void) adapter;
    if (!out)
        return false;
    uint32_t kind = xrt_value_kind(sequence);
    if (kind == XR_TAG_ARRAY) {
        xrt_array_t *array = (xrt_array_t *) sequence.ptr;
        if (!array || index >= (uint32_t) array->length)
            return false;
        *out = xr_typed_get(array->data, (int32_t) index, array->elem_type);
        return true;
    }
    if (kind == XR_TAG_TUPLE) {
        xrt_tuple_t *tuple = (xrt_tuple_t *) sequence.ptr;
        if (!tuple || index >= (uint32_t) tuple->len)
            return false;
        *out = tuple->items[index];
        return true;
    }
    if (kind == XR_TAG_ENUM) {
        const XrAotEnumBox *box = xrt_enum_payload_box(sequence);
        if (!box || index >= box->payload_count)
            return false;
        *out = box->payloads[index];
        return true;
    }
    if (kind == XR_TAG_PTR && sequence.heap_type == XR_TINSTANCE && sequence.ptr) {
        uint16_t type_id = xrt_aot_class_type_id((const XrObjHeader *) sequence.ptr);
        const XrtTypeDeriveInfo *derive = xrt_type_derive_info(type_id);
        if (!derive || index >= derive->inspect_field_count || !derive->inspect_fields)
            return false;
        *out = xrt_inspect_field_value(sequence.ptr, &derive->inspect_fields[index]);
        return true;
    }
    return false;
}

static inline bool xrt_assertion_deep_map_entry(void *adapter, XrValue value, uint32_t slot,
                                                bool *present, XrValue *key,
                                                XrValue *entry_value) {
    (void) adapter;
    xrt_map_t *map = XR_IS_MAP(value) ? (xrt_map_t *) value.ptr : NULL;
    if (!map || !present || !key || !entry_value)
        return false;
    if (xrt_map_is_boolmap(map)) {
        if (slot >= 2)
            return false;
        *key = XR_FROM_BOOL(slot != 0);
        *present = xrt_map_has(map, *key) != 0;
        if (*present)
            *entry_value = xrt_map_get(map, *key);
        return true;
    }
    uint32_t extent = xrt_map_is_typed(map) ? (uint32_t) map->cap : map->nentries;
    if (slot >= extent)
        return false;
    *present = xrt_map_slot_is_full(map, (int64_t) slot) != 0;
    if (*present) {
        *key = xrt_map_slot_key(map, (int64_t) slot);
        *entry_value = xrt_map_slot_value(map, (int64_t) slot);
    }
    return true;
}

static inline bool xrt_assertion_deep_map_find(void *adapter, XrValue value, XrValue key,
                                               bool *found, XrValue *entry_value) {
    (void) adapter;
    xrt_map_t *map = XR_IS_MAP(value) ? (xrt_map_t *) value.ptr : NULL;
    if (!map || !found || !entry_value)
        return false;
    *found = xrt_map_has(map, key) != 0;
    if (*found)
        *entry_value = xrt_map_get(map, key);
    return true;
}

static inline bool xrt_assertion_deep_set_entry(void *adapter, XrValue value, uint32_t slot,
                                                bool *present, XrValue *entry_value) {
    (void) adapter;
    xrt_set_t *set = XR_IS_SET(value) ? (xrt_set_t *) value.ptr : NULL;
    if (!set || !present || !entry_value)
        return false;
    uint32_t extent = xrt_set_is_typed(set) ? (uint32_t) set->cap : set->nentries;
    if (slot >= extent)
        return false;
    *present = xrt_set_slot_is_full(set, (int64_t) slot) != 0;
    if (*present)
        *entry_value = xrt_set_slot_item(set, (int64_t) slot);
    return true;
}

static inline bool xrt_assertion_deep_set_contains(void *adapter, XrValue value,
                                                   XrValue entry_value, bool *contains) {
    (void) adapter;
    xrt_set_t *set = XR_IS_SET(value) ? (xrt_set_t *) value.ptr : NULL;
    if (!set || !contains)
        return false;
    *contains = xrt_set_has(set, entry_value) != 0;
    return true;
}

static inline bool xrt_assertion_deep_struct_field_pair(
    void *adapter, XrValue left, XrValue right, uint32_t left_ordinal, XrValue *left_value,
    XrValue *right_value) {
    (void) adapter;
    xrt_object_t *a = xrt_is_struct_object_value(left) ? (xrt_object_t *) left.ptr : NULL;
    xrt_object_t *b = xrt_is_struct_object_value(right) ? (xrt_object_t *) right.ptr : NULL;
    const char *name = a ? xrt_object_field_name(a, left_ordinal) : NULL;
    int64_t right_index = b && name ? xrt_object_find_field(b, name) : -1;
    if (!a || !b || !left_value || !right_value ||
        left_ordinal >= (uint32_t) xrt_object_field_count(a) || right_index < 0)
        return false;
    *left_value = a->fields[left_ordinal];
    *right_value = b->fields[right_index];
    return true;
}

static const XrDeepEqualityOps xrt_assertion_deep_equality_ops = {
    .describe = xrt_assertion_deep_describe,
    .fallback_equal = xrt_assertion_deep_fallback_equal,
    .string_equal = xrt_assertion_deep_string_equal,
    .sequence_element = xrt_assertion_deep_sequence_element,
    .map_entry = xrt_assertion_deep_map_entry,
    .map_find_key_equivalent = xrt_assertion_deep_map_find,
    .set_entry = xrt_assertion_deep_set_entry,
    .set_contains_key_equivalent = xrt_assertion_deep_set_contains,
    .struct_field_pair = xrt_assertion_deep_struct_field_pair,
};

static inline bool xrt_assertion_deep_equal(XrValue left, XrValue right) {
    return xr_deep_equality_apply(&xrt_assertion_deep_equality_ops, NULL, left, right);
}

static inline XrValue xrt_assertion_literal_exception(const char *text, size_t length) {
    return xrt_exception_new_value(0, text, length);
}

static inline bool xrt_assertion_value_text(XrValue value, XrtAssertionText *out) {
    if (!out)
        return false;
    *out = (XrtAssertionText){.owned = XR_NULL_VAL};
    if (XR_IS_STR(value)) {
        out->bytes = xr_str_data(value);
        return out->bytes != NULL;
    }
    out->owned = xrt_value_to_string(value);
    if (!XR_IS_STR(out->owned) || !xr_str_data(out->owned)) {
        xrt_release(out->owned);
        *out = (XrtAssertionText){.owned = XR_NULL_VAL};
        return false;
    }
    out->bytes = xr_str_data(out->owned);
    return true;
}

static inline void xrt_assertion_text_release(XrtAssertionText *text) {
    if (!text)
        return;
    xrt_release(text->owned);
    *text = (XrtAssertionText){.owned = XR_NULL_VAL};
}

/* Formatting is target code: a custom formatter may publish a typed error or
 * panic after earlier operands already produced owned text.  Contain that
 * channel at each operand boundary so the caller can run one cleanup frontier
 * and turn a formatter failure into the canonical assertion failure. */
static inline bool xrt_assertion_value_text_guarded(XrValue value, XrtAssertionText *out,
                                                     unsigned step) {
    XrValue saved_error = xrt_pending_error;
    xrt_pending_error = XR_NULL_VAL;
    XrtExcFrame frame;
    frame.prev = xrt_exc_top;
    frame.exception = XR_NULL_VAL;
    xrt_exc_top = &frame;
    bool panicked = setjmp(frame.buf) != 0;
    bool ok = false;
    if (!panicked) {
        XRT_ASSERTION_FORMAT_STEP(step);
        ok = xrt_assertion_value_text(value, out);
    }
    xrt_exc_top = frame.prev;
    XrValue formatter_error = xrt_pending_error;
    xrt_pending_error = saved_error;
    if (!XR_IS_NULL(formatter_error)) {
        xrt_release(formatter_error);
        ok = false;
    }
    if (panicked) {
        xrt_release(frame.exception);
        ok = false;
    }
    if (!ok)
        xrt_assertion_text_release(out);
    return ok;
}

static inline XrValue
xrt_assertion_failure_exception(XrAssertionFailureKind kind, const char *file, uint32_t line,
                                uint32_t column, uint32_t end_line, uint32_t end_column,
                                XrValue message, XrValue actual, XrValue expected,
                                XrValue caught_error, XrValue caught_panic) {
    XrtAssertionText actual_text = {.owned = XR_NULL_VAL};
    XrtAssertionText expected_text = {.owned = XR_NULL_VAL};
    XrtAssertionText error_text = {.owned = XR_NULL_VAL};
    XrtAssertionText panic_text = {.owned = XR_NULL_VAL};
    const char *message_text = NULL;
    if (!XR_IS_NULL(message)) {
        if (!XR_IS_STR(message))
            return xrt_assertion_literal_exception(
                "typed assertion message is not a string",
                sizeof("typed assertion message is not a string") - 1u);
        message_text = xr_str_data(message);
    }
    bool text_ok =
        (XR_IS_NULL(actual) || xrt_assertion_value_text_guarded(actual, &actual_text, 0u)) &&
        (XR_IS_NULL(expected) ||
         xrt_assertion_value_text_guarded(expected, &expected_text, 1u)) &&
        (XR_IS_NULL(caught_error) ||
         xrt_assertion_value_text_guarded(caught_error, &error_text, 2u)) &&
        (XR_IS_NULL(caught_panic) ||
         xrt_assertion_value_text_guarded(caught_panic, &panic_text, 3u));
    if (!text_ok) {
        xrt_assertion_text_release(&actual_text);
        xrt_assertion_text_release(&expected_text);
        xrt_assertion_text_release(&error_text);
        xrt_assertion_text_release(&panic_text);
        return xrt_assertion_literal_exception(
            "typed assertion value formatter failed",
            sizeof("typed assertion value formatter failed") - 1u);
    }
    XrAssertionFailure failure = {
        .kind = kind,
        .source = {file, line, column, end_line, end_column},
        .message = message_text,
        .actual = actual_text.bytes,
        .expected = expected_text.bytes,
        .caught_error = error_text.bytes,
        .caught_panic = panic_text.bytes,
    };
    int size = xr_assertion_failure_render_size(&failure);
    char *rendered = size >= 0 ? (char *) XRT_MALLOC((size_t) size + 1u) : NULL;
    XrValue exception = XR_NULL_VAL;
    if (size < 0) {
        exception = xrt_assertion_literal_exception(
            "typed assertion failure schema is invalid",
            sizeof("typed assertion failure schema is invalid") - 1u);
    } else if (!rendered) {
        exception = xrt_assertion_literal_exception(
            "failed to render assertion failure",
            sizeof("failed to render assertion failure") - 1u);
    } else if (xr_assertion_failure_render(rendered, (size_t) size + 1u, &failure) != size) {
        exception = xrt_assertion_literal_exception(
            "typed assertion renderer rejected exact capacity",
            sizeof("typed assertion renderer rejected exact capacity") - 1u);
    } else {
        exception = xrt_exception_new_value(0, rendered, (size_t) size);
    }
    XRT_FREE(rendered);
    xrt_assertion_text_release(&actual_text);
    xrt_assertion_text_release(&expected_text);
    xrt_assertion_text_release(&error_text);
    xrt_assertion_text_release(&panic_text);
    return exception;
}

/* This is the hosted production binding named by the semantic-owner registry.
 * It consumes only the shared failure row; public names and assertion kinds are
 * resolved before C emission. */
static inline XRT_COLD _Noreturn void
xrt_assertion_fail(XrAssertionFailureKind kind, const char *file, uint32_t line,
                   uint32_t column, uint32_t end_line, uint32_t end_column, XrValue message,
                   XrValue actual, XrValue expected, XrValue caught_error, XrValue caught_panic) {
    XR_ASSERTION_OWNER_GUARD(XR_SEM_OWNER_ID_SHARED_ASSERTION_HI,
                             XR_SEM_OWNER_ID_SHARED_ASSERTION_LO);
    XR_ASSERTION_CONSUMER_GUARD(XR_SEM_CONSUMER_AOT_HOSTED);
    xrt_throw_exc(xrt_assertion_failure_exception(kind, file, line, column, end_line, end_column,
                                                  message, actual, expected, caught_error,
                                                  caught_panic));
}

static inline XrValue xrt_assertion_condition(int64_t condition, const char *file, uint32_t line,
                                              uint32_t column, uint32_t end_line,
                                              uint32_t end_column, XrValue message) {
    if (!condition)
        xrt_assertion_fail(XR_ASSERTION_FAILURE_CONDITION_FALSE, file, line, column, end_line,
                           end_column, message, XR_NULL_VAL, XR_NULL_VAL, XR_NULL_VAL,
                           XR_NULL_VAL);
    return XR_NULL_VAL;
}

static inline XrValue xrt_assertion_equal(XrValue actual, XrValue expected, const char *file,
                                          uint32_t line, uint32_t column, uint32_t end_line,
                                          uint32_t end_column, XrValue message) {
    if (!xrt_assertion_deep_equal(actual, expected))
        xrt_assertion_fail(XR_ASSERTION_FAILURE_VALUES_NOT_EQUAL, file, line, column, end_line,
                           end_column, message, actual, expected, XR_NULL_VAL, XR_NULL_VAL);
    return XR_NULL_VAL;
}

static inline XrValue xrt_assertion_action(XrValue action,
                                           XrCoreIntrinsicExpectedFailureChannel expected_channel,
                                           const char *file, uint32_t line, uint32_t column,
                                           uint32_t end_line, uint32_t end_column, XrValue message) {
    if (action.tag != XR_TAG_CLOSURE || !action.ptr) {
        xrt_throw_exc(xrt_assertion_literal_exception(
            "typed assertion action is not a closure",
            sizeof("typed assertion action is not a closure") - 1u));
    }
    xrt_closure_t *closure = (xrt_closure_t *) action.ptr;
    if (!closure->callable || !closure->callable->sync_entry)
        xrt_throw_exc(xrt_assertion_literal_exception(
            "typed assertion action has no synchronous entry",
            sizeof("typed assertion action has no synchronous entry") - 1u));

    if (!XR_IS_NULL(xrt_pending_error))
        xrt_throw_exc(xrt_assertion_literal_exception(
            "typed assertion action entered with a pending error",
            sizeof("typed assertion action entered with a pending error") - 1u));
    XrValue action_result = XR_NULL_VAL;
    XrtExcFrame frame;
    frame.prev = xrt_exc_top;
    frame.exception = XR_NULL_VAL;
    xrt_exc_top = &frame;
    bool has_panic = setjmp(frame.buf) != 0;
    if (!has_panic)
        action_result = xrt_closure_call0(action);
    xrt_exc_top = frame.prev;
    XrValue typed_error = xrt_pending_error;
    xrt_pending_error = XR_NULL_VAL;
    XrValue panic = has_panic ? frame.exception : XR_NULL_VAL;
    /* A longjmp makes an automatic modified after setjmp indeterminate.  The
     * panic edge has no returned action result, so it must not inspect or
     * release action_result.  Normal and typed-error returns own the value. */
    if (!has_panic)
        xrt_release(action_result);

    XrAssertionActionOutcome outcome = {
        .returned_normally = !has_panic && XR_IS_NULL(typed_error),
        .has_typed_error = !XR_IS_NULL(typed_error),
        .has_panic = has_panic,
    };
    XrAssertionFailureKind failure_kind = XR_ASSERTION_FAILURE_COUNT;
    if (!xr_assertion_classify_action_channels(expected_channel, outcome, &failure_kind)) {
        xrt_release(typed_error);
        xrt_release(panic);
        xrt_throw_exc(xrt_assertion_literal_exception(
            "invalid typed assertion action outcome",
            sizeof("invalid typed assertion action outcome") - 1u));
    }
    if (failure_kind == XR_ASSERTION_FAILURE_NONE) {
        xrt_release(typed_error);
        xrt_release(panic);
        return XR_NULL_VAL;
    }
    XrValue exception = xrt_assertion_failure_exception(
        failure_kind, file, line, column, end_line, end_column, message, XR_NULL_VAL, XR_NULL_VAL,
        outcome.has_typed_error ? typed_error : XR_NULL_VAL,
        outcome.has_panic ? panic : XR_NULL_VAL);
    xrt_release(typed_error);
    xrt_release(panic);
    xrt_throw_exc(exception);
}

#endif /* XRT_ASSERTION_H */
