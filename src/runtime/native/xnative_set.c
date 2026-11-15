/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_set.c - Canonical Set owners and deferred COW publication
 *
 * KEY CONCEPT:
 *   The actual binding and owner ledger publish before a retired root is released.
 */
#include "xnative_set.h"
#include "xnative_value.h"
#include "xnative_execution.h"
#include "xir/xxir_value_internal.h"
#include "xir/xxir_type_arena_internal.h"
#include "xir/xxir_types.h"
#include "xir/xxir_map.h"
#include "xir/xxir_value_place.h"
#include "base/xhash.h"
#include "base/xchecks.h"
#include <string.h>
struct XrtSetOwner {XrXirValue value;const XrXirTypeArena *arena;uint32_t count;};
XR_FUNC XrXirValueStatus xrt_set_types_new(uint64_t limit, XrXirTypeArena **output, uint64_t *bytes) {
    if (!output || *output || !bytes) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_FINITE, limit, true};
    if (limit == UINT64_MAX) limits = (XrCompileResourceLimits){XR_COMPILE_MEMORY_ORDINARY, 0, true};
    XrXirCompileContext context = {.limits=xr_xir_compile_default_limits()};
    XrCompileResourceStatus resource = xr_compile_resources_new(&limits, &context.resources);
    if (resource != XR_COMPILE_RESOURCE_OK)
        return resource == XR_COMPILE_RESOURCE_MEMORY_LIMIT ? XR_XIR_VALUE_MEMORY_LIMIT : XR_XIR_VALUE_OOM;
    XrXirTypeNode node = {.kind=XR_XIR_TYPE_SET, .element=XR_XIR_I64};
    XrXirTypes types = {&node, 1, NULL, NULL};
    XrXirValueStatus status = xr_xir_compile_type_arena_new(&context, &types, output);
    XrCompileResourceStats stats = {0};
    if (status == XR_XIR_VALUE_OK) {
        resource = xr_compile_resources_stats(context.resources, &stats);
        if (resource != XR_COMPILE_RESOURCE_OK) {
            xr_xir_compile_type_arena_drop(*output); *output = NULL;
            status = XR_XIR_VALUE_BAD_ARGUMENT;
        } else *bytes = stats.held_bytes;
    }
    xr_compile_resources_release(context.resources);
    return status;
}

static XrXirValue set_value(const XrtSet *set) {
    return set ? (XrXirValue){((const XirObject *)set)->type, 0, (int64_t)(intptr_t)set} : (XrXirValue){0};
}

static bool set_shape(const XrXirValue *value) {
    if (!value->payload || !xr_xir_value_valid(value)) return false;
    const XrXirTypeNode *node = xr_xir_type_node(
        xr_xir_compile_type_arena_types(xr_xir_value_arena(value)), (XrXirType)value->type);
    return node && node->kind == XR_XIR_TYPE_SET && node->element == XR_XIR_I64 && node->result == XR_XIR_UNIT;
}

static XrXirValueStatus set_root_types_ensure(XrtValueRoot *root) {
    if (!root) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (root->set_types) return XR_XIR_VALUE_OK;
    uint64_t bytes = 0;
    return xrt_set_types_new(UINT64_MAX, &root->set_types, &bytes);
}

static XrXirValueAdmission set_admission(XrtValueRoot *root, uint32_t source) {
    XrXirValueStatus status = set_root_types_ensure(root);
    if (status != XR_XIR_VALUE_OK) xrt_value_failure(status, source);
    return (XrXirValueAdmission){root->set_types, xrt_value_root_domain(root, source),
        NULL, NULL, UINT64_MAX, UINT64_MAX};
}

XR_FUNC const XrXirTypeArena *xrt_set_root_types(uint32_t source) {
    const XrXirTypeArena *types = NULL;
    XrXirValueStatus status = xrt_set_root_types_ensure(&types);
    if (status != XR_XIR_VALUE_OK) xrt_value_failure(status, source);
    return types;
}

XR_FUNC XrXirValueStatus xrt_set_root_types_ensure(const XrXirTypeArena **output) {
    if (!output || *output) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrtValueRoot *root = xrt_value_root_active();
    XrXirValueStatus status = set_root_types_ensure(root);
    if (status == XR_XIR_VALUE_OK) *output = root->set_types;
    return status;
}

