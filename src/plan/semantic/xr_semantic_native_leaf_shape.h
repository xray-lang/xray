/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_native_leaf_shape.h - Exact direct native leaf call authority
 */

#ifndef XR_SEMANTIC_NATIVE_LEAF_SHAPE_H
#define XR_SEMANTIC_NATIVE_LEAF_SHAPE_H

#include "../../ir/xi.h"
#include "../../ir/xi_ops_gen.h"
#include "xr_semantic_native_module_shape.h"
#include "../../stdlib/xstdlib_metadata.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

static inline bool xr_semantic_native_target_leaf_identity(const XrStdlibDefEntry *entry,
                                                           XrStableId *out) {
    if (!entry || !out || entry->target_leaf <= XR_STDLIB_TARGET_LEAF_NONE ||
        entry->target_leaf >= XR_STDLIB_TARGET_LEAF_COUNT)
        return false;
    char key[512];
    int written =
        snprintf(key, sizeof(key), "stdlib-target-leaf-v1:%u:%s.%s:%s",
                 (unsigned) entry->target_leaf, entry->module, entry->name, entry->signature);
    XrFingerprint digest;
    return written > 0 && (size_t) written < sizeof(key) &&
           xr_stable_id_from_key(key, out, &digest);
}

static inline const XrSemanticOperationRecord *
xr_semantic_unique_operation_for_value(const XrSemanticPlan *plan, uint32_t function,
                                       uint32_t value) {
    const XrSemanticOperationRecord *match = NULL;
    uint32_t count = (uint32_t) xr_semantic_plan_operation_count(plan);
    for (uint32_t i = 0; i < count; i++) {
        const XrSemanticOperationRecord *candidate = xr_semantic_plan_operation(plan, i);
        if (!candidate || candidate->function != function || candidate->result_value != value)
            continue;
        if (match)
            return NULL;
        match = candidate;
    }
    return match;
}

/* Resolve a private native declaration used through the module's shared slot.
 * This is existence authority, independent of whether the registry marks the
 * member yieldable.  The module initializer publishes one grounded member
 * import, a function reads that exact slot, and the call consumes the read as
 * its callee. */
static inline const XrSemanticOperationRecord *
xr_semantic_native_direct_import_for_value(const XrSemanticPlan *plan, uint32_t function,
                                           uint32_t value) {
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(plan);
    for (uint32_t depth = 0; plan && operands && depth < operation_count; depth++) {
        const XrSemanticOperationRecord *producer =
            xr_semantic_unique_operation_for_value(plan, function, value);
        if (!producer)
            return NULL;
        if (producer->opcode == XI_IMPORT_REF)
            return producer;
        if (producer->opcode == XI_COPY && producer->semantic_immediate == XI_COPY_KIND_IDENTITY &&
            producer->operand_count == 1 && producer->operand_begin < operand_count &&
            producer->result_alias_operand == 0) {
            value = operands[producer->operand_begin].value;
            continue;
        }
        if (producer->opcode != XI_GET_SHARED || producer->semantic_immediate < 0 ||
            producer->operand_count != 0)
            return NULL;
        const XrSemanticOperationRecord *store = NULL;
        for (uint32_t i = 0; i < operation_count; i++) {
            const XrSemanticOperationRecord *candidate = xr_semantic_plan_operation(plan, i);
            if (!candidate || candidate->opcode != XI_SET_SHARED || candidate->function != 0 ||
                candidate->semantic_immediate != producer->semantic_immediate)
                continue;
            if (store)
                return NULL;
            store = candidate;
        }
        if (!store || store->operand_count != 1 || store->operand_begin >= operand_count ||
            operands[store->operand_begin].type != producer->result_type)
            return NULL;
        function = 0;
        value = operands[store->operand_begin].value;
    }
    return NULL;
}

