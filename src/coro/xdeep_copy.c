/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdeep_copy.c - Deep copy implementation
 */

#include "xdeep_copy.h"
#include "../base/xchecks.h"
#include "../runtime/xshared.h"
#include "../runtime/value/xvalue.h"
#include "../runtime/gc/xgc.h"
#include "../runtime/gc/xgc_internal.h"
#include "../runtime/gc/xcoro_gc.h"
#include "../runtime/object/xarray.h"
#include "../runtime/object/xmap.h"
#include "../runtime/object/xstring.h"
#include "../runtime/object/xset.h"
#include "../runtime/object/xjson.h"
#include "../runtime/class/xinstance.h"
#include "xcoroutine.h"
#include "../runtime/xisolate_api.h"
#include "../vm/xcontext.h"
#include "../base/xmalloc.h"
#include <stdlib.h>
#include <string.h>
#include "../base/xhash.h"

XrCopyKind xr_value_copy_kind(XrValue value) {
    if (XR_IS_NUM(value) || XR_IS_BOOL(value) || XR_IS_NULL(value)) return XR_COPY_IMMEDIATE;
    if (!XR_IS_PTR(value)) return XR_COPY_IMMEDIATE;
    
    uint8_t type = XR_HEAP_TYPE(value);
    switch (type) {
        case XR_TSTRING: return XR_COPY_SHARED;
        case XR_TARRAY:
        case XR_TARRAY_SLICE:
        case XR_TMAP:
        case XR_TFUNCTION: return XR_COPY_DEEP;
        default: return XR_COPY_DEEP;
    }
}

#define SEEN_BUCKET_COUNT 32

static inline int seen_hash(void *ptr) {
    return (int)(xr_hash_int((int)(uintptr_t)ptr) % SEEN_BUCKET_COUNT);
}

void xr_copy_context_init(XrCopyContext *ctx, struct XrayIsolate *X, struct XrGC *dst_gc) {
    XR_DCHECK(ctx != NULL, "copy_context_init: NULL ctx");
    XR_DCHECK(X != NULL, "copy_context_init: NULL isolate");
    ctx->X = X;
    ctx->dst_gc = dst_gc;
    ctx->dst_coro_gc = NULL;
    ctx->buckets = NULL;
    ctx->bucket_count = 0;
    ctx->objects_copied = 0;
    ctx->arena_head = NULL;
}

// Unified allocation: prefer Immix heap, fallback to fixed GC
static inline void* copy_ctx_alloc(XrCopyContext *ctx, size_t size, uint8_t type) {
    if (ctx->dst_coro_gc) {
        return xr_coro_gc_newobj(ctx->dst_coro_gc, type, size);
    }
    return xr_gc_alloc(ctx->dst_gc, size, type);
}

void xr_copy_context_cleanup(XrCopyContext *ctx) {
    // Free arena blocks (bulk deallocation, no per-entry free)
    XrSeenArena *arena = ctx->arena_head;
    while (arena) {
        XrSeenArena *next = arena->next;
        xr_free(arena);
        arena = next;
    }
    ctx->arena_head = NULL;
    if (ctx->buckets) {
        xr_free(ctx->buckets);
        ctx->buckets = NULL;
        ctx->bucket_count = 0;
    }
}

static XrValue xr_copy_context_lookup(XrCopyContext *ctx, void *src) {
    if (!ctx->buckets) return XR_NULL_VAL;
    int idx = seen_hash(src);
    for (XrSeenEntry *e = ctx->buckets[idx]; e; e = e->next) {
        if (e->src == src) return e->dst;
    }
    return XR_NULL_VAL;
}

// Allocate XrSeenEntry from arena (one malloc per 64 entries)
static inline XrSeenEntry *seen_arena_alloc(XrCopyContext *ctx) {
    XrSeenArena *a = ctx->arena_head;
    if (!a || a->used >= XR_SEEN_ARENA_BLOCK_SIZE) {
        a = (XrSeenArena *)xr_malloc(sizeof(XrSeenArena));
        if (!a) return NULL;
        a->used = 0;
        a->next = ctx->arena_head;
        ctx->arena_head = a;
    }
    return &a->entries[a->used++];
}

