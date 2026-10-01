/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_value_equal.inc.c - Explicit-stack value equality over a finite type domain
 *
 * KEY CONCEPT:
 *   Shared array storage does not prove reflexivity of its floating leaves.
 */
typedef struct ValueEqualFrame {
    const XirArray *left, *right;
    size_t next, count;
} ValueEqualFrame;
typedef struct ValueEqualStack {
    ValueEqualFrame *frames;
    size_t depth, capacity, bytes;
} ValueEqualStack;
static bool equal_work(XrXirValueAdmission *admission, uint64_t count) {
    if (count > admission->work) return false;
    admission->work -= count;
    return true;
}
static XrXirValueStatus equal_closed_type(XrXirType type, XrXirValueAdmission *admission) {
    const XrXirTypes *types = xr_xir_type_arena_types(admission->arena);
    for (;;) {
        if (!equal_work(admission, 1)) return XR_XIR_VALUE_LIMIT;
        if (type == XR_XIR_BOOL || xr_xir_type_is_number(type) || type == XR_XIR_STRING)
            return XR_XIR_VALUE_OK;
        const XrXirTypeNode *node = xr_xir_type_node(types, type);
        if (!node || node->kind != XR_XIR_TYPE_ARRAY || node->parameter_span)
            return XR_XIR_VALUE_BAD_ARGUMENT;
        type = node->element;
    }
}
static XrXirValueStatus equal_push(ValueEqualStack *stack, const XirArray *left,
    const XirArray *right, XrXirValueAdmission *admission) {
    if (stack->depth == stack->capacity) {
        if (stack->capacity > SIZE_MAX / 2 / sizeof(*stack->frames)) return XR_XIR_VALUE_LIMIT;
        size_t capacity = stack->capacity ? stack->capacity * 2 : 1;
        size_t bytes = capacity * sizeof(*stack->frames);
        if (bytes > admission->scratch_bytes) return XR_XIR_VALUE_LIMIT;
        if (!equal_work(admission, (uint64_t)stack->depth + 1)) return XR_XIR_VALUE_LIMIT;
        XrXirValueStatus status = XR_XIR_VALUE_OK;
        ValueEqualFrame *frames = xr_xir_domain_allocate(admission->domain, bytes, &status);
        if (!frames) return status;
        admission->scratch_bytes -= bytes;
        if (stack->frames) {
            memcpy(frames, stack->frames, stack->depth * sizeof(*frames));
            xr_xir_domain_deallocate(admission->domain, stack->frames, stack->bytes);
            admission->scratch_bytes += stack->bytes;
        }
        stack->frames = frames; stack->capacity = capacity; stack->bytes = bytes;
    }
    stack->frames[stack->depth++] = (ValueEqualFrame){left, right, 0, left->length};
    return XR_XIR_VALUE_OK;
}
static XrXirValueStatus equal_leaf(const XrXirValue *left, const XrXirValue *right,
    XrXirValueAdmission *admission, bool *equal) {
    XrXirType type = (XrXirType)left->type;
    if (type == XR_XIR_STRING) {
        const XirString *a = string_pointer(left), *b = string_pointer(right);
        if (a->length != b->length) { *equal = false; return XR_XIR_VALUE_OK; }
        if (!equal_work(admission, a->length)) return XR_XIR_VALUE_LIMIT;
        *equal = !a->length || !memcmp(a->bytes, b->bytes, a->length);
    } else if (xr_xir_float_bits(type)) {
        int64_t result = 0;
        if (xr_xir_float_relation(type, XR_XIR_FLOAT_EQ, left->payload, right->payload, &result) != XR_XIR_RUN_OK)
            return XR_XIR_VALUE_BAD_ARGUMENT;
        *equal = result != 0;
    } else if (xr_xir_type_is_integer(type)) {
        int ordering = 0;
        if (xr_xir_integer_compare(xr_xir_integer_format(type), left->payload, right->payload, &ordering) != XR_XIR_RUN_OK)
            return XR_XIR_VALUE_BAD_ARGUMENT;
        *equal = ordering == 0;
    } else if (type == XR_XIR_BOOL) *equal = left->payload == right->payload;
    else return XR_XIR_VALUE_BAD_ARGUMENT;
    return XR_XIR_VALUE_OK;
}
static XrXirValueStatus equal_current(const XrXirValue *left, const XrXirValue *right,
    ValueEqualStack *stack, XrXirValueAdmission *admission, bool *equal) {
    XrXirType type = (XrXirType)left->type;
    if (right->type != left->type || !xr_xir_value_argument(left, admission->arena, type) ||
        !xr_xir_value_argument(right, admission->arena, type)) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!xr_xir_type_is_array(xr_xir_type_arena_types(admission->arena), type))
        return equal_leaf(left, right, admission, equal);
    const XirArray *a = (const XirArray *)object_pointer(left);
    const XirArray *b = (const XirArray *)object_pointer(right);
    if (a->length != b->length) { *equal = false; return XR_XIR_VALUE_OK; }
    return a->length ? equal_push(stack, a, b, admission) : XR_XIR_VALUE_OK;
}
static void equal_child(ValueEqualFrame *frame, XrXirValue *left, XrXirValue *right) {
    XR_CHECK(frame->next < frame->count, "comparison cursor exceeded validated array length");
    size_t index = frame->next++;
    *left = storage_leaf_value(frame->left->element,
        frame->left->data + index * frame->left->stride, frame->left->stride);
    *right = storage_leaf_value(frame->right->element,
        frame->right->data + index * frame->right->stride, frame->right->stride);
}
XR_FUNC XrXirValueStatus xr_xir_value_equal(const XrXirValue *left, const XrXirValue *right,
    XrXirType type, XrXirValueAdmission *admission, bool *output) {
    if (!left || !right || !admission || !admission->domain || !output ||
        left->type != (uint32_t)type || right->type != (uint32_t)type) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValueStatus status = equal_closed_type(type, admission);
    if (status != XR_XIR_VALUE_OK) return status;
    ValueEqualStack stack = {0};
    XrXirValue a = *left, b = *right;
    bool equal = true;
    for (;;) {
        if (!equal_work(admission, 1)) { status = XR_XIR_VALUE_LIMIT; break; }
        status = equal_current(&a, &b, &stack, admission, &equal);
        if (status != XR_XIR_VALUE_OK || !equal) break;
        while (stack.depth && stack.frames[stack.depth - 1].next == stack.frames[stack.depth - 1].count)
            --stack.depth;
        if (!stack.depth) break;
        equal_child(&stack.frames[stack.depth - 1], &a, &b);
    }
    if (stack.frames) {
        xr_xir_domain_deallocate(admission->domain, stack.frames, stack.bytes);
        admission->scratch_bytes += stack.bytes;
    }
    if (status == XR_XIR_VALUE_OK) *output = equal;
    return status;
}