static inline bool
xr_semantic_native_direct_scalar_call_shape_is_exact(const XrSemanticPlan *plan,
                                                     const XrSemanticOperationRecord *operation,
                                                     const XrStdlibDefEntry **out_entry) {
    uint32_t operand_count = 0;
    uint32_t metadata_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    const char *const *metadata = xr_semantic_plan_metadata(plan, &metadata_count);
    const XrSemanticTypeRecord *result_type =
        operation ? xr_semantic_plan_type(plan, operation->result_type) : NULL;
    if (!plan || !operation || !operands || !metadata || operation->opcode != XI_CALL ||
        operation->operand_count == 0 || operation->operand_begin >= operand_count ||
        operation->operand_count > operand_count - operation->operand_begin ||
        operation->metadata_count != 0 || operation->semantic_immediate != 0 ||
        operation->auxiliary_kind != XI_AUX_KIND_NONE ||
        (operation->flags & XI_FLAG_MAY_SUSPEND) != 0 ||
        operation->effects != xi_generated_op_effects(XI_CALL) ||
        operation->result_alias_operand != -1 ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_CALL_RESULT ||
        !xr_semantic_native_module_boundary_type_is_exact(result_type, true))
        return false;
    const XrSemanticOperandRecord *callee = &operands[operation->operand_begin];
    if (callee->role != XR_SEM_OPERAND_CALLEE || callee->parameter != -1 || callee->flags != 0 ||
        callee->ownership_action != XR_SEM_OPERAND_BORROW)
        return false;
    const XrSemanticOperationRecord *import =
        xr_semantic_native_direct_import_for_value(plan, operation->function, callee->value);
    if (!import || import->opcode != XI_IMPORT_REF ||
        (import->function != 0 && import->function != operation->function) ||
        import->operand_count != 0 || import->metadata_count != 2 ||
        import->metadata_begin >= metadata_count || import->metadata_begin + 1u >= metadata_count ||
        import->import_resolution != XR_SEM_IMPORT_RESOLUTION_NATIVE_STDLIB ||
        import->intrinsic_kind != XR_SEM_INTRINSIC_NONE ||
        import->auxiliary_kind != XI_AUX_KIND_NONE ||
        import->effects != xi_generated_op_effects(XI_IMPORT_REF) ||
        (import->flags & XI_FLAG_MAY_SUSPEND) != 0 || import->result_type != callee->type)
        return false;
    for (uint16_t i = 1; i < operation->operand_count; i++) {
        const XrSemanticOperandRecord *argument = callee + i;
        if (argument->role != XR_SEM_OPERAND_ARGUMENT ||
            argument->parameter != (int16_t) (i - 1u) ||
            argument->flags != XR_SEM_OPERAND_CALL_CONTRACT ||
            argument->ownership_action != XR_SEM_OPERAND_BORROW ||
            !xr_semantic_native_module_boundary_type_is_exact(
                xr_semantic_plan_type(plan, argument->type), false))
            return false;
    }
    const XrStdlibDefEntry *entry = xr_stdlib_metadata_exact_native_direct_member(
        metadata[import->metadata_begin], metadata[import->metadata_begin + 1u],
        (uint16_t) (operation->operand_count - 1u));
    if (!entry || entry->target_leaf != XR_STDLIB_TARGET_LEAF_NONE)
        return false;
    if (out_entry)
        *out_entry = entry;
    return true;
}

/* A direct private native leaf is a grounded XI_IMPORT_REF used as the callee
 * of XI_CALL. The generated registry must opt the member into one typed leaf
 * kind; ordinary direct members, yieldable members, source imports, and open
 * callables remain outside. The call row carries only scalar arguments and one
 * scalar result, so it creates no ownership or suspension authority. */
