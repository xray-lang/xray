/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcoro_gc_traverse.c - Type-specific GC traversal functions
 */

#include "xcoro_gc_traverse.h"
#include "../closure/xcell.h"
#include "../../base/xchecks.h"
#include "../value/xslot_type.h"
#include "../object/xarray.h"
#include "../object/xmap.h"
#include "../object/xset.h"
#include "../object/xjson.h"
#include "../object/xiterator.h"
#include "../object/xexception.h"
#include "xalloc_unified.h"        // xr_coro_get_isolate
#include "../xisolate_internal.h"  // XrayIsolate ext_traverse_funcs
#include "../xerror_impl.h"
#include "../class/xclass.h"
#include "../class/xinstance.h"
#include "../../module/xmodule.h"
#include "../../runtime/xexec_frame.h"
#include "../../coro/xtask.h"

/* ========== Array Traversal ========== */

void xr_gc_traverse_array(XrCoroGC *gc, XrGCHeader *obj) {
    if (!gc || !obj)
        return;
    struct XrArray *arr = (struct XrArray *) obj;
    XR_DCHECK(XR_GC_GET_TYPE(&arr->gc) == XR_TARRAY, "gc_traverse_array: object is not an array");

    // Mark source array (for slices)
    if (arr->source) {
        xr_coro_gc_markobject(gc, (XrGCHeader *) arr->source);
    }

    // Mark data blob if it lives on GC heap (prevents sweep from reclaiming it)
    if (arr->data && arr->data_on_gc_heap) {
        XrGCHeader *blob = ((XrGCHeader *) arr->data) - 1;
        xr_coro_gc_markobject(gc, blob);
    }

    // Only XR_ELEM_ANY arrays with GC pointers need element scanning
    if (arr->data && arr->length > 0 && XR_ARRAY_IS_GC_TRACED(arr) && arr->has_gc_ptrs) {
        XR_DCHECK(arr->length <= arr->capacity, "gc_traverse_array: length > capacity");
        XrValue *data = (XrValue *) arr->data;
        for (int32_t i = 0; i < arr->length; i++) {
            xr_coro_gc_markvalue(gc, data[i]);
        }
    }
}

/* ========== Map Traversal ========== */

void xr_gc_traverse_map(XrCoroGC *gc, XrGCHeader *obj) {
    if (!gc || !obj)
        return;
    struct XrMap *map = (struct XrMap *) obj;
    XR_DCHECK(XR_GC_GET_TYPE(&map->gc) == XR_TMAP, "gc_traverse_map: object is not a map");

    // Skip dummy map
    if (xr_map_isdummy(map))
        return;

    // Skip uninitialized map (node pointer not yet set)
    if (!map->node)
        return;

    // Mark node blob if it lives on GC heap (prevents sweep from reclaiming it)
    if (map->flags & XR_MAP_FLAG_NODES_ON_GC) {
        XrGCHeader *blob = ((XrGCHeader *) map->node) - 1;
        xr_coro_gc_markobject(gc, blob);
    }

    // Check if this is a weak map
    bool is_weak = (map->flags & XR_MAP_FLAG_WEAK) != 0;

    if (is_weak) {
        // Weak map: only mark values, not keys
        // Add to weak list for later processing in atomic phase
        xr_gclist_push(&gc->weak, (XrGCHeader *) map);

        // Only mark values (keys are weak references)
        uint32_t size = xr_map_sizenode(map);
        for (uint32_t i = 0; i < size; i++) {
            XrMapNode *n = xr_map_node(map, i);
            if (!XR_MAP_NODE_EMPTY(n)) {
                // Skip marking key (weak reference)
                xr_coro_gc_markvalue(gc, n->value);
            }
        }
    } else {
        // Normal map: mark both keys and values
        uint32_t size = xr_map_sizenode(map);
        for (uint32_t i = 0; i < size; i++) {
            XrMapNode *n = xr_map_node(map, i);
            if (!XR_MAP_NODE_EMPTY(n)) {
                xr_coro_gc_markvalue(gc, n->key);
                xr_coro_gc_markvalue(gc, n->value);
            }
        }
    }
}

/* ========== Set Traversal ========== */

