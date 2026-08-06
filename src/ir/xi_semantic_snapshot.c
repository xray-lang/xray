/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_semantic_snapshot.c - Analyzer-independent Xi semantic metadata snapshot
 */

#include "xi_semantic_snapshot.h"

#include "xi.h"
#include "xi_core_api.h"
#include "xi_module.h"
#include "../base/xmalloc.h"
#include "../runtime/class/xclass_info.h"
#include "../runtime/value/xenum_layout.h"
#include "../runtime/value/xstruct_layout.h"
#include "../runtime/value/xtype.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct XiSnapshotPtrMap {
    const void **keys;
    void **values;
    size_t capacity;
    size_t count;
} XiSnapshotPtrMap;

typedef struct XiSemanticSnapshot {
    XiFunc *root;
    XiSnapshotPtrMap types;
    XiSnapshotPtrMap enum_layouts;
    XiSnapshotPtrMap aggregate_layouts;
    XiSnapshotPtrMap class_infos;
    const char *failure;
    int failure_type_kind;
    int failure_value_op;
    bool failed;
} XiSemanticSnapshot;

static void snapshot_note_failure(XiSemanticSnapshot *snapshot, const char *failure) {
    if (snapshot && !snapshot->failure)
        snapshot->failure = failure;
}

static size_t snapshot_ptr_hash(const void *ptr) {
    uintptr_t value = (uintptr_t) ptr;
    value >>= 3;
#if UINTPTR_MAX > UINT32_MAX
    value ^= value >> 33;
    value *= UINT64_C(0xff51afd7ed558ccd);
    value ^= value >> 33;
#else
    value ^= value >> 16;
    value *= UINT32_C(0x7feb352d);
    value ^= value >> 15;
#endif
    return (size_t) value;
}

static void snapshot_map_dispose(XiSnapshotPtrMap *map) {
    if (!map)
        return;
    xr_free(map->keys);
    xr_free(map->values);
    memset(map, 0, sizeof(*map));
}

static void *snapshot_map_get(const XiSnapshotPtrMap *map, const void *key) {
    if (!map || !key || map->capacity == 0)
        return NULL;
    size_t mask = map->capacity - 1;
    size_t slot = snapshot_ptr_hash(key) & mask;
    while (map->keys[slot]) {
        if (map->keys[slot] == key)
            return map->values[slot];
        slot = (slot + 1) & mask;
    }
    return NULL;
}

static bool snapshot_map_resize(XiSnapshotPtrMap *map, size_t capacity) {
    const void **keys = (const void **) xr_calloc(capacity, sizeof(*keys));
    void **values = (void **) xr_calloc(capacity, sizeof(*values));
    if (!keys || !values) {
        xr_free(keys);
        xr_free(values);
        return false;
    }

    if (map->capacity > 0) {
        size_t mask = capacity - 1;
        for (size_t i = 0; i < map->capacity; i++) {
            if (!map->keys[i])
                continue;
            size_t slot = snapshot_ptr_hash(map->keys[i]) & mask;
            while (keys[slot])
                slot = (slot + 1) & mask;
            keys[slot] = map->keys[i];
            values[slot] = map->values[i];
        }
    }
    xr_free(map->keys);
    xr_free(map->values);
    map->keys = keys;
    map->values = values;
    map->capacity = capacity;
    return true;
}

static bool snapshot_map_put(XiSnapshotPtrMap *map, const void *key, void *value) {
    if (!map || !key || !value)
        return false;
    if (map->capacity == 0) {
        if (!snapshot_map_resize(map, 64))
            return false;
    } else if ((map->count + 1) * 10 >= map->capacity * 7) {
        if (map->capacity > SIZE_MAX / 2 || !snapshot_map_resize(map, map->capacity * 2))
            return false;
    }
    size_t mask = map->capacity - 1;
    size_t slot = snapshot_ptr_hash(key) & mask;
    while (map->keys[slot]) {
        if (map->keys[slot] == key) {
            map->values[slot] = value;
            return true;
        }
        slot = (slot + 1) & mask;
    }
    map->keys[slot] = key;
    map->values[slot] = value;
    map->count++;
    return true;
}