static inline bool xr_semantic_native_target_leaf_call_shape_is_exact(
    const XrSemanticPlan *plan, const XrSemanticOperationRecord *operation,
    const XrStdlibDefEntry **out_entry, XrStableId *out_identity) {
    uint32_t operand_count = 0;
    uint32_t metadata_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    const char *const *metadata = xr_semantic_plan_metadata(plan, &metadata_count);
    const XrSemanticTypeRecord *result_type =
        operation ? xr_semantic_plan_type(plan, operation->result_type) : NULL;
    if (!plan || !operation || !operands || !metadata || operation->opcode != XI_CALL ||
        operation->operand_count == 0 || operation->operand_begin >= operand_count ||
        operation->operand_count > operand_count - operation->operand_begin ||
        operation->metadata_count != 0 || operation->semantic_immediate != 0 ||
        operation->auxiliary_kind != XI_AUX_KIND_NONE ||
        (operation->flags & XI_FLAG_MAY_SUSPEND) != 0 ||
        operation->effects != xi_generated_op_effects(XI_CALL) ||
        operation->result_alias_operand != -1 ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_CALL_RESULT || !result_type ||
        result_type->kind != XR_KIND_INT ||
        !xr_semantic_native_module_boundary_type_is_exact(result_type, true))
        return false;

    const XrSemanticOperandRecord *callee = &operands[operation->operand_begin];
    if (callee->role != XR_SEM_OPERAND_CALLEE || callee->parameter != -1 || callee->flags != 0 ||
        callee->ownership_action != XR_SEM_OPERAND_BORROW)
        return false;
    const XrSemanticOperationRecord *import =
        xr_semantic_unique_operation_for_value(plan, operation->function, callee->value);
    if (!import || import->opcode != XI_IMPORT_REF || import->operand_count != 0 ||
        import->metadata_count != 2 || import->metadata_begin >= metadata_count ||
        import->metadata_begin + 1u >= metadata_count ||
        import->import_resolution != XR_SEM_IMPORT_RESOLUTION_NATIVE_STDLIB ||
        import->intrinsic_kind != XR_SEM_INTRINSIC_NONE ||
        import->auxiliary_kind != XI_AUX_KIND_NONE ||
        import->effects != xi_generated_op_effects(XI_IMPORT_REF) ||
        (import->flags & XI_FLAG_MAY_SUSPEND) != 0)
        return false;

    for (uint16_t i = 1; i < operation->operand_count; i++) {
        const XrSemanticOperandRecord *argument = callee + i;
        if (argument->role != XR_SEM_OPERAND_ARGUMENT ||
            argument->parameter != (int16_t) (i - 1u) ||
            argument->flags != XR_SEM_OPERAND_CALL_CONTRACT ||
            argument->ownership_action != XR_SEM_OPERAND_BORROW ||
            !xr_semantic_native_module_boundary_type_is_exact(
                xr_semantic_plan_type(plan, argument->type), false))
            return false;
    }

    const char *module = metadata[import->metadata_begin];
    const char *member = metadata[import->metadata_begin + 1u];
    const XrStdlibDefEntry *entry = xr_stdlib_metadata_exact_native_target_leaf(
        module, member, (uint16_t) (operation->operand_count - 1u));
    XrStableId identity = {{0}};
    if (!entry || !xr_semantic_native_target_leaf_identity(entry, &identity))
        return false;
    if (out_entry)
        *out_entry = entry;
    if (out_identity)
        *out_identity = identity;
    return true;
}

static inline bool xr_semantic_native_target_leaf_call_is_exact(
    const XrSemanticPlan *plan, const XrSemanticOperationRecord *operation,
    const XrStdlibDefEntry **out_entry, XrStableId *out_identity) {
    return operation &&
           operation->intrinsic_kind == XR_SEM_INTRINSIC_NATIVE_TARGET_LEAF_SCALAR_CALL &&
           xr_semantic_native_target_leaf_call_shape_is_exact(plan, operation, out_entry,
                                                              out_identity);
}

static inline bool xr_semantic_native_direct_named_type_matches(const char *module,
                                                                const char *name,
                                                                size_t name_length,
                                                                const XrSemanticTypeRecord *type) {
    bool registered = false;
    for (uint32_t i = 0; i < XR_STDLIB_NATIVE_CLASS_DEF_ENTRY_COUNT; i++) {
        const XrStdlibNativeClassDefEntry *entry = &xr_stdlib_native_class_def_entries[i];
        if (!entry->module || strcmp(entry->module, module) != 0)
            continue;
        bool storage = entry->name && strlen(entry->name) == name_length &&
                       memcmp(entry->name, name, name_length) == 0;
        bool wrapper = entry->source_wrapper && strlen(entry->source_wrapper) == name_length &&
                       memcmp(entry->source_wrapper, name, name_length) == 0;
        if (storage || wrapper) {
            if (registered)
                return false;
            registered = true;
        }
    }
    if (!registered || !type || type->kind != XR_KIND_INSTANCE ||
        type->builtin_type != XR_TID_NULL || type->scalar_rep != XR_SCALAR_REP_NONE ||
        type->child_count != 0 ||
        type->flags != (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT) ||
        !type->canonical_key)
        return false;
    const char *component = strstr(type->canonical_key, ";named:");
    if (!component)
        return false;
    component += strlen(";named:");
    char *end = NULL;
    unsigned long frozen_length = strtoul(component, &end, 10);
    return end && *end == ':' && frozen_length == name_length &&
           memcmp(end + 1, name, name_length) == 0 && end[1 + name_length] == '[' &&
           end[2 + name_length] == '0' && end[3 + name_length] == ']';
}

