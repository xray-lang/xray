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
#include "../../vm/xcontext.h"
#include "../../base/xchecks.h"
#include "../value/xslot_type.h"
#include "../object/xarray.h"
#include "../object/xmap.h"
#include "../object/xset.h"
#include "../object/xjson.h"
#include "../object/xiterator.h"
#include "../object/xexception.h"
#include "../xerror_impl.h"
#include "../class/xclass.h"
#include "../class/xinstance.h"
#include "../../module/xmodule.h"
#include "../../vm/xvm_state_frame.h"
#include "../../coro/xtask.h"

/* ========== Array Traversal ========== */

void xr_gc_traverse_array(XrCoroGC *gc, struct XrArray *arr) {
    if (!gc || !arr) return;
    XR_DCHECK(XR_GC_GET_TYPE(&arr->gc) == XR_TARRAY || XR_GC_GET_TYPE(&arr->gc) == XR_TARRAY_SLICE,
              "gc_traverse_array: object is not an array");
    
    // Mark source array (for slices)
    if (arr->source) {
        xr_coro_gc_markobject(gc, (XrGCHeader*)arr->source);
    }
    
    // Mark data blob if it lives on GC heap (prevents sweep from reclaiming it)
    if (arr->data && arr->data_on_gc_heap) {
        XrGCHeader *blob = ((XrGCHeader*)arr->data) - 1;
        xr_coro_gc_markobject(gc, blob);
    }
    
    // Only XR_ELEM_ANY arrays with GC pointers need element scanning
    if (arr->data && arr->length > 0 && XR_ARRAY_IS_GC_TRACED(arr) && arr->has_gc_ptrs) {
        XR_DCHECK(arr->length <= arr->capacity, "gc_traverse_array: length > capacity");
        XrValue *data = (XrValue*)arr->data;
        for (int32_t i = 0; i < arr->length; i++) {
            xr_coro_gc_markvalue(gc, data[i]);
        }
    }
}

/* ========== Map Traversal ========== */