static void *snapshot_alloc(XiSemanticSnapshot *snapshot, size_t size) {
    if (!snapshot || snapshot->failed || size == 0 || size > UINT32_MAX) {
        if (snapshot)
            snapshot->failed = true;
        return NULL;
    }
    void *ptr = xi_func_arena_alloc(snapshot->root, (uint32_t) size);
    if (!ptr)
        snapshot->failed = true;
    return ptr;
}

static char *snapshot_strdup(XiSemanticSnapshot *snapshot, const char *text) {
    if (!text)
        return NULL;
    size_t length = strlen(text);
    if (length == SIZE_MAX) {
        snapshot->failed = true;
        return NULL;
    }
    char *copy = (char *) snapshot_alloc(snapshot, length + 1);
    if (copy)
        memcpy(copy, text, length + 1);
    return copy;
}

static XrType *snapshot_type(XiSemanticSnapshot *snapshot, const XrType *source);

static XrAggregateLayout *snapshot_aggregate_layout(XiSemanticSnapshot *snapshot,
                                                    const XrAggregateLayout *source) {
    if (!source)
        return NULL;
    XrAggregateLayout *known =
        (XrAggregateLayout *) snapshot_map_get(&snapshot->aggregate_layouts, source);
    if (known)
        return known;
    if (source->field_count > XR_MAX_AGG_FIELDS) {
        snapshot->failed = true;
        return NULL;
    }

    XrAggregateLayout *copy = (XrAggregateLayout *) snapshot_alloc(snapshot, sizeof(*copy));
    if (!copy || !snapshot_map_put(&snapshot->aggregate_layouts, source, copy)) {
        snapshot->failed = true;
        return NULL;
    }
    *copy = *source;
    copy->field_names = NULL;
    if (source->field_count > 0) {
        copy->field_names = (const char **) snapshot_alloc(
            snapshot, (size_t) source->field_count * sizeof(*copy->field_names));
        if (!copy->field_names)
            return NULL;
    }
    for (uint16_t i = 0; i < source->field_count; i++) {
        const char *name = source->field_names ? source->field_names[i] : NULL;
        copy->field_names[i] = snapshot_strdup(snapshot, name);
        if (name && !copy->field_names[i])
            return NULL;
        if (source->fields[i].sub_layout) {
            copy->fields[i].sub_layout =
                snapshot_aggregate_layout(snapshot, source->fields[i].sub_layout);
            if (!copy->fields[i].sub_layout)
                return NULL;
        }
    }
    return copy;
}

static XrEnumLayout *snapshot_enum_layout(XiSemanticSnapshot *snapshot,
                                          const XrEnumLayout *source) {
    if (!source)
        return NULL;
    XrEnumLayout *known = (XrEnumLayout *) snapshot_map_get(&snapshot->enum_layouts, source);
    if (known)
        return known;
    if (source->variant_count > 0 && !source->variants) {
        snapshot->failed = true;
        return NULL;
    }

    XrEnumLayout *copy = (XrEnumLayout *) snapshot_alloc(snapshot, sizeof(*copy));
    if (!copy || !snapshot_map_put(&snapshot->enum_layouts, source, copy)) {
        snapshot->failed = true;
        return NULL;
    }
    *copy = *source;
    copy->name = snapshot_strdup(snapshot, source->name);
    copy->variants = NULL;
    if ((source->name && !copy->name) || source->variant_count == 0)
        return copy;

    copy->variants = (XrEnumVariantLayout *) snapshot_alloc(
        snapshot, (size_t) source->variant_count * sizeof(*copy->variants));
    if (!copy->variants)
        return NULL;
    for (uint32_t i = 0; i < source->variant_count; i++) {
        const XrEnumVariantLayout *src_variant = &source->variants[i];
        XrEnumVariantLayout *dst_variant = &copy->variants[i];
        *dst_variant = *src_variant;
        dst_variant->name = snapshot_strdup(snapshot, src_variant->name);
        dst_variant->payload_names = NULL;
        dst_variant->payload_type_ids = NULL;
        if (src_variant->name && !dst_variant->name)
            return NULL;
        if (src_variant->payload_count == 0)
            continue;
        dst_variant->payload_names = (const char **) snapshot_alloc(
            snapshot, (size_t) src_variant->payload_count * sizeof(*dst_variant->payload_names));
        dst_variant->payload_type_ids = (uint8_t *) snapshot_alloc(
            snapshot, (size_t) src_variant->payload_count * sizeof(*dst_variant->payload_type_ids));
        if (!dst_variant->payload_names || !dst_variant->payload_type_ids)
            return NULL;
        for (uint16_t p = 0; p < src_variant->payload_count; p++) {
            const char *payload_name =
                src_variant->payload_names ? src_variant->payload_names[p] : NULL;
            dst_variant->payload_names[p] = snapshot_strdup(snapshot, payload_name);
            if (payload_name && !dst_variant->payload_names[p])
                return NULL;
            dst_variant->payload_type_ids[p] =
                src_variant->payload_type_ids ? src_variant->payload_type_ids[p] : 0;
        }
    }
    return copy;
}

