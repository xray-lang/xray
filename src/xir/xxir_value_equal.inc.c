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
#include "xxir_scalar_core.h"

typedef struct ValueEqualFrame {
    const XirArray *left, *right;
    size_t next;
    XrXirType element;
} ValueEqualFrame;
_Static_assert(sizeof(ValueEqualFrame)<=32,"equality keeps its original bounded frame footprint");
typedef struct ValueEqualStack {
    ValueEqualFrame *frames;
    size_t depth, capacity, bytes;
} ValueEqualStack;
typedef struct ValueEqualCursor {
    XrXirValue left, right;
    StorageSpan left_span, right_span;
    const XrXirTypeArena *left_arena,*right_arena;
    XrXirType type;
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
    } else if (type == XR_XIR_BOOL || xr_xir_type_is_integer(type)) {
        if (xir_scalar_core_equal(type,left->payload,right->payload,equal) != XR_XIR_RUN_OK)
            return XR_XIR_VALUE_BAD_ARGUMENT;
    } else if (type == XR_XIR_RUNE) *equal = left->payload == right->payload;
    else return XR_XIR_VALUE_BAD_ARGUMENT;
    return XR_XIR_VALUE_OK;
}
static XrXirValueStatus equal_current(ValueEqualCursor *cursor, ValueEqualStack *stack,
    XrXirValueAdmission *admission, bool *equal) {
    XrXirValue *left=&cursor->left,*right=&cursor->right;
    StorageSpan *a_span=&cursor->left_span,*b_span=&cursor->right_span;
    bool *inlined=&cursor->inlined,*descend=&cursor->descend;
    const XrXirTypes *types = xr_xir_compile_type_arena_types(admission->arena);
    XrXirType type=cursor->type;
    *descend = false;
    if (*inlined && (!xir_type_arena_same_type(cursor->left_arena,a_span->type,admission->arena,type) ||
        !xir_type_arena_same_type(cursor->right_arena,b_span->type,admission->arena,type)))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    if (*inlined && xr_xir_type_is_nullable(types, type)) {
        const XrXirTypes *a_types=xr_xir_compile_type_arena_types(cursor->left_arena);
        const XrXirTypes *b_types=xr_xir_compile_type_arena_types(cursor->right_arena);
        const XrXirStorageLayout *a_layout=xr_xir_compile_type_arena_storage(cursor->left_arena,a_span->type);
        const XrXirStorageLayout *b_layout=xr_xir_compile_type_arena_storage(cursor->right_arena,b_span->type);
        if (!a_layout || !b_layout || !xr_xir_type_is_nullable(a_types,a_span->type) ||
            !xr_xir_type_is_nullable(b_types,b_span->type) || !a_span->bytes || !b_span->bytes ||
            a_span->bytes[0] > 1 || b_span->bytes[0] > 1) return XR_XIR_VALUE_BAD_ARGUMENT;
        if (a_span->bytes[0] != b_span->bytes[0]) { *equal = false; return XR_XIR_VALUE_OK; }
        if (a_span->bytes[0]) {
            cursor->type=xr_xir_nullable_element(types,type);
            a_span->type=xr_xir_nullable_element(a_types,a_span->type);
            b_span->type=xr_xir_nullable_element(b_types,b_span->type);
            a_span->bytes+=a_layout->value.alignment;b_span->bytes+=b_layout->value.alignment;
            *descend = true;
        }
        return XR_XIR_VALUE_OK;
    }
    if (*inlined) {
        XrXirLayout a_layout={0},b_layout={0};
        if (!xr_xir_compile_type_arena_layout(cursor->left_arena,a_span->type,&a_layout) ||
            !xr_xir_compile_type_arena_layout(cursor->right_arena,b_span->type,&b_layout) ||
            !a_layout.size || a_layout.size>sizeof(uint64_t) || !b_layout.size ||
            b_layout.size>sizeof(uint64_t) || !a_span->bytes || !b_span->bytes)
            return XR_XIR_VALUE_BAD_ARGUMENT;
        if (!storage_leaf_value(cursor->left_arena,a_span->type,a_span->bytes,a_layout.size,left) ||
            !storage_leaf_value(cursor->right_arena,b_span->type,b_span->bytes,b_layout.size,right))
            return XR_XIR_VALUE_BAD_ARGUMENT;

    }
    if (!xr_xir_value_argument(left, admission->arena, type) ||
        !xr_xir_value_argument(right, admission->arena, type)) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (xr_xir_type_is_nullable(types,type)) {
        const XirNominalValue *a = (const XirNominalValue *)object_pointer(left);
        const XirNominalValue *b = (const XirNominalValue *)object_pointer(right);
        if (a->variant != b->variant) { *equal = false; return XR_XIR_VALUE_OK; }
        if (a->count) {
            *left=a->fields[0];*right=b->fields[0];cursor->type=xr_xir_nullable_element(types,type);
            *inlined=false;*descend=true;
        }
        return XR_XIR_VALUE_OK;
    }
    if (!xr_xir_type_is_array(types,type)) return equal_leaf(left,right,admission,equal);
    const XirArray *a = (const XirArray *)object_pointer(left);
    const XirArray *b = (const XirArray *)object_pointer(right);
    if (a->length != b->length) { *equal = false; return XR_XIR_VALUE_OK; }
    return a->length ? equal_push(stack,(ValueEqualFrame){a,b,0,xr_xir_array_element(types,type)},admission) : XR_XIR_VALUE_OK;
}
static void equal_child(ValueEqualFrame *frame,ValueEqualCursor *cursor) {
    XR_CHECK(frame->next<frame->left->length,"comparison cursor exceeded validated active children");
    size_t index = frame->next++;
    cursor->inlined=true;cursor->type=frame->element;
    cursor->left_arena=frame->left->object.arena;cursor->right_arena=frame->right->object.arena;
    cursor->left_span=(StorageSpan){frame->left->element,frame->left->data+index*frame->left->stride};
    cursor->right_span=(StorageSpan){frame->right->element,frame->right->data+index*frame->right->stride};
}
static XrXirValueStatus xr_xir_value_equal_graph_operation(const XrXirValue *left, const XrXirValue *right,
    XrXirType type, XrXirValueAdmission *admission, bool *output) {
    if (!left || !right || !admission || !admission->domain || !output ||
        !xr_xir_value_argument(left,admission->arena,type) || !xr_xir_value_argument(right,admission->arena,type))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValueStatus status = equal_closed_type(type, admission);
    if (status != XR_XIR_VALUE_OK) return status;
    ValueEqualStack stack = {0};
    ValueEqualCursor cursor={.left=*left,.right=*right,.type=type};
    bool equal = true;
    for (;;) {
        if (!equal_work(admission, 1)) { status = XR_XIR_VALUE_LIMIT; break; }
        status = equal_current(&cursor,&stack,admission,&equal);
        if (status != XR_XIR_VALUE_OK || !equal) break;
        if (cursor.descend) continue;
        while (stack.depth && stack.frames[stack.depth-1].next==stack.frames[stack.depth-1].left->length)
            --stack.depth;
        if (!stack.depth) break;
        equal_child(&stack.frames[stack.depth-1],&cursor);
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
    if (!admission_domain_current(admission)) return XR_XIR_VALUE_BAD_ARGUMENT;
    xr_xir_value_graph_begin();
    XrXirValueStatus graph_outcome = xr_xir_value_equal_graph_operation(left, right, type, admission, output);
    xr_xir_value_graph_end();
    return graph_outcome;
}