void xr_gc_traverse_map(XrCoroGC *gc, struct XrMap *map) {
    if (!gc || !map) return;
    XR_DCHECK(XR_GC_GET_TYPE(&map->gc) == XR_TMAP, "gc_traverse_map: object is not a map");
    
    // Skip dummy map
    if (xr_map_isdummy(map)) return;
    
    // Skip uninitialized map (node pointer not yet set)
    if (!map->node) return;
    
    // Mark node blob if it lives on GC heap (prevents sweep from reclaiming it)
    if (map->flags & XR_MAP_FLAG_NODES_ON_GC) {
        XrGCHeader *blob = ((XrGCHeader*)map->node) - 1;
        xr_coro_gc_markobject(gc, blob);
    }
    
    // Check if this is a weak map
    bool is_weak = (map->flags & XR_MAP_FLAG_WEAK) != 0;
    
    if (is_weak) {
        // Weak map: only mark values, not keys
        // Add to weak list for later processing in atomic phase
        xr_gclist_push(&gc->weak, (XrGCHeader*)map);
        
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

void xr_gc_traverse_set(XrCoroGC *gc, struct XrSet *set) {
    if (!gc || !set) return;
    XR_DCHECK(XR_GC_GET_TYPE(&set->gc) == XR_TSET, "gc_traverse_set: object is not a set");
    
    // XrSet uses open addressing (NOT XrMap's chained hash)
    if (!set->entries || set->capacity == 0) return;
    
    // Mark entries blob if it lives on GC heap (prevents sweep from reclaiming it)
    if (set->flags & XR_SET_FLAG_ENTRIES_ON_GC) {
        XrGCHeader *blob = ((XrGCHeader*)set->entries) - 1;
        xr_coro_gc_markobject(gc, blob);
    }
    
    bool is_weak = (set->flags & XR_SET_FLAG_WEAK) != 0;
    
    if (is_weak) {
        // Weak set: don't mark elements, add to weak list
        xr_gclist_push(&gc->weak, (XrGCHeader*)set);
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

void xr_coro_gc_traverse_json(XrCoroGC *gc, struct XrJson *json) {
    if (!gc || !json) return;
    
    XrShape *shape = xr_json_shape(json);
    if (!shape) return;
    
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
        if (ov_count > ov->length) ov_count = ov->length;
        for (uint16_t i = 0; i < ov_count; i++) {
            xr_coro_gc_markvalue(gc, ov->values[i]);
        }
    }
}

/* ========== Closure Traversal ========== */

void xr_gc_traverse_closure(XrCoroGC *gc, struct XrClosure *closure) {
    if (!gc || !closure) return;
    
    // Mark flat upvals array (BY_VALUE snapshots may hold GC objects, including cells)
    for (int i = 0; i < closure->upval_count; i++) {
        xr_coro_gc_markvalue(gc, closure->upvals[i]);
    }
}

/* ========== Context Traversal ========== */

void xr_gc_traverse_context(XrCoroGC *gc, struct XrContext *ctx) {
    if (!gc || !ctx) return;
    
    // Mark parent Context
    if (ctx->parent)
        xr_coro_gc_markobject(gc, (XrGCHeader*)ctx->parent);
    
    // Mark all captured variable slots
    for (int i = 0; i < ctx->slot_count; i++) {
        xr_coro_gc_markvalue(gc, ctx->slots[i]);
    }
}

/* ========== Instance Traversal ========== */

void xr_gc_traverse_instance(XrCoroGC *gc, struct XrInstance *inst) {
    if (!gc || !inst) return;
    
    // Note: XrClass is usually in system heap, skip marking
    
    // Mark all instance fields
    if (inst->klass && inst->klass->field_count > 0) {
        for (uint16_t i = 0; i < inst->klass->field_count; i++) {
            xr_coro_gc_markvalue(gc, inst->fields[i]);
        }
    }
}

/* ========== Iterator Traversal ========== */

void xr_gc_traverse_iterator(XrCoroGC *gc, struct XrIterator *iter) {
    if (!gc || !iter) return;
    
    // Mark the source container (map/set/json) by type
    switch (iter->type) {
        case XR_ITERATOR_MAP:
            if (iter->source.map)
                xr_coro_gc_markobject(gc, (XrGCHeader*)iter->source.map);
            break;
        case XR_ITERATOR_SET:
            if (iter->source.set)
                xr_coro_gc_markobject(gc, (XrGCHeader*)iter->source.set);
            break;
        case XR_ITERATOR_JSON:
            if (iter->source.json)
                xr_coro_gc_markobject(gc, (XrGCHeader*)iter->source.json);
            break;
    }
    // coro is a raw runtime pointer, not a GC object; context is XrayIsolate* or
    // XrSymbolTable* — neither is GC-managed. No further marking needed.
}

/* ========== Generic Traversal Dispatcher ========== */

void xr_gc_traverse_object(XrCoroGC *gc, XrGCHeader *obj) {
    if (!gc || !obj) return;
    XR_DCHECK(obj->type < XGC_MAX_TYPES, "traverse_object: invalid GC type");
    
    switch (obj->type) {
        case XR_TARRAY:
        case XR_TARRAY_SLICE:
            xr_gc_traverse_array(gc, (struct XrArray*)obj);
            break;
            
        case XR_TMAP:
            xr_gc_traverse_map(gc, (struct XrMap*)obj);
            break;
            
        case XR_TSET:
            xr_gc_traverse_set(gc, (struct XrSet*)obj);
            break;
            
        case XR_TJSON:
            xr_coro_gc_traverse_json(gc, (struct XrJson*)obj);
            break;
            
        case XR_TFUNCTION:
            xr_gc_traverse_closure(gc, (struct XrClosure*)obj);
            break;
            
        case XR_TINSTANCE:
            xr_gc_traverse_instance(gc, (struct XrInstance*)obj);
            break;
            
        case XR_TITERATOR:
            xr_gc_traverse_iterator(gc, (struct XrIterator*)obj);
            break;
            
        case XR_TCONTEXT:
            xr_gc_traverse_context(gc, (struct XrContext*)obj);
            break;

        case XR_TCELL: {
            // XrCell layout: { XrGCHeader gc; XrValue value; }
            XrValue *cell_val = (XrValue*)((char*)obj + sizeof(XrGCHeader));
            xr_coro_gc_markvalue(gc, *cell_val);
            break;
        }
        
        case XR_TBOUND_METHOD: {
            // XrBoundMethod layout: { XrGCHeader gc; XrValue receiver; ... }
            // Mark receiver without including vm headers (avoid gc->vm dependency)
            XrValue *receiver = (XrValue*)((char*)obj + sizeof(XrGCHeader));
            xr_coro_gc_markvalue(gc, *receiver);
            break;
        }
        
        case XR_TMODULE: {
            XrModule *mod = (XrModule*)obj;
            if (mod->export_values && mod->export_count > 0) {
                for (uint16_t i = 0; i < mod->export_count; i++) {
                    xr_coro_gc_markvalue(gc, mod->export_values[i]);
                }
            }
            break;
        }
        
        case XR_TEXCEPTION: {
            XrException *exc = (XrException*)obj;
            if (exc->message) xr_coro_gc_markobject(gc, (XrGCHeader*)exc->message);
            if (exc->file) xr_coro_gc_markobject(gc, (XrGCHeader*)exc->file);
            if (exc->stackTrace) xr_coro_gc_markobject(gc, (XrGCHeader*)exc->stackTrace);
            xr_coro_gc_markvalue(gc, exc->userData);
            break;
        }

        case XR_TERROR: {
            XrError *err = (XrError*)obj;
            if (err->message) xr_coro_gc_markobject(gc, (XrGCHeader*)err->message);
            break;
        }


        case XR_TLOGGER:
            // XrLoggerRef only holds a malloc pointer, no GC references
            break;
        
        case XR_TRANGE:
            // Range only holds int64 fields, no GC-traced children
            break;
        
        case XR_TDATETIME:
            // DateTime only holds primitive fields, no GC-traced children
            break;

        case XR_TTASK: {
            XrTask *task = (XrTask*)obj;
            xr_coro_gc_markvalue(gc, task->result);
            xr_coro_gc_markvalue(gc, task->error);
            // Mark parent-child hierarchy (structured concurrency)
            if (task->parent)
                xr_coro_gc_markobject(gc, (XrGCHeader*)task->parent);
            for (XrTask *child = task->first_child; child; child = child->next_sibling)
                xr_coro_gc_markobject(gc, (XrGCHeader*)child);
            // Mark bidirectional link peers (task.link() API)
            for (XrTaskLink *lk = task->links; lk; lk = lk->next) {
                if (lk->peer)
                    xr_coro_gc_markobject(gc, (XrGCHeader*)lk->peer);
            }
            // Mark closures in completion listeners
            for (XrCompletionNode *cn = task->on_completion; cn; cn = cn->next) {
                if (cn->type == XR_COMPLETION_CLOSURE)
                    xr_coro_gc_markvalue(gc, cn->as.closure);
            }
            break;
        }
        
        default:
            // Unknown type or no refs, nothing to traverse
            break;
    }
}
