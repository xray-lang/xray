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
#include "../../module/xmodule_identity.h"
#include "xr_semantic_class_shape.h"
#include "xr_semantic_native_module_shape.h"
#include "xr_semantic_owner_transfer_shape.h"
#include "xr_semantic_string_shape.h"
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

static inline const XrStdlibNativeClassDefEntry *
xr_semantic_native_direct_unique_storage(const char *module, const char *name, size_t name_length) {
    return module ? xr_stdlib_metadata_unique_native_class_span(module, strlen(module), name,
                                                                name_length)
                  : NULL;
}

static inline const XrStdlibNativeClassDefEntry *
xr_semantic_native_direct_unique_wrapper(const char *module, const char *name, size_t name_length) {
    const XrStdlibNativeClassDefEntry *match = NULL;
    for (uint32_t i = 0; module && name && i < XR_STDLIB_NATIVE_CLASS_DEF_ENTRY_COUNT; i++) {
        const XrStdlibNativeClassDefEntry *entry = &xr_stdlib_native_class_def_entries[i];
        if (!entry->module || strcmp(entry->module, module) != 0 || !entry->source_wrapper ||
            strlen(entry->source_wrapper) != name_length ||
            memcmp(entry->source_wrapper, name, name_length) != 0)
            continue;
        if (match)
            return NULL;
        match = entry;
    }
    return match;
}

static inline bool xr_semantic_native_direct_named_type_key_is_exact(
    const XrSemanticTypeRecord *type, const char *name, size_t name_length, bool nullable,
    const XrStableId *source_class_identity) {
    if (!type || !type->canonical_key || !name || name_length > INT_MAX)
        return false;
    char source_suffix[sizeof(";source-class:") + XR_STABLE_ID_BYTES * 2u] = {0};
    if (source_class_identity) {
        char identity[XR_STABLE_ID_BYTES * 2u + 1u];
        xr_stable_id_hex(*source_class_identity, identity);
        int suffix_length =
            snprintf(source_suffix, sizeof(source_suffix), ";source-class:%s", identity);
        if (suffix_length <= 0 || (size_t) suffix_length >= sizeof(source_suffix))
            return false;
    }
    char expected[768];
    int length = snprintf(
        expected, sizeof(expected), "type-v3:%u:0:%u:%u:0:0:0:0:0:%u:0:;named:%zu:%.*s[0]%s",
        (unsigned) XR_KIND_INSTANCE, (unsigned) XR_TID_NULL, nullable ? 1u : 0u,
        (unsigned) XR_SCALAR_REP_NONE, name_length, (int) name_length, name, source_suffix);
    return length > 0 && (size_t) length < sizeof(expected) &&
           strcmp(type->canonical_key, expected) == 0;
}

/* Native storage classes are analyzer/VM registry declarations, never Xi
 * source classes.  Their frozen type rows therefore have no source-class id;
 * the authority is the unique generated (module, storage-name) row plus the
 * exact nominal spelling carried by the NATIVE_STDLIB call signature. */
static inline bool xr_semantic_native_direct_storage_type_matches(
    const char *module, const char *name, size_t name_length, const XrSemanticTypeRecord *type,
    bool nullable, const XrStdlibNativeClassDefEntry **out_storage) {
    const XrStdlibNativeClassDefEntry *storage =
        xr_semantic_native_direct_unique_storage(module, name, name_length);
    XrStableId zero = {{0}};
    uint8_t expected_flags = (uint8_t) (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT |
                                        (nullable ? XR_SEM_TYPE_NULLABLE : 0u));
    if (!storage || !storage->native_body_expr || !storage->native_body_expr[0] ||
        !storage->flags || !storage->flags[0] || !type || type->kind != XR_KIND_INSTANCE ||
        type->builtin_type != XR_TID_NULL || type->source_class != XR_SEMANTIC_INDEX_NONE ||
        !xr_stable_id_equal(type->source_class_identity, zero) ||
        type->scalar_rep != XR_SCALAR_REP_NONE || type->child_count != 0 ||
        type->aggregate_extent != 0 || type->aggregate_align != 0 || type->enum_member_count != 0 ||
        type->enum_flags != 0 || type->flags != expected_flags ||
        !xr_semantic_native_direct_named_type_key_is_exact(type, name, name_length, nullable, NULL))
        return false;
    if (out_storage)
        *out_storage = storage;
    return true;
}