XR_FUNC XrXirValueStatus xrt_set_root_bind_types(const XrXirTypeArena *types) {
    XrtValueRoot *root = xrt_value_root_active();
    const XrXirTypes *view = xr_xir_compile_type_arena_types(types);
    const XrXirTypeNode *node = xr_xir_type_node(view, XR_XIR_CONSTRUCTED_TYPE_BASE);
    if (!root || !view || view->count != 1 || !node || node->kind != XR_XIR_TYPE_SET ||
        node->element != XR_XIR_I64 || node->result != XR_XIR_UNIT) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (root->set_types)
        return xir_type_arena_same_type(root->set_types, XR_XIR_CONSTRUCTED_TYPE_BASE,
            types, XR_XIR_CONSTRUCTED_TYPE_BASE) ? XR_XIR_VALUE_OK : XR_XIR_VALUE_BAD_ARGUMENT;
    if (!xr_xir_compile_type_arena_retain((XrXirTypeArena *)types)) return XR_XIR_VALUE_REFCOUNT_LIMIT;
    root->set_types = (XrXirTypeArena *)types;
    return XR_XIR_VALUE_OK;
}

static size_t set_slot(int64_t payload, uint32_t capacity) {
    return xr_hash_bytes(&payload, sizeof(payload)) & (capacity - 1);
}

static XrtSetOwner *set_find(const XrtValueRoot *root, int64_t payload) {
    if (!root->sets) return NULL;
    for (size_t i = set_slot(payload, root->set_capacity);; i = (i + 1) & (root->set_capacity - 1)) {
        XrtSetOwner *owner = &root->sets[i];
        if (owner->value.payload == payload) return owner;
        if (!owner->value.payload) return NULL;
    }
}

static void set_place(XrtValueRoot *root, XrtSetOwner owner) {
    for (size_t i = set_slot(owner.value.payload, root->set_capacity);; i = (i + 1) & (root->set_capacity - 1))
        if (!root->sets[i].value.payload) { root->sets[i] = owner; return; }
}

static void set_reserve(XrtValueRoot *root, uint32_t source) {
    if (root->set_held < root->set_capacity / 2) return;
    if (root->set_capacity > UINT32_MAX / 4) xrt_value_failure(XR_XIR_VALUE_LIMIT, source);
    uint32_t capacity = root->set_capacity ? root->set_capacity * 2 : 16;
    if (sizeof(XrtSetOwner) > SIZE_MAX / capacity) xrt_value_failure(XR_XIR_VALUE_LIMIT, source);
    XrXirValueStatus status = XR_XIR_VALUE_OK;
    XrtSetOwner *table = xr_xir_domain_allocate(xrt_value_root_domain(root, source),
        (size_t)capacity * sizeof(*table), &status);
    if (!table) xrt_value_failure(status, source);
    memset(table, 0, (size_t)capacity * sizeof(*table));
    XrtSetOwner *previous = root->sets;
    uint32_t old_capacity = root->set_capacity;
    root->sets = table; root->set_capacity = capacity;
    for (uint32_t i = 0; i < old_capacity; ++i)
        if (previous[i].value.payload) set_place(root, previous[i]);
    if (previous) xr_xir_domain_deallocate(root->domain, previous, (size_t)old_capacity * sizeof(*previous));
}

static void set_remove_owner(XrtValueRoot *root, XrtSetOwner *owner) {
    size_t hole = (size_t)(owner - root->sets), mask = root->set_capacity - 1;
    for (size_t next = (hole + 1) & mask; root->sets[next].value.payload; next = (next + 1) & mask) {
        size_t home = set_slot(root->sets[next].value.payload, root->set_capacity);
        if (((next - home) & mask) >= ((next - hole) & mask)) {
            root->sets[hole] = root->sets[next]; hole = next;
        }
    }
    root->sets[hole] = (XrtSetOwner){0}; --root->set_count;
}

static void set_adopt(XrtValueRoot *root, XrXirValue value) {
    XrtSetOwner *owner = set_find(root, value.payload);
    if (owner) ++owner->count;
    else { set_place(root, (XrtSetOwner){value, xr_xir_value_arena(&value), 1}); ++root->set_count; }
    ++root->set_held;
}

static XrXirValue set_argument(XrtValueRoot *root, const XrtSet *set,
    XrXirValueAdmission *admission, uint32_t source) {
    XrtSetOwner *owner = set ? set_find(root, (int64_t)(intptr_t)set) : NULL;
    if (owner) { admission->arena = owner->arena; return owner->value; }
    XrXirValue value = set_value(set);
    if (!set_shape(&value)) xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
    admission->arena = xr_xir_value_arena(&value);
    XrXirValueStatus status = xr_xir_value_admit(&value, (XrXirType)value.type, admission);
    if (status != XR_XIR_VALUE_OK) xrt_value_failure(status, source);
    return value;
}

