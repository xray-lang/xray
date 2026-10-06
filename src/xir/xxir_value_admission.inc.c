/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_value_admission.inc.c - Budgeted iterative authority traversal
 */
typedef struct ValueAdmissionFrame {
    union { XirObject *object; StorageSpan storage; };
    size_t next, count;
    uint32_t begin;
    bool inline_storage;
} ValueAdmissionFrame;
typedef struct ValueAdmissionStack {
    ValueAdmissionFrame *frames;
    size_t depth, capacity, bytes;
} ValueAdmissionStack;
static XrXirValueStatus admission_push(ValueAdmissionStack *stack, ValueAdmissionFrame frame,
    XrXirValueAdmission *admission) {
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
    stack->frames[stack->depth++] = frame;
    return XR_XIR_VALUE_OK;
}
static XrXirValueStatus array_needs_admission(const XirArray *array,
    XrXirValueAdmission *admission, bool *needed) {
    XrXirType type = array->object.type;
    const XrXirTypes *types = xr_xir_compile_type_arena_types(array->object.arena);
    for (;;) {
        if (!admission->work) return XR_XIR_VALUE_LIMIT;
        --admission->work;
        const XrXirTypeNode *node = xr_xir_type_node(types, type);
        if (!node) { *needed = type == XR_XIR_ERROR; return XR_XIR_VALUE_OK; }
        if (node->kind == XR_XIR_TYPE_CALLABLE || node->kind == XR_XIR_TYPE_NOMINAL ||
            node->kind == XR_XIR_TYPE_NULLABLE || node->kind == XR_XIR_TYPE_TUPLE ||
            node->kind == XR_XIR_TYPE_ATOMIC) {
            *needed = true; return XR_XIR_VALUE_OK;
        }
        if (node->kind != XR_XIR_TYPE_ARRAY && node->kind != XR_XIR_TYPE_NULLABLE) return XR_XIR_VALUE_BAD_ARGUMENT;
        type = node->element;
    }
}
static void admission_child(ValueAdmissionFrame *frame, const XrXirTypeArena *arena,
    XrXirValue *value, StorageSpan *span, bool *inlined, bool *tuple_field) {
    XR_CHECK(frame->next < frame->count, "admission cursor exceeded its validated owner");
    size_t index = frame->next++;
    *tuple_field=!frame->inline_storage && frame->object->kind==XR_XIR_TYPE_TUPLE;
    *inlined = frame->inline_storage || frame->object->kind == XR_XIR_TYPE_ARRAY ||
        frame->object->kind==XIR_OBJECT_CLASS;
    if (!frame->inline_storage && frame->object->kind==XIR_OBJECT_CLASS) {
        const XrXirTypeNode *node=xr_xir_type_node(xr_xir_compile_type_arena_types(arena),frame->object->type);
        const XrXirStorageLayout *layout=class_body_layout(frame->object);
        const unsigned char *body=(const unsigned char *)((const XirClassObject *)frame->object+1);
        *span=(StorageSpan){node->nominal.fields[index],body+layout->field_offsets[index]};
    } else if (frame->inline_storage) {
        const XrXirTypeNode *node = xr_xir_type_node(xr_xir_compile_type_arena_types(arena), frame->storage.type);
        const XrXirStorageLayout *layout = xr_xir_compile_type_arena_storage(arena, frame->storage.type);
        uint32_t field = frame->begin + (uint32_t)index;
        *span = (StorageSpan){node->kind == XR_XIR_TYPE_NULLABLE ? node->element : node->nominal.fields[field],
            frame->storage.bytes ? frame->storage.bytes +
                (node->kind == XR_XIR_TYPE_NULLABLE ? layout->value.alignment : layout->field_offsets[field]) : NULL};
    } else if (*inlined) {
        const XirArray *array = (const XirArray *)frame->object;
        *span = (StorageSpan){array->element, array->data ? array->data + index * array->stride : NULL};
    } else if (*tuple_field) *value=((XirTuple *)frame->object)->fields[index];
    else *value = ((XirNominalValue *)frame->object)->fields[index];
}
static XrXirValueStatus admission_inline(ValueAdmissionStack *stack,
    StorageSpan span, XrXirValueAdmission *admission) {
    if (!admission->domain) return XR_XIR_VALUE_BAD_ARGUMENT;
    const XrXirTypes *types = xr_xir_compile_type_arena_types(admission->arena);
    const XrXirTypeNode *node = xr_xir_type_node(types, span.type);
    const XrXirStorageLayout *layout = xr_xir_compile_type_arena_storage(admission->arena, span.type);
    if (!layout || (layout->value.size && !span.bytes)) return XR_XIR_VALUE_BAD_ARGUMENT;
    uint32_t begin = 0, count = node->nominal.field_count, variant = 0;
    if (node->kind == XR_XIR_TYPE_NULLABLE) {
        if (!span.bytes || span.bytes[0] > 1) return XR_XIR_VALUE_BAD_ARGUMENT;
        count = span.bytes[0];
    } else if (types->nominals->identities[node->nominal.declaration].kind == XR_XIR_NOMINAL_ENUM) {
        const XrXirNominalIdentity *identity = &types->nominals->identities[node->nominal.declaration];
        if (layout->tag_bytes) memcpy(&variant, span.bytes, layout->tag_bytes);
        if (variant >= identity->variant_count) return XR_XIR_VALUE_BAD_ARGUMENT;
        begin = identity->variants[variant].field_begin; count = identity->variants[variant].field_count;
    }
    return count ? admission_push(stack, (ValueAdmissionFrame){.storage = span, .count = count,
        .begin = begin, .inline_storage = true}, admission) : XR_XIR_VALUE_OK;
}
XR_FUNC XrXirValueStatus xr_xir_value_admit(const XrXirValue *value, XrXirType type,
    XrXirValueAdmission *admission) {
    if (!admission || !value) return XR_XIR_VALUE_BAD_ARGUMENT;
    ValueAdmissionStack stack = {0}; XrXirValue current = *value;
    StorageSpan span = {0}; bool inlined = false, tuple_field=false;
    XrXirValueStatus status = XR_XIR_VALUE_OK;
    for (;;) {
        if (!admission->work || (admission->domain && !xr_xir_domain_work(admission->domain, 1))) {
            status = XR_XIR_VALUE_LIMIT; break;
        }
        --admission->work;
        if (inlined && inline_nominal_type(xr_xir_compile_type_arena_types(admission->arena), type)) {
            status = admission_inline(&stack, span, admission);
            if (status != XR_XIR_VALUE_OK) break;
        } else {
            if (inlined) {
                XrXirLayout layout = {0};
                if (!xr_xir_compile_type_arena_layout(admission->arena, type, &layout) || !layout.size ||
                    layout.size > sizeof(uint64_t) || !span.bytes) { status = XR_XIR_VALUE_BAD_ARGUMENT; break; }
                current = storage_leaf_value(type, span.bytes, layout.size);
            }
            if (type==XR_XIR_UNIT ? (!tuple_field || inlined || !unit_value(&current)) :
                !xr_xir_value_argument(&current, admission->arena, type)) {
                status = XR_XIR_VALUE_BAD_ARGUMENT; break;
            }
            if (owned_carrier_type(type)) {
                XirObject *object = object_pointer(&current);
                size_t count = 0;
                if (object->kind == XIR_OBJECT_CLASS) {
                    const XrXirStorageLayout *body=class_body_layout(object);
                    if (object->domain != admission->domain || !body) {
                        status=XR_XIR_VALUE_BAD_ARGUMENT;break;
                    }
                    if (body->field_count > admission->work) {status=XR_XIR_VALUE_LIMIT;break;}
                    admission->work-=body->field_count;
                    count=body->field_count;
                } else if (object->kind==XR_XIR_TYPE_TUPLE) {
                    if (!admission->domain) {status=XR_XIR_VALUE_BAD_ARGUMENT;break;}
                    count=((XirTuple *)object)->count;
                } else if (object->kind == XR_XIR_TYPE_NOMINAL || object->kind == XR_XIR_TYPE_NULLABLE) {
                    if (!admission->domain) { status = XR_XIR_VALUE_BAD_ARGUMENT; break; }
                    count = ((XirNominalValue *) object)->count;
                } else if (object->kind == XR_XIR_TYPE_ARRAY) {
                    bool needed = false;
                    status = array_needs_admission((XirArray *) object, admission, &needed);
                    if (status != XR_XIR_VALUE_OK) break;
                    if (needed) count = ((XirArray *) object)->length;
                } else if (object->kind == XR_XIR_TYPE_TASK || object->kind == XR_XIR_TYPE_ATOMIC) {
                    if (!admission->domain) { status = XR_XIR_VALUE_BAD_ARGUMENT; break; }
                } else if (object->kind) {
                    if (object->domain != admission->domain) { status = XR_XIR_VALUE_BAD_ARGUMENT; break; }
                    if (object->kind == XR_XIR_TYPE_CALLABLE) {
                        if (!admission->function) { status = XR_XIR_VALUE_BAD_ARGUMENT; break; }
                        status = admission->function(admission->context,
                            &((XirFunction *) object)->binding, type, &admission->work);
                        if (status != XR_XIR_VALUE_OK) break;
                    } else if (object->kind == XR_XIR_TYPE_ATOMIC) {
                        /* Atomic cells have scalar payloads and no graph children. */
                    } else if (object->kind == XR_XIR_TYPE_CELL) {
                        type = xr_xir_cell_element(xr_xir_compile_type_arena_types(object->arena), type);
                        current = ((XirCell *) object)->value; inlined = false; continue;
                    } else { status = XR_XIR_VALUE_BAD_ARGUMENT; break; }
                }
                if (count) {
                    status = admission_push(&stack, (ValueAdmissionFrame){.object = object, .count = count}, admission);
                    if (status != XR_XIR_VALUE_OK) break;
                }
            }
        }
        while (stack.depth && stack.frames[stack.depth - 1].next == stack.frames[stack.depth - 1].count)
            --stack.depth;
        if (!stack.depth) break;
        admission_child(&stack.frames[stack.depth - 1], admission->arena, &current, &span, &inlined, &tuple_field);
        type = inlined ? span.type : (XrXirType)current.type;
    }
    if (stack.frames) {
        xr_xir_domain_deallocate(admission->domain, stack.frames, stack.bytes);
        admission->scratch_bytes += stack.bytes;
    }
    return status;
}
