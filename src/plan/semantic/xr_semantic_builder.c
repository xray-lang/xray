/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_builder.c - Xi to immutable SemanticPlan construction
 */

#include "xr_semantic_builder.h"
#include "xr_semantic_plan_internal.h"
#include "xr_semantic_verify.h"
#include "../ownership/xr_ownership_obligation.h"
#include "../../base/xmalloc.h"
#include "../../ir/xi.h"
#include "../../ir/xi_arc.h"
#include "../../ir/xi_module.h"
#include "../../ir/xi_own.h"
#include "../../ir/xi_ops_gen.h"
#include "../../runtime/value/xtype.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define XR_SEMANTIC_MAX_TYPES UINT32_C(1000000)
#define XR_SEMANTIC_MAX_FUNCTIONS UINT32_C(100000)
#define XR_SEMANTIC_MAX_BLOCKS UINT32_C(2000000)
#define XR_SEMANTIC_MAX_OPERATIONS UINT32_C(10000000)
#define XR_SEMANTIC_MAX_OPERANDS UINT32_C(40000000)
#define XR_SEMANTIC_MAX_EDGES UINT32_C(40000000)
#define XR_SEMANTIC_MAX_KEY_BYTES UINT32_C(1048576)

typedef struct XrTextBuilder {
    char *data;
    size_t size;
    size_t capacity;
} XrTextBuilder;

typedef struct XrTypeMapEntry {
    const XrType *source;
    uint32_t index;
} XrTypeMapEntry;

typedef struct XrFunctionMapEntry {
    const XiFunc *source;
    uint32_t index;
    uint32_t value_begin;
    uint32_t block_begin;
} XrFunctionMapEntry;

typedef struct XrSemanticBuildContext {
    XrSemanticPlan *plan;
    XrTypeMapEntry *types;
    uint32_t type_count;
    uint32_t type_capacity;
    XrFunctionMapEntry *functions;
    uint32_t function_count;
    uint32_t function_capacity;
    bool types_canonicalized;
    char *error;
    size_t error_size;
} XrSemanticBuildContext;

static bool fail(XrSemanticBuildContext *ctx, const char *code, const char *detail) {
    if (ctx->error && ctx->error_size)
        snprintf(ctx->error, ctx->error_size, "%s: %s", code, detail);
    return false;
}

static bool reserve_array(void **items, uint32_t *capacity, uint32_t required, size_t item_size,
                          uint32_t hard_limit) {
    if (required > hard_limit)
        return false;
    if (required <= *capacity)
        return true;
    uint32_t next = *capacity ? *capacity : 16;
    while (next < required) {
        if (next > hard_limit / 2) {
            next = hard_limit;
            break;
        }
        next *= 2;
    }
    void *grown = xr_realloc(*items, (size_t) next * item_size);
    if (!grown)
        return false;
    *items = grown;
    *capacity = next;
    return true;
}

static bool text_reserve(XrTextBuilder *text, size_t extra) {
    if (extra > XR_SEMANTIC_MAX_KEY_BYTES || text->size > XR_SEMANTIC_MAX_KEY_BYTES - extra)
        return false;
    size_t required = text->size + extra + 1;
    if (required <= text->capacity)
        return true;
    size_t capacity = text->capacity ? text->capacity : 128;
    while (capacity < required)
        capacity *= 2;
    char *grown = (char *) xr_realloc(text->data, capacity);
    if (!grown)
        return false;
    text->data = grown;
    text->capacity = capacity;
    return true;
}

static bool text_append(XrTextBuilder *text, const char *value) {
    size_t length = strlen(value);
    if (!text_reserve(text, length))
        return false;
    memcpy(text->data + text->size, value, length);
    text->size += length;
    text->data[text->size] = '\0';
    return true;
}

static bool text_append_format(XrTextBuilder *text, const char *format, ...) {
    va_list args;
    va_start(args, format);
    va_list measure;
    va_copy(measure, args);
    int written = vsnprintf(NULL, 0, format, measure);
    va_end(measure);
    if (written < 0 || !text_reserve(text, (size_t) written)) {
        va_end(args);
        return false;
    }
    int emitted = vsnprintf(text->data + text->size, text->capacity - text->size, format, args);
    va_end(args);
    if (emitted != written)
        return false;
    text->size += (size_t) written;
    return true;
}

static bool text_append_component(XrTextBuilder *text, const char *value) {
    const char *component = value ? value : "";
    return text_append_format(text, "%zu:", strlen(component)) && text_append(text, component);
}

static bool text_append_stable_id(XrTextBuilder *text, XrStableId id) {
    char hex[XR_STABLE_ID_BYTES * 2 + 1];
    xr_stable_id_hex(id, hex);
    return text_append(text, hex);
}

static void text_dispose(XrTextBuilder *text) {
    xr_free(text->data);
    memset(text, 0, sizeof(*text));
}

static bool type_key(const XrType *type, XrTextBuilder *key, const XrType **stack, uint32_t depth,
                     XrSemanticBuildContext *ctx);

static bool type_key_list(XrType *const *types, int count, XrTextBuilder *key, const XrType **stack,
                          uint32_t depth, XrSemanticBuildContext *ctx) {
    if (count < 0 || (count > 0 && !types) || !text_append_format(key, "[%d", count))
        return false;
    for (int i = 0; i < count; i++) {
        if (!text_append(key, ";") || !type_key(types[i], key, stack, depth, ctx))
            return false;
    }
    return text_append(key, "]");
}

static bool type_key_object(const XrType *type, XrTextBuilder *key, const XrType **stack,
                            uint32_t depth, XrSemanticBuildContext *ctx) {
    const XrObjectType *object = &type->object;
    if (object->field_count < 0 ||
        (object->field_count > 0 && (!object->field_names || !object->field_types)) ||
        !text_append_format(key, "object:%d:", object->field_count) ||
        !text_append_component(key, object->type_name))
        return false;
    for (int i = 0; i < object->field_count; i++) {
        bool readonly = object->field_readonly && object->field_readonly[i];
        if (!text_append(key, ";") || !text_append_component(key, object->field_names[i]) ||
            !text_append_format(key, ":%u:", readonly ? 1u : 0u) ||
            !type_key(object->field_types[i], key, stack, depth, ctx))
            return false;
    }
    return true;
}

static bool type_key_function(const XrType *type, XrTextBuilder *key, const XrType **stack,
                              uint32_t depth, XrSemanticBuildContext *ctx) {
    if (type->function.param_count < 0 ||
        (type->function.param_count > 0 && !type->function.params) ||
        !text_append_format(key, "fn:%d:%d:%u:%u:%u", type->function.param_count,
                            type->function.min_params, type->function.is_variadic ? 1u : 0u,
                            type->function.is_c_abi ? 1u : 0u,
                            (unsigned) type->function.throw_effect))
        return false;
    for (int i = 0; i < type->function.param_count; i++) {
        if (!text_append_format(key, ";p%u:", (unsigned) type->function.params[i].mode) ||
            !type_key(type->function.params[i].type, key, stack, depth, ctx))
            return false;
    }
    return text_append(key, ";ret:") &&
           type_key(type->function.return_type, key, stack, depth, ctx) &&
           text_append_format(key, ";view:%u:%d:%u", (unsigned) type->function.view_return_source,
                              (int) type->function.view_return_param,
                              type->function.view_return_complete ? 1u : 0u);
}

