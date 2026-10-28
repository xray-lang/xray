/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_value_admission.inc.c - Budgeted iterative authority traversal
 */
typedef union ValueAdmissionFrame {
    struct {
        bool inline_storage;
        XirObject *object;
        size_t next,count;
    } boxed;
    struct {
        bool inline_storage;
        XrXirType type;
        const unsigned char *bytes;
        const XrXirTypeArena *arena;
        uint32_t begin,next,count;
    } packed;
} ValueAdmissionFrame;
/* Both alternatives share their initial discriminator. Object owners already
 * retain their arena; only inline fields need a separate coordinate. Closed
 * field ordinals fit uint32_t while materialized Array indices remain size_t. */
XR_STATIC_ASSERT(sizeof(ValueAdmissionFrame)<=40,"admission frames preserve the original bounded scratch footprint");
static size_t admission_frame_next(const ValueAdmissionFrame *frame) {
    return frame->boxed.inline_storage ? frame->packed.next : frame->boxed.next;
}
static size_t admission_frame_count(const ValueAdmissionFrame *frame) {
    return frame->boxed.inline_storage ? frame->packed.count : frame->boxed.count;
}
static const XrXirTypeArena *admission_frame_arena(const ValueAdmissionFrame *frame) {
    if (frame->boxed.inline_storage) return frame->packed.arena;
    if (frame->boxed.object->type==XR_XIR_PANIC_INFO) {
        const XrXirPanicPayload *panic=&((const XirPanicInfo *)frame->boxed.object)->panic;
        return xr_xir_value_arena(!unit_value(&panic->error)?&panic->error:
            !unit_value(&panic->replaced_error)?&panic->replaced_error:&panic->message);
    }
    return frame->boxed.object->arena;
}