static inline bool
xr_semantic_native_direct_signature_type_matches(const XrSemanticPlan *plan, uint32_t type_index,
                                                 const char *module, const char *spelling,
                                                 size_t spelling_length, bool result_position) {
    const XrSemanticTypeRecord *type = xr_semantic_plan_type(plan, type_index);
    const XrExactScalarDesc *scalar = xr_exact_scalar_by_source_name(spelling, spelling_length);
    if (scalar)
        return type && type->builtin_type == XR_TID_NULL && type->flags == 0 &&
               type->child_count == 0 && type->scalar_rep == scalar->native_type &&
               ((scalar->family == XR_EXACT_SCALAR_FAMILY_INTEGER && type->kind == XR_KIND_INT) ||
                (scalar->family == XR_EXACT_SCALAR_FAMILY_FLOAT && type->kind == XR_KIND_FLOAT));
    if (spelling_length == 4 && memcmp(spelling, "bool", 4) == 0)
        return type && type->kind == XR_KIND_BOOL && type->builtin_type == XR_TID_NULL &&
               type->flags == 0 && type->child_count == 0 && type->scalar_rep == XR_SCALAR_REP_NONE;
    if (spelling_length == 4 && memcmp(spelling, "rune", 4) == 0)
        return type && type->kind == XR_KIND_RUNE && type->builtin_type == XR_TID_NULL &&
               type->flags == 0 && type->child_count == 0 && type->scalar_rep == XR_SCALAR_REP_NONE;
    if (result_position && spelling_length == 2 && memcmp(spelling, "()", 2) == 0)
        return type && type->kind == XR_KIND_UNIT && type->builtin_type == XR_TID_NULL &&
               type->flags == 0 && type->child_count == 0 && type->scalar_rep == XR_SCALAR_REP_NONE;
    return !result_position &&
           xr_semantic_native_direct_named_type_matches(module, spelling, spelling_length, type);
}

static inline bool xr_semantic_native_direct_function_type_is_exact(
    const XrSemanticPlan *plan, const XrSemanticTypeRecord *function_type,
    const XrSemanticOperationRecord *operation, const XrSemanticOperandRecord *arguments) {
    uint32_t child_count = 0;
    const uint32_t *children = xr_semantic_plan_type_children(plan, &child_count);
    uint32_t arity = operation ? operation->operand_count - 1u : 0u;
    if (!function_type || function_type->kind != XR_KIND_FUNCTION ||
        function_type->builtin_type != XR_TID_NULL ||
        function_type->flags != (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT) ||
        function_type->scalar_rep != XR_SCALAR_REP_NONE ||
        function_type->child_count != arity + 1u || function_type->child_begin > child_count ||
        function_type->child_count > child_count - function_type->child_begin ||
        !function_type->canonical_key || !children)
        return false;
    const char *cursor = strstr(function_type->canonical_key, ":fn:");
    unsigned parameters = 0, minimum = 0, variadic = 1, c_abi = 1, throw_effect = UINT_MAX;
    int consumed = 0;
    if (!cursor ||
        sscanf(cursor, ":fn:%u:%u:%u:%u:%u%n", &parameters, &minimum, &variadic, &c_abi,
               &throw_effect, &consumed) != 5 ||
        parameters != arity || minimum != arity || variadic != 0 || c_abi != 0 ||
        throw_effect != XR_FN_EFFECT_MAY_THROW)
        return false;
    cursor += consumed;
    for (uint32_t ordinal = 0; ordinal < arity; ordinal++) {
        const char prefix[] = ";p0:";
        uint32_t child = children[function_type->child_begin + ordinal];
        const XrSemanticTypeRecord *child_type = xr_semantic_plan_type(plan, child);
        size_t length =
            child_type && child_type->canonical_key ? strlen(child_type->canonical_key) : 0;
        if (!length || strncmp(cursor, prefix, sizeof(prefix) - 1u) != 0 ||
            strncmp(cursor + sizeof(prefix) - 1u, child_type->canonical_key, length) != 0 ||
            arguments[ordinal].type != child || arguments[ordinal].parameter_mode != XR_PARAM_READ)
            return false;
        cursor += sizeof(prefix) - 1u + length;
    }
    const uint32_t result = children[function_type->child_begin + arity];
    const XrSemanticTypeRecord *result_type = xr_semantic_plan_type(plan, result);
    size_t result_length =
        result_type && result_type->canonical_key ? strlen(result_type->canonical_key) : 0;
    return result_length && operation->result_type == result && strncmp(cursor, ";ret:", 5) == 0 &&
           strncmp(cursor + 5, result_type->canonical_key, result_length) == 0 &&
           strcmp(cursor + 5 + result_length, ";view-count:0") == 0;
}