static bool type_key(const XrType *type, XrTextBuilder *key, const XrType **stack, uint32_t depth,
                     XrSemanticBuildContext *ctx) {
    if (!type)
        return fail(ctx, "XR_SEM_0005", "semantic type is null");
    if (depth >= 64)
        return fail(ctx, "XR_SEM_0006", "semantic type recursion exceeds 64 levels");
    for (uint32_t i = 0; i < depth; i++) {
        if (stack[i] != type)
            continue;
        if (type->semantic_type_id == 0)
            return fail(ctx, "XR_SEM_0002", "recursive type has no stable semantic identity");
        return text_append_format(key, "cycle:%u", type->semantic_type_id);
    }
    stack[depth] = type;
    if (!text_append_format(key, "type-v2:%u:%u:%u:%u:%u:%u:%u:%u:%u:", (unsigned) type->kind,
                            type->semantic_type_id, type->is_nullable ? 1u : 0u,
                            type->is_const ? 1u : 0u, type->is_value_type ? 1u : 0u,
                            type->is_literal ? 1u : 0u, type->is_cycle_candidate ? 1u : 0u,
                            type->ptr_is_mut ? 1u : 0u, (unsigned) type->scalar_rep) ||
        !text_append_component(key, type->alias_name))
        return false;
    switch (type->kind) {
        case XR_KIND_ARRAY:
        case XR_KIND_SET:
        case XR_KIND_CHANNEL:
        case XR_KIND_SLICE:
            return text_append(key, ";element:") &&
                   type_key(type->container.element_type, key, stack, depth + 1, ctx);
        case XR_KIND_MAP:
            return text_append(key, ";key:") &&
                   type_key(type->map.key_type, key, stack, depth + 1, ctx) &&
                   text_append(key, ";value:") &&
                   type_key(type->map.value_type, key, stack, depth + 1, ctx);
        case XR_KIND_STRUCT_OBJECT:
            return type_key_object(type, key, stack, depth + 1, ctx);
        case XR_KIND_CLASS:
        case XR_KIND_INSTANCE:
        case XR_KIND_INTERFACE:
            return text_append(key, ";named:") &&
                   text_append_component(key, type->instance.class_name) &&
                   type_key_list(type->instance.type_args, type->instance.type_arg_count, key,
                                 stack, depth + 1, ctx);
        case XR_KIND_FUNCTION:
            return type_key_function(type, key, stack, depth + 1, ctx);
        case XR_KIND_TYPE_PARAM:
            return text_append_format(key, ";param:%d:", type->type_param.id) &&
                   text_append_component(key, type->type_param.name);
        case XR_KIND_TUPLE:
            return type_key_list(type->tuple.element_types, type->tuple.element_count, key, stack,
                                 depth + 1, ctx);
        case XR_KIND_ENUM:
            return text_append(key, ";enum:") &&
                   text_append_component(key, type->enum_type.enum_name) &&
                   text_append_format(key, ":%u:", type->enum_type.layout_id) &&
                   type_key_list(type->enum_type.type_args, type->enum_type.type_arg_count, key,
                                 stack, depth + 1, ctx);
        case XR_KIND_UNION:
            return type_key_list(type->union_type.members, type->union_type.member_count, key,
                                 stack, depth + 1, ctx);
        case XR_KIND_FIXED_ARRAY:
            return text_append_format(key, ";length:%d:", type->fixed_array.length) &&
                   type_key(type->fixed_array.element_type, key, stack, depth + 1, ctx);
        default:
            return true;
    }
}

static uint32_t find_type(const XrSemanticBuildContext *ctx, const XrType *type) {
    for (uint32_t i = 0; i < ctx->type_count; i++) {
        if (ctx->types[i].source == type)
            return ctx->types[i].index;
    }
    return XR_SEMANTIC_INDEX_NONE;
}

static bool append_u32(uint32_t **items, uint32_t *count, uint32_t *capacity, uint32_t value,
                       uint32_t limit) {
    if (!reserve_array((void **) items, capacity, *count + 1, sizeof(**items), limit))
        return false;
    (*items)[(*count)++] = value;
    return true;
}

static bool add_type_children(XrSemanticBuildContext *ctx, const XrType *type,
                              uint32_t record_index);

static bool add_type(XrSemanticBuildContext *ctx, const XrType *type, uint32_t *out) {
    uint32_t existing = find_type(ctx, type);
    if (existing != XR_SEMANTIC_INDEX_NONE) {
        *out = existing;
        return true;
    }
    if (ctx->plan->type_count >= XR_SEMANTIC_MAX_TYPES ||
        !reserve_array((void **) &ctx->plan->types, &ctx->plan->type_capacity,
                       ctx->plan->type_count + 1, sizeof(*ctx->plan->types),
                       XR_SEMANTIC_MAX_TYPES) ||
        !reserve_array((void **) &ctx->types, &ctx->type_capacity, ctx->type_count + 1,
                       sizeof(*ctx->types), XR_SEMANTIC_MAX_TYPES))
        return fail(ctx, "XR_EXEC_5003", "semantic type budget exhausted");
    XrTextBuilder key = {0};
    const XrType *stack[64] = {0};
    if (!type_key(type, &key, stack, 0, ctx)) {
        text_dispose(&key);
        return false;
    }
    for (uint32_t i = 0; i < ctx->plan->type_count; i++) {
        if (strcmp(ctx->plan->types[i].canonical_key, key.data) != 0)
            continue;
        ctx->types[ctx->type_count++] = (XrTypeMapEntry) {type, i};
        text_dispose(&key);
        *out = i;
        return true;
    }
    if (ctx->types_canonicalized) {
        text_dispose(&key);
        return fail(ctx, "XR_SEM_0012", "type discovery changed after canonical type-table freeze");
    }
    uint32_t index = ctx->plan->type_count++;
    XrSemanticTypeRecord *record = &ctx->plan->types[index];
    memset(record, 0, sizeof(*record));
    record->canonical_key = xr_semantic_plan_copy_string(ctx->plan, key.data);
    text_dispose(&key);
    if (!record->canonical_key ||
        !xr_stable_id_from_key(record->canonical_key, &record->id, &(XrFingerprint) {{0}}))
        return fail(ctx, "XR_EXEC_5003", "semantic type identity allocation failed");
    record->kind = (uint32_t) type->kind;
    record->scalar_rep = type->scalar_rep;
    bool reference_capable = xi_own_type_is_rc(type);
    bool borrow_view = type->kind == XR_KIND_SLICE;
    record->flags =
        (uint8_t) ((type->is_nullable ? XR_SEM_TYPE_NULLABLE : 0u) |
                   (type->is_const ? XR_SEM_TYPE_CONST : 0u) |
                   (type->is_value_type ? XR_SEM_TYPE_VALUE : 0u) |
                   (type->is_literal ? XR_SEM_TYPE_LITERAL : 0u) |
                   (reference_capable ? XR_SEM_TYPE_REFERENCE_CAPABLE : 0u) |
                   (borrow_view ? XR_SEM_TYPE_BORROW_VIEW : 0u) |
                   (reference_capable && !borrow_view ? XR_SEM_TYPE_OWNERSHIP_ROOT : 0u));
    ctx->types[ctx->type_count++] = (XrTypeMapEntry) {type, index};
    if (!add_type_children(ctx, type, index))
        return false;
    *out = index;
    return true;
}