static void xr_copy_context_record(XrCopyContext *ctx, void *src, XrValue dst) {
    if (!ctx->buckets) {
        ctx->buckets = (XrSeenEntry **)xr_calloc(SEEN_BUCKET_COUNT, sizeof(XrSeenEntry *));
        if (!ctx->buckets) return;
        ctx->bucket_count = SEEN_BUCKET_COUNT;
    }
    XrSeenEntry *entry = seen_arena_alloc(ctx);
    if (!entry) return;
    entry->src = src;
    entry->dst = dst;
    int idx = seen_hash(src);
    entry->next = ctx->buckets[idx];
    ctx->buckets[idx] = entry;
}

static XrValue xr_deep_copy_array_with_ctx(XrCopyContext *ctx, XrArray *array) {
    if (!array || !ctx->dst_gc) return XR_NULL_VAL;
    XrValue cached = xr_copy_context_lookup(ctx, array);
    if (!XR_IS_NULL(cached)) return cached;
    
    int32_t length = array->length;
    XrArray *new_arr = (XrArray *)copy_ctx_alloc(ctx, sizeof(XrArray), XR_TARRAY);
    if (!new_arr) return XR_NULL_VAL;
    
    new_arr->length = length;
    new_arr->capacity = length > 0 ? length : XR_ARRAY_INIT_CAPACITY;
    XR_DCHECK(new_arr->length <= new_arr->capacity, "deep_copy_array: length > capacity");
    new_arr->source = NULL;
    new_arr->elem_type = array->elem_type;
    new_arr->elem_size = array->elem_size;
    new_arr->elem_tid = array->elem_tid;
    new_arr->has_gc_ptrs = array->has_gc_ptrs;
    new_arr->data_on_gc_heap = 0;  // data allocated via xr_malloc (system heap)
    memset(new_arr->_pad, 0, sizeof(new_arr->_pad));
    
    size_t alloc_size = (size_t)new_arr->elem_size * new_arr->capacity;
    if (new_arr->capacity > 0) {
        new_arr->data = xr_malloc(alloc_size);
        if (!new_arr->data) return XR_NULL_VAL;
        memset(new_arr->data, 0, alloc_size);
        /* Notify destination GC about the external malloc'd data buffer
         * so sweep's sub_external (via xr_gc_destroy_array) balances. */
        if (ctx->dst_coro_gc) {
            xr_gc_add_external(ctx->dst_coro_gc, (int64_t)alloc_size);
        }
    } else {
        new_arr->data = NULL;
    }
    
    XrValue result = XR_FROM_PTR(new_arr);
    xr_copy_context_record(ctx, array, result);
    ctx->objects_copied++;
    
    if (array->elem_type == XR_ELEM_ANY) {
        // Deep copy each element
        XrValue *src = (XrValue*)array->data;
        XrValue *dst = (XrValue*)new_arr->data;
        for (int32_t i = 0; i < length; i++) dst[i] = xr_deep_copy_with_ctx(ctx, src[i]);
    } else {
        // Typed array: memcpy raw data (no GC pointers)
        if (length > 0) memcpy(new_arr->data, array->data, (size_t)length * array->elem_size);
    }
    return result;
}

static XrValue xr_deep_copy_map_with_ctx(XrCopyContext *ctx, XrMap *map) {
    if (!map || !ctx->dst_gc) return XR_NULL_VAL;
    XrValue cached = xr_copy_context_lookup(ctx, map);
    if (!XR_IS_NULL(cached)) return cached;
    
    XrMap *new_map = (XrMap *)copy_ctx_alloc(ctx, sizeof(XrMap), XR_TMAP);
    if (!new_map) return XR_NULL_VAL;
    
    new_map->count = 0;
    new_map->flags = 0;
    new_map->key_tid = map->key_tid;
    new_map->value_tid = map->value_tid;
    
    if (xr_map_isdummy(map)) {
        new_map->lsizenode = 0;
        new_map->node = &xr_map_dummynode;
        new_map->lastfree = NULL;
        new_map->flags |= XR_MAP_FLAG_DUMMY;
        return XR_FROM_PTR(new_map);
    }
    
    uint32_t size = xr_map_sizenode(map);
    new_map->lsizenode = map->lsizenode;
    new_map->node = (XrMapNode *)xr_malloc(sizeof(XrMapNode) * size);
    if (!new_map->node) return XR_NULL_VAL;
    
    for (uint32_t i = 0; i < size; i++) {
        new_map->node[i].key_tt = XR_MAP_NODE_NIL_KEY;
        new_map->node[i].next = 0;
    }
    new_map->lastfree = &new_map->node[size - 1];
    
    XrValue result = XR_FROM_PTR(new_map);
    xr_copy_context_record(ctx, map, result);
    ctx->objects_copied++;
    
    for (uint32_t i = 0; i < size; i++) {
        XrMapNode *node = &map->node[i];
        if (!XR_MAP_NODE_EMPTY(node)) {
            xr_map_set(new_map, xr_deep_copy_with_ctx(ctx, node->key), xr_deep_copy_with_ctx(ctx, node->value));
        }
    }
    return result;
}

