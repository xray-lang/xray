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
typedef struct ValueEqualCursor {
    XrXirValue left, right;
    StorageSpan left_span, right_span;
    bool inlined, descend;
} ValueEqualCursor;
static bool equal_work(XrXirValueAdmission *admission, uint64_t count) {
    if (count > admission->work) return false;
    admission->work -= count;
    return true;
}
static XrXirValueStatus equal_closed_type(XrXirType type, XrXirValueAdmission *admission) {
    const XrXirTypes *types = xr_xir_compile_type_arena_types(admission->arena);
    for (;;) {
        if (!equal_work(admission, 1)) return XR_XIR_VALUE_LIMIT;
        if (type == XR_XIR_BOOL || type == XR_XIR_RUNE || xr_xir_type_is_number(type) || type == XR_XIR_STRING)
            return XR_XIR_VALUE_OK;
        const XrXirTypeNode *node = xr_xir_type_node(types, type);
        if (!node || (node->kind != XR_XIR_TYPE_ARRAY && node->kind != XR_XIR_TYPE_NULLABLE) || node->parameter_span)
            return XR_XIR_VALUE_BAD_ARGUMENT;
        type = node->element;
    }
}
static XrXirValueStatus equal_push(ValueEqualStack *stack, ValueEqualFrame frame,
    XrXirValueAdmission *admission) {
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
    stack->frames[stack->depth++] = frame;
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
    } else if ((type == XR_XIR_BOOL || type == XR_XIR_RUNE)) *equal = left->payload == right->payload;
    else return XR_XIR_VALUE_BAD_ARGUMENT;
    return XR_XIR_VALUE_OK;
}
static XrXirValueStatus equal_current(ValueEqualCursor *cursor, ValueEqualStack *stack,
    XrXirValueAdmission *admission, bool *equal) {
    XrXirValue *left=&cursor->left,*right=&cursor->right;
    StorageSpan *a_span=&cursor->left_span,*b_span=&cursor->right_span;
    bool *inlined=&cursor->inlined,*descend=&cursor->descend;
    const XrXirTypes *types = xr_xir_compile_type_arena_types(admission->arena);
    XrXirType type = *inlined ? a_span->type : (XrXirType)left->type;
    *descend = false;
    if (*inlined && xr_xir_type_is_nullable(types, type)) {
        const XrXirStorageLayout *layout = xr_xir_compile_type_arena_storage(admission->arena, type);
        if (b_span->type != type || !layout || !a_span->bytes || !b_span->bytes ||
            a_span->bytes[0] > 1 || b_span->bytes[0] > 1) return XR_XIR_VALUE_BAD_ARGUMENT;
        if (a_span->bytes[0] != b_span->bytes[0]) { *equal = false; return XR_XIR_VALUE_OK; }
        if (a_span->bytes[0]) {
            XrXirType element = xr_xir_nullable_element(types,type);
            a_span->type = b_span->type = element;
            a_span->bytes += layout->value.alignment; b_span->bytes += layout->value.alignment;
            *descend = true;
        }
        return XR_XIR_VALUE_OK;
    }
    if (*inlined) {
        XrXirLayout layout = {0};
        if (b_span->type != type || !xr_xir_compile_type_arena_layout(admission->arena,type,&layout) ||
            !layout.size || layout.size > sizeof(uint64_t) || !a_span->bytes || !b_span->bytes)
            return XR_XIR_VALUE_BAD_ARGUMENT;
        *left = storage_leaf_value(type,a_span->bytes,layout.size);
        *right = storage_leaf_value(type,b_span->bytes,layout.size);
    }
    if (right->type != left->type || !xr_xir_value_argument(left, admission->arena, type) ||
        !xr_xir_value_argument(right, admission->arena, type)) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (xr_xir_type_is_nullable(types,type)) {
        const XirNominalValue *a = (const XirNominalValue *)object_pointer(left);
        const XirNominalValue *b = (const XirNominalValue *)object_pointer(right);
        if (a->variant != b->variant) { *equal = false; return XR_XIR_VALUE_OK; }
        if (a->count) { *left = a->fields[0]; *right = b->fields[0]; *inlined = false; *descend = true; }
        return XR_XIR_VALUE_OK;
    }
    if (!xr_xir_type_is_array(types,type)) return equal_leaf(left,right,admission,equal);
    const XirArray *a = (const XirArray *)object_pointer(left);
    const XirArray *b = (const XirArray *)object_pointer(right);
    if (a->length != b->length) { *equal = false; return XR_XIR_VALUE_OK; }
    return a->length ? equal_push(stack,(ValueEqualFrame){a,b,0,a->length},admission) : XR_XIR_VALUE_OK;
}
static void equal_child(ValueEqualFrame *frame,
    StorageSpan *a_span, StorageSpan *b_span, bool *inlined) {
    XR_CHECK(frame->next < frame->count, "comparison cursor exceeded validated active children");
    size_t index = frame->next++;
    *inlined = true;
    *a_span = (StorageSpan){frame->left->element,frame->left->data + index * frame->left->stride};
    *b_span = (StorageSpan){frame->right->element,frame->right->data + index * frame->right->stride};
}
static XrXirValueStatus xr_xir_value_equal_graph_operation(const XrXirValue *left, const XrXirValue *right,
    XrXirType type, XrXirValueAdmission *admission, bool *output) {
    if (!left || !right || !admission || !admission->domain || !output ||
        left->type != (uint32_t)type || right->type != (uint32_t)type) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValueStatus status = equal_closed_type(type, admission);
    if (status != XR_XIR_VALUE_OK) return status;
    ValueEqualStack stack = {0};
    ValueEqualCursor cursor = {*left,*right,{0},{0},false,false};
    bool equal = true;
    for (;;) {
        if (!equal_work(admission, 1)) { status = XR_XIR_VALUE_LIMIT; break; }
        status = equal_current(&cursor,&stack,admission,&equal);
        if (status != XR_XIR_VALUE_OK || !equal) break;
        if (cursor.descend) continue;
        while (stack.depth && stack.frames[stack.depth - 1].next == stack.frames[stack.depth - 1].count)
            --stack.depth;
        if (!stack.depth) break;
        equal_child(&stack.frames[stack.depth - 1],&cursor.left_span,&cursor.right_span,&cursor.inlined);
    }
    if (stack.frames) {
        xr_xir_domain_deallocate(admission->domain, stack.frames, stack.bytes);
        admission->scratch_bytes += stack.bytes;
    }
    if (status == XR_XIR_VALUE_OK) *output = equal;
    return status;
}
XR_FUNC XrXirValueStatus xr_xir_value_equal(const XrXirValue *left, const XrXirValue *right,
    XrXirType type, XrXirValueAdmission *admission, bool *output) {
    xr_xir_value_graph_begin();
    XrXirValueStatus graph_outcome = xr_xir_value_equal_graph_operation(left, right, type, admission, output);
    xr_xir_value_graph_end();
    return graph_outcome;
}