static bool add_type_children(XrSemanticBuildContext *ctx, const XrType *type,
                              uint32_t record_index) {
    uint32_t count = 0;
    switch (type->kind) {
        case XR_KIND_ARRAY:
        case XR_KIND_SET:
        case XR_KIND_CHANNEL:
        case XR_KIND_SLICE:
        case XR_KIND_FIXED_ARRAY:
            count = 1;
            break;
        case XR_KIND_MAP:
            count = 2;
            break;
        case XR_KIND_STRUCT_OBJECT:
            count = (uint32_t) type->object.field_count;
            break;
        case XR_KIND_CLASS:
        case XR_KIND_INSTANCE:
        case XR_KIND_INTERFACE:
            count = (uint32_t) type->instance.type_arg_count;
            break;
        case XR_KIND_FUNCTION:
            count = (uint32_t) type->function.param_count + 1u;
            break;
        case XR_KIND_TUPLE:
            count = (uint32_t) type->tuple.element_count;
            break;
        case XR_KIND_ENUM:
            count = (uint32_t) type->enum_type.type_arg_count;
            break;
        case XR_KIND_UNION:
            count = type->union_type.member_count;
            break;
        default:
            break;
    }
    if (count > UINT16_MAX)
        return fail(ctx, "XR_SEM_0012", "semantic type has too many immediate children");
    uint32_t *indices = count ? (uint32_t *) xr_malloc((size_t) count * sizeof(*indices)) : NULL;
    if (count && !indices)
        return fail(ctx, "XR_EXEC_5003", "semantic type child allocation failed");
    for (uint32_t i = 0; i < count; i++) {
        const XrType *child = NULL;
        switch (type->kind) {
            case XR_KIND_ARRAY:
            case XR_KIND_SET:
            case XR_KIND_CHANNEL:
            case XR_KIND_SLICE:
                child = type->container.element_type;
                break;
            case XR_KIND_FIXED_ARRAY:
                child = type->fixed_array.element_type;
                break;
            case XR_KIND_MAP:
                child = i == 0 ? type->map.key_type : type->map.value_type;
                break;
            case XR_KIND_STRUCT_OBJECT:
                child = type->object.field_types[i];
                break;
            case XR_KIND_CLASS:
            case XR_KIND_INSTANCE:
            case XR_KIND_INTERFACE:
                child = type->instance.type_args[i];
                break;
            case XR_KIND_FUNCTION:
                child = i < (uint32_t) type->function.param_count ? type->function.params[i].type
                                                                  : type->function.return_type;
                break;
            case XR_KIND_TUPLE:
                child = type->tuple.element_types[i];
                break;
            case XR_KIND_ENUM:
                child = type->enum_type.type_args[i];
                break;
            case XR_KIND_UNION:
                child = type->union_type.members[i];
                break;
            default:
                break;
        }
        if (!child || !add_type(ctx, child, &indices[i])) {
            xr_free(indices);
            return false;
        }
    }
    XrSemanticTypeRecord *record = &ctx->plan->types[record_index];
    record->child_begin = ctx->plan->type_child_count;
    record->child_count = (uint16_t) count;
    for (uint32_t i = 0; i < count; i++) {
        if (!append_u32(&ctx->plan->type_children, &ctx->plan->type_child_count,
                        &ctx->plan->type_child_capacity, indices[i], XR_SEMANTIC_MAX_TYPES * 8u)) {
            xr_free(indices);
            return fail(ctx, "XR_EXEC_5003", "semantic type child budget exhausted");
        }
    }
    xr_free(indices);
    return true;
}

static bool collect_functions(XrSemanticBuildContext *ctx, const XiFunc *function) {
    if (!function || ctx->function_count >= XR_SEMANTIC_MAX_FUNCTIONS ||
        !reserve_array((void **) &ctx->functions, &ctx->function_capacity, ctx->function_count + 1,
                       sizeof(*ctx->functions), XR_SEMANTIC_MAX_FUNCTIONS))
        return fail(ctx, "XR_EXEC_5003", "semantic function budget exhausted");
    uint32_t index = ctx->function_count;
    ctx->functions[ctx->function_count++] = (XrFunctionMapEntry) {function, index, 0, 0};
    for (uint16_t i = 0; i < function->nchildren; i++) {
        if (!collect_functions(ctx, function->children[i]))
            return false;
    }
    return true;
}

typedef struct XrCanonicalTypeEntry {
    XrSemanticTypeRecord record;
    uint32_t old_index;
} XrCanonicalTypeEntry;

static int compare_canonical_type(const void *left, const void *right) {
    const XrCanonicalTypeEntry *a = (const XrCanonicalTypeEntry *) left;
    const XrCanonicalTypeEntry *b = (const XrCanonicalTypeEntry *) right;
    int by_id = xr_stable_id_compare(a->record.id, b->record.id);
    if (by_id != 0)
        return by_id;
    return strcmp(a->record.canonical_key, b->record.canonical_key);
}

static bool collect_semantic_types(XrSemanticBuildContext *ctx) {
    for (uint32_t f = 0; f < ctx->function_count; f++) {
        const XiFunc *function = ctx->functions[f].source;
        uint32_t ignored;
        if (!add_type(ctx, function->return_type, &ignored))
            return false;
        for (uint16_t p = 0; p < xi_func_semantic_param_count(function); p++) {
            if (!function->params[p] || !add_type(ctx, function->params[p]->type, &ignored))
                return fail(ctx, "XR_SEM_0013", "semantic function has a missing parameter");
        }
        for (uint32_t b = 0; b < function->nblocks; b++) {
            const XiBlock *block = function->blocks[b];
            if (!block)
                return fail(ctx, "XR_SEM_0010", "semantic function has a missing block");
            for (const XiPhi *phi = block->phis; phi; phi = phi->next) {
                if (!add_type(ctx, phi->value.type, &ignored))
                    return false;
            }
            for (uint32_t v = 0; v < block->nvalues; v++) {
                const XiValue *value = block->values[v];
                if (!value || !value->type) {
                    if (ctx->error && ctx->error_size)
                        snprintf(ctx->error, ctx->error_size,
                                 "XR_SEM_0015: semantic operation has no result type "
                                 "(func=%s block=%u row=%u op=%s value=%u)",
                                 function->name ? function->name : "<anonymous>", b, v,
                                 value ? xi_generated_op_name(value->op) : "<null>",
                                 value ? value->id : UINT32_MAX);
                    return false;
                }
                if (!add_type(ctx, value->type, &ignored))
                    return false;
                if (value->op != XI_CONST || value->aux_kind != XI_AUX_KIND_ENUM_NAMESPACE)
                    continue;
                const XiEnumData *data = (const XiEnumData *) value->aux;
                if (!data || (data->member_count > 0 && !data->members))
                    return fail(ctx, "XR_SEM_0007",
                                "enum namespace has incomplete semantic metadata");
                for (uint32_t m = 0; m < data->member_count; m++) {
                    const XiEnumMemberData *member = &data->members[m];
                    if (member->payload_count < 0 ||
                        (member->payload_count > 0 && !member->payload_types))
                        return fail(ctx, "XR_SEM_0007",
                                    "enum member has incomplete payload metadata");
                    for (int p = 0; p < member->payload_count; p++) {
                        if (!add_type(ctx, member->payload_types[p], &ignored))
                            return false;
                    }
                }
            }
        }
    }
    return true;
}

static bool canonicalize_type_table(XrSemanticBuildContext *ctx) {
    uint32_t count = ctx->plan->type_count;
    XrCanonicalTypeEntry *entries =
        count ? (XrCanonicalTypeEntry *) xr_malloc((size_t) count * sizeof(*entries)) : NULL;
    uint32_t *remap = count ? (uint32_t *) xr_malloc((size_t) count * sizeof(*remap)) : NULL;
    uint32_t *children =
        ctx->plan->type_child_count
            ? (uint32_t *) xr_malloc((size_t) ctx->plan->type_child_count * sizeof(*children))
            : NULL;
    if ((count && (!entries || !remap)) || (ctx->plan->type_child_count && !children)) {
        xr_free(entries);
        xr_free(remap);
        xr_free(children);
        return fail(ctx, "XR_EXEC_5003", "canonical type-table allocation failed");
    }
    for (uint32_t i = 0; i < count; i++) {
        entries[i].record = ctx->plan->types[i];
        entries[i].old_index = i;
    }
    qsort(entries, count, sizeof(*entries), compare_canonical_type);
    for (uint32_t i = 0; i < count; i++) {
        if (i > 0 && xr_stable_id_equal(entries[i - 1].record.id, entries[i].record.id) &&
            strcmp(entries[i - 1].record.canonical_key, entries[i].record.canonical_key) != 0) {
            xr_free(entries);
            xr_free(remap);
            xr_free(children);
            return fail(ctx, "XR_SEM_0003", "distinct type keys collide on one stable ID");
        }
        remap[entries[i].old_index] = i;
    }
    uint32_t child_cursor = 0;
    for (uint32_t i = 0; i < count; i++) {
        XrSemanticTypeRecord record = entries[i].record;
        uint32_t old_begin = record.child_begin;
        record.child_begin = child_cursor;
        for (uint16_t c = 0; c < record.child_count; c++) {
            uint32_t old_child = ctx->plan->type_children[old_begin + c];
            if (old_child >= count) {
                xr_free(entries);
                xr_free(remap);
                xr_free(children);
                return fail(ctx, "XR_SEM_0012", "semantic type child index is invalid");
            }
            children[child_cursor++] = remap[old_child];
        }
        ctx->plan->types[i] = record;
    }
    for (uint32_t i = 0; i < ctx->type_count; i++)
        ctx->types[i].index = remap[ctx->types[i].index];
    xr_free(ctx->plan->type_children);
    ctx->plan->type_children = children;
    ctx->plan->type_child_count = child_cursor;
    ctx->plan->type_child_capacity = child_cursor;
    ctx->types_canonicalized = true;
    xr_free(entries);
    xr_free(remap);
    return true;
}

