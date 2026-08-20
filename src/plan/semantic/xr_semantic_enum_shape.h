/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_enum_shape.h - Shared exactness judgement for ADT enum construction
 *
 * KEY CONCEPT:
 *   An ADT enum constructor is identified only by the frozen enum namespace,
 *   the exact source-enum identity carried by the result type, and the ordered
 *   payload contract. Target planning and C emission consume this judgement;
 *   neither layer recovers a constructor from analyzer objects or selector
 *   spelling alone.
 */

#ifndef XR_SEMANTIC_ENUM_SHAPE_H
#define XR_SEMANTIC_ENUM_SHAPE_H

#include "xr_semantic_plan.h"
#include "../../ir/xi.h"
#include "../../ir/xi_ops_gen.h"
#include "../../runtime/value/xtype.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>

typedef struct XrSemanticAdtEnumConstructorShape {
    uint32_t receiver_value;
    uint32_t result_value;
    uint32_t layout_id;
    uint32_t member_ordinal;
    uint32_t payload_operand_begin;
    uint16_t payload_count;
    const char *enum_name;
    const char *member_name;
} XrSemanticAdtEnumConstructorShape;

static inline bool xr_semantic_adt_enum_type_is_exact(const XrSemanticTypeRecord *type) {
    XrStableId zero = {{0}};
    return type && type->kind == XR_KIND_ENUM && type->source_enum_key &&
           !xr_stable_id_equal(type->source_enum_identity, zero) && type->enum_layout_id != 0 &&
           type->enum_member_count != 0 && type->enum_flags == XR_SEM_ENUM_DECLARATION_EXACT &&
           type->reserved_enum == 0 && type->builtin_type == XR_TID_NULL &&
           type->source_class == XR_SEMANTIC_INDEX_NONE && type->scalar_rep == XR_SCALAR_REP_NONE &&
           (type->flags &
            (XR_SEM_TYPE_NULLABLE | XR_SEM_TYPE_BORROW_VIEW | XR_SEM_TYPE_AGGREGATE_EXACT)) == 0 &&
           (type->flags & (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT)) ==
               (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT);
}

/* The payload-free sibling of the judgement above. Its members carry nothing,
 * so the value is the ordinal alone and the enum flags say so; every other
 * field is pinned exactly as the ADT form pins it. Both layers read this one
 * copy: the target builder and the independent verifier each used to carry a
 * byte-identical private one, which is how a rule can be tightened on one side
 * and left behind on the other. */
static inline bool xr_semantic_unit_enum_type_is_exact(const XrSemanticTypeRecord *type) {
    XrStableId zero = {{0}};
    return type && type->kind == XR_KIND_ENUM && type->source_enum_key &&
           !xr_stable_id_equal(type->source_enum_identity, zero) && type->enum_layout_id != 0 &&
           type->enum_member_count != 0 &&
           type->enum_flags == (XR_SEM_ENUM_DECLARATION_EXACT | XR_SEM_ENUM_UNIT) &&
           type->reserved_enum == 0 && type->builtin_type == XR_TID_NULL &&
           type->source_class == XR_SEMANTIC_INDEX_NONE && type->child_count == 0 &&
           type->aggregate_extent == 0 && type->aggregate_align == 0 &&
           type->scalar_rep == XR_SCALAR_REP_NONE && (type->flags & XR_SEM_TYPE_NULLABLE) == 0;
}

/* Whether the generated opcode table's result-void declaration governs where
 * this value is stored. The table is authority on the opcode, not on the type:
 * a unit enum still carries the ordinal a direct-local argument hands to its
 * callee, and its own family binds that. Erasing it to void here would publish
 * a second, contradictory storage fact for one value and leave the two families
 * to disagree at materialization rather than at the judgement. */
static inline bool xr_semantic_operation_result_void_governs_storage(
    const XrSemanticPlan *plan, const XrSemanticOperationRecord *operation, uint32_t semantic_value,
    uint32_t semantic_type, uint32_t semantic_function) {
    return operation && operation->opcode < XI_OP_COUNT &&
           xi_generated_op_result_kind(operation->opcode) == XI_GEN_RESULT_VOID &&
           operation->function == semantic_function && operation->result_value == semantic_value &&
           operation->result_type == semantic_type &&
           !xr_semantic_unit_enum_type_is_exact(xr_semantic_plan_type(plan, semantic_type));
}