/* Source wrappers occupy the ordinary source-class authority domain.  A
 * wrapper name in native metadata is only a bridge declaration; it may match
 * a parameter after its exact source class and canonical stdlib namespace are
 * independently frozen. */
static inline bool
xr_semantic_native_direct_wrapper_type_matches(const XrSemanticPlan *plan, const char *module,
                                               const char *name, size_t name_length,
                                               const XrSemanticTypeRecord *type, bool nullable) {
    const XrStdlibNativeClassDefEntry *storage =
        xr_semantic_native_direct_unique_wrapper(module, name, name_length);
    uint32_t source_class = nullable
                                ? xr_semantic_nullable_class_instance_type_source_class(plan, type)
                                : xr_semantic_class_instance_type_source_class(plan, type);
    const XrSemanticSourceClassRecord *source =
        source_class != XR_SEMANTIC_INDEX_NONE ? xr_semantic_plan_source_class(plan, source_class)
                                               : NULL;
    const char *source_namespace = NULL;
    size_t source_namespace_length = 0;
    if (!storage || !source || !source->name || !source->module_path ||
        strlen(source->name) != name_length || memcmp(source->name, name, name_length) != 0 ||
        !xr_module_identity_stdlib_namespace(source->module_path, &source_namespace,
                                             &source_namespace_length) ||
        source_namespace_length != strlen(module) ||
        memcmp(source_namespace, module, source_namespace_length) != 0 ||
        !xr_semantic_native_direct_named_type_key_is_exact(type, name, name_length, nullable,
                                                           &source->id))
        return false;
    return true;
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
    if (!result_position && spelling_length == 6 && memcmp(spelling, "string", 6) == 0)
        return xr_semantic_tagged_string_type_is_exact(type);
    if (result_position && spelling_length == 2 && memcmp(spelling, "()", 2) == 0)
        return type && type->kind == XR_KIND_UNIT && type->builtin_type == XR_TID_NULL &&
               type->flags == 0 && type->child_count == 0 && type->scalar_rep == XR_SCALAR_REP_NONE;
    if (!result_position)
        return xr_semantic_native_direct_wrapper_type_matches(plan, module, spelling,
                                                              spelling_length, type, false) ||
               xr_semantic_native_direct_storage_type_matches(module, spelling, spelling_length,
                                                              type, false, NULL);
    return spelling_length > 1u && spelling[spelling_length - 1u] == '?' &&
           xr_semantic_native_direct_storage_type_matches(module, spelling, spelling_length - 1u,
                                                          type, true, NULL);
}

typedef enum XrSemanticNativeDirectResultKind {
    XR_SEM_NATIVE_DIRECT_RESULT_INVALID = 0,
    XR_SEM_NATIVE_DIRECT_RESULT_TRIVIAL,
    XR_SEM_NATIVE_DIRECT_RESULT_FRESH_NULLABLE_NATIVE,
} XrSemanticNativeDirectResultKind;

/* A direct provider either returns a scalar/unit with no ownership transfer or
 * a freshly owned nullable native-class carrier.  The latter is deliberately
 * narrower than "any tagged result": the generated registry must state fresh,
 * the signature/type judgement must already have tied the nullable result to
 * one native class, and SemanticPlan must freeze the call's complete owned
 * provenance. */