static int function_index(const XrSemanticBuildContext *ctx, const XiFunc *function) {
    for (uint32_t i = 0; i < ctx->function_count; i++) {
        if (ctx->functions[i].source == function)
            return (int) i;
    }
    return -1;
}

static bool function_lexical_ordinal(const XiFunc *function, uint16_t *out) {
    if (!function || !out)
        return false;
    if (!function->parent_func) {
        *out = 0;
        return true;
    }
    for (uint16_t i = 0; i < function->parent_func->nchildren; i++) {
        if (function->parent_func->children[i] == function) {
            *out = i;
            return true;
        }
    }
    return false;
}

static bool build_function_records(XrSemanticBuildContext *ctx) {
    if (!reserve_array((void **) &ctx->plan->functions, &ctx->plan->function_capacity,
                       ctx->function_count, sizeof(*ctx->plan->functions),
                       XR_SEMANTIC_MAX_FUNCTIONS))
        return fail(ctx, "XR_EXEC_5003", "semantic function table allocation failed");
    uint32_t value_cursor = 0;
    uint32_t block_cursor = 0;
    for (uint32_t i = 0; i < ctx->function_count; i++) {
        const XiFunc *source = ctx->functions[i].source;
        XrSemanticFunctionRecord *record = &ctx->plan->functions[i];
        memset(record, 0, sizeof(*record));
        int parent = function_index(ctx, source->parent_func);
        record->name = xr_semantic_plan_copy_string(ctx->plan, source->name ? source->name : "");
        if (!record->name || !add_type(ctx, source->return_type, &record->return_type))
            return false;
        record->parameter_begin = ctx->plan->parameter_count;
        record->parameter_count = xi_func_semantic_param_count(source);
        for (uint16_t p = 0; p < record->parameter_count; p++) {
            uint32_t type_index;
            if (!source->params[p])
                return fail(ctx, "XR_SEM_0013", "semantic function has a missing parameter");
            if (!add_type(ctx, source->params[p]->type, &type_index))
                return false;
            if (!append_u32(&ctx->plan->parameters, &ctx->plan->parameter_count,
                            &ctx->plan->parameter_capacity, type_index,
                            XR_SEMANTIC_MAX_FUNCTIONS * 256u)) {
                if (ctx->error && ctx->error_size)
                    snprintf(ctx->error, ctx->error_size,
                             "XR_EXEC_5003: semantic parameter budget exhausted "
                             "(func=%s parameter=%u count=%u capacity=%u)",
                             source->name ? source->name : "<anonymous>", p,
                             ctx->plan->parameter_count, ctx->plan->parameter_capacity);
                return false;
            }
        }
        uint16_t lexical_ordinal = 0;
        XrTextBuilder key = {0};
        if (!function_lexical_ordinal(source, &lexical_ordinal) ||
            !text_append(&key, "function-v2:parent=") ||
            (parent >= 0 && !text_append_stable_id(&key, ctx->plan->functions[parent].id)) ||
            (parent < 0 && !text_append(&key, "module-root")) ||
            !text_append_format(&key, ":ordinal=%u:name=", lexical_ordinal) ||
            !text_append_component(&key, source->name) ||
            !text_append_format(&key, ":body=%u:return=", source->xg_body_func_id) ||
            !text_append_stable_id(&key, ctx->plan->types[record->return_type].id) ||
            !text_append_format(&key, ":params=%u", record->parameter_count)) {
            text_dispose(&key);
            return fail(ctx, "XR_EXEC_5003", "semantic function key allocation failed");
        }
        for (uint16_t p = 0; p < record->parameter_count; p++) {
            uint32_t type_index = ctx->plan->parameters[record->parameter_begin + p];
            if (!text_append_format(&key, ":p%u:mode=%u:type=", p,
                                    (unsigned) source->params[p]->param_mode) ||
                !text_append_stable_id(&key, ctx->plan->types[type_index].id)) {
                text_dispose(&key);
                return fail(ctx, "XR_EXEC_5003", "semantic function key allocation failed");
            }
        }
        if (!text_append_format(&key, ":effects=%u:unsafe=%u:entry=%u:extern=%u",
                                source->semantic_effects, source->requires_unsafe_at_call ? 1u : 0u,
                                (unsigned) source->entry_type, source->is_extern ? 1u : 0u)) {
            text_dispose(&key);
            return fail(ctx, "XR_EXEC_5003", "semantic function key allocation failed");
        }
        record->canonical_key = xr_semantic_plan_copy_string(ctx->plan, key.data);
        text_dispose(&key);
        XrFingerprint digest;
        if (!record->canonical_key ||
            !xr_stable_id_from_key(record->canonical_key, &record->id, &digest))
            return fail(ctx, "XR_EXEC_5003", "semantic function identity allocation failed");
        record->child_count = source->nchildren;
        record->block_begin = block_cursor;
        record->block_count = source->nblocks;
        record->value_begin = value_cursor;
        record->value_count = source->next_value_id;
        record->semantic_effects = source->semantic_effects;
        record->capability_mask = source->requires_unsafe_at_call ? 1u : 0u;
        record->return_parameter = source->arc_return_ownership.param_index;
        record->return_provenance = source->arc_return_ownership.kind;
        if (source->return_type && source->return_type->kind == XR_KIND_SLICE) {
            record->return_parameter = -1;
            record->return_provenance = XR_SEM_RETURN_NONE;
            if (source->view_return_complete) {
                if (source->view_return_source == XR_VIEW_RETURN_PARAM) {
                    record->return_provenance = XR_SEM_RETURN_BORROWED_PARAM;
                    record->return_parameter = source->view_return_param;
                } else if (source->view_return_source == XR_VIEW_RETURN_RECEIVER) {
                    record->return_provenance = XR_SEM_RETURN_BORROWED_PARAM;
                    record->return_parameter = 0;
                } else if (source->view_return_source == XR_VIEW_RETURN_STATIC) {
                    record->return_provenance = XR_SEM_RETURN_BORROWED_STATIC;
                }
            }
        } else if (source->entry_type == 2 && xi_own_type_is_rc(source->return_type)) {
            /* A generator body completes with a control-only RET: its owned
             * Iterator handle is materialized by GEN_CALL in the caller, not
             * by a value-returning terminator in this resumable body. */
            record->return_provenance = XR_SEM_RETURN_OWNED;
            record->return_parameter = -1;
        }
        record->flags =
            (uint8_t) ((source->error_effect_nothrow ? 1u : 0u) |
                       (source->contains_unsafe_op ? 2u : 0u) |
                       (source->entry_type == 2 ? 4u : 0u) | (source->is_extern ? 8u : 0u));
        ctx->functions[i].value_begin = value_cursor;
        ctx->functions[i].block_begin = block_cursor;
        if (UINT32_MAX - value_cursor < source->next_value_id ||
            UINT32_MAX - block_cursor < source->nblocks)
            return fail(ctx, "XR_EXEC_5003", "semantic index space exhausted");
        value_cursor += source->next_value_id;
        block_cursor += source->nblocks;
    }
    ctx->plan->function_count = ctx->function_count;
    return true;
}