static XrClassInfo *snapshot_class_info(XiSemanticSnapshot *snapshot, const XrClassInfo *source) {
    if (!source)
        return NULL;
    XrClassInfo *known = (XrClassInfo *) snapshot_map_get(&snapshot->class_infos, source);
    if (known)
        return known;

    XrClassInfo *copy = (XrClassInfo *) snapshot_alloc(snapshot, sizeof(*copy));
    if (!copy || !snapshot_map_put(&snapshot->class_infos, source, copy)) {
        snapshot->failed = true;
        return NULL;
    }
    *copy = *source;
    copy->name = snapshot_strdup(snapshot, source->name);
    copy->base_name = snapshot_strdup(snapshot, source->base_name);
    copy->location.file = snapshot_strdup(snapshot, source->location.file);
    if ((source->name && !copy->name) || (source->base_name && !copy->base_name) ||
        (source->location.file && !copy->location.file))
        return NULL;

    /* Analyzer lookup structures never escape.  Post-pipeline consumers only
     * inspect nominal ancestry and aggregate layout through class_ref.  Do not
     * snapshot constructor/interface tables: they are analyzer work products,
     * can be intentionally incomplete while a declaration is being resolved,
     * and are not part of the backend contract. */
    copy->scope = NULL;
    copy->fields = NULL;
    copy->field_count = 0;
    copy->methods = NULL;
    copy->method_count = 0;
    copy->static_fields = NULL;
    copy->static_field_count = 0;
    copy->static_methods = NULL;
    copy->static_method_count = 0;
    copy->members_map = NULL;
    copy->vtable = NULL;
    copy->vtable_size = 0;
    copy->constructor_params = NULL;
    copy->constructor_param_count = 0;
    copy->interface_types = NULL;
    copy->interface_count = 0;

    copy->base = snapshot_class_info(snapshot, source->base);
    copy->struct_layout = snapshot_aggregate_layout(snapshot, source->struct_layout);
    return snapshot->failed ? NULL : copy;
}

static bool snapshot_type_vector(XiSemanticSnapshot *snapshot, XrType ***target,
                                 XrType *const *source, int count) {
    *target = NULL;
    if (count <= 0)
        return count == 0;
    if (!source)
        return false;
    XrType **items = (XrType **) snapshot_alloc(snapshot, (size_t) count * sizeof(*items));
    if (!items)
        return false;
    *target = items;
    for (int i = 0; i < count; i++) {
        items[i] = snapshot_type(snapshot, source[i]);
        if (source[i] && !items[i])
            return false;
    }
    return true;
}

static bool snapshot_object_type(XiSemanticSnapshot *snapshot, XrObjectType *target,
                                 const XrObjectType *source) {
    target->field_names = NULL;
    target->field_types = NULL;
    target->field_readonly = NULL;
    target->type_name = snapshot_strdup(snapshot, source->type_name);
    if (source->type_name && !target->type_name)
        return false;
    if (source->field_count <= 0)
        return source->field_count == 0;
    if (!source->field_names || !source->field_types)
        return false;
    size_t count = (size_t) source->field_count;
    target->field_names =
        (const char **) snapshot_alloc(snapshot, count * sizeof(*target->field_names));
    target->field_types =
        (XrType **) snapshot_alloc(snapshot, count * sizeof(*target->field_types));
    if (source->field_readonly)
        target->field_readonly =
            (bool *) snapshot_alloc(snapshot, count * sizeof(*target->field_readonly));
    if (!target->field_names || !target->field_types ||
        (source->field_readonly && !target->field_readonly))
        return false;
    for (int i = 0; i < source->field_count; i++) {
        target->field_names[i] = snapshot_strdup(snapshot, source->field_names[i]);
        target->field_types[i] = snapshot_type(snapshot, source->field_types[i]);
        if ((source->field_names[i] && !target->field_names[i]) ||
            (source->field_types[i] && !target->field_types[i]))
            return false;
        if (target->field_readonly)
            target->field_readonly[i] = source->field_readonly[i];
    }
    return true;
}