static inline bool xr_semantic_native_direct_signature_is_exact(
    const XrSemanticPlan *plan, const XrStdlibDefEntry *entry,
    const XrSemanticOperationRecord *operation, const XrSemanticOperandRecord *arguments) {
    if (!entry || !entry->signature || !operation || !arguments)
        return false;
    const char *signature = entry->signature;
    const char *close = NULL;
    uint32_t depth = 0;
    for (const char *cursor = signature; *cursor; cursor++) {
        if (*cursor == '(')
            depth++;
        else if (*cursor == ')' && depth != 0 && --depth == 0) {
            close = cursor;
            break;
        }
    }
    if (signature[0] != '(' || !close)
        return false;
    const char *cursor = signature + 1;
    bool reference_argument = false;
    for (uint32_t ordinal = 0; ordinal < entry->argc; ordinal++) {
        while (cursor < close && *cursor == ' ')
            cursor++;
        const char *colon = NULL;
        const char *end = close;
        depth = 0;
        for (const char *scan = cursor; scan < close; scan++) {
            if (*scan == '<' || *scan == '(' || *scan == '[')
                depth++;
            else if ((*scan == '>' || *scan == ')' || *scan == ']') && depth != 0)
                depth--;
            else if (*scan == ':' && depth == 0 && !colon)
                colon = scan;
            else if (*scan == ',' && depth == 0) {
                end = scan;
                break;
            }
        }
        if (!colon || colon >= end)
            return false;
        const char *type = colon + 1;
        while (type < end && *type == ' ')
            type++;
        if ((size_t) (end - type) >= 4u && memcmp(type, "ref ", 4) == 0)
            return false;
        if ((size_t) (end - type) >= 5u && memcmp(type, "move ", 5) == 0)
            return false;
        while (end > type && end[-1] == ' ')
            end--;
        if (memchr(type, '=', (size_t) (end - type)) ||
            !xr_semantic_native_direct_signature_type_matches(
                plan, arguments[ordinal].type, entry->module, type, (size_t) (end - type), false))
            return false;
        const XrSemanticTypeRecord *argument_type =
            xr_semantic_plan_type(plan, arguments[ordinal].type);
        reference_argument =
            reference_argument ||
            (argument_type && (argument_type->flags & XR_SEM_TYPE_REFERENCE_CAPABLE) != 0);
        cursor = end;
        while (cursor < close && *cursor == ' ')
            cursor++;
        if (ordinal + 1u < entry->argc) {
            if (cursor >= close || *cursor != ',')
                return false;
            cursor++;
        }
    }
    while (cursor < close && *cursor == ' ')
        cursor++;
    const char *result = close + 1;
    while (*result == ' ')
        result++;
    if (cursor != close || *result++ != ':')
        return false;
    while (*result == ' ')
        result++;
    const char *result_end = result + strlen(result);
    while (result_end > result && result_end[-1] == ' ')
        result_end--;
    return reference_argument && xr_semantic_native_direct_signature_type_matches(
                                     plan, operation->result_type, entry->module, result,
                                     (size_t) (result_end - result), true);
}

static inline bool xr_semantic_native_direct_identity(const XrStdlibDefEntry *entry,
                                                      XrStableId *out) {
    char key[768];
    XrFingerprint digest;
    int written = entry ? snprintf(key, sizeof(key), "stdlib-native-direct-v1:%s.%s:%s:%s:%s:%u",
                                   entry->module, entry->name, entry->signature, entry->aot,
                                   entry->arg_spec, entry->runtime_capabilities)
                        : -1;
    return out && written > 0 && (size_t) written < sizeof(key) &&
           xr_stable_id_from_key(key, out, &digest);
}

/* A grounded normal stdlib call whose generated direct shim accepts and
 * returns tagged XrValue carriers. This family exists only where at least one
 * argument is reference-capable; scalar-only calls remain owned by the older
 * NATIVE_MODULE_SCALAR family. Every admitted parameter is a READ/PLAIN
 * BORROW-SHARE value, and the registry signature is checked against both the
 * import function type and the call operands. */