static uint32_t value_ref(const XrSemanticBuildContext *ctx, const XiFunc *function,
                          const XiValue *value) {
    int index = function_index(ctx, function);
    if (index < 0 || !value || value->id >= function->next_value_id)
        return XR_SEMANTIC_INDEX_NONE;
    return ctx->functions[index].value_begin + value->id;
}

static uint32_t block_ref(const XrSemanticBuildContext *ctx, const XiBlock *block) {
    if (!block || !block->func || block->id >= block->func->nblocks)
        return XR_SEMANTIC_INDEX_NONE;
    int index = function_index(ctx, block->func);
    return index < 0 ? XR_SEMANTIC_INDEX_NONE : ctx->functions[index].block_begin + block->id;
}

static bool add_metadata(XrSemanticBuildContext *ctx, XrSemanticOperationRecord *record,
                         const char *value) {
    if (!value)
        return true;
    if (!reserve_array((void **) &ctx->plan->metadata, &ctx->plan->metadata_capacity,
                       ctx->plan->metadata_count + 1, sizeof(*ctx->plan->metadata),
                       XR_SEMANTIC_MAX_OPERATIONS * 8u))
        return fail(ctx, "XR_EXEC_5003", "semantic metadata budget exhausted");
    const char *copy = xr_semantic_plan_copy_string(ctx->plan, value);
    if (!copy)
        return fail(ctx, "XR_EXEC_5003", "semantic metadata allocation failed");
    ctx->plan->metadata[ctx->plan->metadata_count++] = copy;
    record->metadata_count++;
    return true;
}

static bool add_metadata_u32(XrSemanticBuildContext *ctx, XrSemanticOperationRecord *record,
                             uint32_t value) {
    char text[16];
    snprintf(text, sizeof(text), "%u", value);
    return add_metadata(ctx, record, text);
}

static bool add_enum_metadata(XrSemanticBuildContext *ctx, const XiEnumData *data,
                              XrSemanticOperationRecord *record) {
    if (!data || !data->name || (data->member_count && !data->members))
        return fail(ctx, "XR_SEM_0007", "enum namespace has incomplete semantic metadata");
    if (!add_metadata(ctx, record, "enum-v2") || !add_metadata(ctx, record, data->name) ||
        !add_metadata_u32(ctx, record, data->layout_id) ||
        !add_metadata_u32(ctx, record, data->is_adt ? 1u : 0u) ||
        !add_metadata_u32(ctx, record, data->type_param_count))
        return false;
    for (uint8_t i = 0; i < data->type_param_count; i++) {
        if (!add_metadata(ctx, record, data->type_param_names[i]))
            return false;
    }
    if (!add_metadata_u32(ctx, record, data->member_count))
        return false;
    for (uint32_t i = 0; i < data->member_count; i++) {
        const XiEnumMemberData *member = &data->members[i];
        if (!member->name || member->payload_count < 0 ||
            !add_metadata(ctx, record, member->name) ||
            !add_metadata_u32(ctx, record, member->ordinal) ||
            !add_metadata_u32(ctx, record, (uint32_t) member->payload_count))
            return false;
        for (int p = 0; p < member->payload_count; p++) {
            uint32_t type_index;
            if (!member->payload_types || !add_type(ctx, member->payload_types[p], &type_index) ||
                !add_metadata(ctx, record, member->payload_names ? member->payload_names[p] : "") ||
                !add_metadata_u32(ctx, record, type_index))
                return false;
        }
    }
    return true;
}

static bool add_operation_metadata(XrSemanticBuildContext *ctx, const XiValue *value,
                                   XrSemanticOperationRecord *record) {
    record->metadata_begin = ctx->plan->metadata_count;
    if (value->op == XI_CONST && value->aux_kind == XI_AUX_KIND_ENUM_NAMESPACE)
        return add_enum_metadata(ctx, (const XiEnumData *) value->aux, record);
    switch ((XiOp) value->op) {
        case XI_LOAD_FIELD:
        case XI_STORE_FIELD:
        case XI_WEAK_LOAD_FIELD:
        case XI_WEAK_STORE_FIELD:
        case XI_CALL_METHOD:
        case XI_CALL_METHOD_DIRECT:
        case XI_CALL_BUILTIN:
        case XI_AS:
        case XI_ASSERT:
        case XI_ASSERT_EQ:
        case XI_ASSERT_NE:
        case XI_ASSERT_THROWS:
        case XI_GET_BUILTIN:
            return add_metadata(ctx, record, (const char *) value->aux);
        case XI_OBJECT_NEW: {
            int32_t count = xi_object_field_count(value);
            const char *const *names = (const char *const *) value->aux;
            if (count < 0 || (count > 0 && !names))
                return fail(ctx, "XR_SEM_0007", "object operation has invalid field metadata");
            for (int32_t i = 0; i < count; i++) {
                if (!add_metadata(ctx, record, names[i]))
                    return false;
            }
            return true;
        }
        case XI_IMPORT_REF: {
            const XiImportRef *import_ref = (const XiImportRef *) value->aux;
            return !import_ref || (add_metadata(ctx, record, import_ref->module_path) &&
                                   add_metadata(ctx, record, import_ref->member_name));
        }
        case XI_CLASS_CREATE: {
            const XiClassData *class_data = (const XiClassData *) value->aux;
            if (!class_data)
                return fail(ctx, "XR_SEM_0007", "class operation has no semantic metadata");
            if (!add_metadata(ctx, record, class_data->class_name) ||
                !add_metadata(ctx, record, class_data->super_name) ||
                !add_metadata(ctx, record, class_data->generic_origin_name) ||
                !add_metadata(ctx, record, class_data->display_name))
                return false;
            for (uint16_t i = 0; i < class_data->instance_field_count; i++) {
                if (!add_metadata(ctx, record, class_data->instance_field_names[i]))
                    return false;
            }
            for (uint16_t i = 0; i < class_data->nmethod; i++) {
                if (!add_metadata(ctx, record, class_data->methods[i].name))
                    return false;
            }
            return true;
        }
        default:
            return true;
    }
}

static bool operation_has_explicit_call_layout(uint16_t opcode) {
    return opcode == XI_CALL || opcode == XI_TAIL_CALL || opcode == XI_CALL_METHOD ||
           opcode == XI_CALL_METHOD_DIRECT;
}

static const XiCallArgPlan *call_argument_plan(const XiValue *value, uint16_t operand) {
    if (!value || !value->call_plan || !operation_has_explicit_call_layout(value->op))
        return NULL;
    if (operand == 0)
        return value->call_plan->has_receiver ? &value->call_plan->receiver : NULL;
    uint16_t parameter = (uint16_t) (operand - 1);
    return parameter < value->call_plan->nargs ? &value->call_plan->args[parameter] : NULL;
}