static inline XrSemanticNativeDirectResultKind
xr_semantic_native_direct_result_kind(const XrSemanticPlan *plan,
                                      const XrSemanticOperationRecord *operation,
                                      const XrStdlibDefEntry *entry) {
    const XrSemanticTypeRecord *type =
        operation ? xr_semantic_plan_type(plan, operation->result_type) : NULL;
    if (!operation || !entry || !entry->return_ownership || operation->return_parameter != -1)
        return XR_SEM_NATIVE_DIRECT_RESULT_INVALID;
    if (xr_semantic_native_module_boundary_type_is_exact(type, true))
        return entry->return_ownership[0] == '\0' &&
                       operation->return_provenance == XR_SEM_RETURN_NONE &&
                       operation->return_complete == 0
                   ? XR_SEM_NATIVE_DIRECT_RESULT_TRIVIAL
                   : XR_SEM_NATIVE_DIRECT_RESULT_INVALID;
    return strcmp(entry->return_ownership, "fresh") == 0 && type &&
                   type->kind == XR_KIND_INSTANCE &&
                   type->flags == (XR_SEM_TYPE_NULLABLE | XR_SEM_TYPE_REFERENCE_CAPABLE |
                                   XR_SEM_TYPE_OWNERSHIP_ROOT) &&
                   operation->return_provenance == XR_SEM_RETURN_OWNED &&
                   operation->return_complete == 1
               ? XR_SEM_NATIVE_DIRECT_RESULT_FRESH_NULLABLE_NATIVE
               : XR_SEM_NATIVE_DIRECT_RESULT_INVALID;
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
    char key[896];
    char native_class_identity[XR_FINGERPRINT_BYTES * 2u + 1u] = {0};
    const XrStdlibNativeClassDefEntry *native_class =
        xr_stdlib_metadata_fresh_result_native_class(entry);
    if (native_class) {
        static const char hex[] = "0123456789abcdef";
        XrFingerprint fingerprint;
        xr_stdlib_metadata_native_class_fingerprint(native_class, &fingerprint);
        for (size_t i = 0; i < sizeof(fingerprint.bytes); i++) {
            native_class_identity[i * 2u] = hex[fingerprint.bytes[i] >> 4u];
            native_class_identity[i * 2u + 1u] = hex[fingerprint.bytes[i] & 15u];
        }
    }
    XrFingerprint digest;
    int written =
        entry
            ? snprintf(key, sizeof(key), "stdlib-native-direct-v2:%s.%s:%s:%s:%s:%s:%u:%s",
                       entry->module, entry->name, entry->signature, entry->aot, entry->arg_spec,
                       entry->return_ownership, entry->runtime_capabilities, native_class_identity)
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
        !result_type)
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
    XrSemanticNativeDirectResultKind result_kind =
        xr_semantic_native_direct_result_kind(plan, operation, entry);
    if (!entry ||
        !xr_semantic_native_direct_signature_is_exact(plan, entry, operation, callee + 1u) ||
        !xr_semantic_native_direct_function_type_is_exact(plan, function_type, operation,
                                                          callee + 1u) ||
        result_kind == XR_SEM_NATIVE_DIRECT_RESULT_INVALID ||
        (result_kind == XR_SEM_NATIVE_DIRECT_RESULT_TRIVIAL &&
         operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_CALL_RESULT) ||
        (result_kind == XR_SEM_NATIVE_DIRECT_RESULT_FRESH_NULLABLE_NATIVE &&
         operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_OWNED))
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

static inline bool
xr_semantic_native_direct_fresh_result_is_exact(const XrSemanticPlan *plan,
                                                const XrSemanticOperationRecord *operation,
                                                const XrStdlibDefEntry **out_entry) {
    const XrStdlibDefEntry *entry = NULL;
    if (!xr_semantic_native_direct_call_shape_is_exact(plan, operation, &entry, NULL) ||
        xr_semantic_native_direct_result_kind(plan, operation, entry) !=
            XR_SEM_NATIVE_DIRECT_RESULT_FRESH_NULLABLE_NATIVE)
        return false;
    if (out_entry)
        *out_entry = entry;
    return true;
}