void xr_gc_traverse_set(XrCoroGC *gc, XrGCHeader *obj) {
    if (!gc || !obj)
        return;
    struct XrSet *set = (struct XrSet *) obj;
    XR_DCHECK(XR_GC_GET_TYPE(&set->gc) == XR_TSET, "gc_traverse_set: object is not a set");

    // XrSet uses open addressing (NOT XrMap's chained hash)
    if (!set->entries || set->capacity == 0)
        return;

    // entries[] always lives on malloc, not on the per-coro Immix heap,
    // so there is no GC blob to mark for the table itself.

    bool is_weak = (set->flags & XR_SET_FLAG_WEAK) != 0;

    if (is_weak) {
        // Weak set: don't mark elements, add to weak list
        xr_gclist_push(&gc->weak, (XrGCHeader *) set);
    } else {
        // Normal set: mark all elements
        for (uint32_t i = 0; i < set->capacity; i++) {
            XrSetEntry *e = &set->entries[i];
            if (e->state >= XR_SET_VALID) {
                xr_coro_gc_markvalue(gc, e->value);
            }
        }
    }
}

/* ========== Json Traversal ========== */

void xr_coro_gc_traverse_json(XrCoroGC *gc, XrGCHeader *obj) {
    if (!gc || !obj)
        return;
    struct XrJson *json = (struct XrJson *) obj;

    XrayIsolate *X = gc->owner ? xr_coro_get_isolate(gc->owner) : NULL;
    XrShape *shape = xr_json_shape(X, json);
    if (!shape)
        return;

    // Mark in-object fields
    uint16_t in_obj = shape->in_object_capacity;
    uint16_t total = shape->field_count;
    XR_DCHECK(in_obj <= 64, "gc_traverse_json: in_object_capacity too large");
    uint16_t n = (total < in_obj) ? total : in_obj;
    for (uint16_t i = 0; i < n; i++) {
        xr_coro_gc_markvalue(gc, json->fields[i]);
    }

    // Mark overflow fields
    XrJsonOverflow *ov = json->overflow;
    if (ov && total > in_obj) {
        uint16_t ov_count = total - in_obj;
        if (ov_count > ov->length)
            ov_count = ov->length;
        for (uint16_t i = 0; i < ov_count; i++) {
            xr_coro_gc_markvalue(gc, ov->values[i]);
        }
    }
}

/* ========== Closure Traversal ========== */

void xr_gc_traverse_closure(XrCoroGC *gc, XrGCHeader *obj) {
    if (!gc || !obj)
        return;
    struct XrClosure *closure = (struct XrClosure *) obj;

    // Mark flat upvals array (BY_VALUE snapshots may hold GC objects, including cells)
    for (int i = 0; i < closure->upval_count; i++) {
        xr_coro_gc_markvalue(gc, closure->upvals[i]);
    }
}

/* ========== Instance Traversal ========== */

void xr_gc_traverse_instance(XrCoroGC *gc, XrGCHeader *obj) {
    if (!gc || !obj)
        return;
    struct XrInstance *inst = (struct XrInstance *) obj;

    // Note: XrClass is usually in system heap, skip marking

    // Mark all instance fields
    if (inst->klass && inst->klass->field_count > 0) {
        for (uint16_t i = 0; i < inst->klass->field_count; i++) {
            xr_coro_gc_markvalue(gc, inst->fields[i]);
        }
    }
}

/* ========== Iterator Traversal ========== */

void xr_gc_traverse_iterator(XrCoroGC *gc, XrGCHeader *obj) {
    if (!gc || !obj)
        return;
    struct XrIterator *iter = (struct XrIterator *) obj;

    // Mark the source container (map/set/json) by type
    switch (iter->type) {
        case XR_ITERATOR_MAP:
            if (iter->source.map)
                xr_coro_gc_markobject(gc, (XrGCHeader *) iter->source.map);
            break;
        case XR_ITERATOR_SET:
            if (iter->source.set)
                xr_coro_gc_markobject(gc, (XrGCHeader *) iter->source.set);
            break;
        case XR_ITERATOR_JSON:
            if (iter->source.json)
                xr_coro_gc_markobject(gc, (XrGCHeader *) iter->source.json);
            break;
        case XR_ITERATOR_ARRAY:
            if (iter->source.array)
                xr_coro_gc_markobject(gc, (XrGCHeader *) iter->source.array);
            break;
        case XR_ITERATOR_STRING:
            if (iter->source.string)
                xr_coro_gc_markobject(gc, (XrGCHeader *) iter->source.string);
            break;
    }
    // coro is a raw runtime pointer, not a GC object; context is XrayIsolate* or
    // XrSymbolTable* — neither is GC-managed. No further marking needed.
}

