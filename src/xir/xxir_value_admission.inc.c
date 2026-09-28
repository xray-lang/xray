/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_value_admission.inc.c - Budgeted iterative authority traversal
 */
typedef struct ValueAdmissionFrame {
    XirObject *object;
    size_t next, count;
} ValueAdmissionFrame;
typedef struct ValueAdmissionStack {
    ValueAdmissionFrame *frames;
    size_t depth, capacity, bytes;
} ValueAdmissionStack;
static XrXirValueStatus admission_push(ValueAdmissionStack *stack, XirObject *object,
    size_t count, XrXirValueAdmission *admission) {
    if (stack->depth == stack->capacity) {
        if (stack->capacity > SIZE_MAX / 2 / sizeof(*stack->frames)) return XR_XIR_VALUE_LIMIT;
        size_t capacity = stack->capacity ? stack->capacity * 2 : 1;
        size_t bytes = capacity * sizeof(*stack->frames);
        if (bytes > admission->scratch_bytes) return XR_XIR_VALUE_LIMIT;
        if (!admission->domain) return XR_XIR_VALUE_BAD_ARGUMENT;
        XrXirValueStatus status = XR_XIR_VALUE_OK;
        ValueAdmissionFrame *frames = xr_xir_domain_allocate(admission->domain, bytes, &status);
        if (!frames) return status;
        admission->scratch_bytes -= bytes;
        if (stack->frames) {
            memcpy(frames, stack->frames, stack->depth * sizeof(*frames));
            xr_xir_domain_deallocate(admission->domain, stack->frames, stack->bytes);
            admission->scratch_bytes += stack->bytes;
        }
        stack->frames = frames; stack->capacity = capacity; stack->bytes = bytes;
    }
    stack->frames[stack->depth++] = (ValueAdmissionFrame) {object, 0, count};
    return XR_XIR_VALUE_OK;
}
static XrXirValueStatus array_needs_admission(const XirArray *array,
    XrXirValueAdmission *admission, bool *needed) {
    XrXirType type = array->object.type;
    const XrXirTypes *types = xr_xir_type_arena_types(array->object.arena);
    for (;;) {
        if (!admission->work) return XR_XIR_VALUE_LIMIT;
        --admission->work;
        const XrXirTypeNode *node = xr_xir_type_node(types, type);
        if (!node) { *needed = type == XR_XIR_ERROR; return XR_XIR_VALUE_OK; }
        if (node->kind == XR_XIR_TYPE_CALLABLE || node->kind == XR_XIR_TYPE_NOMINAL) {
            *needed = true; return XR_XIR_VALUE_OK;
        }
        if (node->kind != XR_XIR_TYPE_ARRAY) return XR_XIR_VALUE_BAD_ARGUMENT;
        type = node->element;
    }
}
static XrXirValue admission_child(ValueAdmissionFrame *frame) {
    XR_CHECK(frame->next < frame->count, "admission cursor exceeded its validated owner");
    size_t index = frame->next++;
    return frame->object->kind == XR_XIR_TYPE_ARRAY ?
        array_element_value((XirArray *) frame->object, index) :
        ((XirNominalValue *) frame->object)->fields[index];
}
XR_FUNC XrXirValueStatus xr_xir_value_admit(const XrXirValue *value, XrXirType type,
    XrXirValueAdmission *admission) {
    if (!admission || !value) return XR_XIR_VALUE_BAD_ARGUMENT;
    ValueAdmissionStack stack = {0}; XrXirValue current = *value;
    XrXirValueStatus status = XR_XIR_VALUE_OK;
    for (;;) {
        if (!admission->work) { status = XR_XIR_VALUE_LIMIT; break; }
        --admission->work;
        if (!xr_xir_value_argument(&current, admission->arena, type)) {
            status = XR_XIR_VALUE_BAD_ARGUMENT; break;
        }
        if (owned_carrier_type(type)) {
            XirObject *object = object_pointer(&current);
            size_t count = 0;
            if (object->kind == XR_XIR_TYPE_NOMINAL) {
                if (!admission->domain) { status = XR_XIR_VALUE_BAD_ARGUMENT; break; }
                count = ((XirNominalValue *) object)->count;
            } else if (object->kind == XR_XIR_TYPE_ARRAY) {
                bool needed = false;
                status = array_needs_admission((XirArray *) object, admission, &needed);
                if (status != XR_XIR_VALUE_OK) break;
                if (needed) count = ((XirArray *) object)->length;
            } else if (object->kind) {
                if (object->domain != admission->domain) { status = XR_XIR_VALUE_BAD_ARGUMENT; break; }
                if (object->kind == XR_XIR_TYPE_CALLABLE) {
                    if (!admission->function) { status = XR_XIR_VALUE_BAD_ARGUMENT; break; }
                    status = admission->function(admission->context,
                        &((XirFunction *) object)->binding, type, &admission->work);
                    if (status != XR_XIR_VALUE_OK) break;
                } else if (object->kind == XR_XIR_TYPE_CELL) {
                    type = xr_xir_cell_element(xr_xir_type_arena_types(object->arena), type);
                    current = ((XirCell *) object)->value; continue;
                } else { status = XR_XIR_VALUE_BAD_ARGUMENT; break; }
            }
            if (count) {
                status = admission_push(&stack, object, count, admission);
                if (status != XR_XIR_VALUE_OK) break;
            }
        }
        while (stack.depth && stack.frames[stack.depth - 1].next == stack.frames[stack.depth - 1].count)
            --stack.depth;
        if (!stack.depth) break;
        current = admission_child(&stack.frames[stack.depth - 1]); type = (XrXirType) current.type;
    }
    if (stack.frames) {
        xr_xir_domain_deallocate(admission->domain, stack.frames, stack.bytes);
        admission->scratch_bytes += stack.bytes;
    }
    return status;
}