static bool classify_operand_contract(XrSemanticBuildContext *ctx, const XiValue *operation,
                                      uint16_t index, XrSemanticOperandRecord *record) {
    record->parameter = -1;
    record->role = XR_SEM_OPERAND_VALUE;
    record->parameter_mode = XR_PARAM_READ;
    record->access = XR_CALL_ARG_PLAIN;
    if (operation_has_explicit_call_layout(operation->op)) {
        if (operation->nargs == 0)
            return fail(ctx, "XR_SEM_0018", "call operation has no callee or receiver operand");
        if (operation->nargs > (uint32_t) INT16_MAX + 1u)
            return fail(ctx, "XR_EXEC_5003", "call parameter index budget exhausted");
        if (operation->call_plan &&
            operation->call_plan->nargs != (uint16_t) (operation->nargs - 1))
            return fail(ctx, "XR_SEM_0018", "call argument plan does not match operation arity");
        if (index == 0) {
            record->role = operation->op == XI_CALL || operation->op == XI_TAIL_CALL
                               ? XR_SEM_OPERAND_CALLEE
                               : XR_SEM_OPERAND_RECEIVER;
        } else {
            record->role = XR_SEM_OPERAND_ARGUMENT;
            record->parameter = (int16_t) (index - 1);
        }
        if (record->role != XR_SEM_OPERAND_CALLEE)
            record->flags |= XR_SEM_OPERAND_CALL_CONTRACT;
    } else if (operation->op == XI_CALL_BUILTIN) {
        if (index > INT16_MAX)
            return fail(ctx, "XR_EXEC_5003", "builtin call parameter index budget exhausted");
        record->role = XR_SEM_OPERAND_ARGUMENT;
        record->parameter = (int16_t) index;
        record->flags |= XR_SEM_OPERAND_CALL_CONTRACT;
    }
    const XiCallArgPlan *argument = call_argument_plan(operation, index);
    if (!argument)
        return true;
    record->parameter_mode = argument->param_mode;
    record->access = argument->access;
    record->origin = argument->origin;
    record->lifetime = argument->lifetime;
    record->escape = argument->escape;
    if (argument->addressable)
        record->flags |= XR_SEM_OPERAND_ADDRESSABLE;
    return true;
}

static bool append_operand(XrSemanticBuildContext *ctx, const XiFunc *function,
                           const XiValue *value, uint16_t index) {
    if (ctx->plan->operand_count >= XR_SEMANTIC_MAX_OPERANDS ||
        !reserve_array((void **) &ctx->plan->operands, &ctx->plan->operand_capacity,
                       ctx->plan->operand_count + 1, sizeof(*ctx->plan->operands),
                       XR_SEMANTIC_MAX_OPERANDS))
        return fail(ctx, "XR_EXEC_5003", "semantic operand budget exhausted");
    uint32_t ref = value_ref(ctx, function, value->args[index]);
    if (ref == XR_SEMANTIC_INDEX_NONE)
        return fail(ctx, "XR_SEM_0008", "operation has an invalid SSA operand");
    uint32_t cursor = ctx->plan->operand_count++;
    XrSemanticOperandRecord *record = &ctx->plan->operands[cursor];
    memset(record, 0, sizeof(*record));
    record->value = ref;
    if (!add_type(ctx, value->args[index]->type, &record->type) ||
        !classify_operand_contract(ctx, value, index, record))
        return false;
    uint8_t transfer_mode = XR_TRANSFER_SHARE;
    if ((value->op == XI_GO || value->op == XI_THREAD_SPAWN) && index > 0)
        transfer_mode = xi_go_arg_transfer_mode(value, (uint16_t) (index - 1));
    else if ((value->op == XI_CHAN_SEND || value->op == XI_CHAN_TRY_SEND) && index > 0)
        transfer_mode = xi_chan_send_transfer_mode(value);
    record->transfer_mode = transfer_mode;
    const XiValue *operand = value->args[index];
    bool destroys_scoped_stack_closure =
        operand && operand->op == XI_STACK_ALLOC && operand->aux_int == XI_CLOSURE_NEW &&
        ((value->op == XI_PAR_FOR && index == 3) || (value->op == XI_PAR_MAP && index == 3) ||
         (value->op == XI_PAR_REDUCE && (index == 4 || index == 5)));
    record->ownership_action =
        destroys_scoped_stack_closure || xi_arc_operand_consumes(function, value, index)
            ? XR_SEM_OPERAND_CONSUME
            : XR_SEM_OPERAND_BORROW;
    return true;
}

static bool append_constant(XrSemanticBuildContext *ctx, const XiValue *value,
                            XrSemanticOperationRecord *operation) {
    operation->constant = XR_SEMANTIC_INDEX_NONE;
    if (value->op != XI_CONST)
        return true;
    if (!reserve_array((void **) &ctx->plan->constants, &ctx->plan->constant_capacity,
                       ctx->plan->constant_count + 1, sizeof(*ctx->plan->constants),
                       XR_SEMANTIC_MAX_OPERATIONS))
        return fail(ctx, "XR_EXEC_5003", "semantic constant budget exhausted");
    XrSemanticConstantRecord *constant = &ctx->plan->constants[ctx->plan->constant_count];
    memset(constant, 0, sizeof(*constant));
    constant->type = operation->result_type;
    if (value->aux_kind == XI_AUX_KIND_ENUM_NAMESPACE) {
        const XiEnumData *data = (const XiEnumData *) value->aux;
        if (!data || !data->name)
            return fail(ctx, "XR_SEM_0009", "enum namespace constant has no descriptor");
        constant->kind = XR_SEM_CONST_ENUM_NAMESPACE;
        constant->integer = data->layout_id;
        constant->string = xr_semantic_plan_copy_string(ctx->plan, data->name);
        if (!constant->string)
            return fail(ctx, "XR_EXEC_5003", "enum namespace name allocation failed");
        operation->constant = ctx->plan->constant_count++;
        return true;
    }
    switch (value->type->kind) {
        case XR_KIND_NULL:
            constant->kind = XR_SEM_CONST_NULL;
            break;
        case XR_KIND_INT:
            constant->kind = XR_SEM_CONST_INT;
            constant->integer = value->aux_int;
            break;
        case XR_KIND_FLOAT:
            constant->kind = XR_SEM_CONST_FLOAT;
            constant->float_bits = (uint64_t) value->aux_int;
            break;
        case XR_KIND_BOOL:
            constant->kind = XR_SEM_CONST_BOOL;
            constant->integer = value->aux_int != 0;
            break;
        case XR_KIND_RUNE:
            constant->kind = XR_SEM_CONST_RUNE;
            constant->integer = value->aux_int;
            break;
        case XR_KIND_POINTER:
            constant->kind = XR_SEM_CONST_INT;
            constant->integer = value->aux_int;
            break;
        case XR_KIND_STRING:
            constant->kind = XR_SEM_CONST_STRING;
            constant->string =
                xr_semantic_plan_copy_string(ctx->plan, value->aux ? value->aux : "");
            if (!constant->string)
                return fail(ctx, "XR_EXEC_5003", "semantic string constant allocation failed");
            break;
        default:
            if (ctx->error && ctx->error_size)
                snprintf(ctx->error, ctx->error_size,
                         "XR_SEM_0009: unsupported semantic constant kind %u at value %u",
                         (unsigned) value->type->kind, value->id);
            return false;
    }
    operation->constant = ctx->plan->constant_count++;
    return true;
}

