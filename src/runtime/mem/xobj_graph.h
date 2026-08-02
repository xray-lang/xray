/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xobj_graph.h - Object-graph edge traversal shared by everything that walks
 *                the coroutine-local reference graph.
 *
 * The type-specific "what are this object's children" knowledge lives here
 * rather than inside any one consumer: the cycle collector and the
 * development-mode cycle detector must agree on it exactly, or one of them
 * reports edges the other cannot see. Task 247 phase E removes the collector;
 * this file is what stays.
 */

#ifndef XR_OBJ_GRAPH_H
#define XR_OBJ_GRAPH_H

#include "xobj_header.h"
#include "../class/xinstance.h"
#include "../class/xenum.h"
#include "../class/xclass.h"
#include "../closure/xcell.h"
#include "../closure/xclosure.h"
#include "../object/xarray.h"
#include "../object/xmap.h"
#include "../object/xset.h"
#include "../../base/xchecks.h"

/* An edge is only followable when the child is a coro-local RC object. Shared
 * objects use atomic refcounts (raw ++/-- would race other workers), managed
 * objects are runtime-owned, and region objects ignore RC entirely. Shared objects use
 * atomic refcounts (raw ++/-- would race other workers), managed objects
 * are runtime-owned, and region objects ignore RC entirely. None of them
 * None of them can be a member of a coro-local cycle, so the edge is skipped
 * altogether — and it must be skipped CONSISTENTLY in every phase of whatever
 * walks the graph, or the bookkeeping breaks. */
static inline bool xr_obj_graph_child_eligible(const XrObjHeader *obj) {
    if (obj->extra & (XR_OBJ_REGION | XR_OBJ_MANAGED | XR_OBJ_ATOMIC | XR_OBJ_IMMORTAL))
        return false;
    if (XR_OBJ_IS_SHARED(obj))
        return false;
    return true;
}

/* Which slot of the holder an edge came out of.
 *
 * For an instance this is the field index, which is what turns "there is a
 * cycle through this class" into "annotate THIS field `weak`" — the whole
 * point of reporting a candidate edge. For a container it is the element or
 * entry index, which has no source-level declaration to annotate; the fix
 * there is on the field that holds the container. */
#define XR_OBJ_GRAPH_SLOT_NONE UINT32_MAX

/* Callback type for iterating GC-managed children of an object.
 * `slot` is the holder-relative index described above. */
typedef void (*XrObjGraphVisitor)(XrObjHeader *child, uint32_t slot, void *ctx);

/* Visit all GC pointer children of an object (type-specific traversal). */
static inline void xr_obj_graph_visit_children(XrObjHeader *obj, XrObjGraphVisitor visitor,
                                               void *ctx) {
    XR_DCHECK(obj != NULL, "xr_obj_graph_visit_children: NULL obj");
    switch (obj->type) {
        case XR_TINSTANCE: {
            XrInstance *inst = (XrInstance *) obj;
            XrClass *klass = inst->klass;
            if (!klass)
                break;
            if (klass->builtin_kind == XR_BK_ADT_ENUM) {
                XrEnumAggregateValue *agg = (XrEnumAggregateValue *) obj;
                for (uint32_t i = 0; i < agg->payload_count; i++) {
                    XrValue v = agg->payloads[i];
                    if (XR_IS_PTR(v)) {
                        XrObjHeader *child = XR_VALUE_GCPTR(v);
                        if (child)
                            visitor(child, XR_OBJ_GRAPH_SLOT_NONE, ctx);
                    }
                }
                break;
            }
            uint32_t fc = xr_class_instance_field_count(klass);
            for (uint32_t i = 0; i < fc; i++) {
                XrValue v = inst->fields[i];
                if (XR_IS_PTR(v)) {
                    XrObjHeader *child = XR_VALUE_GCPTR(v);
                    if (child)
                        visitor(child, i, ctx);
                }
            }
            break;
        }
        case XR_TARRAY: {
            XrArray *arr = (XrArray *) obj;
            if (arr->elem_type != XR_ELEM_ANY || arr->length <= 0)
                break;
            XrValue *data = (XrValue *) arr->data;
            for (int32_t i = 0; i < arr->length; i++) {
                if (XR_IS_PTR(data[i])) {
                    XrObjHeader *child = XR_VALUE_GCPTR(data[i]);
                    if (child)
                        visitor(child, XR_OBJ_GRAPH_SLOT_NONE, ctx);
                }
            }
            break;
        }
        case XR_TMAP: {
            XrMap *map = (XrMap *) obj;
            if (xr_map_isdummy(map) || !map->entries)
                break;
            uint32_t count = map->nentries;
            for (uint32_t i = 0; i < count; i++) {
                XrMapEntry *node = &map->entries[i];
                if (XR_MAP_ENTRY_EMPTY(node))
                    continue;
                if (XR_IS_PTR(node->key)) {
                    XrObjHeader *child = XR_VALUE_GCPTR(node->key);
                    if (child)
                        visitor(child, XR_OBJ_GRAPH_SLOT_NONE, ctx);
                }
                if (XR_IS_PTR(node->value)) {
                    XrObjHeader *child = XR_VALUE_GCPTR(node->value);
                    if (child)
                        visitor(child, XR_OBJ_GRAPH_SLOT_NONE, ctx);
                }
            }
            break;
        }
        case XR_TSET: {
            XrSet *set = (XrSet *) obj;
            if (!set->entries)
                break;
            for (uint32_t i = 0; i < set->nentries; i++) {
                XrSetEntry *e = &set->entries[i];
                if (XR_SET_ENTRY_EMPTY(e))
                    continue;
                if (XR_IS_PTR(e->value)) {
                    XrObjHeader *child = XR_VALUE_GCPTR(e->value);
                    if (child)
                        visitor(child, XR_OBJ_GRAPH_SLOT_NONE, ctx);
                }
            }
            break;
        }
        case XR_TFUNCTION: {
            XrClosure *closure = (XrClosure *) obj;
            for (uint16_t i = 0; i < closure->upval_count; i++) {
                XrValue v = closure->upvals[i];
                if (XR_IS_PTR(v)) {
                    XrObjHeader *child = XR_VALUE_GCPTR(v);
                    if (child)
                        visitor(child, XR_OBJ_GRAPH_SLOT_NONE, ctx);
                }
            }
            break;
        }
        case XR_TCELL: {
            XrCell *cell = (XrCell *) obj;
            XrValue v = cell->value;
            if (XR_IS_PTR(v)) {
                XrObjHeader *child = XR_VALUE_GCPTR(v);
                if (child)
                    visitor(child, XR_OBJ_GRAPH_SLOT_NONE, ctx);
            }
            break;
        }
        default:
            /* Leaf or runtime-managed type: no coro-local RC children. */
            break;
    }
}

#endif  // XR_OBJ_GRAPH_H