/* A force unwrap does not change the native object carried by an owned tagged
 * value.  ARC lowers `storage!` to OWNER_FORWARD, so the one admitted
 * cross-type transfer is nullable -> non-null for the exact same generated
 * native-storage row.  Its source must be the fresh native call that supplied
 * the nullable value; an arbitrary nominally similar carrier is not enough. */
static inline bool xr_semantic_native_storage_owner_forward_is_exact(
    const XrSemanticPlan *plan, const XrSemanticOperationRecord *operation,
    uint32_t *source_value_out) {
    const XrSemanticOperandRecord *source =
        xr_semantic_owner_transfer_base_is_exact(plan, operation);
    const XrSemanticOperationRecord *producer =
        source ? xr_semantic_unique_operation_for_value(plan, operation->function, source->value)
               : NULL;
    const XrStdlibDefEntry *producer_entry = NULL;
    if (!source || operation->opcode != XI_OWNER_FORWARD || source->type == operation->result_type ||
        !producer || producer->result_type != source->type ||
        !xr_semantic_native_direct_fresh_result_is_exact(plan, producer, &producer_entry))
        return false;
    const XrStdlibNativeClassDefEntry *storage =
        xr_stdlib_metadata_fresh_result_native_class(producer_entry);
    if (!storage || !storage->module || !storage->name ||
        !xr_semantic_native_direct_storage_type_matches(
            storage->module, storage->name, strlen(storage->name),
            xr_semantic_plan_type(plan, source->type), true, NULL) ||
        !xr_semantic_native_direct_storage_type_matches(
            storage->module, storage->name, strlen(storage->name),
            xr_semantic_plan_type(plan, operation->result_type), false, NULL))
        return false;
    if (source_value_out)
        *source_value_out = source->value;
    return true;
}

static inline bool xr_semantic_owner_transfer_storage_is_exact(
    const XrSemanticPlan *plan, const XrSemanticOperationRecord *operation,
    uint32_t *source_value_out) {
    return xr_semantic_owner_transfer_is_exact(plan, operation, source_value_out) ||
           xr_semantic_native_storage_owner_forward_is_exact(plan, operation, source_value_out);
}

/* A source wrapper is a one-field capability boundary: its only explicit
 * constructor argument is stored into the one field the provider registry
 * names.  The SemanticPlan already freezes the constructor's field store, so
 * the linkage is proved from ordinary operations rather than by adding a
 * provider-specific row to the schema. */
static inline bool xr_semantic_native_storage_constructor_field_link_is_exact(
    const XrSemanticPlan *plan, uint32_t source_class, uint32_t constructor,
    const XrSemanticParameterRecord *storage_parameter,
    const XrStdlibNativeClassDefEntry *storage) {
    const XrSemanticFunctionRecord *function = xr_semantic_plan_function(plan, constructor);
    const XrSemanticParameterRecord *receiver =
        function && function->parameter_count != 0
            ? xr_semantic_plan_parameter(plan, function->parameter_begin)
            : NULL;
    if (!plan || !function || !receiver || !storage_parameter || !storage ||
        !storage->source_storage_field || !storage->source_storage_field[0] ||
        xr_semantic_class_constructor_receiver_source_class(plan, function->parameter_begin) !=
            source_class)
        return false;

    const XrSemanticOperationRecord *field_store = NULL;
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(plan);
    for (uint32_t i = 0; i < operation_count; i++) {
        const XrSemanticOperationRecord *candidate = xr_semantic_plan_operation(plan, i);
        if (!candidate || candidate->function != constructor || candidate->opcode != XI_STORE_FIELD)
            continue;
        if (field_store)
            return false;
        field_store = candidate;
    }
    if (!field_store || field_store->semantic_immediate != 0 ||
        xr_semantic_class_field_store_source_class(plan, field_store) != source_class)
        return false;

    uint32_t metadata_count = 0;
    const char *const *metadata = xr_semantic_plan_metadata(plan, &metadata_count);
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    if (!metadata || field_store->metadata_begin >= metadata_count ||
        field_store->metadata_count != 1 ||
        strcmp(metadata[field_store->metadata_begin], storage->source_storage_field) != 0 ||
        !operands || field_store->operand_begin >= operand_count ||
        operand_count - field_store->operand_begin < 2)
        return false;
    const XrSemanticOperandRecord *field_receiver = &operands[field_store->operand_begin];
    const XrSemanticOperandRecord *stored = &operands[field_store->operand_begin + 1u];
    return field_receiver->value == receiver->value && field_receiver->type == receiver->type &&
           stored->value == storage_parameter->value && stored->type == storage_parameter->type;
}