static inline bool xr_semantic_native_direct_call_shape_is_exact(
    const XrSemanticPlan *plan, const XrSemanticOperationRecord *operation,
    const XrStdlibDefEntry **out_entry, XrStableId *out_identity) {
    uint32_t operand_count = 0;
    uint32_t metadata_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    const char *const *metadata = xr_semantic_plan_metadata(plan, &metadata_count);
    const XrSemanticTypeRecord *result_type =
        operation ? xr_semantic_plan_type(plan, operation->result_type) : NULL;
    if (!plan || !operation || !operands || !metadata || operation->opcode != XI_CALL ||
        operation->intrinsic_kind != XR_SEM_INTRINSIC_NONE || operation->operand_count == 0 ||
        operation->operand_begin >= operand_count ||
        operation->operand_count > operand_count - operation->operand_begin ||
        operation->metadata_count != 0 || operation->semantic_immediate != 0 ||
        operation->auxiliary_kind != XI_AUX_KIND_NONE ||
        (operation->flags & XI_FLAG_MAY_SUSPEND) != 0 ||
        operation->effects != xi_generated_op_effects(XI_CALL) ||
        operation->result_alias_operand != -1 ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_CALL_RESULT ||
        !xr_semantic_native_module_boundary_type_is_exact(result_type, true))
        return false;

    const XrSemanticOperandRecord *callee = &operands[operation->operand_begin];
    if (callee->role != XR_SEM_OPERAND_CALLEE || callee->parameter != -1 || callee->flags != 0 ||
        callee->ownership_action != XR_SEM_OPERAND_BORROW)
        return false;
    const XrSemanticOperationRecord *import =
        xr_semantic_native_direct_import_for_value(plan, operation->function, callee->value);
    if (!import || import->opcode != XI_IMPORT_REF ||
        (import->function != 0 && import->function != operation->function) ||
        import->operand_count != 0 || import->metadata_count != 2 ||
        import->metadata_begin >= metadata_count || import->metadata_begin + 1u >= metadata_count ||
        import->import_resolution != XR_SEM_IMPORT_RESOLUTION_NATIVE_STDLIB ||
        import->intrinsic_kind != XR_SEM_INTRINSIC_NONE ||
        import->auxiliary_kind != XI_AUX_KIND_NONE ||
        import->effects != xi_generated_op_effects(XI_IMPORT_REF) ||
        (import->flags & XI_FLAG_MAY_SUSPEND) != 0 || import->result_type != callee->type)
        return false;
    for (uint16_t i = 1; i < operation->operand_count; i++) {
        const XrSemanticOperandRecord *argument = callee + i;
        if (argument->role != XR_SEM_OPERAND_ARGUMENT ||
            argument->parameter != (int16_t) (i - 1u) ||
            argument->flags != XR_SEM_OPERAND_CALL_CONTRACT ||
            argument->ownership_action != XR_SEM_OPERAND_BORROW ||
            argument->parameter_mode != XR_PARAM_READ || argument->access != XR_CALL_ARG_PLAIN ||
            argument->origin != XI_PLACE_ORIGIN_NONE ||
            argument->lifetime != XI_PLACE_LIFETIME_NONE ||
            argument->escape != XI_PLACE_ESCAPE_NONE ||
            argument->transfer_mode != XR_TRANSFER_SHARE ||
            !xr_semantic_plan_type(plan, argument->type))
            return false;
    }

    const char *module = metadata[import->metadata_begin];
    const char *member = metadata[import->metadata_begin + 1u];
    const XrStdlibDefEntry *entry = xr_stdlib_metadata_exact_native_direct_call(
        module, member, (uint16_t) (operation->operand_count - 1u));
    const XrSemanticTypeRecord *function_type = xr_semantic_plan_type(plan, callee->type);
    if (!entry ||
        !xr_semantic_native_direct_signature_is_exact(plan, entry, operation, callee + 1u) ||
        !xr_semantic_native_direct_function_type_is_exact(plan, function_type, operation,
                                                          callee + 1u))
        return false;
    XrStableId identity = {{0}};
    if (!xr_semantic_native_direct_identity(entry, &identity))
        return false;
    if (out_entry)
        *out_entry = entry;
    if (out_identity)
        *out_identity = identity;
    return true;
}

#endif /* XR_SEMANTIC_NATIVE_LEAF_SHAPE_H */