static bool append_operation(XrSemanticBuildContext *ctx, uint32_t function_index_value,
                             uint32_t block_index_value, const XiValue *value) {
    if (!value || value->op >= XI_OP_COUNT || !xi_generated_op_name(value->op))
        return fail(ctx, "XR_SEM_0001", "semantic operation has no registered owner");
    if (!value->type || ctx->plan->operation_count >= XR_SEMANTIC_MAX_OPERATIONS ||
        !reserve_array((void **) &ctx->plan->operations, &ctx->plan->operation_capacity,
                       ctx->plan->operation_count + 1, sizeof(*ctx->plan->operations),
                       XR_SEMANTIC_MAX_OPERATIONS))
        return fail(ctx, "XR_EXEC_5003", "semantic operation budget exhausted");
    const XiFunc *function = ctx->functions[function_index_value].source;
    uint32_t result_type;
    if (!add_type(ctx, value->type, &result_type))
        return false;
    uint32_t index = ctx->plan->operation_count++;
    XrSemanticOperationRecord *record = &ctx->plan->operations[index];
    memset(record, 0, sizeof(*record));
    record->evidence[7] = XR_SEMANTIC_INDEX_NONE;
    XrTextBuilder key = {0};
    if (!text_append_format(&key, "%s/op:%u:%s",
                            ctx->plan->functions[function_index_value].canonical_key, value->id,
                            xi_generated_op_name(value->op))) {
        text_dispose(&key);
        return fail(ctx, "XR_EXEC_5003", "semantic operation key allocation failed");
    }
    record->canonical_key = xr_semantic_plan_copy_string(ctx->plan, key.data);
    text_dispose(&key);
    XrFingerprint digest;
    if (!record->canonical_key ||
        !xr_stable_id_from_key(record->canonical_key, &record->id, &digest))
        return fail(ctx, "XR_EXEC_5003", "semantic operation identity allocation failed");
    record->function = function_index_value;
    record->block = block_index_value;
    record->result_value = value_ref(ctx, function, value);
    record->result_type = result_type;
    record->operand_begin = ctx->plan->operand_count;
    record->operand_count = value->nargs;
    record->opcode = value->op;
    record->metadata_begin = ctx->plan->metadata_count;
    record->auxiliary_kind = value->aux_kind;
    record->effects = xi_generated_op_effects(value->op);
    record->source_line = value->line;
    record->semantic_immediate = value->aux_int;
    record->evidence[0] = value->xg_callsite_id;
    record->evidence[1] = value->xa_intrinsic_id;
    record->evidence[2] = value->xg_method_id;
    record->evidence[3] = value->xg_object_access_id;
    record->evidence[4] = value->xg_key_access_id;
    record->evidence[5] = value->xg_class_field_id;
    record->evidence[6] = value->move_evidence_id;
    record->ownership_use = xi_generated_op_own_use(value->op);
    record->result_ownership = xi_arc_value_result_ownership(function, value);
    record->transfer_mode = value->transfer_mode;
    record->parameter_mode = value->param_mode;
    if (value->op == XI_PARAM)
        record->parameter_ownership = xi_arc_parameter_ownership(function, value);
    record->flags = value->flags;
    XiReturnOwnership value_ownership = xi_arc_value_return_ownership(function, value);
    record->result_alias_operand = xi_arc_value_alias_operand(function, value);
    if (record->result_alias_operand < 0 && value->nargs > 0 &&
        (xi_copy_is_identity_alias(value) || xi_op_is_identity_forward(value->op)))
        record->result_alias_operand = 0;
    record->return_parameter = value_ownership.param_index;
    record->return_provenance = value_ownership.kind;
    record->return_complete = value_ownership.complete ? 1u : 0u;
    if ((record->effects & XI_EFFECT_ALLOCATES) != 0 ||
        xi_generated_op_escape_alloc(value->op) == XI_GEN_ESCAPE_ALLOC_HEAP) {
        XrTextBuilder allocation_key = {0};
        if (!text_append_format(&allocation_key, "%s/allocation", record->canonical_key)) {
            text_dispose(&allocation_key);
            return fail(ctx, "XR_EXEC_5003", "allocation identity allocation failed");
        }
        record->allocation_key = xr_semantic_plan_copy_string(ctx->plan, allocation_key.data);
        text_dispose(&allocation_key);
        if (!record->allocation_key ||
            !xr_stable_id_from_key(record->allocation_key, &record->allocation_id, &digest))
            return fail(ctx, "XR_EXEC_5003", "allocation stable identity failed");
    }
    for (uint16_t i = 0; i < value->nargs; i++) {
        if (!append_operand(ctx, function, value, i))
            return false;
    }
    if (value->op == XI_CLOSURE_NEW && value->aux) {
        int child = function_index(ctx, (const XiFunc *) value->aux);
        if (child < 0)
            return fail(ctx, "XR_SEM_0007", "closure operation references an unknown function");
        record->evidence[7] = (uint32_t) child;
    } else if (value->op == XI_TRY && value->aux) {
        record->evidence[7] = block_ref(ctx, (const XiBlock *) value->aux);
        if (record->evidence[7] == XR_SEMANTIC_INDEX_NONE)
            return fail(ctx, "XR_SEM_0007", "try operation references an unknown block");
    }
    return add_operation_metadata(ctx, value, record) && append_constant(ctx, value, record);
}

static bool build_blocks_and_operations(XrSemanticBuildContext *ctx) {
    uint32_t total_blocks = 0;
    for (uint32_t i = 0; i < ctx->function_count; i++)
        total_blocks += ctx->functions[i].source->nblocks;
    if (total_blocks > XR_SEMANTIC_MAX_BLOCKS ||
        !reserve_array((void **) &ctx->plan->blocks, &ctx->plan->block_capacity, total_blocks,
                       sizeof(*ctx->plan->blocks), XR_SEMANTIC_MAX_BLOCKS))
        return fail(ctx, "XR_EXEC_5003", "semantic block budget exhausted");
    for (uint32_t f = 0; f < ctx->function_count; f++) {
        const XiFunc *function = ctx->functions[f].source;
        for (uint32_t b = 0; b < function->nblocks; b++) {
            const XiBlock *source = function->blocks[b];
            if (!source || source->id != b)
                return fail(ctx, "XR_SEM_0010", "Xi block IDs are not dense and canonical");
            uint32_t index = ctx->plan->block_count++;
            XrSemanticBlockRecord *record = &ctx->plan->blocks[index];
            memset(record, 0, sizeof(*record));
            XrTextBuilder key = {0};
            if (!text_append_format(&key, "%s/block:%u", ctx->plan->functions[f].canonical_key,
                                    source->id)) {
                text_dispose(&key);
                return fail(ctx, "XR_EXEC_5003", "semantic block key allocation failed");
            }
            record->canonical_key = xr_semantic_plan_copy_string(ctx->plan, key.data);
            text_dispose(&key);
            XrFingerprint digest;
            if (!record->canonical_key ||
                !xr_stable_id_from_key(record->canonical_key, &record->id, &digest))
                return fail(ctx, "XR_EXEC_5003", "semantic block identity allocation failed");
            record->function = f;
            record->operation_begin = ctx->plan->operation_count;
            record->predecessor_begin = ctx->plan->predecessor_count;
            record->predecessor_count = source->npreds;
            record->kind = source->kind;
            record->successors[0] =
                source->succs[0] ? block_ref(ctx, source->succs[0]) : XR_SEMANTIC_INDEX_NONE;
            record->successors[1] =
                source->succs[1] ? block_ref(ctx, source->succs[1]) : XR_SEMANTIC_INDEX_NONE;
            record->control_value = source->control ? value_ref(ctx, function, source->control)
                                                    : XR_SEMANTIC_INDEX_NONE;
            record->source_line = source->line;
            for (uint16_t p = 0; p < source->npreds; p++) {
                uint32_t predecessor = block_ref(ctx, source->preds[p]);
                if (predecessor == XR_SEMANTIC_INDEX_NONE ||
                    !append_u32(&ctx->plan->predecessors, &ctx->plan->predecessor_count,
                                &ctx->plan->predecessor_capacity, predecessor,
                                XR_SEMANTIC_MAX_BLOCKS * 8u))
                    return fail(ctx, "XR_SEM_0010", "block predecessor is invalid");
            }
            for (const XiPhi *phi = source->phis; phi; phi = phi->next) {
                if (!append_operation(ctx, f, index, &phi->value))
                    return false;
            }
            for (uint32_t v = 0; v < source->nvalues; v++) {
                if (!append_operation(ctx, f, index, source->values[v]))
                    return false;
            }
            record->operation_count = ctx->plan->operation_count - record->operation_begin;
        }
    }
    return true;
}