/* A private native-storage parameter is admitted only on the constructor of
 * the generated source wrapper that owns that storage row.  The wrapper's
 * frozen source-class module identity supplies the stdlib namespace, so an
 * equal private spelling in another module cannot acquire this authority. */
static inline const XrStdlibNativeClassDefEntry *
xr_semantic_native_storage_constructor_parameter_is_exact(const XrSemanticPlan *plan,
                                                          uint32_t parameter_index) {
    const XrSemanticParameterRecord *parameter = xr_semantic_plan_parameter(plan, parameter_index);
    const XrSemanticFunctionRecord *function =
        parameter ? xr_semantic_plan_function(plan, parameter->function) : NULL;
    if (!plan || !parameter || !function || function->parameter_count != 2 ||
        parameter_index != function->parameter_begin + 1u || parameter->ordinal != 1 ||
        parameter->value == XR_SEMANTIC_INDEX_NONE || parameter->mode != XR_PARAM_READ ||
        (parameter->ownership != XI_OWN_OWNED && parameter->ownership != XI_OWN_BORROWED) ||
        parameter->transfer_mode != XR_TRANSFER_SHARE ||
        parameter->flags != XR_SEM_PARAMETER_REQUIRED || parameter->reserved != 0)
        return NULL;

    const XrStdlibNativeClassDefEntry *match = NULL;
    uint32_t matched_source_class = XR_SEMANTIC_INDEX_NONE;
    uint32_t source_class_count = (uint32_t) xr_semantic_plan_source_class_count(plan);
    for (uint32_t class_index = 0; class_index < source_class_count; class_index++) {
        const XrSemanticSourceClassRecord *source_class =
            xr_semantic_plan_source_class(plan, class_index);
        if (!source_class || !source_class->name || !source_class->module_path ||
            xr_semantic_class_constructor_function(plan, class_index) != parameter->function)
            continue;
        const char *module = NULL;
        size_t module_length = 0;
        if (!xr_module_identity_stdlib_namespace(source_class->module_path, &module,
                                                 &module_length))
            continue;
        for (uint32_t i = 0; i < XR_STDLIB_NATIVE_CLASS_DEF_ENTRY_COUNT; i++) {
            const XrStdlibNativeClassDefEntry *storage = &xr_stdlib_native_class_def_entries[i];
            if (!storage->module || !storage->name || !storage->source_wrapper ||
                strlen(storage->module) != module_length ||
                memcmp(storage->module, module, module_length) != 0 ||
                strcmp(storage->source_wrapper, source_class->name) != 0 ||
                !xr_semantic_native_direct_storage_type_matches(
                    storage->module, storage->name, strlen(storage->name),
                    xr_semantic_plan_type(plan, parameter->type), false, NULL))
                continue;
            if (match)
                return NULL;
            match = storage;
            matched_source_class = class_index;
        }
    }
    return match && xr_semantic_native_storage_constructor_field_link_is_exact(
                        plan, matched_source_class, parameter->function, parameter, match)
               ? match
               : NULL;
}

#endif /* XR_SEMANTIC_NATIVE_LEAF_SHAPE_H */
