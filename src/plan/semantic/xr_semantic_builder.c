/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_builder.c - Xi to immutable SemanticPlan construction
 */

#include "xr_semantic_array_type_shape.h"
#include "xr_semantic_builder.h"
#include "xr_semantic_array_element_storage_shape.h"
#include "xr_semantic_builtin_identity_shape.h"
#include "xr_semantic_coroutine_lifecycle_shape.h"
#include "xr_semantic_class_shape.h"
#include "xr_semantic_string_runes_shape.h"
#include "xr_semantic_iterator_rune_has_next_shape.h"
#include "xr_semantic_iterator_rune_next_shape.h"
#include "xr_semantic_iterator_rune_nth_shape.h"
#include "xr_semantic_map_entry_iterator_shape.h"
#include "xr_semantic_rune_to_string_shape.h"
#include "xr_semantic_rune_to_uint32_shape.h"
#include "xr_semantic_rune_is_whitespace_shape.h"
#include "xr_semantic_string_slice_shape.h"
#include "xr_semantic_native_module_shape.h"
#include "xr_semantic_plan_internal.h"
#include "xr_semantic_verify.h"
#include "../../module/xmodule_identity.h"
#include "../ownership/xr_ownership_obligation.h"
#include "../ownership/xr_ownership_certificate_internal.h"
#include "../../base/xglobal_indices.h"
#include "../../base/xmalloc.h"
#include "../../frontend/analyzer/xa_intrinsic_registry.h"
#include "../../frontend/analyzer/xanalyzer_symbol.h"
#include "../../ir/xi.h"
#include "../../ir/xi_arc.h"
#include "../../ir/xi_coro_analyze.h"
#include "../../ir/xi_module.h"
#include "../../ir/xi_program_semantic.h"
#include "../../ir/xi_program_semantic_plan.h"
#include "../../ir/xi_own.h"
#include "../../ir/xi_builtin_map_entry_iterator_shape.h"
#include "../../ir/xi_ops_gen.h"
#include "../../ir/xi_value_query.h"
#include "../../runtime/value/xtype.h"
#include "../../runtime/value/xenum_layout.h"
#include "../../runtime/class/xclass_info.h"
#include "../../stdlib/xstdlib_metadata.h"
#include "../../shared/xr_exact_scalar_registry.h"
#include "xr_semantic_array_member_shape.h"
#include "xr_semantic_panic_info_shape.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define XR_SEMANTIC_MAX_TYPES UINT32_C(1000000)
#define XR_SEMANTIC_MAX_FUNCTIONS UINT32_C(100000)
#define XR_SEMANTIC_MAX_SOURCE_EXPORTS UINT32_C(200000)
#define XR_SEMANTIC_MAX_BLOCKS UINT32_C(2000000)
#define XR_SEMANTIC_MAX_OPERATIONS UINT32_C(10000000)
#define XR_SEMANTIC_MAX_CALL_TARGETS XR_SEMANTIC_MAX_OPERATIONS
#define XR_SEMANTIC_MAX_OPERANDS UINT32_C(40000000)
#define XR_SEMANTIC_MAX_EDGES UINT32_C(40000000)
#define XR_SEMANTIC_MAX_PARAMETERS (XR_SEMANTIC_MAX_FUNCTIONS * UINT32_C(256))
#define XR_SEMANTIC_MAX_CAPTURES (XR_SEMANTIC_MAX_FUNCTIONS * (uint32_t) XI_MAX_CAPTURES)
#define XR_SEMANTIC_MAX_ENTITIES UINT32_C(80000000)
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
    uint32_t parent;
    uint32_t source_class;
    uint16_t lexical_ordinal;
    uint16_t source_member_ordinal;
    uint8_t source_kind;
} XrFunctionMapEntry;

typedef struct XrSuspendabilityCacheEntry {
    const XrSemanticPlan *plan;
    uint8_t *functions;
} XrSuspendabilityCacheEntry;

typedef struct XrSemanticBuildContext {
    XrSemanticPlan *plan;
    const char *module_identity;
    XrTypeMapEntry *types;
    uint32_t type_count;
    uint32_t type_capacity;
    XrFunctionMapEntry *functions;
    uint32_t function_count;
    uint32_t function_capacity;
    bool types_canonicalized;
    XiModule *const *dependency_modules;
    uint32_t dependency_module_count;
    XrSuspendabilityCacheEntry *suspendability;
    uint32_t suspendability_count;
    uint32_t suspendability_capacity;
    const XiValue **root_shared_stores;
    uint8_t *root_shared_store_ambiguous;
    uint32_t root_shared_store_count;
    const XrProgramSemanticClosure *program_closure;
    const XrScalarCallDecision *construction_decision;
    const XrTargetProfile *construction_target_profile;
    XrSemanticProgramTypeBinding *program_type_bindings;
    uint32_t program_type_binding_count;
    XrSemanticProgramTypeFieldBinding *program_type_field_bindings;
    uint32_t program_type_field_binding_count;
    XrSemanticProgramFunctionBinding *program_function_bindings;
    uint32_t program_function_binding_count;
    XrSemanticProgramCallBinding *program_call_bindings;
    uint32_t program_call_binding_count;
    char *error;
    size_t error_size;
} XrSemanticBuildContext;

static const XiValue *find_unique_shared_store(const XiFunc *owner, int64_t slot, bool *ambiguous);
static bool root_store_precedes_activation(const XiFunc *owner, const XiValue *store);
static int resolve_closure_binding(const XrSemanticBuildContext *ctx, const XiFunc *owner,
                                   const XiValue *value);
static const XiValue *indexed_root_shared_store(const XrSemanticBuildContext *ctx, int64_t slot,
                                                bool *ambiguous);

static bool fail(XrSemanticBuildContext *ctx, const char *code, const char *detail) {
    if (ctx->error && ctx->error_size)
        snprintf(ctx->error, ctx->error_size, "%s: %s", code, detail);
    return false;
}

static bool xi_tree_has_program_binding(const XiFunc *function) {
    if (!function)
        return false;
    if (function->psc_function_index != XI_PSC_ROW_NONE ||
        function->psc_return_type_index != XI_PSC_ROW_NONE)
        return true;
    for (uint32_t b = 0; b < function->nblocks; b++) {
        const XiBlock *block = function->blocks[b];
        for (const XiPhi *phi = block ? block->phis : NULL; phi; phi = phi->next)
            if (phi->value.psc_call_index != XI_PSC_ROW_NONE ||
                phi->value.psc_type_index != XI_PSC_ROW_NONE)
                return true;
        for (uint32_t i = 0; block && i < block->nvalues; i++)
            if (block->values[i] && (block->values[i]->psc_call_index != XI_PSC_ROW_NONE ||
                                     block->values[i]->psc_type_index != XI_PSC_ROW_NONE))
                return true;
    }
    for (uint16_t p = 0; p < function->nparams; p++)
        if (function->params[p] && function->params[p]->psc_type_index != XI_PSC_ROW_NONE)
            return true;
    for (uint16_t i = 0; i < function->nchildren; i++)
        if (xi_tree_has_program_binding(function->children[i]))
            return true;
    return false;
}