static XrValue deep_copy_context_chain(XrCopyContext *ctx, XrContext *src_context);

static XrValue xr_deep_copy_closure_with_ctx(XrCopyContext *ctx, XrClosure *closure) {
    if (!closure || !ctx->dst_gc) return XR_NULL_VAL;
    XrValue cached = xr_copy_context_lookup(ctx, closure);
    if (!XR_IS_NULL(cached)) return cached;
    
    size_t alloc_size = sizeof(XrClosure) + closure->upval_count * sizeof(XrValue);
    XrClosure *new_closure = (XrClosure *)copy_ctx_alloc(ctx, alloc_size, XR_TFUNCTION);
    if (!new_closure) return XR_NULL_VAL;
    
    new_closure->proto = closure->proto;
    new_closure->upval_count = closure->upval_count;
    
    XrValue result = XR_FROM_PTR(new_closure);
    xr_copy_context_record(ctx, closure, result);
    ctx->objects_copied++;
    
    // Deep copy flat upvals (cells and values)
    for (int i = 0; i < closure->upval_count; i++) {
        new_closure->upvals[i] = xr_deep_copy_with_ctx(ctx, closure->upvals[i]);
    }
    return result;
}

// Deep copy a Context object and its parent chain
static XrValue deep_copy_context_chain(XrCopyContext *ctx, XrContext *src_context) {
    if (!src_context) return XR_NULL_VAL;
    
    XrValue cached = xr_copy_context_lookup(ctx, src_context);
    if (!XR_IS_NULL(cached)) return cached;
    
    size_t size = sizeof(XrContext) + src_context->slot_count * sizeof(XrValue);
    XrContext *new_ctx = (XrContext *)copy_ctx_alloc(ctx, size, XR_TCONTEXT);
    if (!new_ctx) return XR_NULL_VAL;
    
    new_ctx->slot_count = src_context->slot_count;
    new_ctx->parent = NULL;
    
    XrValue result = XR_FROM_PTR(new_ctx);
    xr_copy_context_record(ctx, src_context, result);
    ctx->objects_copied++;
    
    // Deep copy parent chain
    if (src_context->parent) {
        XrValue parent_val = deep_copy_context_chain(ctx, src_context->parent);
        new_ctx->parent = XR_IS_NULL(parent_val) ? NULL : (XrContext*)parent_val.ptr;
    }
    
    // Deep copy all slots
    for (int i = 0; i < src_context->slot_count; i++) {
        new_ctx->slots[i] = xr_deep_copy_with_ctx(ctx, src_context->slots[i]);
    }
    
    return result;
}

static XrValue xr_deep_copy_set_with_ctx(XrCopyContext *ctx, XrSet *set) {
    if (!set || !ctx->dst_gc) return XR_NULL_VAL;
    XrValue cached = xr_copy_context_lookup(ctx, set);
    if (!XR_IS_NULL(cached)) return cached;
    
    XrSet *new_set = (XrSet *)copy_ctx_alloc(ctx, sizeof(XrSet), XR_TSET);
    if (!new_set) return XR_NULL_VAL;
    xr_set_init_inplace(new_set);
    new_set->elem_tid = set->elem_tid;
    
    XrValue result = XR_FROM_PTR(new_set);
    xr_copy_context_record(ctx, set, result);
    ctx->objects_copied++;
    for (uint32_t i = 0; i < set->capacity; i++) {
        XrSetEntry *entry = &set->entries[i];
        if (entry->state & XR_SET_VALID) xr_set_add(new_set, xr_deep_copy_with_ctx(ctx, entry->value));
    }
    return result;
}