typedef struct ValueAdmissionStack {
    ValueAdmissionFrame *frames;
    size_t depth, capacity, bytes;
    ValueAdmissionFrame first;
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
        ValueAdmissionFrame *frames;
        if (!stack->capacity) {
            if (!xr_xir_domain_current(admission->domain)) return XR_XIR_VALUE_BAD_ARGUMENT;
            /* A call-local first frame still reserves the original scratch and
             * pays the same owner's push work. Only its heap request disappears. */
            if (!xr_xir_domain_is_compile(admission->domain) && (!xr_xir_domain_work(admission->domain, 1))) return XR_XIR_VALUE_LIMIT;
            frames = &stack->first;
        } else {
            frames = xr_xir_domain_allocate(admission->domain, bytes, &status);
            if (!frames) return status;
        }
        admission->scratch_bytes -= bytes;
        if (stack->frames) {
            memcpy(frames, stack->frames, stack->depth * sizeof(*frames));
            if (stack->frames != &stack->first)
                xr_xir_domain_deallocate(admission->domain, stack->frames, stack->bytes);
            admission->scratch_bytes += stack->bytes;
        }
        stack->frames = frames; stack->capacity = capacity; stack->bytes = bytes;
    }
    stack->frames[stack->depth++] = frame;
    return XR_XIR_VALUE_OK;
}
static XrXirValueStatus type_needs_admission(const XrXirTypes *types, XrXirType type,
    XrXirValueAdmission *admission, bool *needed) {
    for (;;) {
        if (!admission_is_compile(admission) && (!admission->work)) return XR_XIR_VALUE_LIMIT;
        if (!admission_is_compile(admission)) --admission->work;
        const XrXirTypeNode *node = xr_xir_type_node(types, type);
        if (!node) { *needed = type == XR_XIR_ERROR || type==XR_XIR_PANIC_INFO; return XR_XIR_VALUE_OK; }
        if (node->kind == XR_XIR_TYPE_CALLABLE || node->kind == XR_XIR_TYPE_NOMINAL ||
            node->kind == XR_XIR_TYPE_NULLABLE || node->kind == XR_XIR_TYPE_TUPLE ||
            node->kind == XR_XIR_TYPE_ATOMIC || node->kind == XR_XIR_TYPE_TASK ||
            (node->kind == XR_XIR_TYPE_ITERATOR || node->kind == XR_XIR_TYPE_CHANNEL)) {
            *needed = true; return XR_XIR_VALUE_OK;
        }
        if (node->kind == XR_XIR_TYPE_SET) { *needed = false; return XR_XIR_VALUE_OK; }
        /* Nullable adds no execution authority beyond its element type. */
        if (node->kind != XR_XIR_TYPE_ARRAY && node->kind != XR_XIR_TYPE_MAP &&
            node->kind != XR_XIR_TYPE_NULLABLE) return XR_XIR_VALUE_BAD_ARGUMENT;
        type = node->kind == XR_XIR_TYPE_MAP ? node->result : node->element;
    }
}
static void admission_child(ValueAdmissionFrame *frame, const XrXirTypeArena *arena,
    XrXirValue *value, StorageSpan *span, bool *inlined, bool *tuple_field) {
    XR_CHECK(admission_frame_next(frame)<admission_frame_count(frame), "admission cursor exceeded its validated owner");
    size_t index=frame->boxed.inline_storage ? frame->packed.next++ : frame->boxed.next++;
    *tuple_field=!frame->boxed.inline_storage && frame->boxed.object->kind==XR_XIR_TYPE_TUPLE;
    *inlined = frame->boxed.inline_storage || frame->boxed.object->kind == XR_XIR_TYPE_ARRAY ||
        frame->boxed.object->kind==XIR_OBJECT_CLASS;
    if (!frame->boxed.inline_storage && frame->boxed.object->type==XR_XIR_PANIC_INFO) {
        const XrXirPanicPayload *panic=&((const XirPanicInfo *)frame->boxed.object)->panic;
        const XrXirValue *edges[]={&panic->message,&panic->error,&panic->replaced_error};
        size_t at=0;
        for (size_t i=0;i<3;++i) if (!unit_value(edges[i]) && at++==index) { *value=*edges[i];break; }
    } else if (!frame->boxed.inline_storage && frame->boxed.object->kind==XIR_OBJECT_CLASS) {
        const XrXirTypeNode *node=xr_xir_type_node(xr_xir_compile_type_arena_types(arena),frame->boxed.object->type);
        const XrXirStorageLayout *layout=class_body_layout(frame->boxed.object);
        const unsigned char *body=(const unsigned char *)((const XirClassObject *)frame->boxed.object+1);
        *span=(StorageSpan){node->nominal.fields[index],body+layout->field_offsets[index]};
    } else if (frame->boxed.inline_storage) {
        const XrXirTypeNode *node = xr_xir_type_node(xr_xir_compile_type_arena_types(arena), frame->packed.type);
        const XrXirStorageLayout *layout = xr_xir_compile_type_arena_storage(arena, frame->packed.type);
        uint32_t field = frame->packed.begin + (uint32_t)index;
        *span = (StorageSpan){node->kind == XR_XIR_TYPE_NULLABLE ? node->element : node->nominal.fields[field],
            frame->packed.bytes ? frame->packed.bytes +
                (node->kind == XR_XIR_TYPE_NULLABLE ? layout->value.alignment : layout->field_offsets[field]) : NULL};
    } else if (frame->boxed.object->kind == XR_XIR_TYPE_MAP || frame->boxed.object->kind == XR_XIR_TYPE_SET) {
        const XirHash *map = (const XirHash *)frame->boxed.object;
        bool map_value = map->object.kind == XR_XIR_TYPE_MAP && (index & 1);
        *span = hash_span(map, map->object.kind == XR_XIR_TYPE_MAP ? index / 2 : index, map_value);
        *inlined = span->type != XR_XIR_UNIT;
        if (!*inlined) { *tuple_field = true; *value = (XrXirValue){0}; }
    } else if (*inlined) {
        const XirArray *array = (const XirArray *)frame->boxed.object;
        *span = (StorageSpan){array->element, array->data ? array->data + index * array->stride : NULL};
    } else if (*tuple_field) *value=((XirTuple *)frame->boxed.object)->fields[index];
    else *value = ((XirNominalValue *)frame->boxed.object)->fields[index];
    const XrXirTypes *types=xr_xir_compile_type_arena_types(arena);
    const XrXirTypeNode *parent=xr_xir_type_node(types,frame->boxed.inline_storage?
        frame->packed.type:frame->boxed.object->type);
    bool unit_nullable=parent && parent->kind==XR_XIR_TYPE_NULLABLE && parent->element==XR_XIR_UNIT;
    if (frame->boxed.inline_storage && span->type==XR_XIR_UNIT &&
        (unit_nullable || xr_xir_nominal_native_recv(types,frame->packed.type,XR_XIR_UNIT))) {
        *value=(XrXirValue){0}; *inlined=false; *tuple_field=true;
    } else if (!frame->boxed.inline_storage && !*inlined && value->type==XR_XIR_UNIT &&
        (unit_nullable || xr_xir_nominal_native_recv(types,frame->boxed.object->type,XR_XIR_UNIT))) *tuple_field=true;
}
static XrXirValueStatus admission_inline(ValueAdmissionStack *stack,
    StorageSpan span,const XrXirTypeArena *arena,XrXirValueAdmission *admission) {
    if (!admission->domain) return XR_XIR_VALUE_BAD_ARGUMENT;
    const XrXirTypes *types = xr_xir_compile_type_arena_types(arena);
    const XrXirTypeNode *node = xr_xir_type_node(types, span.type);
    const XrXirStorageLayout *layout = xr_xir_compile_type_arena_storage(arena, span.type);
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
    return count ? admission_push(stack, (ValueAdmissionFrame){.packed={true,span.type,span.bytes,arena,begin,0,count}}, admission) : XR_XIR_VALUE_OK;
}
/* Ownership summary shares the authority walk and its existing work/scratch
 * charges. Empty or pure scalar/String aggregates need no foreign detachment. */