/* ========== Cell / Bound Method / Module / Exception / Error / Task ==========
 *
 * These traverse one or two well-known references off the object body.
 * They are split out as plain functions so g_type_ops can dispatch
 * directly without a switch on type. */

void xr_gc_traverse_cell(XrCoroGC *gc, XrGCHeader *obj) {
    // XrCell layout: { XrGCHeader gc; XrValue value; }
    XrValue *cell_val = (XrValue *) ((char *) obj + sizeof(XrGCHeader));
    xr_coro_gc_markvalue(gc, *cell_val);
}

void xr_gc_traverse_bound_method(XrCoroGC *gc, XrGCHeader *obj) {
    // XrBoundMethod layout: { XrGCHeader gc; XrValue receiver; ... }
    // Mark receiver without including vm headers (avoid gc->vm dependency)
    XrValue *receiver = (XrValue *) ((char *) obj + sizeof(XrGCHeader));
    xr_coro_gc_markvalue(gc, *receiver);
}

void xr_gc_traverse_module(XrCoroGC *gc, XrGCHeader *obj) {
    XrModule *mod = (XrModule *) obj;
    if (mod->export_values && mod->export_count > 0) {
        for (uint16_t i = 0; i < mod->export_count; i++) {
            xr_coro_gc_markvalue(gc, mod->export_values[i]);
        }
    }
}

void xr_gc_traverse_exception(XrCoroGC *gc, XrGCHeader *obj) {
    XrException *exc = (XrException *) obj;
    if (exc->message)
        xr_coro_gc_markobject(gc, (XrGCHeader *) exc->message);
    if (exc->file)
        xr_coro_gc_markobject(gc, (XrGCHeader *) exc->file);
    if (exc->stackTrace)
        xr_coro_gc_markobject(gc, (XrGCHeader *) exc->stackTrace);
    xr_coro_gc_markvalue(gc, exc->userData);
}

void xr_gc_traverse_error(XrCoroGC *gc, XrGCHeader *obj) {
    XrError *err = (XrError *) obj;
    if (err->message)
        xr_coro_gc_markobject(gc, (XrGCHeader *) err->message);
}

void xr_gc_traverse_task(XrCoroGC *gc, XrGCHeader *obj) {
    XrTask *task = (XrTask *) obj;
    xr_coro_gc_markvalue(gc, task->result);
    xr_coro_gc_markvalue(gc, task->error);
    // Mark parent-child hierarchy (structured concurrency)
    if (task->parent)
        xr_coro_gc_markobject(gc, (XrGCHeader *) task->parent);
    for (XrTask *child = task->first_child; child; child = child->next_sibling)
        xr_coro_gc_markobject(gc, (XrGCHeader *) child);
    // Mark bidirectional link peers (task.link() API)
    for (XrTaskLink *lk = task->links; lk; lk = lk->next) {
        if (lk->peer)
            xr_coro_gc_markobject(gc, (XrGCHeader *) lk->peer);
    }
    // Mark closures in completion listeners
    for (XrCompletionNode *cn = task->on_completion; cn; cn = cn->next) {
        if (cn->type == XR_COMPLETION_CLOSURE)
            xr_coro_gc_markvalue(gc, cn->as.closure);
    }
}

/* ========== Generic Traversal Dispatcher ==========
 *
 * Compile-time types resolve through g_type_ops in O(1); types whose
 * traverse slot is NULL are leaf objects (range, datetime, channel,
 * coroutine, logger, regex, stringbuilder, enum value/type, ...) and
 * have no children to mark from this side.
 *
 * Extension types (registered via xr_register_extension_traverse) are
 * stored in the per-isolate ext_traverse_funcs table and dispatched
 * here when their type id falls outside the compile-time range. */

void xr_gc_traverse_object(XrCoroGC *gc, XrGCHeader *obj) {
    if (!gc || !obj)
        return;
    XR_DCHECK(obj->type < XGC_MAX_TYPES, "traverse_object: invalid GC type");

    XrGCTraverseFn traverse = g_type_ops[obj->type].traverse;
    if (traverse) {
        traverse(gc, obj);
        return;
    }

    // Extension types: call per-isolate traverse callback if registered
    XrayIsolate *iso = xr_coro_get_isolate(gc->owner);
    if (iso) {
        void *fn = iso->ext_traverse_funcs[obj->type];
        if (fn) {
            ((XrGCTraverseFn) fn)(gc, obj);
        }
    }
}