static XrValue xr_deep_copy_instance_with_ctx(XrCopyContext *ctx, XrInstance *inst) {
    if (!inst || !ctx->dst_gc) return XR_NULL_VAL;
    XrValue cached = xr_copy_context_lookup(ctx, inst);
    if (!XR_IS_NULL(cached)) return cached;
    
    XrClass *cls = inst->klass;
    uint32_t field_count = xr_class_instance_field_count(cls);
    
    XrInstance *new_inst = (XrInstance *)copy_ctx_alloc(ctx, xr_instance_size(cls), XR_TINSTANCE);
    if (!new_inst) return XR_NULL_VAL;
    xr_instance_init_inplace(new_inst, cls);
    // Propagate reified type args from gc.extra
    new_inst->gc.extra = (new_inst->gc.extra & 0x01) | (inst->gc.extra & ~0x01);
    
    XrValue result = XR_FROM_PTR(new_inst);
    xr_copy_context_record(ctx, inst, result);
    ctx->objects_copied++;
    
    // Fast path: flat-copyable struct — memcpy all fields at once
    if ((cls->flags & XR_CLASS_FLAT_COPYABLE) && field_count > 0) {
        memcpy(new_inst->fields, inst->fields, sizeof(XrValue) * field_count);
        return result;
    }
    
    for (uint32_t i = 0; i < field_count; i++) {
        new_inst->fields[i] = xr_deep_copy_with_ctx(ctx, inst->fields[i]);
    }
    return result;
}

static XrValue xr_deep_copy_json_with_ctx(XrCopyContext *ctx, XrJson *json) {
    if (!json || !ctx->dst_gc) return XR_NULL_VAL;
    XrValue cached = xr_copy_context_lookup(ctx, json);
    if (!XR_IS_NULL(cached)) return cached;
    
    XrShape *shape = xr_json_shape(json);
    int field_count = shape->field_count;
    
    size_t size = xr_json_size(shape->in_object_capacity);
    XrJson *new_json = (XrJson *)copy_ctx_alloc(ctx, size, XR_TJSON);
    if (!new_json) return XR_NULL_VAL;
    xr_json_init_inplace(new_json, shape);
    
    XrValue result = XR_FROM_PTR(new_json);
    xr_copy_context_record(ctx, json, result);
    ctx->objects_copied++;
    
    // Fast path: compact value-layout (no GC pointers, no overflow) → memcpy fields
    if (shape->is_value_layout && !json->overflow) {
        uint16_t n = (field_count < shape->in_object_capacity) ? field_count : shape->in_object_capacity;
        memcpy(new_json->fields, json->fields, n * sizeof(XrValue));
        return result;
    }
    
    // Copy in-object fields
    uint16_t in_obj = shape->in_object_capacity;
    uint16_t n = (field_count < in_obj) ? field_count : in_obj;
    for (int i = 0; i < n; i++) {
        new_json->fields[i] = xr_deep_copy_with_ctx(ctx, json->fields[i]);
    }
    // Copy overflow fields
    if (field_count > in_obj && json->overflow) {
        uint16_t ov_count = field_count - in_obj;
        XrJsonOverflow *src_ov = json->overflow;
        size_t ov_size = sizeof(XrJsonOverflow) + src_ov->capacity * sizeof(XrValue);
        XrJsonOverflow *dst_ov = (XrJsonOverflow *)xr_malloc(ov_size);
        if (dst_ov) {
            dst_ov->capacity = src_ov->capacity;
            dst_ov->length = src_ov->length;
            XR_DCHECK(dst_ov->length <= dst_ov->capacity, "deep_copy_json: overflow length > capacity");
            dst_ov->_pad = 0;
            for (uint16_t i = 0; i < ov_count && i < src_ov->length; i++) {
                dst_ov->values[i] = xr_deep_copy_with_ctx(ctx, src_ov->values[i]);
            }
            for (uint16_t i = ov_count; i < dst_ov->capacity; i++) {
                dst_ov->values[i] = xr_null();
            }
            new_json->overflow = dst_ov;
        }
    }
    return result;
}