static XrXirValueStatus value_admit_summary_mode(const XrXirValue *value, XrXirType type,
    XrXirValueAdmission *admission, bool *domain_sensitive, bool task) {
    if (!admission_domain_current(admission)) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (domain_sensitive) *domain_sensitive = false;
    if (!admission || !value) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (xr_xir_compile_type_arena_borrowed(admission->arena,type)) return XR_XIR_VALUE_BAD_ARGUMENT;
    ValueAdmissionStack stack = {0}; XrXirValue current = *value;
    const XrXirTypeArena *arena=admission->arena;
    StorageSpan span = {0}; bool inlined = false, tuple_field=false;
    XrXirValueStatus status = XR_XIR_VALUE_OK;
    for (;;) {
        if (!admission_is_compile(admission) && (!admission->work || (admission->domain && !xr_xir_domain_work(admission->domain, 1)))) {
            status = XR_XIR_VALUE_LIMIT; break;
        }
        if (!admission_is_compile(admission)) --admission->work;
        if (inlined && inline_nominal_type(xr_xir_compile_type_arena_types(arena), type)) {
            status = admission_inline(&stack, span,arena,admission);
            if (status != XR_XIR_VALUE_OK) break;
        } else {
            if (inlined) {
                XrXirLayout layout = {0};
                if (!xr_xir_compile_type_arena_layout(arena, type, &layout) || !layout.size ||
                    layout.size > sizeof(uint64_t) || !span.bytes) { status = XR_XIR_VALUE_BAD_ARGUMENT; break; }
                if (!storage_leaf_value(arena,type,span.bytes,layout.size,&current)) {
                    status=XR_XIR_VALUE_BAD_ARGUMENT;break;
                }
            }
            if (type==XR_XIR_UNIT ? (!tuple_field || inlined || !unit_value(&current)) :
                !xr_xir_value_argument(&current, arena, type)) {
                status = XR_XIR_VALUE_BAD_ARGUMENT; break;
            }
            if (owned_carrier_type(type)) {
                XirObject *object = object_pointer(&current);
                arena=object->arena;type=object->type;
                if (domain_sensitive && (object->kind == XIR_OBJECT_CLASS || object->kind == XIR_OBJECT_CELL ||
                    object->kind == XIR_OBJECT_FUNCTION || object->kind == XR_XIR_TYPE_TASK ||
                    object->kind == XR_XIR_TYPE_ITERATOR))
                    *domain_sensitive = true;
                if (task && object->arena && xr_xir_type_is_enum(xr_xir_compile_type_arena_types(object->arena),object->type) &&
                    (object->arena!=admission->arena || !xr_xir_compile_type_arena_enum_sendable(object->arena,object->type))) {
                    status=XR_XIR_VALUE_BAD_ARGUMENT;break;
                }
                size_t count = 0;
                if (object->type==XR_XIR_PANIC_INFO) {
                    const XrXirPanicPayload *panic=&((const XirPanicInfo *)object)->panic;
                    count=(size_t)!unit_value(&panic->message)+(size_t)!unit_value(&panic->error)+
                        (size_t)!unit_value(&panic->replaced_error);
                    if (task && ((!unit_value(&panic->error) && xr_xir_value_arena(&panic->error)!=admission->arena) ||
                        (!unit_value(&panic->replaced_error) && xr_xir_value_arena(&panic->replaced_error)!=admission->arena))) {
                        status=XR_XIR_VALUE_BAD_ARGUMENT;break;
                    }
                } else if (object->kind == XIR_OBJECT_CLASS) {
                    const XrXirStorageLayout *body=class_body_layout(object);
                    if (object->domain != admission->domain || !body) {
                        status=XR_XIR_VALUE_BAD_ARGUMENT;break;
                    }
                    if (!admission_is_compile(admission) && (body->field_count > admission->work)) {status=XR_XIR_VALUE_LIMIT;break;}
                    if (!admission_is_compile(admission)) admission->work-=body->field_count;
                    count=body->field_count;
                    /* This identity's active ancestor already schedules every
                     * child. Revalidate this handle, then stop only its back edge. */
                    for (size_t i = 0; i < stack.depth; ++i) {
                        if (!admission_is_compile(admission) && (!admission->work || !xr_xir_domain_work(admission->domain, 1))) {
                            status = XR_XIR_VALUE_LIMIT; break;
                        }
                        if (!admission_is_compile(admission)) --admission->work;
                        if (!stack.frames[i].boxed.inline_storage && stack.frames[i].boxed.object == object) {
                            count = 0; break;
                        }
                    }
                    if (status != XR_XIR_VALUE_OK) break;
                } else if (object->kind==XR_XIR_TYPE_TUPLE) {
                    if (!admission->domain) {status=XR_XIR_VALUE_BAD_ARGUMENT;break;}
                    count=((XirTuple *)object)->count;
                } else if (object->kind == XR_XIR_TYPE_NOMINAL || object->kind == XR_XIR_TYPE_NULLABLE) {
                    if (!admission->domain) { status = XR_XIR_VALUE_BAD_ARGUMENT; break; }
                    count = ((XirNominalValue *) object)->count;
                } else if (object->kind == XR_XIR_TYPE_ARRAY) {
                    bool needed = false;
                    status = type_needs_admission(xr_xir_compile_type_arena_types(object->arena),
                        object->type, admission, &needed);
                    if (status != XR_XIR_VALUE_OK) break;
                    if (needed) count = ((XirArray *) object)->length;
                } else if (object->kind == XR_XIR_TYPE_MAP || object->kind == XR_XIR_TYPE_SET) {
                    XirHash *map = (XirHash *)object;
                    bool needed = false;
                    status = type_needs_admission(xr_xir_compile_type_arena_types(object->arena),
                        object->type, admission, &needed);
                    if (status != XR_XIR_VALUE_OK) break;
                    if (needed) count = map->length * (object->kind == XR_XIR_TYPE_MAP ? 2u : 1u);
                } else if(object->kind==XR_XIR_TYPE_ITERATOR) {
                    if(object->domain!=admission->domain) {status=XR_XIR_VALUE_BAD_ARGUMENT;break;}
                } else if (object->kind == XR_XIR_TYPE_TASK || object->kind == XR_XIR_TYPE_ATOMIC || object->kind == XR_XIR_TYPE_CHANNEL) {

                    if (!admission->domain) { status = XR_XIR_VALUE_BAD_ARGUMENT; break; }
                } else if (object->kind) {
                    if (object->domain != admission->domain) { status = XR_XIR_VALUE_BAD_ARGUMENT; break; }
                    if (object->kind == XIR_OBJECT_FUNCTION) {
                        if (!admission->function) { status = XR_XIR_VALUE_BAD_ARGUMENT; break; }
                        const XrXirTypeArena *caller_arena=admission->arena;
                        admission->arena=arena;
                        status = admission->function(admission->context,
                            &((XirFunction *) object)->binding, type, admission_is_compile(admission)?NULL:&admission->work);
                        admission->arena=caller_arena;
                        if (status != XR_XIR_VALUE_OK) break;
                    } else if (object->kind == XR_XIR_TYPE_ATOMIC) {
                        /* Atomic cells have scalar payloads and no graph children. */
                    } else if (object->kind == XIR_OBJECT_CELL) {
                        const XirCell *cell = (const XirCell *)object;
                        type = cell->projection_count ? (XrXirType)cell->value.type :
                            xr_xir_cell_element(xr_xir_compile_type_arena_types(object->arena), type);
                        tuple_field = !cell->projection_count && type == XR_XIR_UNIT;
                        current=cell->value;
                        /* A projection retains its actual root, not a value in its own storage type. */
                        if (cell->projection_count) arena=xr_xir_value_arena(&current);
                        inlined=false;continue;
                    } else { status = XR_XIR_VALUE_BAD_ARGUMENT; break; }
                }
                if (count) {
                    status = admission_push(&stack, (ValueAdmissionFrame){.boxed={false,object,0,count}}, admission);
                    if (status != XR_XIR_VALUE_OK) break;
                }
            }
        }
        while (stack.depth && admission_frame_next(&stack.frames[stack.depth-1])==admission_frame_count(&stack.frames[stack.depth-1]))
            --stack.depth;
        if (!stack.depth) break;
        arena=admission_frame_arena(&stack.frames[stack.depth-1]);
        admission_child(&stack.frames[stack.depth - 1], arena, &current, &span, &inlined, &tuple_field);
        type = inlined ? span.type : (XrXirType)current.type;
    }
    if (stack.frames) {
        if (stack.frames != &stack.first)
            xr_xir_domain_deallocate(admission->domain, stack.frames, stack.bytes);
        admission->scratch_bytes += stack.bytes;
    }
    return status;
}
static XrXirValueStatus value_admit_summary(const XrXirValue *value,XrXirType type,
    XrXirValueAdmission *admission,bool *domain_sensitive) {
    return value_admit_summary_mode(value,type,admission,domain_sensitive,false);
}
XR_FUNCDEF XrXirValueStatus xr_xir_panic_graph_admit(const XrXirValue *value,XrXirType type,
    XrXirValueAdmission *admission) {
    if (!admission_domain_current(admission)) return XR_XIR_VALUE_BAD_ARGUMENT;
    xr_xir_value_graph_begin();
    XrXirValueStatus status=value_admit_summary_mode(value,type,admission,NULL,true);
    xr_xir_value_graph_end();return status;
}
XR_FUNC XrXirValueStatus xr_xir_value_admit(const XrXirValue *value, XrXirType type,
    XrXirValueAdmission *admission) {
    if (!admission_domain_current(admission)) return XR_XIR_VALUE_BAD_ARGUMENT;
    xr_xir_value_graph_begin();
    XrXirValueStatus graph_outcome = value_admit_summary(value, type, admission, NULL);
    xr_xir_value_graph_end();
    return graph_outcome;
}