XR_FUNC XrtSet *xrt_set_publish_pending(uint32_t source) {
    XrtValueRoot *root = xrt_value_root_current(source);
    if (!set_shape(&root->pending)) xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
    set_reserve(root, source);
    XrtSet *result = (XrtSet *)(intptr_t)root->pending.payload;
    set_adopt(root, root->pending); root->pending = (XrXirValue){0};
    return result;
}

XRAY_API XrtSet *xrt_set_retain(const XrtSet *set, uint32_t source) {
    XrtValueRoot *root = xrt_value_root_current(source);
    XrXirValueAdmission admission = set_admission(root, source);
    XrXirValue value = set_argument(root, set, &admission, source), copy = {0};
    set_reserve(root, source);
    XrXirValueStatus status = xr_xir_value_copy(&value, &copy);
    if (status != XR_XIR_VALUE_OK) xrt_value_failure(status, source);
    set_adopt(root, copy);
    return (XrtSet *)(intptr_t)copy.payload;
}

XRAY_API void xrt_set_drop(XrtSet **set) {
    if (!set || !*set) return;
    XrtValueRoot *root = xrt_value_root_current(0);
    XrtSetOwner *owner = set_find(root, (int64_t)(intptr_t)*set);
    XR_CHECK(owner && owner->count, "Set drop consumes one actual guest owner");
    XrXirValue value = owner->value; *set = NULL; --root->set_held;
    if (!--owner->count) set_remove_owner(root, owner);
    xr_xir_value_drop(&value);
}

XRAY_API void xrt_set_assign(XrtSet **destination, const XrtSet *value, uint32_t source) {
    if (!destination) xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
    XrtSet *copy = xrt_set_retain(value, source);
    XrtSet *previous = *destination;
    *destination = copy;
    xrt_set_drop(&previous);
}