static bool snapshot_function_type(XiSemanticSnapshot *snapshot, XrType *copy,
                                   const XrType *source) {
    int param_count = source->function.param_count;
    copy->function.params = NULL;
    if (param_count < 0 || (param_count > 0 && !source->function.params))
        return false;
    if (param_count > 0) {
        copy->function.params = (XrFunctionParam *) snapshot_alloc(
            snapshot, (size_t) param_count * sizeof(*copy->function.params));
        if (!copy->function.params)
            return false;
        for (int i = 0; i < param_count; i++) {
            copy->function.params[i] = source->function.params[i];
            copy->function.params[i].type =
                snapshot_type(snapshot, source->function.params[i].type);
            if (source->function.params[i].type && !copy->function.params[i].type)
                return false;
        }
    }
    copy->function.return_type = snapshot_type(snapshot, source->function.return_type);
    if (source->function.return_type && !copy->function.return_type)
        return false;

    int type_param_count = source->function.type_param_count;
    copy->function.type_param_names = NULL;
    copy->function.type_param_constraints = NULL;
    copy->function.type_param_constraint_counts = NULL;
    if (type_param_count < 0)
        return false;
    if (type_param_count == 0)
        return true;
    if (!source->function.type_param_names || !source->function.type_param_constraint_counts)
        return false;
    size_t count = (size_t) type_param_count;
    copy->function.type_param_names =
        (const char **) snapshot_alloc(snapshot, count * sizeof(*copy->function.type_param_names));
    copy->function.type_param_constraints = (XrType ***) snapshot_alloc(
        snapshot, count * sizeof(*copy->function.type_param_constraints));
    copy->function.type_param_constraint_counts = (int *) snapshot_alloc(
        snapshot, count * sizeof(*copy->function.type_param_constraint_counts));
    if (!copy->function.type_param_names || !copy->function.type_param_constraints ||
        !copy->function.type_param_constraint_counts)
        return false;
    for (int i = 0; i < type_param_count; i++) {
        copy->function.type_param_names[i] =
            snapshot_strdup(snapshot, source->function.type_param_names[i]);
        int constraint_count = source->function.type_param_constraint_counts[i];
        copy->function.type_param_constraint_counts[i] = constraint_count;
        if ((source->function.type_param_names[i] && !copy->function.type_param_names[i]) ||
            constraint_count < 0)
            return false;
        XrType *const *constraints = source->function.type_param_constraints
                                         ? source->function.type_param_constraints[i]
                                         : NULL;
        if (!snapshot_type_vector(snapshot, &copy->function.type_param_constraints[i], constraints,
                                  constraint_count))
            return false;
    }
    return true;
}