XrValue xr_deep_copy_with_ctx(XrCopyContext *ctx, XrValue value) {
    XR_DCHECK(ctx != NULL, "deep_copy_with_ctx: NULL context");
    if (!XR_IS_PTR(value)) return value;
    XrGCHeader *obj = XR_VALUE_GCPTR(value);
    if (!obj) return value;
    if (XR_GC_IS_SHARED(obj)) { xr_shared_incref(obj); return value; }
    
    uint8_t type = XR_GC_GET_TYPE(obj);
    switch (type) {
        case XR_TSTRING: return value;
        case XR_TARRAY:
        case XR_TARRAY_SLICE: return xr_deep_copy_array_with_ctx(ctx, (XrArray *)obj);
        case XR_TMAP: return xr_deep_copy_map_with_ctx(ctx, (XrMap *)obj);
        case XR_TFUNCTION: return xr_deep_copy_closure_with_ctx(ctx, (XrClosure *)obj);
        case XR_TSET: return xr_deep_copy_set_with_ctx(ctx, (XrSet *)obj);
        case XR_TINSTANCE: return xr_deep_copy_instance_with_ctx(ctx, (XrInstance *)obj);
        case XR_TJSON: return xr_deep_copy_json_with_ctx(ctx, (XrJson *)obj);
        case XR_TDATETIME: {
            // DateTime has no child GC references, shallow copy suffices
            XrGCHeader *copy = (XrGCHeader*)copy_ctx_alloc(ctx, obj->objsize, XR_TDATETIME);
            memcpy((char*)copy + sizeof(XrGCHeader),
                   (char*)obj + sizeof(XrGCHeader),
                   obj->objsize - sizeof(XrGCHeader));
            return XR_FROM_PTR(copy);
        }
        default: return value;
    }
}

XrValue xr_deep_copy(struct XrayIsolate *X, XrValue value, struct XrGC *dst_gc) {
    XR_DCHECK(X != NULL, "deep_copy: NULL isolate");
    if (xr_value_copy_kind(value) != XR_COPY_DEEP) return value;
    if (!dst_gc) dst_gc = xr_isolate_get_gc(X);
    XrCopyContext ctx;
    xr_copy_context_init(&ctx, X, dst_gc);
    XrValue result = xr_deep_copy_with_ctx(&ctx, value);
    xr_copy_context_cleanup(&ctx);
    return result;
}

XrValue xr_deep_copy_counted(struct XrayIsolate *X, XrValue value, struct XrGC *dst_gc, int *out_count) {
    XR_DCHECK(X != NULL, "deep_copy_counted: NULL isolate");
    if (xr_value_copy_kind(value) != XR_COPY_DEEP) {
        if (out_count) *out_count = 0;
        return value;
    }
    if (!dst_gc) dst_gc = xr_isolate_get_gc(X);
    XrCopyContext ctx;
    xr_copy_context_init(&ctx, X, dst_gc);
    XrValue result = xr_deep_copy_with_ctx(&ctx, value);
    if (out_count) *out_count = ctx.objects_copied;
    xr_copy_context_cleanup(&ctx);
    return result;
}

XrValue xr_deep_copy_to_coro(struct XrayIsolate *X, XrValue value, struct XrCoroutine *dst_coro) {
    XR_DCHECK(X != NULL, "deep_copy_to_coro: NULL isolate");
    if (!XR_IS_PTR(value)) return value;
    // Shared objects (channel, etc): just increment refcount, no copy needed
    XrGCHeader *obj = XR_VALUE_GCPTR(value);
    if (obj && XR_GC_IS_SHARED(obj)) { xr_shared_incref(obj); return value; }
    if (xr_value_copy_kind(value) != XR_COPY_DEEP) return value;
    // Deep copy to destination coroutine's Immix heap when available
    if (dst_coro && dst_coro->coro_gc) {
        XrCopyContext ctx;
        xr_copy_context_init(&ctx, X, xr_isolate_get_gc(X));
        ctx.dst_coro_gc = dst_coro->coro_gc;
        XrValue result = xr_deep_copy_with_ctx(&ctx, value);
        xr_copy_context_cleanup(&ctx);
        return result;
    }
    return xr_deep_copy(X, value, xr_isolate_get_gc(X));
}

XrValue xr_deep_copy_to_coro_counted(struct XrayIsolate *X, XrValue value,
                                      struct XrCoroutine *dst_coro, int *out_count) {
    if (xr_value_copy_kind(value) != XR_COPY_DEEP) {
        if (out_count) *out_count = 0;
        return value;
    }
    if (dst_coro && dst_coro->coro_gc) {
        XrCopyContext ctx;
        xr_copy_context_init(&ctx, X, xr_isolate_get_gc(X));
        ctx.dst_coro_gc = dst_coro->coro_gc;
        XrValue result = xr_deep_copy_with_ctx(&ctx, value);
        if (out_count) *out_count = ctx.objects_copied;
        xr_copy_context_cleanup(&ctx);
        return result;
    }
    return xr_deep_copy_counted(X, value, xr_isolate_get_gc(X), out_count);
}