static inline bool
xr_semantic_direct_local_adt_enum_result_is_exact(const XrSemanticPlan *plan,
                                                  const XrSemanticOperationRecord *operation,
                                                  const XrSemanticFunctionRecord *callee) {
    return plan && operation && callee &&
           (operation->opcode == XI_CALL || operation->opcode == XI_TAIL_CALL) &&
           operation->result_type == callee->return_type &&
           operation->result_value != XR_SEMANTIC_INDEX_NONE &&
           operation->result_alias_operand == -1 && operation->return_parameter == -1 &&
           operation->return_complete == 1 && operation->return_provenance == XR_SEM_RETURN_OWNED &&
           callee->return_parameter == -1 && callee->return_provenance == XR_SEM_RETURN_OWNED &&
           xr_semantic_adt_enum_type_is_exact(xr_semantic_plan_type(plan, operation->result_type));
}

static inline bool xr_semantic_enum_take_u32(const char *text, uint32_t *out) {
    if (!text || !text[0] || !out)
        return false;
    uint64_t value = 0;
    for (const unsigned char *p = (const unsigned char *) text; *p; p++) {
        if (*p < '0' || *p > '9')
            return false;
        value = value * 10u + (uint64_t) (*p - '0');
        if (value > UINT32_MAX)
            return false;
    }
    *out = (uint32_t) value;
    return true;
}

static inline bool xr_semantic_enum_key_take_u32(const char **cursor, uint32_t *out) {
    if (!cursor || !*cursor || !out || **cursor < '0' || **cursor > '9')
        return false;
    uint64_t value = 0;
    const char *p = *cursor;
    do {
        value = value * 10u + (uint64_t) (*p - '0');
        if (value > UINT32_MAX)
            return false;
        p++;
    } while (*p >= '0' && *p <= '9');
    *cursor = p;
    *out = (uint32_t) value;
    return true;
}

static inline bool xr_semantic_enum_key_take_component(const char **cursor, const char **text,
                                                       size_t *length) {
    uint32_t width = 0;
    if (!cursor || !*cursor || !text || !length || !xr_semantic_enum_key_take_u32(cursor, &width) ||
        **cursor != ':')
        return false;
    (*cursor)++;
    size_t remaining = strlen(*cursor);
    if (width > remaining)
        return false;
    *text = *cursor;
    *length = width;
    *cursor += width;
    return true;
}

static inline const XrSemanticOperationRecord *
xr_semantic_enum_value_definition(const XrSemanticPlan *plan, uint32_t semantic_value) {
    const XrSemanticOperationRecord *match = NULL;
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(plan);
    for (uint32_t i = 0; i < operation_count; i++) {
        const XrSemanticOperationRecord *operation = xr_semantic_plan_operation(plan, i);
        if (!operation || operation->result_value != semantic_value)
            continue;
        if (match)
            return NULL;
        match = operation;
    }
    return match;
}

/* Enum namespaces may be hoisted through the module shared table before a
 * constructor use. Follow only a unique frozen COPY or SET_SHARED/GET_SHARED
 * chain; any second writer, malformed operand, or cycle refuses authority. */
static inline const XrSemanticOperationRecord *
xr_semantic_enum_namespace_definition(const XrSemanticPlan *plan, uint32_t semantic_value) {
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(plan);
    if (!plan || !operands)
        return NULL;
    for (uint32_t depth = 0; depth <= operation_count; depth++) {
        const XrSemanticOperationRecord *definition =
            xr_semantic_enum_value_definition(plan, semantic_value);
        if (!definition)
            return NULL;
        if (definition->opcode == XI_CONST)
            return definition;
        if (definition->opcode == XI_COPY) {
            if (definition->operand_count != 1 || definition->operand_begin >= operand_count)
                return NULL;
            const XrSemanticOperandRecord *source = &operands[definition->operand_begin];
            if (source->role != XR_SEM_OPERAND_VALUE || source->parameter != -1 ||
                source->flags != 0)
                return NULL;
            semantic_value = source->value;
            continue;
        }
        if (definition->opcode != XI_GET_SHARED || definition->operand_count != 0 ||
            definition->semantic_immediate < 0 || definition->semantic_immediate > UINT16_MAX)
            return NULL;
        const XrSemanticOperationRecord *store = NULL;
        for (uint32_t i = 0; i < operation_count; i++) {
            const XrSemanticOperationRecord *candidate = xr_semantic_plan_operation(plan, i);
            if (!candidate || candidate->opcode != XI_SET_SHARED ||
                candidate->semantic_immediate != definition->semantic_immediate)
                continue;
            if (store)
                return NULL;
            store = candidate;
        }
        if (!store || store->operand_count != 1 || store->operand_begin >= operand_count)
            return NULL;
        const XrSemanticOperandRecord *source = &operands[store->operand_begin];
        if (source->role != XR_SEM_OPERAND_VALUE || source->parameter != -1 || source->flags != 0)
            return NULL;
        semantic_value = source->value;
    }
    return NULL;
}