static XrType *snapshot_type(XiSemanticSnapshot *snapshot, const XrType *source) {
    if (!source)
        return NULL;
    /* Frozen primitive singletons have process lifetime and intentional pointer
     * identity.  All analyzer-pool types are non-frozen and are detached. */
    if (source->frozen)
        return (XrType *) source;
    XrType *known = (XrType *) snapshot_map_get(&snapshot->types, source);
    if (known)
        return known;
    if (source->kind < 0 || source->kind >= XR_KIND_COUNT) {
        snapshot_note_failure(snapshot, "invalid type kind");
        snapshot->failure_type_kind = (int) source->kind;
        snapshot->failed = true;
        return NULL;
    }

    XrType *copy = (XrType *) snapshot_alloc(snapshot, sizeof(*copy));
    if (!copy || !snapshot_map_put(&snapshot->types, source, copy)) {
        snapshot->failed = true;
        return NULL;
    }
    *copy = *source;
    copy->alias_name = snapshot_strdup(snapshot, source->alias_name);
    if (source->alias_name && !copy->alias_name)
        return NULL;

    bool ok = true;
    switch (source->kind) {
        case XR_KIND_ARRAY:
        case XR_KIND_SLICE:
        case XR_KIND_SET:
        case XR_KIND_CHANNEL:
        case XR_KIND_POINTER:
            copy->container.element_type = snapshot_type(snapshot, source->container.element_type);
            ok = !source->container.element_type || copy->container.element_type;
            break;
        case XR_KIND_MAP:
            copy->map.key_type = snapshot_type(snapshot, source->map.key_type);
            copy->map.value_type = snapshot_type(snapshot, source->map.value_type);
            ok = (!source->map.key_type || copy->map.key_type) &&
                 (!source->map.value_type || copy->map.value_type);
            break;
        case XR_KIND_JSON:
        case XR_KIND_STRUCT_OBJECT:
            ok = snapshot_object_type(snapshot, &copy->object, &source->object);
            break;
        case XR_KIND_CLASS:
        case XR_KIND_INSTANCE:
        case XR_KIND_INTERFACE:
            copy->instance.class_name = snapshot_strdup(snapshot, source->instance.class_name);
            copy->instance.class_ref = snapshot_class_info(snapshot, source->instance.class_ref);
            copy->instance.superclass = snapshot_type(snapshot, source->instance.superclass);
            ok = (!source->instance.class_name || copy->instance.class_name) &&
                 (!source->instance.class_ref || copy->instance.class_ref) &&
                 (!source->instance.superclass || copy->instance.superclass) &&
                 snapshot_type_vector(snapshot, &copy->instance.type_args,
                                      source->instance.type_args, source->instance.type_arg_count);
            break;
        case XR_KIND_FUNCTION:
            ok = snapshot_function_type(snapshot, copy, source);
            break;
        case XR_KIND_TYPE_PARAM:
            copy->type_param.name = snapshot_strdup(snapshot, source->type_param.name);
            copy->type_param.constraint = snapshot_type(snapshot, source->type_param.constraint);
            ok = (!source->type_param.name || copy->type_param.name) &&
                 (!source->type_param.constraint || copy->type_param.constraint);
            break;
        case XR_KIND_TUPLE:
            ok = snapshot_type_vector(snapshot, &copy->tuple.element_types,
                                      source->tuple.element_types, source->tuple.element_count);
            break;
        case XR_KIND_ENUM:
            copy->enum_type.enum_name = snapshot_strdup(snapshot, source->enum_type.enum_name);
            copy->enum_type.layout = snapshot_enum_layout(snapshot, source->enum_type.layout);
            ok =
                (!source->enum_type.enum_name || copy->enum_type.enum_name) &&
                (!source->enum_type.layout || copy->enum_type.layout) &&
                snapshot_type_vector(snapshot, &copy->enum_type.type_args,
                                     source->enum_type.type_args, source->enum_type.type_arg_count);
            break;
        case XR_KIND_UNION: {
            XrType **members = NULL;
            ok = snapshot_type_vector(snapshot, &members, source->union_type.members,
                                      (int) source->union_type.member_count);
            copy->union_type.members = members;
            break;
        }
        case XR_KIND_FIXED_ARRAY:
            copy->fixed_array.element_type =
                snapshot_type(snapshot, source->fixed_array.element_type);
            ok = !source->fixed_array.element_type || copy->fixed_array.element_type;
            break;
        default:
            if (source->is_literal && source->kind == XR_KIND_STRING) {
                copy->literal.str_value = snapshot_strdup(snapshot, source->literal.str_value);
                ok = !source->literal.str_value || copy->literal.str_value;
            }
            break;
    }
    if (!ok) {
        snapshot_note_failure(snapshot, "malformed type metadata");
        snapshot->failure_type_kind = (int) source->kind;
        snapshot->failed = true;
    }
    return snapshot->failed ? NULL : copy;
}

static bool snapshot_literal_types(XiSemanticSnapshot *snapshot, XiConstLiteral *literals,
                                   uint16_t count) {
    if (!literals)
        return count == 0;
    for (uint16_t i = 0; i < count; i++) {
        XrType *source = literals[i].type;
        literals[i].type = snapshot_type(snapshot, source);
        if (source && !literals[i].type)
            return false;
    }
    return true;
}