XrValue xr_deep_copy_array(struct XrayIsolate *X, struct XrArray *array, struct XrGC *dst_gc) {
    XR_DCHECK(X != NULL, "deep_copy_array: NULL isolate");
    if (!array) return XR_NULL_VAL;
    if (!dst_gc) dst_gc = xr_isolate_get_gc(X);
    XrCopyContext ctx;
    xr_copy_context_init(&ctx, X, dst_gc);
    XrValue result = xr_deep_copy_array_with_ctx(&ctx, array);
    xr_copy_context_cleanup(&ctx);
    return result;
}

XrValue xr_deep_copy_map(struct XrayIsolate *X, struct XrMap *map, struct XrGC *dst_gc) {
    XR_DCHECK(X != NULL, "deep_copy_map: NULL isolate");
    if (!map) return XR_NULL_VAL;
    if (!dst_gc) dst_gc = xr_isolate_get_gc(X);
    XrCopyContext ctx;
    xr_copy_context_init(&ctx, X, dst_gc);
    XrValue result = xr_deep_copy_map_with_ctx(&ctx, map);
    xr_copy_context_cleanup(&ctx);
    return result;
}

XrValue xr_deep_copy_closure(struct XrayIsolate *X, struct XrClosure *closure, struct XrGC *dst_gc) {
    XR_DCHECK(X != NULL, "deep_copy_closure: NULL isolate");
    if (!closure) return XR_NULL_VAL;
    if (!dst_gc) dst_gc = xr_isolate_get_gc(X);
    XrCopyContext ctx;
    xr_copy_context_init(&ctx, X, dst_gc);
    XrValue result = xr_deep_copy_closure_with_ctx(&ctx, (XrClosure *)closure);
    xr_copy_context_cleanup(&ctx);
    return result;
}

#include "../runtime/gc/xsystem_heap.h"
#include "../runtime/object/xstringbuilder.h"

bool xr_can_relocate(XrValue value) {
    if (!XR_IS_PTR(value)) return false;
    XrGCHeader *obj = XR_VALUE_GCPTR(value);
    if (!obj) return false;
    if (XR_GC_IS_SHARED(obj)) return false;
    if (XR_GC_GET_TYPE(obj) == XR_TSTRING) return false;
    return true;
}

static XrValue deep_copy_array_to_shared(struct XrayIsolate *X, XrArray *array) {
    if (!array || !xr_isolate_get_sys_heap(X)) return XR_NULL_VAL;
    int32_t length = array->length;
    XrArray *new_arr = (XrArray*)xr_sysheap_alloc_shared(xr_isolate_get_sys_heap(X), sizeof(XrArray), XR_TARRAY);
    if (!new_arr) return XR_NULL_VAL;
    xr_array_init_inplace(new_arr, length > 0 ? length : 4, array->elem_type);
    XR_GC_SET_STORAGE(&new_arr->gc, XR_GC_STORAGE_SHARED);
    xr_shared_set_refc(&new_arr->gc, 1);
    if (array->elem_type == XR_ELEM_ANY) {
        XrValue *src = (XrValue*)array->data;
        for (int32_t i = 0; i < length; i++) xr_array_push(new_arr, xr_to_shared(X, src[i]));
    } else {
        // Typed array: memcpy raw data
        if (length > 0) {
            xr_array_ensure_capacity(new_arr, length);
            memcpy(new_arr->data, array->data, (size_t)length * array->elem_size);
            new_arr->length = length;
        }
    }
    return XR_FROM_PTR(new_arr);
}

static XrValue deep_copy_map_to_shared(struct XrayIsolate *X, XrMap *map) {
    if (!map || !xr_isolate_get_sys_heap(X)) return XR_NULL_VAL;
    XrMap *new_map = (XrMap*)xr_sysheap_alloc_shared(xr_isolate_get_sys_heap(X), sizeof(XrMap), XR_TMAP);
    if (!new_map) return XR_NULL_VAL;
    xr_map_init_inplace(new_map, 8);
    new_map->key_tid = map->key_tid;
    new_map->value_tid = map->value_tid;
    XR_GC_SET_STORAGE(&new_map->gc, XR_GC_STORAGE_SHARED);
    xr_shared_set_refc(&new_map->gc, 1);
    if (!xr_map_isdummy(map)) {
        uint32_t map_size = xr_map_sizenode(map);
        for (uint32_t i = 0; i < map_size; i++) {
            XrMapNode *node = &map->node[i];
            if (!XR_MAP_NODE_EMPTY(node)) xr_map_set(new_map, xr_to_shared(X, node->key), xr_to_shared(X, node->value));
        }
    }
    return XR_FROM_PTR(new_map);
}