static bool append_semantic_edge(XrSemanticBuildContext *ctx, uint32_t function,
                                 uint32_t from_block, uint32_t to_block, uint32_t operation,
                                 XrSemanticEdgeKind kind, uint8_t flags) {
    if (function >= ctx->plan->function_count || from_block >= ctx->plan->block_count ||
        to_block >= ctx->plan->block_count ||
        (operation != XR_SEMANTIC_INDEX_NONE && operation >= ctx->plan->operation_count) ||
        !reserve_array((void **) &ctx->plan->edges, &ctx->plan->edge_capacity,
                       ctx->plan->edge_count + 1, sizeof(*ctx->plan->edges), XR_SEMANTIC_MAX_EDGES))
        return fail(ctx, "XR_EXEC_5003", "semantic control-edge budget exhausted");
    XrSemanticEdgeRecord *record = &ctx->plan->edges[ctx->plan->edge_count];
    memset(record, 0, sizeof(*record));
    XrTextBuilder key = {0};
    if (!text_append_format(&key, "edge-v2:function=") ||
        !text_append_stable_id(&key, ctx->plan->functions[function].id) ||
        !text_append(&key, ":from=") ||
        !text_append_stable_id(&key, ctx->plan->blocks[from_block].id) ||
        !text_append(&key, ":to=") ||
        !text_append_stable_id(&key, ctx->plan->blocks[to_block].id) ||
        !text_append_format(&key, ":kind=%u:operation=", (unsigned) kind) ||
        (operation == XR_SEMANTIC_INDEX_NONE
             ? !text_append(&key, "block-control")
             : !text_append_stable_id(&key, ctx->plan->operations[operation].id))) {
        text_dispose(&key);
        return fail(ctx, "XR_EXEC_5003", "semantic control-edge key allocation failed");
    }
    record->canonical_key = xr_semantic_plan_copy_string(ctx->plan, key.data);
    text_dispose(&key);
    XrFingerprint digest;
    if (!record->canonical_key ||
        !xr_stable_id_from_key(record->canonical_key, &record->id, &digest))
        return fail(ctx, "XR_EXEC_5003", "semantic control-edge identity allocation failed");
    record->function = function;
    record->from_block = from_block;
    record->to_block = to_block;
    record->operation = operation;
    record->kind = (uint8_t) kind;
    record->flags = flags;
    ctx->plan->edge_count++;
    return true;
}

static uint32_t operation_for_result_in_block(const XrSemanticPlan *plan,
                                              const XrSemanticBlockRecord *block, uint32_t value) {
    if (value == XR_SEMANTIC_INDEX_NONE)
        return XR_SEMANTIC_INDEX_NONE;
    for (uint32_t o = block->operation_begin; o < block->operation_begin + block->operation_count;
         o++) {
        if (plan->operations[o].result_value == value)
            return o;
    }
    return XR_SEMANTIC_INDEX_NONE;
}

static uint32_t block_error_source(const XrSemanticPlan *plan, const XrSemanticBlockRecord *block) {
    uint32_t source = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t o = block->operation_begin; o < block->operation_begin + block->operation_count;
         o++) {
        if (plan->operations[o].opcode == XI_ERR_CATCH)
            source = XR_SEMANTIC_INDEX_NONE;
        else if (plan->operations[o].opcode == XI_ERR_SET)
            source = o;
    }
    return source;
}

static bool build_semantic_edges(XrSemanticBuildContext *ctx) {
    for (uint32_t from = 0; from < ctx->plan->block_count; from++) {
        const XrSemanticBlockRecord *block = &ctx->plan->blocks[from];
        uint32_t control = operation_for_result_in_block(ctx->plan, block, block->control_value);
        uint32_t error_source = block_error_source(ctx->plan, block);
        for (unsigned s = 0; s < 2; s++) {
            uint32_t to = block->successors[s];
            if (to == XR_SEMANTIC_INDEX_NONE || (s == 1 && to == block->successors[0]))
                continue;
            bool checked_error = control != XR_SEMANTIC_INDEX_NONE && s == 0 &&
                                 ctx->plan->operations[control].opcode == XI_ERR_CHECK;
            XrSemanticEdgeKind kind = checked_error || error_source != XR_SEMANTIC_INDEX_NONE
                                          ? XR_SEM_EDGE_ERROR
                                          : XR_SEM_EDGE_NORMAL;
            uint32_t operation = checked_error               ? control
                                 : kind == XR_SEM_EDGE_ERROR ? error_source
                                                             : XR_SEMANTIC_INDEX_NONE;
            if (!append_semantic_edge(ctx, block->function, from, to, operation, kind, 0))
                return false;
        }
    }
    /* Xi predecessor slots are an SSA construction relation, not an executable
     * edge authority.  In particular, lowering may add a synthetic predecessor
     * to an otherwise unreachable error/panic handler so Braun SSA can seal the
     * block.  The real panic relation is the XI_TRY operation and its explicit
     * handler target; derive it directly instead of guessing from a predecessor
     * that has no matching normal successor. */
    for (uint32_t o = 0; o < ctx->plan->operation_count; o++) {
        const XrSemanticOperationRecord *operation = &ctx->plan->operations[o];
        if (operation->opcode != XI_TRY)
            continue;
        uint32_t to = operation->evidence[7];
        if (to == XR_SEMANTIC_INDEX_NONE || to >= ctx->plan->block_count)
            return fail(ctx, "XR_SEM_0010", "try operation has no valid panic handler target");
        if (!append_semantic_edge(ctx, operation->function, operation->block, to, o,
                                  XR_SEM_EDGE_PANIC, XR_SEM_EDGE_HANDLER_SCOPE))
            return false;
    }
    return true;
}

bool xr_semantic_plan_build(const XiFunc *root, XrSemanticPlan **out, char *error,
                            size_t error_size) {
    if (out)
        *out = NULL;
    if (!root || !out || root->stage != XI_STAGE_OPTIMIZED) {
        if (error && error_size)
            snprintf(error, error_size,
                     "XR_SEM_0011: SemanticPlan requires an exact Optimized Xi graph");
        return false;
    }
    XrSemanticBuildContext ctx = {0};
    ctx.error = error;
    ctx.error_size = error_size;
    ctx.plan = xr_semantic_plan_create();
    if (!ctx.plan || !collect_functions(&ctx, root) || !collect_semantic_types(&ctx) ||
        !canonicalize_type_table(&ctx) || !build_function_records(&ctx) ||
        !build_blocks_and_operations(&ctx) || !build_semantic_edges(&ctx))
        goto failure;
    XrOwnershipCertificate *ownership = NULL;
    if (!xr_ownership_certificate_build(ctx.plan, &ownership, error, error_size))
        goto failure;
    xr_semantic_plan_set_ownership(ctx.plan, ownership);
    if (!xr_semantic_plan_freeze(ctx.plan, error, error_size))
        goto failure;
    if (!xr_semantic_plan_verify(ctx.plan, error, error_size))
        goto failure;
    ctx.plan->verified = true;
    xr_free(ctx.types);
    xr_free(ctx.functions);
    *out = ctx.plan;
    return true;

failure:
    xr_free(ctx.types);
    xr_free(ctx.functions);
    xr_semantic_plan_free(ctx.plan);
    return false;
}

static bool plan_tree_is_unattached(const XiFunc *function) {
    if (!function || function->semantic_plan)
        return false;
    for (uint16_t i = 0; i < function->nchildren; i++) {
        if (function->children[i] && !plan_tree_is_unattached(function->children[i]))
            return false;
    }
    return true;
}

static void attach_plan_tree(XiFunc *function, XrSemanticPlan *plan, bool transfer_reference) {
    if (!function)
        return;
    function->semantic_plan = transfer_reference ? plan : xr_semantic_plan_retain(plan);
    for (uint16_t i = 0; i < function->nchildren; i++)
        attach_plan_tree(function->children[i], plan, false);
}

bool xr_semantic_plan_build_and_attach(XiFunc *root, char *error, size_t error_size) {
    if (!root || !plan_tree_is_unattached(root)) {
        if (error && error_size)
            snprintf(error, error_size,
                     "XR_SEM_0011: Xi graph already has a SemanticPlan attachment");
        return false;
    }
    XrSemanticPlan *plan = NULL;
    if (!xr_semantic_plan_build(root, &plan, error, error_size))
        return false;
    attach_plan_tree(root, plan, true);
    return true;
}