XR_FUNC XrXirValueStatus xrt_set_owned_copy(const XrtSet *set, const XrXirTypeArena *types, XrtSet **output) {
    if (!set || !types || !output || *output) return XR_XIR_VALUE_BAD_ARGUMENT;
    const XrXirTypeNode *node = xr_xir_type_node(xr_xir_compile_type_arena_types(types),
        (XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE);
    XrXirValue value = set_value(set), copy = {0};
    if (!node || node->kind != XR_XIR_TYPE_SET || node->element != XR_XIR_I64 ||
        node->result != XR_XIR_UNIT || !set_shape(&value)) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValueAdmission admission = {xr_xir_value_arena(&value), ((const XirObject *)set)->domain,
        NULL, NULL, UINT64_MAX, UINT64_MAX};
    XrXirValueStatus status = xr_xir_value_admit(&value, (XrXirType)value.type, &admission);
    if (status == XR_XIR_VALUE_OK) status = xr_xir_value_copy(&value, &copy);
    if (status == XR_XIR_VALUE_OK) *output = (XrtSet *)(intptr_t)copy.payload;
    return status;
}

XRAY_API void xrt_set_owned_drop(XrtSet **set) {
    if (!set || !*set) return;
    XrXirValue value = set_value(*set); *set = NULL;
    xr_xir_value_drop(&value);
}

XR_FUNC XrtSet *xrt_set_root_take(XrtSet *set) {
    XrtValueRoot *root = xrt_value_root_current(0);
    XrtSetOwner *owner = set_find(root, (int64_t)(intptr_t)set);
    XR_CHECK(owner && owner->count, "Set result transfer owns one registered actual reference");
    --root->set_held;
    if (!--owner->count) set_remove_owner(root, owner);
    return set;
}

XRAY_API int64_t xrt_set_length(const XrtSet *set, uint32_t source) {
    XrtValueRoot *root = xrt_value_root_current(source);
    XrXirValueAdmission admission = set_admission(root, source);
    XrXirValue value = set_argument(root, set, &admission, source);
    int64_t length = 0;
    XrXirValueStatus status = xr_xir_hash_len(&value, &admission, &length);
    if (status != XR_XIR_VALUE_OK) xrt_value_failure(status, source);
    return length;
}

XR_FUNC void xrt_set_root_drain(XrtValueRoot *root) {
    for (uint32_t i = 0; i < root->set_capacity; ++i) {
        XrtSetOwner owner = root->sets[i];
        for (uint32_t count = owner.count; count; --count) {
            XrXirValue value = owner.value;
            xr_xir_value_drop(&value);
        }
    }
    if (root->sets) xr_xir_domain_deallocate(root->domain, root->sets,
        (size_t)root->set_capacity * sizeof(*root->sets));
    xr_xir_compile_type_arena_drop(root->set_types);
    root->sets = NULL; root->set_types = NULL;
    root->set_capacity = root->set_count = 0; root->set_held = 0;
}

XRAY_API XrtSet *xrt_set_i64_new(const int64_t *elements,size_t count,uint32_t source) {
    XrtValueRoot *root=xrt_value_root_current(source);
    if((count && !elements) || count>INT64_MAX || count>SIZE_MAX/sizeof(XrXirValue))
        xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT,source);
    XR_CHECK(!root->pending.type && !root->pending.payload,"Set construction owns an empty pending result");
    XrXirValueAdmission admission=set_admission(root,source);set_reserve(root,source);
    XrXirValueStatus status=XR_XIR_VALUE_OK;
    XrXirValue *input=count?xr_xir_domain_allocate(root->domain,count*sizeof(*input),&status):NULL;
    if(count && !input)xrt_value_failure(status,source);
    for(size_t n=0;n<count;++n)input[n]=(XrXirValue){XR_XIR_I64,0,elements[n]};
    status=xr_xir_set_new((XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE,input,count,&admission,&root->pending);
    if(input)xr_xir_domain_deallocate(root->domain,input,count*sizeof(*input));
    if(status!=XR_XIR_VALUE_OK)xrt_value_failure(status,source);
    return xrt_set_publish_pending(source);
}
XRAY_API bool xrt_set_i64_contains(const XrtSet *set,int64_t number,uint32_t source) {
    XrtValueRoot *root=xrt_value_root_current(source);XrXirValueAdmission admission=set_admission(root,source);
    XrXirValue value=set_argument(root,set,&admission,source),element={XR_XIR_I64,0,number};bool found=false;
    XrXirValueStatus status=xr_xir_set_contains(&value,&element,&admission,&found);
    if(status!=XR_XIR_VALUE_OK)xrt_value_failure(status,source);return found;
}
typedef enum SetMutation {SET_ADD,SET_REMOVE,SET_CLEAR} SetMutation;
static bool set_mutate(XrtSet **binding,bool registered,int64_t number,SetMutation operation,uint32_t source) {
    XrtValueRoot *root=xrt_value_root_current(source);
    if(!binding || !*binding)xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT,source);
    XrXirValueAdmission admission=set_admission(root,source);
    XrXirValue value=set_argument(root,*binding,&admission,source);int64_t previous=value.payload;
    if(registered) {XrtSetOwner *owner=set_find(root,previous);if(!owner || !owner->count)xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT,source);set_reserve(root,source);}
    XrXirValue element={XR_XIR_I64,0,number},retired={0};bool removed=false;
    XrXirValuePlace place={(XrXirType)value.type,binding,NULL};xr_xir_value_graph_begin();
    XrXirValueStatus status=operation==SET_ADD?xr_xir_set_add_deferred(&place,&element,&admission,&retired):
        operation==SET_REMOVE?xr_xir_hash_delete_deferred(&place,&element,&admission,&removed,&retired):
        xr_xir_hash_clear_deferred(&place,&admission,&retired);
    if(status==XR_XIR_VALUE_OK && registered && (int64_t)(intptr_t)*binding!=previous) {
        XrtSetOwner *owner=set_find(root,previous);XR_CHECK(owner && owner->count,"Set mutation retires a registered owner");
        if(!--owner->count)set_remove_owner(root,owner);--root->set_held;
        XrXirValue replacement=set_value(*binding);set_adopt(root,replacement);
    }
    xr_xir_value_drop(&retired);xr_xir_value_graph_end();
    if(status!=XR_XIR_VALUE_OK)xrt_value_failure(status,source);return removed;
}
XRAY_API void xrt_set_i64_add(XrtSet **set,int64_t value,uint32_t source) {(void)set_mutate(set,true,value,SET_ADD,source);}
XRAY_API bool xrt_set_i64_remove(XrtSet **set,int64_t value,uint32_t source) {return set_mutate(set,true,value,SET_REMOVE,source);}
XRAY_API void xrt_set_clear(XrtSet **set,uint32_t source) {(void)set_mutate(set,true,0,SET_CLEAR,source);}
#include "xnative_set_ref.inc.c"