static XrValue deep_copy_set_to_shared(struct XrayIsolate *X, XrSet *set) {
    if (!set || !xr_isolate_get_sys_heap(X)) return XR_NULL_VAL;
    XrSet *new_set = (XrSet*)xr_sysheap_alloc_shared(xr_isolate_get_sys_heap(X), sizeof(XrSet), XR_TSET);
    if (!new_set) return XR_NULL_VAL;
    xr_set_init_inplace(new_set);
    new_set->elem_tid = set->elem_tid;
    XR_GC_SET_STORAGE(&new_set->gc, XR_GC_STORAGE_SHARED);
    xr_shared_set_refc(&new_set->gc, 1);
    for (uint32_t i = 0; i < set->capacity; i++) {
        XrSetEntry *entry = &set->entries[i];
        if (entry->state & XR_SET_VALID) xr_set_add(new_set, xr_to_shared(X, entry->value));
    }
    return XR_FROM_PTR(new_set);
}


static XrValue deep_copy_instance_to_shared(struct XrayIsolate *X, XrInstance *inst) {
    if (!inst || !xr_isolate_get_sys_heap(X)) return XR_NULL_VAL;
    XrClass *cls = inst->klass;
    XrInstance *new_inst = (XrInstance*)xr_sysheap_alloc_shared(xr_isolate_get_sys_heap(X), xr_instance_size(cls), XR_TINSTANCE);
    if (!new_inst) return XR_NULL_VAL;
    xr_instance_init_inplace(new_inst, cls);
    XR_GC_SET_STORAGE(&new_inst->gc, XR_GC_STORAGE_SHARED);
    // Propagate reified type args from gc.extra
    new_inst->gc.extra = (new_inst->gc.extra & 0x01) | (inst->gc.extra & ~0x01);
    xr_shared_set_refc(&new_inst->gc, 1);
    uint32_t field_count = xr_class_instance_field_count(cls);
    for (uint32_t i = 0; i < field_count; i++) new_inst->fields[i] = xr_to_shared(X, inst->fields[i]);
    return XR_FROM_PTR(new_inst);
}

static XrValue deep_copy_json_to_shared(struct XrayIsolate *X, XrJson *json) {
    if (!json || !xr_isolate_get_sys_heap(X)) return XR_NULL_VAL;
    XrShape *shape = xr_json_shape(json);
    uint16_t in_obj = shape->in_object_capacity;
    uint16_t field_count = shape->field_count;
    size_t size = xr_json_size(in_obj);
    XrJson *new_json = (XrJson*)xr_sysheap_alloc_shared(xr_isolate_get_sys_heap(X), size, XR_TJSON);
    if (!new_json) return XR_NULL_VAL;
    xr_json_init_inplace(new_json, shape);
    XR_GC_SET_STORAGE(&new_json->gc, XR_GC_STORAGE_SHARED);
    xr_shared_set_refc(&new_json->gc, 1);
    // Copy in-object fields
    uint16_t n = (field_count < in_obj) ? field_count : in_obj;
    for (uint16_t i = 0; i < n; i++) {
        new_json->fields[i] = xr_to_shared(X, json->fields[i]);
    }
    // Copy overflow fields
    if (field_count > in_obj && json->overflow) {
        uint16_t ov_count = field_count - in_obj;
        XrJsonOverflow *src_ov = json->overflow;
        size_t ov_size = sizeof(XrJsonOverflow) + src_ov->capacity * sizeof(XrValue);
        XrJsonOverflow *dst_ov = (XrJsonOverflow *)xr_malloc(ov_size);
        if (dst_ov) {
            dst_ov->capacity = src_ov->capacity;
            dst_ov->length = src_ov->length;
            XR_DCHECK(dst_ov->length <= dst_ov->capacity, "to_shared_json: overflow length > capacity");
            dst_ov->_pad = 0;
            for (uint16_t i = 0; i < ov_count && i < src_ov->length; i++) {
                dst_ov->values[i] = xr_to_shared(X, src_ov->values[i]);
            }
            for (uint16_t i = ov_count; i < dst_ov->capacity; i++) {
                dst_ov->values[i] = xr_null();
            }
            new_json->overflow = dst_ov;
        }
    }
    return XR_FROM_PTR(new_json);
}