static inline bool
xr_semantic_enum_source_key_matches(const XrSemanticTypeRecord *type, const char *enum_name,
                                    const char *const *metadata, uint32_t metadata_count,
                                    uint32_t member_metadata_begin, uint32_t member_count) {
    /* Built from the schema macro rather than written out, so the prefix
     * cannot drift from the key the builder emits when the schema moves. */
    char prefix[64];
    int prefix_len =
        snprintf(prefix, sizeof(prefix),
                 "source-enum-v1:schema=%u:owner=", (unsigned) XR_SEMANTIC_SCHEMA_VERSION);
    if (prefix_len <= 0 || (size_t) prefix_len >= sizeof(prefix))
        return false;
    if (!type || !type->source_enum_key || !enum_name || !metadata ||
        strncmp(type->source_enum_key, prefix, (size_t) prefix_len) != 0)
        return false;
    const char *cursor = type->source_enum_key + (size_t) prefix_len;
    const char *component = NULL;
    size_t component_length = 0;
    if (!xr_semantic_enum_key_take_component(&cursor, &component, &component_length) ||
        component_length == 0 || strncmp(cursor, ":name=", 6) != 0)
        return false;
    cursor += 6;
    if (!xr_semantic_enum_key_take_component(&cursor, &component, &component_length) ||
        strlen(enum_name) != component_length ||
        memcmp(enum_name, component, component_length) != 0 || strncmp(cursor, ":members=", 9) != 0)
        return false;
    cursor += 9;
    uint32_t key_member_count = 0;
    if (!xr_semantic_enum_key_take_u32(&cursor, &key_member_count) ||
        key_member_count != member_count || member_count != type->enum_member_count)
        return false;
    uint32_t metadata_cursor = member_metadata_begin;
    for (uint32_t i = 0; i < member_count; i++) {
        char marker[32];
        int marker_length = snprintf(marker, sizeof(marker), ":m%u=", i);
        uint32_t ordinal = 0;
        uint32_t payload_count = 0;
        if (metadata_cursor > metadata_count || metadata_count - metadata_cursor < 3u ||
            marker_length <= 0 || (size_t) marker_length >= sizeof(marker) ||
            strncmp(cursor, marker, (size_t) marker_length) != 0 || !metadata[metadata_cursor] ||
            !xr_semantic_enum_take_u32(metadata[metadata_cursor + 1u], &ordinal) || ordinal != i ||
            !xr_semantic_enum_take_u32(metadata[metadata_cursor + 2u], &payload_count))
            return false;
        cursor += marker_length;
        if (!xr_semantic_enum_key_take_component(&cursor, &component, &component_length) ||
            strlen(metadata[metadata_cursor]) != component_length ||
            memcmp(metadata[metadata_cursor], component, component_length) != 0 ||
            strncmp(cursor, ":payloads=", 10) != 0)
            return false;
        cursor += 10;
        uint32_t key_payload_count = 0;
        if (!xr_semantic_enum_key_take_u32(&cursor, &key_payload_count) ||
            key_payload_count != payload_count || payload_count > UINT16_MAX ||
            payload_count > (metadata_count - metadata_cursor - 3u) / 2u)
            return false;
        metadata_cursor += 3u + payload_count * 2u;
    }
    return *cursor == '\0' && metadata_cursor == metadata_count;
}