static bool snapshot_enum_data(XiSemanticSnapshot *snapshot, XiEnumData *data) {
    if (!data)
        return true;
    if (data->member_count > 0 && !data->members)
        return false;
    for (uint32_t i = 0; i < data->member_count; i++) {
        XiEnumMemberData *member = &data->members[i];
        if (member->payload_count < 0 || (member->payload_count > 0 && !member->payload_types))
            return false;
        for (int p = 0; p < member->payload_count; p++) {
            XrType *source = member->payload_types[p];
            member->payload_types[p] = snapshot_type(snapshot, source);
            if (source && !member->payload_types[p])
                return false;
        }
    }
    return true;
}

static bool snapshot_class_data(XiSemanticSnapshot *snapshot, XiClassData *data) {
    if (!data)
        return true;
    data->struct_layout = snapshot_aggregate_layout(snapshot, data->struct_layout);
    data->instance_layout = snapshot_aggregate_layout(snapshot, data->instance_layout);
    /* No backend may recover semantics from frontend objects after this point. */
    data->ast = NULL;
    data->class_info = NULL;
    return !snapshot->failed;
}

static bool snapshot_value(XiSemanticSnapshot *snapshot, XiValue *value) {
    if (!value)
        return true;
    XrType *source_type = value->type;
    XrType *source_owner = value->enum_metadata_owner;
    value->type = snapshot_type(snapshot, source_type);
    value->enum_metadata_owner = snapshot_type(snapshot, source_owner);
    if ((source_type && !value->type) || (source_owner && !value->enum_metadata_owner)) {
        snapshot_note_failure(snapshot, "Xi value type metadata");
        snapshot->failure_value_op = (int) value->op;
        return false;
    }

    if (value->op == XI_IS && value->aux) {
        value->aux = snapshot_type(snapshot, (const XrType *) value->aux);
    } else if ((value->op == XI_AGG_NEW || value->op == XI_AGG_GET || value->op == XI_AGG_SET ||
                value->op == XI_SLICE_FROM_PTR || value->op == XI_SLICE_REINTERPRET ||
                value->op == XI_BUFFER_MATERIALIZE) &&
               value->aux) {
        value->aux = snapshot_aggregate_layout(snapshot, (const XrAggregateLayout *) value->aux);
    } else if (value->op == XI_CLASS_CREATE) {
        if (!snapshot_class_data(snapshot, (XiClassData *) value->aux))
            return false;
    } else if (value->aux_kind == XI_AUX_KIND_ENUM_NAMESPACE) {
        if (!snapshot_enum_data(snapshot, (XiEnumData *) value->aux))
            return false;
    } else if (value->aux_kind == XI_AUX_KIND_PAR_FOR && value->aux) {
        XiParallelForData *data = (XiParallelForData *) value->aux;
        data->state_type = snapshot_type(snapshot, data->state_type);
    } else if (value->aux_kind == XI_AUX_KIND_PAR_MAP && value->aux) {
        XiParallelMapData *data = (XiParallelMapData *) value->aux;
        data->element_type = snapshot_type(snapshot, data->element_type);
        data->state_type = snapshot_type(snapshot, data->state_type);
    } else if (value->aux_kind == XI_AUX_KIND_PAR_REDUCE && value->aux) {
        XiParallelReduceData *data = (XiParallelReduceData *) value->aux;
        data->accumulator_type = snapshot_type(snapshot, data->accumulator_type);
        data->state_type = snapshot_type(snapshot, data->state_type);
    }
    return !snapshot->failed;
}

static bool snapshot_module(XiSemanticSnapshot *snapshot, XiModule *module) {
    if (!module)
        return true;
    for (uint16_t i = 0; i < module->nexports; i++) {
        XrType *source = module->exports[i].value_type;
        module->exports[i].value_type = snapshot_type(snapshot, source);
        if (source && !module->exports[i].value_type)
            return false;
    }
    for (uint16_t i = 0; i < module->nclasses; i++) {
        if (!snapshot_class_data(snapshot, module->classes ? module->classes[i] : NULL))
            return false;
    }
    for (uint16_t i = 0; i < module->nslots; i++) {
        if (module->slot_classes && !snapshot_class_data(snapshot, module->slot_classes[i]))
            return false;
        if (module->slot_enums && !snapshot_enum_data(snapshot, module->slot_enums[i]))
            return false;
    }
    /* Slot metadata arrays are sparse optional side tables.  A module may have
     * slots without having either literal table at all; only present tables
     * contain analyzer-owned type pointers that need rewriting. */
    if (module->slot_const_literals &&
        !snapshot_literal_types(snapshot, module->slot_const_literals, module->nslots))
        return false;
    if (module->slot_shared_initializers &&
        !snapshot_literal_types(snapshot, module->slot_shared_initializers, module->nslots))
        return false;
    return true;
}