static XrValue deep_copy_stringbuilder_to_shared(struct XrayIsolate *X, XrStringBuilder *sb) {
    if (!sb || !xr_isolate_get_sys_heap(X)) return XR_NULL_VAL;
    XrStringBuilder *new_sb = (XrStringBuilder*)xr_sysheap_alloc_shared(
        xr_isolate_get_sys_heap(X), sizeof(XrStringBuilder), XR_TSTRINGBUILDER);
    if (!new_sb) return XR_NULL_VAL;
    xr_stringbuilder_init_inplace(new_sb);
    XR_GC_SET_STORAGE(&new_sb->gc, XR_GC_STORAGE_SHARED);
    xr_shared_set_refc(&new_sb->gc, 1);
    // Copy buffer contents
    if (sb->buffer && sb->buffer->length > 0) {
        xr_strbuf_append_cstr(new_sb->buffer, sb->buffer->data, sb->buffer->length);
    }
    return XR_FROM_PTR(new_sb);
}

static XrValue deep_copy_closure_to_shared(struct XrayIsolate *X, XrClosure *closure) {
    if (!closure || !xr_isolate_get_sys_heap(X)) return XR_NULL_VAL;
    XrClosure *new_cl = (XrClosure*)xr_sysheap_alloc_shared(
        xr_isolate_get_sys_heap(X), sizeof(XrClosure), XR_TFUNCTION);
    if (!new_cl) return XR_NULL_VAL;
    new_cl->proto = closure->proto;
    new_cl->upval_count = 0;  // shared closures don't carry upvals (captured via shared_array)
    XR_GC_SET_STORAGE(&new_cl->gc, XR_GC_STORAGE_SHARED);
    xr_shared_set_refc(&new_cl->gc, 1);
    return XR_FROM_PTR(new_cl);
}

static XrValue deep_copy_datetime_to_shared(struct XrayIsolate *X, XrGCHeader *obj) {
    if (!obj || !xr_isolate_get_sys_heap(X)) return XR_NULL_VAL;
    XrGCHeader *new_dt = (XrGCHeader*)xr_sysheap_alloc_shared(
        xr_isolate_get_sys_heap(X), obj->objsize, XR_TDATETIME);
    if (!new_dt) return XR_NULL_VAL;
    // DateTime has no GC child references, memcpy the payload
    memcpy((char*)new_dt + sizeof(XrGCHeader),
           (char*)obj + sizeof(XrGCHeader),
           obj->objsize - sizeof(XrGCHeader));
    XR_GC_SET_STORAGE(new_dt, XR_GC_STORAGE_SHARED);
    xr_shared_set_refc(new_dt, 1);
    return XR_FROM_PTR(new_dt);
}

XrValue xr_to_shared(struct XrayIsolate *X, XrValue value) {
    if (!XR_IS_PTR(value)) return value;
    XrGCHeader *obj = XR_VALUE_GCPTR(value);
    if (!obj) return value;
    // Already shared: no-op (do NOT incref — caller already owns the reference)
    if (XR_GC_IS_SHARED(obj)) { return value; }
    if (XR_GC_GET_TYPE(obj) == XR_TSTRING) return value;
    uint8_t type = XR_GC_GET_TYPE(obj);
    switch (type) {
        case XR_TARRAY:
        case XR_TARRAY_SLICE: return deep_copy_array_to_shared(X, (XrArray*)obj);
        case XR_TMAP: return deep_copy_map_to_shared(X, (XrMap*)obj);
        case XR_TSET: return deep_copy_set_to_shared(X, (XrSet*)obj);
        case XR_TINSTANCE: return deep_copy_instance_to_shared(X, (XrInstance*)obj);
        case XR_TJSON: return deep_copy_json_to_shared(X, (XrJson*)obj);
        case XR_TSTRINGBUILDER: return deep_copy_stringbuilder_to_shared(X, (XrStringBuilder*)obj);
        case XR_TFUNCTION: return deep_copy_closure_to_shared(X, (XrClosure*)obj);
        case XR_TDATETIME: return deep_copy_datetime_to_shared(X, obj);
        default: return value;
    }
}
