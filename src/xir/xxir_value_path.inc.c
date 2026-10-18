/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_value_path.inc.c - Borrowed navigation and private subtree publication
 */
typedef struct ValuePathSlot {
    XrXirType type;
    unsigned char *bytes;
    bool inline_storage;
    const XrXirTypeArena *arena;
} ValuePathSlot;
typedef struct ValuePathWrite {
    XrXirValue candidate;
    unsigned char *publish;
    bool domain_sensitive, module_publication;
} ValuePathWrite;

static XrXirValue path_handle(ValuePathSlot slot) {
    XrXirValue value = {(uint32_t)slot.type, 0, 0};
    memcpy(&value.payload, slot.bytes, sizeof(value.payload));
    XrXirValue actual={0};
    return xr_xir_value_from_payload(slot.arena,slot.type,value.payload,&actual) ? actual : value;
}
static XrXirValueStatus path_array_clone(const XrXirValue *value,
    XrXirValueAdmission *admission, XrXirValue *output) {
    XirArray *source = (XirArray *)object_pointer(value), *copy = NULL;
    if (source->length > admission->work) return XR_XIR_VALUE_LIMIT;
    admission->work -= source->length;
    XrXirValueStatus status = array_allocate((XrXirType)value->type, source->capacity, admission, &copy);
    if (status != XR_XIR_VALUE_OK) return status;
    XrXirValue owned = array_value(copy); StoragePack pack = {0};
    status = storage_pack_begin(admission, source->element, &pack);
    if (status == XR_XIR_VALUE_OK) {
        for (size_t i = 0; i < source->length; ++i) {
            status = storage_pack_copy(&pack, array_slot(source, i), array_slot(copy, i));
            if (status != XR_XIR_VALUE_OK) break;
            ++copy->length;
        }
        storage_pack_end(&pack);
    }
    if (status != XR_XIR_VALUE_OK) { xr_xir_value_drop(&owned); return status; }
    *output = owned; return XR_XIR_VALUE_OK;
}
/* Only the first replaced owner remains unpublished. Further replacements are
 * inside that private subtree, whose retained children preserve original data. */