static bool snapshot_func(XiSemanticSnapshot *snapshot, XiFunc *func) {
    if (!func)
        return true;
    XrType *return_type = func->return_type;
    func->return_type = snapshot_type(snapshot, return_type);
    if (return_type && !func->return_type) {
        snapshot_note_failure(snapshot, "function return type");
        return false;
    }

    func->source_file = snapshot_strdup(snapshot, func->source_file);
    func->analyzer = NULL;
    for (uint32_t i = 0; i < func->source_var_count; i++) {
        XrType *source = func->source_var_types ? func->source_var_types[i] : NULL;
        if (func->source_var_types)
            func->source_var_types[i] = snapshot_type(snapshot, source);
        if (source && (!func->source_var_types || !func->source_var_types[i])) {
            snapshot_note_failure(snapshot, "source variable type");
            return false;
        }
    }
    for (uint16_t i = 0; i < func->ncaptures; i++) {
        XrType *source = func->captures[i].type;
        func->captures[i].type = snapshot_type(snapshot, source);
        func->captures[i].name = snapshot_strdup(snapshot, func->captures[i].name);
        if (source && !func->captures[i].type) {
            snapshot_note_failure(snapshot, "capture type");
            return false;
        }
    }
    if (!snapshot_literal_types(snapshot, func->shared_const_literals,
                                func->shared_const_literal_count) ||
        !snapshot_literal_types(snapshot, func->shared_init_literals,
                                func->shared_init_literal_count)) {
        snapshot_note_failure(snapshot, "function shared literal type");
        return false;
    }

    for (uint32_t bi = 0; bi < func->nblocks; bi++) {
        XiBlock *block = func->blocks ? func->blocks[bi] : NULL;
        if (!block)
            continue;
        for (XiPhi *phi = block->phis; phi; phi = phi->next) {
            if (!snapshot_value(snapshot, &phi->value)) {
                snapshot_note_failure(snapshot, "phi metadata");
                return false;
            }
        }
        for (uint32_t vi = 0; vi < block->nvalues; vi++) {
            if (!snapshot_value(snapshot, block->values ? block->values[vi] : NULL)) {
                snapshot_note_failure(snapshot, "instruction metadata");
                return false;
            }
        }
    }
    if (!snapshot_module(snapshot, func->module)) {
        snapshot_note_failure(snapshot, "module metadata");
        return false;
    }
    for (uint16_t i = 0; i < func->nchildren; i++) {
        if (!snapshot_func(snapshot, func->children ? func->children[i] : NULL)) {
            snapshot_note_failure(snapshot, "child function metadata");
            return false;
        }
    }
    return !snapshot->failed;
}

static void snapshot_mark_detached(XiFunc *func) {
    if (!func)
        return;
    func->semantic_snapshot_detached = true;
    for (uint16_t i = 0; i < func->nchildren; i++)
        snapshot_mark_detached(func->children ? func->children[i] : NULL);
}

bool xi_semantic_snapshot_detach_ex(XiFunc *root, char *error, size_t error_size) {
    if (error && error_size > 0)
        error[0] = '\0';
    if (!root)
        return false;
    if (root->semantic_snapshot_detached)
        return true;
    XiSemanticSnapshot snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.root = root;

    bool ok = snapshot_func(&snapshot, root) && !snapshot.failed;
    if (ok)
        snapshot_mark_detached(root);
    snapshot_map_dispose(&snapshot.types);
    snapshot_map_dispose(&snapshot.enum_layouts);
    snapshot_map_dispose(&snapshot.aggregate_layouts);
    snapshot_map_dispose(&snapshot.class_infos);
    if (!ok && error && error_size > 0) {
        snprintf(error, error_size, "%s (type_kind=%d, value_op=%d)",
                 snapshot.failure ? snapshot.failure : "unknown semantic snapshot failure",
                 snapshot.failure_type_kind, snapshot.failure_value_op);
    }
    return ok;
}

bool xi_semantic_snapshot_detach(XiFunc *root) {
    return xi_semantic_snapshot_detach_ex(root, NULL, 0);
}