/* Resolve one payload-bearing enum constructor from immutable SemanticPlan
 * rows. The enum-v2 namespace table and the exact source-enum type identity
 * must describe the same declaration and every payload operand must match the
 * selected member's frozen payload type. */
static inline bool
xr_semantic_adt_enum_constructor_is_exact(const XrSemanticPlan *plan,
                                          const XrSemanticOperationRecord *operation,
                                          XrSemanticAdtEnumConstructorShape *out) {
    if (out)
        memset(out, 0, sizeof(*out));
    uint32_t operand_count = 0;
    uint32_t metadata_count = 0;
    uint32_t constant_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    const char *const *metadata = xr_semantic_plan_metadata(plan, &metadata_count);
    constant_count = (uint32_t) xr_semantic_plan_constant_count(plan);
    const XrSemanticFunctionRecord *function =
        operation ? xr_semantic_plan_function(plan, operation->function) : NULL;
    if (!plan || !operation || !operands || !metadata || !function ||
        operation->opcode != XI_CALL_METHOD || operation->operand_count < 2u ||
        operation->operand_begin > operand_count ||
        operation->operand_count > operand_count - operation->operand_begin ||
        operation->metadata_count != 1 || operation->metadata_begin >= metadata_count ||
        !metadata[operation->metadata_begin] || operation->result_value == XR_SEMANTIC_INDEX_NONE ||
        operation->result_type >= xr_semantic_plan_type_count(plan) ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE ||
        operation->intrinsic_kind != XR_SEM_INTRINSIC_NONE ||
        operation->import_resolution != XR_SEM_IMPORT_RESOLUTION_NONE ||
        operation->effects != xi_generated_op_effects(XI_CALL_METHOD) ||
        operation->flags != xi_generated_op_default_flags(XI_CALL_METHOD) ||
        operation->ownership_use != xi_generated_op_own_use(XI_CALL_METHOD) ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_OWNED ||
        operation->transfer_mode != 0 || operation->parameter_mode != 0 ||
        operation->parameter_ownership != 0 || operation->result_alias_operand != -1 ||
        operation->return_provenance != XR_SEM_RETURN_OWNED || operation->return_parameter != -1 ||
        operation->return_complete != 1 || operation->view_complete != 0 ||
        operation->view_source_operand != -1 || operation->view_source_parameter != -1 ||
        operation->result_value < function->value_begin ||
        operation->result_value >= function->value_begin + function->value_count)
        return false;

    const XrSemanticOperandRecord *receiver = &operands[operation->operand_begin];
    if (receiver->role != XR_SEM_OPERAND_RECEIVER || receiver->parameter != -1 ||
        receiver->transfer_mode != XR_TRANSFER_SHARE ||
        receiver->ownership_action != XR_SEM_OPERAND_BORROW ||
        receiver->parameter_mode != XR_PARAM_READ || receiver->access != XR_CALL_ARG_PLAIN ||
        receiver->origin != 0 || receiver->lifetime != 0 || receiver->escape != 0 ||
        receiver->flags != XR_SEM_OPERAND_CALL_CONTRACT)
        return false;
    const XrSemanticOperationRecord *definition =
        xr_semantic_enum_namespace_definition(plan, receiver->value);
    if (!definition || definition->opcode != XI_CONST ||
        definition->auxiliary_kind != XI_AUX_KIND_ENUM_NAMESPACE ||
        definition->constant >= constant_count || definition->metadata_count < 6u ||
        definition->metadata_begin > metadata_count ||
        definition->metadata_count > metadata_count - definition->metadata_begin)
        return false;
    const XrSemanticConstantRecord *constant =
        xr_semantic_plan_constant(plan, definition->constant);
    const char *const *enum_metadata = &metadata[definition->metadata_begin];
    uint32_t enum_metadata_count = definition->metadata_count;
    uint32_t layout_id = 0;
    uint32_t is_adt = 0;
    uint32_t type_parameter_count = 0;
    if (!enum_metadata[0] || strcmp(enum_metadata[0], "enum-v2") != 0 || !enum_metadata[1] ||
        !xr_semantic_enum_take_u32(enum_metadata[2], &layout_id) ||
        !xr_semantic_enum_take_u32(enum_metadata[3], &is_adt) || is_adt != 1u ||
        !xr_semantic_enum_take_u32(enum_metadata[4], &type_parameter_count) ||
        type_parameter_count > enum_metadata_count - 6u)
        return false;
    uint32_t member_count_index = 5u + type_parameter_count;
    uint32_t member_count = 0;
    if (!xr_semantic_enum_take_u32(enum_metadata[member_count_index], &member_count) ||
        member_count == 0 || member_count > UINT16_MAX)
        return false;
    uint32_t member_metadata_begin = member_count_index + 1u;
    const XrSemanticTypeRecord *result_type = xr_semantic_plan_type(plan, operation->result_type);
    XrStableId zero = {{0}};
    if (!xr_semantic_adt_enum_type_is_exact(result_type) ||
        xr_stable_id_equal(result_type->source_enum_identity, zero) ||
        result_type->enum_layout_id != layout_id ||
        result_type->enum_member_count != member_count || !constant ||
        result_type->reserved_enum != 0 || constant->kind != XR_SEM_CONST_ENUM_NAMESPACE ||
        constant->type != receiver->type || constant->integer != (int64_t) layout_id ||
        !constant->string || strcmp(constant->string, enum_metadata[1]) != 0 ||
        !xr_semantic_enum_source_key_matches(result_type, enum_metadata[1], enum_metadata,
                                             enum_metadata_count, member_metadata_begin,
                                             member_count))
        return false;

    uint32_t cursor = member_metadata_begin;
    uint32_t selected_ordinal = UINT32_MAX;
    uint32_t selected_payload_count = 0;
    uint32_t selected_payload_begin = 0;
    const char *selected_member = NULL;
    for (uint32_t i = 0; i < member_count; i++) {
        uint32_t ordinal = 0;
        uint32_t payload_count = 0;
        if (cursor > enum_metadata_count || enum_metadata_count - cursor < 3u ||
            !enum_metadata[cursor] ||
            !xr_semantic_enum_take_u32(enum_metadata[cursor + 1u], &ordinal) ||
            !xr_semantic_enum_take_u32(enum_metadata[cursor + 2u], &payload_count) ||
            ordinal != i || payload_count > (enum_metadata_count - cursor - 3u) / 2u)
            return false;
        if (strcmp(enum_metadata[cursor], metadata[operation->metadata_begin]) == 0) {
            if (selected_member)
                return false;
            selected_ordinal = ordinal;
            selected_payload_count = payload_count;
            selected_payload_begin = cursor + 3u;
            selected_member = enum_metadata[cursor];
        }
        cursor += 3u + payload_count * 2u;
    }
    if (cursor != enum_metadata_count || !selected_member || selected_payload_count == 0 ||
        selected_payload_count != operation->operand_count - 1u ||
        selected_payload_count > UINT16_MAX)
        return false;
    for (uint32_t i = 0; i < selected_payload_count; i++) {
        const XrSemanticOperandRecord *payload = receiver + 1u + i;
        uint32_t payload_type = 0;
        if (!xr_semantic_enum_take_u32(enum_metadata[selected_payload_begin + i * 2u + 1u],
                                       &payload_type) ||
            payload_type != payload->type || payload->role != XR_SEM_OPERAND_ARGUMENT ||
            payload->parameter != (int16_t) i || payload->transfer_mode != XR_TRANSFER_SHARE ||
            payload->ownership_action != XR_SEM_OPERAND_CONSUME ||
            payload->parameter_mode != XR_PARAM_READ || payload->access != XR_CALL_ARG_PLAIN ||
            payload->origin != 0 || payload->lifetime != 0 || payload->escape != 0 ||
            payload->flags != XR_SEM_OPERAND_CALL_CONTRACT)
            return false;
    }
    if (out) {
        out->receiver_value = receiver->value;
        out->result_value = operation->result_value;
        out->layout_id = layout_id;
        out->member_ordinal = selected_ordinal;
        out->payload_operand_begin = operation->operand_begin + 1u;
        out->payload_count = (uint16_t) selected_payload_count;
        out->enum_name = enum_metadata[1];
        out->member_name = selected_member;
    }
    return true;
}

#endif  // XR_SEMANTIC_ENUM_SHAPE_H