static XrXirValueStatus path_unique(ValuePathSlot *slot,
    ValuePathWrite *write, XrXirValueAdmission *admission) {
    const XrXirTypes *types = xr_xir_compile_type_arena_types(admission->arena);
    bool array = xr_xir_type_is_array(types, slot->type);
    if (!array && slot->inline_storage) return XR_XIR_VALUE_OK;
    XrXirValue current = path_handle(*slot);
    XirObject *object = object_pointer(&current);
    if (atomic_load_explicit(&object->references, memory_order_acquire) == 1 &&
        (!write->domain_sensitive || object->domain == admission->domain)) return XR_XIR_VALUE_OK;
    XrXirValue copy = {0}; XrXirValueStatus status;
    if (array) status = path_array_clone(&current, admission, &copy);
    else {
        XirNominalValue *record = (XirNominalValue *)object;
        if (record->count > admission->work) return XR_XIR_VALUE_LIMIT;
        admission->work -= record->count;
        status = struct_copy_fields(slot->type, record->fields, record->count, admission, &copy);
    }
    if (status != XR_XIR_VALUE_OK) return status;
    if (!write->publish) {
        write->publish = slot->bytes; write->candidate = copy;
        slot->bytes = (unsigned char *)&write->candidate.payload;
    } else {
        memcpy(slot->bytes, &copy.payload, sizeof(copy.payload));
        xr_xir_value_drop(&current);
    }
    return XR_XIR_VALUE_OK;
}
static XrXirValueStatus path_storage_step(ValuePathSlot *slot,
    const XrXirValuePathStep *step, XrXirValueAdmission *admission,
    ValuePathWrite *write, XrXirFaultDetail *fault, const XirObject *borrow_owner) {
    if (!admission->work) return XR_XIR_VALUE_LIMIT;
    --admission->work;
    if (slot->type != step->container) return XR_XIR_VALUE_BAD_ARGUMENT;
    const XrXirTypes *types = xr_xir_compile_type_arena_types(admission->arena);
    const XrXirTypeNode *node = xr_xir_type_node(types, slot->type);
    if (step->kind == XR_XIR_PATH_FIELD && node && xr_xir_type_is_class(types, slot->type)) {
        /* A class is an identity, not a copyable value: its fields change in place, so no
         * ancestor is made unique and no candidate is published for this step. */
        if (step->selector < 0 || (uint64_t)step->selector >= node->nominal.field_count)
            return XR_XIR_VALUE_BAD_ARGUMENT;
        uint32_t field = (uint32_t)step->selector;
        if (write && !(types->nominals->identities[node->nominal.declaration].fields[field].flags &
            XR_XIR_FIELD_MUTABLE)) return XR_XIR_VALUE_BAD_ARGUMENT;
        XrXirValue handle = path_handle(*slot);
        XirObject *object = object_pointer(&handle);
        const XrXirStorageLayout *layout = class_body_layout(object);
        if (!layout || (object != borrow_owner &&
                ((class_origin_state((const XirClassObject *)object) &
                    (XIR_CLASS_ORIGIN_PREPARED | XIR_CLASS_ORIGIN_BORROWED)) ||
                    ((const XirClassObject *)object)->borrow_top)) ||
            field >= layout->field_count) return XR_XIR_VALUE_BAD_ARGUMENT;
        if (write && xr_xir_class_module_storage(&handle)) write->module_publication = true;
        *slot = (ValuePathSlot){node->nominal.fields[field],
            (unsigned char *)((XirClassObject *)object + 1) + layout->field_offsets[field], true,admission->arena};
        return XR_XIR_VALUE_OK;
    }
    if (step->kind == XR_XIR_PATH_FIELD) {
        if (!node || !xr_xir_type_is_struct(types, slot->type) || step->selector < 0 ||
            (uint64_t)step->selector >= node->nominal.field_count) return XR_XIR_VALUE_BAD_ARGUMENT;
        uint32_t field = (uint32_t)step->selector;
        if (write && !(types->nominals->identities[node->nominal.declaration].fields[field].flags &
            XR_XIR_FIELD_MUTABLE)) return XR_XIR_VALUE_BAD_ARGUMENT;
        if (write) {
            XrXirValueStatus status = path_unique(slot, write, admission);
            if (status != XR_XIR_VALUE_OK) return status;
        }
        if (slot->inline_storage) {
            const XrXirStorageLayout *layout = xr_xir_compile_type_arena_storage(admission->arena, slot->type);
            if (slot->bytes) slot->bytes += layout->field_offsets[field];
        } else {
            XrXirValue value = path_handle(*slot);
            XirNominalValue *record = (XirNominalValue *)object_pointer(&value);
            slot->bytes = (unsigned char *)&record->fields[field].payload;
        }
        slot->type = node->nominal.fields[field]; return XR_XIR_VALUE_OK;
    }
    if (step->kind != XR_XIR_PATH_INDEX || !xr_xir_type_is_array(types, slot->type))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValue value = path_handle(*slot);
    XirArray *array = (XirArray *)object_pointer(&value);
    if (step->selector < 0 || (uint64_t)step->selector >= array->length) {
        *fault = (XrXirFaultDetail){430, 0, step->selector, (int64_t)array->length};
        return XR_XIR_VALUE_BOUNDS;
    }
    if (write) {
        XrXirValueStatus status = path_unique(slot, write, admission);
        if (status != XR_XIR_VALUE_OK) return status;
        value = path_handle(*slot); array = (XirArray *)object_pointer(&value);
    }
    *slot = (ValuePathSlot){array->element, array_slot(array, (size_t)step->selector), true,admission->arena};
    return XR_XIR_VALUE_OK;
}
typedef struct ValuePathSelector {
    const XrXirValuePathStep *step;
    const XrXirTypeArena *arena;
} ValuePathSelector;
static XrXirValueStatus path_step(ValuePathSlot *slot,ValuePathSelector selector,
    XrXirValueAdmission *admission,ValuePathWrite *write,XrXirFaultDetail *fault,const XirObject *borrow_owner) {
    if (!xir_type_arena_same_type(slot->arena,slot->type,selector.arena,selector.step->container))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    const XrXirTypes *types=xr_xir_compile_type_arena_types(slot->arena);
    if (owned_carrier_type(slot->type) && (!slot->inline_storage || !inline_nominal_type(types,slot->type))) {
        XrXirValue value=path_handle(*slot);
        if (!xr_xir_value_argument(&value,slot->arena,slot->type)) return XR_XIR_VALUE_BAD_ARGUMENT;
        if (owned_carrier_type((XrXirType)value.type) && object_pointer(&value)->arena) {
            slot->type=(XrXirType)value.type;slot->arena=object_pointer(&value)->arena;
        }
    }
    XrXirValueAdmission local=*admission;local.arena=slot->arena;
    /* Only the checked descriptor's type coordinate changes; the selector,
     * actual object header and caller's Domain/loan authority stay intact. */
    XrXirValuePathStep step=*selector.step;step.container=slot->type;
    XrXirValueStatus status=path_storage_step(slot,&step,&local,write,fault,borrow_owner);
    return storage_admission_end(admission,&local,status);
}
static XrXirValueStatus path_begin(const XrXirValuePlace *root,
    const XrXirValuePath *path, XrXirValueAdmission *admission,
    ValuePathSlot *slot, XrXirFaultDetail *fault) {
    if (fault) *fault = (XrXirFaultDetail){0};
    if (!root || !root->payload || !path || (path->count && !path->steps) ||
        !admission || !admission->arena || !fault) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (path->count > admission->work) return XR_XIR_VALUE_LIMIT;
    *slot = (ValuePathSlot){root->type, root->payload, false,admission->arena};
    XrXirValue value = path_handle(*slot);
    return xr_xir_value_admit(&value, root->type, admission);
}
static XrXirValueStatus path_storage_store(ValuePathSlot slot,
    const XrXirValue *value, XrXirValueAdmission *admission,
    XrXirClassPublication *publication) {
    /* The incoming tree was admitted once before any ancestor replacement.
     * The final place still requires its exact type and arena. */
    if (!xr_xir_value_argument(value, admission->arena, slot.type)) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValueStatus status = XR_XIR_VALUE_OK;
    if (!slot.inline_storage) {
        XrXirValue owned = {0}, previous = path_handle(slot);
        status = xr_xir_value_copy(value, &owned);
        if (status != XR_XIR_VALUE_OK) return status;
        if (publication) xr_xir_class_publication_commit(publication);
        memcpy(slot.bytes, &owned.payload, sizeof(owned.payload));
        xr_xir_value_drop(&previous); return XR_XIR_VALUE_OK;
    }
    XrXirLayout layout = {0};
    if (!xr_xir_compile_type_arena_layout(admission->arena, slot.type, &layout)) return XR_XIR_VALUE_BAD_ARGUMENT;
    StoragePrepared prepared = {0};
    status = storage_prepared_begin(&prepared,slot.type,value,layout.size,admission);
    if (status != XR_XIR_VALUE_OK) return status;
    StorageCursor cursor = {0}; XirObject *pending = NULL;
    XR_CHECK(storage_cursor_init(admission->arena, (StorageSpan){slot.type, slot.bytes},
        (StorageFrame *)prepared.pack.frames, prepared.pack.capacity, true, &cursor) == XR_XIR_VALUE_OK,
        "inline overwrite reserves release traversal before publication");
    if (publication) xr_xir_class_publication_commit(publication);
    storage_queue_release(&cursor, UINT64_MAX, &pending);
    if (layout.size) memcpy(slot.bytes, prepared.bytes, layout.size);
    prepared.owns = false; release_pending(pending); storage_prepared_end(&prepared);
    return XR_XIR_VALUE_OK;
}
static XrXirValueStatus path_store(ValuePathSlot slot,const XrXirValue *value,
    XrXirValueAdmission *admission,XrXirClassPublication *publication) {
    XrXirValueAdmission local=*admission;local.arena=slot.arena;
    XrXirValueStatus status=path_storage_store(slot,value,&local,publication);
    return storage_admission_end(admission,&local,status);
}
typedef struct ValuePathRoute {
    XrXirValuePath prefix, suffix;
    const XirObject *borrow_owner;
    bool module_publication;
    const XrXirTypeArena *root_arena,*prefix_arena;
} ValuePathRoute;
static ValuePathSelector path_route_step(const ValuePathRoute *route,uint32_t i,
    const XrXirTypeArena *arena) {
    return i<route->prefix.count ? (ValuePathSelector){&route->prefix.steps[i],route->prefix_arena ? route->prefix_arena : arena} :
        (ValuePathSelector){&route->suffix.steps[i-route->prefix.count],arena};
}
static XrXirValueStatus path_route_begin(const XrXirValuePlace *root,
    const ValuePathRoute *route, XrXirValueAdmission *admission,
    ValuePathSlot *slot, uint32_t *count, XrXirFaultDetail *fault) {
    if (!route || !count || (route->prefix.count && !route->prefix.steps) ||
        (route->suffix.count && !route->suffix.steps) ||
        route->suffix.count > UINT32_MAX - route->prefix.count) return XR_XIR_VALUE_BAD_ARGUMENT;
    *count = route->prefix.count + route->suffix.count;
    XrXirValuePath shape = {route->prefix.count ? route->prefix.steps : route->suffix.steps, *count};
    XrXirValueAdmission local=*admission;
    if (route->root_arena) local.arena=route->root_arena;
    XrXirValueStatus status=path_begin(root,&shape,&local,slot,fault);
    return storage_admission_end(admission,&local,status);
}
static XrXirValueStatus value_path_read_route(const XrXirValuePlace *root,
    const ValuePathRoute *route, XrXirValueAdmission *admission,
    XrXirValue *output, XrXirFaultDetail *fault) {
    if (!unit_value(output)) return XR_XIR_VALUE_BAD_ARGUMENT;
    ValuePathSlot slot = {0}; uint32_t count = 0;
    XrXirValueStatus status = path_route_begin(root, route, admission, &slot, &count, fault);
    if (status != XR_XIR_VALUE_OK) return status;
    for (uint32_t i = 0; i < count; ++i) {
        status = path_step(&slot, path_route_step(route,i,admission->arena), admission, NULL, fault, i ? NULL : route->borrow_owner);
        if (status != XR_XIR_VALUE_OK) return status;
    }
    if (slot.inline_storage) {
        XrXirValueAdmission local=*admission;local.arena=slot.arena;
        status=storage_unpack((StorageSpan){slot.type,slot.bytes},&local,output);
        return storage_admission_end(admission,&local,status);
    }
    XrXirValue value = path_handle(slot); return xr_xir_value_copy(&value, output);
}
static XrXirValueStatus xr_xir_value_path_read_graph_operation(const XrXirValuePlace *root,
    const XrXirValuePath *path, XrXirValueAdmission *admission,
    XrXirValue *output, XrXirFaultDetail *fault) {
    if (!path) return XR_XIR_VALUE_BAD_ARGUMENT;
    ValuePathRoute route = {.prefix=*path};
    return value_path_read_route(root, &route, admission, output, fault);
}
XR_FUNC XrXirValueStatus xr_xir_value_path_read(const XrXirValuePlace *root,
    const XrXirValuePath *path, XrXirValueAdmission *admission,
    XrXirValue *output, XrXirFaultDetail *fault) {
    if (!admission_domain_current(admission)) return XR_XIR_VALUE_BAD_ARGUMENT;
    xr_xir_value_graph_begin();
    XrXirValueStatus graph_outcome = xr_xir_value_path_read_graph_operation(root, path, admission, output, fault);
    xr_xir_value_graph_end();
    return graph_outcome;
}
static XrXirValueStatus path_mutate_route(const XrXirValuePlace *root,
    const ValuePathRoute *route, const XrXirValue *value,
    XrXirValueAdmission *admission, XrXirFaultDetail *fault, bool append) {
    if (!admission || !admission->domain || !value) return XR_XIR_VALUE_BAD_ARGUMENT;
    ValuePathSlot slot = {0};
    ValuePathWrite write = {.module_publication = route && route->module_publication};
    XrXirClassPublication publication = {0};
    uint32_t count = 0;
    XrXirValueStatus status = path_route_begin(root, route, admission, &slot, &count, fault);
    if (status != XR_XIR_VALUE_OK) return status;
    XrXirValueAdmission incoming={0};
    status=storage_admission_begin(value,admission,&incoming);
    if (status==XR_XIR_VALUE_OK) {
        status=value_admit_summary(value,(XrXirType)value->type,&incoming,&write.domain_sensitive);
        status=storage_admission_end(admission,&incoming,status);
    }
    if (status != XR_XIR_VALUE_OK) return status;
    for (uint32_t i = 0; i < count; ++i) {
        status = path_step(&slot, path_route_step(route,i,admission->arena), admission, &write, fault, i ? NULL : route->borrow_owner);
        if (status != XR_XIR_VALUE_OK) break;
    }
    if (status == XR_XIR_VALUE_OK) {
        if (write.module_publication)
            status = class_publication_prepare_route(value, 1, admission,
                route->borrow_owner && route->borrow_owner->kind == XIR_OBJECT_CLASS ?
                    route->borrow_owner : NULL, &publication);
        if (status == XR_XIR_VALUE_OK) {
            XrXirValuePlace leaf = {slot.type, slot.bytes};
            XrXirClassPublication *prepared = publication.operation ? &publication : NULL;
            const ArrayMutation mutation = {true, true, write.domain_sensitive, prepared};
            if (append) {
                XrXirValueAdmission local=*admission,storage={0};local.arena=slot.arena;
                XrXirValuePlace actual={0};
                status=storage_place_admission(&leaf,&local,&storage,&actual);
                if (status==XR_XIR_VALUE_OK) {
                    status=array_mutate(&actual,0,value,&storage,NULL,&mutation);
                    status=storage_admission_end(&local,&storage,status);
                }
                status=storage_admission_end(admission,&local,status);
            } else status=path_store(slot,value,admission,prepared);
        }
    }
    if (status == XR_XIR_VALUE_OK && write.publish) {
        XrXirValue previous = {(uint32_t)write.candidate.type, 0, 0};
        memcpy(&previous.payload, write.publish, sizeof(previous.payload));
        memcpy(write.publish, &write.candidate.payload, sizeof(write.candidate.payload));
        write.candidate = (XrXirValue){0}; xr_xir_value_drop(&previous);
    }
    xr_xir_value_drop(&write.candidate);
    xr_xir_class_publication_drop(&publication); return status;
}
static XrXirValueStatus path_mutate(const XrXirValuePlace *root,
    const XrXirValuePath *path, const XrXirValue *value,
    XrXirValueAdmission *admission, XrXirFaultDetail *fault, bool append) {
    if (!path) return XR_XIR_VALUE_BAD_ARGUMENT;
    ValuePathRoute route = {.prefix=*path};
    return path_mutate_route(root, &route, value, admission, fault, append);
}
static XrXirValueStatus xr_xir_value_path_write_graph_operation(const XrXirValuePlace *root,
    const XrXirValuePath *path, const XrXirValue *value,
    XrXirValueAdmission *admission, XrXirFaultDetail *fault) {
    return path_mutate(root, path, value, admission, fault, false);
}
XR_FUNC XrXirValueStatus xr_xir_value_path_write(const XrXirValuePlace *root,
    const XrXirValuePath *path, const XrXirValue *value,
    XrXirValueAdmission *admission, XrXirFaultDetail *fault) {
    if (!admission_domain_current(admission)) return XR_XIR_VALUE_BAD_ARGUMENT;
    xr_xir_value_graph_begin();
    XrXirValueStatus graph_outcome = xr_xir_value_path_write_graph_operation(root, path, value, admission, fault);
    xr_xir_value_graph_end();
    return graph_outcome;
}
static XrXirValueStatus xr_xir_value_path_push_graph_operation(const XrXirValuePlace *root,
    const XrXirValuePath *path, const XrXirValue *value,
    XrXirValueAdmission *admission, XrXirFaultDetail *fault) {
    return path_mutate(root, path, value, admission, fault, true);
}
XR_FUNC XrXirValueStatus xr_xir_value_path_push(const XrXirValuePlace *root,
    const XrXirValuePath *path, const XrXirValue *value,
    XrXirValueAdmission *admission, XrXirFaultDetail *fault) {
    if (!admission_domain_current(admission)) return XR_XIR_VALUE_BAD_ARGUMENT;
    xr_xir_value_graph_begin();
    XrXirValueStatus graph_outcome = xr_xir_value_path_push_graph_operation(root, path, value, admission, fault);
    xr_xir_value_graph_end();
    return graph_outcome;
}
XR_FUNC XrXirValueStatus xir_value_path_replacement(const XrXirValue *root,const XrXirValuePath *path,
    const XrXirValue *replacement,XrXirValueAdmission *admission,XrXirValue *output,XrXirFaultDetail *fault) {
    if (!admission || !admission->arena || !admission->domain || !admission_domain_current(admission) ||
        !root || !replacement || !unit_value(output)) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValue borrowed=*root;XrXirValuePlace place={(XrXirType)root->type,&borrowed.payload};ValuePathSlot slot={0};
    xr_xir_value_graph_begin();XrXirValueStatus status=path_begin(&place,path,admission,&slot,fault);
    const XrXirTypes *types=xr_xir_compile_type_arena_types(admission->arena);
    for (uint32_t i=0;status==XR_XIR_VALUE_OK && i<path->count;++i) {
        if (xr_xir_type_is_class(types,slot.type)) { status=XR_XIR_VALUE_UNSUPPORTED;break; }
        status=path_step(&slot,&path->steps[i],admission,NULL,fault,NULL);
    }
    XrXirValue owned={0};
    if (status==XR_XIR_VALUE_OK) status=xr_xir_value_copy(root,&owned);
    if (status==XR_XIR_VALUE_OK) {
        place=(XrXirValuePlace){(XrXirType)owned.type,&owned.payload};
        status=xr_xir_value_path_write_graph_operation(&place,path,replacement,admission,fault);
    }
    if (status==XR_XIR_VALUE_OK) { *output=owned;owned=(XrXirValue){0}; }
    xr_xir_value_drop(&owned);xr_xir_value_graph_end();return status;
}