static bool prepare_program_authority(XrSemanticBuildContext *ctx, const XiFunc *root) {
    const XiModule *module = root ? root->module : NULL;
    bool closure = module && module->program_semantic_closure;
    bool decision = module && module->scalar_call_decision;
    bool profile = module && module->scalar_target_profile;
    if (!closure && !decision && !profile) {
        if (xi_tree_has_program_binding(root))
            return fail(ctx, "XR_SEM_0019",
                        "Xi program rows have no retained construction authority");
        return true;
    }
    if (!closure || module->init != root)
        return fail(ctx, "XR_SEM_0019", "SemanticPlan construction authority is incomplete");
    XrProgramSemanticFamily family =
        xr_program_semantic_closure_family(module->program_semantic_closure);
    bool scalar = family == XR_PROGRAM_SEMANTIC_FAMILY_SCALAR_DIRECT_CALL;
    bool aggregate = family == XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_AGGREGATE_DIRECT_CALL;
    if ((!scalar && !aggregate) || (scalar && (!decision || !profile)) ||
        (aggregate && (decision || profile)) ||
        !xi_program_semantic_verify(module, scalar ? module->scalar_target_profile : NULL,
                                    ctx->error, ctx->error_size))
        return fail(ctx, "XR_SEM_0019", "SemanticPlan construction family is inconsistent");
    ctx->program_closure = module->program_semantic_closure;
    ctx->construction_decision = scalar ? module->scalar_call_decision : NULL;
    ctx->construction_target_profile = scalar ? module->scalar_target_profile : NULL;
    return true;
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

static bool text_append_path_component(XrTextBuilder *text, const char *value) {
    const char *path = value ? value : "";
    while (path[0] == '.' && (path[1] == '/' || path[1] == '\\'))
        path += 2;
    size_t length = strlen(path);
    if (!text_append_format(text, "%zu:", length) || !text_reserve(text, length))
        return false;
    for (size_t i = 0; i < length; i++)
        text->data[text->size++] = path[i] == '\\' ? '/' : path[i];
    text->data[text->size] = '\0';
    return true;
}

static const char *copy_canonical_source_file(XrSemanticBuildContext *ctx, const char *value) {
    const char *path = value ? value : "";
    while (path[0] == '.' && (path[1] == '/' || path[1] == '\\'))
        path += 2;
    size_t length = strlen(path);
    if (length == 0)
        return NULL;
    XrTextBuilder canonical = {0};
    if (!text_reserve(&canonical, length))
        return NULL;
    for (size_t i = 0; i < length; i++)
        canonical.data[canonical.size++] = path[i] == '\\' ? '/' : path[i];
    canonical.data[canonical.size] = '\0';
    const char *result = xr_semantic_plan_copy_string(ctx->plan, canonical.data);
    text_dispose(&canonical);
    return result;
}

/* Whether a type carries the VALUE flag.
 *
 * A declared struct is a value aggregate wherever it is named. The front end
 * sets `is_value_type` on the construction site and leaves it clear where the
 * type is merely referenced -- as a parameter type, a field type, an element
 * type -- so that bit describes the position, not the type. Reading it raw
 * interns one declaration as two semantic types whose records agree about
 * everything else, and only the construction site's copy ever gets an
 * allocation bound to it; the other copy then looks like an aggregate whose
 * fields nothing declares. Everything asking "is this a value aggregate" asks
 * here, so the key and the record flag can no longer disagree. */
static bool semantic_type_is_value(const XrType *type) {
    return type &&
           (type->is_value_type || (type->kind == XR_KIND_INSTANCE && type->instance.class_ref &&
                                    type->instance.class_ref->struct_layout));
}

static bool type_key(const XrType *type, XrTextBuilder *key, const XrType **stack, uint32_t depth,
                     XrSemanticBuildContext *ctx);
static uint32_t source_class_for_type(const XrSemanticBuildContext *ctx, const XrType *type);
static bool source_class_identity_for_type(const XrSemanticBuildContext *ctx, const XrType *type,
                                           XrStableId *out);
static uint8_t classify_import_resolution(const XiImportRef *ref);
static const XiValue *strip_identity_copies(const XiFunc *function, const XiValue *value);

static bool source_enum_key(const XrType *type, XrTextBuilder *key, XrStableId *identity,
                            uint8_t *flags) {
    if (!type || type->kind != XR_KIND_ENUM || !type->enum_type.layout ||
        !type->enum_type.layout->nominal_owner || !type->enum_type.layout->nominal_owner[0] ||
        !type->enum_type.layout->name || !type->enum_type.layout->name[0] ||
        !type->enum_type.layout->variants || type->enum_type.layout->variant_count == 0 ||
        type->enum_type.layout->variant_count > UINT16_MAX)
        return false;
    const XrEnumLayout *layout = type->enum_type.layout;
    if (type->enum_type.layout_id != layout->layout_id ||
        layout->layout_id != xr_enum_layout_nominal_id(layout) ||
        strcmp(type->enum_type.enum_name ? type->enum_type.enum_name : "", layout->name) != 0)
        return false;
    bool unit = layout->is_zero_payload;
    if (!text_append_format(key, "source-enum-v1:schema=%u:owner=", XR_SEMANTIC_SCHEMA_VERSION) ||
        !text_append_component(key, layout->nominal_owner) || !text_append(key, ":name=") ||
        !text_append_component(key, layout->name) ||
        !text_append_format(key, ":members=%u", layout->variant_count))
        return false;
    for (uint32_t i = 0; i < layout->variant_count; i++) {
        const XrEnumVariantLayout *variant = &layout->variants[i];
        if (!variant->name || !variant->name[0] || variant->tag != i ||
            !text_append_format(key, ":m%u=", i) || !text_append_component(key, variant->name) ||
            !text_append_format(key, ":payloads=%u", variant->payload_count))
            return false;
        unit = unit && variant->payload_count == 0;
    }
    XrFingerprint digest;
    if (!xr_stable_id_from_key(key->data, identity, &digest))
        return false;
    *flags = XR_SEM_ENUM_DECLARATION_EXACT | (unit ? XR_SEM_ENUM_UNIT : 0u);
    return true;
}

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
    if (!text_append_format(key, "type-v3:%u:%u:%u:%u:%u:%u:%u:%u:%u:%u:", (unsigned) type->kind,
                            type->semantic_type_id, xr_semantic_frozen_builtin_type(type),
                            type->is_nullable ? 1u : 0u, type->is_const ? 1u : 0u,
                            semantic_type_is_value(type) ? 1u : 0u, type->is_literal ? 1u : 0u,
                            type->is_cycle_candidate ? 1u : 0u, type->ptr_is_mut ? 1u : 0u,
                            (unsigned) type->scalar_rep) ||
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
        case XR_KIND_INTERFACE: {
            if (!text_append(key, ";named:") ||
                !text_append_component(key, type->instance.class_name) ||
                !type_key_list(type->instance.type_args, type->instance.type_arg_count, key, stack,
                               depth + 1, ctx))
                return false;
            XrStableId source_class = {{0}};
            return !source_class_identity_for_type(ctx, type, &source_class) ||
                   (text_append(key, ";source-class:") && text_append_stable_id(key, source_class));
        }
        case XR_KIND_FUNCTION:
            return type_key_function(type, key, stack, depth + 1, ctx);
        case XR_KIND_TYPE_PARAM:
            return text_append_format(key, ";param:%d:", type->type_param.id) &&
                   text_append_component(key, type->type_param.name);
        case XR_KIND_TUPLE:
            return type_key_list(type->tuple.element_types, type->tuple.element_count, key, stack,
                                 depth + 1, ctx);
        case XR_KIND_ENUM: {
            XrTextBuilder declaration = {0};
            XrStableId identity = {{0}};
            uint8_t flags = 0;
            bool exact = source_enum_key(type, &declaration, &identity, &flags);
            bool valid = text_append(key, ";enum:") &&
                         text_append_component(key, type->enum_type.enum_name) &&
                         text_append_format(key, ":%u:", type->enum_type.layout_id) &&
                         type_key_list(type->enum_type.type_args, type->enum_type.type_arg_count,
                                       key, stack, depth + 1, ctx) &&
                         (!exact || (text_append(key, ";source-enum:") &&
                                     text_append_stable_id(key, identity)));
            text_dispose(&declaration);
            return valid;
        }
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
static bool add_type(XrSemanticBuildContext *ctx, const XrType *type, uint32_t *out);

static bool merge_program_type_row(uint32_t candidate, uint32_t *match) {
    if (candidate == XI_PSC_ROW_NONE)
        return true;
    if (*match != XI_PSC_ROW_NONE && *match != candidate)
        return false;
    *match = candidate;
    return true;
}

static uint32_t program_type_row_for_source(const XrSemanticBuildContext *ctx, const XrType *type) {
    uint32_t match = XI_PSC_ROW_NONE;
    for (uint32_t f = 0; ctx && type && f < ctx->function_count; f++) {
        const XiFunc *function = ctx->functions[f].source;
        if (!function)
            return XI_PSC_ROW_NONE;
        if (function->return_type == type &&
            !merge_program_type_row(function->psc_return_type_index, &match))
            return XI_PSC_ROW_NONE;
        for (uint16_t p = 0; p < function->nparams; p++)
            if (function->params[p] && function->params[p]->type == type &&
                !merge_program_type_row(function->params[p]->psc_type_index, &match))
                return XI_PSC_ROW_NONE;
        for (uint32_t b = 0; b < function->nblocks; b++) {
            const XiBlock *block = function->blocks[b];
            for (const XiPhi *phi = block ? block->phis : NULL; phi; phi = phi->next)
                if (phi->value.type == type &&
                    !merge_program_type_row(phi->value.psc_type_index, &match))
                    return XI_PSC_ROW_NONE;
            for (uint32_t v = 0; block && v < block->nvalues; v++)
                if (block->values[v] && block->values[v]->type == type &&
                    !merge_program_type_row(block->values[v]->psc_type_index, &match))
                    return XI_PSC_ROW_NONE;
        }
    }
    return match;
}

static uint32_t program_type_row_for_id(const XrProgramSemanticClosure *closure,
                                        XrStableId identity) {
    uint32_t match = XI_PSC_ROW_NONE;
    for (uint32_t i = 0; closure && i < xr_program_semantic_closure_type_count(closure); i++) {
        const XrProgramSemanticTypeRecord *row = xr_program_semantic_closure_type(closure, i);
        if (!row || !xr_stable_id_equal(row->id, identity))
            continue;
        if (match != XI_PSC_ROW_NONE)
            return XI_PSC_ROW_NONE;
        match = i;
    }
    return match;
}

static uint32_t semantic_type_for_program_row(const XrSemanticBuildContext *ctx,
                                              uint32_t program_row) {
    const XrProgramSemanticTypeRecord *program_type =
        ctx && ctx->program_closure
            ? xr_program_semantic_closure_type(ctx->program_closure, program_row)
            : NULL;
    if (!program_type)
        return XR_SEMANTIC_INDEX_NONE;
    if (program_type->kind == XR_PROGRAM_SEMANTIC_TYPE_EXACT_SCALAR) {
        const XrExactScalarDesc *scalar =
            xr_exact_scalar_by_id((XrExactScalarId) program_type->exact_scalar);
        uint32_t match = XR_SEMANTIC_INDEX_NONE;
        for (uint32_t i = 0; scalar && i < ctx->plan->type_count; i++) {
            const XrSemanticTypeRecord *type = &ctx->plan->types[i];
            if (!((scalar->family == XR_EXACT_SCALAR_FAMILY_INTEGER && type->kind == XR_KIND_INT) ||
                  (scalar->family == XR_EXACT_SCALAR_FAMILY_FLOAT &&
                   type->kind == XR_KIND_FLOAT)) ||
                type->scalar_rep != scalar->native_type ||
                (type->flags & (XR_SEM_TYPE_NULLABLE | XR_SEM_TYPE_CONST | XR_SEM_TYPE_LITERAL)) !=
                    0)
                continue;
            if (match != XR_SEMANTIC_INDEX_NONE)
                return XR_SEMANTIC_INDEX_NONE;
            match = i;
        }
        return match;
    }
    uint32_t match = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; ctx && i < ctx->type_count; i++) {
        if (program_type_row_for_source(ctx, ctx->types[i].source) != program_row)
            continue;
        if (match != XR_SEMANTIC_INDEX_NONE && match != ctx->types[i].index)
            return XR_SEMANTIC_INDEX_NONE;
        match = ctx->types[i].index;
    }
    return match;
}

static const XiClassData *value_aggregate_class_data(const XrSemanticBuildContext *ctx,
                                                     const XrType *type) {
    if (!ctx || !type || type->kind != XR_KIND_INSTANCE || !type->instance.class_ref ||
        !type->instance.class_ref->struct_layout)
        return NULL;
    const XrAggregateLayout *layout = type->instance.class_ref->struct_layout;
    uint32_t class_id = type->instance.class_ref->xg_class_id;
    const XiClassData *match = NULL;
    for (uint32_t f = 0; f < ctx->function_count; f++) {
        const XiFunc *function = ctx->functions[f].source;
        for (uint32_t b = 0; b < function->nblocks; b++) {
            const XiBlock *block = function->blocks[b];
            if (!block)
                continue;
            for (uint32_t v = 0; v < block->nvalues; v++) {
                const XiValue *value = block->values[v];
                if (!value || value->op != XI_CLASS_CREATE)
                    continue;
                const XiClassData *data = (const XiClassData *) value->aux;
                if (!data || (data->class_info != type->instance.class_ref &&
                              data->struct_layout != layout &&
                              (class_id == 0 || data->xg_class_id != class_id)))
                    continue;
                if (match && match != data)
                    return NULL;
                match = data;
            }
        }
    }
    /* A packed struct differs from an ordinary one only in that the layout
     * leaves out the padding: the fields are the same fields, in the same
     * order, with the same names and types, so every answer this record
     * carries reads the same. A union is not admitted -- its fields overlap,
     * so a field index does not name a distinct place the way the rest of
     * this judgement assumes. */
    if (!match || match->needs_runtime_type || !match->struct_layout ||
        (match->struct_layout->kind != XR_AGG_LAYOUT_STRUCT &&
         match->struct_layout->kind != XR_AGG_LAYOUT_PACKED_STRUCT) ||
        match->instance_field_count != match->struct_layout->field_count ||
        (match->instance_field_count &&
         (!match->instance_field_types || !match->instance_field_names)))
        return NULL;
    for (uint16_t i = 0; i < match->instance_field_count; i++)
        if (!match->instance_field_types[i] || !match->instance_field_names[i] ||
            match->struct_layout->fields[i].is_flexible)
            return NULL;
    return match;
}

static const XiClassData *value_aggregate_data_for_type(const XrSemanticBuildContext *ctx,
                                                        uint32_t semantic_type,
                                                        const XrType **source_out) {
    const XiClassData *match = NULL;
    const XrType *source = NULL;
    for (uint32_t i = 0; i < ctx->type_count; i++) {
        if (ctx->types[i].index != semantic_type)
            continue;
        const XiClassData *candidate = value_aggregate_class_data(ctx, ctx->types[i].source);
        if (!candidate)
            continue;
        if (match && match != candidate)
            return NULL;
        match = candidate;
        source = ctx->types[i].source;
    }
    if (source_out)
        *source_out = source;
    return match;
}

static const XrProgramSemanticTypeRecord *
program_aggregate_for_semantic_type(const XrSemanticBuildContext *ctx, uint32_t semantic_type,
                                    uint32_t *program_row_out) {
    uint32_t match = XI_PSC_ROW_NONE;
    const XrProgramSemanticTypeRecord *row = NULL;
    for (uint32_t i = 0; ctx && ctx->program_closure && i < ctx->type_count; i++) {
        if (ctx->types[i].index != semantic_type)
            continue;
        uint32_t candidate = program_type_row_for_source(ctx, ctx->types[i].source);
        if (candidate == XI_PSC_ROW_NONE)
            continue;
        const XrProgramSemanticTypeRecord *candidate_row =
            xr_program_semantic_closure_type(ctx->program_closure, candidate);
        if (!candidate_row || candidate_row->kind != XR_PROGRAM_SEMANTIC_TYPE_LEAF_VALUE_AGGREGATE)
            continue;
        if (match != XI_PSC_ROW_NONE && match != candidate)
            return NULL;
        match = candidate;
        row = candidate_row;
    }
    if (program_row_out)
        *program_row_out = match;
    return row;
}

static bool program_aggregate_source_is_exact(const XrSemanticBuildContext *ctx,
                                              const XrType *source,
                                              const XrProgramSemanticTypeRecord *aggregate) {
    if (!ctx || !ctx->program_closure || !source || !aggregate ||
        aggregate->kind != XR_PROGRAM_SEMANTIC_TYPE_LEAF_VALUE_AGGREGATE ||
        source->kind != XR_KIND_INSTANCE || source->is_nullable || source->is_const ||
        source->is_literal || source->instance.type_arg_count != 0 || !source->instance.class_ref)
        return false;
    const XrClassInfo *info = source->instance.class_ref;
    if (!info->struct_layout || info->is_overlay_union || info->base || info->base_name ||
        info->interface_count != 0 || info->method_count != 0 || info->field_count <= 0 ||
        (uint32_t) info->field_count != aggregate->field_count || !info->fields)
        return false;
    for (uint32_t field = 0; field < aggregate->field_count; field++) {
        const XrProgramSemanticTypeFieldRecord *field_row = xr_program_semantic_closure_type_field(
            ctx->program_closure, aggregate->field_begin + field);
        uint32_t child_program =
            field_row ? program_type_row_for_id(ctx->program_closure, field_row->field_type)
                      : XI_PSC_ROW_NONE;
        const XrProgramSemanticTypeRecord *child =
            child_program != XI_PSC_ROW_NONE
                ? xr_program_semantic_closure_type(ctx->program_closure, child_program)
                : NULL;
        const XrExactScalarDesc *scalar =
            child && child->kind == XR_PROGRAM_SEMANTIC_TYPE_EXACT_SCALAR
                ? xr_exact_scalar_by_id((XrExactScalarId) child->exact_scalar)
                : NULL;
        const XaSymbol *source_field = info->fields[field];
        const XrType *source_field_type = source_field ? source_field->links.type : NULL;
        if (!field_row || field_row->declaration_ordinal != field || !scalar || !source_field ||
            source_field->is_static || source_field->is_weak || !source_field_type ||
            !((scalar->family == XR_EXACT_SCALAR_FAMILY_INTEGER &&
               source_field_type->kind == XR_KIND_INT) ||
              (scalar->family == XR_EXACT_SCALAR_FAMILY_FLOAT &&
               source_field_type->kind == XR_KIND_FLOAT)) ||
            source_field_type->scalar_rep != scalar->native_type ||
            source_field_type->is_nullable || source_field_type->is_const ||
            source_field_type->is_literal)
            return false;
    }
    return true;
}

static bool refine_value_aggregate_types(XrSemanticBuildContext *ctx) {
    for (uint32_t semantic_type = 0; semantic_type < ctx->plan->type_count; semantic_type++) {
        XrSemanticTypeRecord *record = &ctx->plan->types[semantic_type];
        if (record->kind != XR_KIND_INSTANCE || (record->flags & XR_SEM_TYPE_VALUE) == 0)
            continue;
        const XrProgramSemanticTypeRecord *program_aggregate =
            program_aggregate_for_semantic_type(ctx, semantic_type, NULL);
        if (program_aggregate) {
            const XrClassInfo *aggregate_info = NULL;
            for (uint32_t i = 0; i < ctx->type_count; i++) {
                if (ctx->types[i].index != semantic_type)
                    continue;
                const XrType *candidate = ctx->types[i].source;
                if (!candidate || candidate->kind != XR_KIND_INSTANCE ||
                    !candidate->instance.class_ref)
                    continue;
                if (!program_aggregate_source_is_exact(ctx, candidate, program_aggregate))
                    return fail(ctx, "XR_SEM_0019",
                                "program aggregate source type is inconsistent");
                if (!aggregate_info)
                    aggregate_info = candidate->instance.class_ref;
            }
            if (!aggregate_info || !aggregate_info->fields || aggregate_info->field_count <= 0 ||
                (uint32_t) aggregate_info->field_count != program_aggregate->field_count)
                return fail(ctx, "XR_SEM_0019", "program aggregate source fields are incomplete");
            uint32_t *indices =
                program_aggregate->field_count
                    ? (uint32_t *) xr_malloc((size_t) program_aggregate->field_count *
                                             sizeof(*indices))
                    : NULL;
            if (program_aggregate->field_count && !indices)
                return fail(ctx, "XR_EXEC_5003", "program aggregate child allocation failed");
            for (uint32_t field = 0; field < program_aggregate->field_count; field++) {
                const XrProgramSemanticTypeFieldRecord *field_row =
                    xr_program_semantic_closure_type_field(ctx->program_closure,
                                                           program_aggregate->field_begin + field);
                uint32_t child_program =
                    field_row ? program_type_row_for_id(ctx->program_closure, field_row->field_type)
                              : XI_PSC_ROW_NONE;
                const XrProgramSemanticTypeRecord *child_row =
                    child_program != XI_PSC_ROW_NONE
                        ? xr_program_semantic_closure_type(ctx->program_closure, child_program)
                        : NULL;
                const XaSymbol *source_field = aggregate_info->fields[field];
                const XrType *source_field_type = source_field ? source_field->links.type : NULL;
                const XrExactScalarDesc *scalar =
                    child_row && child_row->kind == XR_PROGRAM_SEMANTIC_TYPE_EXACT_SCALAR
                        ? xr_exact_scalar_by_id((XrExactScalarId) child_row->exact_scalar)
                        : NULL;
                if (!scalar || !source_field_type || source_field->is_static ||
                    source_field->is_weak || source_field_type->scalar_rep != scalar->native_type ||
                    !((scalar->family == XR_EXACT_SCALAR_FAMILY_INTEGER &&
                       source_field_type->kind == XR_KIND_INT) ||
                      (scalar->family == XR_EXACT_SCALAR_FAMILY_FLOAT &&
                       source_field_type->kind == XR_KIND_FLOAT)) ||
                    source_field_type->is_nullable || source_field_type->is_const ||
                    source_field_type->is_literal ||
                    !add_type(ctx, source_field_type, &indices[field])) {
                    xr_free(indices);
                    return fail(ctx, "XR_SEM_0019", "program aggregate field type is inconsistent");
                }
                if (!field_row || field_row->declaration_ordinal != field ||
                    indices[field] == XR_SEMANTIC_INDEX_NONE) {
                    xr_free(indices);
                    return fail(ctx, "XR_SEM_0019",
                                "program aggregate child binding is incomplete");
                }
            }
            record = &ctx->plan->types[semantic_type];
            record->child_begin = ctx->plan->type_child_count;
            record->child_count = (uint16_t) program_aggregate->field_count;
            record->aggregate_extent = program_aggregate->field_count;
            record->aggregate_align = 0;
            record->builtin_type = XR_TID_NULL;
            record->scalar_rep = XR_SCALAR_REP_NONE;
            record->flags = XR_SEM_TYPE_VALUE | XR_SEM_TYPE_AGGREGATE_EXACT;
            for (uint32_t field = 0; field < program_aggregate->field_count; field++) {
                if (!append_u32(&ctx->plan->type_children, &ctx->plan->type_child_count,
                                &ctx->plan->type_child_capacity, indices[field],
                                XR_SEMANTIC_MAX_TYPES * 8u)) {
                    xr_free(indices);
                    return fail(ctx, "XR_EXEC_5003", "program aggregate child budget exhausted");
                }
            }
            xr_free(indices);
            continue;
        }
        bool declared_aggregate = false;
        for (uint32_t i = 0; i < ctx->type_count; i++) {
            const XrType *source = ctx->types[i].source;
            /* The raw bit, deliberately: this asks whether *this* module declares
             * the aggregate, and the front end sets it exactly where the
             * declaration is used to build one. Asking the shared judgement here
             * would count a type merely imported and referenced as declared, and
             * then demand declaration facts no module in this plan holds. */
            if (ctx->types[i].index == semantic_type && source &&
                source->kind == XR_KIND_INSTANCE && source->is_value_type &&
                source->instance.class_ref && source->instance.class_ref->struct_layout) {
                declared_aggregate = true;
                break;
            }
        }
        const XiClassData *aggregate = value_aggregate_data_for_type(ctx, semantic_type, NULL);
        if (!aggregate) {
            if (declared_aggregate) {
                if (ctx->error && ctx->error_size)
                    snprintf(ctx->error, ctx->error_size,
                             "XR_SEM_0019: value aggregate declaration facts are unavailable "
                             "(type=%s)",
                             record->canonical_key ? record->canonical_key : "<unknown>");
                return false;
            }
            continue;
        }
        uint32_t *indices = aggregate->instance_field_count
                                ? (uint32_t *) xr_malloc((size_t) aggregate->instance_field_count *
                                                         sizeof(*indices))
                                : NULL;
        if (aggregate->instance_field_count && !indices)
            return fail(ctx, "XR_EXEC_5003", "value aggregate child allocation failed");
        for (uint16_t field = 0; field < aggregate->instance_field_count; field++) {
            if (!add_type(ctx, aggregate->instance_field_types[field], &indices[field])) {
                xr_free(indices);
                return false;
            }
        }
        record = &ctx->plan->types[semantic_type];
        record->child_begin = ctx->plan->type_child_count;
        record->child_count = aggregate->instance_field_count;
        record->aggregate_extent = aggregate->instance_field_count;
        record->aggregate_align = aggregate->struct_layout->explicit_align;
        record->flags |= XR_SEM_TYPE_AGGREGATE_EXACT;
        for (uint16_t field = 0; field < aggregate->instance_field_count; field++) {
            if (!append_u32(&ctx->plan->type_children, &ctx->plan->type_child_count,
                            &ctx->plan->type_child_capacity, indices[field],
                            XR_SEMANTIC_MAX_TYPES * 8u)) {
                xr_free(indices);
                return fail(ctx, "XR_EXEC_5003", "value aggregate child budget exhausted");
            }
        }
        xr_free(indices);
    }
    return true;
}

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
    record->builtin_type = xr_semantic_frozen_builtin_type(type);
    record->source_class = source_class_for_type(ctx, type);
    (void) source_class_identity_for_type(ctx, type, &record->source_class_identity);
    if (type->kind == XR_KIND_ENUM) {
        XrTextBuilder enum_key = {0};
        bool exact =
            source_enum_key(type, &enum_key, &record->source_enum_identity, &record->enum_flags);
        record->source_enum_key =
            exact ? xr_semantic_plan_copy_string(ctx->plan, enum_key.data) : NULL;
        text_dispose(&enum_key);
        if (exact && !record->source_enum_key)
            return fail(ctx, "XR_EXEC_5003", "enum declaration identity allocation failed");
        if (exact) {
            record->enum_layout_id = type->enum_type.layout->layout_id;
            record->enum_member_count = (uint16_t) type->enum_type.layout->variant_count;
        }
    }
    record->scalar_rep = type->scalar_rep;
    bool reference_capable =
        xr_semantic_frozen_builtin_type(type) != XR_TID_NULL || xi_own_type_is_rc(type);
    bool borrow_view = type->kind == XR_KIND_SLICE;
    record->flags =
        (uint8_t) ((type->is_nullable ? XR_SEM_TYPE_NULLABLE : 0u) |
                   (type->is_const ? XR_SEM_TYPE_CONST : 0u) |
                   (semantic_type_is_value(type) ? XR_SEM_TYPE_VALUE : 0u) |
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
        case XR_KIND_INTERFACE:
            count = (uint32_t) type->instance.type_arg_count;
            break;
        case XR_KIND_INSTANCE: {
            uint32_t program_row = program_type_row_for_source(ctx, type);
            const XrProgramSemanticTypeRecord *program_type =
                ctx->program_closure && program_row != XI_PSC_ROW_NONE
                    ? xr_program_semantic_closure_type(ctx->program_closure, program_row)
                    : NULL;
            const XiClassData *data =
                program_type && program_type->kind == XR_PROGRAM_SEMANTIC_TYPE_LEAF_VALUE_AGGREGATE
                    ? NULL
                    : value_aggregate_class_data(ctx, type);
            count =
                program_type && program_type->kind == XR_PROGRAM_SEMANTIC_TYPE_LEAF_VALUE_AGGREGATE
                    ? 0u
                : data ? data->instance_field_count
                       : (uint32_t) type->instance.type_arg_count;
            break;
        }
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
            case XR_KIND_INTERFACE:
                child = type->instance.type_args[i];
                break;
            case XR_KIND_INSTANCE: {
                const XiClassData *data = value_aggregate_class_data(ctx, type);
                child = data ? data->instance_field_types[i] : type->instance.type_args[i];
                break;
            }
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
    if (type->kind == XR_KIND_TUPLE || type->kind == XR_KIND_STRUCT_OBJECT)
        record->aggregate_extent = count;
    else if (type->kind == XR_KIND_FIXED_ARRAY) {
        if (type->fixed_array.length <= 0)
            return fail(ctx, "XR_SEM_0012", "fixed-array type has an invalid exact extent");
        record->aggregate_extent = (uint32_t) type->fixed_array.length;
    } else if (type->kind == XR_KIND_INSTANCE) {
        uint32_t program_row = program_type_row_for_source(ctx, type);
        const XrProgramSemanticTypeRecord *program_type =
            ctx->program_closure && program_row != XI_PSC_ROW_NONE
                ? xr_program_semantic_closure_type(ctx->program_closure, program_row)
                : NULL;
        const XiClassData *aggregate =
            program_type && program_type->kind == XR_PROGRAM_SEMANTIC_TYPE_LEAF_VALUE_AGGREGATE
                ? NULL
                : value_aggregate_class_data(ctx, type);
        if (aggregate) {
            record->aggregate_extent = aggregate->instance_field_count;
            record->aggregate_align = aggregate->struct_layout->explicit_align;
            record->flags |= XR_SEM_TYPE_AGGREGATE_EXACT;
        }
    }
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

static bool collect_functions(XrSemanticBuildContext *ctx, const XiFunc *function, uint32_t parent,
                              uint16_t lexical_ordinal) {
    if (!function || ctx->function_count >= XR_SEMANTIC_MAX_FUNCTIONS ||
        !reserve_array((void **) &ctx->functions, &ctx->function_capacity, ctx->function_count + 1,
                       sizeof(*ctx->functions), XR_SEMANTIC_MAX_FUNCTIONS))
        return fail(ctx, "XR_EXEC_5003", "semantic function budget exhausted");
    uint32_t index = ctx->function_count;
    ctx->functions[ctx->function_count++] = (XrFunctionMapEntry) {function,
                                                                  index,
                                                                  0,
                                                                  0,
                                                                  parent,
                                                                  XR_SEMANTIC_INDEX_NONE,
                                                                  lexical_ordinal,
                                                                  UINT16_MAX,
                                                                  XR_SEM_SOURCE_FUNCTION_NONE};
    for (uint16_t i = 0; i < function->nchildren; i++) {
        if (!collect_functions(ctx, function->children[i], index, i))
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
        for (uint16_t c = 0; c < function->ncaptures; c++) {
            if (!function->captures[c].type || !add_type(ctx, function->captures[c].type, &ignored))
                return fail(ctx, "XR_SEM_0018", "semantic function has an untyped capture");
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

static bool module_stable_identity(XrSemanticBuildContext *ctx, const XiFunc *root,
                                   XrStableId *module_id, const char **module_path_out) {
    const char *identity = root && root->module ? root->module->identity : NULL;
    const char *module_name =
        root->module && root->module->name ? root->module->name : (root->name ? root->name : "");
    XrTextBuilder package_key = {0};
    XrTextBuilder module_key = {0};
    XrStableId package_id = {{0}};
    XrFingerprint digest;
    const char *frozen_identity =
        identity && identity[0] ? xr_semantic_plan_copy_string(ctx->plan, identity) : NULL;
    bool valid =
        frozen_identity &&
        text_append_format(&package_key, "entity-v1:schema=%u:kind=%u:parent=none:authority=",
                           XR_SEMANTIC_SCHEMA_VERSION, (unsigned) XR_SEM_ENTITY_PACKAGE) &&
        text_append_component(&package_key, frozen_identity) &&
        xr_stable_id_from_key(package_key.data, &package_id, &digest) &&
        text_append_format(&module_key,
                           "entity-v1:schema=%u:kind=%u:parent=", XR_SEMANTIC_SCHEMA_VERSION,
                           (unsigned) XR_SEM_ENTITY_MODULE) &&
        text_append_stable_id(&module_key, package_id) && text_append(&module_key, ":name=") &&
        text_append_component(&module_key, module_name) && text_append(&module_key, ":identity=") &&
        text_append_component(&module_key, frozen_identity) &&
        xr_stable_id_from_key(module_key.data, module_id, &digest);
    text_dispose(&package_key);
    text_dispose(&module_key);
    if (!valid)
        return fail(ctx, "XR_SEM_0019", "source-class module identity is incomplete");
    *module_path_out = frozen_identity;
    return true;
}

static bool build_source_classes(XrSemanticBuildContext *ctx, const XiFunc *root) {
    const XiModule *module = root ? root->module : NULL;
    if (!module)
        return true;
    if (module->init != root)
        return fail(ctx, "XR_SEM_0019", "source-class module ownership is incomplete");
    if (module->nclasses == 0)
        return true;
    XrStableId module_id;
    const char *module_path = NULL;
    if (!module_stable_identity(ctx, root, &module_id, &module_path))
        return false;
    if (module->nclasses > XR_SEMANTIC_MAX_FUNCTIONS || (module->nclasses && !module->classes) ||
        !reserve_array((void **) &ctx->plan->source_classes, &ctx->plan->source_class_capacity,
                       module->nclasses, sizeof(*ctx->plan->source_classes),
                       XR_SEMANTIC_MAX_FUNCTIONS))
        return fail(ctx, "XR_EXEC_5003", "semantic source-class budget exhausted");
    for (uint16_t c = 0; c < module->nclasses; c++) {
        const XiClassData *source = module->classes[c];
        if (!source || !source->class_name || !source->class_name[0] ||
            source->nmethod != (uint16_t) (source->ninst + source->nstat) ||
            (source->nmethod && (!source->methods || !source->child_idx)))
            return fail(ctx, "XR_SEM_0019", "source-class method authority is incomplete");
        XrSemanticSourceClassRecord *record = &ctx->plan->source_classes[c];
        memset(record, 0, sizeof(*record));
        record->module = module_id;
        record->module_path = module_path;
        record->name = xr_semantic_plan_copy_string(ctx->plan, source->class_name);
        record->super_name = source->super_name && source->super_name[0]
                                 ? xr_semantic_plan_copy_string(ctx->plan, source->super_name)
                                 : NULL;
        if (source->super_name && source->super_name[0] && !record->super_name)
            return fail(ctx, "XR_SEM_0019", "source-class parent name is incomplete");
        record->ordinal = c;
        record->method_count = source->nmethod;
        record->flags =
            (uint8_t) ((source->explicit_final ? XR_SEM_SOURCE_CLASS_EXPLICIT_FINAL : 0u) |
                       (source->needs_runtime_type ? XR_SEM_SOURCE_CLASS_RUNTIME_TYPE : 0u) |
                       ((source->is_generic_skeleton || source->is_monomorphized ||
                         source->mono_type_arg_count != 0)
                            ? XR_SEM_SOURCE_CLASS_GENERIC
                            : 0u));
        XrTextBuilder key = {0};
        bool valid =
            record->name &&
            text_append_format(&key,
                               "source-class-v1:schema=%u:module=", XR_SEMANTIC_SCHEMA_VERSION) &&
            text_append_stable_id(&key, record->module) && text_append(&key, ":path=") &&
            text_append_path_component(&key, record->module_path) && text_append(&key, ":name=") &&
            text_append_component(&key, record->name) && text_append(&key, ":super=") &&
            text_append_component(&key, record->super_name ? record->super_name : "") &&
            text_append_format(&key, ":ordinal=%u:methods=%u:flags=%u", record->ordinal,
                               record->method_count, record->flags);
        if (valid)
            record->canonical_key = xr_semantic_plan_copy_string(ctx->plan, key.data);
        text_dispose(&key);
        XrFingerprint digest;
        if (!valid || !record->canonical_key ||
            !xr_stable_id_from_key(record->canonical_key, &record->id, &digest))
            return fail(ctx, "XR_SEM_0019", "source-class identity is incomplete");
        for (uint16_t m = 0; m < source->nmethod; m++) {
            uint16_t child = source->child_idx[m];
            if (child >= root->nchildren || !root->children[child] || !source->methods[m].name)
                return fail(ctx, "XR_SEM_0019", "source-class method binding is incomplete");
            int function = function_index(ctx, root->children[child]);
            if (function < 0 || ctx->functions[function].source_class != XR_SEMANTIC_INDEX_NONE)
                return fail(ctx, "XR_SEM_0019", "source-class method binding is ambiguous");
            XrFunctionMapEntry *mapped = &ctx->functions[function];
            mapped->source_class = c;
            mapped->source_member_ordinal = m;
            mapped->source_kind =
                source->methods[m].is_constructor ? XR_SEM_SOURCE_FUNCTION_CONSTRUCTOR
                : source->methods[m].is_static    ? XR_SEM_SOURCE_FUNCTION_STATIC_METHOD
                                                  : XR_SEM_SOURCE_FUNCTION_INSTANCE_METHOD;
            if (strcmp(mapped->source->name ? mapped->source->name : "", source->methods[m].name) !=
                0)
                return fail(ctx, "XR_SEM_0019", "source-class method name is inconsistent");
        }
    }
    ctx->plan->source_class_count = module->nclasses;
    return true;
}

/* Whether this class declaration is the one the type names.
 *
 * The analyzer's class pointer is the exact answer and is used whenever the
 * type carries one. Not every XrType for a class does: instance.class_ref is
 * filled at some construction sites and left null at others, so whether a given
 * type object has it is a property of the expression that produced the object,
 * not of the class it denotes. That distinction cannot reach the canonical key.
 * The key records this answer, and a key is a stable identity: if the pointer
 * were the only route, the same declared class would hash one way in the module
 * that declares it and another way in a module that only names it, and no
 * cross-module contract mentioning that type could ever agree with itself --
 * which is what refused every direct call into io, log, http, ws, and the pure
 * stdlib modules. The declared name closes that gap for the pointerless
 * objects; callers still discard a name that matches more than one identity. */
static bool source_class_matches_type(const XiClassData *source, const XrType *type,
                                      uint32_t class_id) {
    if (!source || !type)
        return false;
    if (type->instance.class_ref)
        return source->class_info == type->instance.class_ref ||
               (class_id != 0 && source->xg_class_id == class_id);
    return type->instance.class_name && source->class_name &&
           strcmp(source->class_name, type->instance.class_name) == 0;
}

static uint32_t source_class_for_type(const XrSemanticBuildContext *ctx, const XrType *type) {
    if (!type || (type->kind != XR_KIND_CLASS && type->kind != XR_KIND_INSTANCE))
        return XR_SEMANTIC_INDEX_NONE;
    const XiModule *module = ctx->functions[0].source->module;
    uint32_t match = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t c = 0; module && c < module->nclasses; c++) {
        if (!source_class_matches_type(module->classes[c], type, 0))
            continue;
        if (match != XR_SEMANTIC_INDEX_NONE)
            return XR_SEMANTIC_INDEX_NONE;
        match = c;
    }
    return match;
}

static bool source_class_identity_for_type(const XrSemanticBuildContext *ctx, const XrType *type,
                                           XrStableId *out) {
    memset(out, 0, sizeof(*out));
    if (!type || (type->kind != XR_KIND_CLASS && type->kind != XR_KIND_INSTANCE))
        return false;
    uint32_t local = source_class_for_type(ctx, type);
    if (local != XR_SEMANTIC_INDEX_NONE) {
        *out = ctx->plan->source_classes[local].id;
        return true;
    }
    const XrSemanticSourceClassRecord *match = NULL;
    uint32_t class_id = type->instance.class_ref ? type->instance.class_ref->xg_class_id : 0;
    for (uint32_t m = 0; m < ctx->dependency_module_count; m++) {
        const XiModule *module = ctx->dependency_modules[m];
        const XrSemanticPlan *plan = module && module->init ? module->init->semantic_plan : NULL;
        if (!plan || plan->schema != XR_SEMANTIC_SCHEMA_VERSION || !plan->frozen || !plan->verified)
            continue;
        for (uint32_t c = 0; c < module->nclasses; c++) {
            if (c >= plan->source_class_count ||
                !source_class_matches_type(module->classes[c], type, class_id))
                continue;
            const XrSemanticSourceClassRecord *candidate = &plan->source_classes[c];
            if (match && !xr_stable_id_equal(match->id, candidate->id))
                return false;
            match = candidate;
        }
    }
    if (!match)
        return false;
    *out = match->id;
    return true;
}

static bool build_function_identity(XrSemanticBuildContext *ctx, uint32_t index,
                                    const XiFunc *source, XrSemanticFunctionRecord *record) {
    record->name = xr_semantic_plan_copy_string(ctx->plan, source->name ? source->name : "");
    if (!record->name || !add_type(ctx, source->return_type, &record->return_type))
        return false;
    record->parameter_count = xi_func_semantic_param_count(source);
    record->source_class = ctx->functions[index].source_class;
    record->source_member_ordinal = ctx->functions[index].source_member_ordinal;
    record->source_kind = ctx->functions[index].source_kind;
    XrTextBuilder key = {0};
    uint32_t parent = ctx->functions[index].parent;
    bool valid =
        text_append(&key, "function-v3:parent=") &&
        (parent == XR_SEMANTIC_INDEX_NONE
             ? text_append(&key, "module-root")
             : text_append_stable_id(&key, ctx->plan->functions[parent].id)) &&
        text_append_format(&key, ":ordinal=%u:name=", ctx->functions[index].lexical_ordinal) &&
        text_append_component(&key, source->name) && text_append(&key, ":source-class=") &&
        (record->source_class == XR_SEMANTIC_INDEX_NONE
             ? text_append(&key, "none")
             : text_append_stable_id(&key, ctx->plan->source_classes[record->source_class].id)) &&
        (record->source_member_ordinal == UINT16_MAX
             ? text_append_format(&key, ":member=none:source-kind=%u:return=", record->source_kind)
             : text_append_format(&key, ":member=%u:source-kind=%u:return=",
                                  record->source_member_ordinal, record->source_kind)) &&
        text_append_stable_id(&key, ctx->plan->types[record->return_type].id) &&
        text_append_format(&key, ":params=%u", record->parameter_count);
    for (uint16_t p = 0; valid && p < record->parameter_count; p++) {
        uint32_t type_index;
        valid = source->params[p] && add_type(ctx, source->params[p]->type, &type_index) &&
                text_append_format(&key, ":p%u:mode=%u:type=", p,
                                   (unsigned) source->params[p]->param_mode) &&
                text_append_stable_id(&key, ctx->plan->types[type_index].id);
    }
    valid =
        valid && text_append_format(&key, ":effects=%u:caps=%u:flags=%u", record->semantic_effects,
                                    record->capability_mask, (unsigned) record->flags);
    if (valid)
        record->canonical_key = xr_semantic_plan_copy_string(ctx->plan, key.data);
    text_dispose(&key);
    XrFingerprint digest;
    if (!valid || !record->canonical_key ||
        !xr_stable_id_from_key(record->canonical_key, &record->id, &digest))
        return fail(ctx, "XR_SEM_0013", "semantic function identity is incomplete");
    return true;
}

static bool build_source_methods(XrSemanticBuildContext *ctx) {
    for (uint32_t f = 0; f < ctx->plan->function_count; f++) {
        const XrSemanticFunctionRecord *function = &ctx->plan->functions[f];
        if (function->source_kind != XR_SEM_SOURCE_FUNCTION_INSTANCE_METHOD)
            continue;
        if (function->source_class >= ctx->plan->source_class_count ||
            function->source_member_ordinal == UINT16_MAX || !function->name || !function->name[0])
            return fail(ctx, "XR_SEM_0019", "source method declaration is incomplete");
        if (!reserve_array((void **) &ctx->plan->source_methods, &ctx->plan->source_method_capacity,
                           ctx->plan->source_method_count + 1, sizeof(*ctx->plan->source_methods),
                           XR_SEMANTIC_MAX_FUNCTIONS))
            return fail(ctx, "XR_EXEC_5003", "semantic source-method budget exhausted");
        XrSemanticSourceMethodRecord *record =
            &ctx->plan->source_methods[ctx->plan->source_method_count++];
        memset(record, 0, sizeof(*record));
        const XrSemanticSourceClassRecord *source_class =
            &ctx->plan->source_classes[function->source_class];
        record->name = xr_semantic_plan_copy_string(ctx->plan, function->name);
        record->source_class = function->source_class;
        record->function = f;
        record->member_ordinal = function->source_member_ordinal;
        record->parameter_count = function->parameter_count;
        record->flags = (uint8_t) (XR_SEM_SOURCE_METHOD_INSTANCE |
                                   ((source_class->flags & XR_SEM_SOURCE_CLASS_EXPLICIT_FINAL) == 0
                                        ? XR_SEM_SOURCE_METHOD_OPEN_DOMAIN
                                        : 0u));
        XrTextBuilder key = {0};
        bool valid =
            record->name &&
            text_append_format(&key,
                               "source-method-v1:schema=%u:class=", XR_SEMANTIC_SCHEMA_VERSION) &&
            text_append_stable_id(&key, source_class->id) &&
            text_append_format(&key, ":member=%u:name=", record->member_ordinal) &&
            text_append_component(&key, record->name) && text_append(&key, ":function=") &&
            text_append_stable_id(&key, function->id) &&
            text_append_format(&key, ":params=%u:flags=%u", record->parameter_count, record->flags);
        if (valid)
            record->canonical_key = xr_semantic_plan_copy_string(ctx->plan, key.data);
        text_dispose(&key);
        XrFingerprint digest;
        if (!valid || !record->canonical_key ||
            !xr_stable_id_from_key(record->canonical_key, &record->id, &digest))
            return fail(ctx, "XR_SEM_0019", "source method identity is incomplete");
    }
    return true;
}

static void set_function_return_contract(const XiFunc *source, XrSemanticFunctionRecord *record) {
    record->return_parameter = source->arc_return_ownership.param_index;
    record->is_module_initializer = source->is_module_initializer ? 1u : 0u;
    /* Walks the body once: a function carries coroutine operations when any of
     * its own values is one. Only the body counts -- a call to a coroutine is
     * an ordinary call here. */
    record->carries_coroutine_ops = 0u;
    for (uint32_t cb = 0; !record->carries_coroutine_ops && cb < source->nblocks; cb++) {
        const XiBlock *block = source->blocks[cb];
        if (!block)
            continue;
        for (uint32_t cv = 0; cv < block->nvalues; cv++) {
            const XiValue *candidate = block->values[cv];
            if (candidate && xi_generated_op_class(candidate->op) == XI_GEN_CLASS_COROUTINE) {
                record->carries_coroutine_ops = 1u;
                break;
            }
        }
    }
    record->return_provenance = source->arc_return_ownership.kind;
    if (source->return_type && source->return_type->kind == XR_KIND_SLICE) {
        record->return_parameter = -1;
        record->return_provenance = XR_SEM_RETURN_NONE;
        if (source->view_return_complete && source->view_return_source == XR_VIEW_RETURN_PARAM) {
            record->return_provenance = XR_SEM_RETURN_BORROWED_PARAM;
            record->return_parameter = source->view_return_param;
        } else if (source->view_return_complete &&
                   source->view_return_source == XR_VIEW_RETURN_RECEIVER) {
            record->return_provenance = XR_SEM_RETURN_BORROWED_PARAM;
            record->return_parameter = 0;
        } else if (source->view_return_complete &&
                   source->view_return_source == XR_VIEW_RETURN_STATIC) {
            record->return_provenance = XR_SEM_RETURN_BORROWED_STATIC;
        }
    } else if (source->entry_type == 2 && xi_own_type_is_rc(source->return_type)) {
        record->return_provenance = XR_SEM_RETURN_OWNED;
        record->return_parameter = -1;
    }
}

static bool append_parameter_records(XrSemanticBuildContext *ctx, uint32_t function_index_value,
                                     const XiFunc *source,
                                     const XrSemanticFunctionRecord *function) {
    XrFingerprint digest;
    for (uint16_t p = 0; p < function->parameter_count; p++) {
        if (!reserve_array((void **) &ctx->plan->parameters, &ctx->plan->parameter_capacity,
                           ctx->plan->parameter_count + 1, sizeof(*ctx->plan->parameters),
                           XR_SEMANTIC_MAX_PARAMETERS))
            return fail(ctx, "XR_EXEC_5003", "semantic parameter budget exhausted");
        XrSemanticParameterRecord *record = &ctx->plan->parameters[ctx->plan->parameter_count];
        memset(record, 0, sizeof(*record));
        uint32_t type_index;
        if (!source->params[p] || !add_type(ctx, source->params[p]->type, &type_index))
            return fail(ctx, "XR_SEM_0013", "semantic function has a missing parameter");
        XrTextBuilder key = {0};
        bool valid = text_append(&key, "parameter-v1:function=") &&
                     text_append_stable_id(&key, function->id) &&
                     text_append_format(&key, ":ordinal=%u:type=", p) &&
                     text_append_stable_id(&key, ctx->plan->types[type_index].id) &&
                     text_append_format(&key, ":mode=%u", source->params[p]->param_mode);
        if (valid)
            record->canonical_key = xr_semantic_plan_copy_string(ctx->plan, key.data);
        text_dispose(&key);
        if (!valid || !record->canonical_key ||
            !xr_stable_id_from_key(record->canonical_key, &record->id, &digest))
            return fail(ctx, "XR_EXEC_5003", "semantic parameter identity allocation failed");
        record->function = function_index_value;
        record->type = type_index;
        record->value = function->value_begin + source->params[p]->id;
        record->ordinal = p;
        record->mode = source->params[p]->param_mode;
        record->ownership = xi_arc_parameter_ownership(source, source->params[p]);
        record->transfer_mode = source->params[p]->transfer_mode;
        const XrProgramSemanticTypeRecord *program_parameter_type =
            ctx->program_closure && source->params[p]->psc_type_index != XI_PSC_ROW_NONE
                ? xr_program_semantic_closure_type(ctx->program_closure,
                                                   source->params[p]->psc_type_index)
                : NULL;
        bool program_leaf_value =
            program_parameter_type &&
            program_parameter_type->kind == XR_PROGRAM_SEMANTIC_TYPE_LEAF_VALUE_AGGREGATE;
        if (program_leaf_value) {
            record->ownership = XI_OWN_NONE;
            record->transfer_mode = XR_TRANSFER_SHARE;
        }
        record->flags =
            (uint8_t) ((p < source->min_params ? XR_SEM_PARAMETER_REQUIRED : 0u) |
                       (source->is_vararg && p == source->nparams ? XR_SEM_PARAMETER_VARIADIC
                                                                  : 0u) |
                       (source->receiver_borrowed && p == 0 ? XR_SEM_PARAMETER_RECEIVER_BORROWED
                                                            : 0u) |
                       (!program_leaf_value && (source->params[p]->lowering_flags &
                                                XI_LOWERING_FLAG_PARAM_READ_PLACE) != 0
                            ? XR_SEM_PARAMETER_READ_PLACE
                            : 0u));
        ctx->plan->parameter_count++;
    }
    return true;
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
        record->parent = ctx->functions[i].parent;
        record->semantic_effects = source->semantic_effects;
        record->capability_mask = source->requires_unsafe_at_call ? 1u : 0u;
        record->flags =
            (uint8_t) ((source->error_effect_nothrow ? XR_SEM_FUNCTION_NOTHROW : 0u) |
                       (source->contains_unsafe_op ? XR_SEM_FUNCTION_CONTAINS_UNSAFE : 0u) |
                       (source->entry_type == 2 ? XR_SEM_FUNCTION_GENERATOR : 0u) |
                       (source->is_extern ? XR_SEM_FUNCTION_EXTERN : 0u));
        if (!build_function_identity(ctx, i, source, record))
            return false;
        record->parameter_begin = ctx->plan->parameter_count;
        record->child_count = source->nchildren;
        record->capture_begin = ctx->plan->capture_count;
        record->block_begin = block_cursor;
        record->block_count = source->nblocks;
        record->value_begin = value_cursor;
        record->value_count = source->next_value_id;
        set_function_return_contract(source, record);
        ctx->functions[i].value_begin = value_cursor;
        ctx->functions[i].block_begin = block_cursor;
        if (!append_parameter_records(ctx, i, source, record))
            return false;
        if (UINT32_MAX - value_cursor < source->next_value_id ||
            UINT32_MAX - block_cursor < source->nblocks)
            return fail(ctx, "XR_EXEC_5003", "semantic function index space exhausted");
        value_cursor += source->next_value_id;
        block_cursor += source->nblocks;
    }
    ctx->plan->function_count = ctx->function_count;
    return true;
}

static bool build_capture_records(XrSemanticBuildContext *ctx) {
    for (uint32_t i = 0; i < ctx->function_count; i++) {
        const XiFunc *source = ctx->functions[i].source;
        XrSemanticFunctionRecord *function = &ctx->plan->functions[i];
        function->capture_begin = ctx->plan->capture_count;
        function->capture_count = source->ncaptures;
        if (source->ncaptures > 0 && function->parent == XR_SEMANTIC_INDEX_NONE)
            return fail(ctx, "XR_SEM_0018", "root function cannot capture a lexical value");
        for (uint16_t ordinal = 0; ordinal < source->ncaptures; ordinal++) {
            const XiCapture *capture = &source->captures[ordinal];
            if (!capture->type || capture->capture_kind > XI_CAPTURE_SHARED ||
                (capture->source != XI_CAPTURE_SRC_REG &&
                 capture->source != XI_CAPTURE_SRC_UPVAL) ||
                capture->storage_domain == XR_STORAGE_DOMAIN_UNKNOWN ||
                capture->value_capability == XR_SEM_VALUE_CAPABILITY_UNKNOWN ||
                !reserve_array((void **) &ctx->plan->captures, &ctx->plan->capture_capacity,
                               ctx->plan->capture_count + 1, sizeof(*ctx->plan->captures),
                               XR_SEMANTIC_MAX_CAPTURES))
                return fail(ctx, "XR_SEM_0018", "semantic capture contract is incomplete");
            uint32_t type_index;
            if (!add_type(ctx, capture->type, &type_index))
                return false;
            uint32_t source_type_index;
            if (capture->source == XI_CAPTURE_SRC_REG) {
                if (!capture->value || !capture->value->type ||
                    !add_type(ctx, capture->value->type, &source_type_index))
                    return fail(ctx, "XR_SEM_0018", "capture has no typed local source value");
            } else {
                const XrSemanticFunctionRecord *parent = &ctx->plan->functions[function->parent];
                if (capture->index >= parent->capture_count)
                    return fail(ctx, "XR_SEM_0018", "capture source link is out of range");
                source_type_index =
                    ctx->plan->captures[parent->capture_begin + capture->index].source_type;
            }
            XrSemanticCaptureRecord *record = &ctx->plan->captures[ctx->plan->capture_count];
            memset(record, 0, sizeof(*record));
            record->name =
                xr_semantic_plan_copy_string(ctx->plan, capture->name ? capture->name : "");
            XrTextBuilder key = {0};
            if (!record->name || !text_append(&key, "capture-v1:function=") ||
                !text_append_stable_id(&key, function->id) ||
                !text_append_format(&key, ":ordinal=%u:name=", ordinal) ||
                !text_append_component(&key, capture->name) ||
                !text_append_format(
                    &key, ":source=%u:index=%u:kind=%u:type=",
                    capture->source == XI_CAPTURE_SRC_REG ? XR_SEM_CAPTURE_LOCAL_VALUE
                                                          : XR_SEM_CAPTURE_PARENT_CAPTURE,
                    capture->source == XI_CAPTURE_SRC_REG && capture->value ? capture->value->id
                                                                            : capture->index,
                    capture->capture_kind) ||
                !text_append_stable_id(&key, ctx->plan->types[type_index].id) ||
                !text_append(&key, ":source-type=") ||
                !text_append_stable_id(&key, ctx->plan->types[source_type_index].id) ||
                !text_append_format(&key, ":domain=%u:capability=%u", capture->storage_domain,
                                    capture->value_capability)) {
                text_dispose(&key);
                return fail(ctx, "XR_EXEC_5003", "semantic capture key allocation failed");
            }
            record->canonical_key = xr_semantic_plan_copy_string(ctx->plan, key.data);
            text_dispose(&key);
            XrFingerprint digest;
            if (!record->canonical_key ||
                !xr_stable_id_from_key(record->canonical_key, &record->id, &digest))
                return fail(ctx, "XR_EXEC_5003", "semantic capture identity allocation failed");
            record->function = i;
            record->source_function = function->parent;
            record->source_value = XR_SEMANTIC_INDEX_NONE;
            record->source_capture = XR_SEMANTIC_INDEX_NONE;
            record->type = type_index;
            record->source_type = source_type_index;
            record->ordinal = ordinal;
            record->source_index = capture->source == XI_CAPTURE_SRC_REG && capture->value
                                       ? capture->value->id
                                       : capture->index;
            record->source = capture->source == XI_CAPTURE_SRC_REG ? XR_SEM_CAPTURE_LOCAL_VALUE
                                                                   : XR_SEM_CAPTURE_PARENT_CAPTURE;
            record->kind = capture->capture_kind;
            record->storage_domain = capture->storage_domain;
            record->value_capability = capture->value_capability;
            record->flags = (uint8_t) ((capture->needs_cell ? XR_SEM_CAPTURE_NEEDS_CELL : 0u) |
                                       (capture->is_mutable ? XR_SEM_CAPTURE_MUTABLE : 0u) |
                                       (capture->is_reassigned ? XR_SEM_CAPTURE_REASSIGNED : 0u));
            if (record->source == XR_SEM_CAPTURE_LOCAL_VALUE) {
                if (function->parent == XR_SEMANTIC_INDEX_NONE)
                    return fail(ctx, "XR_SEM_0018", "capture has no local source value");
                record->source_value =
                    ctx->functions[function->parent].value_begin + capture->value->id;
                const XrSemanticFunctionRecord *parent = &ctx->plan->functions[function->parent];
                if (record->source_value < parent->value_begin ||
                    record->source_value >= parent->value_begin + parent->value_count)
                    return fail(ctx, "XR_SEM_0018", "capture source value is out of range");
            } else {
                const XrSemanticFunctionRecord *parent = &ctx->plan->functions[function->parent];
                record->source_capture = parent->capture_begin + capture->index;
            }
            ctx->plan->capture_count++;
        }
    }
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
        case XI_AS:
        case XI_GET_BUILTIN:
            return add_metadata(ctx, record, (const char *) value->aux);
        case XI_CALL_BUILTIN:
            return value->array_intrinsic_kind != XI_ARRAY_INTRINSIC_NONE
                       ? true
                       : add_metadata(ctx, record, (const char *) value->aux);
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
            if (!import_ref)
                return true;
            const char *module_path = import_ref->module_path ? import_ref->module_path : "";
            if (classify_import_resolution(import_ref) == XR_SEM_IMPORT_RESOLUTION_SOURCE_MODULE &&
                import_ref->resolved_module && import_ref->resolved_module->identity) {
                module_path =
                    xr_semantic_plan_copy_string(ctx->plan, import_ref->resolved_module->identity);
                if (!module_path)
                    return fail(ctx, "XR_SEM_0019",
                                "source import module path identity is incomplete");
            }
            return add_metadata(ctx, record, module_path) &&
                   add_metadata(ctx, record,
                                import_ref->member_name ? import_ref->member_name : "");
        }
        case XI_SET_SHARED: {
            const XiFunc *owner = value->block ? value->block->func : NULL;
            const XiModule *module = owner ? owner->module : NULL;
            const XiModuleExport *match = NULL;
            for (uint16_t i = 0; module && owner == module->init && i < module->nexports; i++) {
                const XiModuleExport *candidate = &module->exports[i];
                bool function_export = candidate->function && !candidate->class_data;
                bool class_export = candidate->class_data && !candidate->function;
                if (candidate->shared_slot != value->aux_int || (!function_export && !class_export))
                    continue;
                if (match)
                    return fail(ctx, "XR_SEM_0019", "source export shared slot is ambiguous");
                match = candidate;
            }
            if (match) {
                bool ambiguous = false;
                const XiValue *store = indexed_root_shared_store(ctx, value->aux_int, &ambiguous);
                const XiValue *stored = store && store == value && !ambiguous &&
                                                store->nargs == 1 &&
                                                root_store_precedes_activation(owner, store)
                                            ? strip_identity_copies(owner, store->args[0])
                                            : NULL;
                int function = match->function && stored
                                   ? resolve_closure_binding(ctx, owner, store->args[0])
                                   : -1;
                bool exact_function =
                    function >= 0 && ctx->functions[function].source == match->function;
                uint32_t source_class = XR_SEMANTIC_INDEX_NONE;
                for (uint32_t c = 0; match->class_data && module && c < module->nclasses; c++) {
                    if (module->classes[c] != match->class_data)
                        continue;
                    if (source_class != XR_SEMANTIC_INDEX_NONE) {
                        source_class = XR_SEMANTIC_INDEX_NONE;
                        break;
                    }
                    source_class = c;
                }
                bool exact_class = match->class_data && stored && stored->op == XI_CLASS_CREATE &&
                                   stored->aux == match->class_data && module->slot_classes &&
                                   value->aux_int >= 0 && value->aux_int < module->nslots &&
                                   module->slot_classes[value->aux_int] == match->class_data &&
                                   xr_semantic_class_declaration_is_frozen(ctx->plan, source_class);
                if (!exact_function && !exact_class)
                    match = NULL;
            }
            return !match ||
                   (add_metadata(ctx, record,
                                 match->function ? "source-export-v1" : "source-class-export-v1") &&
                    add_metadata(ctx, record, match->name));
        }
        case XI_AGG_NEW: {
            const XiClassData *aggregate = value_aggregate_class_data(ctx, value->type);
            if (!aggregate || !aggregate->struct_layout ||
                aggregate->instance_field_count != aggregate->struct_layout->field_count ||
                (aggregate->instance_field_count != 0 && !aggregate->instance_field_names))
                return fail(ctx, "XR_SEM_0019", "value aggregate field identity is incomplete");
            for (uint16_t i = 0; i < aggregate->instance_field_count; i++) {
                const char *name = aggregate->instance_field_names[i];
                if (!name || !name[0] || !add_metadata(ctx, record, name))
                    return false;
            }
            return true;
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
    if (value->enum_metadata_kind != XR_ENUM_METADATA_NONE) {
        /* An enum metadata view -- `Color.variants` and its kin -- is a
         * constant the lowering already marked as such on the value, in the
         * enum_metadata_* fields rather than in aux_kind. What it carries is
         * the count the view spans; the descriptors themselves are read from
         * the owning enum's layout, which the plan states elsewhere. Without
         * this the constant fell to the type switch, where an enum metadata
         * type is an ordinary instance and has no case. */
        constant->kind = XR_SEM_CONST_ENUM_METADATA;
        constant->integer = value->aux_int;
        operation->constant = ctx->plan->constant_count++;
        return true;
    }
    if (value->aux_kind == XI_AUX_KIND_BIGINT_DIGITS) {
        /* An arbitrary-precision literal has no fixed-width home, so the plan
         * states it the way the front end wrote it: decimal digits. The mark
         * comes from the single place that builds these, which keeps this off
         * the type's spelling. */
        if (!value->aux)
            return fail(ctx, "XR_SEM_0009", "BigInt constant carries no digits");
        constant->kind = XR_SEM_CONST_BIGINT;
        constant->string = xr_semantic_plan_copy_string(ctx->plan, (const char *) value->aux);
        if (!constant->string)
            return fail(ctx, "XR_EXEC_5003", "semantic BigInt constant allocation failed");
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

static const XiValue *strip_identity_copies(const XiFunc *owner, const XiValue *value) {
    uint32_t depth = 0;
    while (value && xi_copy_is_identity_alias(value) && value->nargs == 1 && value->block &&
           value->block->func == owner && depth++ < XR_SEMANTIC_MAX_OPERATIONS)
        value = value->args[0];
    return depth < XR_SEMANTIC_MAX_OPERATIONS ? value : NULL;
}

static const XiValue *find_unique_shared_store(const XiFunc *owner, int64_t slot, bool *ambiguous) {
    const XiValue *found = NULL;
    *ambiguous = false;
    for (uint32_t block_index = 0; owner && block_index < owner->nblocks; block_index++) {
        const XiBlock *block = owner->blocks[block_index];
        for (uint32_t value_index = 0; block && value_index < block->nvalues; value_index++) {
            const XiValue *value = block->values[value_index];
            if (!value || value->op != XI_SET_SHARED || value->aux_int != slot)
                continue;
            if (found) {
                *ambiguous = true;
                return NULL;
            }
            found = value;
        }
    }
    return found;
}

static bool value_precedes_in_block(const XiBlock *block, const XiValue *before,
                                    const XiValue *after) {
    bool saw_before = false;
    for (uint32_t index = 0; block && index < block->nvalues; index++) {
        if (block->values[index] == before)
            saw_before = true;
        if (block->values[index] == after)
            return saw_before;
    }
    return false;
}

static bool block_dominates(const XiBlock *dominator, const XiBlock *block) {
    uint32_t limit = block && block->func ? block->func->nblocks : 0;
    uint32_t depth = 0;
    for (const XiBlock *cursor = block; cursor && depth++ <= limit; cursor = cursor->idom) {
        if (cursor == dominator)
            return true;
    }
    return false;
}

static bool value_can_cross_activation_boundary(const XiValue *value) {
    return value && (value->op == XI_CALL || value->op == XI_TAIL_CALL ||
                     value->op == XI_CALL_METHOD || value->op == XI_CALL_METHOD_DIRECT ||
                     value->op == XI_CALL_BUILTIN || value->op == XI_GO);
}

static bool root_store_precedes_activation(const XiFunc *owner, const XiValue *store) {
    if (!owner || !owner->entry || store->block != owner->entry)
        return false;
    for (uint32_t index = 0; index < owner->entry->nvalues; index++) {
        const XiValue *value = owner->entry->values[index];
        if (value == store)
            return true;
        if (value_can_cross_activation_boundary(value))
            return false;
    }
    return false;
}

static bool shared_store_initializes_load(const XrSemanticBuildContext *ctx, const XiFunc *caller,
                                          const XiFunc *owner, const XiValue *store,
                                          const XiValue *load) {
    if (!store || !load || !store->block || !load->block)
        return false;
    if (owner != caller)
        return owner->parent_func == NULL && function_index(ctx, owner) == 0 &&
               root_store_precedes_activation(owner, store);
    if (store->block == load->block)
        return value_precedes_in_block(store->block, store, load);
    return block_dominates(store->block, load->block);
}

/* A callable survives the forwarding ops unchanged: an identity copy, the
 * ownership forwards, and a successful CHECKTYPE all hand on the same closure
 * with at most a refined static type.  Naming a call target through them is
 * the same question the IR coroutine resolver already answers this way. */
static const XiValue *strip_callable_forwards(const XiFunc *owner, const XiValue *value) {
    uint32_t depth = 0;
    while (value && value->nargs >= 1 && value->block && value->block->func == owner &&
           depth++ < XR_SEMANTIC_MAX_OPERATIONS &&
           (xi_copy_is_identity_alias(value) || xi_op_is_identity_forward(value->op) ||
            value->op == XI_CHECKTYPE))
        value = value->args[0];
    return depth < XR_SEMANTIC_MAX_OPERATIONS ? value : NULL;
}

static int resolve_closure_binding(const XrSemanticBuildContext *ctx, const XiFunc *owner,
                                   const XiValue *value) {
    value = strip_callable_forwards(owner, value);
    if (!value || !value->block || value->block->func != owner || !value->aux ||
        (value->op != XI_CLOSURE_NEW &&
         !(value->op == XI_STACK_ALLOC && value->aux_int == XI_CLOSURE_NEW)))
        return -1;
    return function_index(ctx, (const XiFunc *) value->aux);
}

/* A binding that merely forwards another shared slot names the same function.
 * Following that chain is bounded by XR_SEMANTIC_MAX_SHARED_CALLEE_HOPS, and
 * an ambiguous or unresolved hop still fails closed. */
static int resolve_direct_local_callee_depth(const XrSemanticBuildContext *ctx,
                                             const XiFunc *caller, const XiValue *callee,
                                             unsigned depth) {
    callee = strip_callable_forwards(caller, callee);
    int direct = resolve_closure_binding(ctx, caller, callee);
    if (direct >= 0)
        return direct;
    if (!callee || !callee->block || callee->block->func != caller || callee->op != XI_GET_SHARED ||
        callee->aux_int < 0)
        return -1;
    for (const XiFunc *owner = caller; owner; owner = owner->parent_func) {
        bool ambiguous = false;
        const XiValue *store = find_unique_shared_store(owner, callee->aux_int, &ambiguous);
        if (ambiguous)
            return -1;
        if (!store)
            continue;
        if (store->nargs != 1 || !shared_store_initializes_load(ctx, caller, owner, store, callee))
            return -1;
        direct = resolve_closure_binding(ctx, owner, store->args[0]);
        if (direct >= 0 || depth >= XR_SEMANTIC_MAX_SHARED_CALLEE_HOPS)
            return direct;
        return resolve_direct_local_callee_depth(ctx, owner, store->args[0], depth + 1u);
    }
    return -1;
}

static int resolve_direct_local_callee(const XrSemanticBuildContext *ctx, const XiFunc *caller,
                                       const XiValue *callee) {
    return resolve_direct_local_callee_depth(ctx, caller, callee, 0u);
}

static bool program_direct_local_callee(XrSemanticBuildContext *ctx, uint32_t caller,
                                        const XiValue *call, uint32_t *callee) {
    if (callee)
        *callee = XR_SEMANTIC_INDEX_NONE;
    if (!ctx || !ctx->program_closure || !callee || caller >= ctx->function_count || !call ||
        call->op != XI_CALL || call->psc_call_index == XI_PSC_ROW_NONE)
        return fail(ctx, "XR_SEM_0019", "program call has no exact SemanticPlan input row");
    const XrProgramSemanticCallRecord *call_row =
        xr_program_semantic_closure_call(ctx->program_closure, call->psc_call_index);
    const XiFunc *caller_function = ctx->functions[caller].source;
    const XrProgramSemanticFunctionRecord *caller_row =
        caller_function && caller_function->psc_function_index != XI_PSC_ROW_NONE
            ? xr_program_semantic_closure_function(ctx->program_closure,
                                                   caller_function->psc_function_index)
            : NULL;
    if (!call_row || !caller_row ||
        !xr_stable_id_equal(call_row->caller_function, caller_row->id) ||
        (ctx->construction_decision &&
         (!xr_stable_id_equal(call_row->id, ctx->construction_decision->call_identity) ||
          !xr_stable_id_equal(call_row->callsite_identity,
                              ctx->construction_decision->callsite_identity) ||
          !xr_stable_id_equal(call_row->caller_function,
                              ctx->construction_decision->caller_function) ||
          !xr_stable_id_equal(call_row->callee_function,
                              ctx->construction_decision->callee_function))))
        return fail(ctx, "XR_SEM_0019", "program call row disagrees with its sealed decision");
    uint32_t match = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < ctx->function_count; i++) {
        const XiFunc *candidate = ctx->functions[i].source;
        const XrProgramSemanticFunctionRecord *candidate_row =
            candidate && candidate->psc_function_index != XI_PSC_ROW_NONE
                ? xr_program_semantic_closure_function(ctx->program_closure,
                                                       candidate->psc_function_index)
                : NULL;
        if (!candidate_row || !xr_stable_id_equal(candidate_row->id, call_row->callee_function))
            continue;
        if (match != XR_SEMANTIC_INDEX_NONE)
            return fail(ctx, "XR_SEM_0019", "program callee row is ambiguous in SemanticPlan");
        match = i;
    }
    if (match == XR_SEMANTIC_INDEX_NONE)
        return fail(ctx, "XR_SEM_0019", "program callee row is missing from SemanticPlan");
    *callee = match;
    return true;
}

static bool resolve_native_yieldable_callee(const XiFunc *caller, const XiValue *call,
                                            const char **module, const char **member) {
    if (!call || call->op != XI_CALL || call->nargs == 0)
        return false;
    const XiValue *callee = call->args[0];
    callee = strip_identity_copies(caller, callee);
    if (!callee || callee->op != XI_IMPORT_REF || !callee->aux)
        return false;
    const XiImportRef *ref = (const XiImportRef *) callee->aux;
    /* Same grounding question the IR coroutine analysis asks, asked through the
     * same predicate so the two layers cannot drift apart: a native identity is
     * only authority once import resolution has actually run over it. */
    if (!xi_import_ref_is_grounded_native(ref))
        return false;
    const XrStdlibDefEntry *binding =
        xr_stdlib_metadata_unique_func(ref->module_path, ref->member_name);
    if (!binding || !binding->signature || !binding->vm || !binding->vm_binding ||
        strcmp(binding->vm_binding, "yieldable") != 0 ||
        call->nargs != (uint16_t) (binding->argc + 1u))
        return false;
    *module = ref->module_path;
    *member = ref->member_name;
    return true;
}

static bool call_has_coroutine_state(const XiFunc *caller, const XiValue *call) {
    const XiCoroPlan *coro = caller ? caller->coro_plan : NULL;
    if (!coro || !call)
        return false;
    for (uint32_t state = 0; state < coro->nstates; state++)
        if (coro->points[state].op == call && coro->points[state].kind == XI_CORO_SUSP_CALL)
            return true;
    return false;
}

/* The three import-resolution questions are asked through the same shared
 * predicates the IR coroutine analysis asks, so the layers cannot drift on
 * which references carry call-target authority. */
static uint8_t classify_import_resolution(const XiImportRef *ref) {
    if (!ref || !ref->module_path || !ref->module_path[0])
        return XR_SEM_IMPORT_RESOLUTION_NONE;
    if (xi_import_ref_is_source_module(ref))
        return XR_SEM_IMPORT_RESOLUTION_SOURCE_MODULE;
    if (xi_import_ref_is_native_stdlib(ref))
        return XR_SEM_IMPORT_RESOLUTION_NATIVE_STDLIB;
    return XR_SEM_IMPORT_RESOLUTION_UNRESOLVED;
}

static bool prepare_root_shared_store_index(XrSemanticBuildContext *ctx, const XiFunc *root) {
    uint32_t module_slots = root && root->module ? root->module->nslots : 0;
    ctx->root_shared_store_count =
        root && root->nshared > module_slots ? root->nshared : module_slots;
    if (ctx->root_shared_store_count == 0)
        return true;
    ctx->root_shared_stores = (const XiValue **) xr_calloc(ctx->root_shared_store_count,
                                                           sizeof(*ctx->root_shared_stores));
    ctx->root_shared_store_ambiguous = (uint8_t *) xr_calloc(
        ctx->root_shared_store_count, sizeof(*ctx->root_shared_store_ambiguous));
    if (!ctx->root_shared_stores || !ctx->root_shared_store_ambiguous)
        return fail(ctx, "XR_EXEC_5003", "root shared-store index allocation failed");
    for (uint32_t block_index = 0; block_index < root->nblocks; block_index++) {
        const XiBlock *block = root->blocks[block_index];
        for (uint32_t value_index = 0; block && value_index < block->nvalues; value_index++) {
            const XiValue *value = block->values[value_index];
            if (!value || value->op != XI_SET_SHARED || value->aux_int < 0 ||
                (uint64_t) value->aux_int >= ctx->root_shared_store_count)
                continue;
            uint32_t slot = (uint32_t) value->aux_int;
            if (ctx->root_shared_stores[slot])
                ctx->root_shared_store_ambiguous[slot] = 1;
            else
                ctx->root_shared_stores[slot] = value;
        }
    }
    return true;
}

static const XiValue *indexed_root_shared_store(const XrSemanticBuildContext *ctx, int64_t slot,
                                                bool *ambiguous) {
    if (ambiguous)
        *ambiguous = false;
    if (!ctx || slot < 0 || (uint64_t) slot >= ctx->root_shared_store_count)
        return NULL;
    uint32_t index = (uint32_t) slot;
    if (ambiguous)
        *ambiguous = ctx->root_shared_store_ambiguous[index] != 0;
    return ctx->root_shared_stores[index];
}

static int compare_source_export(const void *left, const void *right) {
    const XrSemanticSourceExportRecord *a = (const XrSemanticSourceExportRecord *) left;
    const XrSemanticSourceExportRecord *b = (const XrSemanticSourceExportRecord *) right;
    int order = strcmp(a->name, b->name);
    return order != 0 ? order : xr_stable_id_compare(a->id, b->id);
}

static bool build_source_exports(XrSemanticBuildContext *ctx, const XiFunc *root) {
    const XiModule *module = root ? root->module : NULL;
    bool *export_slots =
        module && module->nslots ? (bool *) xr_calloc(module->nslots, sizeof(*export_slots)) : NULL;
    if (module && module->nslots && !export_slots)
        return fail(ctx, "XR_EXEC_5003", "source-export slot index allocation failed");
    for (uint16_t i = 0; module && i < module->nexports; i++) {
        const XiModuleExport *source_export = &module->exports[i];
        bool function_export = source_export->function != NULL && source_export->class_data == NULL;
        bool class_export = source_export->class_data != NULL && source_export->function == NULL;
        if (!function_export && !class_export)
            continue;
        if (source_export->shared_slot >= module->nslots ||
            export_slots[source_export->shared_slot]) {
            xr_free(export_slots);
            return fail(ctx, "XR_SEM_0019", "source-export shared slot is duplicated");
        }
        export_slots[source_export->shared_slot] = true;
        int function = function_export ? function_index(ctx, source_export->function) : -1;
        uint32_t source_class = XR_SEMANTIC_INDEX_NONE;
        for (uint32_t c = 0; class_export && c < module->nclasses; c++) {
            if (module->classes[c] != source_export->class_data)
                continue;
            if (source_class != XR_SEMANTIC_INDEX_NONE) {
                source_class = XR_SEMANTIC_INDEX_NONE;
                break;
            }
            source_class = c;
        }
        bool ambiguous = false;
        const XiValue *store =
            indexed_root_shared_store(ctx, source_export->shared_slot, &ambiguous);
        const XiValue *stored =
            store && store->nargs == 1 ? strip_identity_copies(root, store->args[0]) : NULL;
        bool exact_function =
            function_export && function >= 0 &&
            resolve_closure_binding(ctx, root, store ? store->args[0] : NULL) == function;
        bool exact_class =
            class_export && source_class < ctx->plan->source_class_count && stored &&
            stored->op == XI_CLASS_CREATE && stored->aux == source_export->class_data &&
            module->slot_classes && source_export->shared_slot < module->nslots &&
            module->slot_classes[source_export->shared_slot] == source_export->class_data &&
            xr_semantic_class_declaration_is_frozen(ctx->plan, source_class);
        if (!source_export->name || !source_export->name[0] || ambiguous || !store ||
            store->nargs != 1 || !root_store_precedes_activation(root, store) ||
            (!exact_function && !exact_class))
            continue;
        if (!reserve_array((void **) &ctx->plan->source_exports, &ctx->plan->source_export_capacity,
                           ctx->plan->source_export_count + 1, sizeof(*ctx->plan->source_exports),
                           XR_SEMANTIC_MAX_SOURCE_EXPORTS)) {
            xr_free(export_slots);
            return fail(ctx, "XR_EXEC_5003", "semantic source-export budget exhausted");
        }
        XrSemanticSourceExportRecord *record =
            &ctx->plan->source_exports[ctx->plan->source_export_count++];
        memset(record, 0, sizeof(*record));
        record->name = xr_semantic_plan_copy_string(ctx->plan, source_export->name);
        record->exported_entity = exact_function ? ctx->plan->functions[function].id
                                                 : ctx->plan->source_classes[source_class].id;
        record->function = exact_function ? (uint32_t) function : XR_SEMANTIC_INDEX_NONE;
        record->source_class = exact_class ? source_class : XR_SEMANTIC_INDEX_NONE;
        record->shared_slot = source_export->shared_slot;
        record->kind =
            exact_function ? XR_SEM_SOURCE_EXPORT_FUNCTION : XR_SEM_SOURCE_EXPORT_SOURCE_CLASS;
        XrTextBuilder key = {0};
        bool valid = record->name &&
                     text_append_format(&key,
                                        exact_function ? "source-export-v1:schema=%u:name="
                                                       : "source-class-export-v1:schema=%u:name=",
                                        XR_SEMANTIC_SCHEMA_VERSION) &&
                     text_append_component(&key, record->name) &&
                     text_append(&key, exact_function ? ":function=" : ":source-class=") &&
                     text_append_stable_id(
                         &key, exact_function ? ctx->plan->functions[function].id
                                              : ctx->plan->source_classes[source_class].id) &&
                     text_append_format(&key, ":slot=%u", record->shared_slot);
        if (valid)
            record->canonical_key = xr_semantic_plan_copy_string(ctx->plan, key.data);
        text_dispose(&key);
        XrFingerprint digest;
        if (!valid || !record->canonical_key ||
            !xr_stable_id_from_key(record->canonical_key, &record->id, &digest)) {
            xr_free(export_slots);
            return fail(ctx, "XR_SEM_0019", "source-export identity is incomplete");
        }
    }
    xr_free(export_slots);
    if (ctx->plan->source_export_count > 1)
        qsort(ctx->plan->source_exports, ctx->plan->source_export_count,
              sizeof(*ctx->plan->source_exports), compare_source_export);
    for (uint32_t i = 1; i < ctx->plan->source_export_count; i++)
        if (strcmp(ctx->plan->source_exports[i - 1].name, ctx->plan->source_exports[i].name) == 0)
            return fail(ctx, "XR_SEM_0019", "source-export name is duplicated");
    return true;
}

static const XrSemanticEntityRecord *plan_module_entity(const XrSemanticPlan *plan) {
    const XrSemanticEntityRecord *found = NULL;
    for (uint32_t i = 0; plan && i < plan->entity_count; i++) {
        const XrSemanticEntityRecord *entity = &plan->entities[i];
        if (entity->kind != XR_SEM_ENTITY_MODULE)
            continue;
        if (found)
            return NULL;
        found = entity;
    }
    return found;
}

static const XrSemanticSourceExportRecord *
find_source_export(const XrSemanticPlan *plan, const char *name, uint32_t *index_out) {
    const XrSemanticSourceExportRecord *found = NULL;
    for (uint32_t i = 0; plan && name && i < plan->source_export_count; i++) {
        const XrSemanticSourceExportRecord *candidate = &plan->source_exports[i];
        if (!candidate->name || strcmp(candidate->name, name) != 0)
            continue;
        if (found)
            return NULL;
        found = candidate;
        if (index_out)
            *index_out = i;
    }
    return found;
}

static const uint8_t *plan_suspendability(XrSemanticBuildContext *ctx, const XrSemanticPlan *plan) {
    for (uint32_t i = 0; i < ctx->suspendability_count; i++)
        if (ctx->suspendability[i].plan == plan)
            return ctx->suspendability[i].functions;
    if (!plan || !reserve_array((void **) &ctx->suspendability, &ctx->suspendability_capacity,
                                ctx->suspendability_count + 1, sizeof(*ctx->suspendability),
                                XR_SEMANTIC_MAX_FUNCTIONS))
        return NULL;
    uint8_t *flags = plan->function_count ? (uint8_t *) xr_calloc(plan->function_count, 1) : NULL;
    uint32_t *head = plan->function_count
                         ? (uint32_t *) xr_malloc((size_t) plan->function_count * sizeof(*head))
                         : NULL;
    uint32_t *next = plan->call_target_count
                         ? (uint32_t *) xr_malloc((size_t) plan->call_target_count * sizeof(*next))
                         : NULL;
    uint32_t *queue = plan->function_count
                          ? (uint32_t *) xr_malloc((size_t) plan->function_count * sizeof(*queue))
                          : NULL;
    if ((plan->function_count && (!flags || !head || !queue)) ||
        (plan->call_target_count && !next)) {
        xr_free(flags);
        xr_free(head);
        xr_free(next);
        xr_free(queue);
        return NULL;
    }
    for (uint32_t f = 0; f < plan->function_count; f++)
        head[f] = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < plan->call_target_count; i++)
        next[i] = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < plan->entity_count; i++) {
        const XrSemanticEntityRecord *entity = &plan->entities[i];
        if (entity->kind == XR_SEM_ENTITY_COROUTINE_STATE &&
            entity->subject < plan->operation_count)
            flags[plan->operations[entity->subject].function] = 1;
    }
    for (uint32_t i = 0; i < plan->call_target_count; i++) {
        const XrSemanticCallTargetRecord *target = &plan->call_targets[i];
        if (target->function < plan->function_count && target->operation < plan->operation_count &&
            ((target->kind == XR_SEM_CALL_TARGET_DIRECT_LOCAL &&
              (plan->operations[target->operation].opcode == XI_CALL ||
               plan->operations[target->operation].opcode == XI_TAIL_CALL)) ||
             (target->kind == XR_SEM_CALL_TARGET_SOURCE_INSTANCE_METHOD_LOCAL &&
              plan->operations[target->operation].opcode == XI_CALL_METHOD))) {
            next[i] = head[target->function];
            head[target->function] = i;
        }
    }
    uint32_t begin = 0, end = 0;
    for (uint32_t f = 0; f < plan->function_count; f++)
        if (flags[f])
            queue[end++] = f;
    while (begin < end) {
        uint32_t callee = queue[begin++];
        for (uint32_t edge = head[callee]; edge != XR_SEMANTIC_INDEX_NONE; edge = next[edge]) {
            uint32_t caller = plan->operations[plan->call_targets[edge].operation].function;
            if (!flags[caller]) {
                flags[caller] = 1;
                queue[end++] = caller;
            }
        }
    }
    xr_free(head);
    xr_free(next);
    xr_free(queue);
    XrSuspendabilityCacheEntry *entry = &ctx->suspendability[ctx->suspendability_count++];
    entry->plan = plan;
    entry->functions = flags;
    return flags;
}

static bool append_dependency(XrSemanticBuildContext *ctx, const XiModule *module,
                              uint32_t *dependency_out) {
    if (!module || !module->identity || !module->identity[0] || !module->init ||
        !module->init->semantic_plan || !xr_semantic_plan_is_verified(module->init->semantic_plan))
        return false;
    const XrSemanticPlan *dependency_plan = module->init->semantic_plan;
    const XrSemanticEntityRecord *module_entity = plan_module_entity(dependency_plan);
    if (!module_entity)
        return false;
    const char *module_path = xr_semantic_plan_copy_string(ctx->plan, module->identity);
    if (!module_path)
        return false;
    XrFingerprint fingerprint = xr_semantic_plan_fingerprint(dependency_plan);
    for (uint32_t i = 0; i < ctx->plan->dependency_count; i++) {
        XrSemanticDependencyRecord *record = &ctx->plan->dependencies[i];
        if (!xr_stable_id_equal(record->module, module_entity->id))
            continue;
        if (!xr_fingerprint_equal(record->semantic_fingerprint, fingerprint))
            return false;
        if (strcmp(record->module_path, module_path) != 0)
            return false;
        *dependency_out = i;
        return true;
    }
    if (!reserve_array((void **) &ctx->plan->dependencies, &ctx->plan->dependency_capacity,
                       ctx->plan->dependency_count + 1, sizeof(*ctx->plan->dependencies),
                       XR_SEMANTIC_MAX_FUNCTIONS) ||
        !reserve_array((void **) &ctx->plan->dependency_plans, &ctx->plan->dependency_plan_capacity,
                       ctx->plan->dependency_count + 1, sizeof(*ctx->plan->dependency_plans),
                       XR_SEMANTIC_MAX_FUNCTIONS))
        return fail(ctx, "XR_EXEC_5003", "semantic dependency budget exhausted");
    uint32_t index = ctx->plan->dependency_count++;
    XrSemanticDependencyRecord *record = &ctx->plan->dependencies[index];
    memset(record, 0, sizeof(*record));
    record->module_path = module_path;
    record->module = module_entity->id;
    record->semantic_fingerprint = fingerprint;
    XrTextBuilder key = {0};
    bool valid =
        record->module_path &&
        text_append_format(&key, "dependency-v1:schema=%u:path=", XR_SEMANTIC_SCHEMA_VERSION) &&
        text_append_component(&key, record->module_path) && text_append(&key, ":module=") &&
        text_append_stable_id(&key, record->module) && text_append(&key, ":semantic=");
    if (valid) {
        for (unsigned byte = 0; byte < XR_FINGERPRINT_BYTES; byte++)
            valid = text_append_format(&key, "%02x", fingerprint.bytes[byte]);
    }
    if (valid)
        record->canonical_key = xr_semantic_plan_copy_string(ctx->plan, key.data);
    text_dispose(&key);
    XrFingerprint digest;
    if (!valid || !record->canonical_key ||
        !xr_stable_id_from_key(record->canonical_key, &record->id, &digest))
        return fail(ctx, "XR_SEM_0019", "source dependency identity is incomplete");
    ctx->plan->dependency_plans[index] = xr_semantic_plan_retain(module->init->semantic_plan);
    ctx->plan->dependency_plan_count = ctx->plan->dependency_count;
    *dependency_out = index;
    return true;
}

/* A source namespace remains a module dependency even when no member is read
 * from the local alias.  Freeze that dependency while the resolved Xi module
 * identity is still available; later consumers must reconstruct the binding
 * from the frozen SOURCE_MODULE classification and this exact dependency row. */
static bool append_source_namespace_dependency(XrSemanticBuildContext *ctx, const XiValue *value) {
    if (!ctx || !value || value->op != XI_IMPORT_REF)
        return true;
    const XiImportRef *ref = (const XiImportRef *) value->aux;
    if (classify_import_resolution(ref) != XR_SEM_IMPORT_RESOLUTION_SOURCE_MODULE || !ref ||
        (ref->member_name && ref->member_name[0] != '\0'))
        return true;
    bool dependency_present = false;
    for (uint32_t i = 0; i < ctx->dependency_module_count; i++)
        dependency_present |= ctx->dependency_modules[i] == ref->resolved_module;
    if (!dependency_present)
        return true;
    uint32_t dependency = XR_SEMANTIC_INDEX_NONE;
    if (ref->resolved_func || ref->resolved_shared_slot != -1 || ref->resolved_export_slot != -1 ||
        !append_dependency(ctx, ref->resolved_module, &dependency))
        return fail(ctx, "XR_SEM_0019", "source namespace dependency identity is incomplete");
    return true;
}

static const XiImportRef *resolve_namespace_import_receiver(const XrSemanticBuildContext *ctx,
                                                            const XiFunc *caller,
                                                            const XiValue *receiver) {
    receiver = strip_identity_copies(caller, receiver);
    if (!receiver || !receiver->block || receiver->block->func != caller ||
        receiver->op != XI_GET_SHARED || receiver->aux_int < 0)
        return NULL;
    const XiFunc *root = caller;
    while (root->parent_func)
        root = root->parent_func;
    bool ambiguous = false;
    const XiValue *store = indexed_root_shared_store(ctx, receiver->aux_int, &ambiguous);
    if (ambiguous || !store || store->nargs != 1 || !root_store_precedes_activation(root, store))
        return NULL;
    const XiValue *source = strip_identity_copies(root, store->args[0]);
    if (!source || source->op != XI_IMPORT_REF || !source->aux)
        return NULL;
    const XiImportRef *ref = (const XiImportRef *) source->aux;
    if (!ref->module_path || (ref->member_name && ref->member_name[0] != '\0') ||
        !ref->resolved_module || ref->resolved_func || ref->resolved_shared_slot != -1 ||
        ref->resolved_export_slot != -1)
        return NULL;
    bool dependency_present = false;
    for (uint32_t i = 0; i < ctx->dependency_module_count; i++)
        dependency_present |= ctx->dependency_modules[i] == ref->resolved_module;
    return dependency_present ? ref : NULL;
}

/* Resolve a named source import while the module graph identity still exists.
 * A shared load is admitted only through the unique root initializer store;
 * the member string alone is never used as call authority. */
static const XiImportRef *resolve_source_import_callee(const XrSemanticBuildContext *ctx,
                                                       const XiFunc *caller,
                                                       const XiValue *callee) {
    callee = strip_identity_copies(caller, callee);
    const XiValue *source = callee;
    if (callee && callee->block && callee->block->func == caller && callee->op == XI_GET_SHARED &&
        callee->aux_int >= 0) {
        const XiFunc *root = caller;
        while (root->parent_func)
            root = root->parent_func;
        bool ambiguous = false;
        const XiValue *store = indexed_root_shared_store(ctx, callee->aux_int, &ambiguous);
        if (ambiguous || !store || store->nargs != 1 ||
            !root_store_precedes_activation(root, store))
            return NULL;
        source = strip_identity_copies(root, store->args[0]);
    }
    if (!source || source->op != XI_IMPORT_REF || !source->aux)
        return NULL;
    const XiImportRef *ref = (const XiImportRef *) source->aux;
    if (classify_import_resolution(ref) != XR_SEM_IMPORT_RESOLUTION_SOURCE_MODULE ||
        !ref->member_name || ref->member_name[0] == '\0' || !ref->resolved_func ||
        ref->resolved_shared_slot < 0 || ref->resolved_export_slot < 0)
        return NULL;
    bool dependency_present = false;
    for (uint32_t i = 0; i < ctx->dependency_module_count; i++)
        dependency_present |= ctx->dependency_modules[i] == ref->resolved_module;
    return dependency_present ? ref : NULL;
}

/* A named class import is not a source-function export: resolved_func is
 * intentionally NULL. Prove the resolver's shared/export slot, the module's
 * public class export and the dependency SemanticPlan class-export row as one
 * identity before the builder is allowed to freeze a constructor target. */
static const XiImportRef *resolve_source_imported_class_callee(
    const XrSemanticBuildContext *ctx, const XiFunc *caller, const XiValue *callee,
    const XrSemanticPlan **dependency_plan_out,
    const XrSemanticSourceExportRecord **source_export_out, uint32_t *source_export_index_out,
    uint32_t *source_class_out) {
    const XiImportRef *ref = xi_value_import_ref(caller, callee);
    if (!ctx || !ref || classify_import_resolution(ref) != XR_SEM_IMPORT_RESOLUTION_SOURCE_MODULE ||
        !ref->member_name || !ref->member_name[0] || !ref->resolved_module || ref->resolved_func ||
        ref->resolved_shared_slot < 0 || ref->resolved_export_slot < 0 ||
        ref->resolved_shared_slot >= ref->resolved_module->nslots ||
        !ref->resolved_module->slot_classes)
        return NULL;
    bool dependency_present = false;
    for (uint32_t i = 0; i < ctx->dependency_module_count; i++)
        dependency_present |= ctx->dependency_modules[i] == ref->resolved_module;
    const XiClassData *class_data = ref->resolved_module->slot_classes[ref->resolved_shared_slot];
    const XiModuleExport *module_export = NULL;
    for (uint16_t i = 0; dependency_present && class_data && i < ref->resolved_module->nexports;
         i++) {
        const XiModuleExport *candidate = &ref->resolved_module->exports[i];
        if (!candidate->name || strcmp(candidate->name, ref->member_name) != 0 ||
            candidate->shared_slot != ref->resolved_shared_slot ||
            candidate->class_data != class_data || candidate->function)
            continue;
        if (module_export)
            return NULL;
        module_export = candidate;
    }
    const XrSemanticPlan *dependency_plan =
        ref->resolved_module->init ? ref->resolved_module->init->semantic_plan : NULL;
    uint32_t source_export_index = XR_SEMANTIC_INDEX_NONE;
    const XrSemanticSourceExportRecord *source_export =
        find_source_export(dependency_plan, ref->member_name, &source_export_index);
    uint32_t source_class =
        xr_semantic_source_class_export_source_class(dependency_plan, source_export);
    const XrSemanticSourceClassRecord *semantic_class =
        xr_semantic_plan_source_class(dependency_plan, source_class);
    if (!dependency_present || !class_data || !module_export || !dependency_plan ||
        !xr_semantic_plan_is_verified(dependency_plan) || !source_export ||
        source_export->kind != XR_SEM_SOURCE_EXPORT_SOURCE_CLASS ||
        source_export->function != XR_SEMANTIC_INDEX_NONE ||
        source_export->source_class != source_class || !semantic_class ||
        !xr_stable_id_equal(source_export->exported_entity, semantic_class->id) ||
        source_class >= ref->resolved_module->nclasses ||
        ref->resolved_module->classes[source_class] != class_data || !class_data->class_name ||
        strcmp(class_data->class_name, semantic_class->name) != 0)
        return NULL;
    if (dependency_plan_out)
        *dependency_plan_out = dependency_plan;
    if (source_export_out)
        *source_export_out = source_export;
    if (source_export_index_out)
        *source_export_index_out = source_export_index;
    if (source_class_out)
        *source_class_out = source_class;
    return ref;
}

static const XiImportRef *
resolve_native_namespace_import_receiver(const XrSemanticBuildContext *ctx, const XiFunc *caller,
                                         const XiValue *receiver) {
    receiver = strip_identity_copies(caller, receiver);
    if (!receiver || !receiver->block || receiver->block->func != caller ||
        receiver->op != XI_GET_SHARED || receiver->aux_int < 0)
        return NULL;
    const XiFunc *root = caller;
    while (root->parent_func)
        root = root->parent_func;
    bool ambiguous = false;
    const XiValue *store = indexed_root_shared_store(ctx, receiver->aux_int, &ambiguous);
    if (ambiguous || !store || store->nargs != 1 || !root_store_precedes_activation(root, store))
        return NULL;
    const XiValue *source = strip_identity_copies(root, store->args[0]);
    if (!source || source->op != XI_IMPORT_REF || !source->aux)
        return NULL;
    const XiImportRef *ref = (const XiImportRef *) source->aux;
    return (!ref->member_name || ref->member_name[0] == '\0') &&
                   classify_import_resolution(ref) == XR_SEM_IMPORT_RESOLUTION_NATIVE_STDLIB
               ? ref
               : NULL;
}

static bool append_native_namespace_call_target(XrSemanticBuildContext *ctx, const XiValue *value,
                                                uint32_t operation) {
    if (!value || value->op != XI_CALL_METHOD || value->nargs == 0 || !value->aux ||
        (value->aux_int & 1) != 0)
        return true;
    const XrSemanticOperationRecord *call = &ctx->plan->operations[operation];
    const XiFunc *caller = ctx->functions[call->function].source;
    const XiImportRef *ref = resolve_native_namespace_import_receiver(ctx, caller, value->args[0]);
    const char *selector = (const char *) value->aux;
    const XrStdlibDefEntry *binding =
        ref ? xr_stdlib_metadata_unique_func(ref->module_path, selector) : NULL;
    if (!binding || !binding->signature || !binding->vm || !binding->vm_binding ||
        strcmp(binding->vm_binding, "yieldable") != 0 ||
        value->nargs != (uint16_t) (binding->argc + 1u))
        return true;
    if (ctx->plan->call_target_count >= XR_SEMANTIC_MAX_CALL_TARGETS ||
        !reserve_array((void **) &ctx->plan->call_targets, &ctx->plan->call_target_capacity,
                       ctx->plan->call_target_count + 1, sizeof(*ctx->plan->call_targets),
                       XR_SEMANTIC_MAX_CALL_TARGETS))
        return fail(ctx, "XR_EXEC_5003", "semantic call-target budget exhausted");
    XrSemanticCallTargetRecord *record = &ctx->plan->call_targets[ctx->plan->call_target_count++];
    memset(record, 0, sizeof(*record));
    record->operation = operation;
    record->function = XR_SEMANTIC_INDEX_NONE;
    record->dependency = XR_SEMANTIC_INDEX_NONE;
    record->source_export = XR_SEMANTIC_INDEX_NONE;
    record->callable_type = XR_SEMANTIC_INDEX_NONE;
    record->kind = XR_SEM_CALL_TARGET_NATIVE_NAMESPACE_YIELDABLE;
    XrTextBuilder key = {0};
    bool valid = text_append_format(
                     &key, "call-target-v5:schema=%u:operation=", XR_SEMANTIC_SCHEMA_VERSION) &&
                 text_append_stable_id(&key, ctx->plan->operations[operation].id) &&
                 text_append(&key, ":native-namespace=") && text_append(&key, ref->module_path) &&
                 text_append(&key, ".") && text_append(&key, selector) &&
                 text_append_format(&key, ":kind=%u", (unsigned) record->kind);
    if (valid)
        record->canonical_key = xr_semantic_plan_copy_string(ctx->plan, key.data);
    text_dispose(&key);
    XrFingerprint digest;
    if (!valid || !record->canonical_key ||
        !xr_stable_id_from_key(record->canonical_key, &record->id, &digest))
        return fail(ctx, "XR_EXEC_5003", "semantic native namespace identity allocation failed");
    return true;
}

/* Name the frozen builtin class whose method parks the caller, resolving the
 * live receiver type against the shared roster. A receiver that is not a frozen
 * builtin, or whose selector and arity name no suspending method, yields no
 * name and therefore no call target. */
static const char *builtin_instance_yieldable_name(const XrType *type, const char *selector,
                                                   uint16_t argument_count) {
    uint32_t builtin_type =
        xr_semantic_yieldable_builtin_id(type ? (unsigned) type->kind : (unsigned) XR_KIND_COUNT,
                                         xr_semantic_frozen_builtin_type(type));
    return xr_semantic_builtin_instance_yieldable(builtin_type, selector, argument_count)
               ? xr_semantic_yieldable_builtin_name(builtin_type)
               : NULL;
}

static bool append_builtin_instance_yieldable_call_target(XrSemanticBuildContext *ctx,
                                                          const XiValue *value,
                                                          uint32_t operation) {
    if (!value || value->op != XI_CALL_METHOD || value->nargs == 0 || !value->args[0] ||
        !value->args[0]->type || !value->aux || (value->aux_int & 1) != 0)
        return true;
    const XrSemanticOperationRecord *call = &ctx->plan->operations[operation];
    const char *selector = (const char *) value->aux;
    const char *builtin = builtin_instance_yieldable_name(value->args[0]->type, selector,
                                                          (uint16_t) (value->nargs - 1u));
    if (!builtin || call->metadata_count != 1 ||
        strcmp(ctx->plan->metadata[call->metadata_begin], selector) != 0 ||
        call->operand_count != value->nargs)
        return true;
    uint32_t receiver_type = ctx->plan->operands[call->operand_begin].type;
    if (receiver_type >= ctx->plan->type_count ||
        ctx->plan->operands[call->operand_begin].role != XR_SEM_OPERAND_RECEIVER)
        return true;
    if (ctx->plan->call_target_count >= XR_SEMANTIC_MAX_CALL_TARGETS ||
        !reserve_array((void **) &ctx->plan->call_targets, &ctx->plan->call_target_capacity,
                       ctx->plan->call_target_count + 1, sizeof(*ctx->plan->call_targets),
                       XR_SEMANTIC_MAX_CALL_TARGETS))
        return fail(ctx, "XR_EXEC_5003", "semantic call-target budget exhausted");
    XrSemanticCallTargetRecord *record = &ctx->plan->call_targets[ctx->plan->call_target_count++];
    memset(record, 0, sizeof(*record));
    record->operation = operation;
    record->function = XR_SEMANTIC_INDEX_NONE;
    record->dependency = XR_SEMANTIC_INDEX_NONE;
    record->source_export = XR_SEMANTIC_INDEX_NONE;
    record->callable_type = receiver_type;
    record->kind = XR_SEM_CALL_TARGET_BUILTIN_INSTANCE_YIELDABLE;
    XrTextBuilder key = {0};
    bool valid = text_append_format(
                     &key, "call-target-v6:schema=%u:operation=", XR_SEMANTIC_SCHEMA_VERSION) &&
                 text_append_stable_id(&key, call->id) && text_append(&key, ":builtin-instance=") &&
                 text_append(&key, builtin) && text_append(&key, ".") &&
                 text_append(&key, selector) && text_append(&key, ":type=") &&
                 text_append_stable_id(&key, ctx->plan->types[receiver_type].id) &&
                 text_append_format(&key, ":kind=%u", (unsigned) record->kind);
    if (valid)
        record->canonical_key = xr_semantic_plan_copy_string(ctx->plan, key.data);
    text_dispose(&key);
    XrFingerprint digest;
    if (!valid || !record->canonical_key ||
        !xr_stable_id_from_key(record->canonical_key, &record->id, &digest))
        return fail(ctx, "XR_EXEC_5003", "semantic builtin instance identity allocation failed");
    return true;
}

static int resolve_source_instance_method_local(const XrSemanticBuildContext *ctx,
                                                const XiValue *value, uint32_t operation,
                                                uint32_t *receiver_type_out,
                                                uint8_t *out_class_flags) {
    if (!value || value->op != XI_CALL_METHOD || value->nargs == 0 || !value->args[0] ||
        !value->aux || (value->aux_int & 1) != 0 || operation >= ctx->plan->operation_count)
        return -1;
    const XrSemanticOperationRecord *call = &ctx->plan->operations[operation];
    if (call->function >= ctx->function_count || call->operand_count != value->nargs ||
        call->metadata_count != 1 ||
        strcmp(ctx->plan->metadata[call->metadata_begin], (const char *) value->aux) != 0)
        return -1;
    const XrFunctionMapEntry *caller = &ctx->functions[call->function];
    const XrSemanticOperandRecord *receiver = &ctx->plan->operands[call->operand_begin];
    if (receiver->role != XR_SEM_OPERAND_RECEIVER || receiver->type >= ctx->plan->type_count)
        return -1;
    uint32_t source_class_index = ctx->plan->types[receiver->type].source_class;
    bool exact_self = caller->source_class != XR_SEMANTIC_INDEX_NONE &&
                      caller->source_kind == XR_SEM_SOURCE_FUNCTION_INSTANCE_METHOD &&
                      caller->source && caller->source->nparams > 0 &&
                      value->args[0] == caller->source->params[0];
    if (source_class_index == XR_SEMANTIC_INDEX_NONE && exact_self)
        source_class_index = caller->source_class;
    if (source_class_index >= ctx->plan->source_class_count)
        return -1;
    const XrSemanticSourceClassRecord *source_class =
        &ctx->plan->source_classes[source_class_index];
    /* A runtime type and a non-generic declaration are what make one body
     * nameable at all. Being final is a different question: it is what makes
     * binding that body safe without seeing the whole graph. Report it instead
     * of requiring it, so the caller can record an obligation for the layer
     * that does see the graph. */
    if (!xr_semantic_source_class_can_name_one_method(source_class->flags))
        return -1;
    if (out_class_flags)
        *out_class_flags = source_class->flags;
    int match = -1;
    for (uint32_t f = 0; f < ctx->function_count; f++) {
        const XrFunctionMapEntry *candidate = &ctx->functions[f];
        if (candidate->source_class != source_class_index ||
            candidate->source_kind != XR_SEM_SOURCE_FUNCTION_INSTANCE_METHOD ||
            !candidate->source || !candidate->source->name ||
            strcmp(candidate->source->name, (const char *) value->aux) != 0 ||
            xi_func_semantic_param_count(candidate->source) != value->nargs)
            continue;
        if (match >= 0)
            return -1;
        match = (int) f;
    }
    if (match >= 0)
        *receiver_type_out = receiver->type;
    return match;
}

static bool append_source_instance_method_local_call_target(XrSemanticBuildContext *ctx,
                                                            const XiValue *value,
                                                            uint32_t operation) {
    uint32_t receiver_type = XR_SEMANTIC_INDEX_NONE;
    uint8_t class_flags = 0;
    int function =
        resolve_source_instance_method_local(ctx, value, operation, &receiver_type, &class_flags);
    if (function < 0)
        return true;
    if (ctx->plan->call_target_count >= XR_SEMANTIC_MAX_CALL_TARGETS ||
        !reserve_array((void **) &ctx->plan->call_targets, &ctx->plan->call_target_capacity,
                       ctx->plan->call_target_count + 1, sizeof(*ctx->plan->call_targets),
                       XR_SEMANTIC_MAX_CALL_TARGETS))
        return fail(ctx, "XR_EXEC_5003", "semantic call-target budget exhausted");
    const XrSemanticOperationRecord *call = &ctx->plan->operations[operation];
    uint32_t source_class = ctx->plan->types[receiver_type].source_class;
    if (source_class == XR_SEMANTIC_INDEX_NONE)
        source_class = ctx->functions[call->function].source_class;
    XrSemanticCallTargetRecord *record = &ctx->plan->call_targets[ctx->plan->call_target_count++];
    memset(record, 0, sizeof(*record));
    record->operation = operation;
    record->function = (uint32_t) function;
    record->dependency = XR_SEMANTIC_INDEX_NONE;
    record->source_export = XR_SEMANTIC_INDEX_NONE;
    record->callee_function = ctx->plan->functions[function].id;
    record->callable_type = receiver_type;
    /* Final says no subclass can exist anywhere, so the binding stands on its
     * own. Without it the binding holds only if the final graph carries no
     * override, which this module cannot know -- the row states that
     * obligation and the graph-holding layer discharges it. */
    record->kind = xr_semantic_source_instance_method_call_kind(class_flags);
    XrTextBuilder key = {0};
    bool valid =
        text_append_format(&key,
                           "call-target-v7:schema=%u:operation=", XR_SEMANTIC_SCHEMA_VERSION) &&
        text_append_stable_id(&key, call->id) && text_append(&key, ":source-class=") &&
        text_append_stable_id(&key, ctx->plan->source_classes[source_class].id) &&
        text_append(&key, ":selector=") && text_append_component(&key, (const char *) value->aux) &&
        text_append(&key, ":function=") && text_append_stable_id(&key, record->callee_function) &&
        text_append(&key, ":type=") &&
        text_append_stable_id(&key, ctx->plan->types[receiver_type].id) &&
        text_append_format(&key, ":kind=%u", (unsigned) record->kind);
    if (valid)
        record->canonical_key = xr_semantic_plan_copy_string(ctx->plan, key.data);
    text_dispose(&key);
    XrFingerprint digest;
    if (!valid || !record->canonical_key ||
        !xr_stable_id_from_key(record->canonical_key, &record->id, &digest))
        return fail(ctx, "XR_SEM_0019", "source instance method call identity is incomplete");
    return true;
}

static bool append_source_instance_method_open_call_target(XrSemanticBuildContext *ctx,
                                                           const XiValue *value,
                                                           uint32_t operation) {
    if (!value || value->op != XI_CALL_METHOD || value->nargs == 0 || !value->aux ||
        (value->aux_int & 1) != 0 || operation >= ctx->plan->operation_count)
        return true;
    const XrSemanticOperationRecord *call = &ctx->plan->operations[operation];
    if (call->metadata_count != 1 || call->operand_count != value->nargs ||
        strcmp(ctx->plan->metadata[call->metadata_begin], (const char *) value->aux) != 0)
        return true;
    const XrSemanticOperandRecord *receiver = &ctx->plan->operands[call->operand_begin];
    if (receiver->role != XR_SEM_OPERAND_RECEIVER || receiver->type >= ctx->plan->type_count)
        return true;
    const XrSemanticTypeRecord *type = &ctx->plan->types[receiver->type];
    XrStableId zero = {{0}};
    if (type->source_class != XR_SEMANTIC_INDEX_NONE ||
        xr_stable_id_equal(type->source_class_identity, zero))
        return true;
    const XiModule *match_module = NULL;
    const XrSemanticSourceClassRecord *match_class = NULL;
    const XrSemanticSourceMethodRecord *match_method = NULL;
    for (uint32_t m = 0; m < ctx->dependency_module_count; m++) {
        const XiModule *module = ctx->dependency_modules[m];
        const XrSemanticPlan *plan = module && module->init ? module->init->semantic_plan : NULL;
        if (!plan || plan->schema != XR_SEMANTIC_SCHEMA_VERSION || !plan->frozen || !plan->verified)
            continue;
        const uint8_t *suspendable = plan_suspendability(ctx, plan);
        if (!suspendable)
            continue;
        for (uint32_t sm = 0; sm < plan->source_method_count; sm++) {
            const XrSemanticSourceMethodRecord *candidate = &plan->source_methods[sm];
            const XrSemanticSourceClassRecord *source_class =
                candidate->source_class < plan->source_class_count
                    ? &plan->source_classes[candidate->source_class]
                    : NULL;
            uint8_t class_required = XR_SEM_SOURCE_CLASS_RUNTIME_TYPE;
            uint8_t method_required =
                XR_SEM_SOURCE_METHOD_INSTANCE | XR_SEM_SOURCE_METHOD_OPEN_DOMAIN;
            if (!source_class ||
                !xr_stable_id_equal(source_class->id, type->source_class_identity) ||
                (source_class->flags & class_required) != class_required ||
                (source_class->flags &
                 (XR_SEM_SOURCE_CLASS_EXPLICIT_FINAL | XR_SEM_SOURCE_CLASS_GENERIC)) != 0 ||
                (candidate->flags & method_required) != method_required ||
                candidate->parameter_count != call->operand_count ||
                candidate->function >= plan->function_count || !suspendable[candidate->function] ||
                strcmp(candidate->name, (const char *) value->aux) != 0)
                continue;
            if (match_method)
                return true;
            match_module = module;
            match_class = source_class;
            match_method = candidate;
        }
    }
    if (!match_method || !match_module || !match_class)
        return true;
    uint32_t dependency = XR_SEMANTIC_INDEX_NONE;
    if (!append_dependency(ctx, match_module, &dependency))
        return fail(ctx, "XR_SEM_0019", "open source method dependency is incomplete");
    if (!reserve_array((void **) &ctx->plan->call_targets, &ctx->plan->call_target_capacity,
                       ctx->plan->call_target_count + 1, sizeof(*ctx->plan->call_targets),
                       XR_SEMANTIC_MAX_CALL_TARGETS))
        return fail(ctx, "XR_EXEC_5003", "semantic call-target budget exhausted");
    XrSemanticCallTargetRecord *record = &ctx->plan->call_targets[ctx->plan->call_target_count++];
    memset(record, 0, sizeof(*record));
    record->operation = operation;
    record->function = XR_SEMANTIC_INDEX_NONE;
    record->dependency = dependency;
    record->source_export = XR_SEMANTIC_INDEX_NONE;
    record->export_identity = match_method->id;
    record->callable_type = receiver->type;
    record->kind = XR_SEM_CALL_TARGET_SOURCE_INSTANCE_METHOD_OPEN;
    XrTextBuilder key = {0};
    bool valid =
        text_append_format(&key,
                           "call-target-v8:schema=%u:operation=", XR_SEMANTIC_SCHEMA_VERSION) &&
        text_append_stable_id(&key, call->id) && text_append(&key, ":dependency=") &&
        text_append_stable_id(&key, ctx->plan->dependencies[dependency].id) &&
        text_append(&key, ":source-class=") && text_append_stable_id(&key, match_class->id) &&
        text_append(&key, ":source-method=") && text_append_stable_id(&key, match_method->id) &&
        text_append(&key, ":type=") && text_append_stable_id(&key, type->id) &&
        text_append_format(&key, ":kind=%u", (unsigned) record->kind);
    if (valid)
        record->canonical_key = xr_semantic_plan_copy_string(ctx->plan, key.data);
    text_dispose(&key);
    XrFingerprint digest;
    if (!valid || !record->canonical_key ||
        !xr_stable_id_from_key(record->canonical_key, &record->id, &digest))
        return fail(ctx, "XR_SEM_0019", "open source method call identity is incomplete");
    return true;
}

static bool append_source_export_call_target(XrSemanticBuildContext *ctx, const XiValue *value,
                                             uint32_t operation) {
    if (!value || (value->op != XI_CALL && value->op != XI_CALL_METHOD) || value->nargs == 0 ||
        (value->op == XI_CALL_METHOD && (!value->aux || (value->aux_int & 1) != 0)))
        return true;
    const XrSemanticOperationRecord *call = &ctx->plan->operations[operation];
    const XiFunc *caller = ctx->functions[call->function].source;
    const XiImportRef *ref = value->op == XI_CALL_METHOD
                                 ? resolve_namespace_import_receiver(ctx, caller, value->args[0])
                                 : resolve_source_import_callee(ctx, caller, value->args[0]);
    if (!ref)
        return true;
    const char *selector =
        value->op == XI_CALL_METHOD ? (const char *) value->aux : ref->member_name;
    const XrSemanticPlan *dependency_plan =
        ref->resolved_module->init ? ref->resolved_module->init->semantic_plan : NULL;
    uint32_t source_export = XR_SEMANTIC_INDEX_NONE;
    const XrSemanticSourceExportRecord *exported =
        find_source_export(dependency_plan, selector, &source_export);
    const XrSemanticFunctionRecord *callee =
        exported ? xr_semantic_plan_function(dependency_plan, exported->function) : NULL;
    const uint8_t *suspendable = plan_suspendability(ctx, dependency_plan);
    if (!exported || exported->kind != XR_SEM_SOURCE_EXPORT_FUNCTION || !callee ||
        !xr_stable_id_equal(exported->exported_entity, callee->id) || !suspendable ||
        callee->parameter_count == UINT16_MAX ||
        value->nargs != (uint16_t) (callee->parameter_count + 1u) ||
        (value->op == XI_CALL_METHOD &&
         (call->metadata_count != 1 ||
          strcmp(ctx->plan->metadata[call->metadata_begin], exported->name) != 0)) ||
        (value->op == XI_CALL && call->metadata_count != 0))
        return true;
    uint32_t dependency = XR_SEMANTIC_INDEX_NONE;
    if (!append_dependency(ctx, ref->resolved_module, &dependency))
        return fail(ctx, "XR_SEM_0019", "source-export dependency is incomplete");
    if (ctx->plan->call_target_count >= XR_SEMANTIC_MAX_CALL_TARGETS ||
        !reserve_array((void **) &ctx->plan->call_targets, &ctx->plan->call_target_capacity,
                       ctx->plan->call_target_count + 1, sizeof(*ctx->plan->call_targets),
                       XR_SEMANTIC_MAX_CALL_TARGETS))
        return fail(ctx, "XR_EXEC_5003", "semantic call-target budget exhausted");
    XrSemanticCallTargetRecord *record = &ctx->plan->call_targets[ctx->plan->call_target_count++];
    memset(record, 0, sizeof(*record));
    record->operation = operation;
    record->function = XR_SEMANTIC_INDEX_NONE;
    record->dependency = dependency;
    record->source_export = source_export;
    record->export_identity = exported->id;
    record->callee_function = callee->id;
    record->callable_type = XR_SEMANTIC_INDEX_NONE;
    record->kind = XR_SEM_CALL_TARGET_SOURCE_EXPORT;
    XrTextBuilder key = {0};
    bool valid = text_append_format(
                     &key, "call-target-v4:schema=%u:operation=", XR_SEMANTIC_SCHEMA_VERSION) &&
                 text_append_stable_id(&key, call->id) && text_append(&key, ":dependency=") &&
                 text_append_stable_id(&key, ctx->plan->dependencies[dependency].id) &&
                 text_append(&key, ":export=") && text_append_stable_id(&key, exported->id) &&
                 text_append(&key, ":function=") && text_append_stable_id(&key, callee->id) &&
                 text_append_format(&key, ":kind=%u", (unsigned) record->kind);
    if (valid)
        record->canonical_key = xr_semantic_plan_copy_string(ctx->plan, key.data);
    text_dispose(&key);
    XrFingerprint digest;
    if (!valid || !record->canonical_key ||
        !xr_stable_id_from_key(record->canonical_key, &record->id, &digest))
        return fail(ctx, "XR_SEM_0019", "source-export call identity is incomplete");
    return true;
}

/* Names the construction of a declared class. A local construction is proved
 * through its own class-object shared slot. An imported construction instead
 * freezes the exact dependency class export and constructor body; treating its
 * import binding as a local class slot would erase the module boundary. */
static bool append_source_class_constructor_call_target(XrSemanticBuildContext *ctx,
                                                        const XiValue *value, uint32_t operation) {
    const XrSemanticOperationRecord *call = &ctx->plan->operations[operation];
    uint32_t source_class = xr_semantic_class_construction_source_class(ctx->plan, call);
    uint32_t dependency = XR_SEMANTIC_INDEX_NONE;
    uint32_t source_export = XR_SEMANTIC_INDEX_NONE;
    uint32_t imported_source_class = XR_SEMANTIC_INDEX_NONE;
    uint32_t constructor = XR_SEMANTIC_INDEX_NONE;
    const XrSemanticPlan *dependency_plan = NULL;
    const XrSemanticSourceExportRecord *exported = NULL;
    const XiFunc *caller =
        call->function < ctx->plan->function_count ? ctx->functions[call->function].source : NULL;
    const XiImportRef *import_ref = NULL;
    if (source_class == XR_SEMANTIC_INDEX_NONE && value && value->nargs > 0 &&
        xi_value_is_constructor_call(value)) {
        import_ref =
            resolve_source_imported_class_callee(ctx, caller, value->args[0], &dependency_plan,
                                                 &exported, &source_export, &imported_source_class);
        XrSemanticDependencyRecord preview = {
            .module_path = import_ref ? import_ref->resolved_module->identity : NULL,
        };
        if (import_ref && xr_semantic_imported_class_construction_source_class(
                              ctx->plan, dependency_plan, &preview, exported, call, &constructor) !=
                              imported_source_class)
            import_ref = NULL;
        if (import_ref && !append_dependency(ctx, import_ref->resolved_module, &dependency))
            return fail(ctx, "XR_SEM_0019", "imported source class dependency is incomplete");
        if (import_ref && xr_semantic_imported_class_construction_source_class(
                              ctx->plan, dependency_plan, &ctx->plan->dependencies[dependency],
                              exported, call, &constructor) != imported_source_class)
            import_ref = NULL;
    }
    bool imported = import_ref != NULL;
    if (source_class == XR_SEMANTIC_INDEX_NONE && !imported)
        return true;
    if (ctx->plan->call_target_count >= XR_SEMANTIC_MAX_CALL_TARGETS ||
        !reserve_array((void **) &ctx->plan->call_targets, &ctx->plan->call_target_capacity,
                       ctx->plan->call_target_count + 1, sizeof(*ctx->plan->call_targets),
                       XR_SEMANTIC_MAX_CALL_TARGETS))
        return fail(ctx, "XR_EXEC_5003", "semantic call-target budget exhausted");
    XrSemanticCallTargetRecord *record = &ctx->plan->call_targets[ctx->plan->call_target_count++];
    memset(record, 0, sizeof(*record));
    record->operation = operation;
    record->function = XR_SEMANTIC_INDEX_NONE;
    record->dependency = imported ? dependency : XR_SEMANTIC_INDEX_NONE;
    record->source_export = imported ? source_export : XR_SEMANTIC_INDEX_NONE;
    if (imported) {
        record->export_identity = exported->id;
        if (constructor != XR_SEMANTIC_INDEX_NONE)
            record->callee_function = dependency_plan->functions[constructor].id;
    }
    record->callable_type = call->result_type;
    record->kind = XR_SEM_CALL_TARGET_SOURCE_CLASS_CONSTRUCTOR;
    XrTextBuilder key = {0};
    bool valid = false;
    if (!imported) {
        valid = text_append_format(
                    &key, "call-target-v9:schema=%u:operation=", XR_SEMANTIC_SCHEMA_VERSION) &&
                text_append_stable_id(&key, call->id) && text_append(&key, ":source-class=") &&
                text_append_stable_id(&key, ctx->plan->source_classes[source_class].id) &&
                text_append(&key, ":type=") &&
                text_append_stable_id(&key, ctx->plan->types[record->callable_type].id) &&
                text_append_format(&key, ":kind=%u", (unsigned) record->kind);
    } else {
        XrStableId zero = {{0}};
        valid = text_append_format(
                    &key, "call-target-v10:schema=%u:operation=", XR_SEMANTIC_SCHEMA_VERSION) &&
                text_append_stable_id(&key, call->id) && text_append(&key, ":dependency=") &&
                text_append_stable_id(&key, ctx->plan->dependencies[dependency].id) &&
                text_append(&key, ":class-export=") && text_append_stable_id(&key, exported->id) &&
                text_append(&key, ":source-class=") &&
                text_append_stable_id(&key,
                                      dependency_plan->source_classes[imported_source_class].id) &&
                text_append(&key, ":constructor=") &&
                (xr_stable_id_equal(record->callee_function, zero)
                     ? text_append(&key, "none")
                     : text_append_stable_id(&key, record->callee_function)) &&
                text_append(&key, ":type=") &&
                text_append_stable_id(&key, ctx->plan->types[record->callable_type].id) &&
                text_append_format(&key, ":kind=%u", (unsigned) record->kind);
    }
    if (valid)
        record->canonical_key = xr_semantic_plan_copy_string(ctx->plan, key.data);
    text_dispose(&key);
    XrFingerprint digest;
    if (!valid || !record->canonical_key ||
        !xr_stable_id_from_key(record->canonical_key, &record->id, &digest))
        return fail(ctx, "XR_SEM_0019", "source class construction identity is incomplete");
    return true;
}

static bool append_call_target(XrSemanticBuildContext *ctx, const XiValue *value,
                               uint32_t operation) {
    if (!value || operation >= ctx->plan->operation_count || value->nargs == 0)
        return true;
    if (value->op == XI_CALL_METHOD) {
        uint32_t before = ctx->plan->call_target_count;
        if (!append_source_export_call_target(ctx, value, operation))
            return false;
        if (ctx->plan->call_target_count != before)
            return true;
        if (!append_native_namespace_call_target(ctx, value, operation))
            return false;
        if (ctx->plan->call_target_count != before)
            return true;
        if (!append_builtin_instance_yieldable_call_target(ctx, value, operation))
            return false;
        if (ctx->plan->call_target_count != before)
            return true;
        if (!append_source_instance_method_local_call_target(ctx, value, operation))
            return false;
        return ctx->plan->call_target_count != before
                   ? true
                   : append_source_instance_method_open_call_target(ctx, value, operation);
    }
    if (value->op != XI_CALL && value->op != XI_TAIL_CALL)
        return true;
    uint32_t before = ctx->plan->call_target_count;
    uint32_t caller = ctx->plan->operations[operation].function;
    bool program_bound = ctx->program_closure != NULL && value->psc_call_index != XI_PSC_ROW_NONE;
    if (!program_bound) {
        if (!append_source_export_call_target(ctx, value, operation))
            return false;
        if (ctx->plan->call_target_count != before)
            return true;
    } else if (value->op != XI_CALL) {
        return fail(ctx, "XR_SEM_0019", "program row binds a non-direct call operation");
    }
    /* A call on its own activation carries no callee value to resolve: the
     * shared judgement reads the whole target out of the immediate, and the
     * function it names is the caller.  Everything past this point is the
     * ordinary direct-local row, so a self-call is admitted on exactly the
     * signature, argument and result terms any other local call must meet. */
    uint32_t program_function = XR_SEMANTIC_INDEX_NONE;
    if (program_bound && !program_direct_local_callee(ctx, caller, value, &program_function))
        return false;
    int function = program_bound ? (int) program_function
                                 : (xi_call_targets_own_frame(value->op, value->aux_int)
                                        ? (int) caller
                                        : resolve_direct_local_callee(
                                              ctx, ctx->functions[caller].source, value->args[0]));
    const char *native_module = NULL;
    const char *native_member = NULL;
    bool native_yieldable = !program_bound && function < 0 &&
                            resolve_native_yieldable_callee(ctx->functions[caller].source, value,
                                                            &native_module, &native_member);
    const XiValue *indirect_callee = program_bound ? NULL : value->args[0];
    while (!program_bound && indirect_callee && xi_copy_is_identity_alias(indirect_callee) &&
           indirect_callee->nargs == 1)
        indirect_callee = indirect_callee->args[0];
    bool indirect_callable =
        !program_bound && function < 0 && !native_yieldable && value->op == XI_CALL &&
        value->args[0] && value->args[0]->type && value->args[0]->type->kind == XR_KIND_FUNCTION &&
        indirect_callee && indirect_callee->op != XI_IMPORT_REF &&
        indirect_callee->op != XI_GET_BUILTIN &&
        (indirect_callee->op != XI_GET_SHARED ||
         call_has_coroutine_state(ctx->functions[caller].source, value)) &&
        indirect_callee->op != XI_CLOSURE_NEW &&
        !(indirect_callee->op == XI_STACK_ALLOC && indirect_callee->aux_int == XI_CLOSURE_NEW);
    if (!program_bound && function < 0 && !native_yieldable && !indirect_callable)
        return append_source_class_constructor_call_target(ctx, value, operation);
    if (ctx->plan->call_target_count >= XR_SEMANTIC_MAX_CALL_TARGETS ||
        !reserve_array((void **) &ctx->plan->call_targets, &ctx->plan->call_target_capacity,
                       ctx->plan->call_target_count + 1, sizeof(*ctx->plan->call_targets),
                       XR_SEMANTIC_MAX_CALL_TARGETS))
        return fail(ctx, "XR_EXEC_5003", "semantic call-target budget exhausted");
    XrSemanticCallTargetRecord *record = &ctx->plan->call_targets[ctx->plan->call_target_count++];
    memset(record, 0, sizeof(*record));
    record->operation = operation;
    record->function = function >= 0 ? (uint32_t) function : XR_SEMANTIC_INDEX_NONE;
    record->dependency = XR_SEMANTIC_INDEX_NONE;
    record->source_export = XR_SEMANTIC_INDEX_NONE;
    record->callable_type = XR_SEMANTIC_INDEX_NONE;
    record->kind = function >= 0      ? XR_SEM_CALL_TARGET_DIRECT_LOCAL
                   : native_yieldable ? XR_SEM_CALL_TARGET_NATIVE_YIELDABLE
                                      : XR_SEM_CALL_TARGET_INDIRECT_CALLABLE;
    if (indirect_callable) {
        const XrSemanticOperationRecord *call = &ctx->plan->operations[operation];
        record->callable_type = ctx->plan->operands[call->operand_begin].type;
    }
    XrTextBuilder key = {0};
    bool valid = text_append_format(
                     &key, "call-target-v3:schema=%u:operation=", XR_SEMANTIC_SCHEMA_VERSION) &&
                 text_append_stable_id(&key, ctx->plan->operations[operation].id);
    if (valid && function >= 0)
        valid = text_append(&key, ":function=") &&
                text_append_stable_id(&key, ctx->plan->functions[function].id);
    if (valid && native_yieldable)
        valid = text_append(&key, ":native=") && text_append(&key, native_module) &&
                text_append(&key, ".") && text_append(&key, native_member);
    if (valid && indirect_callable)
        valid = text_append(&key, ":callable-type=") &&
                text_append_stable_id(&key, ctx->plan->types[record->callable_type].id);
    valid = valid && text_append_format(&key, ":kind=%u", (unsigned) record->kind);
    if (valid)
        record->canonical_key = xr_semantic_plan_copy_string(ctx->plan, key.data);
    text_dispose(&key);
    XrFingerprint digest;
    if (!valid || !record->canonical_key ||
        !xr_stable_id_from_key(record->canonical_key, &record->id, &digest))
        return fail(ctx, "XR_EXEC_5003", "semantic call-target identity allocation failed");
    return true;
}

static bool xi_type_names_string_builder(const XrType *type) {
    return xr_type_is_named_class(type, "StringBuilder");
}

static bool xi_string_runes_exact(const XiValue *value) {
    const XiValue *receiver = value && value->nargs == 1 ? value->args[0] : NULL;
    const XrType *receiver_type = receiver ? receiver->type : NULL;
    return value && value->op == XI_CALL_METHOD && receiver && value->aux &&
           strcmp((const char *) value->aux, "runes") == 0 && value->aux_kind == XI_AUX_KIND_NONE &&
           value->aux_int > 0 && (value->aux_int & 1) == 0 && receiver_type &&
           receiver_type->kind == XR_KIND_STRING;
}

static bool xi_iterator_rune_has_next_exact(const XiValue *value) {
    const XiValue *receiver = value && value->nargs == 1 ? value->args[0] : NULL;
    const XrType *receiver_type = receiver ? receiver->type : NULL;
    const XrType *element = receiver_type && receiver_type->kind == XR_KIND_INSTANCE &&
                                    receiver_type->instance.type_arg_count == 1
                                ? receiver_type->instance.type_args[0]
                                : NULL;
    return value && value->op == XI_CALL_METHOD && receiver && value->aux &&
           strcmp((const char *) value->aux, "hasNext") == 0 &&
           value->aux_kind == XI_AUX_KIND_NONE && value->aux_int > 0 && (value->aux_int & 1) == 0 &&
           receiver_type && xr_type_is_named_class(receiver_type, "Iterator") && element &&
           element->kind == XR_KIND_RUNE && value->type && value->type->kind == XR_KIND_BOOL;
}

static bool xi_iterator_rune_next_exact(const XiValue *value) {
    const XiValue *receiver = value && value->nargs == 1 ? value->args[0] : NULL;
    const XrType *receiver_type = receiver ? receiver->type : NULL;
    const XrType *element = receiver_type && receiver_type->kind == XR_KIND_INSTANCE &&
                                    receiver_type->instance.type_arg_count == 1
                                ? receiver_type->instance.type_args[0]
                                : NULL;
    return value && value->op == XI_CALL_METHOD && receiver && value->aux &&
           strcmp((const char *) value->aux, "next") == 0 && value->aux_kind == XI_AUX_KIND_NONE &&
           value->aux_int > 0 && (value->aux_int & 1) == 0 && receiver_type &&
           xr_type_is_named_class(receiver_type, "Iterator") && element &&
           element->kind == XR_KIND_RUNE && value->type && value->type->kind == XR_KIND_RUNE &&
           xi_string_runes_exact(receiver);
}

/* `s.runes().nth(i)` -- the same receiver as `next`, with an index argument.
 * Unlike `next` it does not advance the iterator, so it is a pure projection
 * of the string by rune index, which is what makes it answerable at all. The
 * index is required to be a plain int value; nothing else about it is assumed
 * here, and the target layer decides how it is held. */
static bool xi_iterator_rune_nth_exact(const XiValue *value) {
    const XiValue *receiver = value && value->nargs == 2 ? value->args[0] : NULL;
    const XiValue *index = value && value->nargs == 2 ? value->args[1] : NULL;
    const XrType *receiver_type = receiver ? receiver->type : NULL;
    const XrType *element = receiver_type && receiver_type->kind == XR_KIND_INSTANCE &&
                                    receiver_type->instance.type_arg_count == 1
                                ? receiver_type->instance.type_args[0]
                                : NULL;
    return value && value->op == XI_CALL_METHOD && receiver && index && value->aux &&
           strcmp((const char *) value->aux, "nth") == 0 && value->aux_kind == XI_AUX_KIND_NONE &&
           value->aux_int > 0 && (value->aux_int & 1) == 0 && receiver_type &&
           xr_type_is_named_class(receiver_type, "Iterator") && element &&
           element->kind == XR_KIND_RUNE && value->type && value->type->kind == XR_KIND_RUNE &&
           index->type && index->type->kind == XR_KIND_INT && xi_string_runes_exact(receiver);
}

/* A rune obtained from a `String.runes()` iterator, by either spelling: `next`
 * steps through and `nth` projects by index, but both hand back exactly the
 * same thing, and every consumer of such a rune accepts either. Stated once so
 * adding the second spelling did not mean revisiting each consumer. */
static bool xi_iterator_rune_source_exact(const XiValue *value) {
    return xi_iterator_rune_next_exact(value) || xi_iterator_rune_nth_exact(value);
}

static bool xi_rune_to_uint32_exact(const XiValue *value) {
    const XiValue *receiver = value && value->nargs == 1 ? value->args[0] : NULL;
    return value && value->op == XI_CALL_METHOD && receiver &&
           value->aux_kind == XI_AUX_KIND_NONE &&
           value->aux_int == (int64_t) XI_METHOD_SYMBOL_TO_UINT32 << 1 && receiver->type &&
           receiver->type->kind == XR_KIND_RUNE && value->type &&
           value->type->kind == XR_KIND_INT && !value->type->is_nullable &&
           value->type->scalar_rep == XR_NATIVE_U32;
}

/* `r.toString()` on a rune from a `String.runes()` iterator: the one-rune
 * string. Sits beside toUInt32 -- same receiver family, different result. */
static bool xi_rune_to_string_exact(const XiValue *value) {
    const XiValue *receiver = value && value->nargs == 1 ? value->args[0] : NULL;
    return value && value->op == XI_CALL_METHOD && receiver && value->aux &&
           strcmp((const char *) value->aux, "toString") == 0 &&
           value->aux_kind == XI_AUX_KIND_NONE &&
           value->aux_int == (int64_t) XI_METHOD_SYMBOL_TOSTRING << 1 && receiver->type &&
           receiver->type->kind == XR_KIND_RUNE && value->type &&
           value->type->kind == XR_KIND_STRING && !value->type->is_nullable &&
           xi_iterator_rune_source_exact(receiver);
}

static bool xi_rune_is_whitespace_exact(const XiValue *value) {
    const XiValue *receiver = value && value->nargs == 1 ? value->args[0] : NULL;
    return value && value->op == XI_CALL_METHOD && receiver && value->aux &&
           strcmp((const char *) value->aux, "isWhitespace") == 0 &&
           value->aux_kind == XI_AUX_KIND_NONE && value->aux_int > 0 && (value->aux_int & 1) == 0 &&
           receiver->type && receiver->type->kind == XR_KIND_RUNE && value->type &&
           value->type->kind == XR_KIND_BOOL && xi_iterator_rune_source_exact(receiver);
}

static bool xi_string_slice_range_exact(const XiValue *value) {
    const XiValue *receiver = value && value->nargs == 3 ? value->args[0] : NULL;
    const XiValue *start = value && value->nargs == 3 ? value->args[1] : NULL;
    const XiValue *end = value && value->nargs == 3 ? value->args[2] : NULL;
    return value && value->op == XI_CALL_METHOD && receiver && start && end && value->aux &&
           strcmp((const char *) value->aux, "slice") == 0 && value->aux_kind == XI_AUX_KIND_NONE &&
           value->aux_int > 0 && (value->aux_int & 1) == 0 && value->type && receiver->type &&
           value->type == receiver->type && value->type->kind == XR_KIND_STRING &&
           !value->type->is_nullable && start->type && end->type &&
           start->type->kind == XR_KIND_INT && end->type->kind == XR_KIND_INT &&
           !start->type->is_nullable && !end->type->is_nullable &&
           start->type->scalar_rep == XR_NATIVE_I64 && end->type->scalar_rep == XR_NATIVE_I64;
}

static bool xi_string_builder_constructor_candidate(const XiValue *value) {
    if (!value || value->op != XI_CALL_BUILTIN)
        return false;
    bool named_builtin = value->aux && strcmp((const char *) value->aux, "StringBuilder") == 0;
    return named_builtin || xi_type_names_string_builder(value->type);
}

static bool xi_string_builder_constructor_exact(const XiValue *value) {
    const XrType *type = value ? value->type : NULL;
    return value && value->op == XI_CALL_BUILTIN && value->nargs == 0 && value->aux &&
           strcmp((const char *) value->aux, "StringBuilder") == 0 &&
           value->aux_kind == XI_AUX_KIND_NONE && value->aux_int == 0 &&
           value->flags == xi_generated_op_default_flags(XI_CALL_BUILTIN) &&
           value->transfer_mode == XR_TRANSFER_SHARE && value->param_mode == XR_PARAM_READ &&
           xr_type_is_builtin_named_class(type, "StringBuilder") && type->semantic_type_id == 0 &&
           type->instance.type_arg_count == 0 && !type->is_nullable && !type->is_const &&
           !type->is_value_type && !type->is_literal && !type->is_cycle_candidate &&
           !type->ptr_is_mut && type->scalar_rep == XR_SCALAR_REP_NONE && !type->alias_name;
}

static bool semantic_string_builder_type_exact(const XrSemanticTypeRecord *type) {
    char expected[160];
    int length = snprintf(expected, sizeof(expected),
                          "type-v3:%u:0:%u:0:0:0:0:0:0:%u:0:;named:13:StringBuilder[0]",
                          (unsigned) XR_KIND_INSTANCE, (unsigned) XR_TID_STRINGBUILDER,
                          (unsigned) XR_SCALAR_REP_NONE);
    return type && length > 0 && (size_t) length < sizeof(expected) &&
           type->kind == XR_KIND_INSTANCE && type->builtin_type == XR_TID_STRINGBUILDER &&
           type->child_count == 0 && type->aggregate_extent == 0 && type->aggregate_align == 0 &&
           type->scalar_rep == XR_SCALAR_REP_NONE &&
           type->flags == (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT) &&
           type->canonical_key && strcmp(type->canonical_key, expected) == 0;
}

static bool xi_string_builder_to_string_exact(const XiValue *value) {
    return value && value->op == XI_CALL_METHOD && value->nargs == 1 && value->args[0] &&
           value->aux && strcmp((const char *) value->aux, "toString") == 0 &&
           value->aux_kind == XI_AUX_KIND_NONE && value->aux_int > 0 && (value->aux_int & 1) == 0 &&
           xr_type_is_builtin_named_class(value->args[0]->type, "StringBuilder") && value->type &&
           value->type->kind == XR_KIND_STRING;
}

static bool
semantic_string_builder_append_common_exact(const XrSemanticBuildContext *ctx,
                                            const XrSemanticOperationRecord *record,
                                            const XrSemanticTypeRecord **out_argument_type) {
    if (out_argument_type)
        *out_argument_type = NULL;
    if (!ctx || !record || record->operand_count != 2 ||
        record->operand_begin > ctx->plan->operand_count ||
        record->operand_count > ctx->plan->operand_count - record->operand_begin ||
        record->metadata_count != 1 || record->metadata_begin >= ctx->plan->metadata_count)
        return false;
    const XrSemanticOperandRecord *receiver = &ctx->plan->operands[record->operand_begin];
    const XrSemanticOperandRecord *argument = receiver + 1;
    const XrSemanticTypeRecord *receiver_type =
        receiver->type < ctx->plan->type_count ? &ctx->plan->types[receiver->type] : NULL;
    const XrSemanticTypeRecord *argument_type =
        argument->type < ctx->plan->type_count ? &ctx->plan->types[argument->type] : NULL;
    const XrSemanticFunctionRecord *function = record->function < ctx->plan->function_count
                                                   ? &ctx->plan->functions[record->function]
                                                   : NULL;
    bool result_exact =
        (record->result_ownership == XI_GEN_RESULT_OWNERSHIP_OWNED &&
         record->return_parameter == -1 &&
         ((record->return_provenance == XR_SEM_RETURN_OWNED && record->return_complete == 1) ||
          (record->return_provenance == XR_SEM_RETURN_NONE && record->return_complete == 0))) ||
        (record->result_ownership == XI_GEN_RESULT_OWNERSHIP_BORROWED &&
             (((record->return_provenance == XR_SEM_RETURN_BORROWED_STATIC &&
                record->return_parameter == -1) ||
               (record->return_provenance == XR_SEM_RETURN_BORROWED_PARAM && function &&
                record->return_parameter >= 0 &&
                (uint16_t) record->return_parameter < function->parameter_count)) &&
              record->return_complete == 1) ||
         (record->return_provenance == XR_SEM_RETURN_NONE && record->return_parameter == -1 &&
          record->return_complete == 0));
    bool exact =
        semantic_string_builder_type_exact(receiver_type) && argument_type && result_exact &&
        record->opcode == XI_CALL_METHOD && record->result_type == receiver->type &&
        record->semantic_immediate == (int64_t) XI_METHOD_SYMBOL_APPEND << 1 &&
        record->evidence[1] == XA_INTRINSIC_STRING_BUILDER_APPEND &&
        strcmp(ctx->plan->metadata[record->metadata_begin],
               XA_INTRINSIC_STRING_BUILDER_APPEND_SOURCE_MEMBER) == 0 &&
        record->auxiliary_kind == XI_AUX_KIND_NONE && record->constant == XR_SEMANTIC_INDEX_NONE &&
        record->callable_function == XR_SEMANTIC_INDEX_NONE &&
        record->import_resolution == XR_SEM_IMPORT_RESOLUTION_NONE &&
        record->effects == xi_generated_op_effects(XI_CALL_METHOD) &&
        record->ownership_use == xi_generated_op_own_use(XI_CALL_METHOD) &&
        record->flags == xi_generated_op_default_flags(XI_CALL_METHOD) &&
        receiver->role == XR_SEM_OPERAND_RECEIVER && receiver->parameter == -1 &&
        receiver->flags == XR_SEM_OPERAND_CALL_CONTRACT &&
        argument->role == XR_SEM_OPERAND_ARGUMENT && argument->parameter == 0 &&
        argument->flags == XR_SEM_OPERAND_CALL_CONTRACT && record->result_alias_operand == 0;
    if (exact && out_argument_type)
        *out_argument_type = argument_type;
    return exact;
}

static bool semantic_string_builder_append_rune_exact(const XrSemanticBuildContext *ctx,
                                                      const XrSemanticOperationRecord *record) {
    const XrSemanticTypeRecord *argument_type = NULL;
    return semantic_string_builder_append_common_exact(ctx, record, &argument_type) &&
           argument_type->kind == XR_KIND_RUNE && argument_type->builtin_type == XR_TID_NULL &&
           argument_type->child_count == 0 && argument_type->scalar_rep == XR_SCALAR_REP_NONE &&
           argument_type->flags == 0;
}

static bool semantic_string_builder_to_string_exact(const XrSemanticBuildContext *ctx,
                                                    const XrSemanticOperationRecord *record) {
    if (!ctx || !record || record->operand_count != 1 ||
        record->operand_begin >= ctx->plan->operand_count || record->metadata_count != 1 ||
        record->metadata_begin >= ctx->plan->metadata_count)
        return false;
    const XrSemanticOperandRecord *receiver = &ctx->plan->operands[record->operand_begin];
    const XrSemanticTypeRecord *receiver_type =
        receiver->type < ctx->plan->type_count ? &ctx->plan->types[receiver->type] : NULL;
    const XrSemanticTypeRecord *result_type =
        record->result_type < ctx->plan->type_count ? &ctx->plan->types[record->result_type] : NULL;
    return semantic_string_builder_type_exact(receiver_type) && result_type &&
           result_type->kind == XR_KIND_STRING && result_type->builtin_type == XR_TID_NULL &&
           result_type->child_count == 0 && result_type->scalar_rep == XR_SCALAR_REP_NONE &&
           result_type->flags == (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT) &&
           strcmp(ctx->plan->metadata[record->metadata_begin], "toString") == 0 &&
           receiver->role == XR_SEM_OPERAND_RECEIVER && receiver->parameter == -1 &&
           receiver->flags == XR_SEM_OPERAND_CALL_CONTRACT && record->result_alias_operand == -1 &&
           record->result_ownership == XI_GEN_RESULT_OWNERSHIP_OWNED;
}

static bool semantic_string_builder_append_string_exact(const XrSemanticBuildContext *ctx,
                                                        const XrSemanticOperationRecord *record) {
    const XrSemanticTypeRecord *argument_type = NULL;
    return semantic_string_builder_append_common_exact(ctx, record, &argument_type) &&
           argument_type->kind == XR_KIND_STRING && argument_type->builtin_type == XR_TID_NULL &&
           argument_type->child_count == 0 && argument_type->scalar_rep == XR_SCALAR_REP_NONE &&
           argument_type->flags == (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT);
}

/* `Array<T>` is a compiler-owned container: the array kind is produced only by
 * the builtin container type, never by a declaration, and the language admits
 * no way to declare a member on it.  A receiver carrying the array kind plus
 * one of the frozen selectors below therefore names exactly one implementation,
 * so a user class that happens to declare the same name never reaches this
 * authority: its receiver record carries the instance kind and its own class
 * authority.
 *
 * Each row states reference access, ownership action, and eventual drop
 * lifecycle. Existing-element readers and movers preserve ownership. The one
 * reference-capable store consumes an exact local source-class instance into
 * tagged Array storage, whose canonical lifecycle releases it on erase or
 * container destruction.
 *
 * Each row states the whole shape one selector may present: the operand count
 * range, which operand carries the element (0 means the member takes none,
 * because operand 0 is always the receiver), and what the result is.  A member
 * that hands back its receiver states RESULT_RECEIVER: its result is the
 * receiver's own reference rather than a new value, so the row claims no
 * storage for it and every use of that result stays without authority. */
static bool xi_array_member_scalar_exact(const XiValue *value) {
    const XiValue *receiver = value && value->nargs >= 1 ? value->args[0] : NULL;
    const XrType *receiver_type = receiver ? receiver->type : NULL;
    const XrArrayMemberShape *shape =
        value && value->op == XI_CALL_METHOD && value->aux && value->aux_kind == XI_AUX_KIND_NONE
            ? xr_array_member_shape((const char *) value->aux, value->nargs)
            : NULL;
    /* The tail bit records where the call sits, not what it is: lowering sets it
     * on any `return <method call>`, so demanding the bare default refused the
     * very same member for its surroundings. `return out.join("|")` in the
     * stdlib probe is exactly that shape. */
    if (!shape || !receiver || !receiver_type || value->aux_int <= 0 ||
        (value->flags & XI_FLAG_MAY_SUSPEND) != 0 || receiver_type->kind != XR_KIND_ARRAY ||
        !receiver_type->container.element_type || !value->type)
        return false;
    for (uint16_t i = 1; i < value->nargs; i++) {
        if (!value->args[i] || !value->args[i]->type)
            return false;
    }
    switch (shape->result_shape) {
        case XR_ARRAY_MEMBER_RESULT_UNIT:
            return value->type->kind == XR_KIND_UNIT;
        case XR_ARRAY_MEMBER_RESULT_INT:
            return value->type->kind == XR_KIND_INT;
        case XR_ARRAY_MEMBER_RESULT_BOOL:
            return value->type->kind == XR_KIND_BOOL;
        case XR_ARRAY_MEMBER_RESULT_STRING:
            /* The default arm below is the receiver-returning one, which would
             * demand the result be the array itself. */
            return value->type->kind == XR_KIND_STRING && !value->type->is_nullable;
        default:
            return value->type == receiver_type;
    }
}

/* Array.reserve is admitted by the analyzer's stable intrinsic id. The
 * frozen operation then proves the complete receiver/capacity/result shape;
 * no selector or live Xi type participates in the classification. */
static bool semantic_array_reserve_exact(const XrSemanticBuildContext *ctx,
                                         const XrSemanticOperationRecord *record) {
    if (!ctx || !record || record->evidence[1] != XA_INTRINSIC_ARRAY_RESERVE ||
        record->opcode != XI_CALL_BUILTIN || record->operand_count != 2 ||
        record->operand_begin > ctx->plan->operand_count ||
        record->operand_count > ctx->plan->operand_count - record->operand_begin ||
        record->metadata_count != 0 || record->auxiliary_kind != XI_AUX_KIND_NONE ||
        record->semantic_immediate != 0 ||
        record->effects != xi_generated_op_effects(XI_CALL_BUILTIN))
        return false;
    const XrSemanticOperandRecord *receiver = &ctx->plan->operands[record->operand_begin];
    const XrSemanticOperandRecord *capacity = receiver + 1;
    const XrSemanticTypeRecord *receiver_type =
        receiver->type < ctx->plan->type_count ? &ctx->plan->types[receiver->type] : NULL;
    const XrSemanticTypeRecord *capacity_type =
        capacity->type < ctx->plan->type_count ? &ctx->plan->types[capacity->type] : NULL;
    return xr_semantic_array_type_row_is_exact(receiver_type) &&
           xr_semantic_array_member_i64_type_is_exact(capacity_type) &&
           record->result_type == receiver->type && record->result_alias_operand == 0 &&
           record->result_ownership == XI_GEN_RESULT_OWNERSHIP_OWNED &&
           record->return_provenance == XR_SEM_RETURN_OWNED && record->return_parameter == -1 &&
           record->return_complete == 1 && receiver->role == XR_SEM_OPERAND_ARGUMENT &&
           receiver->parameter == 0 && receiver->flags == XR_SEM_OPERAND_CALL_CONTRACT &&
           receiver->ownership_action == XR_SEM_OPERAND_BORROW &&
           capacity->role == XR_SEM_OPERAND_ARGUMENT && capacity->parameter == 1 &&
           capacity->flags == XR_SEM_OPERAND_CALL_CONTRACT &&
           capacity->ownership_action == XR_SEM_OPERAND_CONSUME;
}

static uint8_t semantic_array_hof_kind_from_xi(uint8_t kind) {
    switch (kind) {
        case XI_ARRAY_HOF_MAP:
            return XR_SEM_ARRAY_HOF_MAP;
        case XI_ARRAY_HOF_FILTER:
            return XR_SEM_ARRAY_HOF_FILTER;
        case XI_ARRAY_HOF_REDUCE:
            return XR_SEM_ARRAY_HOF_REDUCE;
        default:
            return XR_SEM_ARRAY_HOF_NONE;
    }
}

/* The lowering discriminant is the only dispatch identity admitted here. The
 * selector stays debug metadata. This proof binds the callback directly to a
 * noncapturing child of the caller and freezes both sides of its scalar
 * signature before Xi is discarded. */
static bool semantic_array_hof_exact(const XrSemanticBuildContext *ctx, const XiFunc *owner,
                                     const XiValue *value, const XrSemanticOperationRecord *record,
                                     uint32_t *callback_function) {
    uint8_t kind = semantic_array_hof_kind_from_xi(value ? value->array_hof_kind : 0);
    uint16_t expected_operands = kind == XR_SEM_ARRAY_HOF_REDUCE ? 3u : 2u;
    if (!ctx || !owner || !value || !record || kind == XR_SEM_ARRAY_HOF_NONE ||
        value->op != XI_CALL_METHOD || value->nargs != expected_operands ||
        record->operand_count != expected_operands ||
        record->operand_begin > ctx->plan->operand_count ||
        record->operand_count > ctx->plan->operand_count - record->operand_begin ||
        record->metadata_count != 1 || record->metadata_begin >= ctx->plan->metadata_count ||
        value->array_element_storage <= XR_ELEM_ANY ||
        value->array_element_storage >= XR_ELEM_RAWPTR ||
        value->array_result_element_storage <= XR_ELEM_ANY ||
        value->array_result_element_storage >= XR_ELEM_RAWPTR || !value->args[0] || !value->args[1])
        return false;
    int callback = resolve_closure_binding(ctx, owner, value->args[1]);
    if (callback < 0 || (uint32_t) callback >= ctx->plan->function_count)
        return false;
    const XrSemanticFunctionRecord *callee = &ctx->plan->functions[callback];
    uint32_t owner_index = record->function;
    uint16_t minimum_parameters = kind == XR_SEM_ARRAY_HOF_REDUCE ? 2u : 1u;
    if (callee->parent != owner_index || callee->capture_count != 0 ||
        callee->parameter_count != minimum_parameters ||
        (callee->semantic_effects & (XI_EFFECT_SIDE_EFFECT | XI_EFFECT_MEMORY_WRITE |
                                     XI_EFFECT_MAY_THROW | XI_EFFECT_MAY_SUSPEND)) != 0 ||
        callee->parameter_begin > ctx->plan->parameter_count ||
        callee->parameter_count > ctx->plan->parameter_count - callee->parameter_begin)
        return false;
    const XrSemanticOperandRecord *operands = &ctx->plan->operands[record->operand_begin];
    const XrSemanticTypeRecord *receiver_type =
        operands[0].type < ctx->plan->type_count ? &ctx->plan->types[operands[0].type] : NULL;
    if (!xr_semantic_array_type_row_is_exact(receiver_type) ||
        receiver_type->child_begin >= ctx->plan->type_child_count)
        return false;
    uint32_t receiver_element = ctx->plan->type_children[receiver_type->child_begin];
    const XrSemanticTypeRecord *receiver_element_type =
        receiver_element < ctx->plan->type_count ? &ctx->plan->types[receiver_element] : NULL;
    if (!receiver_element_type ||
        xr_semantic_array_element_storage(receiver_element_type) != value->array_element_storage)
        return false;
    uint32_t result_element = record->result_type;
    if (kind != XR_SEM_ARRAY_HOF_REDUCE) {
        const XrSemanticTypeRecord *result_array = record->result_type < ctx->plan->type_count
                                                       ? &ctx->plan->types[record->result_type]
                                                       : NULL;
        if (!xr_semantic_array_type_row_is_exact(result_array) ||
            result_array->child_begin >= ctx->plan->type_child_count)
            return false;
        result_element = ctx->plan->type_children[result_array->child_begin];
        if (kind == XR_SEM_ARRAY_HOF_FILTER &&
            (record->result_type != operands[0].type || result_element != receiver_element))
            return false;
    } else if (operands[2].type != record->result_type) {
        return false;
    }
    const XrSemanticTypeRecord *result_element_type =
        result_element < ctx->plan->type_count ? &ctx->plan->types[result_element] : NULL;
    if (!result_element_type || xr_semantic_array_element_storage(result_element_type) !=
                                    value->array_result_element_storage)
        return false;
    if (kind == XR_SEM_ARRAY_HOF_FILTER) {
        if (callee->return_type >= ctx->plan->type_count ||
            !xr_semantic_array_member_bool_type_is_exact(&ctx->plan->types[callee->return_type]))
            return false;
    } else if (callee->return_type != result_element) {
        return false;
    }
    const XrSemanticParameterRecord *parameters = &ctx->plan->parameters[callee->parameter_begin];
    if (parameters[0].function != (uint32_t) callback || parameters[0].ordinal != 0 ||
        parameters[0].type != (kind == XR_SEM_ARRAY_HOF_REDUCE ? result_element : receiver_element))
        return false;
    if (kind == XR_SEM_ARRAY_HOF_REDUCE &&
        (parameters[1].function != (uint32_t) callback || parameters[1].ordinal != 1 ||
         parameters[1].type != receiver_element))
        return false;
    const XrSemanticTypeRecord *callback_type =
        operands[1].type < ctx->plan->type_count ? &ctx->plan->types[operands[1].type] : NULL;
    if (!callback_type || callback_type->kind != XR_KIND_FUNCTION ||
        callback_type->child_count != (uint32_t) callee->parameter_count + 1u ||
        callback_type->child_begin > ctx->plan->type_child_count ||
        callback_type->child_count > ctx->plan->type_child_count - callback_type->child_begin)
        return false;
    for (uint16_t i = 0; i < callee->parameter_count; i++)
        if (ctx->plan->type_children[callback_type->child_begin + i] != parameters[i].type)
            return false;
    if (ctx->plan->type_children[callback_type->child_begin + callee->parameter_count] !=
        callee->return_type)
        return false;
    const XiValue *callback_value = value->args[1];
    if (!callback_value->block || callback_value->block->func != owner || !callback_value->aux ||
        (callback_value->op != XI_CLOSURE_NEW &&
         !(callback_value->op == XI_STACK_ALLOC && callback_value->aux_int == XI_CLOSURE_NEW)))
        return false;
    uint32_t uses = 0;
    for (uint16_t b = 0; b < owner->nblocks; b++) {
        const XiBlock *block = owner->blocks[b];
        if (!block)
            return false;
        for (uint32_t v = 0; v < block->nvalues; v++) {
            const XiValue *use = block->values[v];
            if (!use)
                return false;
            for (uint16_t a = 0; a < use->nargs; a++)
                uses += use->args[a] == callback_value;
        }
        if (block->control == callback_value)
            uses++;
    }
    if (uses != 1)
        return false;
    if (record->semantic_immediate != 0 || record->auxiliary_kind != XI_AUX_KIND_NONE ||
        record->constant != XR_SEMANTIC_INDEX_NONE ||
        record->import_resolution != XR_SEM_IMPORT_RESOLUTION_NONE ||
        record->effects != xi_generated_op_effects(XI_CALL_METHOD) ||
        record->ownership_use != xi_generated_op_own_use(XI_CALL_METHOD) ||
        record->result_alias_operand != -1 || record->return_parameter != -1 ||
        (kind == XR_SEM_ARRAY_HOF_REDUCE
             ? record->result_ownership != XI_GEN_RESULT_OWNERSHIP_CALL_RESULT ||
                   record->return_provenance != XR_SEM_RETURN_NONE || record->return_complete != 0
             : record->result_ownership != XI_GEN_RESULT_OWNERSHIP_OWNED ||
                   record->return_provenance != XR_SEM_RETURN_OWNED ||
                   record->return_complete != 1) ||
        operands[0].role != XR_SEM_OPERAND_RECEIVER || operands[0].parameter != -1 ||
        operands[0].flags != XR_SEM_OPERAND_CALL_CONTRACT ||
        operands[1].role != XR_SEM_OPERAND_ARGUMENT || operands[1].parameter != 0 ||
        operands[1].flags != XR_SEM_OPERAND_CALL_CONTRACT ||
        (kind == XR_SEM_ARRAY_HOF_REDUCE &&
         (operands[2].role != XR_SEM_OPERAND_ARGUMENT || operands[2].parameter != 1 ||
          operands[2].flags != XR_SEM_OPERAND_CALL_CONTRACT)))
        return false;
    *callback_function = (uint32_t) callback;
    return true;
}

/* The selector is deliberately absent from this proof. Lowering already
 * stamped XI_ARRAY_MEMBER_FILL after the analyzer-backed builtin receiver
 * registry selected the member. These frozen rows independently re-prove the
 * receiver, result alias, ordered operands, scalar element storage, and
 * ownership contract before preserving that identity. */
static bool semantic_array_fill_scalar_exact(const XrSemanticBuildContext *ctx,
                                             const XrSemanticOperationRecord *record,
                                             uint8_t storage) {
    if (!ctx || !record || record->opcode != XI_CALL_METHOD || record->operand_count != 2 ||
        record->operand_begin > ctx->plan->operand_count ||
        record->operand_count > ctx->plan->operand_count - record->operand_begin ||
        record->metadata_count != 1 || record->metadata_begin >= ctx->plan->metadata_count ||
        storage <= XR_ELEM_ANY || storage >= XR_ELEM_RAWPTR)
        return false;
    const XrSemanticOperandRecord *receiver = &ctx->plan->operands[record->operand_begin];
    const XrSemanticOperandRecord *fill = receiver + 1;
    const XrSemanticTypeRecord *receiver_type =
        receiver->type < ctx->plan->type_count ? &ctx->plan->types[receiver->type] : NULL;
    if (!xr_semantic_array_type_row_is_exact(receiver_type) ||
        receiver_type->child_begin >= ctx->plan->type_child_count)
        return false;
    uint32_t element_index = ctx->plan->type_children[receiver_type->child_begin];
    const XrSemanticTypeRecord *element =
        element_index < ctx->plan->type_count ? &ctx->plan->types[element_index] : NULL;
    return element && fill->type == element_index &&
           xr_semantic_array_element_storage(element) == storage &&
           record->result_type == receiver->type && record->semantic_immediate == 0 &&
           record->auxiliary_kind == XI_AUX_KIND_NONE &&
           record->constant == XR_SEMANTIC_INDEX_NONE &&
           record->callable_function == XR_SEMANTIC_INDEX_NONE &&
           record->import_resolution == XR_SEM_IMPORT_RESOLUTION_NONE &&
           record->effects == xi_generated_op_effects(XI_CALL_METHOD) &&
           record->ownership_use == xi_generated_op_own_use(XI_CALL_METHOD) &&
           record->flags == xi_generated_op_default_flags(XI_CALL_METHOD) &&
           record->result_alias_operand == 0 &&
           /* The receiver comes back as itself, and lowering words that as an
            * owner when the caller already holds it outright and as a borrow
            * when it reads the container through a shared root. Demanding the
            * owner pairing made `fill` unusable on a module-level array while
            * the three-argument spelling of the same member worked. Both
            * pairings say the same thing about the same value; the mixtures do
            * not, and stay refused. */
           ((record->result_ownership == XI_GEN_RESULT_OWNERSHIP_OWNED &&
             record->return_provenance == XR_SEM_RETURN_OWNED) ||
            (record->result_ownership == XI_GEN_RESULT_OWNERSHIP_BORROWED &&
             record->return_provenance == XR_SEM_RETURN_BORROWED_STATIC)) &&
           record->return_parameter == -1 && record->return_complete == 1 &&
           receiver->role == XR_SEM_OPERAND_RECEIVER && receiver->parameter == -1 &&
           receiver->flags == XR_SEM_OPERAND_CALL_CONTRACT &&
           receiver->ownership_action == XR_SEM_OPERAND_BORROW &&
           fill->role == XR_SEM_OPERAND_ARGUMENT && fill->parameter == 0 &&
           fill->flags == XR_SEM_OPERAND_CALL_CONTRACT &&
           fill->ownership_action == XR_SEM_OPERAND_CONSUME;
}

/* Reference-capable storage authority is narrower than the shape table: only
 * the exact source-class push and range-fill method identities may consume an
 * element into tagged Array storage. */

static bool semantic_array_member_reference_contract_exact(
    const XrSemanticBuildContext *ctx, const XrArrayMemberShape *shape,
    const XrSemanticOperationRecord *record, uint32_t element_type_index,
    const XrSemanticTypeRecord *element_type) {
    if (!ctx || !shape || !record || !element_type)
        return false;
    if ((element_type->flags & XR_SEM_TYPE_REFERENCE_CAPABLE) == 0)
        return true;
    if (shape->reference_action == XR_ARRAY_MEMBER_REFERENCE_PRESERVE)
        return shape->element_operand == 0 &&
               (shape->element_access == XR_ARRAY_MEMBER_ELEMENT_ACCESS_READ ||
                shape->element_access == XR_ARRAY_MEMBER_ELEMENT_ACCESS_MOVE) &&
               shape->reference_drop == XR_ARRAY_MEMBER_REFERENCE_DROP_NONE;
    if (shape->reference_action != XR_ARRAY_MEMBER_REFERENCE_CONSUME_INTO_STORAGE ||
        shape->element_access != XR_ARRAY_MEMBER_ELEMENT_ACCESS_STORE ||
        shape->reference_drop != XR_ARRAY_MEMBER_REFERENCE_DROP_RELEASE_ON_ERASE_OR_DESTROY ||
        shape->element_operand == 0 || shape->element_operand >= record->operand_count ||
        xr_semantic_class_instance_type_source_class(ctx->plan, element_type) ==
            XR_SEMANTIC_INDEX_NONE)
        return false;
    bool exact_push = strcmp(shape->selector, "push") == 0 && record->operand_count == 2 &&
                      record->semantic_immediate == (int64_t) XI_METHOD_SYMBOL_PUSH << 1;
    bool exact_fill = strcmp(shape->selector, "fill") == 0 && record->operand_count == 4 &&
                      record->semantic_immediate == (int64_t) XI_METHOD_SYMBOL_FILL << 1;
    if (!exact_push && !exact_fill)
        return false;
    uint32_t semantic_operand = record->operand_begin + shape->element_operand;
    if (semantic_operand >= ctx->plan->operand_count)
        return false;
    const XrSemanticOperandRecord *element = &ctx->plan->operands[semantic_operand];
    return element->type == element_type_index && element->role == XR_SEM_OPERAND_ARGUMENT &&
           element->parameter == (int16_t) (shape->element_operand - 1u) &&
           element->flags == XR_SEM_OPERAND_CALL_CONTRACT &&
           element->ownership_action == XR_SEM_OPERAND_CONSUME;
}

static bool semantic_array_member_scalar_exact(const XrSemanticBuildContext *ctx,
                                               const XrSemanticOperationRecord *record) {
    if (!ctx || !record || record->operand_begin > ctx->plan->operand_count ||
        record->operand_count > ctx->plan->operand_count - record->operand_begin ||
        record->metadata_count != 1 || record->metadata_begin >= ctx->plan->metadata_count)
        return false;
    const XrArrayMemberShape *shape =
        xr_array_member_shape(ctx->plan->metadata[record->metadata_begin], record->operand_count);
    const XrSemanticOperandRecord *receiver = &ctx->plan->operands[record->operand_begin];
    const XrSemanticTypeRecord *receiver_type =
        receiver->type < ctx->plan->type_count ? &ctx->plan->types[receiver->type] : NULL;
    const XrSemanticTypeRecord *result_type =
        record->result_type < ctx->plan->type_count ? &ctx->plan->types[record->result_type] : NULL;
    if (!shape || !xr_semantic_array_type_row_is_exact(receiver_type) ||
        receiver_type->child_begin >= ctx->plan->type_child_count)
        return false;
    uint32_t element_type_index = ctx->plan->type_children[receiver_type->child_begin];
    const XrSemanticTypeRecord *element_type =
        element_type_index < ctx->plan->type_count ? &ctx->plan->types[element_type_index] : NULL;
    bool source_class_fill_result =
        element_type && strcmp(shape->selector, "fill") == 0 && record->operand_count == 4 &&
        record->semantic_immediate == (int64_t) XI_METHOD_SYMBOL_FILL << 1 &&
        xr_semantic_class_instance_type_source_class(ctx->plan, element_type) !=
            XR_SEMANTIC_INDEX_NONE &&
        record->result_type == receiver->type && record->result_alias_operand == 0 &&
        ((record->result_ownership == XI_GEN_RESULT_OWNERSHIP_BORROWED &&
          record->return_provenance == XR_SEM_RETURN_BORROWED_PARAM &&
          record->return_parameter == 0 && record->return_complete == 1) ||
         (record->result_ownership == XI_GEN_RESULT_OWNERSHIP_CALL_RESULT &&
          record->return_provenance == XR_SEM_RETURN_NONE && record->return_parameter == -1 &&
          record->return_complete == 0));
    if (!element_type ||
        !semantic_array_member_reference_contract_exact(ctx, shape, record, element_type_index,
                                                        element_type) ||
        (!xr_semantic_array_member_result_is_exact(record, shape, result_type, receiver->type) &&
         !source_class_fill_result) ||
        record->effects != xi_generated_op_effects(XI_CALL_METHOD) ||
        receiver->role != XR_SEM_OPERAND_RECEIVER || receiver->parameter != -1 ||
        receiver->flags != XR_SEM_OPERAND_CALL_CONTRACT ||
        receiver->ownership_action != XR_SEM_OPERAND_BORROW)
        return false;
    /* The element argument is proven against the receiver's own element entry
     * rather than a spelled type name; every other argument is an exact signed
     * 64-bit bound the container reads. */
    for (uint16_t i = 1; i < record->operand_count; i++) {
        const XrSemanticOperandRecord *argument = receiver + i;
        const XrSemanticTypeRecord *argument_type =
            argument->type < ctx->plan->type_count ? &ctx->plan->types[argument->type] : NULL;
        if (!xr_semantic_array_member_argument_is_exact(shape, argument, argument_type, i,
                                                        element_type_index))
            return false;
    }
    return true;
}

/* The module-init import reference of a native stdlib namespace. No source
 * declaration can produce this record: the frozen import classification is
 * resolved against the native registry rather than against a compiled module,
 * and the frozen metadata pair names the module path with an empty member, so
 * a member import and a source-module namespace both stay outside. */
static bool semantic_native_module_namespace_import_exact(const XrSemanticPlan *plan,
                                                          const XrSemanticOperationRecord *record,
                                                          const char **out_module_path) {
    uint32_t metadata_count = 0;
    const char *const *metadata = xr_semantic_plan_metadata(plan, &metadata_count);
    const XrSemanticTypeRecord *type =
        record ? xr_semantic_plan_type(plan, record->result_type) : NULL;
    XrStableId zero = {{0}};
    if (!record || !type || !metadata || record->opcode != XI_IMPORT_REF || record->function != 0 ||
        record->operand_count != 0 || record->metadata_count != 2 ||
        record->metadata_begin + 1u >= metadata_count ||
        record->import_resolution != XR_SEM_IMPORT_RESOLUTION_NATIVE_STDLIB ||
        record->semantic_immediate < -1 || record->semantic_immediate > UINT16_MAX ||
        record->allocation_key || !xr_stable_id_equal(record->allocation_id, zero) ||
        record->constant != XR_SEMANTIC_INDEX_NONE ||
        record->callable_function != XR_SEMANTIC_INDEX_NONE || record->auxiliary_kind != 0 ||
        record->effects != xi_generated_op_effects(XI_IMPORT_REF) ||
        record->flags != xi_generated_op_default_flags(XI_IMPORT_REF) ||
        record->ownership_use != xi_generated_op_own_use(XI_IMPORT_REF) ||
        record->result_ownership != XI_GEN_RESULT_OWNERSHIP_BORROWED ||
        record->result_alias_operand != -1 ||
        record->return_provenance != XR_SEM_RETURN_BORROWED_STATIC ||
        record->return_parameter != -1 || record->return_complete != 1 ||
        type->scalar_rep != XR_SCALAR_REP_NONE || type->child_count != 0 ||
        type->aggregate_extent != 0 || type->aggregate_align != 0 ||
        type->flags != (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT))
        return false;
    const char *module_path = metadata[record->metadata_begin];
    const char *member = metadata[record->metadata_begin + 1u];
    if (!module_path || !member || member[0] != '\0' ||
        !xr_stdlib_metadata_module_known(module_path))
        return false;
    if (out_module_path)
        *out_module_path = module_path;
    return true;
}

/* Frozen-row reconstruction of a namespace receiver: the load reads a module
 * shared slot, exactly one module-init store publishes that slot, and the
 * stored value is the module-init import reference above. */
static const char *semantic_native_module_namespace_path(const XrSemanticPlan *plan,
                                                         uint32_t receiver_value) {
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(plan);
    const XrSemanticOperationRecord *load = NULL;
    XrStableId zero = {{0}};
    for (uint32_t i = 0; i < operation_count; i++) {
        const XrSemanticOperationRecord *candidate = xr_semantic_plan_operation(plan, i);
        if (!candidate || candidate->result_value != receiver_value)
            continue;
        if (load)
            return NULL;
        load = candidate;
    }
    const XrSemanticTypeRecord *load_type =
        load ? xr_semantic_plan_type(plan, load->result_type) : NULL;
    if (!load || !load_type || load->opcode != XI_GET_SHARED || load->operand_count != 0 ||
        load->metadata_count != 0 || load->semantic_immediate < 0 ||
        load->semantic_immediate > UINT16_MAX || load->allocation_key ||
        !xr_stable_id_equal(load->allocation_id, zero) ||
        load->constant != XR_SEMANTIC_INDEX_NONE ||
        load->callable_function != XR_SEMANTIC_INDEX_NONE || load->auxiliary_kind != 0 ||
        load->import_resolution != XR_SEM_IMPORT_RESOLUTION_NONE ||
        load->effects != xi_generated_op_effects(XI_GET_SHARED) ||
        load->flags != xi_generated_op_default_flags(XI_GET_SHARED) ||
        load->ownership_use != xi_generated_op_own_use(XI_GET_SHARED) ||
        load->result_ownership != XI_GEN_RESULT_OWNERSHIP_BORROWED ||
        load->result_alias_operand != -1 ||
        load->return_provenance != XR_SEM_RETURN_BORROWED_STATIC || load->return_parameter != -1 ||
        load->return_complete != 1 || load_type->scalar_rep != XR_SCALAR_REP_NONE ||
        load_type->child_count != 0 ||
        load_type->flags != (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT))
        return NULL;
    const XrSemanticOperationRecord *store = NULL;
    for (uint32_t i = 0; i < operation_count; i++) {
        const XrSemanticOperationRecord *candidate = xr_semantic_plan_operation(plan, i);
        if (!candidate || candidate->opcode != XI_SET_SHARED || candidate->function != 0 ||
            candidate->semantic_immediate != load->semantic_immediate)
            continue;
        if (store)
            return NULL;
        store = candidate;
    }
    if (!store || store->operand_count != 1 || store->operand_begin >= operand_count)
        return NULL;
    const XrSemanticOperandRecord *stored = &operands[store->operand_begin];
    if (stored->role != XR_SEM_OPERAND_VALUE || stored->parameter != -1 ||
        stored->ownership_action != XR_SEM_OPERAND_CONSUME || stored->flags != 0 ||
        stored->type != load->result_type)
        return NULL;
    const XrSemanticOperationRecord *import = NULL;
    for (uint32_t i = 0; i < operation_count; i++) {
        const XrSemanticOperationRecord *candidate = xr_semantic_plan_operation(plan, i);
        if (!candidate || candidate->result_value != stored->value)
            continue;
        if (import)
            return NULL;
        import = candidate;
    }
    const char *module_path = NULL;
    return import && import->result_type == load->result_type &&
                   semantic_native_module_namespace_import_exact(plan, import, &module_path)
               ? module_path
               : NULL;
}

/* A native stdlib module namespace is not a value a program can construct: the
 * receiver is the module-init import reference published into a shared slot,
 * and the frozen definition registry names exactly one implementation for that
 * module path plus the selector.  A user module resolves to a different import
 * classification, and a local function sharing the selector is a plain call
 * through a callee operand rather than a method call on that namespace, so
 * neither reaches this authority. */
static bool xi_native_module_scalar_call_exact(const XrSemanticBuildContext *ctx,
                                               const XiFunc *caller, const XiValue *value) {
    if (!ctx || !caller || !value || value->op != XI_CALL_METHOD || value->nargs == 0 ||
        !value->aux || value->aux_kind != XI_AUX_KIND_NONE || value->aux_int <= 0 ||
        (value->aux_int & 1) != 0 || (value->flags & XI_FLAG_MAY_SUSPEND) != 0)
        return false;
    const XiImportRef *ref = resolve_native_namespace_import_receiver(ctx, caller, value->args[0]);
    return ref && ref->module_path &&
           xr_stdlib_metadata_exact_native_direct_member(
               ref->module_path, (const char *) value->aux, (uint16_t) (value->nargs - 1u)) != NULL;
}

static bool semantic_native_module_scalar_call_exact(const XrSemanticBuildContext *ctx,
                                                     const XrSemanticOperationRecord *record) {
    if (!ctx || !record || record->operand_count == 0 ||
        record->operand_begin > ctx->plan->operand_count ||
        record->operand_count > ctx->plan->operand_count - record->operand_begin ||
        record->metadata_count != 1 || record->metadata_begin >= ctx->plan->metadata_count ||
        record->result_alias_operand != -1 ||
        record->result_ownership != XI_GEN_RESULT_OWNERSHIP_CALL_RESULT ||
        record->effects != xi_generated_op_effects(XI_CALL_METHOD) ||
        !xr_semantic_native_module_boundary_type_is_exact(
            record->result_type < ctx->plan->type_count ? &ctx->plan->types[record->result_type]
                                                        : NULL,
            true))
        return false;
    const XrSemanticOperandRecord *receiver = &ctx->plan->operands[record->operand_begin];
    if (receiver->role != XR_SEM_OPERAND_RECEIVER || receiver->parameter != -1 ||
        receiver->flags != XR_SEM_OPERAND_CALL_CONTRACT ||
        receiver->ownership_action != XR_SEM_OPERAND_BORROW)
        return false;
    for (uint16_t i = 1; i < record->operand_count; i++) {
        const XrSemanticOperandRecord *argument = receiver + i;
        if (argument->role != XR_SEM_OPERAND_ARGUMENT || argument->parameter != (int16_t) (i - 1) ||
            argument->flags != XR_SEM_OPERAND_CALL_CONTRACT ||
            !xr_semantic_native_module_boundary_type_is_exact(
                argument->type < ctx->plan->type_count ? &ctx->plan->types[argument->type] : NULL,
                false))
            return false;
    }
    const char *module_path = semantic_native_module_namespace_path(ctx->plan, receiver->value);
    return module_path && xr_stdlib_metadata_exact_native_direct_member(
                              module_path, ctx->plan->metadata[record->metadata_begin],
                              (uint16_t) (record->operand_count - 1u)) != NULL;
}

/* `JSON` is a compiler-owned class namespace: its receiver is the reserved
 * XI_GET_BUILTIN global, and no source declaration can produce that type
 * record because a user class always carries its own class reference.  The
 * namespace plus the frozen selector names exactly one implementation, so a
 * `JSON.value(x)` callsite has a single dispatch target with no open domain. */
/* A PanicInfo construction, recognised here only well enough to ask the shared
 * judgement. That judgement re-proves every term and the caller withdraws the
 * mark when it says no, so this states the call shape and nothing more. */
static bool xi_panic_info_constructor_exact(const XiValue *value) {
    return value && value->op == XI_CALL_METHOD && value->nargs == 2 && value->aux &&
           value->aux_kind == XI_AUX_KIND_NONE &&
           strcmp((const char *) value->aux, "constructor") == 0;
}

static bool xi_json_namespace_value_exact(const XiValue *value) {
    const XiValue *receiver = value && value->nargs == 2 ? value->args[0] : NULL;
    return value && value->op == XI_CALL_METHOD && receiver && value->args[1] && value->aux &&
           strcmp((const char *) value->aux, "value") == 0 && value->aux_kind == XI_AUX_KIND_NONE &&
           value->aux_int > 0 && (value->aux_int & 1) == 0 && receiver->op == XI_GET_BUILTIN &&
           receiver->aux_int == XR_GLOBAL_VAR_JSON && receiver->aux &&
           strcmp((const char *) receiver->aux, "JSON") == 0 && receiver->type &&
           receiver->type->kind == XR_KIND_CLASS && !receiver->type->instance.class_ref &&
           value->type && value->type->kind == XR_KIND_JSON;
}

static bool semantic_json_namespace_type_exact(const XrSemanticTypeRecord *type) {
    char expected[160];
    int length =
        snprintf(expected, sizeof(expected), "type-v3:%u:0:%u:0:0:0:0:0:0:%u:0:;named:4:JSON[0]",
                 (unsigned) XR_KIND_CLASS, (unsigned) XR_TID_NULL, (unsigned) XR_SCALAR_REP_NONE);
    XrStableId zero = {{0}};
    return type && length > 0 && (size_t) length < sizeof(expected) &&
           type->kind == XR_KIND_CLASS && type->builtin_type == XR_TID_NULL &&
           type->child_count == 0 && type->aggregate_extent == 0 && type->aggregate_align == 0 &&
           type->scalar_rep == XR_SCALAR_REP_NONE && type->source_class == XR_SEMANTIC_INDEX_NONE &&
           xr_stable_id_equal(type->source_class_identity, zero) && type->canonical_key &&
           strcmp(type->canonical_key, expected) == 0;
}

static bool semantic_json_namespace_value_exact(const XrSemanticBuildContext *ctx,
                                                const XrSemanticOperationRecord *record) {
    if (!ctx || !record || record->operand_count != 2 ||
        record->operand_begin > ctx->plan->operand_count ||
        record->operand_count > ctx->plan->operand_count - record->operand_begin ||
        record->metadata_count != 1 || record->metadata_begin >= ctx->plan->metadata_count)
        return false;
    const XrSemanticOperandRecord *receiver = &ctx->plan->operands[record->operand_begin];
    const XrSemanticOperandRecord *argument = receiver + 1;
    const XrSemanticTypeRecord *receiver_type =
        receiver->type < ctx->plan->type_count ? &ctx->plan->types[receiver->type] : NULL;
    const XrSemanticTypeRecord *result_type =
        record->result_type < ctx->plan->type_count ? &ctx->plan->types[record->result_type] : NULL;
    return semantic_json_namespace_type_exact(receiver_type) && result_type &&
           result_type->kind == XR_KIND_JSON && result_type->builtin_type == XR_TID_NULL &&
           result_type->child_count == 0 && result_type->scalar_rep == XR_SCALAR_REP_NONE &&
           strcmp(ctx->plan->metadata[record->metadata_begin], "value") == 0 &&
           receiver->role == XR_SEM_OPERAND_RECEIVER && receiver->parameter == -1 &&
           receiver->flags == XR_SEM_OPERAND_CALL_CONTRACT &&
           argument->role == XR_SEM_OPERAND_ARGUMENT && argument->parameter == 0 &&
           argument->flags == XR_SEM_OPERAND_CALL_CONTRACT && record->result_alias_operand == -1 &&
           record->result_ownership == XI_GEN_RESULT_OWNERSHIP_OWNED;
}

static bool semantic_string_builder_constructor_exact(const XrSemanticBuildContext *ctx,
                                                      const XrSemanticOperationRecord *record) {
    const XrSemanticTypeRecord *type =
        record->result_type < ctx->plan->type_count ? &ctx->plan->types[record->result_type] : NULL;
    const char *metadata =
        record->metadata_count == 1 && record->metadata_begin < ctx->plan->metadata_count
            ? ctx->plan->metadata[record->metadata_begin]
            : NULL;
    return semantic_string_builder_type_exact(type) && metadata &&
           strcmp(metadata, "StringBuilder") == 0 && record->operand_count == 0 &&
           record->auxiliary_kind == XI_AUX_KIND_NONE && record->semantic_immediate == 0 &&
           record->constant == XR_SEMANTIC_INDEX_NONE &&
           record->callable_function == XR_SEMANTIC_INDEX_NONE &&
           record->import_resolution == XR_SEM_IMPORT_RESOLUTION_NONE &&
           record->ownership_use == xi_generated_op_own_use(XI_CALL_BUILTIN) &&
           record->result_ownership == XI_GEN_RESULT_OWNERSHIP_OWNED &&
           record->transfer_mode == XR_TRANSFER_SHARE && record->parameter_mode == XR_PARAM_READ &&
           record->parameter_ownership == XI_OWN_NONE &&
           record->flags == xi_generated_op_default_flags(XI_CALL_BUILTIN) &&
           record->result_alias_operand == -1 && record->return_parameter == -1 &&
           record->return_provenance == XR_SEM_RETURN_OWNED && record->return_complete == 1;
}

static bool append_operation_allocation_identity(XrSemanticBuildContext *ctx,
                                                 XrSemanticOperationRecord *record) {
    XrTextBuilder allocation_key = {0};
    if (!text_append_format(&allocation_key, "%s/allocation", record->canonical_key)) {
        text_dispose(&allocation_key);
        return fail(ctx, "XR_EXEC_5003", "allocation identity allocation failed");
    }
    record->allocation_key = xr_semantic_plan_copy_string(ctx->plan, allocation_key.data);
    text_dispose(&allocation_key);
    XrFingerprint digest;
    if (!record->allocation_key ||
        !xr_stable_id_from_key(record->allocation_key, &record->allocation_id, &digest))
        return fail(ctx, "XR_EXEC_5003", "allocation stable identity failed");
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
    record->view_source_value = XR_SEMANTIC_INDEX_NONE;
    record->view_element_type = XR_SEMANTIC_INDEX_NONE;
    record->view_source_operand = -1;
    record->view_source_parameter = -1;
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
    record->callable_function = XR_SEMANTIC_INDEX_NONE;
    record->auxiliary_kind = value->aux_kind;
    record->import_resolution = value->op == XI_IMPORT_REF
                                    ? classify_import_resolution((const XiImportRef *) value->aux)
                                    : XR_SEM_IMPORT_RESOLUTION_NONE;
    record->effects = xi_generated_op_effects(value->op);
    record->source_line = value->line;
    if (!xi_source_span_is_empty(value->source_span)) {
        const char *source_file = ctx->module_identity;
        if (!xi_source_span_is_complete(value->source_span) || !source_file || !source_file[0])
            return fail(ctx, "XR_SEM_0019", "operation debug span is incomplete");
        record->source_file = copy_canonical_source_file(ctx, source_file);
        if (!record->source_file)
            return fail(ctx, "XR_EXEC_5003", "operation debug file allocation failed");
        record->source_start_line = value->source_span.start_line;
        record->source_start_column = value->source_span.start_column;
        record->source_end_line = value->source_span.end_line;
        record->source_end_column = value->source_span.end_column;
    }
    record->semantic_immediate = value->array_intrinsic_kind != XI_ARRAY_INTRINSIC_NONE ||
                                         value->array_member_kind != XI_ARRAY_MEMBER_NONE ||
                                         value->array_hof_kind != XI_ARRAY_HOF_NONE
                                     ? 0
                                     : value->aux_int;
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
    if (ctx->program_closure && value->op == XI_PARAM && value->psc_type_index != XI_PSC_ROW_NONE) {
        const XrProgramSemanticTypeRecord *program_parameter_type =
            xr_program_semantic_closure_type(ctx->program_closure, value->psc_type_index);
        if (program_parameter_type &&
            program_parameter_type->kind == XR_PROGRAM_SEMANTIC_TYPE_LEAF_VALUE_AGGREGATE)
            record->parameter_ownership = XI_OWN_NONE;
    }
    record->flags = value->flags;
    XiReturnOwnership value_ownership = xi_arc_value_return_ownership(function, value);
    record->result_alias_operand = xi_arc_value_alias_operand(function, value);
    if (record->result_alias_operand < 0 && value->nargs > 0 &&
        (xi_copy_is_identity_alias(value) || value->op == XI_SOURCE_MOVE))
        record->result_alias_operand = 0;
    record->return_parameter = value_ownership.param_index;
    record->return_provenance = value_ownership.kind;
    record->return_complete = value_ownership.complete ? 1u : 0u;
    if (ctx->program_closure && value->op == XI_CALL && value->psc_call_index != XI_PSC_ROW_NONE &&
        xr_program_semantic_closure_family(ctx->program_closure) ==
            XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_AGGREGATE_DIRECT_CALL) {
        record->result_ownership = XI_GEN_RESULT_OWNERSHIP_CALL_RESULT;
        record->parameter_ownership = XI_OWN_NONE;
        record->return_parameter = -1;
        record->return_provenance = XR_SEM_RETURN_NONE;
        record->return_complete = 0;
    }
    if (value->op == XI_ASSERTION) {
        const XrAssertionPlan *plan = xi_assertion_plan(value);
        if (!plan || !xr_assertion_plan_validate(plan) || plan->arity != value->nargs ||
            !xi_source_span_is_complete(value->source_span) ||
            plan->source.line != value->source_span.start_line ||
            plan->source.column != value->source_span.start_column ||
            plan->source.end_line != value->source_span.end_line ||
            plan->source.end_column != value->source_span.end_column)
            return fail(ctx, "XR_SEM_0019", "assertion plan authority is incomplete");
        record->semantic_immediate = 0;
        record->intrinsic_kind = XR_SEM_INTRINSIC_ASSERTION;
        record->evidence[XR_SEM_ASSERT_EVIDENCE_SCHEMA] = plan->schema_version;
        record->evidence[XR_SEM_ASSERT_EVIDENCE_BUILTIN_ID] = plan->builtin_id;
        record->evidence[XR_SEM_ASSERT_EVIDENCE_KIND] = plan->kind;
        record->evidence[XR_SEM_ASSERT_EVIDENCE_FAILURE_CHANNEL] = plan->expected_failure_channel;
        record->evidence[XR_SEM_ASSERT_EVIDENCE_FLOW_RULE] = plan->flow_rule;
        record->evidence[XR_SEM_ASSERT_EVIDENCE_EQUALITY] = plan->equality_authority;
        record->evidence[XR_SEM_ASSERT_EVIDENCE_TARGET] = plan->target;
        record->evidence[XR_SEM_ASSERT_EVIDENCE_CAPABILITIES] = plan->required_capabilities;
    }
    if (value->xa_intrinsic_id == XA_INTRINSIC_STRING_BYTE_SLICE_VIEW) {
        const XiViewEvidence *view = &value->view_evidence;
        XrType *element = value->type && XR_TYPE_IS_SLICE(value->type)
                              ? value->type->container.element_type
                              : NULL;
        if (!view->complete || view->origin != XI_VIEW_ORIGIN_RECEIVER ||
            view->source_operand != 0 || view->source_param != -1 || value->nargs != 1 ||
            !value->args[0] || view->root_value_id != value->args[0]->id || view->capability != 1 ||
            view->lifetime != 1 || !element || !add_type(ctx, element, &record->view_element_type))
            return fail(ctx, "XR_SEM_0019", "string byte-slice view authority is incomplete");
        record->view_source_value = value_ref(ctx, function, value->args[0]);
        if (record->view_source_value == XR_SEMANTIC_INDEX_NONE)
            return fail(ctx, "XR_SEM_0019", "string byte-slice view source is invalid");
        record->view_source_operand = view->source_operand;
        record->view_source_parameter = view->source_param;
        record->intrinsic_kind = XR_SEM_INTRINSIC_STRING_BYTE_SLICE_VIEW;
        record->view_origin = view->origin;
        record->view_capability = view->capability;
        record->view_lifetime = view->lifetime;
        record->view_complete = 1;
    }
    for (uint16_t i = 0; i < value->nargs; i++) {
        if (!append_operand(ctx, function, value, i))
            return false;
    }
    bool closure_binding = value->op == XI_CLOSURE_NEW ||
                           (value->op == XI_STACK_ALLOC && value->aux_int == XI_CLOSURE_NEW);
    if (closure_binding) {
        if (!value->aux)
            return fail(ctx, "XR_SEM_0007", "closure operation has no exact function binding");
        int child = function_index(ctx, (const XiFunc *) value->aux);
        if (child < 0)
            return fail(ctx, "XR_SEM_0007", "closure operation references an unknown function");
        record->callable_function = (uint32_t) child;
    } else if (value->op == XI_TRY && value->aux) {
        record->evidence[7] = block_ref(ctx, (const XiBlock *) value->aux);
        if (record->evidence[7] == XR_SEMANTIC_INDEX_NONE)
            return fail(ctx, "XR_SEM_0007", "try operation references an unknown block");
    }
    if (!add_operation_metadata(ctx, value, record) ||
        !append_source_namespace_dependency(ctx, value) || !append_constant(ctx, value, record))
        return false;
    if (value->op == XI_ASSERTION) {
        XrAssertionPlan semantic_plan;
        if (!xr_semantic_operation_assertion_plan(record, &semantic_plan))
            return fail(ctx, "XR_SEM_0019", "assertion semantic projection is not exact");
    }
    if (xi_string_runes_exact(value)) {
        record->intrinsic_kind = XR_SEM_INTRINSIC_STRING_RUNES;
        if (!xr_semantic_string_runes_is_exact(ctx->plan, record, NULL))
            return fail(ctx, "XR_SEM_0019", "String.runes authority is not exact");
    }
    if (xi_iterator_rune_has_next_exact(value)) {
        record->intrinsic_kind = XR_SEM_INTRINSIC_ITERATOR_RUNE_HAS_NEXT;
        if (!xr_semantic_iterator_rune_has_next_is_exact(ctx->plan, record, NULL))
            return fail(ctx, "XR_SEM_0019", "Iterator<rune>.hasNext authority is not exact");
    }
    if (xi_iterator_rune_next_exact(value)) {
        record->intrinsic_kind = XR_SEM_INTRINSIC_ITERATOR_RUNE_NEXT;
        if (!xr_semantic_iterator_rune_next_is_exact(ctx->plan, record, NULL))
            return fail(ctx, "XR_SEM_0019", "Iterator<rune>.next authority is not exact");
    }
    if (xi_iterator_rune_nth_exact(value)) {
        record->intrinsic_kind = XR_SEM_INTRINSIC_ITERATOR_RUNE_NTH;
        if (!xr_semantic_iterator_rune_nth_is_exact(ctx->plan, record, NULL, NULL))
            return fail(ctx, "XR_SEM_0019", "Iterator<rune>.nth authority is not exact");
    }
    if (xi_map_entries_iterator_is_exact(value)) {
        record->intrinsic_kind = XR_SEM_INTRINSIC_MAP_ENTRIES_ITERATOR;
        if (!xr_semantic_map_entries_iterator_is_exact(ctx->plan, record, NULL, NULL))
            return fail(ctx, "XR_SEM_0019", "Map.entriesIterator authority is not exact");
    }
    if (xi_map_entry_iterator_has_next_is_exact(value)) {
        record->intrinsic_kind = XR_SEM_INTRINSIC_MAP_ENTRY_ITERATOR_HAS_NEXT;
        if (!xr_semantic_map_entry_iterator_has_next_is_exact(ctx->plan, record, NULL))
            return fail(ctx, "XR_SEM_0019", "Map entry iterator hasNext authority is not exact");
    }
    if (xi_map_entry_iterator_next_is_exact(value)) {
        record->intrinsic_kind = XR_SEM_INTRINSIC_MAP_ENTRY_ITERATOR_NEXT;
        if (!xr_semantic_map_entry_iterator_next_is_exact(ctx->plan, record, NULL))
            return fail(ctx, "XR_SEM_0019", "Map entry iterator next authority is not exact");
    }
    if (xi_rune_to_uint32_exact(value)) {
        record->intrinsic_kind = XR_SEM_INTRINSIC_RUNE_TO_UINT32;
        if (!xr_semantic_rune_to_uint32_is_exact(ctx->plan, record, NULL))
            return fail(ctx, "XR_SEM_0019", "rune.toUInt32 authority is not exact");
    }
    if (xi_rune_to_string_exact(value)) {
        record->intrinsic_kind = XR_SEM_INTRINSIC_RUNE_TO_STRING;
        if (!xr_semantic_rune_to_string_is_exact(ctx->plan, record, NULL))
            return fail(ctx, "XR_SEM_0019", "rune.toString authority is not exact");
    }
    if (xi_rune_is_whitespace_exact(value)) {
        record->intrinsic_kind = XR_SEM_INTRINSIC_RUNE_IS_WHITESPACE;
        if (!xr_semantic_rune_is_whitespace_is_exact(ctx->plan, record, NULL))
            return fail(ctx, "XR_SEM_0019", "rune.isWhitespace authority is not exact");
    }
    if (xi_string_slice_range_exact(value)) {
        record->intrinsic_kind = XR_SEM_INTRINSIC_STRING_SLICE_RANGE;
        if (!xr_semantic_string_slice_range_is_exact(ctx->plan, record, NULL, NULL, NULL))
            record->intrinsic_kind = XR_SEM_INTRINSIC_NONE;
    }
    if (value->xa_intrinsic_id == XA_INTRINSIC_STRING_BUILDER_APPEND) {
        bool rune = semantic_string_builder_append_rune_exact(ctx, record);
        bool string = semantic_string_builder_append_string_exact(ctx, record);
        if (rune == string)
            return fail(ctx, "XR_SEM_0019",
                        "StringBuilder.append producer identity has no unique frozen shape");
        record->intrinsic_kind = rune ? XR_SEM_INTRINSIC_STRINGBUILDER_APPEND_RUNE
                                      : XR_SEM_INTRINSIC_STRINGBUILDER_APPEND_STRING;
    }
    if (xi_string_builder_to_string_exact(value) &&
        semantic_string_builder_to_string_exact(ctx, record))
        record->intrinsic_kind = XR_SEM_INTRINSIC_STRINGBUILDER_TO_STRING;
    if (xi_json_namespace_value_exact(value) && semantic_json_namespace_value_exact(ctx, record))
        record->intrinsic_kind = XR_SEM_INTRINSIC_JSON_NAMESPACE_VALUE;
    if (xi_panic_info_constructor_exact(value)) {
        record->intrinsic_kind = XR_SEM_INTRINSIC_PANIC_INFO_CONSTRUCTOR;
        if (!xr_semantic_panic_info_constructor_is_exact(ctx->plan, record, NULL))
            record->intrinsic_kind = XR_SEM_INTRINSIC_NONE;
    }
    if (value->xa_intrinsic_id == XA_INTRINSIC_ARRAY_RESERVE &&
        semantic_array_reserve_exact(ctx, record))
        record->intrinsic_kind = XR_SEM_INTRINSIC_ARRAY_MEMBER_SCALAR;
    if (value->array_hof_kind != XI_ARRAY_HOF_NONE) {
        uint32_t callback = XR_SEMANTIC_INDEX_NONE;
        /* Lowering marks a higher-order call before captures are computed, so
         * its mark is a candidate rather than a finding: a callback that later
         * turns out to capture -- which lowering saw as capture-free -- cannot
         * carry higher-order authority, because that authority rests on the
         * callback needing no environment. The plan is the authority, and when
         * it declines, the call is simply an ordinary method call and records
         * no higher-order row. Refusing the whole function instead rejected
         * every `arr.map(fn(x) { ... factor ... })` in the program. */
        if (semantic_array_hof_exact(ctx, function, value, record, &callback)) {
            record->intrinsic_kind = XR_SEM_INTRINSIC_ARRAY_HOF;
            record->array_hof_kind = semantic_array_hof_kind_from_xi(value->array_hof_kind);
            record->array_element_storage = value->array_element_storage;
            record->array_result_element_storage = value->array_result_element_storage;
            record->callable_function = callback;
        }
    }
    if (value->array_member_kind == XI_ARRAY_MEMBER_FILL &&
        semantic_array_fill_scalar_exact(ctx, record, value->array_element_storage)) {
        record->intrinsic_kind = XR_SEM_INTRINSIC_ARRAY_FILL_SCALAR;
        record->array_element_storage = value->array_element_storage;
    } else if (xi_array_member_scalar_exact(value) &&
               semantic_array_member_scalar_exact(ctx, record))
        record->intrinsic_kind = XR_SEM_INTRINSIC_ARRAY_MEMBER_SCALAR;
    if (xi_native_module_scalar_call_exact(ctx, function, value) &&
        semantic_native_module_scalar_call_exact(ctx, record))
        record->intrinsic_kind = XR_SEM_INTRINSIC_NATIVE_MODULE_SCALAR_CALL;
    bool array_fill_type_exact =
        value->array_intrinsic_kind != XI_ARRAY_INTRINSIC_FILLED_NEW ||
        (value->nargs > 1 && value->args[1] && value->args[1]->type &&
         !value->args[1]->type->is_nullable &&
         (value->array_element_storage == XR_ELEM_RUNE
              ? XR_TYPE_IS_RUNE(value->args[1]->type)
              : (XR_TYPE_IS_INT(value->args[1]->type) || XR_TYPE_IS_FLOAT(value->args[1]->type) ||
                 XR_TYPE_IS_BOOL(value->args[1]->type))));
    bool array_intrinsic_exact =
        value->op == XI_CALL_BUILTIN && value->array_intrinsic_kind > XI_ARRAY_INTRINSIC_NONE &&
        value->array_intrinsic_kind < XI_ARRAY_INTRINSIC_COUNT &&
        value->array_element_storage > XR_ELEM_ANY &&
        value->array_element_storage < XR_ELEM_RAWPTR && value->aux_kind == XI_AUX_KIND_NONE &&
        value->type && XR_TYPE_IS_ARRAY(value->type) && value->type->container.element_type &&
        value->nargs ==
            (value->array_intrinsic_kind == XI_ARRAY_INTRINSIC_WITH_CAPACITY ? 1u : 2u) &&
        value->args[0] && value->args[0]->type && XR_TYPE_IS_INT(value->args[0]->type) &&
        array_fill_type_exact;
    if (value->array_intrinsic_kind != XI_ARRAY_INTRINSIC_NONE && !array_intrinsic_exact) {
        if (ctx->error && ctx->error_size)
            snprintf(ctx->error, ctx->error_size,
                     "XR_SEM_0019: Array intrinsic authority is not exact "
                     "kind=%u storage=%u immediate=%lld op=%u nargs=%u result-kind=%u arg0-kind=%u "
                     "arg1-match=%u",
                     value->array_intrinsic_kind, value->array_element_storage,
                     (long long) value->aux_int, value->op, value->nargs,
                     value->type ? value->type->kind : UINT16_MAX,
                     value->nargs && value->args[0] && value->args[0]->type
                         ? value->args[0]->type->kind
                         : UINT16_MAX,
                     array_fill_type_exact);
        return false;
    }
    if (array_intrinsic_exact) {
        record->intrinsic_kind = value->array_intrinsic_kind == XI_ARRAY_INTRINSIC_WITH_CAPACITY
                                     ? XR_SEM_INTRINSIC_ARRAY_WITH_CAPACITY
                                     : XR_SEM_INTRINSIC_ARRAY_FILLED_NEW;
        record->array_element_storage = value->array_element_storage;
    }
    const XrType *array_allocation_element =
        value->type && XR_TYPE_IS_ARRAY(value->type) ? value->type->container.element_type : NULL;
    bool array_allocation_scalar =
        value->array_element_storage > XR_ELEM_ANY && value->array_element_storage < XR_ELEM_RAWPTR;
    bool array_allocation_source_class =
        value->array_element_storage == XR_ELEM_ANY &&
        source_class_for_type(ctx, array_allocation_element) != XR_SEMANTIC_INDEX_NONE;
    bool array_allocation_exact = value->op == XI_ARRAY_NEW && value->type &&
                                  XR_TYPE_IS_ARRAY(value->type) && array_allocation_element &&
                                  value->nargs == 1 && value->args[0] && value->args[0]->type &&
                                  XR_TYPE_IS_INT(value->args[0]->type) &&
                                  (array_allocation_scalar || array_allocation_source_class);
    if (array_allocation_exact) {
        record->array_element_storage = value->array_element_storage;
        record->semantic_immediate = 0;
    }
    bool string_builder_candidate = xi_string_builder_constructor_candidate(value);
    bool string_builder_exact = xi_string_builder_constructor_exact(value);
    if (string_builder_candidate &&
        (!string_builder_exact || !semantic_string_builder_constructor_exact(ctx, record)))
        return fail(ctx, "XR_SEM_0019", "StringBuilder constructor authority is not exact");
    if ((record->effects & XI_EFFECT_ALLOCATES) != 0 ||
        xi_generated_op_escape_alloc(value->op) == XI_GEN_ESCAPE_ALLOC_HEAP ||
        string_builder_exact || array_intrinsic_exact) {
        if (!append_operation_allocation_identity(ctx, record))
            return false;
    }
    return append_call_target(ctx, value, index);
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

static int compare_program_type_bindings(const void *left, const void *right) {
    const XrSemanticProgramTypeBinding *a = (const XrSemanticProgramTypeBinding *) left;
    const XrSemanticProgramTypeBinding *b = (const XrSemanticProgramTypeBinding *) right;
    return a->semantic_type < b->semantic_type ? -1 : a->semantic_type > b->semantic_type ? 1 : 0;
}

static bool build_program_binding_rows(XrSemanticBuildContext *ctx) {
    if (!ctx->program_closure)
        return true;
    uint32_t function_count = xr_program_semantic_closure_function_count(ctx->program_closure);
    uint32_t call_count = xr_program_semantic_closure_call_count(ctx->program_closure);
    uint32_t type_count = xr_program_semantic_closure_type_count(ctx->program_closure);
    uint32_t type_field_count = xr_program_semantic_closure_type_field_count(ctx->program_closure);
    ctx->program_type_bindings = type_count ? (XrSemanticProgramTypeBinding *) xr_calloc(
                                                  type_count, sizeof(*ctx->program_type_bindings))
                                            : NULL;
    ctx->program_type_field_bindings =
        type_field_count ? (XrSemanticProgramTypeFieldBinding *) xr_calloc(
                               type_field_count, sizeof(*ctx->program_type_field_bindings))
                         : NULL;
    ctx->program_function_bindings =
        function_count ? (XrSemanticProgramFunctionBinding *) xr_malloc(
                             (size_t) function_count * sizeof(*ctx->program_function_bindings))
                       : NULL;
    ctx->program_call_bindings =
        call_count ? (XrSemanticProgramCallBinding *) xr_malloc((size_t) call_count *
                                                                sizeof(*ctx->program_call_bindings))
                   : NULL;
    if ((type_count && !ctx->program_type_bindings) ||
        (type_field_count && !ctx->program_type_field_bindings) ||
        (function_count && !ctx->program_function_bindings) ||
        (call_count && !ctx->program_call_bindings))
        return fail(ctx, "XR_EXEC_5003", "program authority row allocation failed");
    for (uint32_t program_row = 0; program_row < type_count; program_row++) {
        const XrProgramSemanticTypeRecord *row =
            xr_program_semantic_closure_type(ctx->program_closure, program_row);
        uint32_t semantic = semantic_type_for_program_row(ctx, program_row);
        if (!row || semantic == XR_SEMANTIC_INDEX_NONE || semantic >= ctx->plan->type_count)
            return fail(ctx, "XR_SEM_0019", "program type binding table is incomplete");
        XrSemanticProgramTypeBinding *binding =
            &ctx->program_type_bindings[ctx->program_type_binding_count++];
        *binding = (XrSemanticProgramTypeBinding) {
            .program_type = row->id,
            .source_class_identity = row->kind == XR_PROGRAM_SEMANTIC_TYPE_LEAF_VALUE_AGGREGATE
                                         ? ctx->plan->types[semantic].source_class_identity
                                         : (XrStableId) {{0}},
            .semantic_type = semantic,
            .program_row = program_row,
            .field_begin = ctx->program_type_field_binding_count,
            .field_count = row->field_count,
            .kind = row->kind,
            .exact_scalar = row->exact_scalar,
            .flags = row->flags,
        };
        for (uint32_t field = 0; field < row->field_count; field++) {
            const XrProgramSemanticTypeFieldRecord *field_row =
                xr_program_semantic_closure_type_field(ctx->program_closure,
                                                       row->field_begin + field);
            uint32_t child_program =
                field_row ? program_type_row_for_id(ctx->program_closure, field_row->field_type)
                          : XI_PSC_ROW_NONE;
            uint32_t child_semantic = child_program != XI_PSC_ROW_NONE
                                          ? semantic_type_for_program_row(ctx, child_program)
                                          : XR_SEMANTIC_INDEX_NONE;
            if (!field_row || child_program == XI_PSC_ROW_NONE ||
                child_semantic == XR_SEMANTIC_INDEX_NONE ||
                ctx->program_type_field_binding_count >= type_field_count)
                return fail(ctx, "XR_SEM_0019", "program type field binding is incomplete");
            ctx->program_type_field_bindings[ctx->program_type_field_binding_count++] =
                (XrSemanticProgramTypeFieldBinding) {
                    .program_owner_type = row->id,
                    .program_field_type = field_row->field_type,
                    .owner_program_row = program_row,
                    .field_program_row = child_program,
                    .semantic_field_type = child_semantic,
                    .declaration_ordinal = field_row->declaration_ordinal,
                };
        }
    }
    if (ctx->program_type_binding_count != type_count ||
        ctx->program_type_field_binding_count != type_field_count)
        return fail(ctx, "XR_SEM_0019", "program type provenance coverage is incomplete");
    if (type_count > 1u)
        qsort(ctx->program_type_bindings, type_count, sizeof(*ctx->program_type_bindings),
              compare_program_type_bindings);
    for (uint32_t i = 0; i < ctx->function_count; i++) {
        const XiFunc *source = ctx->functions[i].source;
        if (source->psc_function_index == XI_PSC_ROW_NONE)
            continue;
        const XrProgramSemanticFunctionRecord *row =
            xr_program_semantic_closure_function(ctx->program_closure, source->psc_function_index);
        if (!row || ctx->program_function_binding_count >= function_count)
            return fail(ctx, "XR_SEM_0019", "program function binding is not canonical");
        if (xr_program_semantic_closure_family(ctx->program_closure) ==
            XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_AGGREGATE_DIRECT_CALL) {
            /* The verified PSC row is the narrow proof that this function
             * returns a
             * headerless pointer-free leaf value.  Project NONE at
             * the program
             * binding owner; do not weaken global instance RC or
             * copy semantics. */
            ctx->plan->functions[i].return_parameter = -1;
            ctx->plan->functions[i].return_provenance = XR_SEM_RETURN_NONE;
        }
        ctx->program_function_bindings[ctx->program_function_binding_count++] =
            (XrSemanticProgramFunctionBinding) {
                .program_function = row->id,
                .semantic_function = i,
                .program_row = source->psc_function_index,
                .flags = row->flags,
            };
    }
    if (ctx->program_function_binding_count != function_count)
        return fail(ctx, "XR_SEM_0019", "program function binding table is incomplete");
    uint32_t operation = 0;
    for (uint32_t f = 0; f < ctx->function_count; f++) {
        const XiFunc *function = ctx->functions[f].source;
        for (uint32_t b = 0; b < function->nblocks; b++) {
            const XiBlock *block = function->blocks[b];
            for (const XiPhi *phi = block ? block->phis : NULL; phi; phi = phi->next) {
                if (operation >= ctx->plan->operation_count ||
                    ctx->plan->operations[operation].result_value !=
                        value_ref(ctx, function, &phi->value))
                    return fail(ctx, "XR_SEM_0019", "program operation rows are not canonical");
                if (phi->value.psc_call_index != XI_PSC_ROW_NONE)
                    return fail(ctx, "XR_SEM_0019", "program call cannot bind a phi row");
                operation++;
            }
            for (uint32_t v = 0; block && v < block->nvalues; v++) {
                const XiValue *value = block->values[v];
                if (!value || operation >= ctx->plan->operation_count ||
                    ctx->plan->operations[operation].result_value !=
                        value_ref(ctx, function, value))
                    return fail(ctx, "XR_SEM_0019", "program operation rows are not canonical");
                if (value->psc_call_index != XI_PSC_ROW_NONE) {
                    const XrProgramSemanticCallRecord *row = xr_program_semantic_closure_call(
                        ctx->program_closure, value->psc_call_index);
                    uint32_t target = XR_SEMANTIC_INDEX_NONE;
                    if (!row || ctx->program_call_binding_count >= call_count ||
                        !program_direct_local_callee(ctx, f, value, &target))
                        return false;
                    ctx->program_call_bindings[ctx->program_call_binding_count++] =
                        (XrSemanticProgramCallBinding) {row->id,
                                                        row->callsite_identity,
                                                        row->caller_function,
                                                        row->callee_function,
                                                        operation,
                                                        value->psc_call_index,
                                                        target,
                                                        0};
                }
                operation++;
            }
        }
    }
    if (operation != ctx->plan->operation_count || ctx->program_call_binding_count != call_count)
        return fail(ctx, "XR_SEM_0019", "program operation binding table is incomplete");
    if (xr_program_semantic_closure_family(ctx->program_closure) ==
        XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_AGGREGATE_DIRECT_CALL) {
        for (uint32_t i = 0; i < ctx->program_function_binding_count; i++) {
            uint32_t semantic = ctx->program_function_bindings[i].semantic_function;
            if (semantic >= ctx->plan->function_count ||
                ctx->plan->functions[semantic].return_provenance != XR_SEM_RETURN_NONE)
                return fail(ctx, "XR_SEM_0019",
                            "leaf function return projection was not installed");
        }
    }
    return true;
}

static bool install_program_provenance(XrSemanticBuildContext *ctx) {
    if (!ctx->program_closure)
        return true;
    if (xr_program_semantic_closure_family(ctx->program_closure) ==
        XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_AGGREGATE_DIRECT_CALL) {
        for (uint32_t i = 0; i < ctx->program_function_binding_count; i++) {
            uint32_t semantic = ctx->program_function_bindings[i].semantic_function;
            if (semantic >= ctx->plan->function_count ||
                ctx->plan->functions[semantic].return_parameter != -1 ||
                ctx->plan->functions[semantic].return_provenance != XR_SEM_RETURN_NONE)
                return fail(ctx, "XR_SEM_0019", "leaf function return projection was not retained");
        }
    }
    XrGenerationClosureId generation =
        xr_program_semantic_closure_generation_id(ctx->program_closure);
    XrSemanticProgramProvenance provenance = {
        .schema = XR_SEMANTIC_PROGRAM_PROVENANCE_SCHEMA_VERSION,
        .program_schema = xr_program_semantic_closure_schema(ctx->program_closure),
        .program_family = xr_program_semantic_closure_family(ctx->program_closure),
        .type_count = ctx->program_type_binding_count,
        .type_field_count = ctx->program_type_field_binding_count,
        .function_count = ctx->program_function_binding_count,
        .call_count = ctx->program_call_binding_count,
        .program_fingerprint = xr_program_semantic_closure_fingerprint(ctx->program_closure),
    };
    memcpy(provenance.generation_identity.bytes, generation.bytes, sizeof(generation.bytes));
    if (!xr_semantic_plan_set_program_provenance(
            ctx->plan, &provenance, ctx->program_type_bindings, ctx->program_type_binding_count,
            ctx->program_type_field_bindings, ctx->program_type_field_binding_count,
            ctx->program_function_bindings, ctx->program_function_binding_count,
            ctx->program_call_bindings, ctx->program_call_binding_count))
        return fail(ctx, "XR_EXEC_5003", "program provenance installation failed");
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

typedef struct XrSemanticEntitySortEntry {
    XrSemanticEntityRecord record;
    uint32_t old_index;
} XrSemanticEntitySortEntry;

static int compare_semantic_entity(const void *left, const void *right) {
    const XrSemanticEntitySortEntry *a = (const XrSemanticEntitySortEntry *) left;
    const XrSemanticEntitySortEntry *b = (const XrSemanticEntitySortEntry *) right;
    int order = xr_stable_id_compare(a->record.id, b->record.id);
    if (order != 0)
        return order;
    order = strcmp(a->record.canonical_key, b->record.canonical_key);
    if (order != 0)
        return order;
    return a->record.kind < b->record.kind ? -1 : a->record.kind != b->record.kind;
}

static bool begin_entity_key(XrSemanticBuildContext *ctx, XrTextBuilder *key, uint16_t kind,
                             uint32_t parent) {
    if (!text_append_format(key, "entity-v1:schema=%u:kind=%u:parent=", XR_SEMANTIC_SCHEMA_VERSION,
                            (unsigned) kind))
        return false;
    if (parent == XR_SEMANTIC_INDEX_NONE)
        return text_append(key, "none");
    if (parent >= ctx->plan->entity_count)
        return false;
    return text_append_stable_id(key, ctx->plan->entities[parent].id);
}

static bool append_entity(XrSemanticBuildContext *ctx, uint16_t kind, uint8_t subject_kind,
                          uint32_t subject, uint32_t ordinal, uint32_t parent, XrTextBuilder *key,
                          uint32_t *out) {
    if (kind >= XR_SEM_ENTITY_KIND_COUNT ||
        !reserve_array((void **) &ctx->plan->entities, &ctx->plan->entity_capacity,
                       ctx->plan->entity_count + 1, sizeof(*ctx->plan->entities),
                       XR_SEMANTIC_MAX_ENTITIES))
        return fail(ctx, "XR_EXEC_5003", "semantic entity budget exhausted");
    uint32_t index = ctx->plan->entity_count;
    XrSemanticEntityRecord *record = &ctx->plan->entities[index];
    memset(record, 0, sizeof(*record));
    record->canonical_key = xr_semantic_plan_copy_string(ctx->plan, key->data);
    XrFingerprint digest;
    if (!record->canonical_key ||
        !xr_stable_id_from_key(record->canonical_key, &record->id, &digest))
        return fail(ctx, "XR_EXEC_5003", "semantic entity identity allocation failed");
    record->parent = parent;
    record->subject = subject;
    record->ordinal = ordinal;
    record->kind = kind;
    record->subject_kind = subject_kind;
    ctx->plan->entity_count++;
    if (out)
        *out = index;
    return true;
}

static uint32_t find_entity(const XrSemanticPlan *plan, uint16_t kind, uint8_t subject_kind,
                            uint32_t subject) {
    for (uint32_t i = 0; i < plan->entity_count; i++) {
        const XrSemanticEntityRecord *entity = &plan->entities[i];
        if (entity->kind == kind && entity->subject_kind == subject_kind &&
            entity->subject == subject)
            return i;
    }
    return XR_SEMANTIC_INDEX_NONE;
}

static const XrType *source_type_for_index(const XrSemanticBuildContext *ctx, uint32_t index) {
    for (uint32_t i = 0; i < ctx->type_count; i++) {
        if (ctx->types[i].index == index)
            return ctx->types[i].source;
    }
    return NULL;
}

static bool build_module_entities(XrSemanticBuildContext *ctx, const XiFunc *root,
                                  uint32_t *package_out, uint32_t *module_out) {
    const char *module_name =
        root->module && root->module->name ? root->module->name : (root->name ? root->name : "");
    const char *module_identity = root->module ? root->module->identity : NULL;
    XrTextBuilder key = {0};
    bool valid = xr_module_identity_valid(module_identity, NULL) &&
                 begin_entity_key(ctx, &key, XR_SEM_ENTITY_PACKAGE, XR_SEMANTIC_INDEX_NONE) &&
                 text_append(&key, ":authority=") && text_append_component(&key, module_identity);
    if (!valid ||
        !append_entity(ctx, XR_SEM_ENTITY_PACKAGE, XR_SEM_ENTITY_SUBJECT_NONE,
                       XR_SEMANTIC_INDEX_NONE, 0, XR_SEMANTIC_INDEX_NONE, &key, package_out)) {
        text_dispose(&key);
        return fail(ctx, "XR_SEM_0019", "semantic package identity is incomplete");
    }
    text_dispose(&key);
    valid = begin_entity_key(ctx, &key, XR_SEM_ENTITY_MODULE, *package_out) &&
            text_append(&key, ":name=") && text_append_component(&key, module_name) &&
            text_append(&key, ":identity=") && text_append_component(&key, module_identity);
    if (!valid || !append_entity(ctx, XR_SEM_ENTITY_MODULE, XR_SEM_ENTITY_SUBJECT_NONE,
                                 XR_SEMANTIC_INDEX_NONE, 0, *package_out, &key, module_out)) {
        text_dispose(&key);
        return fail(ctx, "XR_SEM_0019", "semantic module identity is incomplete");
    }
    text_dispose(&key);
    return true;
}

static bool build_type_entities(XrSemanticBuildContext *ctx, uint32_t module) {
    for (uint32_t i = 0; i < ctx->plan->type_count; i++) {
        const XrSemanticTypeRecord *type = &ctx->plan->types[i];
        XrTextBuilder key = {0};
        bool valid = begin_entity_key(ctx, &key, XR_SEM_ENTITY_TYPE_INSTANTIATION, module) &&
                     text_append(&key, ":type=") && text_append_stable_id(&key, type->id);
        uint32_t type_entity;
        if (!valid ||
            !append_entity(ctx, XR_SEM_ENTITY_TYPE_INSTANTIATION, XR_SEM_ENTITY_SUBJECT_TYPE, i, 0,
                           module, &key, &type_entity)) {
            text_dispose(&key);
            return fail(ctx, "XR_SEM_0019", "type-instantiation identity is incomplete");
        }
        text_dispose(&key);
        if (type->kind != XR_KIND_STRUCT_OBJECT && (type->flags & XR_SEM_TYPE_AGGREGATE_EXACT) == 0)
            continue;
        valid = begin_entity_key(ctx, &key, XR_SEM_ENTITY_SHAPE, type_entity) &&
                text_append(&key, ":type=") && text_append_stable_id(&key, type->id);
        uint32_t shape;
        if (!valid || !append_entity(ctx, XR_SEM_ENTITY_SHAPE, XR_SEM_ENTITY_SUBJECT_TYPE, i, 0,
                                     type_entity, &key, &shape)) {
            text_dispose(&key);
            return fail(ctx, "XR_SEM_0019", "structural-shape identity is incomplete");
        }
        text_dispose(&key);
        const XrProgramSemanticTypeRecord *program_aggregate =
            program_aggregate_for_semantic_type(ctx, i, NULL);
        const XrType *source = source_type_for_index(ctx, i);
        const XiClassData *aggregate =
            !program_aggregate && (type->flags & XR_SEM_TYPE_AGGREGATE_EXACT) != 0
                ? value_aggregate_data_for_type(ctx, i, &source)
                : NULL;
        if ((!program_aggregate && !source) ||
            (source->kind == XR_KIND_STRUCT_OBJECT &&
             (source->object.field_count < 0 ||
              (source->object.field_count > 0 && !source->object.field_names))) ||
            (!program_aggregate && source->kind == XR_KIND_INSTANCE && !aggregate))
            return fail(ctx, "XR_SEM_0019", "structural-shape field facts are unavailable");
        for (uint16_t field = 0; field < type->child_count; field++) {
            uint32_t child = ctx->plan->type_children[type->child_begin + field];
            valid =
                begin_entity_key(ctx, &key, XR_SEM_ENTITY_FIELD, shape) &&
                text_append_format(&key, ":ordinal=%u", field) &&
                (!program_aggregate
                     ? text_append(&key, ":name=") &&
                           text_append_component(&key, source->kind == XR_KIND_STRUCT_OBJECT
                                                           ? source->object.field_names[field]
                                                           : aggregate->instance_field_names[field])
                     : true) &&
                text_append(&key, ":type=") &&
                text_append_stable_id(&key, ctx->plan->types[child].id);
            if (!valid || !append_entity(ctx, XR_SEM_ENTITY_FIELD, XR_SEM_ENTITY_SUBJECT_TYPE, i,
                                         field, shape, &key, NULL)) {
                text_dispose(&key);
                return fail(ctx, "XR_SEM_0019", "structural-field identity is incomplete");
            }
            text_dispose(&key);
        }
    }
    return true;
}

static bool build_function_entities(XrSemanticBuildContext *ctx, uint32_t module) {
    for (uint32_t i = 0; i < ctx->plan->function_count; i++) {
        const XrSemanticFunctionRecord *function = &ctx->plan->functions[i];
        const XiFunc *source = ctx->functions[i].source;
        uint32_t parent = module;
        if (function->parent != XR_SEMANTIC_INDEX_NONE) {
            parent = find_entity(ctx->plan, XR_SEM_ENTITY_FUNCTION, XR_SEM_ENTITY_SUBJECT_FUNCTION,
                                 function->parent);
            if (parent == XR_SEMANTIC_INDEX_NONE)
                return fail(ctx, "XR_SEM_0019", "function parent identity is unavailable");
        }
        XrTextBuilder key = {0};
        bool valid = begin_entity_key(ctx, &key, XR_SEM_ENTITY_DECLARATION, parent) &&
                     text_append(&key, ":function=") && text_append_stable_id(&key, function->id) &&
                     text_append_format(&key, ":evidence=%u", source->xg_body_func_id);
        uint32_t declaration;
        if (!valid || !append_entity(ctx, XR_SEM_ENTITY_DECLARATION, XR_SEM_ENTITY_SUBJECT_FUNCTION,
                                     i, 0, parent, &key, &declaration)) {
            text_dispose(&key);
            return fail(ctx, "XR_SEM_0019", "declaration identity is incomplete");
        }
        text_dispose(&key);
        valid = begin_entity_key(ctx, &key, XR_SEM_ENTITY_FUNCTION, declaration) &&
                text_append(&key, ":function=") && text_append_stable_id(&key, function->id);
        uint32_t function_entity;
        if (!valid || !append_entity(ctx, XR_SEM_ENTITY_FUNCTION, XR_SEM_ENTITY_SUBJECT_FUNCTION, i,
                                     0, declaration, &key, &function_entity)) {
            text_dispose(&key);
            return fail(ctx, "XR_SEM_0019", "function entity identity is incomplete");
        }
        text_dispose(&key);
        if (function->parent != XR_SEMANTIC_INDEX_NONE) {
            valid = begin_entity_key(ctx, &key, XR_SEM_ENTITY_CLOSURE, function_entity) &&
                    text_append(&key, ":function=") && text_append_stable_id(&key, function->id);
            if (!valid || !append_entity(ctx, XR_SEM_ENTITY_CLOSURE, XR_SEM_ENTITY_SUBJECT_FUNCTION,
                                         i, 0, function_entity, &key, NULL)) {
                text_dispose(&key);
                return fail(ctx, "XR_SEM_0019", "closure identity is incomplete");
            }
            text_dispose(&key);
        }
        if (source->is_extern || source->native_callback_kind != XI_NATIVE_CALLBACK_NONE) {
            valid = begin_entity_key(ctx, &key, XR_SEM_ENTITY_NATIVE, function_entity) &&
                    text_append(&key, ":function=") && text_append_stable_id(&key, function->id) &&
                    text_append_format(&key, ":callback=%u:extern=%u",
                                       (unsigned) source->native_callback_kind,
                                       source->is_extern ? 1u : 0u);
            if (!valid || !append_entity(ctx, XR_SEM_ENTITY_NATIVE, XR_SEM_ENTITY_SUBJECT_FUNCTION,
                                         i, 0, function_entity, &key, NULL)) {
                text_dispose(&key);
                return fail(ctx, "XR_SEM_0019", "native declaration identity is incomplete");
            }
            text_dispose(&key);
        }
    }
    return true;
}

static bool build_operation_entities(XrSemanticBuildContext *ctx) {
    for (uint32_t i = 0; i < ctx->plan->operation_count; i++) {
        XrSemanticOperationRecord *operation = &ctx->plan->operations[i];
        uint32_t parent = find_entity(ctx->plan, XR_SEM_ENTITY_FUNCTION,
                                      XR_SEM_ENTITY_SUBJECT_FUNCTION, operation->function);
        if (parent == XR_SEMANTIC_INDEX_NONE)
            return fail(ctx, "XR_SEM_0019", "operation function identity is unavailable");
        XrTextBuilder key = {0};
        bool valid = begin_entity_key(ctx, &key, XR_SEM_ENTITY_OPERATION, parent) &&
                     text_append(&key, ":operation=") && text_append_stable_id(&key, operation->id);
        uint32_t operation_entity;
        if (!valid || !append_entity(ctx, XR_SEM_ENTITY_OPERATION, XR_SEM_ENTITY_SUBJECT_OPERATION,
                                     i, 0, parent, &key, &operation_entity)) {
            text_dispose(&key);
            return fail(ctx, "XR_SEM_0019", "operation entity identity is incomplete");
        }
        text_dispose(&key);
        if (operation->allocation_key) {
            valid = begin_entity_key(ctx, &key, XR_SEM_ENTITY_ALLOCATION, operation_entity) &&
                    text_append(&key, ":allocation=") &&
                    text_append_stable_id(&key, operation->allocation_id);
            if (!valid ||
                !append_entity(ctx, XR_SEM_ENTITY_ALLOCATION, XR_SEM_ENTITY_SUBJECT_OPERATION, i, 0,
                               operation_entity, &key, NULL)) {
                text_dispose(&key);
                return fail(ctx, "XR_SEM_0019", "allocation entity identity is incomplete");
            }
            text_dispose(&key);
        }
        if (operation->source_file) {
            uint32_t discriminator = 1;
            for (uint32_t previous = 0; previous < i; previous++) {
                const XrSemanticOperationRecord *candidate = &ctx->plan->operations[previous];
                if (candidate->source_file &&
                    strcmp(candidate->source_file, operation->source_file) == 0 &&
                    candidate->source_start_line == operation->source_start_line &&
                    candidate->source_start_column == operation->source_start_column &&
                    candidate->source_end_line == operation->source_end_line &&
                    candidate->source_end_column == operation->source_end_column)
                    discriminator++;
            }
            operation->source_discriminator = discriminator;
            valid = begin_entity_key(ctx, &key, XR_SEM_ENTITY_DEBUG_SPAN, operation_entity) &&
                    text_append(&key, ":file=") &&
                    text_append_component(&key, operation->source_file) &&
                    text_append_format(&key, ":start=%u:%u:end=%u:%u:discriminator=%u:operation=",
                                       operation->source_start_line, operation->source_start_column,
                                       operation->source_end_line, operation->source_end_column,
                                       discriminator) &&
                    text_append_stable_id(&key, operation->id);
            if (!valid ||
                !append_entity(ctx, XR_SEM_ENTITY_DEBUG_SPAN, XR_SEM_ENTITY_SUBJECT_OPERATION, i,
                               discriminator, operation_entity, &key, NULL)) {
                text_dispose(&key);
                return fail(ctx, "XR_SEM_0019", "debug-span identity is incomplete");
            }
            text_dispose(&key);
        }
    }
    return true;
}

static bool build_ownership_entities(XrSemanticBuildContext *ctx, uint32_t module) {
    for (uint32_t i = 0; i < ctx->plan->ownership->owner_count; i++) {
        const XrOwnershipOwnerRecord *owner = &ctx->plan->ownership->owners[i];
        uint32_t parent = find_entity(ctx->plan, XR_SEM_ENTITY_FUNCTION,
                                      XR_SEM_ENTITY_SUBJECT_FUNCTION, owner->function);
        XrTextBuilder key = {0};
        bool valid = parent != XR_SEMANTIC_INDEX_NONE &&
                     begin_entity_key(ctx, &key, XR_SEM_ENTITY_OWNER, parent) &&
                     text_append(&key, ":owner=") && text_append_stable_id(&key, owner->id);
        if (!valid || !append_entity(ctx, XR_SEM_ENTITY_OWNER, XR_SEM_ENTITY_SUBJECT_OWNER, i, 0,
                                     parent, &key, NULL)) {
            text_dispose(&key);
            return fail(ctx, "XR_SEM_0019", "ownership entity identity is incomplete");
        }
        text_dispose(&key);
    }
    for (uint32_t i = 0; i < ctx->plan->parameter_count; i++) {
        const XrSemanticParameterRecord *parameter = &ctx->plan->parameters[i];
        if (parameter->ownership != XI_OWN_BORROWED)
            continue;
        uint32_t parent = find_entity(ctx->plan, XR_SEM_ENTITY_FUNCTION,
                                      XR_SEM_ENTITY_SUBJECT_FUNCTION, parameter->function);
        XrTextBuilder key = {0};
        bool valid = parent != XR_SEMANTIC_INDEX_NONE &&
                     begin_entity_key(ctx, &key, XR_SEM_ENTITY_LOAN, parent) &&
                     text_append(&key, ":parameter=") && text_append_stable_id(&key, parameter->id);
        if (!valid || !append_entity(ctx, XR_SEM_ENTITY_LOAN, XR_SEM_ENTITY_SUBJECT_PARAMETER, i,
                                     parameter->ordinal, parent, &key, NULL)) {
            text_dispose(&key);
            return fail(ctx, "XR_SEM_0019", "borrowed-parameter loan identity is incomplete");
        }
        text_dispose(&key);
    }
    for (uint32_t i = 0; i < ctx->plan->capture_count; i++) {
        const XrSemanticCaptureRecord *capture = &ctx->plan->captures[i];
        if (capture->kind != XR_SEM_CAPTURE_BY_IMM_REF)
            continue;
        uint32_t parent = find_entity(ctx->plan, XR_SEM_ENTITY_FUNCTION,
                                      XR_SEM_ENTITY_SUBJECT_FUNCTION, capture->function);
        XrTextBuilder key = {0};
        bool valid = parent != XR_SEMANTIC_INDEX_NONE &&
                     begin_entity_key(ctx, &key, XR_SEM_ENTITY_LOAN, parent) &&
                     text_append(&key, ":capture=") && text_append_stable_id(&key, capture->id);
        if (!valid || !append_entity(ctx, XR_SEM_ENTITY_LOAN, XR_SEM_ENTITY_SUBJECT_CAPTURE, i,
                                     capture->ordinal, parent, &key, NULL)) {
            text_dispose(&key);
            return fail(ctx, "XR_SEM_0019", "borrowed-capture loan identity is incomplete");
        }
        text_dispose(&key);
    }
    for (uint32_t i = 0; i < ctx->plan->operation_count; i++) {
        const XrSemanticOperationRecord *operation = &ctx->plan->operations[i];
        if (operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_BORROWED ||
            operation->result_value == XR_SEMANTIC_INDEX_NONE ||
            operation->result_type >= ctx->plan->type_count ||
            (ctx->plan->types[operation->result_type].flags & XR_SEM_TYPE_REFERENCE_CAPABLE) == 0)
            continue;
        uint32_t operation_entity =
            find_entity(ctx->plan, XR_SEM_ENTITY_OPERATION, XR_SEM_ENTITY_SUBJECT_OPERATION, i);
        uint32_t function_entity = find_entity(ctx->plan, XR_SEM_ENTITY_FUNCTION,
                                               XR_SEM_ENTITY_SUBJECT_FUNCTION, operation->function);
        uint32_t declaration_entity =
            find_entity(ctx->plan, XR_SEM_ENTITY_DECLARATION, XR_SEM_ENTITY_SUBJECT_FUNCTION,
                        operation->function);
        XrTextBuilder key = {0};
        bool valid = operation_entity != XR_SEMANTIC_INDEX_NONE &&
                     function_entity != XR_SEMANTIC_INDEX_NONE &&
                     declaration_entity != XR_SEMANTIC_INDEX_NONE &&
                     ctx->plan->entities[operation_entity].parent == function_entity &&
                     ctx->plan->entities[function_entity].parent == declaration_entity &&
                     begin_entity_key(ctx, &key, XR_SEM_ENTITY_LOAN, operation_entity) &&
                     text_append(&key, ":declaration=") &&
                     text_append_stable_id(&key, ctx->plan->entities[declaration_entity].id) &&
                     text_append(&key, ":function=") &&
                     text_append_stable_id(&key, ctx->plan->functions[operation->function].id) &&
                     text_append(&key, ":operation=") &&
                     text_append_stable_id(&key, operation->id) &&
                     text_append_format(&key, ":ordinal=0:type=") &&
                     text_append_stable_id(&key, ctx->plan->types[operation->result_type].id) &&
                     text_append_format(&key, ":ownership=%u:alias=%d",
                                        (unsigned) operation->result_ownership,
                                        (int) operation->result_alias_operand);
        if (!valid || !append_entity(ctx, XR_SEM_ENTITY_LOAN, XR_SEM_ENTITY_SUBJECT_OPERATION, i, 0,
                                     operation_entity, &key, NULL)) {
            text_dispose(&key);
            return fail(ctx, "XR_SEM_0019", "borrowed-result loan identity is incomplete");
        }
        text_dispose(&key);
    }
    for (uint32_t domain = XR_STORAGE_EXEC_LOCAL; domain <= XR_STORAGE_FOREIGN; domain++) {
        XrTextBuilder key = {0};
        bool valid = begin_entity_key(ctx, &key, XR_SEM_ENTITY_DOMAIN, module) &&
                     text_append_format(&key, ":storage-domain=%u", domain);
        if (!valid ||
            !append_entity(ctx, XR_SEM_ENTITY_DOMAIN, XR_SEM_ENTITY_SUBJECT_STORAGE_DOMAIN, domain,
                           0, module, &key, NULL)) {
            text_dispose(&key);
            return fail(ctx, "XR_SEM_0019", "storage-domain identity is incomplete");
        }
        text_dispose(&key);
    }
    return true;
}

static bool mark_coroutine_value_membership(uint8_t *membership, uint32_t total_values,
                                            uint32_t semantic_value, uint8_t shift,
                                            uint32_t *touched, uint32_t *touched_count,
                                            uint32_t touched_capacity) {
    if (!membership || semantic_value >= total_values || shift > 4u || !touched || !touched_count)
        return false;
    uint8_t previous = membership[semantic_value];
    if (previous == 0) {
        if (*touched_count >= touched_capacity)
            return false;
        touched[(*touched_count)++] = semantic_value;
    }
    uint8_t count = (uint8_t) ((previous >> shift) & 0x3u);
    if (count < 2u)
        count++;
    membership[semantic_value] =
        (uint8_t) ((previous & (uint8_t) ~(0x3u << shift)) | (uint8_t) (count << shift));
    return true;
}

static bool build_coroutine_entities(XrSemanticBuildContext *ctx) {
    uint32_t total_values = 0;
    if (ctx->plan->function_count != 0) {
        const XrSemanticFunctionRecord *last = &ctx->plan->functions[ctx->plan->function_count - 1];
        if (last->value_begin > UINT32_MAX - last->value_count)
            return fail(ctx, "XR_EXEC_5003", "coroutine value identity space is invalid");
        total_values = last->value_begin + last->value_count;
    }
    uint32_t *operation_by_value =
        total_values ? (uint32_t *) xr_malloc((size_t) total_values * sizeof(*operation_by_value))
                     : NULL;
    if (total_values && !operation_by_value)
        return fail(ctx, "XR_EXEC_5003", "coroutine identity map allocation failed");
    uint32_t *state_entity_by_operation =
        ctx->plan->operation_count ? (uint32_t *) xr_malloc((size_t) ctx->plan->operation_count *
                                                            sizeof(*state_entity_by_operation))
                                   : NULL;
    if (ctx->plan->operation_count && !state_entity_by_operation) {
        xr_free(operation_by_value);
        return fail(ctx, "XR_EXEC_5003", "coroutine state index allocation failed");
    }
    for (uint32_t value = 0; value < total_values; value++)
        operation_by_value[value] = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t operation = 0; operation < ctx->plan->operation_count; operation++)
        state_entity_by_operation[operation] = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t operation = 0; operation < ctx->plan->operation_count; operation++) {
        uint32_t value = ctx->plan->operations[operation].result_value;
        if (value >= total_values || operation_by_value[value] != XR_SEMANTIC_INDEX_NONE) {
            xr_free(operation_by_value);
            xr_free(state_entity_by_operation);
            return fail(ctx, "XR_SEM_0019", "coroutine operation identity map is ambiguous");
        }
        operation_by_value[value] = operation;
    }
    for (uint32_t function = 0; function < ctx->plan->function_count; function++) {
        const XiCoroPlan *coro = ctx->functions[function].source->coro_plan;
        if (!coro)
            continue;
        if (coro->nstates > ctx->plan->functions[function].value_count ||
            (coro->nstates > 0 && !coro->points)) {
            xr_free(operation_by_value);
            xr_free(state_entity_by_operation);
            return fail(ctx, "XR_SEM_0019", "coroutine state facts are incomplete");
        }
        uint32_t parent = find_entity(ctx->plan, XR_SEM_ENTITY_FUNCTION,
                                      XR_SEM_ENTITY_SUBJECT_FUNCTION, function);
        for (uint32_t state = 0; state < coro->nstates; state++) {
            const XiCoroSuspendPoint *point = &coro->points[state];
            uint32_t value = point->op ? ctx->plan->functions[function].value_begin + point->op->id
                                       : XR_SEMANTIC_INDEX_NONE;
            uint32_t operation =
                value < total_values ? operation_by_value[value] : XR_SEMANTIC_INDEX_NONE;
            if (parent == XR_SEMANTIC_INDEX_NONE || point->state_id != state + 1 ||
                operation == XR_SEMANTIC_INDEX_NONE ||
                ctx->plan->operations[operation].function != function) {
                xr_free(operation_by_value);
                xr_free(state_entity_by_operation);
                return fail(ctx, "XR_SEM_0019", "coroutine state identity is incomplete");
            }
            XrTextBuilder key = {0};
            bool valid = begin_entity_key(ctx, &key, XR_SEM_ENTITY_COROUTINE_STATE, parent) &&
                         text_append_format(&key, ":state=%u:operation=", point->state_id) &&
                         text_append_stable_id(&key, ctx->plan->operations[operation].id);
            if (!valid ||
                !append_entity(ctx, XR_SEM_ENTITY_COROUTINE_STATE, XR_SEM_ENTITY_SUBJECT_OPERATION,
                               operation, point->state_id, parent, &key,
                               &state_entity_by_operation[operation])) {
                text_dispose(&key);
                xr_free(operation_by_value);
                xr_free(state_entity_by_operation);
                return fail(ctx, "XR_SEM_0019", "coroutine state identity allocation failed");
            }
            text_dispose(&key);
        }
    }

    uint64_t list_count = 0;
    uint32_t touched_capacity = 0;
    for (uint32_t function = 0; function < ctx->plan->function_count; function++) {
        const XiCoroPlan *coro = ctx->functions[function].source->coro_plan;
        if (!coro)
            continue;
        for (uint32_t state = 0; state < coro->nstates; state++) {
            const XiCoroSuspendPoint *point = &coro->points[state];
            if ((point->nlive && !point->live) || (point->nroots && !point->roots) ||
                (point->ndrops && !point->drops) || point->nlive > UINT32_MAX - point->nroots ||
                point->nlive + point->nroots > UINT32_MAX - point->ndrops) {
                xr_free(state_entity_by_operation);
                xr_free(operation_by_value);
                return fail(ctx, "XR_SEM_0019", "coroutine lifecycle source sets are invalid");
            }
            uint32_t rows = point->nlive + point->nroots + point->ndrops;
            list_count += rows;
            if (list_count > UINT32_MAX || rows > touched_capacity)
                touched_capacity = rows;
        }
    }
    if (list_count > UINT32_MAX) {
        xr_free(state_entity_by_operation);
        xr_free(operation_by_value);
        return fail(ctx, "XR_EXEC_5003", "coroutine lifecycle source row budget exhausted");
    }
    XrSemanticGraph graph = {0};
    if (!xr_semantic_graph_build(ctx->plan, &graph, ctx->error, ctx->error_size)) {
        xr_free(state_entity_by_operation);
        xr_free(operation_by_value);
        return false;
    }
    XrSemanticCoroutineLifecycleProjection projection = {0};
    XrSemanticCoroutineLifecycleProjectionStatus projection_status =
        xr_semantic_coroutine_lifecycle_projection_build(ctx->plan, &graph,
                                                         (uint32_t) (list_count / 3u), &projection);
    size_t owner_count_size =
        xr_ownership_certificate_owner_count(xr_semantic_plan_ownership(ctx->plan));
    if (projection_status != XR_SEMANTIC_LIFECYCLE_PROJECTION_OK || owner_count_size > UINT32_MAX ||
        !xr_semantic_lifecycle_work_charge_product(&projection.indexed_work, list_count, 2u) ||
        !xr_semantic_lifecycle_work_charge(&projection.indexed_work, ctx->plan->entity_count) ||
        !xr_semantic_lifecycle_work_charge_product(&projection.indexed_work, total_values, 3u) ||
        !xr_semantic_lifecycle_work_charge(&projection.indexed_work, ctx->plan->operation_count)) {
        xr_semantic_coroutine_lifecycle_projection_dispose(&projection);
        xr_semantic_graph_dispose(&graph);
        xr_free(state_entity_by_operation);
        xr_free(operation_by_value);
        return fail(ctx,
                    projection_status == XR_SEMANTIC_LIFECYCLE_PROJECTION_INVALID ? "XR_SEM_0019"
                                                                                  : "XR_EXEC_5003",
                    "coroutine lifecycle projection budget exhausted");
    }
    uint32_t owner_count = (uint32_t) owner_count_size;
    uint32_t *owner_entities =
        owner_count ? (uint32_t *) xr_malloc((size_t) owner_count * sizeof(*owner_entities)) : NULL;
    uint8_t *membership =
        total_values ? (uint8_t *) xr_calloc(total_values, sizeof(*membership)) : NULL;
    uint32_t *touched = touched_capacity
                            ? (uint32_t *) xr_malloc((size_t) touched_capacity * sizeof(*touched))
                            : NULL;
    if ((owner_count && !owner_entities) || (total_values && !membership) ||
        (touched_capacity && !touched)) {
        xr_free(touched);
        xr_free(membership);
        xr_free(owner_entities);
        xr_semantic_coroutine_lifecycle_projection_dispose(&projection);
        xr_semantic_graph_dispose(&graph);
        xr_free(state_entity_by_operation);
        xr_free(operation_by_value);
        return fail(ctx, "XR_EXEC_5003", "coroutine lifecycle projection allocation failed");
    }
    for (uint32_t owner = 0; owner < owner_count; owner++)
        owner_entities[owner] = XR_SEMANTIC_INDEX_NONE;
    bool lifecycle_ok = true;
    for (uint32_t entity = 0; entity < ctx->plan->entity_count; entity++) {
        const XrSemanticEntityRecord *record = &ctx->plan->entities[entity];
        if (record->kind != XR_SEM_ENTITY_OWNER)
            continue;
        if (record->subject_kind != XR_SEM_ENTITY_SUBJECT_OWNER || record->subject >= owner_count ||
            owner_entities[record->subject] != XR_SEMANTIC_INDEX_NONE) {
            lifecycle_ok =
                fail(ctx, "XR_SEM_0019", "owned String coroutine owner identity is incomplete");
            goto lifecycle_cleanup;
        }
        owner_entities[record->subject] = entity;
    }
    uint32_t projection_cursor = 0;
    for (uint32_t function = 0; lifecycle_ok && function < ctx->plan->function_count; function++) {
        const XiCoroPlan *coro = ctx->functions[function].source->coro_plan;
        if (!coro)
            continue;
        for (uint32_t state = 0; lifecycle_ok && state < coro->nstates; state++) {
            const XiCoroSuspendPoint *point = &coro->points[state];
            uint32_t state_value = point->op
                                       ? ctx->plan->functions[function].value_begin + point->op->id
                                       : XR_SEMANTIC_INDEX_NONE;
            uint32_t state_operation = state_value < total_values ? operation_by_value[state_value]
                                                                  : XR_SEMANTIC_INDEX_NONE;
            uint32_t state_entity = state_operation < ctx->plan->operation_count
                                        ? state_entity_by_operation[state_operation]
                                        : XR_SEMANTIC_INDEX_NONE;
            if (state_entity == XR_SEMANTIC_INDEX_NONE) {
                lifecycle_ok = fail(ctx, "XR_SEM_0019", "coroutine state identity is incomplete");
                break;
            }
            uint32_t touched_count = 0;
            XiValue **sets[] = {point->live, point->roots, point->drops};
            uint32_t counts[] = {point->nlive, point->nroots, point->ndrops};
            const uint8_t shifts[] = {0u, 2u, 4u};
            for (uint32_t set = 0; lifecycle_ok && set < 3u; set++) {
                for (uint32_t i = 0; i < counts[set]; i++) {
                    XiValue *value = sets[set][i];
                    uint32_t semantic_value =
                        value && value->id < ctx->plan->functions[function].value_count
                            ? ctx->plan->functions[function].value_begin + value->id
                            : XR_SEMANTIC_INDEX_NONE;
                    if (!mark_coroutine_value_membership(membership, total_values, semantic_value,
                                                         shifts[set], touched, &touched_count,
                                                         touched_capacity)) {
                        lifecycle_ok =
                            fail(ctx, "XR_SEM_0019", "coroutine lifecycle source sets are invalid");
                        break;
                    }
                }
            }
            if (projection_cursor < projection.count &&
                projection.rows[projection_cursor].state_entity < state_entity) {
                lifecycle_ok = fail(ctx, "XR_SEM_0019",
                                    "owned String coroutine lifecycle state order is invalid");
            }
            while (lifecycle_ok && projection_cursor < projection.count &&
                   projection.rows[projection_cursor].state_entity == state_entity) {
                const XrSemanticCoroutineLifecycleShape *shape =
                    &projection.rows[projection_cursor++];
                if (shape->function != function || shape->logical_state != point->state_id ||
                    shape->producer_value >= total_values ||
                    membership[shape->producer_value] != UINT8_C(0x15) ||
                    shape->producer_operation >= ctx->plan->operation_count ||
                    shape->owner >= owner_count ||
                    owner_entities[shape->owner] == XR_SEMANTIC_INDEX_NONE) {
                    lifecycle_ok = fail(ctx, "XR_SEM_0019",
                                        "owned String coroutine lifecycle facts are incomplete");
                    break;
                }
                XrStableId owner_identity = ctx->plan->entities[owner_entities[shape->owner]].id;
                const uint16_t roles[] = {
                    XR_SEM_ENTITY_COROUTINE_LIVE,
                    XR_SEM_ENTITY_COROUTINE_ROOT,
                    XR_SEM_ENTITY_COROUTINE_DROP,
                };
                for (uint32_t role = 0; role < 3u; role++) {
                    XrTextBuilder key = {0};
                    bool valid =
                        begin_entity_key(ctx, &key, roles[role], state_entity) &&
                        text_append(&key, ":value-operation=") &&
                        text_append_stable_id(
                            &key, ctx->plan->operations[shape->producer_operation].id) &&
                        text_append(&key, ":release=") &&
                        text_append_stable_id(&key,
                                              ctx->plan->operations[shape->release_operation].id) &&
                        text_append(&key, ":owner=") && text_append_stable_id(&key, owner_identity);
                    if (!valid ||
                        !append_entity(ctx, roles[role], XR_SEM_ENTITY_SUBJECT_OPERATION,
                                       shape->producer_operation, 0, state_entity, &key, NULL)) {
                        text_dispose(&key);
                        lifecycle_ok =
                            fail(ctx, "XR_SEM_0019",
                                 "owned String coroutine lifecycle identity allocation failed");
                        break;
                    }
                    text_dispose(&key);
                }
            }
            for (uint32_t i = 0; i < touched_count; i++)
                membership[touched[i]] = 0;
        }
    }
    if (lifecycle_ok && projection_cursor != projection.count)
        lifecycle_ok = fail(ctx, "XR_SEM_0019",
                            "owned String coroutine lifecycle state coverage is incomplete");
lifecycle_cleanup:
    xr_semantic_graph_dispose(&graph);
    xr_free(touched);
    xr_free(membership);
    xr_free(owner_entities);
    xr_semantic_coroutine_lifecycle_projection_dispose(&projection);
    xr_free(state_entity_by_operation);
    xr_free(operation_by_value);
    return lifecycle_ok;
}

static bool canonicalize_entity_table(XrSemanticBuildContext *ctx) {
    uint32_t count = ctx->plan->entity_count;
    XrSemanticEntitySortEntry *entries =
        count ? (XrSemanticEntitySortEntry *) xr_malloc((size_t) count * sizeof(*entries)) : NULL;
    uint32_t *remap = count ? (uint32_t *) xr_malloc((size_t) count * sizeof(*remap)) : NULL;
    if (count && (!entries || !remap)) {
        xr_free(entries);
        xr_free(remap);
        return fail(ctx, "XR_EXEC_5003", "semantic entity canonicalization allocation failed");
    }
    for (uint32_t i = 0; i < count; i++)
        entries[i] = (XrSemanticEntitySortEntry) {ctx->plan->entities[i], i};
    qsort(entries, count, sizeof(*entries), compare_semantic_entity);
    for (uint32_t i = 0; i < count; i++) {
        if (i > 0 && xr_stable_id_equal(entries[i - 1].record.id, entries[i].record.id)) {
            bool different_keys =
                strcmp(entries[i - 1].record.canonical_key, entries[i].record.canonical_key) != 0;
            xr_free(entries);
            xr_free(remap);
            return fail(ctx, "XR_SEM_0003",
                        different_keys ? "semantic entity hash collision has distinct keys"
                                       : "semantic entity identity is duplicated");
        }
        remap[entries[i].old_index] = i;
        ctx->plan->entities[i] = entries[i].record;
    }
    for (uint32_t i = 0; i < count; i++) {
        uint32_t parent = ctx->plan->entities[i].parent;
        if (parent != XR_SEMANTIC_INDEX_NONE)
            ctx->plan->entities[i].parent = remap[parent];
    }
    xr_free(entries);
    xr_free(remap);
    return true;
}

static bool build_semantic_entities(XrSemanticBuildContext *ctx, const XiFunc *root) {
    uint32_t package, module;
    return build_module_entities(ctx, root, &package, &module) &&
           build_type_entities(ctx, module) && build_function_entities(ctx, module) &&
           build_operation_entities(ctx) && build_ownership_entities(ctx, module) &&
           build_coroutine_entities(ctx) && canonicalize_entity_table(ctx);
}

static bool semantic_plan_build_with_dependencies(const XiFunc *root, XiModule *const *dependencies,
                                                  uint32_t dependency_count, XrSemanticPlan **out,
                                                  char *error, size_t error_size) {
    if (out)
        *out = NULL;
    if (!root || !out || root->stage != XI_STAGE_OPTIMIZED) {
        if (error && error_size)
            snprintf(error, error_size,
                     "XR_SEM_0011: SemanticPlan requires an exact Optimized Xi graph");
        return false;
    }
    XrSemanticBuildContext ctx = {0};
    if (!root->module || !xr_module_identity_valid(root->module->identity, NULL)) {
        if (error && error_size)
            snprintf(error, error_size, "XR_SEM_0019: module identity is incomplete");
        return false;
    }
    ctx.module_identity = root->module->identity;
    ctx.error = error;
    ctx.error_size = error_size;
    ctx.dependency_modules = dependencies;
    ctx.dependency_module_count = dependency_count;
    ctx.plan = xr_semantic_plan_create();
    if (!ctx.plan || !prepare_program_authority(&ctx, root) ||
        !collect_functions(&ctx, root, XR_SEMANTIC_INDEX_NONE, 0) ||
        !build_source_classes(&ctx, root) || !collect_semantic_types(&ctx) ||
        !refine_value_aggregate_types(&ctx) || !canonicalize_type_table(&ctx) ||
        !build_function_records(&ctx) || !build_source_methods(&ctx) ||
        !build_capture_records(&ctx) || !prepare_root_shared_store_index(&ctx, root) ||
        !build_source_exports(&ctx, root) || !build_blocks_and_operations(&ctx) ||
        !build_program_binding_rows(&ctx) || !build_semantic_edges(&ctx))
        goto failure;
    XrOwnershipCertificate *ownership = NULL;
    if (!xr_ownership_certificate_build(ctx.plan, &ownership, error, error_size))
        goto failure;
    xr_semantic_plan_set_ownership(ctx.plan, ownership);
    if (!build_semantic_entities(&ctx, root) || !install_program_provenance(&ctx))
        goto failure;
    if (!xr_semantic_plan_freeze(ctx.plan, error, error_size))
        goto failure;
    if (ctx.plan->dependency_count == 0
            ? !xr_semantic_plan_verify(ctx.plan, error, error_size)
            : !xr_semantic_plan_verify_module_set(
                  ctx.plan, (const XrSemanticPlan *const *) ctx.plan->dependency_plans,
                  ctx.plan->dependency_plan_count, error, error_size))
        goto failure;
    if (ctx.program_closure &&
        !xi_program_semantic_plan_verify(root, ctx.plan, ctx.construction_target_profile, error,
                                         error_size))
        goto failure;
    ctx.plan->verified = true;
    ctx.plan->module_set_verified = ctx.plan->dependency_count != 0;
    for (uint32_t i = 0; i < ctx.plan->dependency_plan_count; i++)
        xr_semantic_plan_free(ctx.plan->dependency_plans[i]);
    xr_free(ctx.plan->dependency_plans);
    ctx.plan->dependency_plans = NULL;
    ctx.plan->dependency_plan_count = 0;
    ctx.plan->dependency_plan_capacity = 0;
    xr_free(ctx.types);
    xr_free(ctx.functions);
    xr_free(ctx.root_shared_stores);
    xr_free(ctx.root_shared_store_ambiguous);
    xr_free(ctx.program_type_bindings);
    xr_free(ctx.program_type_field_bindings);
    xr_free(ctx.program_function_bindings);
    xr_free(ctx.program_call_bindings);
    for (uint32_t i = 0; i < ctx.suspendability_count; i++)
        xr_free(ctx.suspendability[i].functions);
    xr_free(ctx.suspendability);
    *out = ctx.plan;
    return true;

failure:
    xr_free(ctx.types);
    xr_free(ctx.functions);
    xr_free(ctx.root_shared_stores);
    xr_free(ctx.root_shared_store_ambiguous);
    xr_free(ctx.program_type_bindings);
    xr_free(ctx.program_type_field_bindings);
    xr_free(ctx.program_function_bindings);
    xr_free(ctx.program_call_bindings);
    for (uint32_t i = 0; i < ctx.suspendability_count; i++)
        xr_free(ctx.suspendability[i].functions);
    xr_free(ctx.suspendability);
    xr_semantic_plan_free(ctx.plan);
    return false;
}

bool xr_semantic_plan_build(const XiFunc *root, XrSemanticPlan **out, char *error,
                            size_t error_size) {
    return semantic_plan_build_with_dependencies(root, NULL, 0, out, error, error_size);
}

static bool plan_tree_is_unattached(const XiFunc *function) {
    if (!function || function->semantic_plan ||
        function->semantic_plan_function_index != XR_SEMANTIC_INDEX_NONE)
        return false;
    for (uint16_t i = 0; i < function->nchildren; i++) {
        if (function->children[i] && !plan_tree_is_unattached(function->children[i]))
            return false;
    }
    return true;
}

static bool plan_tree_has_function_count(const XiFunc *function, uint32_t expected,
                                         uint32_t *count) {
    if (!function || *count >= expected)
        return false;
    (*count)++;
    for (uint16_t i = 0; i < function->nchildren; i++) {
        if (!plan_tree_has_function_count(function->children[i], expected, count))
            return false;
    }
    return true;
}

static void attach_plan_tree(XiFunc *function, XrSemanticPlan *plan, uint32_t *cursor,
                             bool transfer_reference) {
    if (!function)
        return;
    function->semantic_plan = transfer_reference ? plan : xr_semantic_plan_retain(plan);
    function->semantic_plan_function_index = (*cursor)++;
    for (uint16_t i = 0; i < function->nchildren; i++)
        attach_plan_tree(function->children[i], plan, cursor, false);
}

bool xr_semantic_plan_build_and_attach(XiFunc *root, char *error, size_t error_size) {
    return xr_semantic_plan_build_and_attach_module_set(root, NULL, 0, error, error_size);
}

bool xr_semantic_plan_build_and_attach_module_set(XiFunc *root, XiModule *const *dependencies,
                                                  uint32_t dependency_count, char *error,
                                                  size_t error_size) {
    if (!root || !plan_tree_is_unattached(root)) {
        if (error && error_size)
            snprintf(error, error_size,
                     "XR_SEM_0011: Xi graph already has a SemanticPlan attachment");
        return false;
    }
    XrSemanticPlan *plan = NULL;
    if (!semantic_plan_build_with_dependencies(root, dependencies, dependency_count, &plan, error,
                                               error_size))
        return false;
    uint32_t expected = xr_semantic_plan_function_count(plan);
    uint32_t cursor = 0;
    if (!plan_tree_has_function_count(root, expected, &cursor) || cursor != expected) {
        if (error && error_size)
            snprintf(error, error_size,
                     "XR_SEM_0011: Xi tree and SemanticPlan function tables diverged");
        xr_semantic_plan_free(plan);
        return false;
    }
    cursor = 0;
    attach_plan_tree(root, plan, &cursor, true);
    return true;
}
