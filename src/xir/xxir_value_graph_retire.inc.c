/*
 * xray - Lightweight typed scripting with native concurrency
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_value_graph_retire.inc.c - Allocation-free residual owner disposal
 *
 * KEY CONCEPT:
 *   Execution closure and final host ownership are distinct retirement boundaries.
 */
typedef bool (*GraphEdgeVisitor)(const XrXirValue *value, void *payload, void *context);
static bool graph_direct_edge(const XrXirValue *value, GraphEdgeVisitor visit, void *context) {
    return visit(value, (void *)&value->payload, context);
}
static bool graph_storage_edges(StorageCursor *cursor, GraphEdgeVisitor visit, void *context) {
    for (;;) {
        StorageSpan leaf = {0}; bool found = false;
        if (storage_cursor_next(cursor, NULL, &leaf, &found) != XR_XIR_VALUE_OK) return false;
        if (!found) return true;
        XrXirValue value = {(uint32_t)leaf.type, 0, 0};
        memcpy(&value.payload, leaf.bytes, sizeof(value.payload));
        if (!visit(&value, (void *)leaf.bytes, context)) return false;
    }
}
static bool graph_edges(XirObject *object, GraphEdgeVisitor visit, void *context) {
    if (object->type == XR_XIR_STRING || object->kind == XR_XIR_TYPE_ATOMIC) return true;
    if (object->type == XR_XIR_PANIC_INFO)
        return graph_direct_edge(&((XirPanicInfo *)object)->panic.message, visit, context);
    if (object->kind == XR_XIR_TYPE_CELL)
        return graph_direct_edge(&((XirCell *)object)->value, visit, context);
    if (object->kind == XR_XIR_TYPE_CALLABLE) {
        const XrXirFunctionBinding *binding = &((XirFunction *)object)->binding;
        for (uint32_t i = 0; i < binding->capture_count; ++i)
            if (!graph_direct_edge(&binding->captures[i], visit, context)) return false;
        return true;
    }
    if (object->kind == XR_XIR_TYPE_TASK) {
        XirTask *task = (XirTask *)object;
        if (atomic_load_explicit(&task->state, memory_order_acquire) != XIR_TASK_TERMINAL ||
            task->executor || task->call) return true;
        return graph_direct_edge(&task->outcome.value, visit, context) &&
            graph_direct_edge(&task->outcome.panic.message, visit, context);
    }
    if (object->kind == XR_XIR_TYPE_TUPLE) {
        XirTuple *tuple = (XirTuple *)object;
        for (uint32_t i = 0; i < tuple->count; ++i)
            if (!graph_direct_edge(&tuple->fields[i], visit, context)) return false;
        return true;
    }
    if (object->kind == XR_XIR_TYPE_NOMINAL || object->kind == XR_XIR_TYPE_NULLABLE) {
        XirNominalValue *record = (XirNominalValue *)object;
        for (uint32_t i = 0; i < record->count; ++i)
            if (!graph_direct_edge(&record->fields[i], visit, context)) return false;
        return true;
    }
    if (object->kind == XIR_OBJECT_CLASS) {
        XirClassObject *instance = (XirClassObject *)object;
        const XrXirStorageLayout *layout = class_body_layout(object);
        const XrXirTypeNode *node = xr_xir_type_node(xr_xir_compile_type_arena_types(object->arena),object->type);
        if (!layout || !node || !class_allocation_valid(object)) return false;
        StorageFrame *frames = instance->release_capacity ?
            (StorageFrame *)((unsigned char *)object + instance->release_offset) : NULL;
        const unsigned char *body = (const unsigned char *)(instance + 1);
        for (uint32_t i = 0; i < layout->field_count; ++i) {
            StorageCursor cursor = {0};
            StorageSpan span = {node->nominal.fields[i], body + layout->field_offsets[i]};
            if (storage_cursor_init(object->arena, span, frames, instance->release_capacity, true, &cursor) != XR_XIR_VALUE_OK ||
                !graph_storage_edges(&cursor, visit, context)) return false;
        }
        return true;
    }
    if (object->kind == XR_XIR_TYPE_ARRAY) {
        XirArray *array = (XirArray *)object;
        if (!array_storage_valid(array)) return false;
        uint32_t depth = array_release_depth(object->arena, array->element);
        for (size_t i = 0; array_owned_elements(array) && i < array->length; ++i) {
            StorageCursor cursor = {0};
            StorageSpan span = {array->element, array->data ? array->data + i * array->stride : NULL};
            if (storage_cursor_init(object->arena, span, depth ? (StorageFrame *)(array + 1) : NULL,
                depth, true, &cursor) != XR_XIR_VALUE_OK || !graph_storage_edges(&cursor, visit, context)) return false;
        }
        return true;
    }
    return false;
}
typedef struct GraphScan {
    XrXirDomain *domain;
    XirObject *work, *pending;
} GraphScan;
static bool graph_subtract(const XrXirValue *value, void *payload, void *context) {
    (void)payload;
    GraphScan *scan = context;
    if (!owned_carrier_type((XrXirType)value->type)) return true;
    if (!value->payload) return false;
    XirObject *child = object_pointer(value);
    if (empty_enum_object(child) || child->domain != scan->domain) return true;
    if (!child->graph_registered || !child->graph_trial) return false;
    --child->graph_trial;
    return true;
}
static void graph_mark(XirObject *object, GraphScan *scan) {
    if (object->graph_live) return;
    object->graph_live = true;
    object->graph_work = scan->work; scan->work = object;
}
static bool graph_mark_edge(const XrXirValue *value, void *payload, void *context) {
    (void)payload;
    GraphScan *scan = context;
    if (!owned_carrier_type((XrXirType)value->type)) return true;
    if (!value->payload) return false;
    XirObject *child = object_pointer(value);
    if (!empty_enum_object(child) && child->domain == scan->domain) {
        if (!child->graph_registered) return false;
        graph_mark(child, scan);
    }
    return true;
}
static bool graph_unlink_edge(const XrXirValue *value, void *payload, void *context) {
    GraphScan *scan = context;
    if (!owned_carrier_type((XrXirType)value->type)) return true;
    XirObject *child = object_pointer(value);
    if (!empty_enum_object(child)) {
        if (!child->graph_dead) graph_object_release_locked(child, &scan->pending);
        /* Only arena-owned descriptor edges survive for the unlocked pass.
         * A foreign child can lose its final unrelated owner after unlock. */
        uint64_t cleared = 0;
        memcpy(payload, &cleared, sizeof(cleared));
    }
    return true;
}
/* Arena-owned empty descriptors have no registry node or ordinary object RC. */
static bool graph_release_empty_edge(const XrXirValue *value, void *payload, void *context) {
    (void)payload;
    (void)context;
    if (owned_carrier_type((XrXirType)value->type) && value->payload) {
        XirObject *child = object_pointer(value);
        if (empty_enum_object(child)) xr_xir_compile_type_arena_drop(child->arena);
    }
    return true;
}
static bool graph_active_task(XirObject *object) {
    if (object->kind != XR_XIR_TYPE_TASK) return false;
    XirTask *task = (XirTask *)object;
    return atomic_load_explicit(&task->state, memory_order_acquire) != XIR_TASK_TERMINAL ||
        task->executor || task->call;
}
static XirObject *graph_retire_locked(XrXirDomain *domain, XirObject **pending) {
    GraphScan scan = {.domain = domain};
    if (domain->graph_invalid) return NULL;
    for (XirObject *o = domain->objects; o; o = o->graph_next) {
        o->graph_trial = atomic_load_explicit(&o->references, memory_order_relaxed);
        o->graph_live = false; o->graph_work = NULL;
    }
    for (XirObject *o = domain->objects; o; o = o->graph_next)
        if (!graph_edges(o, graph_subtract, &scan)) { domain->graph_invalid = true; return NULL; }
    bool external = false;
    for (XirObject *o = domain->objects; o; o = o->graph_next)
        if (o->graph_trial || graph_active_task(o)) { external = true; graph_mark(o, &scan); }
    /* A host residual owner ends only when ALL of its external roots are gone. */
    if (domain->close_started && external) return NULL;
    while (scan.work) {
        XirObject *o = scan.work; scan.work = o->graph_work;
        if (!graph_edges(o, graph_mark_edge, &scan)) { domain->graph_invalid = true; return NULL; }
    }
    domain->close_started = true;
    XirObject *dead = NULL;
    for (XirObject *o = domain->objects; o;) {
        XirObject *next = o->graph_next;
        if (!o->graph_live) {
            graph_object_unlink(o); o->graph_dead = true;
            o->release_next = dead; dead = o;
        }
        o = next;
    }
    for (XirObject *o = dead; o; o = o->release_next)
        XR_CHECK(graph_edges(o, graph_unlink_edge, &scan), "validated residual graph changed during unlink");
    *pending = scan.pending;
    return dead;
}
static void graph_drain(void) {
    for (;;) {
        graph_lock();
        XrXirDomain *domain = value_graph_closed;
        while (domain && !domain->close_pending) domain = domain->closed_next;
        if (value_graph_operations || !domain) {
            value_graph_draining = false;
            graph_unlock(); return;
        }
        domain->close_pending = false;
        domain->close_processing = true;
        XirObject *pending = NULL;
        XirObject *dead = graph_retire_locked(domain, &pending);
        graph_unlock();
        /* All dead nodes stay allocated until every empty-descriptor edge is read. */
        for (XirObject *o = dead; o; o = o->release_next)
            XR_CHECK(graph_edges(o, graph_release_empty_edge, NULL), "detached residual graph lost its complete shape");
        release_pending(dead);
        release_pending(pending);
        graph_lock();
        domain->close_processing = false;
        bool dispose = !atomic_load_explicit(&domain->references, memory_order_relaxed) && !domain->close_pending;
        if (dispose) graph_domain_unlink(domain);
        graph_unlock();
        if (dispose) graph_domain_dispose(domain);
    }
}
