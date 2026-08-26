/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_verify.c - Independent SemanticPlan verifier
 */

#include "xr_semantic_array_type_shape.h"
#include "../../shared/xr_bigint_literal_core.h"
#include "xr_semantic_verify.h"
#include "xr_semantic_allocation_shape.h"
#include "xr_semantic_array_element_storage_shape.h"
#include "xr_semantic_type_admission_shape.h"
#include "xr_semantic_builtin_identity_shape.h"
#include "xr_semantic_class_shape.h"
#include "xr_semantic_coroutine_lifecycle_shape.h"
#include "xr_semantic_enum_shape.h"
#include "xr_semantic_graph.h"
#include "xr_semantic_ops.h"
#include "xr_semantic_plan_internal.h"
#include "xr_program_semantic_closure.h"
#include "xr_semantic_string_runes_shape.h"
#include "xr_semantic_string_shape.h"
#include "xr_semantic_iterator_rune_has_next_shape.h"
#include "xr_semantic_iterator_rune_next_shape.h"
#include "xr_semantic_map_entry_iterator_shape.h"
#include "xr_semantic_rune_is_whitespace_shape.h"
#include "xr_semantic_string_slice_shape.h"
#include "xr_semantic_native_module_shape.h"
#include "../ownership/xr_ownership_check.h"
#include "../ownership/xr_ownership_certificate_internal.h"
#include "../../base/xmalloc.h"
#include "../../base/xsha256.h"
#include "../../frontend/analyzer/xa_intrinsic_registry.h"
#include "../../ir/xi.h"
#include "../../ir/xi_own.h"
#include "../../ir/xi_ops_gen.h"
#include "../../runtime/value/xtype.h"
#include "../../shared/xr_hash_core.h"
#include "../../shared/xr_exact_scalar_registry.h"
#include "../../stdlib/xstdlib_metadata.h"
#include "xr_semantic_array_member_shape.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool report(char *error, size_t size, const char *code, const char *detail) {
    if (error && size)
        snprintf(error, size, "%s: %s", code, detail);
    return false;
}

/* Setting XRAY_COLLECT_ALL_REFUSALS makes this pass keep checking after a
 * refusal instead of stopping at the first one, printing every refusal it finds
 * on stderr under [refusal-survey]. The plan is rejected either way: a
 * judgement that refused stated no authority, so the plan is not verified
 * whatever the judgements after it answer.
 *
 * The same variable drives the same walk in the TargetPlan and representation
 * passes, so one compile surveys whichever pass a program reaches. Without it
 * here, a module that stops in this pass reported exactly one gap however many
 * it has, and every such module looked like an isolated single-cause refusal
 * purely because nothing downstream of the first refusal was ever asked. That
 * is a measurement artefact, and it biased the census toward whichever pass
 * happened to survey itself.
 *
 * What the walk can cover is bounded by what this pass is. The judgements below
 * divide into two kinds. The structural ones establish that the tables are
 * dense, in range and mutually consistent, and every later judgement indexes
 * those tables on the strength of that; continuing past a structural refusal
 * would have the survey read a plan whose bounds nothing has established, so
 * those keep stopping at the first refusal. The authority ones ask whether a
 * structurally sound plan states the facts a backend needs, are mutually
 * independent, and re-derive their own working state, so the survey continues
 * across them and within their per-operation loops.
 *
 * As in the other two passes, the count is a census carrying a bias, not a
 * proof of independence: a later authority judgement can refuse because an
 * earlier one stated nothing. What it establishes firmly is which judgements a
 * module reaches at all.
 *
 * Unset, the variable is never read on a path that is not already failing, so a
 * plan that verifies pays nothing. */
static bool semantic_survey_enabled(void) {
    return getenv("XRAY_COLLECT_ALL_REFUSALS") != NULL;
}

static void semantic_survey_row(const char *family, const char *detail) {
    fprintf(stderr, "[refusal-survey] family=semantic_%s %s\n", family,
            detail && detail[0] ? detail : "refused without a detail");
}

static bool range_valid(uint32_t begin, uint32_t count, uint32_t limit) {
    return begin <= limit && count <= limit - begin;
}

static bool is_power_of_two_u32(uint32_t value) {
    return value != 0 && (value & (value - 1u)) == 0;
}

static bool semantic_builtin_type_identity_exact(const XrSemanticTypeRecord *type) {
    if (!type || !type->canonical_key)
        return false;
    unsigned key_kind = 0;
    unsigned key_semantic_type = 0;
    unsigned key_builtin_type = 0;
    unsigned nullable = 0, is_const = 0, is_value = 0, is_literal = 0;
    unsigned cycle_candidate = 0, pointer_mutable = 0, scalar_rep = 0;
    size_t alias_length = 0;
    int consumed = 0;
    if (sscanf(type->canonical_key, "type-v3:%u:%u:%u:%u:%u:%u:%u:%u:%u:%u:%zu:%n", &key_kind,
               &key_semantic_type, &key_builtin_type, &nullable, &is_const, &is_value, &is_literal,
               &cycle_candidate, &pointer_mutable, &scalar_rep, &alias_length, &consumed) != 11 ||
        consumed <= 0 || key_kind != type->kind || key_builtin_type != type->builtin_type)
        return false;
    if (type->builtin_type == XR_TID_NULL)
        return true;
    const char *name = xr_semantic_frozen_builtin_name(type->builtin_type);
    uint8_t expected_flags =
        (uint8_t) ((nullable ? XR_SEM_TYPE_NULLABLE : 0u) | (is_const ? XR_SEM_TYPE_CONST : 0u) |
                   (is_value ? XR_SEM_TYPE_VALUE : 0u) | (is_literal ? XR_SEM_TYPE_LITERAL : 0u) |
                   XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT);
    size_t key_length = strlen(type->canonical_key);
    if (!name || type->kind != XR_KIND_INSTANCE || key_semantic_type != 0 || cycle_candidate != 0 ||
        pointer_mutable != 0 || scalar_rep != XR_SCALAR_REP_NONE ||
        type->scalar_rep != scalar_rep || type->aggregate_extent != 0 ||
        type->aggregate_align != 0 || type->flags != expected_flags ||
        alias_length > key_length - (size_t) consumed)
        return false;
    const char *named = type->canonical_key + consumed + alias_length;
    char expected[96];
    int length = snprintf(expected, sizeof(expected), ";named:%zu:%s[%u", strlen(name), name,
                          (unsigned) type->child_count);
    return length > 0 && (size_t) length < sizeof(expected) &&
           strncmp(named, expected, (size_t) length) == 0 && key_length > (size_t) length &&
           type->canonical_key[key_length - 1u] == ']';
}

static bool verify_id(const char *key, XrStableId actual) {
    XrStableId expected;
    XrFingerprint digest;
    return key && xr_stable_id_from_key(key, &expected, &digest) &&
           xr_stable_id_equal(expected, actual);
}

#define XR_ENUM_NOMINAL_HASH_SEED UINT64_C(1469598103934665603)
#define XR_ENUM_NOMINAL_HASH_PRIME UINT64_C(1099511628211)

static uint64_t verifier_enum_hash_byte(uint64_t hash, uint8_t value) {
    return (hash ^ value) * XR_ENUM_NOMINAL_HASH_PRIME;
}

static uint64_t verifier_enum_hash_u32(uint64_t hash, uint32_t value) {
    hash = verifier_enum_hash_byte(hash, (uint8_t) (value >> 24));
    hash = verifier_enum_hash_byte(hash, (uint8_t) (value >> 16));
    hash = verifier_enum_hash_byte(hash, (uint8_t) (value >> 8));
    return verifier_enum_hash_byte(hash, (uint8_t) value);
}

static uint64_t verifier_enum_hash_text(uint64_t hash, const char *text, size_t length) {
    hash = verifier_enum_hash_u32(hash, (uint32_t) length);
    for (size_t i = 0; i < length; i++)
        hash = verifier_enum_hash_byte(hash, (uint8_t) text[i]);
    return hash;
}

static bool verifier_take_u32(const char **cursor, uint32_t *out) {
    const char *p = cursor ? *cursor : NULL;
    uint64_t value = 0;
    if (!p || !out || *p < '0' || *p > '9')
        return false;
    do {
        value = value * 10u + (uint32_t) (*p - '0');
        if (value > UINT32_MAX)
            return false;
        p++;
    } while (*p >= '0' && *p <= '9');
    *cursor = p;
    *out = (uint32_t) value;
    return true;
}

static bool verifier_take_component(const char **cursor, const char **text, size_t *length) {
    uint32_t width = 0;
    if (!verifier_take_u32(cursor, &width) || **cursor != ':')
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

static bool semantic_source_enum_identity_exact(const XrSemanticTypeRecord *type) {
    XrStableId zero = {{0}};
    if (!type)
        return false;
    if (type->kind != XR_KIND_ENUM)
        return !type->source_enum_key && xr_stable_id_equal(type->source_enum_identity, zero) &&
               type->enum_layout_id == 0 && type->enum_member_count == 0 && type->enum_flags == 0 &&
               type->reserved_enum == 0;
    if (!type->source_enum_key)
        return xr_stable_id_equal(type->source_enum_identity, zero) && type->enum_layout_id == 0 &&
               type->enum_member_count == 0 && type->enum_flags == 0 && type->reserved_enum == 0;
    const char *cursor = type->source_enum_key;
    /* Built from the schema macro rather than written out, so the prefix
     * cannot drift from the key the builder emits when the schema moves. */
    char prefix[64];
    int prefix_len =
        snprintf(prefix, sizeof(prefix),
                 "source-enum-v1:schema=%u:owner=", (unsigned) XR_SEMANTIC_SCHEMA_VERSION);
    if (prefix_len <= 0 || (size_t) prefix_len >= sizeof(prefix))
        return false;
    if (!cursor || strncmp(cursor, prefix, (size_t) prefix_len) != 0 ||
        !verify_id(cursor, type->source_enum_identity) || type->enum_member_count == 0 ||
        type->enum_layout_id == 0 || type->reserved_enum != 0 ||
        (type->enum_flags & (uint8_t) ~(XR_SEM_ENUM_DECLARATION_EXACT | XR_SEM_ENUM_UNIT)) != 0 ||
        (type->enum_flags & XR_SEM_ENUM_DECLARATION_EXACT) == 0)
        return false;
    cursor += (size_t) prefix_len;
    const char *owner = NULL, *name = NULL;
    size_t owner_length = 0, name_length = 0;
    if (!verifier_take_component(&cursor, &owner, &owner_length) || owner_length == 0 ||
        strncmp(cursor, ":name=", 6) != 0)
        return false;
    cursor += 6;
    if (!verifier_take_component(&cursor, &name, &name_length) || name_length == 0 ||
        strncmp(cursor, ":members=", 9) != 0)
        return false;
    cursor += 9;
    uint32_t member_count = 0;
    if (!verifier_take_u32(&cursor, &member_count) || member_count != type->enum_member_count)
        return false;
    uint64_t hash = XR_ENUM_NOMINAL_HASH_SEED;
    hash = verifier_enum_hash_text(hash, "xray.enum.nominal.v1", strlen("xray.enum.nominal.v1"));
    hash = verifier_enum_hash_text(hash, owner, owner_length);
    hash = verifier_enum_hash_text(hash, name, name_length);
    hash = verifier_enum_hash_u32(hash, member_count);
    bool unit = true;
    for (uint32_t i = 0; i < member_count; i++) {
        char marker[32];
        int marker_length = snprintf(marker, sizeof(marker), ":m%u=", i);
        const char *member = NULL;
        size_t member_length = 0;
        uint32_t payload_count = 0;
        if (marker_length <= 0 || (size_t) marker_length >= sizeof(marker) ||
            strncmp(cursor, marker, (size_t) marker_length) != 0)
            return false;
        cursor += marker_length;
        if (!verifier_take_component(&cursor, &member, &member_length) || member_length == 0 ||
            strncmp(cursor, ":payloads=", 10) != 0)
            return false;
        cursor += 10;
        if (!verifier_take_u32(&cursor, &payload_count) || payload_count > UINT16_MAX)
            return false;
        hash = verifier_enum_hash_text(hash, member, member_length);
        hash = verifier_enum_hash_u32(hash, payload_count);
        unit = unit && payload_count == 0;
    }
    if (*cursor != '\0' || (((type->enum_flags & XR_SEM_ENUM_UNIT) != 0) != unit) ||
        type->enum_layout_id != (((uint32_t) xr_hash_core_mix_u64(hash)) | UINT32_C(0x80000000)))
        return false;
    char enum_prefix[256];
    int enum_prefix_length =
        snprintf(enum_prefix, sizeof(enum_prefix), ";enum:%zu:%.*s:%u:", name_length,
                 (int) name_length, name, type->enum_layout_id);
    char enum_suffix[64];
    char enum_id[XR_STABLE_ID_BYTES * 2 + 1];
    xr_stable_id_hex(type->source_enum_identity, enum_id);
    int enum_suffix_length = snprintf(enum_suffix, sizeof(enum_suffix), ";source-enum:%s", enum_id);
    size_t type_key_length = type->canonical_key ? strlen(type->canonical_key) : 0;
    return enum_prefix_length > 0 && (size_t) enum_prefix_length < sizeof(enum_prefix) &&
           enum_suffix_length > 0 && (size_t) enum_suffix_length < sizeof(enum_suffix) &&
           strstr(type->canonical_key, enum_prefix) != NULL &&
           type_key_length >= (size_t) enum_suffix_length &&
           strcmp(type->canonical_key + type_key_length - (size_t) enum_suffix_length,
                  enum_suffix) == 0;
}

typedef struct XrEntityCoverage {
    uint8_t *types;
    uint8_t *shapes;
    uint8_t *fields;
    uint8_t *functions;
    uint8_t *declarations;
    uint8_t *closures;
    uint8_t *natives;
    uint8_t *operations;
    uint8_t *allocations;
    uint8_t *debug_spans;
    uint8_t *coroutine_states;
    uint8_t *owners;
    uint8_t *parameter_loans;
    uint8_t *capture_loans;
    uint8_t *operation_loans;
    uint8_t domains[XR_STORAGE_FOREIGN + 1];
    uint32_t packages;
    uint32_t modules;
} XrEntityCoverage;

static void entity_coverage_dispose(XrEntityCoverage *coverage) {
    xr_free(coverage->types);
    xr_free(coverage->shapes);
    xr_free(coverage->fields);
    xr_free(coverage->functions);
    xr_free(coverage->declarations);
    xr_free(coverage->closures);
    xr_free(coverage->natives);
    xr_free(coverage->operations);
    xr_free(coverage->allocations);
    xr_free(coverage->debug_spans);
    xr_free(coverage->coroutine_states);
    xr_free(coverage->owners);
    xr_free(coverage->parameter_loans);
    xr_free(coverage->capture_loans);
    xr_free(coverage->operation_loans);
}

static bool entity_coverage_init(const XrSemanticPlan *plan, XrEntityCoverage *coverage) {
#define XR_ENTITY_COVERAGE_ALLOC(field, count)                                                     \
    do {                                                                                           \
        if ((count) != 0) {                                                                        \
            coverage->field = (uint8_t *) xr_calloc((count), 1);                                   \
            if (!coverage->field)                                                                  \
                goto failure;                                                                      \
        }                                                                                          \
    } while (0)
    XR_ENTITY_COVERAGE_ALLOC(types, plan->type_count);
    XR_ENTITY_COVERAGE_ALLOC(shapes, plan->type_count);
    XR_ENTITY_COVERAGE_ALLOC(fields, plan->type_child_count);
    XR_ENTITY_COVERAGE_ALLOC(functions, plan->function_count);
    XR_ENTITY_COVERAGE_ALLOC(declarations, plan->function_count);
    XR_ENTITY_COVERAGE_ALLOC(closures, plan->function_count);
    XR_ENTITY_COVERAGE_ALLOC(natives, plan->function_count);
    XR_ENTITY_COVERAGE_ALLOC(operations, plan->operation_count);
    XR_ENTITY_COVERAGE_ALLOC(allocations, plan->operation_count);
    XR_ENTITY_COVERAGE_ALLOC(debug_spans, plan->operation_count);
    XR_ENTITY_COVERAGE_ALLOC(coroutine_states, plan->operation_count);
    XR_ENTITY_COVERAGE_ALLOC(owners, plan->ownership->owner_count);
    XR_ENTITY_COVERAGE_ALLOC(parameter_loans, plan->parameter_count);
    XR_ENTITY_COVERAGE_ALLOC(capture_loans, plan->capture_count);
    XR_ENTITY_COVERAGE_ALLOC(operation_loans, plan->operation_count);
#undef XR_ENTITY_COVERAGE_ALLOC
    return true;
failure:
    entity_coverage_dispose(coverage);
    return false;
}

static bool mark_entity(uint8_t *coverage, uint32_t count, uint32_t index) {
    if (index >= count || coverage[index] != 0)
        return false;
    coverage[index] = 1;
    return true;
}

static bool borrowed_result_has_loan(const XrSemanticPlan *plan, uint32_t operation) {
    if (operation >= plan->operation_count)
        return false;
    const XrSemanticOperationRecord *record = &plan->operations[operation];
    return record->result_ownership == XR_SEM_RESULT_OWNERSHIP_BORROWED &&
           record->result_value != XR_SEMANTIC_INDEX_NONE &&
           record->result_type < plan->type_count &&
           (plan->types[record->result_type].flags & XR_SEM_TYPE_REFERENCE_CAPABLE) != 0;
}

static bool source_file_is_canonical(const char *file) {
    return file && file[0] != '\0' && !(file[0] == '.' && file[1] == '/') &&
           strchr(file, '\\') == NULL;
}

static bool same_debug_source_span(const XrSemanticOperationRecord *left,
                                   const XrSemanticOperationRecord *right) {
    return left->source_file && right->source_file &&
           strcmp(left->source_file, right->source_file) == 0 &&
           left->source_start_line == right->source_start_line &&
           left->source_start_column == right->source_start_column &&
           left->source_end_line == right->source_end_line &&
           left->source_end_column == right->source_end_column;
}

static uint32_t expected_debug_discriminator(const XrSemanticPlan *plan, uint32_t operation) {
    uint32_t discriminator = 1;
    for (uint32_t i = 0; i < operation; i++) {
        if (same_debug_source_span(&plan->operations[i], &plan->operations[operation]))
            discriminator++;
    }
    return discriminator;
}

static bool operation_debug_span_valid(const XrSemanticPlan *plan, uint32_t operation) {
    const XrSemanticOperationRecord *record = &plan->operations[operation];
    if (!record->source_file)
        return record->source_start_line == 0 && record->source_start_column == 0 &&
               record->source_end_line == 0 && record->source_end_column == 0 &&
               record->source_discriminator == 0;
    return source_file_is_canonical(record->source_file) && record->source_start_line != 0 &&
           record->source_start_column != 0 && record->source_end_line != 0 &&
           record->source_end_column != 0 && record->source_end_line >= record->source_start_line &&
           (record->source_end_line != record->source_start_line ||
            record->source_end_column >= record->source_start_column) &&
           record->source_discriminator == expected_debug_discriminator(plan, operation);
}

static bool verify_debug_span_entity_key(const XrSemanticPlan *plan,
                                         const XrSemanticEntityRecord *entity) {
    if (!entity || entity->subject >= plan->operation_count ||
        entity->parent >= plan->entity_count || !operation_debug_span_valid(plan, entity->subject))
        return false;
    const XrSemanticOperationRecord *operation = &plan->operations[entity->subject];
    const XrSemanticEntityRecord *operation_entity = &plan->entities[entity->parent];
    if (!operation->source_file || operation_entity->kind != XR_SEM_ENTITY_OPERATION ||
        operation_entity->subject != entity->subject ||
        entity->ordinal != operation->source_discriminator)
        return false;
    char parent_id[XR_STABLE_ID_BYTES * 2 + 1];
    char operation_id[XR_STABLE_ID_BYTES * 2 + 1];
    xr_stable_id_hex(operation_entity->id, parent_id);
    xr_stable_id_hex(operation->id, operation_id);
    int required = snprintf(NULL, 0,
                            "entity-v1:schema=%u:kind=%u:parent=%s:file=%zu:%s:"
                            "start=%u:%u:end=%u:%u:discriminator=%u:operation=%s",
                            XR_SEMANTIC_SCHEMA_VERSION, (unsigned) XR_SEM_ENTITY_DEBUG_SPAN,
                            parent_id, strlen(operation->source_file), operation->source_file,
                            operation->source_start_line, operation->source_start_column,
                            operation->source_end_line, operation->source_end_column,
                            operation->source_discriminator, operation_id);
    if (required < 0 || (size_t) required > 1048576u)
        return false;
    char *expected = (char *) xr_malloc((size_t) required + 1u);
    if (!expected)
        return false;
    int written = snprintf(expected, (size_t) required + 1u,
                           "entity-v1:schema=%u:kind=%u:parent=%s:file=%zu:%s:"
                           "start=%u:%u:end=%u:%u:discriminator=%u:operation=%s",
                           XR_SEMANTIC_SCHEMA_VERSION, (unsigned) XR_SEM_ENTITY_DEBUG_SPAN,
                           parent_id, strlen(operation->source_file), operation->source_file,
                           operation->source_start_line, operation->source_start_column,
                           operation->source_end_line, operation->source_end_column,
                           operation->source_discriminator, operation_id);
    bool valid = written == required && strcmp(entity->canonical_key, expected) == 0;
    xr_free(expected);
    return valid;
}

static bool verify_borrowed_result_loan_key(const XrSemanticPlan *plan,
                                            const XrSemanticEntityRecord *entity) {
    if (!entity || entity->parent >= plan->entity_count ||
        !borrowed_result_has_loan(plan, entity->subject))
        return false;
    const XrSemanticEntityRecord *operation_entity = &plan->entities[entity->parent];
    if (operation_entity->kind != XR_SEM_ENTITY_OPERATION ||
        operation_entity->subject != entity->subject ||
        operation_entity->parent >= plan->entity_count)
        return false;
    const XrSemanticEntityRecord *function_entity = &plan->entities[operation_entity->parent];
    if (function_entity->kind != XR_SEM_ENTITY_FUNCTION ||
        function_entity->parent >= plan->entity_count)
        return false;
    const XrSemanticEntityRecord *declaration_entity = &plan->entities[function_entity->parent];
    const XrSemanticOperationRecord *operation = &plan->operations[entity->subject];
    if (declaration_entity->kind != XR_SEM_ENTITY_DECLARATION ||
        function_entity->subject != operation->function ||
        declaration_entity->subject != operation->function)
        return false;
    char parent_id[XR_STABLE_ID_BYTES * 2 + 1];
    char declaration_id[XR_STABLE_ID_BYTES * 2 + 1];
    char function_id[XR_STABLE_ID_BYTES * 2 + 1];
    char operation_id[XR_STABLE_ID_BYTES * 2 + 1];
    char type_id[XR_STABLE_ID_BYTES * 2 + 1];
    xr_stable_id_hex(operation_entity->id, parent_id);
    xr_stable_id_hex(declaration_entity->id, declaration_id);
    xr_stable_id_hex(plan->functions[operation->function].id, function_id);
    xr_stable_id_hex(operation->id, operation_id);
    xr_stable_id_hex(plan->types[operation->result_type].id, type_id);
    char expected[512];
    int length =
        snprintf(expected, sizeof(expected),
                 "entity-v1:schema=%u:kind=%u:parent=%s:declaration=%s:"
                 "function=%s:operation=%s:ordinal=0:type=%s:ownership=%u:alias=%d",
                 XR_SEMANTIC_SCHEMA_VERSION, (unsigned) XR_SEM_ENTITY_LOAN, parent_id,
                 declaration_id, function_id, operation_id, type_id,
                 (unsigned) operation->result_ownership, (int) operation->result_alias_operand);
    return length > 0 && (size_t) length < sizeof(expected) &&
           strcmp(entity->canonical_key, expected) == 0;
}

static bool verify_entity_record(const XrSemanticPlan *plan, const XrSemanticEntityRecord *entity,
                                 XrEntityCoverage *coverage) {
    const XrSemanticEntityRecord *parent =
        entity->parent == XR_SEMANTIC_INDEX_NONE ? NULL : &plan->entities[entity->parent];
    switch (entity->kind) {
        case XR_SEM_ENTITY_PACKAGE:
            coverage->packages++;
            return !parent && entity->subject_kind == XR_SEM_ENTITY_SUBJECT_NONE &&
                   entity->subject == XR_SEMANTIC_INDEX_NONE && entity->ordinal == 0;
        case XR_SEM_ENTITY_MODULE:
            coverage->modules++;
            return parent && parent->kind == XR_SEM_ENTITY_PACKAGE &&
                   entity->subject_kind == XR_SEM_ENTITY_SUBJECT_NONE &&
                   entity->subject == XR_SEMANTIC_INDEX_NONE && entity->ordinal == 0;
        case XR_SEM_ENTITY_DECLARATION:
            return parent &&
                   (parent->kind == XR_SEM_ENTITY_MODULE ||
                    parent->kind == XR_SEM_ENTITY_FUNCTION) &&
                   entity->subject_kind == XR_SEM_ENTITY_SUBJECT_FUNCTION &&
                   mark_entity(coverage->declarations, plan->function_count, entity->subject);
        case XR_SEM_ENTITY_TYPE_INSTANTIATION:
            return parent && parent->kind == XR_SEM_ENTITY_MODULE &&
                   entity->subject_kind == XR_SEM_ENTITY_SUBJECT_TYPE &&
                   mark_entity(coverage->types, plan->type_count, entity->subject);
        case XR_SEM_ENTITY_SHAPE:
            return parent && parent->kind == XR_SEM_ENTITY_TYPE_INSTANTIATION &&
                   entity->subject_kind == XR_SEM_ENTITY_SUBJECT_TYPE &&
                   entity->subject < plan->type_count &&
                   (plan->types[entity->subject].kind == XR_KIND_STRUCT_OBJECT ||
                    (plan->types[entity->subject].flags & XR_SEM_TYPE_AGGREGATE_EXACT) != 0) &&
                   parent->subject == entity->subject &&
                   mark_entity(coverage->shapes, plan->type_count, entity->subject);
        case XR_SEM_ENTITY_FIELD:
            return parent && parent->kind == XR_SEM_ENTITY_SHAPE &&
                   entity->subject_kind == XR_SEM_ENTITY_SUBJECT_TYPE &&
                   entity->subject < plan->type_count && parent->subject == entity->subject &&
                   entity->ordinal < plan->types[entity->subject].child_count &&
                   mark_entity(coverage->fields, plan->type_child_count,
                               plan->types[entity->subject].child_begin + entity->ordinal);
        case XR_SEM_ENTITY_FUNCTION:
            return parent && parent->kind == XR_SEM_ENTITY_DECLARATION &&
                   entity->subject_kind == XR_SEM_ENTITY_SUBJECT_FUNCTION &&
                   parent->subject == entity->subject &&
                   mark_entity(coverage->functions, plan->function_count, entity->subject);
        case XR_SEM_ENTITY_CLOSURE:
            return parent && parent->kind == XR_SEM_ENTITY_FUNCTION &&
                   entity->subject_kind == XR_SEM_ENTITY_SUBJECT_FUNCTION &&
                   entity->subject < plan->function_count && parent->subject == entity->subject &&
                   plan->functions[entity->subject].parent != XR_SEMANTIC_INDEX_NONE &&
                   mark_entity(coverage->closures, plan->function_count, entity->subject);
        case XR_SEM_ENTITY_NATIVE:
            return parent && parent->kind == XR_SEM_ENTITY_FUNCTION &&
                   entity->subject_kind == XR_SEM_ENTITY_SUBJECT_FUNCTION &&
                   parent->subject == entity->subject &&
                   mark_entity(coverage->natives, plan->function_count, entity->subject);
        case XR_SEM_ENTITY_OPERATION:
            return parent && parent->kind == XR_SEM_ENTITY_FUNCTION &&
                   entity->subject_kind == XR_SEM_ENTITY_SUBJECT_OPERATION &&
                   entity->subject < plan->operation_count &&
                   parent->subject == plan->operations[entity->subject].function &&
                   mark_entity(coverage->operations, plan->operation_count, entity->subject);
        case XR_SEM_ENTITY_ALLOCATION:
            return parent && parent->kind == XR_SEM_ENTITY_OPERATION &&
                   entity->subject_kind == XR_SEM_ENTITY_SUBJECT_OPERATION &&
                   entity->subject < plan->operation_count && parent->subject == entity->subject &&
                   plan->operations[entity->subject].allocation_key &&
                   mark_entity(coverage->allocations, plan->operation_count, entity->subject);
        case XR_SEM_ENTITY_OWNER:
            return parent && parent->kind == XR_SEM_ENTITY_FUNCTION &&
                   entity->subject_kind == XR_SEM_ENTITY_SUBJECT_OWNER &&
                   entity->subject < plan->ownership->owner_count &&
                   parent->subject == plan->ownership->owners[entity->subject].function &&
                   mark_entity(coverage->owners, plan->ownership->owner_count, entity->subject);
        case XR_SEM_ENTITY_LOAN:
            if (entity->subject_kind == XR_SEM_ENTITY_SUBJECT_OPERATION)
                return entity->ordinal == 0 && verify_borrowed_result_loan_key(plan, entity) &&
                       mark_entity(coverage->operation_loans, plan->operation_count,
                                   entity->subject);
            if (!parent || parent->kind != XR_SEM_ENTITY_FUNCTION)
                return false;
            if (entity->subject_kind == XR_SEM_ENTITY_SUBJECT_PARAMETER)
                return entity->subject < plan->parameter_count &&
                       parent->subject == plan->parameters[entity->subject].function &&
                       plan->parameters[entity->subject].ownership == XI_OWN_BORROWED &&
                       mark_entity(coverage->parameter_loans, plan->parameter_count,
                                   entity->subject);
            if (entity->subject_kind == XR_SEM_ENTITY_SUBJECT_CAPTURE)
                return entity->subject < plan->capture_count &&
                       parent->subject == plan->captures[entity->subject].function &&
                       plan->captures[entity->subject].kind == XR_SEM_CAPTURE_BY_IMM_REF &&
                       mark_entity(coverage->capture_loans, plan->capture_count, entity->subject);
            return false;
        case XR_SEM_ENTITY_DOMAIN:
            if (!parent || parent->kind != XR_SEM_ENTITY_MODULE ||
                entity->subject_kind != XR_SEM_ENTITY_SUBJECT_STORAGE_DOMAIN ||
                entity->subject < XR_STORAGE_EXEC_LOCAL || entity->subject > XR_STORAGE_FOREIGN ||
                coverage->domains[entity->subject] != 0)
                return false;
            coverage->domains[entity->subject] = 1;
            return entity->ordinal == 0;
        case XR_SEM_ENTITY_COROUTINE_STATE:
            return parent && parent->kind == XR_SEM_ENTITY_FUNCTION &&
                   entity->subject_kind == XR_SEM_ENTITY_SUBJECT_OPERATION &&
                   entity->subject < plan->operation_count &&
                   parent->subject == plan->operations[entity->subject].function &&
                   entity->ordinal != 0 &&
                   mark_entity(coverage->coroutine_states, plan->operation_count, entity->subject);
        case XR_SEM_ENTITY_COROUTINE_LIVE:
        case XR_SEM_ENTITY_COROUTINE_ROOT:
        case XR_SEM_ENTITY_COROUTINE_DROP:
            return parent && parent->kind == XR_SEM_ENTITY_COROUTINE_STATE &&
                   entity->subject_kind == XR_SEM_ENTITY_SUBJECT_OPERATION &&
                   entity->subject < plan->operation_count &&
                   parent->subject < plan->operation_count && entity->ordinal == 0 &&
                   plan->operations[entity->subject].function ==
                       plan->operations[parent->subject].function;
        case XR_SEM_ENTITY_DEBUG_SPAN:
            return parent && parent->kind == XR_SEM_ENTITY_OPERATION &&
                   entity->subject_kind == XR_SEM_ENTITY_SUBJECT_OPERATION &&
                   entity->subject < plan->operation_count && parent->subject == entity->subject &&
                   verify_debug_span_entity_key(plan, entity) &&
                   mark_entity(coverage->debug_spans, plan->operation_count, entity->subject);
        default:
            return false;
    }
}

static bool verify_entity_coverage(const XrSemanticPlan *plan, const XrEntityCoverage *coverage) {
    if (coverage->packages != 1 || coverage->modules != 1)
        return false;
    for (uint32_t i = 0; i < plan->type_count; i++) {
        bool aggregate_shape = plan->types[i].kind == XR_KIND_STRUCT_OBJECT ||
                               (plan->types[i].flags & XR_SEM_TYPE_AGGREGATE_EXACT) != 0;
        if (coverage->types[i] != 1 || (aggregate_shape && coverage->shapes[i] != 1))
            return false;
        if (aggregate_shape) {
            for (uint16_t field = 0; field < plan->types[i].child_count; field++) {
                if (coverage->fields[plan->types[i].child_begin + field] != 1)
                    return false;
            }
        }
    }
    for (uint32_t i = 0; i < plan->function_count; i++) {
        bool closure = plan->functions[i].parent != XR_SEMANTIC_INDEX_NONE;
        bool native = (plan->functions[i].flags & 8u) != 0;
        if (coverage->declarations[i] != 1 || coverage->functions[i] != 1 ||
            coverage->closures[i] != (uint8_t) closure || (native && coverage->natives[i] != 1))
            return false;
    }
    for (uint32_t i = 0; i < plan->operation_count; i++) {
        bool allocation = plan->operations[i].allocation_key != NULL;
        bool debug_span = plan->operations[i].source_file != NULL;
        bool operation_loan = borrowed_result_has_loan(plan, i);
        if (coverage->operations[i] != 1 || coverage->allocations[i] != (uint8_t) allocation ||
            coverage->debug_spans[i] != (uint8_t) debug_span ||
            coverage->operation_loans[i] != (uint8_t) operation_loan)
            return false;
    }
    for (uint32_t i = 0; i < plan->ownership->owner_count; i++) {
        if (coverage->owners[i] != 1)
            return false;
    }
    for (uint32_t i = 0; i < plan->parameter_count; i++) {
        bool borrowed = plan->parameters[i].ownership == XI_OWN_BORROWED;
        if (coverage->parameter_loans[i] != (uint8_t) borrowed)
            return false;
    }
    for (uint32_t i = 0; i < plan->capture_count; i++) {
        bool borrowed = plan->captures[i].kind == XR_SEM_CAPTURE_BY_IMM_REF;
        if (coverage->capture_loans[i] != (uint8_t) borrowed)
            return false;
    }
    for (uint32_t domain = XR_STORAGE_EXEC_LOCAL; domain <= XR_STORAGE_FOREIGN; domain++) {
        if (coverage->domains[domain] != 1)
            return false;
    }
    return true;
}

typedef struct XrCoroutineEntityKey {
    uint32_t function;
    uint32_t state;
} XrCoroutineEntityKey;

static int compare_coroutine_entity_key(const void *left, const void *right) {
    const XrCoroutineEntityKey *a = (const XrCoroutineEntityKey *) left;
    const XrCoroutineEntityKey *b = (const XrCoroutineEntityKey *) right;
    if (a->function != b->function)
        return a->function < b->function ? -1 : 1;
    return a->state < b->state ? -1 : a->state != b->state;
}

static bool verify_coroutine_entity_sequence(const XrSemanticPlan *plan) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < plan->entity_count; i++) {
        if (plan->entities[i].kind == XR_SEM_ENTITY_COROUTINE_STATE)
            count++;
    }
    XrCoroutineEntityKey *keys =
        count ? (XrCoroutineEntityKey *) xr_malloc((size_t) count * sizeof(*keys)) : NULL;
    if (count && !keys)
        return false;
    uint32_t cursor = 0;
    for (uint32_t i = 0; i < plan->entity_count; i++) {
        const XrSemanticEntityRecord *entity = &plan->entities[i];
        if (entity->kind != XR_SEM_ENTITY_COROUTINE_STATE)
            continue;
        keys[cursor++] = (XrCoroutineEntityKey) {
            plan->operations[entity->subject].function,
            entity->ordinal,
        };
    }
    qsort(keys, count, sizeof(*keys), compare_coroutine_entity_key);
    bool valid = true;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t expected =
            i == 0 || keys[i - 1].function != keys[i].function ? 1 : keys[i - 1].state + 1;
        if (keys[i].state != expected) {
            valid = false;
            break;
        }
    }
    xr_free(keys);
    return valid;
}

typedef struct XrCoroutineLifecycleEntityProjection {
    uint32_t entity;
    uint32_t state_entity;
    uint32_t producer_operation;
    uint16_t role;
} XrCoroutineLifecycleEntityProjection;

static int compare_coroutine_lifecycle_entity_projection(const void *left, const void *right) {
    const XrCoroutineLifecycleEntityProjection *a =
        (const XrCoroutineLifecycleEntityProjection *) left;
    const XrCoroutineLifecycleEntityProjection *b =
        (const XrCoroutineLifecycleEntityProjection *) right;
    if (a->state_entity != b->state_entity)
        return a->state_entity < b->state_entity ? -1 : 1;
    if (a->producer_operation != b->producer_operation)
        return a->producer_operation < b->producer_operation ? -1 : 1;
    if (a->role != b->role)
        return a->role < b->role ? -1 : 1;
    if (a->entity != b->entity)
        return a->entity < b->entity ? -1 : 1;
    return 0;
}

static bool coroutine_lifecycle_entity_key_is_exact(const XrSemanticPlan *plan,
                                                    const XrSemanticCoroutineLifecycleShape *shape,
                                                    uint32_t owner_entity,
                                                    const XrSemanticEntityRecord *entity) {
    if (!shape || !entity || entity->parent != shape->state_entity ||
        entity->subject_kind != XR_SEM_ENTITY_SUBJECT_OPERATION ||
        entity->subject != shape->producer_operation || entity->parent >= plan->entity_count ||
        (entity->kind != XR_SEM_ENTITY_COROUTINE_LIVE &&
         entity->kind != XR_SEM_ENTITY_COROUTINE_ROOT &&
         entity->kind != XR_SEM_ENTITY_COROUTINE_DROP) ||
        owner_entity >= plan->entity_count)
        return false;
    const XrSemanticEntityRecord *owner = &plan->entities[owner_entity];
    if (owner->kind != XR_SEM_ENTITY_OWNER || owner->subject_kind != XR_SEM_ENTITY_SUBJECT_OWNER ||
        owner->subject != shape->owner)
        return false;
    char parent_id[XR_STABLE_ID_BYTES * 2 + 1];
    char producer_id[XR_STABLE_ID_BYTES * 2 + 1];
    char release_id[XR_STABLE_ID_BYTES * 2 + 1];
    char owner_id[XR_STABLE_ID_BYTES * 2 + 1];
    xr_stable_id_hex(plan->entities[entity->parent].id, parent_id);
    xr_stable_id_hex(plan->operations[shape->producer_operation].id, producer_id);
    xr_stable_id_hex(plan->operations[shape->release_operation].id, release_id);
    xr_stable_id_hex(owner->id, owner_id);
    char expected_key[512];
    int length =
        snprintf(expected_key, sizeof(expected_key),
                 "entity-v1:schema=%u:kind=%u:parent=%s:value-operation=%s:release=%s:owner=%s",
                 XR_SEMANTIC_SCHEMA_VERSION, (unsigned) entity->kind, parent_id, producer_id,
                 release_id, owner_id);
    return length > 0 && (size_t) length < sizeof(expected_key) &&
           strcmp(entity->canonical_key, expected_key) == 0;
}

static bool verify_coroutine_lifecycle_entities(const XrSemanticPlan *plan,
                                                const XrSemanticGraph *graph, char *error,
                                                size_t error_size) {
    uint32_t lifecycle_count = 0;
    for (uint32_t entity = 0; entity < plan->entity_count; entity++) {
        uint16_t kind = plan->entities[entity].kind;
        if (kind == XR_SEM_ENTITY_COROUTINE_LIVE || kind == XR_SEM_ENTITY_COROUTINE_ROOT ||
            kind == XR_SEM_ENTITY_COROUTINE_DROP)
            lifecycle_count++;
    }
    if (lifecycle_count % 3u != 0)
        return report(error, error_size, "XR_SEM_0019",
                      "coroutine lifecycle identity coverage is incomplete");
    uint32_t expected_capacity = lifecycle_count / 3u;
    XrSemanticCoroutineLifecycleProjection projection = {0};
    XrSemanticCoroutineLifecycleProjectionStatus projection_status =
        xr_semantic_coroutine_lifecycle_projection_build(plan, graph, expected_capacity,
                                                         &projection);
    if (projection_status != XR_SEMANTIC_LIFECYCLE_PROJECTION_OK)
        return report(error, error_size,
                      projection_status == XR_SEMANTIC_LIFECYCLE_PROJECTION_INVALID
                          ? "XR_SEM_0019"
                          : "XR_EXEC_5003",
                      projection_status == XR_SEMANTIC_LIFECYCLE_PROJECTION_INVALID
                          ? "coroutine lifecycle projection is invalid"
                          : "coroutine lifecycle projection budget exhausted");
    size_t owner_count_size =
        xr_ownership_certificate_owner_count(xr_semantic_plan_ownership(plan));
    if (owner_count_size > UINT32_MAX) {
        xr_semantic_coroutine_lifecycle_projection_dispose(&projection);
        return report(error, error_size, "XR_EXEC_5003",
                      "coroutine lifecycle owner index exceeds row width");
    }
    uint32_t owner_count = (uint32_t) owner_count_size;
    uint32_t sort_height = xr_semantic_lifecycle_sort_height(lifecycle_count);
    if (!xr_semantic_lifecycle_work_charge(&projection.indexed_work, owner_count) ||
        !xr_semantic_lifecycle_work_charge(&projection.indexed_work, plan->entity_count) ||
        !xr_semantic_lifecycle_work_charge_product(&projection.indexed_work, lifecycle_count,
                                                   sort_height + 3u)) {
        xr_semantic_coroutine_lifecycle_projection_dispose(&projection);
        return report(error, error_size, "XR_EXEC_5003",
                      "coroutine lifecycle verifier budget exhausted");
    }
    XrCoroutineLifecycleEntityProjection *entities =
        lifecycle_count ? (XrCoroutineLifecycleEntityProjection *) xr_malloc(
                              (size_t) lifecycle_count * sizeof(*entities))
                        : NULL;
    uint32_t *owner_entities =
        owner_count ? (uint32_t *) xr_malloc((size_t) owner_count * sizeof(*owner_entities)) : NULL;
    if ((lifecycle_count && !entities) || (owner_count && !owner_entities)) {
        xr_free(entities);
        xr_free(owner_entities);
        xr_semantic_coroutine_lifecycle_projection_dispose(&projection);
        return report(error, error_size, "XR_EXEC_5003",
                      "coroutine lifecycle verifier index allocation failed");
    }
    for (uint32_t owner = 0; owner < owner_count; owner++)
        owner_entities[owner] = XR_SEMANTIC_INDEX_NONE;
    uint32_t next_entity = 0;
    for (uint32_t entity = 0; entity < plan->entity_count; entity++) {
        const XrSemanticEntityRecord *record = &plan->entities[entity];
        if (record->kind == XR_SEM_ENTITY_COROUTINE_LIVE ||
            record->kind == XR_SEM_ENTITY_COROUTINE_ROOT ||
            record->kind == XR_SEM_ENTITY_COROUTINE_DROP) {
            entities[next_entity++] = (XrCoroutineLifecycleEntityProjection) {
                .entity = entity,
                .state_entity = record->parent,
                .producer_operation = record->subject,
                .role = (uint16_t) (record->kind - XR_SEM_ENTITY_COROUTINE_LIVE),
            };
        }
        if (record->kind != XR_SEM_ENTITY_OWNER)
            continue;
        if (record->subject_kind != XR_SEM_ENTITY_SUBJECT_OWNER || record->subject >= owner_count ||
            owner_entities[record->subject] != XR_SEMANTIC_INDEX_NONE) {
            xr_free(owner_entities);
            xr_free(entities);
            xr_semantic_coroutine_lifecycle_projection_dispose(&projection);
            return report(error, error_size, "XR_SEM_0019",
                          "coroutine lifecycle owner identity is not exact");
        }
        owner_entities[record->subject] = entity;
    }
    qsort(entities, lifecycle_count, sizeof(*entities),
          compare_coroutine_lifecycle_entity_projection);
    bool complete = projection.count <= UINT32_MAX / 3u && lifecycle_count == projection.count * 3u;
    for (uint32_t i = 0; complete && i < projection.count; i++) {
        const XrSemanticCoroutineLifecycleShape *shape = &projection.rows[i];
        uint32_t owner_entity =
            shape->owner < owner_count ? owner_entities[shape->owner] : XR_SEMANTIC_INDEX_NONE;
        if (owner_entity == XR_SEMANTIC_INDEX_NONE) {
            complete = false;
            break;
        }
        for (uint32_t role = 0; complete && role < 3u; role++) {
            const XrCoroutineLifecycleEntityProjection *candidate = &entities[i * 3u + role];
            const XrSemanticEntityRecord *record = &plan->entities[candidate->entity];
            complete = candidate->state_entity == shape->state_entity &&
                       candidate->producer_operation == shape->producer_operation &&
                       candidate->role == role &&
                       coroutine_lifecycle_entity_key_is_exact(plan, shape, owner_entity, record);
        }
    }
    xr_free(owner_entities);
    xr_free(entities);
    xr_semantic_coroutine_lifecycle_projection_dispose(&projection);
    if (!complete)
        return report(error, error_size, "XR_SEM_0019",
                      "coroutine lifecycle identity coverage is incomplete");
    return true;
}

static bool verify_entities(const XrSemanticPlan *plan, char *error, size_t error_size) {
    XrEntityCoverage coverage = {0};
    if (!entity_coverage_init(plan, &coverage))
        return report(error, error_size, "XR_EXEC_5003", "entity verifier budget exhausted");
    for (uint32_t i = 0; i < plan->entity_count; i++) {
        const XrSemanticEntityRecord *entity = &plan->entities[i];
        bool valid =
            verify_id(entity->canonical_key, entity->id) &&
            entity->kind < XR_SEM_ENTITY_KIND_COUNT && entity->flags == 0 &&
            (entity->parent == XR_SEMANTIC_INDEX_NONE || entity->parent < plan->entity_count) &&
            (i == 0 || xr_stable_id_compare(plan->entities[i - 1].id, entity->id) < 0) &&
            verify_entity_record(plan, entity, &coverage);
        if (!valid) {
            entity_coverage_dispose(&coverage);
            return report(error, error_size, "XR_SEM_0019",
                          "stable entity identity relation is invalid");
        }
    }
    bool complete =
        verify_entity_coverage(plan, &coverage) && verify_coroutine_entity_sequence(plan);
    entity_coverage_dispose(&coverage);
    return complete || report(error, error_size, "XR_SEM_0019",
                              "stable entity identity coverage is incomplete");
}

static bool verify_types(const XrSemanticPlan *plan, char *error, size_t error_size) {
    uint32_t child_cursor = 0;
    for (uint32_t i = 0; i < plan->type_count; i++) {
        const XrSemanticTypeRecord *type = &plan->types[i];
        if (!verify_id(type->canonical_key, type->id))
            return report(error, error_size, "XR_SEM_0002", "type stable identity is invalid");
        if (!semantic_source_enum_identity_exact(type))
            return report(error, error_size, "XR_SEM_0002",
                          "source enum declaration identity is not exact");
        XrStableId zero = {{0}};
        if (type->source_class != XR_SEMANTIC_INDEX_NONE) {
            if (type->source_class >= plan->source_class_count ||
                (type->kind != XR_KIND_CLASS && type->kind != XR_KIND_INSTANCE) ||
                !xr_stable_id_equal(type->source_class_identity,
                                    plan->source_classes[type->source_class].id))
                return report(error, error_size, "XR_SEM_0002",
                              "source nominal type authority is invalid");
            char suffix[64];
            char class_id[XR_STABLE_ID_BYTES * 2 + 1];
            xr_stable_id_hex(plan->source_classes[type->source_class].id, class_id);
            int length = snprintf(suffix, sizeof(suffix), ";source-class:%s", class_id);
            size_t key_length = strlen(type->canonical_key);
            if (length <= 0 || (size_t) length >= sizeof(suffix) || key_length < (size_t) length ||
                strcmp(type->canonical_key + key_length - (size_t) length, suffix) != 0)
                return report(error, error_size, "XR_SEM_0002",
                              "source nominal type identity is not exact");
        } else if (!xr_stable_id_equal(type->source_class_identity, zero)) {
            if (type->kind != XR_KIND_CLASS && type->kind != XR_KIND_INSTANCE)
                return report(error, error_size, "XR_SEM_0002",
                              "imported source nominal type authority is invalid");
            char suffix[64];
            char class_id[XR_STABLE_ID_BYTES * 2 + 1];
            xr_stable_id_hex(type->source_class_identity, class_id);
            int length = snprintf(suffix, sizeof(suffix), ";source-class:%s", class_id);
            size_t key_length = strlen(type->canonical_key);
            if (length <= 0 || (size_t) length >= sizeof(suffix) || key_length < (size_t) length ||
                strcmp(type->canonical_key + key_length - (size_t) length, suffix) != 0)
                return report(error, error_size, "XR_SEM_0002",
                              "imported source nominal identity is not exact");
        }
        if (!semantic_builtin_type_identity_exact(type))
            return report(error, error_size, "XR_SEM_0002", "builtin type identity is not exact");
        if (type->kind >= XR_KIND_COUNT)
            return report(error, error_size, "XR_SEM_0005", "plan contains an invalid type kind");
        if ((type->flags & XR_SEM_TYPE_OWNERSHIP_ROOT) != 0 &&
            (type->flags & XR_SEM_TYPE_REFERENCE_CAPABLE) == 0)
            return report(error, error_size, "XR_OWN_3000",
                          "ownership-root type is not reference-capable");
        if ((type->flags & XR_SEM_TYPE_BORROW_VIEW) != 0 &&
            ((type->flags & XR_SEM_TYPE_REFERENCE_CAPABLE) == 0 ||
             (type->flags & XR_SEM_TYPE_OWNERSHIP_ROOT) != 0))
            return report(error, error_size, "XR_OWN_3000",
                          "borrow-view type has an invalid ownership class");
        if ((type->flags & XR_SEM_TYPE_AGGREGATE_EXACT) != 0 &&
            (type->kind != XR_KIND_INSTANCE || (type->flags & XR_SEM_TYPE_VALUE) == 0))
            return report(error, error_size, "XR_SEM_0012", "exact aggregate flag is invalid");
        if (type->kind == XR_KIND_FIXED_ARRAY) {
            if (type->child_count != 1 || type->aggregate_extent == 0 || type->aggregate_align != 0)
                return report(error, error_size, "XR_SEM_0012",
                              "fixed-array aggregate facts are invalid");
        } else if (type->kind == XR_KIND_TUPLE || type->kind == XR_KIND_STRUCT_OBJECT) {
            if (type->aggregate_extent != type->child_count || type->aggregate_align != 0)
                return report(error, error_size, "XR_SEM_0012",
                              "structural aggregate facts are invalid");
        } else if ((type->flags & XR_SEM_TYPE_AGGREGATE_EXACT) != 0) {
            if (type->aggregate_extent != type->child_count ||
                (type->aggregate_align != 0 && !is_power_of_two_u32(type->aggregate_align)))
                return report(error, error_size, "XR_SEM_0012",
                              "named aggregate facts are invalid");
        } else if (type->aggregate_extent != 0 || type->aggregate_align != 0) {
            return report(error, error_size, "XR_SEM_0012",
                          "non-aggregate type carries aggregate facts");
        }
        if (type->child_begin != child_cursor ||
            !range_valid(type->child_begin, type->child_count, plan->type_child_count))
            return report(error, error_size, "XR_SEM_0012", "type child range is invalid");
        for (uint16_t c = 0; c < type->child_count; c++) {
            if (plan->type_children[type->child_begin + c] >= plan->type_count)
                return report(error, error_size, "XR_SEM_0012", "type child index is invalid");
        }
        if (i > 0 && xr_stable_id_compare(plan->types[i - 1].id, type->id) >= 0)
            return report(error, error_size, "XR_SEM_0012",
                          "type table is not in strict stable-identity order");
        child_cursor += type->child_count;
    }
    return child_cursor == plan->type_child_count ||
           report(error, error_size, "XR_SEM_0012", "type child table is not exactly partitioned");
}

static bool verifier_key_append(char *buffer, size_t capacity, size_t *used, const char *format,
                                ...) {
    if (!buffer || !capacity || !used || *used >= capacity)
        return false;
    va_list args;
    va_start(args, format);
    int written = vsnprintf(buffer + *used, capacity - *used, format, args);
    va_end(args);
    if (written < 0 || (size_t) written >= capacity - *used)
        return false;
    *used += (size_t) written;
    return true;
}

static bool verifier_key_append_id(char *buffer, size_t capacity, size_t *used, XrStableId id) {
    char hex[XR_STABLE_ID_BYTES * 2 + 1];
    xr_stable_id_hex(id, hex);
    return verifier_key_append(buffer, capacity, used, "%s", hex);
}

static bool verify_source_classes(const XrSemanticPlan *plan, char *error, size_t error_size) {
    const XrSemanticEntityRecord *module = NULL;
    for (uint32_t e = 0; e < plan->entity_count; e++) {
        if (plan->entities[e].kind != XR_SEM_ENTITY_MODULE)
            continue;
        if (module)
            return report(error, error_size, "XR_SEM_0019",
                          "source-class module identity is ambiguous");
        module = &plan->entities[e];
    }
    for (uint32_t c = 0; c < plan->source_class_count; c++) {
        const XrSemanticSourceClassRecord *source_class = &plan->source_classes[c];
        char expected[768];
        char module_id[XR_STABLE_ID_BYTES * 2 + 1];
        xr_stable_id_hex(source_class->module, module_id);
        int length =
            snprintf(expected, sizeof(expected),
                     "source-class-v1:schema=%u:module=%s:path=%zu:%s:name=%zu:%s:super="
                     "%zu:%s:ordinal=%u:methods=%u:flags=%u",
                     XR_SEMANTIC_SCHEMA_VERSION, module_id,
                     source_class->module_path ? strlen(source_class->module_path) : 0u,
                     source_class->module_path ? source_class->module_path : "",
                     source_class->name ? strlen(source_class->name) : 0u,
                     source_class->name ? source_class->name : "",
                     source_class->super_name ? strlen(source_class->super_name) : 0u,
                     source_class->super_name ? source_class->super_name : "",
                     source_class->ordinal, source_class->method_count, source_class->flags);
        uint8_t allowed = XR_SEM_SOURCE_CLASS_EXPLICIT_FINAL | XR_SEM_SOURCE_CLASS_RUNTIME_TYPE |
                          XR_SEM_SOURCE_CLASS_GENERIC;
        if (!module || !source_class->module_path || !source_class->module_path[0] ||
            !source_class->name || !source_class->name[0] || source_class->ordinal != c ||
            source_class->reserved != 0 || (source_class->flags & (uint8_t) ~allowed) != 0 ||
            !xr_stable_id_equal(source_class->module, module->id) || length <= 0 ||
            (size_t) length >= sizeof(expected) ||
            strcmp(source_class->canonical_key ? source_class->canonical_key : "", expected) != 0 ||
            !verify_id(source_class->canonical_key, source_class->id))
            return report(error, error_size, "XR_SEM_0019",
                          "source-class stable authority is invalid");
    }
    return true;
}

static bool verify_source_methods(const XrSemanticPlan *plan, char *error, size_t error_size) {
    uint8_t **seen = plan->source_class_count
                         ? (uint8_t **) xr_calloc(plan->source_class_count, sizeof(*seen))
                         : NULL;
    if (plan->source_class_count && !seen)
        return report(error, error_size, "XR_EXEC_5003", "source-method verifier budget exhausted");
    for (uint32_t c = 0; c < plan->source_class_count; c++) {
        uint16_t count = plan->source_classes[c].method_count;
        seen[c] = count ? (uint8_t *) xr_calloc(count, 1) : NULL;
        if (count && !seen[c]) {
            for (uint32_t before = 0; before < c; before++)
                xr_free(seen[before]);
            xr_free(seen);
            return report(error, error_size, "XR_EXEC_5003",
                          "source-method verifier budget exhausted");
        }
    }
    bool valid = true;
    for (uint32_t i = 0; valid && i < plan->source_method_count; i++) {
        const XrSemanticSourceMethodRecord *method = &plan->source_methods[i];
        const XrSemanticSourceClassRecord *source_class =
            method->source_class < plan->source_class_count
                ? &plan->source_classes[method->source_class]
                : NULL;
        const XrSemanticFunctionRecord *function =
            method->function < plan->function_count ? &plan->functions[method->function] : NULL;
        uint8_t expected_flags = XR_SEM_SOURCE_METHOD_INSTANCE;
        if (source_class && (source_class->flags & XR_SEM_SOURCE_CLASS_EXPLICIT_FINAL) == 0)
            expected_flags |= XR_SEM_SOURCE_METHOD_OPEN_DOMAIN;
        char class_id[XR_STABLE_ID_BYTES * 2 + 1];
        char function_id[XR_STABLE_ID_BYTES * 2 + 1];
        char expected[768];
        if (source_class)
            xr_stable_id_hex(source_class->id, class_id);
        if (function)
            xr_stable_id_hex(function->id, function_id);
        int length = source_class && function && method->name
                         ? snprintf(expected, sizeof(expected),
                                    "source-method-v1:schema=%u:class=%s:member=%u:name=%zu:%s:"
                                    "function=%s:params=%u:flags=%u",
                                    XR_SEMANTIC_SCHEMA_VERSION, class_id, method->member_ordinal,
                                    strlen(method->name), method->name, function_id,
                                    method->parameter_count, method->flags)
                         : -1;
        valid = source_class && function && method->name && method->name[0] &&
                method->member_ordinal < source_class->method_count &&
                !seen[method->source_class][method->member_ordinal] &&
                function->source_class == method->source_class &&
                function->source_member_ordinal == method->member_ordinal &&
                function->source_kind == XR_SEM_SOURCE_FUNCTION_INSTANCE_METHOD &&
                function->parameter_count == method->parameter_count &&
                strcmp(function->name, method->name) == 0 && method->flags == expected_flags &&
                method->reserved[0] == 0 && method->reserved[1] == 0 && method->reserved[2] == 0 &&
                length > 0 && (size_t) length < sizeof(expected) &&
                strcmp(method->canonical_key ? method->canonical_key : "", expected) == 0 &&
                verify_id(method->canonical_key, method->id);
        if (valid)
            seen[method->source_class][method->member_ordinal] = 1;
    }
    for (uint32_t c = 0; valid && c < plan->source_class_count; c++)
        for (uint16_t m = 0; m < plan->source_classes[c].method_count; m++) {
            const XrSemanticFunctionRecord *function = NULL;
            for (uint32_t f = 0; f < plan->function_count; f++)
                if (plan->functions[f].source_class == c &&
                    plan->functions[f].source_member_ordinal == m) {
                    function = &plan->functions[f];
                    break;
                }
            if (function && function->source_kind == XR_SEM_SOURCE_FUNCTION_INSTANCE_METHOD &&
                !seen[c][m])
                valid = false;
        }
    for (uint32_t c = 0; c < plan->source_class_count; c++)
        xr_free(seen[c]);
    xr_free(seen);
    return valid || report(error, error_size, "XR_SEM_0019",
                           "source-method declaration authority is invalid");
}

static bool verify_function_key_exact(const XrSemanticPlan *plan, uint32_t index) {
    const XrSemanticFunctionRecord *function = &plan->functions[index];
    if (!function->canonical_key || !function->name || function->return_type >= plan->type_count ||
        !range_valid(function->parameter_begin, function->parameter_count, plan->parameter_count) ||
        (function->parent != XR_SEMANTIC_INDEX_NONE && function->parent >= plan->function_count) ||
        (function->source_class != XR_SEMANTIC_INDEX_NONE &&
         function->source_class >= plan->source_class_count))
        return false;
    size_t capacity = strlen(function->canonical_key) + 1u;
    char *expected = (char *) xr_malloc(capacity);
    if (!expected)
        return false;
    expected[0] = '\0';
    size_t used = 0;
    uint32_t lexical_ordinal = 0;
    for (uint32_t before = 0; before < index; before++)
        if (plan->functions[before].parent == function->parent)
            lexical_ordinal++;
    bool valid = verifier_key_append(expected, capacity, &used, "function-v3:parent=") &&
                 (function->parent == XR_SEMANTIC_INDEX_NONE
                      ? verifier_key_append(expected, capacity, &used, "module-root")
                      : verifier_key_append_id(expected, capacity, &used,
                                               plan->functions[function->parent].id)) &&
                 verifier_key_append(expected, capacity, &used,
                                     ":ordinal=%u:name=%zu:%s:source-class=", lexical_ordinal,
                                     strlen(function->name), function->name) &&
                 (function->source_class == XR_SEMANTIC_INDEX_NONE
                      ? verifier_key_append(expected, capacity, &used, "none")
                      : verifier_key_append_id(expected, capacity, &used,
                                               plan->source_classes[function->source_class].id));
    if (valid) {
        if (function->source_member_ordinal == UINT16_MAX)
            valid =
                verifier_key_append(expected, capacity, &used,
                                    ":member=none:source-kind=%u:return=", function->source_kind);
        else
            valid =
                verifier_key_append(expected, capacity, &used, ":member=%u:source-kind=%u:return=",
                                    function->source_member_ordinal, function->source_kind);
    }
    valid =
        valid &&
        verifier_key_append_id(expected, capacity, &used, plan->types[function->return_type].id) &&
        verifier_key_append(expected, capacity, &used, ":params=%u", function->parameter_count);
    for (uint16_t p = 0; valid && p < function->parameter_count; p++) {
        const XrSemanticParameterRecord *parameter =
            &plan->parameters[function->parameter_begin + p];
        valid = parameter->type < plan->type_count &&
                verifier_key_append(expected, capacity, &used, ":p%u:mode=%u:type=", p,
                                    parameter->mode) &&
                verifier_key_append_id(expected, capacity, &used, plan->types[parameter->type].id);
    }
    valid = valid &&
            verifier_key_append(expected, capacity, &used, ":effects=%u:caps=%u:flags=%u",
                                function->semantic_effects, function->capability_mask,
                                (unsigned) function->flags) &&
            strcmp(expected, function->canonical_key) == 0;
    xr_free(expected);
    return valid;
}

static bool verify_functions(const XrSemanticPlan *plan, char *error, size_t error_size) {
    uint32_t parameter_cursor = 0;
    uint32_t capture_cursor = 0;
    uint32_t block_cursor = 0;
    uint32_t value_cursor = 0;
    uint32_t *child_counts = (uint32_t *) xr_calloc(plan->function_count, sizeof(*child_counts));
    uint8_t **source_members =
        plan->source_class_count
            ? (uint8_t **) xr_calloc(plan->source_class_count, sizeof(*source_members))
            : NULL;
    if ((plan->function_count && !child_counts) || (plan->source_class_count && !source_members)) {
        xr_free(child_counts);
        return report(error, error_size, "XR_EXEC_5003",
                      "function relation verifier budget exhausted");
    }
    for (uint32_t c = 0; c < plan->source_class_count; c++) {
        uint16_t count = plan->source_classes[c].method_count;
        source_members[c] = count ? (uint8_t *) xr_calloc(count, 1) : NULL;
        if (count && !source_members[c]) {
            for (uint32_t before = 0; before < c; before++)
                xr_free(source_members[before]);
            xr_free(source_members);
            xr_free(child_counts);
            return report(error, error_size, "XR_EXEC_5003",
                          "source-method verifier budget exhausted");
        }
    }
#define XR_FUNCTION_FAIL(code, detail)                                                             \
    do {                                                                                           \
        for (uint32_t source_class = 0; source_class < plan->source_class_count; source_class++)   \
            xr_free(source_members[source_class]);                                                 \
        xr_free(source_members);                                                                   \
        xr_free(child_counts);                                                                     \
        return report(error, error_size, (code), (detail));                                        \
    } while (0)
    for (uint32_t i = 0; i < plan->function_count; i++) {
        const XrSemanticFunctionRecord *function = &plan->functions[i];
        if (!verify_id(function->canonical_key, function->id) ||
            !verify_function_key_exact(plan, i))
            XR_FUNCTION_FAIL("XR_SEM_0002", "function identity is invalid");
        if (function->is_module_initializer > 1u || function->carries_coroutine_ops > 1u ||
            function->source_kind > XR_SEM_SOURCE_FUNCTION_CONSTRUCTOR)
            XR_FUNCTION_FAIL("XR_SEM_0013", "function source authority is invalid");
        if (function->source_class == XR_SEMANTIC_INDEX_NONE) {
            if (function->source_member_ordinal != UINT16_MAX ||
                function->source_kind != XR_SEM_SOURCE_FUNCTION_NONE)
                XR_FUNCTION_FAIL("XR_SEM_0013", "free function has source member authority");
        } else {
            if (function->source_class >= plan->source_class_count ||
                function->source_kind == XR_SEM_SOURCE_FUNCTION_NONE ||
                function->source_member_ordinal >=
                    plan->source_classes[function->source_class].method_count ||
                source_members[function->source_class][function->source_member_ordinal] != 0)
                XR_FUNCTION_FAIL("XR_SEM_0013", "source method authority is invalid");
            source_members[function->source_class][function->source_member_ordinal] = 1;
        }
        if ((i == 0 && function->parent != XR_SEMANTIC_INDEX_NONE) ||
            (i > 0 && function->parent >= i) || function->reserved != 0)
            XR_FUNCTION_FAIL("XR_SEM_0013", "function lexical parent is invalid");
        if (function->parent != XR_SEMANTIC_INDEX_NONE)
            child_counts[function->parent]++;
        if (function->return_type >= plan->type_count ||
            !range_valid(function->parameter_begin, function->parameter_count,
                         plan->parameter_count) ||
            !range_valid(function->capture_begin, function->capture_count, plan->capture_count) ||
            !range_valid(function->block_begin, function->block_count, plan->block_count) ||
            function->parameter_begin != parameter_cursor ||
            function->capture_begin != capture_cursor || function->block_begin != block_cursor ||
            function->value_begin != value_cursor)
            XR_FUNCTION_FAIL("XR_SEM_0013", "function table range is invalid");
        if ((plan->types[function->return_type].flags & XR_SEM_TYPE_REFERENCE_CAPABLE) != 0 &&
            (function->return_provenance == XR_SEM_RETURN_NONE ||
             function->return_provenance > XR_SEM_RETURN_BORROWED_STATIC))
            XR_FUNCTION_FAIL("XR_OWN_3000", "reference-capable return has unknown provenance");
        for (uint16_t p = 0; p < function->parameter_count; p++) {
            const XrSemanticParameterRecord *parameter =
                &plan->parameters[function->parameter_begin + p];
            uint8_t allowed_flags = XR_SEM_PARAMETER_REQUIRED | XR_SEM_PARAMETER_VARIADIC |
                                    XR_SEM_PARAMETER_RECEIVER_BORROWED |
                                    XR_SEM_PARAMETER_READ_PLACE;
            if (!verify_id(parameter->canonical_key, parameter->id) || parameter->function != i ||
                parameter->ordinal != p || parameter->type >= plan->type_count ||
                parameter->value < function->value_begin ||
                parameter->value >= function->value_begin + function->value_count ||
                !xr_param_mode_is_valid((XrParamMode) parameter->mode) ||
                parameter->ownership > XI_OWN_BORROWED || parameter->reserved != 0 ||
                (parameter->flags & (uint8_t) ~allowed_flags) != 0 ||
                ((parameter->flags & XR_SEM_PARAMETER_VARIADIC) != 0 &&
                 p + 1u != function->parameter_count))
                XR_FUNCTION_FAIL("XR_SEM_0013", "parameter contract is invalid");
        }
        for (uint16_t c = 0; c < function->capture_count; c++) {
            const XrSemanticCaptureRecord *capture = &plan->captures[function->capture_begin + c];
            uint8_t allowed_flags =
                XR_SEM_CAPTURE_NEEDS_CELL | XR_SEM_CAPTURE_MUTABLE | XR_SEM_CAPTURE_REASSIGNED;
            if (!verify_id(capture->canonical_key, capture->id) || !capture->name ||
                capture->function != i || capture->ordinal != c ||
                capture->source_function != function->parent || capture->type >= plan->type_count ||
                capture->source_type >= plan->type_count ||
                capture->source > XR_SEM_CAPTURE_PARENT_CAPTURE ||
                capture->kind > XR_SEM_CAPTURE_SHARED ||
                capture->storage_domain == XR_STORAGE_DOMAIN_UNKNOWN ||
                capture->storage_domain > XR_STORAGE_FOREIGN ||
                capture->value_capability >= XR_SEM_VALUE_CAPABILITY_UNKNOWN ||
                capture->reserved[0] != 0 || (capture->flags & (uint8_t) ~allowed_flags) != 0)
                XR_FUNCTION_FAIL("XR_SEM_0018", "capture contract is invalid");
            if (capture->source == XR_SEM_CAPTURE_LOCAL_VALUE) {
                if (capture->source_capture != XR_SEMANTIC_INDEX_NONE ||
                    function->parent == XR_SEMANTIC_INDEX_NONE)
                    XR_FUNCTION_FAIL("XR_SEM_0018", "capture local source is invalid");
                const XrSemanticFunctionRecord *parent = &plan->functions[function->parent];
                if (capture->source_value < parent->value_begin ||
                    capture->source_value >= parent->value_begin + parent->value_count ||
                    capture->source_index != capture->source_value - parent->value_begin)
                    XR_FUNCTION_FAIL("XR_SEM_0018", "capture local value is invalid");
            } else {
                if (capture->source_value != XR_SEMANTIC_INDEX_NONE ||
                    function->parent == XR_SEMANTIC_INDEX_NONE)
                    XR_FUNCTION_FAIL("XR_SEM_0018", "capture parent link is invalid");
                const XrSemanticFunctionRecord *parent = &plan->functions[function->parent];
                if (capture->source_index >= parent->capture_count ||
                    capture->source_capture != parent->capture_begin + capture->source_index ||
                    plan->captures[capture->source_capture].type != capture->type ||
                    plan->captures[capture->source_capture].source_type != capture->source_type)
                    XR_FUNCTION_FAIL("XR_SEM_0018", "capture parent contract is invalid");
            }
        }
        if (UINT32_MAX - parameter_cursor < function->parameter_count ||
            UINT32_MAX - capture_cursor < function->capture_count ||
            UINT32_MAX - block_cursor < function->block_count ||
            UINT32_MAX - value_cursor < function->value_count)
            XR_FUNCTION_FAIL("XR_EXEC_5003", "function index budget exhausted");
        parameter_cursor += function->parameter_count;
        capture_cursor += function->capture_count;
        block_cursor += function->block_count;
        value_cursor += function->value_count;
    }
    for (uint32_t i = 0; i < plan->function_count; i++) {
        if (child_counts[i] != plan->functions[i].child_count)
            XR_FUNCTION_FAIL("XR_SEM_0013", "function child relation is invalid");
    }
    for (uint32_t c = 0; c < plan->source_class_count; c++) {
        for (uint16_t m = 0; m < plan->source_classes[c].method_count; m++)
            if (source_members[c][m] != 1)
                XR_FUNCTION_FAIL("XR_SEM_0013", "source method coverage is incomplete");
    }
    for (uint32_t c = 0; c < plan->source_class_count; c++)
        xr_free(source_members[c]);
    xr_free(source_members);
    xr_free(child_counts);
    if (parameter_cursor != plan->parameter_count || capture_cursor != plan->capture_count ||
        block_cursor != plan->block_count)
        return report(error, error_size, "XR_SEM_0013",
                      "function tables are not exactly partitioned");
#undef XR_FUNCTION_FAIL
    return true;
}

static bool predecessor_contains(const XrSemanticPlan *plan, uint32_t block, uint32_t predecessor) {
    const XrSemanticBlockRecord *record = &plan->blocks[block];
    for (uint16_t i = 0; i < record->predecessor_count; i++) {
        if (plan->predecessors[record->predecessor_begin + i] == predecessor)
            return true;
    }
    return false;
}

#define XR_SEM_BLOCK_EDGE_SUCCESSOR_0 (1u << 0)
#define XR_SEM_BLOCK_EDGE_SUCCESSOR_1 (1u << 1)
#define XR_SEM_OPERATION_EDGE_ERROR (1u << 0)
#define XR_SEM_OPERATION_EDGE_PANIC (1u << 1)

static bool verify_blocks(const XrSemanticPlan *plan, const uint8_t *edge_mask, char *error,
                          size_t error_size) {
    uint32_t operation_cursor = 0;
    uint32_t predecessor_cursor = 0;
    for (uint32_t i = 0; i < plan->block_count; i++) {
        const XrSemanticBlockRecord *block = &plan->blocks[i];
        if (!verify_id(block->canonical_key, block->id) ||
            block->function >= plan->function_count || block->kind > XI_BLOCK_UNREACHABLE ||
            !range_valid(block->operation_begin, block->operation_count, plan->operation_count) ||
            !range_valid(block->predecessor_begin, block->predecessor_count,
                         plan->predecessor_count) ||
            block->operation_begin != operation_cursor ||
            block->predecessor_begin != predecessor_cursor)
            return report(error, error_size, "XR_SEM_0014", "block record is invalid");
        for (unsigned s = 0; s < 2; s++) {
            uint32_t successor = block->successors[s];
            if (successor == XR_SEMANTIC_INDEX_NONE)
                continue;
            if (successor >= plan->block_count ||
                plan->blocks[successor].function != block->function ||
                !predecessor_contains(plan, successor, i) ||
                (edge_mask[i] & (uint8_t) (1u << s)) == 0)
                return report(error, error_size, "XR_SEM_0014", "CFG edge is not symmetric");
        }
        for (uint16_t p = 0; p < block->predecessor_count; p++) {
            uint32_t predecessor = plan->predecessors[block->predecessor_begin + p];
            if (predecessor >= plan->block_count ||
                plan->blocks[predecessor].function != block->function) {
                if (error && error_size)
                    snprintf(error, error_size,
                             "XR_SEM_0014: SSA predecessor belongs to another function "
                             "(function=%u block=%u predecessor=%u)",
                             block->function, i, predecessor);
                return false;
            }
        }
        if (UINT32_MAX - operation_cursor < block->operation_count ||
            UINT32_MAX - predecessor_cursor < block->predecessor_count)
            return report(error, error_size, "XR_EXEC_5003", "block index budget exhausted");
        operation_cursor += block->operation_count;
        predecessor_cursor += block->predecessor_count;
    }
    if (operation_cursor != plan->operation_count || predecessor_cursor != plan->predecessor_count)
        return report(error, error_size, "XR_SEM_0014",
                      "block table does not exactly partition operation and predecessor tables");
    return true;
}

static bool verify_edges(const XrSemanticPlan *plan, uint8_t *block_edge_mask,
                         uint8_t *operation_edge_mask, char *error, size_t error_size) {
    for (uint32_t i = 0; i < plan->edge_count; i++) {
        const XrSemanticEdgeRecord *edge = &plan->edges[i];
        if (!verify_id(edge->canonical_key, edge->id) || edge->function >= plan->function_count ||
            edge->from_block >= plan->block_count || edge->to_block >= plan->block_count ||
            edge->kind > XR_SEM_EDGE_RESUME ||
            plan->blocks[edge->from_block].function != edge->function ||
            plan->blocks[edge->to_block].function != edge->function ||
            ((edge->kind == XR_SEM_EDGE_NORMAL || edge->kind == XR_SEM_EDGE_ERROR) &&
             !predecessor_contains(plan, edge->to_block, edge->from_block)))
            return report(error, error_size, "XR_SEM_0010",
                          "semantic control-edge record is invalid");
        const XrSemanticBlockRecord *source = &plan->blocks[edge->from_block];
        if (edge->kind == XR_SEM_EDGE_NORMAL) {
            if (edge->flags != 0 || edge->operation != XR_SEMANTIC_INDEX_NONE ||
                (source->successors[0] != edge->to_block &&
                 source->successors[1] != edge->to_block))
                return report(error, error_size, "XR_SEM_0010",
                              "normal semantic edge disagrees with the canonical CFG");
        } else {
            if (edge->operation >= plan->operation_count ||
                plan->operations[edge->operation].function != edge->function ||
                plan->operations[edge->operation].block != edge->from_block)
                return report(error, error_size, "XR_SEM_0010",
                              "exceptional semantic edge has no valid source operation");
        }
        if (edge->kind == XR_SEM_EDGE_ERROR) {
            const XrSemanticOperationRecord *operation = &plan->operations[edge->operation];
            bool checked_branch = operation->opcode == XI_ERR_CHECK &&
                                  source->control_value == operation->result_value &&
                                  source->successors[0] == edge->to_block;
            bool explicit_error =
                operation->opcode == XI_ERR_SET && (source->successors[0] == edge->to_block ||
                                                    source->successors[1] == edge->to_block);
            if (edge->flags != 0 || (!checked_branch && !explicit_error))
                return report(error, error_size, "XR_SEM_0010",
                              "error edge is not backed by an explicit error-channel operation");
            if ((operation_edge_mask[edge->operation] & XR_SEM_OPERATION_EDGE_ERROR) != 0)
                return report(error, error_size, "XR_SEM_0010",
                              "operation has duplicate explicit error edges");
            operation_edge_mask[edge->operation] |= XR_SEM_OPERATION_EDGE_ERROR;
        }
        if (edge->kind == XR_SEM_EDGE_PANIC) {
            if (edge->flags != XR_SEM_EDGE_HANDLER_SCOPE ||
                plan->operations[edge->operation].opcode != XI_TRY ||
                plan->operations[edge->operation].evidence[7] != edge->to_block)
                return report(error, error_size, "XR_SEM_0010",
                              "panic edge is not backed by its try-handler operation");
            if ((operation_edge_mask[edge->operation] & XR_SEM_OPERATION_EDGE_PANIC) != 0)
                return report(error, error_size, "XR_SEM_0010",
                              "operation has duplicate explicit panic edges");
            operation_edge_mask[edge->operation] |= XR_SEM_OPERATION_EDGE_PANIC;
        }
        if (edge->kind != XR_SEM_EDGE_NORMAL && edge->kind != XR_SEM_EDGE_ERROR &&
            edge->kind != XR_SEM_EDGE_PANIC)
            return report(error, error_size, "XR_SEM_0010",
                          "semantic edge kind has no implemented contract");
        if (edge->kind == XR_SEM_EDGE_NORMAL || edge->kind == XR_SEM_EDGE_ERROR) {
            uint8_t mask = 0;
            if (source->successors[0] == edge->to_block)
                mask |= XR_SEM_BLOCK_EDGE_SUCCESSOR_0;
            if (source->successors[1] == edge->to_block)
                mask |= XR_SEM_BLOCK_EDGE_SUCCESSOR_1;
            if (mask == 0 || (block_edge_mask[edge->from_block] & mask) != 0)
                return report(error, error_size, "XR_SEM_0010",
                              "CFG successor has missing or duplicate semantic edges");
            block_edge_mask[edge->from_block] |= mask;
        }
    }
    return true;
}

static bool verify_ssa_use(const XrSemanticPlan *plan, const XrSemanticGraph *graph,
                           const uint32_t *definitions, uint32_t operation_index,
                           uint16_t operand_index, char *error, size_t error_size) {
    const XrSemanticOperationRecord *operation = &plan->operations[operation_index];
    uint32_t value = plan->operands[operation->operand_begin + operand_index].value;
    uint32_t definition_index = definitions[value];
    if (definition_index >= plan->operation_count) {
        uint32_t parameter_index = definition_index - plan->operation_count;
        if (parameter_index >= plan->parameter_count ||
            plan->parameters[parameter_index].function != operation->function)
            return report(error, error_size, "XR_SEM_0016",
                          "SSA parameter use crosses a function boundary");
        return true;
    }
    const XrSemanticOperationRecord *definition = &plan->operations[definition_index];
    if (definition->function != operation->function)
        return report(error, error_size, "XR_SEM_0016", "SSA use crosses a function boundary");

    if (operation->opcode == XI_PHI) {
        const XrSemanticBlockRecord *block = &plan->blocks[operation->block];
        if (operation->operand_count != block->predecessor_count)
            return report(error, error_size, "XR_SEM_0016",
                          "PHI operands do not match SSA predecessor slots");
        uint32_t predecessor = plan->predecessors[block->predecessor_begin + operand_index];
        if (!xr_semantic_graph_is_reachable(graph, operation->block))
            return true;
        /* Braun construction and panic lowering may retain an SSA-only
         * predecessor slot after the executable edge becomes unreachable.
         * Such an input can never be selected at runtime, so dominance is
         * defined only for reachable incoming edges.  The slot and operand
         * remain frozen for exact SSA reconstruction. */
        if (!xr_semantic_graph_is_reachable(graph, predecessor))
            return true;
        if (!xr_semantic_graph_dominates(graph, definition->block, predecessor))
            return report(error, error_size, "XR_SEM_0016",
                          "PHI input definition does not dominate its incoming edge");
        return true;
    }

    if (!xr_semantic_graph_is_reachable(graph, operation->block))
        return true;
    if (!xr_semantic_graph_dominates(graph, definition->block, operation->block))
        return report(error, error_size, "XR_SEM_0016", "SSA definition does not dominate its use");
    if (definition->block == operation->block && definition_index >= operation_index)
        return report(error, error_size, "XR_SEM_0016",
                      "same-block SSA definition does not precede its use");
    return true;
}

static bool verify_operand_contract(const XrSemanticOperationRecord *operation,
                                    const XrSemanticOperandRecord *operand, uint16_t index,
                                    char *error, size_t error_size) {
    uint8_t expected_role = XR_SEM_OPERAND_VALUE;
    int16_t expected_parameter = -1;
    bool call_contract = false;
    if (operation->opcode == XI_CALL || operation->opcode == XI_TAIL_CALL) {
        expected_role = index == 0 ? XR_SEM_OPERAND_CALLEE : XR_SEM_OPERAND_ARGUMENT;
        if (index > 0) {
            expected_parameter = (int16_t) (index - 1);
            call_contract = true;
        }
    } else if (operation->opcode == XI_CALL_METHOD || operation->opcode == XI_CALL_METHOD_DIRECT) {
        expected_role = index == 0 ? XR_SEM_OPERAND_RECEIVER : XR_SEM_OPERAND_ARGUMENT;
        expected_parameter = index == 0 ? -1 : (int16_t) (index - 1);
        call_contract = true;
    } else if (operation->opcode == XI_CALL_BUILTIN) {
        expected_role = XR_SEM_OPERAND_ARGUMENT;
        expected_parameter = (int16_t) index;
        call_contract = true;
    }
    if (operand->role != expected_role || operand->parameter != expected_parameter ||
        operand->role >= XR_SEM_OPERAND_ROLE_COUNT || operand->transfer_mode > XR_TRANSFER_MOVE ||
        operand->ownership_action > XR_SEM_OPERAND_CONSUME ||
        !xr_param_mode_is_valid((XrParamMode) operand->parameter_mode) ||
        !xr_call_arg_access_is_valid((XrCallArgAccess) operand->access) ||
        operand->origin > XI_PLACE_ORIGIN_PROJECTION_TEMP ||
        operand->lifetime > XI_PLACE_LIFETIME_CALL_BOUND ||
        operand->escape > XI_PLACE_ESCAPE_THREAD ||
        (operand->flags & ~(XR_SEM_OPERAND_CALL_CONTRACT | XR_SEM_OPERAND_ADDRESSABLE)) != 0 ||
        ((operand->flags & XR_SEM_OPERAND_CALL_CONTRACT) != 0) != call_contract)
        return report(error, error_size, "XR_SEM_0018", "typed operand contract is invalid");
    if (!call_contract &&
        (operand->parameter_mode != XR_PARAM_READ || operand->access != XR_CALL_ARG_PLAIN ||
         operand->origin != XI_PLACE_ORIGIN_NONE || operand->lifetime != XI_PLACE_LIFETIME_NONE ||
         operand->escape != XI_PLACE_ESCAPE_NONE ||
         (operand->flags & XR_SEM_OPERAND_ADDRESSABLE) != 0))
        return report(error, error_size, "XR_SEM_0018",
                      "non-call operand carries a call-bound contract");
    if ((operand->flags & XR_SEM_OPERAND_ADDRESSABLE) == 0 &&
        (operand->origin != XI_PLACE_ORIGIN_NONE || operand->lifetime != XI_PLACE_LIFETIME_NONE))
        return report(error, error_size, "XR_SEM_0018",
                      "non-addressable operand carries place provenance");
    return true;
}

static bool build_definition_map(const XrSemanticPlan *plan, uint32_t **out,
                                 uint32_t *out_value_count, char *error, size_t error_size) {
    uint32_t value_count = 0;
    for (uint32_t f = 0; f < plan->function_count; f++) {
        uint64_t end = (uint64_t) plan->functions[f].value_begin + plan->functions[f].value_count;
        if (end > UINT32_MAX)
            return report(error, error_size, "XR_EXEC_5003", "SSA value index budget exhausted");
        if (end > value_count)
            value_count = (uint32_t) end;
    }
    uint32_t *definitions = (uint32_t *) xr_malloc((size_t) value_count * sizeof(*definitions));
    if (value_count && !definitions)
        return report(error, error_size, "XR_EXEC_5003", "SSA verifier budget exhausted");
    for (uint32_t value = 0; value < value_count; value++)
        definitions[value] = XR_SEMANTIC_INDEX_NONE;
    if (plan->operation_count > UINT32_MAX - plan->parameter_count) {
        xr_free(definitions);
        return report(error, error_size, "XR_EXEC_5003",
                      "SSA parameter definition index budget exhausted");
    }
    for (uint32_t i = 0; i < plan->parameter_count; i++) {
        const XrSemanticParameterRecord *parameter = &plan->parameters[i];
        if (parameter->value >= value_count ||
            definitions[parameter->value] != XR_SEMANTIC_INDEX_NONE) {
            xr_free(definitions);
            return report(error, error_size, "XR_SEM_0013",
                          "parameter SSA definition is duplicated");
        }
        definitions[parameter->value] = plan->operation_count + i;
    }
    *out = definitions;
    *out_value_count = value_count;
    return true;
}

static bool semantic_type_is_exact_string_builder(const XrSemanticTypeRecord *type) {
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

static bool semantic_type_is_exact_string(const XrSemanticTypeRecord *type) {
    return type && type->kind == XR_KIND_STRING && type->builtin_type == XR_TID_NULL &&
           type->child_count == 0 && type->aggregate_extent == 0 && type->aggregate_align == 0 &&
           type->scalar_rep == XR_SCALAR_REP_NONE &&
           (type->flags & (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT)) ==
               (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT) &&
           (type->flags & ~(XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT |
                            XR_SEM_TYPE_CONST | XR_SEM_TYPE_NULLABLE)) == 0;
}

static bool semantic_type_is_exact_u8(const XrSemanticTypeRecord *type) {
    return type && type->kind == XR_KIND_INT && type->builtin_type == XR_TID_NULL &&
           type->child_count == 0 && type->aggregate_extent == 0 && type->aggregate_align == 0 &&
           type->scalar_rep == XR_NATIVE_U8 && type->flags == 0;
}

static bool semantic_type_is_exact_u8_slice(const XrSemanticPlan *plan, uint32_t type_index,
                                            uint32_t element_type) {
    const XrSemanticTypeRecord *type =
        type_index < plan->type_count ? &plan->types[type_index] : NULL;
    return type && type->kind == XR_KIND_SLICE && type->builtin_type == XR_TID_NULL &&
           type->child_count == 1 && type->child_begin < plan->type_child_count &&
           plan->type_children[type->child_begin] == element_type && type->aggregate_extent == 0 &&
           type->aggregate_align == 0 && type->scalar_rep == XR_SCALAR_REP_NONE &&
           type->flags == (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_BORROW_VIEW);
}

static bool verify_string_byte_slice_view(const XrSemanticPlan *plan,
                                          const XrSemanticOperationRecord *operation, char *error,
                                          size_t error_size) {
    bool candidate = operation->intrinsic_kind == XR_SEM_INTRINSIC_STRING_BYTE_SLICE_VIEW ||
                     operation->evidence[1] == XA_INTRINSIC_STRING_BYTE_SLICE_VIEW;
    if (!candidate) {
        bool empty =
            operation->intrinsic_kind != XR_SEM_INTRINSIC_STRING_BYTE_SLICE_VIEW &&
            operation->view_source_value == XR_SEMANTIC_INDEX_NONE &&
            operation->view_element_type == XR_SEMANTIC_INDEX_NONE &&
            operation->view_source_operand == -1 && operation->view_source_parameter == -1 &&
            operation->view_origin == XI_VIEW_ORIGIN_NONE && operation->view_capability == 0 &&
            operation->view_lifetime == 0 && operation->view_complete == 0 &&
            operation->reserved_view[0] == 0 && operation->reserved_view[1] == 0;
        return empty || report(error, error_size, "XR_SEM_0019",
                               "non-view operation carries string byte-slice authority");
    }
    const XrSemanticOperandRecord *source =
        operation->operand_count == 1 && operation->operand_begin < plan->operand_count
            ? &plan->operands[operation->operand_begin]
            : NULL;
    const XrSemanticTypeRecord *source_type =
        source && source->type < plan->type_count ? &plan->types[source->type] : NULL;
    const XrSemanticTypeRecord *element_type = operation->view_element_type < plan->type_count
                                                   ? &plan->types[operation->view_element_type]
                                                   : NULL;
    bool exact = operation->opcode == XI_CALL_BUILTIN && source &&
                 source->value == operation->view_source_value && source->parameter == 0 &&
                 source->role == XR_SEM_OPERAND_ARGUMENT &&
                 (source->flags & XR_SEM_OPERAND_CALL_CONTRACT) != 0 &&
                 semantic_type_is_exact_string(source_type) &&
                 semantic_type_is_exact_u8(element_type) &&
                 semantic_type_is_exact_u8_slice(plan, operation->result_type,
                                                 operation->view_element_type) &&
                 operation->intrinsic_kind == XR_SEM_INTRINSIC_STRING_BYTE_SLICE_VIEW &&
                 operation->evidence[1] == XA_INTRINSIC_STRING_BYTE_SLICE_VIEW &&
                 operation->view_source_operand == 0 && operation->view_source_parameter == -1 &&
                 operation->view_origin == XI_VIEW_ORIGIN_RECEIVER &&
                 operation->view_capability == 1 && operation->view_lifetime == 1 &&
                 operation->view_complete == 1 && operation->array_element_storage == 0 &&
                 operation->reserved_view[0] == 0 && operation->reserved_view[1] == 0;
    if (!exact && error && error_size) {
        snprintf(
            error, error_size,
            "XR_SEM_0019: string byte-slice view authority is not exact "
            "(op=%u operands=%u source=%u view_source=%u parameter=%d role=%u flags=%u "
            "source_kind=%u source_scalar=%u source_flags=%u element_kind=%u "
            "element_scalar=%u element_flags=%u result=%u:%u:%u:%u intrinsic=%u evidence=%u)",
            operation->opcode, operation->operand_count, source ? source->value : UINT32_MAX,
            operation->view_source_value, source ? (int) source->parameter : INT32_MIN,
            source ? (unsigned) source->role : UINT32_MAX,
            source ? (unsigned) source->flags : UINT32_MAX,
            source_type ? source_type->kind : UINT32_MAX,
            source_type ? source_type->scalar_rep : UINT32_MAX,
            source_type ? source_type->flags : UINT32_MAX,
            element_type ? element_type->kind : UINT32_MAX,
            element_type ? element_type->scalar_rep : UINT32_MAX,
            element_type ? element_type->flags : UINT32_MAX, operation->result_type,
            operation->result_type < plan->type_count ? plan->types[operation->result_type].kind
                                                      : UINT32_MAX,
            operation->result_type < plan->type_count
                ? plan->types[operation->result_type].scalar_rep
                : UINT32_MAX,
            operation->result_type < plan->type_count ? plan->types[operation->result_type].flags
                                                      : UINT32_MAX,
            (unsigned) operation->intrinsic_kind, operation->evidence[1]);
    }
    return exact;
}

static bool string_builder_append_result_is_exact(const XrSemanticPlan *plan,
                                                  const XrSemanticOperationRecord *operation) {
    const XrSemanticFunctionRecord *function =
        operation ? xr_semantic_plan_function(plan, operation->function) : NULL;
    return operation &&
           ((operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_OWNED &&
             operation->return_parameter == -1 &&
             ((operation->return_provenance == XR_SEM_RETURN_OWNED &&
               operation->return_complete == 1) ||
              (operation->return_provenance == XR_SEM_RETURN_NONE &&
               operation->return_complete == 0))) ||
            (operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_BORROWED &&
             ((((operation->return_provenance == XR_SEM_RETURN_BORROWED_STATIC &&
                 operation->return_parameter == -1) ||
                (operation->return_provenance == XR_SEM_RETURN_BORROWED_PARAM && function &&
                 operation->return_parameter >= 0 &&
                 (uint16_t) operation->return_parameter < function->parameter_count)) &&
               operation->return_complete == 1) ||
              (operation->return_provenance == XR_SEM_RETURN_NONE &&
               operation->return_parameter == -1 && operation->return_complete == 0))));
}

static bool verify_string_builder_append_rune(const XrSemanticPlan *plan,
                                              const XrSemanticOperationRecord *operation,
                                              char *error, size_t error_size) {
    bool candidate = operation->intrinsic_kind == XR_SEM_INTRINSIC_STRINGBUILDER_APPEND_RUNE;
    if (!candidate)
        return true;
    const XrSemanticOperandRecord *receiver =
        operation->operand_count == 2 && operation->operand_begin < plan->operand_count
            ? &plan->operands[operation->operand_begin]
            : NULL;
    const XrSemanticOperandRecord *argument = receiver ? receiver + 1 : NULL;
    const XrSemanticTypeRecord *receiver_type =
        receiver && receiver->type < plan->type_count ? &plan->types[receiver->type] : NULL;
    const XrSemanticTypeRecord *argument_type =
        argument && argument->type < plan->type_count ? &plan->types[argument->type] : NULL;
    const char *selector =
        operation->metadata_count == 1 && operation->metadata_begin < plan->metadata_count
            ? plan->metadata[operation->metadata_begin]
            : NULL;
    bool exact =
        operation->opcode == XI_CALL_METHOD &&
        operation->semantic_immediate == (int64_t) XI_METHOD_SYMBOL_APPEND << 1 &&
        operation->evidence[1] == XA_INTRINSIC_STRING_BUILDER_APPEND && selector &&
        strcmp(selector, XA_INTRINSIC_STRING_BUILDER_APPEND_SOURCE_MEMBER) == 0 && receiver &&
        argument && semantic_type_is_exact_string_builder(receiver_type) &&
        operation->result_type == receiver->type && argument_type &&
        argument_type->kind == XR_KIND_RUNE && argument_type->builtin_type == XR_TID_NULL &&
        argument_type->child_count == 0 && argument_type->scalar_rep == XR_SCALAR_REP_NONE &&
        argument_type->flags == 0 && receiver->role == XR_SEM_OPERAND_RECEIVER &&
        receiver->parameter == -1 && receiver->flags == XR_SEM_OPERAND_CALL_CONTRACT &&
        argument->role == XR_SEM_OPERAND_ARGUMENT && argument->parameter == 0 &&
        argument->flags == XR_SEM_OPERAND_CALL_CONTRACT && operation->result_alias_operand == 0 &&
        string_builder_append_result_is_exact(plan, operation);
    return exact || report(error, error_size, "XR_SEM_0019",
                           "StringBuilder.append(rune) authority is not exact");
}

static bool verify_string_runes(const XrSemanticPlan *plan,
                                const XrSemanticOperationRecord *operation, char *error,
                                size_t error_size) {
    if (operation->intrinsic_kind != XR_SEM_INTRINSIC_STRING_RUNES)
        return true;
    return xr_semantic_string_runes_is_exact(plan, operation, NULL) ||
           report(error, error_size, "XR_SEM_0019", "String.runes authority is not exact");
}

static bool verify_iterator_rune_has_next(const XrSemanticPlan *plan,
                                          const XrSemanticOperationRecord *operation, char *error,
                                          size_t error_size) {
    if (operation->intrinsic_kind != XR_SEM_INTRINSIC_ITERATOR_RUNE_HAS_NEXT)
        return true;
    return xr_semantic_iterator_rune_has_next_is_exact(plan, operation, NULL) ||
           report(error, error_size, "XR_SEM_0019",
                  "Iterator<rune>.hasNext authority is not exact");
}

static bool verify_iterator_rune_next(const XrSemanticPlan *plan,
                                      const XrSemanticOperationRecord *operation, char *error,
                                      size_t error_size) {
    if (operation->intrinsic_kind != XR_SEM_INTRINSIC_ITERATOR_RUNE_NEXT)
        return true;
    return xr_semantic_iterator_rune_next_is_exact(plan, operation, NULL) ||
           report(error, error_size, "XR_SEM_0019", "Iterator<rune>.next authority is not exact");
}

static bool verify_map_entry_iterator(const XrSemanticPlan *plan,
                                      const XrSemanticOperationRecord *operation, char *error,
                                      size_t error_size) {
    bool exact = operation->intrinsic_kind == XR_SEM_INTRINSIC_MAP_ENTRIES_ITERATOR
                     ? xr_semantic_map_entries_iterator_is_exact(plan, operation, NULL, NULL)
                 : operation->intrinsic_kind == XR_SEM_INTRINSIC_MAP_ENTRY_ITERATOR_HAS_NEXT
                     ? xr_semantic_map_entry_iterator_has_next_is_exact(plan, operation, NULL)
                 : operation->intrinsic_kind == XR_SEM_INTRINSIC_MAP_ENTRY_ITERATOR_NEXT
                     ? xr_semantic_map_entry_iterator_next_is_exact(plan, operation, NULL)
                     : true;
    return exact ||
           report(error, error_size, "XR_SEM_0019", "Map entry iterator authority is not exact");
}

static bool rune_to_uint32_type_is_exact_verify(const XrSemanticTypeRecord *type, uint16_t kind,
                                                uint8_t scalar_rep, const char *canonical_key) {
    XrStableId zero = {{0}};
    return type && type->kind == kind && type->builtin_type == XR_TID_NULL &&
           type->source_class == XR_SEMANTIC_INDEX_NONE &&
           xr_stable_id_equal(type->source_class_identity, zero) &&
           xr_stable_id_equal(type->source_enum_identity, zero) && !type->source_enum_key &&
           type->child_count == 0 && type->scalar_rep == scalar_rep &&
           type->aggregate_extent == 0 && type->aggregate_align == 0 && type->enum_layout_id == 0 &&
           type->enum_member_count == 0 && type->enum_flags == 0 && type->reserved_enum == 0 &&
           type->flags == 0 && type->canonical_key &&
           strcmp(type->canonical_key, canonical_key) == 0;
}

/* Independent of the builder's shape helper: a bad producer/helper change
 * cannot make the verifier accept the same fabricated answer. */
static bool rune_to_uint32_is_exact_verify(const XrSemanticPlan *plan,
                                           const XrSemanticOperationRecord *operation) {
    uint32_t operand_count = 0;
    uint32_t metadata_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    const char *const *metadata = xr_semantic_plan_metadata(plan, &metadata_count);
    if (!plan || !operation || !operands || !metadata ||
        operation->intrinsic_kind != XR_SEM_INTRINSIC_RUNE_TO_UINT32 ||
        operation->opcode != XI_CALL_METHOD ||
        operation->semantic_immediate != (int64_t) XI_METHOD_SYMBOL_TO_UINT32 << 1 ||
        operation->operand_count != 1 || operation->operand_begin >= operand_count ||
        operation->metadata_count != 1 || operation->metadata_begin >= metadata_count ||
        strcmp(metadata[operation->metadata_begin], "toUInt32") != 0 ||
        operation->auxiliary_kind != XI_AUX_KIND_NONE ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE ||
        operation->import_resolution != XR_SEM_IMPORT_RESOLUTION_NONE ||
        operation->effects != xi_generated_op_effects(XI_CALL_METHOD) ||
        (operation->flags != xi_generated_op_default_flags(XI_CALL_METHOD) &&
         operation->flags != (xi_generated_op_default_flags(XI_CALL_METHOD) | XI_FLAG_TAIL)) ||
        operation->ownership_use != xi_generated_op_own_use(XI_CALL_METHOD) ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_CALL_RESULT ||
        operation->transfer_mode != XR_TRANSFER_SHARE ||
        operation->parameter_mode != XR_PARAM_READ ||
        operation->parameter_ownership != XI_OWN_NONE || operation->result_alias_operand != -1 ||
        operation->return_parameter != -1 || operation->return_provenance != XR_SEM_RETURN_NONE ||
        operation->return_complete != 0 || operation->view_source_value != XR_SEMANTIC_INDEX_NONE ||
        operation->view_element_type != XR_SEMANTIC_INDEX_NONE ||
        operation->view_source_operand != -1 || operation->view_source_parameter != -1 ||
        operation->view_origin != XI_VIEW_ORIGIN_NONE || operation->view_capability != 0 ||
        operation->view_lifetime != 0 || operation->view_complete != 0 ||
        operation->reserved_view[0] != 0 || operation->reserved_view[1] != 0)
        return false;
    const XrSemanticOperandRecord *receiver = &operands[operation->operand_begin];
    return rune_to_uint32_type_is_exact_verify(xr_semantic_plan_type(plan, receiver->type),
                                               XR_KIND_RUNE, XR_SCALAR_REP_NONE,
                                               "type-v3:24:0:0:0:0:0:0:0:0:255:0:") &&
           rune_to_uint32_type_is_exact_verify(xr_semantic_plan_type(plan, operation->result_type),
                                               XR_KIND_INT, XR_NATIVE_U32,
                                               "type-v3:0:0:0:0:0:0:0:0:0:8:0:") &&
           receiver->role == XR_SEM_OPERAND_RECEIVER && receiver->parameter == -1 &&
           receiver->transfer_mode == XR_TRANSFER_SHARE &&
           receiver->ownership_action == XR_SEM_OPERAND_BORROW && receiver->parameter_mode == 0 &&
           receiver->access == 0 && receiver->origin == 0 && receiver->lifetime == 0 &&
           receiver->escape == 0 && receiver->flags == XR_SEM_OPERAND_CALL_CONTRACT;
}

static bool verify_rune_to_uint32(const XrSemanticPlan *plan,
                                  const XrSemanticOperationRecord *operation, char *error,
                                  size_t error_size) {
    if (operation->intrinsic_kind != XR_SEM_INTRINSIC_RUNE_TO_UINT32)
        return true;
    return rune_to_uint32_is_exact_verify(plan, operation) ||
           report(error, error_size, "XR_SEM_0019", "rune.toUInt32 authority is not exact");
}

static bool verify_rune_is_whitespace(const XrSemanticPlan *plan,
                                      const XrSemanticOperationRecord *operation, char *error,
                                      size_t error_size) {
    if (operation->intrinsic_kind != XR_SEM_INTRINSIC_RUNE_IS_WHITESPACE)
        return true;
    return xr_semantic_rune_is_whitespace_is_exact(plan, operation, NULL) ||
           report(error, error_size, "XR_SEM_0019", "rune.isWhitespace authority is not exact");
}

static bool verify_string_slice_range(const XrSemanticPlan *plan,
                                      const XrSemanticOperationRecord *operation, char *error,
                                      size_t error_size) {
    if (operation->intrinsic_kind != XR_SEM_INTRINSIC_STRING_SLICE_RANGE)
        return true;
    return xr_semantic_string_slice_range_is_exact(plan, operation, NULL, NULL, NULL) ||
           report(error, error_size, "XR_SEM_0019",
                  "String.slice(start, end) authority is not exact");
}

static bool verify_string_builder_to_string(const XrSemanticPlan *plan,
                                            const XrSemanticOperationRecord *operation, char *error,
                                            size_t error_size) {
    if (operation->intrinsic_kind != XR_SEM_INTRINSIC_STRINGBUILDER_TO_STRING)
        return true;
    const XrSemanticOperandRecord *receiver =
        operation->operand_count == 1 && operation->operand_begin < plan->operand_count
            ? &plan->operands[operation->operand_begin]
            : NULL;
    const XrSemanticTypeRecord *receiver_type =
        receiver && receiver->type < plan->type_count ? &plan->types[receiver->type] : NULL;
    const XrSemanticTypeRecord *result_type =
        operation->result_type < plan->type_count ? &plan->types[operation->result_type] : NULL;
    const char *selector =
        operation->metadata_count == 1 && operation->metadata_begin < plan->metadata_count
            ? plan->metadata[operation->metadata_begin]
            : NULL;
    bool exact =
        operation->opcode == XI_CALL_METHOD && operation->semantic_immediate > 0 &&
        (operation->semantic_immediate & 1) == 0 && selector && strcmp(selector, "toString") == 0 &&
        receiver && semantic_type_is_exact_string_builder(receiver_type) && result_type &&
        result_type->kind == XR_KIND_STRING && result_type->builtin_type == XR_TID_NULL &&
        result_type->child_count == 0 && result_type->scalar_rep == XR_SCALAR_REP_NONE &&
        result_type->flags == (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT) &&
        receiver->role == XR_SEM_OPERAND_RECEIVER && receiver->parameter == -1 &&
        receiver->flags == XR_SEM_OPERAND_CALL_CONTRACT && operation->result_alias_operand == -1 &&
        operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_OWNED &&
        operation->return_complete == 1;
    return exact || report(error, error_size, "XR_SEM_0019",
                           "StringBuilder.toString authority is not exact");
}

/* Recomputed from the frozen rows alone.  A class type record whose source
 * class index and identity are both absent can only be a compiler-owned
 * namespace: every source declaration records its own class authority.  The
 * frozen selector then names one implementation, which is what makes the
 * callsite an exact target instead of an open-domain method dispatch. */
static bool semantic_type_is_exact_json_namespace(const XrSemanticTypeRecord *type) {
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

static bool verify_json_namespace_value(const XrSemanticPlan *plan,
                                        const XrSemanticOperationRecord *operation, char *error,
                                        size_t error_size) {
    if (operation->intrinsic_kind != XR_SEM_INTRINSIC_JSON_NAMESPACE_VALUE)
        return true;
    const XrSemanticOperandRecord *receiver =
        operation->operand_count == 2 && operation->operand_begin + 1u < plan->operand_count
            ? &plan->operands[operation->operand_begin]
            : NULL;
    const XrSemanticOperandRecord *argument = receiver ? receiver + 1 : NULL;
    const XrSemanticTypeRecord *receiver_type =
        receiver && receiver->type < plan->type_count ? &plan->types[receiver->type] : NULL;
    const XrSemanticTypeRecord *result_type =
        operation->result_type < plan->type_count ? &plan->types[operation->result_type] : NULL;
    const char *selector =
        operation->metadata_count == 1 && operation->metadata_begin < plan->metadata_count
            ? plan->metadata[operation->metadata_begin]
            : NULL;
    bool exact = operation->opcode == XI_CALL_METHOD && operation->semantic_immediate > 0 &&
                 (operation->semantic_immediate & 1) == 0 && selector &&
                 strcmp(selector, "value") == 0 && receiver && argument &&
                 semantic_type_is_exact_json_namespace(receiver_type) && result_type &&
                 result_type->kind == XR_KIND_JSON && result_type->builtin_type == XR_TID_NULL &&
                 result_type->child_count == 0 && result_type->scalar_rep == XR_SCALAR_REP_NONE &&
                 receiver->role == XR_SEM_OPERAND_RECEIVER && receiver->parameter == -1 &&
                 receiver->flags == XR_SEM_OPERAND_CALL_CONTRACT &&
                 argument->role == XR_SEM_OPERAND_ARGUMENT && argument->parameter == 0 &&
                 argument->flags == XR_SEM_OPERAND_CALL_CONTRACT &&
                 operation->result_alias_operand == -1 &&
                 operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_OWNED &&
                 operation->return_complete == 1;
    return exact ||
           report(error, error_size, "XR_SEM_0019", "JSON.value namespace authority is not exact");
}

/* Recomputed from the frozen rows alone.  The array kind is produced only by
 * the builtin container type and carries neither a source class index nor a
 * source class identity, so no declaration can present this receiver record;
 * the frozen selector then names one implementation.  Each row below states the
 * whole shape one selector may present: the operand count range, which operand
 * carries the element, and what the result is. The element clause is checked
 * against the receiver's own element entry rather than a spelled type. The
 * verifier independently reconstructs the row's reference action and drop
 * lifecycle, including the exact consumed source-class element of tagged
 * `Array.push`. */

static bool verify_array_reserve(const XrSemanticPlan *plan,
                                 const XrSemanticOperationRecord *operation) {
    if (!plan || !operation || operation->evidence[1] != XA_INTRINSIC_ARRAY_RESERVE ||
        operation->opcode != XI_CALL_BUILTIN || operation->operand_count != 2 ||
        operation->operand_begin > plan->operand_count ||
        operation->operand_count > plan->operand_count - operation->operand_begin ||
        operation->metadata_count != 0 || operation->auxiliary_kind != XI_AUX_KIND_NONE ||
        operation->semantic_immediate != 0 ||
        operation->effects != xi_generated_op_effects(XI_CALL_BUILTIN))
        return false;
    const XrSemanticOperandRecord *receiver = &plan->operands[operation->operand_begin];
    const XrSemanticOperandRecord *capacity = receiver + 1;
    const XrSemanticTypeRecord *receiver_type =
        receiver->type < plan->type_count ? &plan->types[receiver->type] : NULL;
    const XrSemanticTypeRecord *capacity_type =
        capacity->type < plan->type_count ? &plan->types[capacity->type] : NULL;
    return xr_semantic_array_type_row_is_exact(receiver_type) &&
           xr_semantic_array_member_i64_type_is_exact(capacity_type) &&
           operation->result_type == receiver->type && operation->result_alias_operand == 0 &&
           operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_OWNED &&
           operation->return_provenance == XR_SEM_RETURN_OWNED &&
           operation->return_parameter == -1 && operation->return_complete == 1 &&
           receiver->role == XR_SEM_OPERAND_ARGUMENT && receiver->parameter == 0 &&
           receiver->flags == XR_SEM_OPERAND_CALL_CONTRACT &&
           receiver->ownership_action == XR_SEM_OPERAND_BORROW &&
           capacity->role == XR_SEM_OPERAND_ARGUMENT && capacity->parameter == 1 &&
           capacity->flags == XR_SEM_OPERAND_CALL_CONTRACT &&
           capacity->ownership_action == XR_SEM_OPERAND_CONSUME;
}

static bool semantic_array_fill_type_is_exact(const XrSemanticTypeRecord *type,
                                              uint8_t element_storage) {
    if (!type || type->builtin_type != XR_TID_NULL || type->child_count != 0 ||
        type->aggregate_extent != 0 || type->aggregate_align != 0 || type->flags != 0)
        return false;
    if (element_storage == XR_ELEM_RUNE)
        return type->kind == XR_KIND_RUNE && type->scalar_rep == XR_SCALAR_REP_NONE;
    if (element_storage <= XR_ELEM_ANY || element_storage >= XR_ELEM_RAWPTR)
        return false;
    return ((type->kind == XR_KIND_INT || type->kind == XR_KIND_FLOAT) &&
            xr_semantic_array_element_storage(type) != XR_ELEM_ANY) ||
           (type->kind == XR_KIND_BOOL && type->scalar_rep == XR_SCALAR_REP_NONE);
}

/* Independent verifier for the one-argument Array.fill family. The dedicated
 * intrinsic kind is the operation identity; metadata remains debug-only and
 * neither its spelling nor semantic_immediate participates in this proof. */
static bool verify_array_fill_scalar(const XrSemanticPlan *plan,
                                     const XrSemanticOperationRecord *operation, char *error,
                                     size_t error_size) {
    if (operation->intrinsic_kind != XR_SEM_INTRINSIC_ARRAY_FILL_SCALAR)
        return true;
    const XrSemanticOperandRecord *operands =
        operation->operand_count == 2 && operation->operand_begin <= plan->operand_count &&
                operation->operand_count <= plan->operand_count - operation->operand_begin
            ? &plan->operands[operation->operand_begin]
            : NULL;
    const XrSemanticOperandRecord *receiver = operands;
    const XrSemanticOperandRecord *fill = operands ? operands + 1 : NULL;
    const XrSemanticTypeRecord *receiver_type =
        receiver && receiver->type < plan->type_count ? &plan->types[receiver->type] : NULL;
    uint32_t element_index = receiver_type && receiver_type->child_count == 1 &&
                                     receiver_type->child_begin < plan->type_child_count
                                 ? plan->type_children[receiver_type->child_begin]
                                 : XR_SEMANTIC_INDEX_NONE;
    const XrSemanticTypeRecord *element =
        element_index < plan->type_count ? &plan->types[element_index] : NULL;
    uint8_t storage = xr_semantic_array_element_storage(element);
    bool exact =
        operation->opcode == XI_CALL_METHOD && receiver && fill &&
        xr_semantic_array_type_row_is_exact(receiver_type) && fill->type == element_index &&
        storage > XR_ELEM_ANY && storage < XR_ELEM_RAWPTR &&
        operation->array_element_storage == storage && operation->result_type == receiver->type &&
        operation->metadata_count == 1 && operation->metadata_begin < plan->metadata_count &&
        operation->semantic_immediate == 0 && operation->auxiliary_kind == XI_AUX_KIND_NONE &&
        operation->constant == XR_SEMANTIC_INDEX_NONE &&
        operation->callable_function == XR_SEMANTIC_INDEX_NONE &&
        operation->import_resolution == XR_SEM_IMPORT_RESOLUTION_NONE &&
        operation->effects == xi_generated_op_effects(XI_CALL_METHOD) &&
        operation->ownership_use == xi_generated_op_own_use(XI_CALL_METHOD) &&
        operation->flags == xi_generated_op_default_flags(XI_CALL_METHOD) &&
        operation->result_alias_operand == 0 &&
        /* Both pairings the builder admits, for the reason stated there. */
        ((operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_OWNED &&
          operation->return_provenance == XR_SEM_RETURN_OWNED) ||
         (operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_BORROWED &&
          operation->return_provenance == XR_SEM_RETURN_BORROWED_STATIC)) &&
        operation->return_parameter == -1 && operation->return_complete == 1 &&
        receiver->role == XR_SEM_OPERAND_RECEIVER && receiver->parameter == -1 &&
        receiver->flags == XR_SEM_OPERAND_CALL_CONTRACT &&
        receiver->ownership_action == XR_SEM_OPERAND_BORROW &&
        fill->role == XR_SEM_OPERAND_ARGUMENT && fill->parameter == 0 &&
        fill->flags == XR_SEM_OPERAND_CALL_CONTRACT &&
        fill->ownership_action == XR_SEM_OPERAND_CONSUME;
    return exact ||
           report(error, error_size, "XR_SEM_0019", "Array.fill scalar authority is not exact");
}

static bool verify_array_hof(const XrSemanticPlan *plan, const XrSemanticOperationRecord *operation,
                             char *error, size_t error_size) {
    if (operation->intrinsic_kind != XR_SEM_INTRINSIC_ARRAY_HOF)
        return operation->array_hof_kind == XR_SEM_ARRAY_HOF_NONE &&
                       operation->array_result_element_storage == XR_ELEM_ANY
                   ? true
                   : report(error, error_size, "XR_SEM_0019",
                            "non-Array HOF carries higher-order authority");
    uint16_t expected_operands = operation->array_hof_kind == XR_SEM_ARRAY_HOF_REDUCE ? 3u : 2u;
    const XrSemanticOperandRecord *operands =
        operation->array_hof_kind > XR_SEM_ARRAY_HOF_NONE &&
                operation->array_hof_kind < XR_SEM_ARRAY_HOF_COUNT &&
                operation->operand_count == expected_operands &&
                operation->operand_begin <= plan->operand_count &&
                operation->operand_count <= plan->operand_count - operation->operand_begin
            ? &plan->operands[operation->operand_begin]
            : NULL;
    const XrSemanticTypeRecord *receiver_type =
        operands && operands[0].type < plan->type_count ? &plan->types[operands[0].type] : NULL;
    uint32_t receiver_element = receiver_type &&
                                        xr_semantic_array_type_row_is_exact(receiver_type) &&
                                        receiver_type->child_begin < plan->type_child_count
                                    ? plan->type_children[receiver_type->child_begin]
                                    : XR_SEMANTIC_INDEX_NONE;
    const XrSemanticTypeRecord *receiver_element_type =
        receiver_element < plan->type_count ? &plan->types[receiver_element] : NULL;
    const XrSemanticFunctionRecord *callee = operation->callable_function < plan->function_count
                                                 ? &plan->functions[operation->callable_function]
                                                 : NULL;
    uint16_t minimum_parameters = operation->array_hof_kind == XR_SEM_ARRAY_HOF_REDUCE ? 2u : 1u;
    bool exact =
        operation->opcode == XI_CALL_METHOD && operands && callee &&
        operation->function < plan->function_count && callee->parent == operation->function &&
        callee->capture_count == 0 && callee->parameter_count == minimum_parameters &&
        (callee->semantic_effects & (XI_EFFECT_SIDE_EFFECT | XI_EFFECT_MEMORY_WRITE |
                                     XI_EFFECT_MAY_THROW | XI_EFFECT_MAY_SUSPEND)) == 0 &&
        callee->parameter_begin <= plan->parameter_count &&
        callee->parameter_count <= plan->parameter_count - callee->parameter_begin &&
        receiver_element_type &&
        xr_semantic_array_element_storage(receiver_element_type) ==
            operation->array_element_storage &&
        operation->array_element_storage > XR_ELEM_ANY &&
        operation->array_element_storage < XR_ELEM_RAWPTR &&
        operation->array_result_element_storage > XR_ELEM_ANY &&
        operation->array_result_element_storage < XR_ELEM_RAWPTR &&
        operation->metadata_count == 1 && operation->metadata_begin < plan->metadata_count &&
        operation->semantic_immediate == 0 && operation->auxiliary_kind == XI_AUX_KIND_NONE &&
        operation->constant == XR_SEMANTIC_INDEX_NONE &&
        operation->import_resolution == XR_SEM_IMPORT_RESOLUTION_NONE &&
        operation->effects == xi_generated_op_effects(XI_CALL_METHOD) &&
        operation->ownership_use == xi_generated_op_own_use(XI_CALL_METHOD) &&
        operation->result_alias_operand == -1 && operation->return_parameter == -1 &&
        (operation->array_hof_kind == XR_SEM_ARRAY_HOF_REDUCE
             ? operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_CALL_RESULT &&
                   operation->return_provenance == XR_SEM_RETURN_NONE &&
                   operation->return_complete == 0
             : operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_OWNED &&
                   operation->return_provenance == XR_SEM_RETURN_OWNED &&
                   operation->return_complete == 1) &&
        operands[0].role == XR_SEM_OPERAND_RECEIVER && operands[0].parameter == -1 &&
        operands[0].flags == XR_SEM_OPERAND_CALL_CONTRACT &&
        operands[1].role == XR_SEM_OPERAND_ARGUMENT && operands[1].parameter == 0 &&
        operands[1].flags == XR_SEM_OPERAND_CALL_CONTRACT;
    uint32_t result_element = operation->result_type;
    if (exact && operation->array_hof_kind != XR_SEM_ARRAY_HOF_REDUCE) {
        const XrSemanticTypeRecord *result_array =
            operation->result_type < plan->type_count ? &plan->types[operation->result_type] : NULL;
        exact = xr_semantic_array_type_row_is_exact(result_array) &&
                result_array->child_begin < plan->type_child_count;
        if (exact)
            result_element = plan->type_children[result_array->child_begin];
        if (exact && operation->array_hof_kind == XR_SEM_ARRAY_HOF_FILTER)
            exact =
                operation->result_type == operands[0].type && result_element == receiver_element;
    } else if (exact) {
        exact = operands[2].role == XR_SEM_OPERAND_ARGUMENT && operands[2].parameter == 1 &&
                operands[2].flags == XR_SEM_OPERAND_CALL_CONTRACT &&
                operands[2].type == operation->result_type;
    }
    const XrSemanticTypeRecord *result_element_type =
        result_element < plan->type_count ? &plan->types[result_element] : NULL;
    if (exact)
        exact = result_element_type && xr_semantic_array_element_storage(result_element_type) ==
                                           operation->array_result_element_storage;
    if (exact && operation->array_hof_kind == XR_SEM_ARRAY_HOF_FILTER)
        exact = callee->return_type < plan->type_count &&
                xr_semantic_array_member_bool_type_is_exact(&plan->types[callee->return_type]);
    else if (exact)
        exact = callee->return_type == result_element;
    const XrSemanticParameterRecord *parameters =
        exact ? &plan->parameters[callee->parameter_begin] : NULL;
    if (exact)
        exact = parameters[0].function == operation->callable_function &&
                parameters[0].ordinal == 0 &&
                parameters[0].type == (operation->array_hof_kind == XR_SEM_ARRAY_HOF_REDUCE
                                           ? result_element
                                           : receiver_element);
    if (exact && operation->array_hof_kind == XR_SEM_ARRAY_HOF_REDUCE)
        exact = parameters[1].function == operation->callable_function &&
                parameters[1].ordinal == 1 && parameters[1].type == receiver_element;
    const XrSemanticTypeRecord *callback_type =
        exact && operands[1].type < plan->type_count ? &plan->types[operands[1].type] : NULL;
    if (exact)
        exact = callback_type && callback_type->kind == XR_KIND_FUNCTION &&
                callback_type->child_count == (uint32_t) callee->parameter_count + 1u &&
                callback_type->child_begin <= plan->type_child_count &&
                callback_type->child_count <= plan->type_child_count - callback_type->child_begin;
    for (uint16_t i = 0; exact && i < callee->parameter_count; i++)
        exact = plan->type_children[callback_type->child_begin + i] == parameters[i].type;
    if (exact)
        exact = plan->type_children[callback_type->child_begin + callee->parameter_count] ==
                callee->return_type;
    const XrSemanticOperationRecord *producer = NULL;
    for (uint32_t i = 0; exact && i < plan->operation_count; i++) {
        const XrSemanticOperationRecord *candidate = &plan->operations[i];
        if (candidate->function != operation->function ||
            candidate->result_value != operands[1].value)
            continue;
        if (producer) {
            exact = false;
            break;
        }
        producer = candidate;
    }
    if (exact)
        exact = producer && producer < operation &&
                (producer->opcode == XI_CLOSURE_NEW ||
                 (producer->opcode == XI_STACK_ALLOC &&
                  producer->semantic_immediate == XI_CLOSURE_NEW)) &&
                producer->callable_function == operation->callable_function &&
                producer->result_type == operands[1].type;
    uint32_t callback_uses = 0;
    for (uint32_t i = 0; exact && i < plan->operand_count; i++)
        callback_uses += plan->operands[i].value == operands[1].value;
    for (uint32_t i = 0; exact && i < plan->block_count; i++)
        if (plan->blocks[i].control_value == operands[1].value)
            exact = false;
    exact = exact && callback_uses == 1;
    return exact ||
           report(error, error_size, "XR_SEM_0019", "Array higher-order authority is not exact");
}

static bool verify_array_allocation_storage(const XrSemanticPlan *plan,
                                            const XrSemanticOperationRecord *operation, char *error,
                                            size_t error_size) {
    if (operation->opcode != XI_ARRAY_NEW)
        return true;
    const XrSemanticTypeRecord *array_type =
        operation->result_type < plan->type_count ? &plan->types[operation->result_type] : NULL;
    const XrSemanticOperandRecord *capacity =
        operation->operand_count == 1 && operation->operand_begin < plan->operand_count
            ? &plan->operands[operation->operand_begin]
            : NULL;
    uint32_t element_index = array_type && array_type->child_count == 1 &&
                                     array_type->child_begin < plan->type_child_count
                                 ? plan->type_children[array_type->child_begin]
                                 : XR_SEMANTIC_INDEX_NONE;
    const XrSemanticTypeRecord *element =
        element_index < plan->type_count ? &plan->types[element_index] : NULL;
    const XrSemanticTypeRecord *capacity_type =
        capacity && capacity->type < plan->type_count ? &plan->types[capacity->type] : NULL;
    uint8_t expected_storage = xr_semantic_array_element_storage(element);
    bool source_class_element =
        xr_semantic_class_instance_type_source_class(plan, element) != XR_SEMANTIC_INDEX_NONE;
    bool candidate = (expected_storage > XR_ELEM_ANY && expected_storage < XR_ELEM_RAWPTR) ||
                     (source_class_element && expected_storage == XR_ELEM_ANY);
    if (!candidate)
        return operation->intrinsic_kind == XR_SEM_INTRINSIC_ARRAY_FILL_SCALAR ||
               operation->array_element_storage == XR_ELEM_ANY ||
               report(error, error_size, "XR_SEM_0019",
                      "non-scalar Array allocation carries element-storage authority");
    bool exact =
        array_type && element && capacity && capacity_type &&
        xr_semantic_array_type_row_is_exact(array_type) &&
        expected_storage == operation->array_element_storage &&
        ((operation->array_element_storage > XR_ELEM_ANY &&
          operation->array_element_storage < XR_ELEM_RAWPTR) ||
         (source_class_element && operation->array_element_storage == XR_ELEM_ANY)) &&
        xr_semantic_array_member_i64_type_is_exact(capacity_type) &&
        capacity->role == XR_SEM_OPERAND_VALUE && capacity->parameter == -1 &&
        capacity->flags == 0 && capacity->ownership_action == XR_SEM_OPERAND_CONSUME &&
        operation->intrinsic_kind == XR_SEM_INTRINSIC_NONE && operation->metadata_count == 0 &&
        operation->auxiliary_kind == XI_AUX_KIND_NONE && operation->semantic_immediate == 0 &&
        operation->constant == XR_SEMANTIC_INDEX_NONE &&
        operation->callable_function == XR_SEMANTIC_INDEX_NONE &&
        operation->import_resolution == XR_SEM_IMPORT_RESOLUTION_NONE &&
        operation->effects == xi_generated_op_effects(XI_ARRAY_NEW) &&
        operation->flags == xi_generated_op_default_flags(XI_ARRAY_NEW) &&
        operation->ownership_use == xi_generated_op_own_use(XI_ARRAY_NEW) &&
        operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_OWNED &&
        operation->result_alias_operand == -1 &&
        operation->return_provenance == XR_SEM_RETURN_OWNED && operation->return_parameter == -1 &&
        operation->return_complete == 1 && xr_semantic_allocation_identity_is_canonical(operation);
    return exact || report(error, error_size, "XR_SEM_0019",
                           "Array allocation element storage is not exact");
}

/* Independent frozen-row proof for the compiler-owned Array constructors.
 * Neither a metadata spelling nor semantic_immediate participates: the
 * intrinsic variant and scalar storage have dedicated schema fields. */
static bool verify_array_intrinsic(const XrSemanticPlan *plan,
                                   const XrSemanticOperationRecord *operation, char *error,
                                   size_t error_size) {
    if (!verify_array_hof(plan, operation, error, error_size))
        return false;
    if (operation->intrinsic_kind == XR_SEM_INTRINSIC_ARRAY_HOF)
        return true;
    bool candidate = operation->intrinsic_kind == XR_SEM_INTRINSIC_ARRAY_WITH_CAPACITY ||
                     operation->intrinsic_kind == XR_SEM_INTRINSIC_ARRAY_FILLED_NEW;
    if (!candidate) {
        if (operation->intrinsic_kind == XR_SEM_INTRINSIC_ARRAY_FILL_SCALAR)
            return verify_array_fill_scalar(plan, operation, error, error_size);
        if (operation->opcode == XI_ARRAY_NEW)
            return verify_array_allocation_storage(plan, operation, error, error_size);
        return operation->array_element_storage == XR_ELEM_ANY ||
               report(error, error_size, "XR_SEM_0019",
                      "non-Array intrinsic carries element-storage authority");
    }
    uint16_t expected_operands =
        operation->intrinsic_kind == XR_SEM_INTRINSIC_ARRAY_WITH_CAPACITY ? 1u : 2u;
    const XrSemanticTypeRecord *array_type =
        operation->result_type < plan->type_count ? &plan->types[operation->result_type] : NULL;
    const XrSemanticOperandRecord *operands =
        operation->operand_count == expected_operands &&
                operation->operand_begin <= plan->operand_count &&
                operation->operand_count <= plan->operand_count - operation->operand_begin
            ? &plan->operands[operation->operand_begin]
            : NULL;
    uint32_t element_index = array_type && array_type->child_count == 1 &&
                                     array_type->child_begin < plan->type_child_count
                                 ? plan->type_children[array_type->child_begin]
                                 : XR_SEMANTIC_INDEX_NONE;
    const XrSemanticTypeRecord *element =
        element_index < plan->type_count ? &plan->types[element_index] : NULL;
    const XrSemanticTypeRecord *count =
        operands && operands[0].type < plan->type_count ? &plan->types[operands[0].type] : NULL;
    bool exact =
        operation->opcode == XI_CALL_BUILTIN && operands &&
        xr_semantic_array_type_row_is_exact(array_type) && element && count &&
        xr_semantic_array_element_storage(element) == operation->array_element_storage &&
        operation->array_element_storage > XR_ELEM_ANY &&
        operation->array_element_storage < XR_ELEM_RAWPTR &&
        xr_semantic_array_member_i64_type_is_exact(count) && operation->metadata_count == 0 &&
        operation->auxiliary_kind == XI_AUX_KIND_NONE && operation->semantic_immediate == 0 &&
        operation->constant == XR_SEMANTIC_INDEX_NONE &&
        operation->callable_function == XR_SEMANTIC_INDEX_NONE &&
        operation->import_resolution == XR_SEM_IMPORT_RESOLUTION_NONE &&
        operation->effects == xi_generated_op_effects(XI_CALL_BUILTIN) &&
        operation->flags == xi_generated_op_default_flags(XI_CALL_BUILTIN) &&
        operation->ownership_use == xi_generated_op_own_use(XI_CALL_BUILTIN) &&
        operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_OWNED &&
        operation->result_alias_operand == -1 &&
        operation->return_provenance == XR_SEM_RETURN_OWNED && operation->return_parameter == -1 &&
        operation->return_complete == 1 && xr_semantic_allocation_identity_is_canonical(operation);
    for (uint16_t i = 0; exact && i < expected_operands; i++) {
        const XrSemanticOperandRecord *operand = &operands[i];
        exact = operand->role == XR_SEM_OPERAND_ARGUMENT && operand->parameter == (int16_t) i &&
                operand->flags == XR_SEM_OPERAND_CALL_CONTRACT &&
                operand->ownership_action == XR_SEM_OPERAND_CONSUME &&
                (i == 0 ? operand->type == operands[0].type
                        : operand->type < plan->type_count &&
                              semantic_array_fill_type_is_exact(&plan->types[operand->type],
                                                                operation->array_element_storage));
    }
    if (!exact && error && error_size)
        snprintf(error, error_size,
                 "XR_SEM_0019: Array intrinsic authority is not exact "
                 "kind=%u storage=%u operands=%u metadata=%u immediate=%lld "
                 "ownership=%u return=%u/%u alias=%d flags=%u",
                 operation->intrinsic_kind, operation->array_element_storage,
                 operation->operand_count, operation->metadata_count,
                 (long long) operation->semantic_immediate, operation->result_ownership,
                 operation->return_provenance, operation->return_complete,
                 operation->result_alias_operand, operation->flags);
    return exact;
}

/* Rebuild the sealed reference-storage family without trusting the builder:
 * only exact source-class push and range-fill identities may consume an
 * element into tagged Array storage. */

static bool verify_array_member_reference_contract(const XrSemanticPlan *plan,
                                                   const XrArrayMemberShape *shape,
                                                   const XrSemanticOperationRecord *operation,
                                                   uint32_t element_type_index,
                                                   const XrSemanticTypeRecord *element_type) {
    if (!plan || !shape || !operation || !element_type)
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
        shape->element_operand == 0 || shape->element_operand >= operation->operand_count ||
        xr_semantic_class_instance_type_source_class(plan, element_type) == XR_SEMANTIC_INDEX_NONE)
        return false;
    bool exact_push = strcmp(shape->selector, "push") == 0 && operation->operand_count == 2 &&
                      operation->semantic_immediate == (int64_t) XI_METHOD_SYMBOL_PUSH << 1;
    bool exact_fill = strcmp(shape->selector, "fill") == 0 && operation->operand_count == 4 &&
                      operation->semantic_immediate == (int64_t) XI_METHOD_SYMBOL_FILL << 1;
    if (!exact_push && !exact_fill)
        return false;
    uint32_t semantic_operand = operation->operand_begin + shape->element_operand;
    if (semantic_operand >= plan->operand_count)
        return false;
    const XrSemanticOperandRecord *element = &plan->operands[semantic_operand];
    return element->type == element_type_index && element->role == XR_SEM_OPERAND_ARGUMENT &&
           element->parameter == (int16_t) (shape->element_operand - 1u) &&
           element->flags == XR_SEM_OPERAND_CALL_CONTRACT &&
           element->ownership_action == XR_SEM_OPERAND_CONSUME;
}

static bool verify_array_member_scalar(const XrSemanticPlan *plan,
                                       const XrSemanticOperationRecord *operation, char *error,
                                       size_t error_size) {
    if (operation->intrinsic_kind != XR_SEM_INTRINSIC_ARRAY_MEMBER_SCALAR)
        return true;
    if (operation->evidence[1] == XA_INTRINSIC_ARRAY_RESERVE)
        return verify_array_reserve(plan, operation) ||
               report(error, error_size, "XR_SEM_0019", "Array.reserve authority is not exact");
    const char *selector =
        operation->metadata_count == 1 && operation->metadata_begin < plan->metadata_count
            ? plan->metadata[operation->metadata_begin]
            : NULL;
    const XrArrayMemberShape *shape = xr_array_member_shape(selector, operation->operand_count);
    const XrSemanticOperandRecord *receiver =
        shape && operation->operand_begin < plan->operand_count &&
                operation->operand_count <= plan->operand_count - operation->operand_begin
            ? &plan->operands[operation->operand_begin]
            : NULL;
    const XrSemanticTypeRecord *receiver_type =
        receiver && receiver->type < plan->type_count ? &plan->types[receiver->type] : NULL;
    const XrSemanticTypeRecord *result_type =
        operation->result_type < plan->type_count ? &plan->types[operation->result_type] : NULL;
    bool exact = operation->opcode == XI_CALL_METHOD && operation->semantic_immediate > 0 &&
                 (operation->semantic_immediate & 1) == 0 && receiver &&
                 xr_semantic_array_type_row_is_exact(receiver_type) &&
                 receiver_type->child_begin < plan->type_child_count &&
                 operation->effects == xi_generated_op_effects(XI_CALL_METHOD) &&
                 receiver->role == XR_SEM_OPERAND_RECEIVER && receiver->parameter == -1 &&
                 receiver->flags == XR_SEM_OPERAND_CALL_CONTRACT &&
                 receiver->ownership_action == XR_SEM_OPERAND_BORROW;
    uint32_t element_type_index =
        exact ? plan->type_children[receiver_type->child_begin] : XR_SEMANTIC_INDEX_NONE;
    const XrSemanticTypeRecord *element_type =
        exact && element_type_index < plan->type_count ? &plan->types[element_type_index] : NULL;
    bool source_class_fill_result =
        exact && element_type && strcmp(shape->selector, "fill") == 0 &&
        operation->operand_count == 4 &&
        operation->semantic_immediate == (int64_t) XI_METHOD_SYMBOL_FILL << 1 &&
        xr_semantic_class_instance_type_source_class(plan, element_type) !=
            XR_SEMANTIC_INDEX_NONE &&
        operation->result_type == receiver->type && operation->result_alias_operand == 0 &&
        ((operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_BORROWED &&
          operation->return_provenance == XR_SEM_RETURN_BORROWED_PARAM &&
          operation->return_parameter == 0 && operation->return_complete == 1) ||
         (operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_CALL_RESULT &&
          operation->return_provenance == XR_SEM_RETURN_NONE && operation->return_parameter == -1 &&
          operation->return_complete == 0));
    exact = exact && (xr_semantic_array_member_result_is_exact(operation, shape, result_type,
                                                               receiver->type) ||
                      source_class_fill_result);
    exact = exact && element_type &&
            verify_array_member_reference_contract(plan, shape, operation, element_type_index,
                                                   element_type);
    for (uint16_t i = 1; exact && i < operation->operand_count; i++) {
        const XrSemanticOperandRecord *argument = receiver + i;
        const XrSemanticTypeRecord *argument_type =
            argument->type < plan->type_count ? &plan->types[argument->type] : NULL;
        exact = xr_semantic_array_member_argument_is_exact(shape, argument, argument_type, i,
                                                           element_type_index);
    }
    return exact ||
           report(error, error_size, "XR_SEM_0019", "Array member scalar authority is not exact");
}

/* Recomputed from the frozen rows alone.  A native stdlib namespace receiver
 * cannot be spelled by a declaration: the module-init import record classifies
 * its resolution against the native registry rather than against a compiled
 * module, and its frozen metadata pair names the module path with an empty
 * member.  The registry then names exactly one implementation for that path
 * plus the selector, which is what turns the callsite into an exact target
 * instead of an open method dispatch. */
static bool semantic_native_module_import_row(const XrSemanticPlan *plan,
                                              const XrSemanticOperationRecord *record,
                                              const char **out_module_path) {
    const XrSemanticTypeRecord *type =
        record && record->result_type < plan->type_count ? &plan->types[record->result_type] : NULL;
    XrStableId zero = {{0}};
    if (!record || !type || record->opcode != XI_IMPORT_REF || record->function != 0 ||
        record->operand_count != 0 || record->metadata_count != 2 ||
        record->metadata_begin + 1u >= plan->metadata_count ||
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
    const char *module_path = plan->metadata[record->metadata_begin];
    const char *member = plan->metadata[record->metadata_begin + 1u];
    if (!module_path || !member || member[0] != '\0' ||
        !xr_stdlib_metadata_module_known(module_path))
        return false;
    if (out_module_path)
        *out_module_path = module_path;
    return true;
}

static const char *semantic_native_module_receiver_path(const XrSemanticPlan *plan,
                                                        uint32_t receiver_value) {
    const XrSemanticOperationRecord *load = NULL;
    XrStableId zero = {{0}};
    for (uint32_t i = 0; i < plan->operation_count; i++) {
        if (plan->operations[i].result_value != receiver_value)
            continue;
        if (load)
            return NULL;
        load = &plan->operations[i];
    }
    const XrSemanticTypeRecord *load_type =
        load && load->result_type < plan->type_count ? &plan->types[load->result_type] : NULL;
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
    for (uint32_t i = 0; i < plan->operation_count; i++) {
        if (plan->operations[i].opcode != XI_SET_SHARED || plan->operations[i].function != 0 ||
            plan->operations[i].semantic_immediate != load->semantic_immediate)
            continue;
        if (store)
            return NULL;
        store = &plan->operations[i];
    }
    if (!store || store->operand_count != 1 || store->operand_begin >= plan->operand_count)
        return NULL;
    const XrSemanticOperandRecord *stored = &plan->operands[store->operand_begin];
    if (stored->role != XR_SEM_OPERAND_VALUE || stored->parameter != -1 ||
        stored->ownership_action != XR_SEM_OPERAND_CONSUME || stored->flags != 0 ||
        stored->type != load->result_type)
        return NULL;
    const XrSemanticOperationRecord *import = NULL;
    for (uint32_t i = 0; i < plan->operation_count; i++) {
        if (plan->operations[i].result_value != stored->value)
            continue;
        if (import)
            return NULL;
        import = &plan->operations[i];
    }
    const char *module_path = NULL;
    return import && import->result_type == load->result_type &&
                   semantic_native_module_import_row(plan, import, &module_path)
               ? module_path
               : NULL;
}

static bool verify_native_module_scalar_call(const XrSemanticPlan *plan,
                                             const XrSemanticOperationRecord *operation,
                                             char *error, size_t error_size) {
    if (operation->intrinsic_kind != XR_SEM_INTRINSIC_NATIVE_MODULE_SCALAR_CALL)
        return true;
    const XrSemanticOperandRecord *receiver =
        operation->operand_count > 0 && operation->operand_begin < plan->operand_count &&
                operation->operand_count <= plan->operand_count - operation->operand_begin
            ? &plan->operands[operation->operand_begin]
            : NULL;
    const char *selector =
        operation->metadata_count == 1 && operation->metadata_begin < plan->metadata_count
            ? plan->metadata[operation->metadata_begin]
            : NULL;
    bool exact =
        operation->opcode == XI_CALL_METHOD && operation->semantic_immediate > 0 &&
        (operation->semantic_immediate & 1) == 0 && selector && receiver &&
        (operation->flags & XI_FLAG_MAY_SUSPEND) == 0 &&
        operation->effects == xi_generated_op_effects(XI_CALL_METHOD) &&
        xr_semantic_native_module_boundary_type_is_exact(
            operation->result_type < plan->type_count ? &plan->types[operation->result_type] : NULL,
            true) &&
        operation->result_alias_operand == -1 &&
        operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_CALL_RESULT &&
        receiver->role == XR_SEM_OPERAND_RECEIVER && receiver->parameter == -1 &&
        receiver->flags == XR_SEM_OPERAND_CALL_CONTRACT &&
        receiver->ownership_action == XR_SEM_OPERAND_BORROW;
    for (uint16_t i = 1; exact && i < operation->operand_count; i++) {
        const XrSemanticOperandRecord *argument = receiver + i;
        exact = argument->role == XR_SEM_OPERAND_ARGUMENT &&
                argument->parameter == (int16_t) (i - 1) &&
                argument->flags == XR_SEM_OPERAND_CALL_CONTRACT &&
                xr_semantic_native_module_boundary_type_is_exact(
                    argument->type < plan->type_count ? &plan->types[argument->type] : NULL, false);
    }
    if (exact) {
        const char *module_path = semantic_native_module_receiver_path(plan, receiver->value);
        exact = module_path &&
                xr_stdlib_metadata_exact_native_direct_member(
                    module_path, selector, (uint16_t) (operation->operand_count - 1u)) != NULL;
    }
    return exact || report(error, error_size, "XR_SEM_0019",
                           "native module scalar call authority is not exact");
}

static bool verify_string_builder_append_string(const XrSemanticPlan *plan,
                                                const XrSemanticOperationRecord *operation,
                                                char *error, size_t error_size) {
    if (operation->intrinsic_kind != XR_SEM_INTRINSIC_STRINGBUILDER_APPEND_STRING)
        return true;
    const XrSemanticOperandRecord *receiver =
        operation->operand_count == 2 && operation->operand_begin + 1u < plan->operand_count
            ? &plan->operands[operation->operand_begin]
            : NULL;
    const XrSemanticOperandRecord *argument = receiver ? receiver + 1 : NULL;
    const XrSemanticTypeRecord *receiver_type =
        receiver && receiver->type < plan->type_count ? &plan->types[receiver->type] : NULL;
    const XrSemanticTypeRecord *argument_type =
        argument && argument->type < plan->type_count ? &plan->types[argument->type] : NULL;
    const char *selector =
        operation->metadata_count == 1 && operation->metadata_begin < plan->metadata_count
            ? plan->metadata[operation->metadata_begin]
            : NULL;
    bool exact =
        operation->opcode == XI_CALL_METHOD &&
        operation->semantic_immediate == (int64_t) XI_METHOD_SYMBOL_APPEND << 1 &&
        operation->evidence[1] == XA_INTRINSIC_STRING_BUILDER_APPEND && selector &&
        strcmp(selector, XA_INTRINSIC_STRING_BUILDER_APPEND_SOURCE_MEMBER) == 0 && receiver &&
        argument && semantic_type_is_exact_string_builder(receiver_type) && argument_type &&
        argument_type->kind == XR_KIND_STRING && operation->result_type == receiver->type &&
        receiver->role == XR_SEM_OPERAND_RECEIVER && receiver->parameter == -1 &&
        receiver->flags == XR_SEM_OPERAND_CALL_CONTRACT &&
        argument->role == XR_SEM_OPERAND_ARGUMENT && argument->parameter == 0 &&
        argument->flags == XR_SEM_OPERAND_CALL_CONTRACT && operation->result_alias_operand == 0 &&
        string_builder_append_result_is_exact(plan, operation);
    return exact || report(error, error_size, "XR_SEM_0019",
                           "StringBuilder.append(string) authority is not exact");
}

static bool verify_string_builder_constructor(const XrSemanticPlan *plan,
                                              const XrSemanticOperationRecord *operation,
                                              char *error, size_t error_size) {
    if (operation->opcode != XI_CALL_BUILTIN)
        return true;
    const XrSemanticTypeRecord *type =
        operation->result_type < plan->type_count ? &plan->types[operation->result_type] : NULL;
    bool type_names_constructor = semantic_type_is_exact_string_builder(type);
    const char *metadata =
        operation->metadata_count == 1 && operation->metadata_begin < plan->metadata_count
            ? plan->metadata[operation->metadata_begin]
            : NULL;
    bool metadata_names_constructor = metadata && strcmp(metadata, "StringBuilder") == 0;
    if (!type_names_constructor && !metadata_names_constructor)
        return true;
    bool exact =
        type_names_constructor && metadata_names_constructor && operation->operand_count == 0 &&
        operation->auxiliary_kind == XI_AUX_KIND_NONE && operation->semantic_immediate == 0 &&
        operation->constant == XR_SEMANTIC_INDEX_NONE &&
        operation->callable_function == XR_SEMANTIC_INDEX_NONE &&
        operation->import_resolution == XR_SEM_IMPORT_RESOLUTION_NONE &&
        operation->ownership_use == xi_generated_op_own_use(XI_CALL_BUILTIN) &&
        operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_OWNED &&
        operation->transfer_mode == XR_TRANSFER_SHARE &&
        operation->parameter_mode == XR_PARAM_READ &&
        operation->parameter_ownership == XI_OWN_NONE &&
        operation->flags == xi_generated_op_default_flags(XI_CALL_BUILTIN) &&
        operation->result_alias_operand == -1 && operation->return_parameter == -1 &&
        operation->return_provenance == XR_SEM_RETURN_OWNED && operation->return_complete == 1 &&
        xr_semantic_allocation_identity_is_canonical(operation);
    return exact || report(error, error_size, "XR_SEM_0019",
                           "StringBuilder constructor authority is not exact");
}

static bool verify_operation_records(const XrSemanticPlan *plan, const uint8_t *edge_mask,
                                     uint32_t *definitions, uint32_t value_count, char *error,
                                     size_t error_size) {
    uint32_t operand_cursor = 0;
    uint32_t metadata_cursor = 0;
    for (uint32_t i = 0; i < plan->operation_count; i++) {
        const XrSemanticOperationRecord *operation = &plan->operations[i];
        const XrSemanticFunctionRecord *function = operation->function < plan->function_count
                                                       ? &plan->functions[operation->function]
                                                       : NULL;
        const XrSemanticOpContract *contract = xr_semantic_op_contract(operation->opcode);
        uint8_t arity = contract ? contract->arity : XR_SEMANTIC_OP_ARITY_VARIADIC;
        bool explicit_call = operation->opcode == XI_CALL || operation->opcode == XI_TAIL_CALL ||
                             operation->opcode == XI_CALL_METHOD ||
                             operation->opcode == XI_CALL_METHOD_DIRECT;
        bool closure_binding = operation->opcode == XI_CLOSURE_NEW ||
                               (operation->opcode == XI_STACK_ALLOC &&
                                operation->semantic_immediate == XI_CLOSURE_NEW);
        bool array_hof_binding = operation->intrinsic_kind == XR_SEM_INTRINSIC_ARRAY_HOF;
        bool import_ref = operation->opcode == XI_IMPORT_REF;
        bool import_resolution_valid =
            operation->import_resolution < XR_SEM_IMPORT_RESOLUTION_COUNT &&
            (import_ref ? operation->import_resolution != XR_SEM_IMPORT_RESOLUTION_NONE
                        : operation->import_resolution == XR_SEM_IMPORT_RESOLUTION_NONE);
        if (!verify_id(operation->canonical_key, operation->id) || !function ||
            operation->block >= plan->block_count ||
            plan->blocks[operation->block].function != operation->function ||
            i < plan->blocks[operation->block].operation_begin ||
            i >= plan->blocks[operation->block].operation_begin +
                     plan->blocks[operation->block].operation_count ||
            operation->result_type >= plan->type_count || operation->result_value >= value_count ||
            operation->result_value < function->value_begin ||
            operation->result_value >= function->value_begin + function->value_count ||
            operation->opcode >= XI_OP_COUNT || !contract ||
            (arity != XR_SEMANTIC_OP_ARITY_VARIADIC && arity != operation->operand_count) ||
            (explicit_call &&
             (operation->operand_count == 0 || operation->operand_count > INT16_MAX + 1u)) ||
            (operation->opcode == XI_CALL_BUILTIN && operation->operand_count > INT16_MAX) ||
            !import_resolution_valid || operation->effects != contract->effects ||
            operation->ownership_use != contract->ownership_use ||
            operation->result_ownership >= XR_SEM_RESULT_OWNERSHIP_COUNT ||
            (contract->result_ownership == XR_SEM_RESULT_OWNERSHIP_NONE &&
             operation->result_ownership != XR_SEM_RESULT_OWNERSHIP_NONE) ||
            operation->parameter_ownership > XI_OWN_BORROWED ||
            operation->result_alias_operand < -1 ||
            (operation->result_alias_operand >= 0 &&
             (uint16_t) operation->result_alias_operand >= operation->operand_count) ||
            operation->operand_begin != operand_cursor ||
            operation->metadata_begin != metadata_cursor ||
            !range_valid(operation->operand_begin, operation->operand_count, plan->operand_count) ||
            !range_valid(operation->metadata_begin, operation->metadata_count,
                         plan->metadata_count) ||
            (operation->constant != XR_SEMANTIC_INDEX_NONE &&
             operation->constant >= plan->constant_count) ||
            ((closure_binding || array_hof_binding) !=
             (operation->callable_function != XR_SEMANTIC_INDEX_NONE)) ||
            ((closure_binding || array_hof_binding) &&
             (operation->callable_function >= plan->function_count ||
              plan->functions[operation->callable_function].parent != operation->function))) {
            return report(error, error_size, "XR_SEM_0015", "operation record is invalid");
        }
        if (import_ref) {
            if (operation->metadata_count != 2 ||
                operation->metadata_begin > plan->metadata_count ||
                operation->metadata_count > plan->metadata_count - operation->metadata_begin) {
                return report(error, error_size, "XR_SEM_0019",
                              "import resolution has incomplete metadata");
            }
            const char *module = plan->metadata[operation->metadata_begin];
            if (operation->import_resolution == XR_SEM_IMPORT_RESOLUTION_NATIVE_STDLIB &&
                !xr_stdlib_metadata_module_known(module)) {
                return report(error, error_size, "XR_SEM_0019",
                              "native import resolution is not registry grounded");
            }
        }
        if (!operation_debug_span_valid(plan, i))
            return report(error, error_size, "XR_SEM_0019", "operation debug span is invalid");
        if (operation->opcode == XI_ASSERTION ||
            operation->auxiliary_kind == XI_AUX_KIND_ASSERTION_PLAN ||
            operation->intrinsic_kind == XR_SEM_INTRINSIC_ASSERTION) {
            XrAssertionPlan assertion_plan;
            if (!xr_semantic_operation_assertion_plan(operation, &assertion_plan))
                return report(error, error_size, "XR_SEM_0019",
                              "assertion operation lacks an exact typed plan");
        }
        if (!verify_string_byte_slice_view(plan, operation, error, error_size))
            return false;
        if (!verify_string_runes(plan, operation, error, error_size))
            return false;
        if (!verify_iterator_rune_has_next(plan, operation, error, error_size))
            return false;
        if (!verify_iterator_rune_next(plan, operation, error, error_size))
            return false;
        if (!verify_map_entry_iterator(plan, operation, error, error_size))
            return false;
        if (!verify_rune_to_uint32(plan, operation, error, error_size))
            return false;
        if (!verify_rune_is_whitespace(plan, operation, error, error_size))
            return false;
        if (!verify_string_slice_range(plan, operation, error, error_size))
            return false;
        if (!verify_string_builder_append_rune(plan, operation, error, error_size))
            return false;
        if (!verify_string_builder_to_string(plan, operation, error, error_size))
            return false;
        if (!verify_string_builder_append_string(plan, operation, error, error_size))
            return false;
        if (!verify_json_namespace_value(plan, operation, error, error_size))
            return false;
        if (!verify_array_member_scalar(plan, operation, error, error_size))
            return false;
        if (!verify_array_fill_scalar(plan, operation, error, error_size))
            return false;
        if (!verify_array_intrinsic(plan, operation, error, error_size))
            return false;
        if (!verify_native_module_scalar_call(plan, operation, error, error_size))
            return false;
        uint32_t existing_definition = definitions[operation->result_value];
        if (existing_definition != XR_SEMANTIC_INDEX_NONE) {
            bool matching_parameter = false;
            if (existing_definition >= plan->operation_count) {
                uint32_t parameter_index = existing_definition - plan->operation_count;
                const XrSemanticParameterRecord *parameter =
                    parameter_index < plan->parameter_count ? &plan->parameters[parameter_index]
                                                            : NULL;
                matching_parameter = parameter && operation->opcode == XI_PARAM &&
                                     operation->function == parameter->function &&
                                     operation->result_type == parameter->type &&
                                     operation->semantic_immediate == parameter->ordinal &&
                                     operation->parameter_mode == parameter->mode &&
                                     operation->parameter_ownership == parameter->ownership &&
                                     operation->transfer_mode == parameter->transfer_mode;
            }
            if (!matching_parameter) {
                return report(error, error_size, "XR_SEM_0015", "SSA value is defined twice");
            }
        }
        definitions[operation->result_value] = i;
        if (operation->allocation_key &&
            !verify_id(operation->allocation_key, operation->allocation_id)) {
            return report(error, error_size, "XR_SEM_0002", "allocation identity is invalid");
        }
        if (!verify_string_builder_constructor(plan, operation, error, error_size))
            return false;
        if (operation->opcode == XI_TRY && (operation->evidence[7] >= plan->block_count ||
                                            (edge_mask[i] & XR_SEM_OPERATION_EDGE_PANIC) == 0)) {
            return report(error, error_size, "XR_SEM_0010",
                          "try operation is missing its explicit panic edge");
        }
        if (operation->opcode == XI_ERR_CHECK &&
            plan->blocks[operation->block].control_value == operation->result_value &&
            plan->blocks[operation->block].successors[0] != XR_SEMANTIC_INDEX_NONE &&
            (edge_mask[i] & XR_SEM_OPERATION_EDGE_ERROR) == 0) {
            return report(error, error_size, "XR_SEM_0010",
                          "error check is missing its explicit error edge");
        }
        operand_cursor += operation->operand_count;
        metadata_cursor += operation->metadata_count;
    }
    return (operand_cursor == plan->operand_count && metadata_cursor == plan->metadata_count) ||
           report(error, error_size, "XR_SEM_0015",
                  "operation side tables are not exactly partitioned");
}

/* A callable survives the forwarding ops unchanged: an identity copy, the
 * ownership forwards, and a successful CHECKTYPE all hand on the same closure
 * with at most a refined static type. */
static bool frozen_operation_forwards_callable(const XrSemanticOperationRecord *producer) {
    if (producer->operand_count != 1)
        return false;
    if (producer->opcode == XI_COPY)
        return producer->semantic_immediate == XI_COPY_KIND_IDENTITY &&
               producer->result_alias_operand == 0;
    return producer->opcode == XI_SOURCE_MOVE || producer->opcode == XI_OWNER_FORWARD ||
           producer->opcode == XI_CHECKTYPE;
}

static uint32_t resolve_frozen_closure_value(const XrSemanticPlan *plan,
                                             const uint32_t *definitions, uint32_t value_count,
                                             uint32_t function, uint32_t value) {
    for (uint32_t depth = 0; depth < plan->operation_count; depth++) {
        if (value >= value_count)
            return XR_SEMANTIC_INDEX_NONE;
        uint32_t producer_index = definitions[value];
        if (producer_index >= plan->operation_count)
            return XR_SEMANTIC_INDEX_NONE;
        const XrSemanticOperationRecord *producer = &plan->operations[producer_index];
        if (producer->function != function)
            return XR_SEMANTIC_INDEX_NONE;
        bool closure_binding =
            producer->opcode == XI_CLOSURE_NEW ||
            (producer->opcode == XI_STACK_ALLOC && producer->semantic_immediate == XI_CLOSURE_NEW);
        if (closure_binding)
            return producer->callable_function;
        if (!frozen_operation_forwards_callable(producer))
            return XR_SEMANTIC_INDEX_NONE;
        value = plan->operands[producer->operand_begin].value;
    }
    return XR_SEMANTIC_INDEX_NONE;
}

typedef struct XrFrozenSharedStoreRow {
    uint32_t owner;
    int64_t slot;
    uint32_t operation;
} XrFrozenSharedStoreRow;

typedef struct XrFrozenSharedStoreIndex {
    XrFrozenSharedStoreRow *rows;
    uint32_t count;
} XrFrozenSharedStoreIndex;

static int compare_frozen_shared_store_rows(const void *left_value, const void *right_value) {
    const XrFrozenSharedStoreRow *left = (const XrFrozenSharedStoreRow *) left_value;
    const XrFrozenSharedStoreRow *right = (const XrFrozenSharedStoreRow *) right_value;
    if (left->owner != right->owner)
        return left->owner < right->owner ? -1 : 1;
    if (left->slot != right->slot)
        return left->slot < right->slot ? -1 : 1;
    if (left->operation != right->operation)
        return left->operation < right->operation ? -1 : 1;
    return 0;
}

static bool frozen_shared_store_index_build(const XrSemanticPlan *plan,
                                            XrFrozenSharedStoreIndex *index) {
    memset(index, 0, sizeof(*index));
    for (uint32_t operation = 0; operation < plan->operation_count; operation++) {
        if (plan->operations[operation].opcode == XI_SET_SHARED)
            index->count++;
    }
    if (index->count == 0)
        return true;
    index->rows =
        (XrFrozenSharedStoreRow *) xr_malloc((size_t) index->count * sizeof(*index->rows));
    if (!index->rows)
        return false;
    uint32_t cursor = 0;
    for (uint32_t operation = 0; operation < plan->operation_count; operation++) {
        const XrSemanticOperationRecord *record = &plan->operations[operation];
        if (record->opcode != XI_SET_SHARED)
            continue;
        index->rows[cursor++] = (XrFrozenSharedStoreRow) {
            .owner = record->function,
            .slot = record->semantic_immediate,
            .operation = operation,
        };
    }
    qsort(index->rows, index->count, sizeof(*index->rows), compare_frozen_shared_store_rows);
    return true;
}

static bool find_frozen_shared_store(const XrFrozenSharedStoreIndex *index, uint32_t owner,
                                     int64_t slot, uint32_t *store) {
    uint32_t lower = 0;
    uint32_t upper = index->count;
    while (lower < upper) {
        uint32_t middle = lower + (upper - lower) / 2u;
        const XrFrozenSharedStoreRow *row = &index->rows[middle];
        if (row->owner < owner || (row->owner == owner && row->slot < slot))
            lower = middle + 1u;
        else
            upper = middle;
    }
    *store = XR_SEMANTIC_INDEX_NONE;
    if (lower == index->count || index->rows[lower].owner != owner ||
        index->rows[lower].slot != slot)
        return true;
    *store = index->rows[lower].operation;
    return lower + 1u == index->count || index->rows[lower + 1u].owner != owner ||
           index->rows[lower + 1u].slot != slot;
}

static bool operation_can_cross_activation_boundary(const XrSemanticOperationRecord *operation) {
    return operation &&
           (operation->opcode == XI_CALL || operation->opcode == XI_TAIL_CALL ||
            operation->opcode == XI_CALL_METHOD || operation->opcode == XI_CALL_METHOD_DIRECT ||
            operation->opcode == XI_CALL_BUILTIN || operation->opcode == XI_GO);
}

static bool root_store_precedes_activation(const XrSemanticPlan *plan, uint32_t store) {
    const XrSemanticFunctionRecord *root = &plan->functions[0];
    const XrSemanticOperationRecord *record = &plan->operations[store];
    if (record->block != root->block_begin)
        return false;
    const XrSemanticBlockRecord *entry = &plan->blocks[root->block_begin];
    for (uint32_t operation = entry->operation_begin; operation < store; operation++) {
        if (operation_can_cross_activation_boundary(&plan->operations[operation]))
            return false;
    }
    return store >= entry->operation_begin &&
           store < entry->operation_begin + entry->operation_count;
}

static uint32_t resolve_frozen_shared_target_depth(const XrSemanticPlan *plan,
                                                   const uint32_t *definitions,
                                                   const XrSemanticGraph *graph,
                                                   const XrFrozenSharedStoreIndex *stores,
                                                   uint32_t value_count, uint32_t caller,
                                                   uint32_t load, int64_t slot, unsigned depth);

static uint32_t
frozen_chained_shared_target(const XrSemanticPlan *plan, const uint32_t *definitions,
                             const XrSemanticGraph *graph, const XrFrozenSharedStoreIndex *stores,
                             uint32_t value_count, uint32_t owner, uint32_t value, unsigned depth) {
    if (depth >= XR_SEMANTIC_MAX_SHARED_CALLEE_HOPS)
        return XR_SEMANTIC_INDEX_NONE;
    for (uint32_t step = 0; step < plan->operation_count; step++) {
        if (value >= value_count || definitions[value] >= plan->operation_count)
            return XR_SEMANTIC_INDEX_NONE;
        uint32_t producer_index = definitions[value];
        const XrSemanticOperationRecord *producer = &plan->operations[producer_index];
        if (producer->function != owner)
            return XR_SEMANTIC_INDEX_NONE;
        if (producer->opcode == XI_GET_SHARED && producer->semantic_immediate >= 0)
            return resolve_frozen_shared_target_depth(plan, definitions, graph, stores, value_count,
                                                      owner, producer_index,
                                                      producer->semantic_immediate, depth + 1u);
        if (!frozen_operation_forwards_callable(producer))
            return XR_SEMANTIC_INDEX_NONE;
        value = plan->operands[producer->operand_begin].value;
    }
    return XR_SEMANTIC_INDEX_NONE;
}

static uint32_t resolve_frozen_shared_target_depth(const XrSemanticPlan *plan,
                                                   const uint32_t *definitions,
                                                   const XrSemanticGraph *graph,
                                                   const XrFrozenSharedStoreIndex *stores,
                                                   uint32_t value_count, uint32_t caller,
                                                   uint32_t load, int64_t slot, unsigned depth) {
    for (uint32_t owner = caller; owner != XR_SEMANTIC_INDEX_NONE;
         owner = plan->functions[owner].parent) {
        uint32_t store;
        if (!find_frozen_shared_store(stores, owner, slot, &store))
            return XR_SEMANTIC_INDEX_NONE;
        if (store == XR_SEMANTIC_INDEX_NONE)
            continue;
        const XrSemanticOperationRecord *record = &plan->operations[store];
        const XrSemanticOperationRecord *load_record = &plan->operations[load];
        bool initialized =
            owner == caller
                ? (record->block == load_record->block
                       ? store < load
                       : xr_semantic_graph_dominates(graph, record->block, load_record->block))
                : owner == 0 && plan->functions[owner].parent == XR_SEMANTIC_INDEX_NONE &&
                      root_store_precedes_activation(plan, store);
        if (record->operand_count != 1 || !initialized)
            return XR_SEMANTIC_INDEX_NONE;
        uint32_t bound = resolve_frozen_closure_value(plan, definitions, value_count, owner,
                                                      plan->operands[record->operand_begin].value);
        if (bound != XR_SEMANTIC_INDEX_NONE)
            return bound;
        return frozen_chained_shared_target(plan, definitions, graph, stores, value_count, owner,
                                            plan->operands[record->operand_begin].value, depth);
    }
    return XR_SEMANTIC_INDEX_NONE;
}

static uint32_t
resolve_frozen_shared_target(const XrSemanticPlan *plan, const uint32_t *definitions,
                             const XrSemanticGraph *graph, const XrFrozenSharedStoreIndex *stores,
                             uint32_t value_count, uint32_t caller, uint32_t load, int64_t slot) {
    return resolve_frozen_shared_target_depth(plan, definitions, graph, stores, value_count, caller,
                                              load, slot, 0u);
}

static uint32_t resolve_frozen_direct_call_target(const XrSemanticPlan *plan,
                                                  const uint32_t *definitions,
                                                  const XrSemanticGraph *graph,
                                                  const XrFrozenSharedStoreIndex *stores,
                                                  uint32_t value_count, uint32_t operation_index) {
    const XrSemanticOperationRecord *call = &plan->operations[operation_index];
    if ((call->opcode != XI_CALL && call->opcode != XI_TAIL_CALL) || call->operand_count == 0)
        return XR_SEMANTIC_INDEX_NONE;
    /* Re-derived from the frozen immediate through the same shared judgement
     * the IR emitter obeys, never read back from the row being checked: a call
     * on its own activation names the function it already sits in, and its
     * callee operand holds a placeholder no layer resolves. */
    if (xi_call_targets_own_frame(call->opcode, call->semantic_immediate))
        return call->function < plan->function_count ? call->function : XR_SEMANTIC_INDEX_NONE;
    uint32_t value = plan->operands[call->operand_begin].value;
    for (uint32_t depth = 0; depth < plan->operation_count; depth++) {
        if (value >= value_count)
            return XR_SEMANTIC_INDEX_NONE;
        uint32_t producer_index = definitions[value];
        if (producer_index >= plan->operation_count)
            return XR_SEMANTIC_INDEX_NONE;
        const XrSemanticOperationRecord *producer = &plan->operations[producer_index];
        if (producer->function != call->function)
            return XR_SEMANTIC_INDEX_NONE;
        bool closure_binding =
            producer->opcode == XI_CLOSURE_NEW ||
            (producer->opcode == XI_STACK_ALLOC && producer->semantic_immediate == XI_CLOSURE_NEW);
        if (closure_binding)
            return producer->callable_function;
        if (producer->opcode == XI_GET_SHARED && producer->semantic_immediate >= 0)
            return resolve_frozen_shared_target(plan, definitions, graph, stores, value_count,
                                                call->function, producer_index,
                                                producer->semantic_immediate);
        if (!frozen_operation_forwards_callable(producer))
            return XR_SEMANTIC_INDEX_NONE;
        value = plan->operands[producer->operand_begin].value;
    }
    return XR_SEMANTIC_INDEX_NONE;
}

static bool resolve_frozen_native_yieldable_target(const XrSemanticPlan *plan,
                                                   const uint32_t *definitions,
                                                   uint32_t value_count, uint32_t operation_index,
                                                   const char **module, const char **member) {
    const XrSemanticOperationRecord *call = &plan->operations[operation_index];
    if (call->opcode != XI_CALL || call->operand_count == 0 ||
        plan->operands[call->operand_begin].role != XR_SEM_OPERAND_CALLEE)
        return false;
    uint32_t value = plan->operands[call->operand_begin].value;
    for (uint32_t depth = 0; depth < plan->operation_count; depth++) {
        if (value >= value_count)
            return false;
        uint32_t producer_index = definitions[value];
        if (producer_index >= plan->operation_count)
            return false;
        const XrSemanticOperationRecord *producer = &plan->operations[producer_index];
        if (producer->function != call->function)
            return false;
        if (producer->opcode == XI_IMPORT_REF) {
            if (producer->metadata_count != 2 || producer->metadata_begin > plan->metadata_count ||
                producer->metadata_count > plan->metadata_count - producer->metadata_begin)
                return false;
            if (producer->import_resolution != XR_SEM_IMPORT_RESOLUTION_NATIVE_STDLIB)
                return false;
            const char *candidate_module = plan->metadata[producer->metadata_begin];
            const char *candidate_member = plan->metadata[producer->metadata_begin + 1];
            const XrStdlibDefEntry *binding =
                xr_stdlib_metadata_unique_func(candidate_module, candidate_member);
            if (!binding || !binding->signature || !binding->vm || !binding->vm_binding ||
                strcmp(binding->vm_binding, "yieldable") != 0 ||
                call->operand_count != (uint16_t) (binding->argc + 1u))
                return false;
            *module = candidate_module;
            *member = candidate_member;
            return true;
        }
        if (producer->opcode != XI_COPY || producer->semantic_immediate != XI_COPY_KIND_IDENTITY ||
            producer->operand_count != 1 || producer->result_alias_operand != 0)
            return false;
        value = plan->operands[producer->operand_begin].value;
    }
    return false;
}

/* This proof deliberately stops at the open function-value boundary. It says
 * nothing about the members of the target set and therefore cannot authorize
 * execution; it only freezes the conservative coroutine-state obligation. */
static bool frozen_operation_has_coroutine_state(const XrSemanticPlan *plan, uint32_t operation) {
    for (uint32_t entity = 0; entity < plan->entity_count; entity++)
        if (plan->entities[entity].kind == XR_SEM_ENTITY_COROUTINE_STATE &&
            plan->entities[entity].subject_kind == XR_SEM_ENTITY_SUBJECT_OPERATION &&
            plan->entities[entity].subject == operation)
            return true;
    return false;
}

static uint32_t resolve_frozen_indirect_callable_type(const XrSemanticPlan *plan,
                                                      const uint32_t *definitions,
                                                      uint32_t value_count,
                                                      uint32_t operation_index) {
    const XrSemanticOperationRecord *call = &plan->operations[operation_index];
    if (call->opcode != XI_CALL || call->operand_count == 0 ||
        call->operand_begin >= plan->operand_count)
        return XR_SEMANTIC_INDEX_NONE;
    const XrSemanticOperandRecord *callee = &plan->operands[call->operand_begin];
    if (callee->role != XR_SEM_OPERAND_CALLEE || callee->type >= plan->type_count ||
        plan->types[callee->type].kind != XR_KIND_FUNCTION)
        return XR_SEMANTIC_INDEX_NONE;
    uint32_t value = callee->value;
    for (uint32_t depth = 0; depth < plan->operation_count; depth++) {
        if (value >= value_count)
            return XR_SEMANTIC_INDEX_NONE;
        uint32_t producer_index = definitions[value];
        if (producer_index >= plan->operation_count) {
            uint32_t parameter = producer_index - plan->operation_count;
            return parameter < plan->parameter_count &&
                           plan->parameters[parameter].function == call->function
                       ? callee->type
                       : XR_SEMANTIC_INDEX_NONE;
        }
        const XrSemanticOperationRecord *producer = &plan->operations[producer_index];
        if (producer->function != call->function)
            return XR_SEMANTIC_INDEX_NONE;
        if (producer->opcode == XI_COPY && producer->semantic_immediate == XI_COPY_KIND_IDENTITY &&
            producer->operand_count == 1 && producer->result_alias_operand == 0) {
            value = plan->operands[producer->operand_begin].value;
            continue;
        }
        if (producer->opcode == XI_GET_SHARED)
            return frozen_operation_has_coroutine_state(plan, operation_index)
                       ? callee->type
                       : XR_SEMANTIC_INDEX_NONE;
        if (producer->opcode == XI_IMPORT_REF || producer->opcode == XI_GET_BUILTIN ||
            producer->opcode == XI_CLOSURE_NEW ||
            (producer->opcode == XI_STACK_ALLOC && producer->semantic_immediate == XI_CLOSURE_NEW))
            return XR_SEMANTIC_INDEX_NONE;
        return callee->type;
    }
    return XR_SEMANTIC_INDEX_NONE;
}

static bool resolve_frozen_source_import(const XrSemanticPlan *plan, const uint32_t *definitions,
                                         uint32_t value_count,
                                         const XrFrozenSharedStoreIndex *stores,
                                         uint32_t caller_function, uint32_t value,
                                         bool namespace_import, const char **module_path,
                                         const char **member) {
    uint32_t load = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t depth = 0; depth < plan->operation_count; depth++) {
        if (value >= value_count || definitions[value] >= plan->operation_count)
            return false;
        const XrSemanticOperationRecord *producer = &plan->operations[definitions[value]];
        if (producer->function != caller_function)
            return false;
        if (producer->opcode == XI_GET_SHARED && producer->semantic_immediate >= 0) {
            load = definitions[value];
            break;
        }
        if (producer->opcode != XI_COPY || producer->semantic_immediate != XI_COPY_KIND_IDENTITY ||
            producer->operand_count != 1 || producer->result_alias_operand != 0)
            return false;
        value = plan->operands[producer->operand_begin].value;
    }
    if (load == XR_SEMANTIC_INDEX_NONE)
        return false;
    int64_t slot = plan->operations[load].semantic_immediate;
    uint32_t store = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t owner = caller_function; owner != XR_SEMANTIC_INDEX_NONE;
         owner = plan->functions[owner].parent) {
        uint32_t candidate = XR_SEMANTIC_INDEX_NONE;
        if (!find_frozen_shared_store(stores, owner, slot, &candidate))
            return false;
        if (candidate == XR_SEMANTIC_INDEX_NONE)
            continue;
        if (owner != 0 || plan->functions[0].parent != XR_SEMANTIC_INDEX_NONE ||
            !root_store_precedes_activation(plan, candidate))
            return false;
        store = candidate;
        break;
    }
    if (store == XR_SEMANTIC_INDEX_NONE || plan->operations[store].operand_count != 1)
        return false;
    value = plan->operands[plan->operations[store].operand_begin].value;
    for (uint32_t depth = 0; depth < plan->operation_count; depth++) {
        if (value >= value_count || definitions[value] >= plan->operation_count)
            return false;
        const XrSemanticOperationRecord *producer = &plan->operations[definitions[value]];
        if (producer->function != 0)
            return false;
        if (producer->opcode == XI_IMPORT_REF) {
            if (producer->metadata_count != 2 || producer->metadata_begin > plan->metadata_count ||
                producer->metadata_count > plan->metadata_count - producer->metadata_begin ||
                producer->import_resolution != XR_SEM_IMPORT_RESOLUTION_SOURCE_MODULE ||
                plan->metadata[producer->metadata_begin][0] == '\0' ||
                (plan->metadata[producer->metadata_begin + 1][0] == '\0') != namespace_import)
                return false;
            *module_path = plan->metadata[producer->metadata_begin];
            *member = plan->metadata[producer->metadata_begin + 1];
            return true;
        }
        if (producer->opcode != XI_COPY || producer->semantic_immediate != XI_COPY_KIND_IDENTITY ||
            producer->operand_count != 1 || producer->result_alias_operand != 0)
            return false;
        value = plan->operands[producer->operand_begin].value;
    }
    return false;
}

static bool
resolve_frozen_source_namespace_target(const XrSemanticPlan *plan, const uint32_t *definitions,
                                       uint32_t value_count, const XrFrozenSharedStoreIndex *stores,
                                       uint32_t operation_index, const char **module_path,
                                       const char **selector) {
    const XrSemanticOperationRecord *call = &plan->operations[operation_index];
    if (call->operand_count == 0)
        return false;
    if (call->opcode == XI_CALL_METHOD) {
        if ((call->semantic_immediate & 1) != 0 || call->metadata_count != 1 ||
            plan->operands[call->operand_begin].role != XR_SEM_OPERAND_RECEIVER)
            return false;
        const char *member = NULL;
        if (!resolve_frozen_source_import(plan, definitions, value_count, stores, call->function,
                                          plan->operands[call->operand_begin].value, true,
                                          module_path, &member) ||
            !member || member[0] != '\0')
            return false;
        *selector = plan->metadata[call->metadata_begin];
        return (*selector)[0] != '\0';
    }
    if (call->opcode != XI_CALL || call->metadata_count != 0 ||
        plan->operands[call->operand_begin].role != XR_SEM_OPERAND_CALLEE)
        return false;
    return resolve_frozen_source_import(plan, definitions, value_count, stores, call->function,
                                        plan->operands[call->operand_begin].value, false,
                                        module_path, selector);
}

static bool resolve_frozen_native_namespace_yieldable_target(
    const XrSemanticPlan *plan, const uint32_t *definitions, uint32_t value_count,
    const XrFrozenSharedStoreIndex *stores, uint32_t operation_index, const char **module_path,
    const char **selector) {
    const XrSemanticOperationRecord *call = &plan->operations[operation_index];
    if (call->opcode != XI_CALL_METHOD || (call->semantic_immediate & 1) != 0 ||
        call->operand_count == 0 || call->metadata_count != 1 ||
        plan->operands[call->operand_begin].role != XR_SEM_OPERAND_RECEIVER)
        return false;
    uint32_t value = plan->operands[call->operand_begin].value;
    uint32_t load = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t depth = 0; depth < plan->operation_count; depth++) {
        if (value >= value_count || definitions[value] >= plan->operation_count)
            return false;
        const XrSemanticOperationRecord *producer = &plan->operations[definitions[value]];
        if (producer->function != call->function)
            return false;
        if (producer->opcode == XI_GET_SHARED && producer->semantic_immediate >= 0) {
            load = definitions[value];
            break;
        }
        if (producer->opcode != XI_COPY || producer->semantic_immediate != XI_COPY_KIND_IDENTITY ||
            producer->operand_count != 1 || producer->result_alias_operand != 0)
            return false;
        value = plan->operands[producer->operand_begin].value;
    }
    if (load == XR_SEMANTIC_INDEX_NONE)
        return false;
    int64_t slot = plan->operations[load].semantic_immediate;
    uint32_t store = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t owner = call->function; owner != XR_SEMANTIC_INDEX_NONE;
         owner = plan->functions[owner].parent) {
        uint32_t candidate = XR_SEMANTIC_INDEX_NONE;
        if (!find_frozen_shared_store(stores, owner, slot, &candidate))
            return false;
        if (candidate == XR_SEMANTIC_INDEX_NONE)
            continue;
        if (owner != 0 || plan->functions[0].parent != XR_SEMANTIC_INDEX_NONE ||
            !root_store_precedes_activation(plan, candidate))
            return false;
        store = candidate;
        break;
    }
    if (store == XR_SEMANTIC_INDEX_NONE || plan->operations[store].operand_count != 1)
        return false;
    value = plan->operands[plan->operations[store].operand_begin].value;
    for (uint32_t depth = 0; depth < plan->operation_count; depth++) {
        if (value >= value_count || definitions[value] >= plan->operation_count)
            return false;
        const XrSemanticOperationRecord *producer = &plan->operations[definitions[value]];
        if (producer->function != 0)
            return false;
        if (producer->opcode == XI_IMPORT_REF) {
            if (producer->metadata_count != 2 || producer->metadata_begin > plan->metadata_count ||
                producer->metadata_count > plan->metadata_count - producer->metadata_begin ||
                producer->import_resolution != XR_SEM_IMPORT_RESOLUTION_NATIVE_STDLIB ||
                plan->metadata[producer->metadata_begin][0] == '\0' ||
                plan->metadata[producer->metadata_begin + 1][0] != '\0')
                return false;
            const char *candidate_module = plan->metadata[producer->metadata_begin];
            const char *candidate_selector = plan->metadata[call->metadata_begin];
            const XrStdlibDefEntry *binding =
                xr_stdlib_metadata_unique_func(candidate_module, candidate_selector);
            if (!binding || !binding->signature || !binding->vm || !binding->vm_binding ||
                strcmp(binding->vm_binding, "yieldable") != 0 ||
                call->operand_count != (uint16_t) (binding->argc + 1u))
                return false;
            *module_path = candidate_module;
            *selector = candidate_selector;
            return true;
        }
        if (producer->opcode != XI_COPY || producer->semantic_immediate != XI_COPY_KIND_IDENTITY ||
            producer->operand_count != 1 || producer->result_alias_operand != 0)
            return false;
        value = plan->operands[producer->operand_begin].value;
    }
    return false;
}

static const char *resolve_frozen_builtin_instance_yieldable_target(const XrSemanticPlan *plan,
                                                                    uint32_t operation_index,
                                                                    uint32_t *receiver_type) {
    const XrSemanticOperationRecord *call = &plan->operations[operation_index];
    if (call->opcode != XI_CALL_METHOD || (call->semantic_immediate & 1) != 0 ||
        call->operand_count == 0 || call->metadata_count != 1 ||
        plan->operands[call->operand_begin].role != XR_SEM_OPERAND_RECEIVER)
        return NULL;
    const XrSemanticOperandRecord *receiver = &plan->operands[call->operand_begin];
    if (receiver->type >= plan->type_count)
        return NULL;
    const XrSemanticTypeRecord *type = &plan->types[receiver->type];
    if (!semantic_builtin_type_identity_exact(type))
        return NULL;
    const char *selector = plan->metadata[call->metadata_begin];
    uint16_t argument_count = (uint16_t) (call->operand_count - 1u);
    /* One table answers this, shared with the builder that wrote the row and
     * restated in the Xi coroutine analysis that decided the operation
     * suspends. Spelling the selectors again here is what let the three drift
     * apart in the first place. */
    uint32_t builtin_type =
        xr_semantic_yieldable_builtin_id((unsigned) type->kind, type->builtin_type);
    if (!xr_semantic_builtin_instance_yieldable(builtin_type, selector, argument_count))
        return NULL;
    *receiver_type = receiver->type;
    return xr_semantic_yieldable_builtin_name(builtin_type);
}

static bool stable_id_zero(XrStableId id) {
    XrStableId zero = {{0}};
    return xr_stable_id_equal(id, zero);
}

static const XrSemanticEntityRecord *verify_plan_module_entity(const XrSemanticPlan *plan) {
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

static bool verify_source_export_rows(const XrSemanticPlan *plan, const uint32_t *definitions,
                                      uint32_t value_count, char *error, size_t error_size) {
    const char *previous = NULL;
    uint32_t annotated = 0;
    uint32_t root_shared_count = 0;
    for (uint32_t operation = 0; operation < plan->operation_count; operation++) {
        const XrSemanticOperationRecord *candidate = &plan->operations[operation];
        if (candidate->function == 0 && candidate->opcode == XI_SET_SHARED &&
            candidate->semantic_immediate >= 0 &&
            (uint64_t) candidate->semantic_immediate < UINT32_MAX) {
            uint32_t required = (uint32_t) candidate->semantic_immediate + 1u;
            if (required > root_shared_count)
                root_shared_count = required;
        }
    }
    uint32_t *root_shared_stores =
        root_shared_count
            ? (uint32_t *) xr_malloc((size_t) root_shared_count * sizeof(*root_shared_stores))
            : NULL;
    uint8_t *root_shared_ambiguous =
        root_shared_count ? (uint8_t *) xr_calloc(root_shared_count, 1) : NULL;
    if (root_shared_count && (!root_shared_stores || !root_shared_ambiguous)) {
        xr_free(root_shared_stores);
        xr_free(root_shared_ambiguous);
        return report(error, error_size, "XR_EXEC_5003",
                      "source-export shared-store index allocation failed");
    }
    for (uint32_t slot = 0; slot < root_shared_count; slot++)
        root_shared_stores[slot] = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t operation = 0; operation < plan->operation_count; operation++) {
        const XrSemanticOperationRecord *candidate = &plan->operations[operation];
        if (candidate->function == 0 && candidate->opcode == XI_SET_SHARED &&
            candidate->semantic_immediate >= 0 &&
            (uint64_t) candidate->semantic_immediate < root_shared_count) {
            uint32_t slot = (uint32_t) candidate->semantic_immediate;
            if (root_shared_stores[slot] != XR_SEMANTIC_INDEX_NONE)
                root_shared_ambiguous[slot] = 1;
            else
                root_shared_stores[slot] = operation;
        }
        if (candidate->opcode == XI_SET_SHARED && candidate->metadata_count == 2 &&
            (strcmp(plan->metadata[candidate->metadata_begin], "source-export-v1") == 0 ||
             strcmp(plan->metadata[candidate->metadata_begin], "source-class-export-v1") == 0))
            annotated++;
    }
    if (annotated != plan->source_export_count) {
        xr_free(root_shared_stores);
        xr_free(root_shared_ambiguous);
        return report(error, error_size, "XR_SEM_0019",
                      "source-export table does not exactly cover root export stores");
    }
    for (uint32_t i = 0; i < plan->source_export_count; i++) {
        const XrSemanticSourceExportRecord *record = &plan->source_exports[i];
        uint32_t store = record->shared_slot < root_shared_count
                             ? root_shared_stores[record->shared_slot]
                             : XR_SEMANTIC_INDEX_NONE;
        bool function_export = record->kind == XR_SEM_SOURCE_EXPORT_FUNCTION &&
                               record->function < plan->function_count &&
                               record->source_class == XR_SEMANTIC_INDEX_NONE;
        bool class_export = record->kind == XR_SEM_SOURCE_EXPORT_SOURCE_CLASS &&
                            record->function == XR_SEMANTIC_INDEX_NONE &&
                            record->source_class < plan->source_class_count;
        if (!record->name || !record->name[0] || (!function_export && !class_export) ||
            record->reserved[0] != 0 || record->reserved[1] != 0 || record->reserved[2] != 0 ||
            (previous && strcmp(previous, record->name) >= 0) ||
            record->shared_slot >= root_shared_count ||
            root_shared_ambiguous[record->shared_slot] || store == XR_SEMANTIC_INDEX_NONE ||
            !root_store_precedes_activation(plan, store)) {
            xr_free(root_shared_stores);
            xr_free(root_shared_ambiguous);
            return report(error, error_size, "XR_SEM_0019", "source-export table is not canonical");
        }
        const XrSemanticOperationRecord *operation = &plan->operations[store];
        uint32_t target =
            function_export && operation->operand_count == 1
                ? resolve_frozen_closure_value(plan, definitions, value_count, 0,
                                               plan->operands[operation->operand_begin].value)
                : XR_SEMANTIC_INDEX_NONE;
        const XrSemanticOperationRecord *class_definition = NULL;
        uint32_t value = operation->operand_count == 1
                             ? plan->operands[operation->operand_begin].value
                             : XR_SEMANTIC_INDEX_NONE;
        for (uint32_t depth = 0; class_export && depth < plan->operation_count; depth++) {
            if (value >= value_count || definitions[value] >= plan->operation_count)
                break;
            const XrSemanticOperationRecord *producer = &plan->operations[definitions[value]];
            if (producer->function != 0)
                break;
            if (producer->opcode == XI_COPY &&
                producer->semantic_immediate == XI_COPY_KIND_IDENTITY &&
                producer->operand_count == 1 && producer->result_alias_operand == 0) {
                value = plan->operands[producer->operand_begin].value;
                continue;
            }
            class_definition = producer;
            break;
        }
        uint32_t source_class = class_export
                                    ? xr_semantic_class_object_source_class(plan, class_definition)
                                    : XR_SEMANTIC_INDEX_NONE;
        char expected[512];
        char subject_id[XR_STABLE_ID_BYTES * 2 + 1];
        if (function_export)
            xr_stable_id_hex(plan->functions[record->function].id, subject_id);
        else if (source_class < plan->source_class_count)
            xr_stable_id_hex(plan->source_classes[source_class].id, subject_id);
        else
            subject_id[0] = '\0';
        int length =
            snprintf(expected, sizeof(expected),
                     function_export
                         ? "source-export-v1:schema=%u:name=%zu:%s:function=%s:slot=%u"
                         : "source-class-export-v1:schema=%u:name=%zu:%s:source-class=%s:slot=%u",
                     XR_SEMANTIC_SCHEMA_VERSION, strlen(record->name), record->name, subject_id,
                     record->shared_slot);
        const char *expected_tag = function_export ? "source-export-v1" : "source-class-export-v1";
        XrStableId exported_entity = function_export ? plan->functions[record->function].id
                                                     : (source_class < plan->source_class_count
                                                            ? plan->source_classes[source_class].id
                                                            : (XrStableId) {{0}});
        if (!xr_stable_id_equal(record->exported_entity, exported_entity) ||
            (function_export && target != record->function) ||
            (class_export && (source_class != record->source_class ||
                              !xr_semantic_class_declaration_is_frozen(plan, source_class))) ||
            operation->metadata_count != 2 ||
            strcmp(plan->metadata[operation->metadata_begin], expected_tag) != 0 ||
            strcmp(plan->metadata[operation->metadata_begin + 1], record->name) != 0 ||
            length <= 0 || (size_t) length >= sizeof(expected) ||
            strcmp(record->canonical_key ? record->canonical_key : "", expected) != 0 ||
            !verify_id(record->canonical_key, record->id)) {
            xr_free(root_shared_stores);
            xr_free(root_shared_ambiguous);
            return report(error, error_size, "XR_SEM_0019",
                          "source-export row is not grounded in the root initializer");
        }
        previous = record->name;
    }
    xr_free(root_shared_stores);
    xr_free(root_shared_ambiguous);
    return true;
}

static int verify_aggregate_type_kind(const XrSemanticTypeRecord *type) {
    if (!type)
        return -1;
    if ((type->flags & XR_SEM_TYPE_NULLABLE) != 0)
        return 0;
    if (type->kind == XR_KIND_TUPLE || type->kind == XR_KIND_FIXED_ARRAY)
        return type->scalar_rep == XR_SCALAR_REP_NONE ? 1 : -1;
    if (type->kind == XR_KIND_STRUCT_OBJECT)
        return (type->flags & XR_SEM_TYPE_VALUE) == 0
                   ? 0
                   : (type->scalar_rep == XR_SCALAR_REP_NONE ? 1 : -1);
    if (type->kind == XR_KIND_INSTANCE)
        return (type->flags & XR_SEM_TYPE_AGGREGATE_EXACT) == 0
                   ? 0
                   : (type->scalar_rep == XR_SCALAR_REP_NONE ? 1 : -1);
    return 0;
}

static bool verify_source_structural_shape_is_exact(const XrSemanticPlan *plan,
                                                    uint32_t semantic_type) {
    const XrSemanticTypeRecord *type = xr_semantic_plan_type(plan, semantic_type);
    if (!plan || !type || type->kind != XR_KIND_STRUCT_OBJECT ||
        type->scalar_rep != XR_SCALAR_REP_NONE || type->child_count == 0 ||
        type->aggregate_extent != type->child_count ||
        (type->flags &
         (XR_SEM_TYPE_NULLABLE | XR_SEM_TYPE_BORROW_VIEW | XR_SEM_TYPE_AGGREGATE_EXACT)) != 0 ||
        type->child_count > 64u)
        return false;
    uint32_t type_entity = XR_SEMANTIC_INDEX_NONE;
    uint32_t shape_entity = XR_SEMANTIC_INDEX_NONE;
    uint64_t field_mask = 0;
    for (uint32_t i = 0; i < plan->entity_count; i++) {
        const XrSemanticEntityRecord *entity = &plan->entities[i];
        if (entity->subject_kind != XR_SEM_ENTITY_SUBJECT_TYPE || entity->subject != semantic_type)
            continue;
        if (entity->kind == XR_SEM_ENTITY_TYPE_INSTANTIATION) {
            if (type_entity != XR_SEMANTIC_INDEX_NONE)
                return false;
            type_entity = i;
        } else if (entity->kind == XR_SEM_ENTITY_SHAPE) {
            if (shape_entity != XR_SEMANTIC_INDEX_NONE)
                return false;
            shape_entity = i;
        }
    }
    if (type_entity == XR_SEMANTIC_INDEX_NONE || shape_entity == XR_SEMANTIC_INDEX_NONE ||
        plan->entities[shape_entity].parent != type_entity)
        return false;
    for (uint32_t i = 0; i < plan->entity_count; i++) {
        const XrSemanticEntityRecord *entity = &plan->entities[i];
        if (entity->kind != XR_SEM_ENTITY_FIELD || entity->parent != shape_entity)
            continue;
        if (entity->subject_kind != XR_SEM_ENTITY_SUBJECT_TYPE ||
            entity->subject != semantic_type || entity->ordinal >= type->child_count ||
            (field_mask & (UINT64_C(1) << entity->ordinal)) != 0)
            return false;
        field_mask |= UINT64_C(1) << entity->ordinal;
    }
    return field_mask == (type->child_count == 64u
                              ? UINT64_MAX
                              : (UINT64_C(1) << type->child_count) - UINT64_C(1));
}

/* Independent reconstruction of the managed-field aggregate precursor. This
 * deliberately does not call xr_semantic_managed_aggregate_field_graph(): the
 * builder-side normalized view and this verifier must disagree if either one
 * starts admitting another leaf, recursion rule, or field geometry. */
static bool verify_managed_aggregate_field_graph(const XrSemanticPlan *plan, uint32_t semantic_type,
                                                 uint32_t *stack, uint32_t depth,
                                                 uint32_t *managed_fields) {
    const XrSemanticTypeRecord *type = xr_semantic_plan_type(plan, semantic_type);
    if (!plan || !type || !stack || !managed_fields || depth >= 64u)
        return false;
    if (xr_semantic_tagged_string_type_is_exact(type) || xr_semantic_adt_enum_type_is_exact(type)) {
        if (*managed_fields == UINT32_MAX)
            return false;
        (*managed_fields)++;
        return true;
    }
    if (xr_semantic_unit_enum_type_is_exact(type))
        return true;
    switch ((XrTypeKind) type->kind) {
        case XR_KIND_INT:
        case XR_KIND_FLOAT:
        case XR_KIND_BOOL:
        case XR_KIND_RUNE:
            return (type->flags & (XR_SEM_TYPE_NULLABLE | XR_SEM_TYPE_REFERENCE_CAPABLE |
                                   XR_SEM_TYPE_BORROW_VIEW | XR_SEM_TYPE_AGGREGATE_EXACT)) == 0;
        default:
            break;
    }
    if (verify_aggregate_type_kind(type) != 1 &&
        !verify_source_structural_shape_is_exact(plan, semantic_type))
        return false;
    for (uint32_t i = 0; i < depth; i++)
        if (stack[i] == semantic_type)
            return false;
    if (!plan->type_children || type->child_begin > plan->type_child_count ||
        type->child_count > plan->type_child_count - type->child_begin ||
        (type->kind == XR_KIND_FIXED_ARRAY ? (type->child_count != 1 || type->aggregate_extent == 0)
                                           : type->aggregate_extent != type->child_count))
        return false;
    stack[depth] = semantic_type;
    uint32_t repetitions = type->kind == XR_KIND_FIXED_ARRAY ? type->aggregate_extent : 1u;
    uint32_t dependencies = type->kind == XR_KIND_FIXED_ARRAY ? 1u : type->child_count;
    for (uint32_t repetition = 0; repetition < repetitions; repetition++)
        for (uint32_t i = 0; i < dependencies; i++)
            if (!verify_managed_aggregate_field_graph(plan,
                                                      plan->type_children[type->child_begin + i],
                                                      stack, depth + 1u, managed_fields))
                return false;
    return true;
}

static bool verify_direct_local_managed_aggregate_boundaries(
    const XrSemanticPlan *plan, const XrSemanticOperationRecord *operation,
    const XrSemanticFunctionRecord *callee, char *error, size_t error_size) {
    uint32_t parameter_count = callee ? callee->parameter_count : 0;
    const XrSemanticParameterRecord *last_parameter =
        plan && callee && parameter_count > 0 &&
                callee->parameter_begin <= plan->parameter_count &&
                parameter_count <= plan->parameter_count - callee->parameter_begin
            ? &plan->parameters[callee->parameter_begin + parameter_count - 1u]
            : NULL;
    bool variadic = last_parameter &&
                    (last_parameter->flags & XR_SEM_PARAMETER_VARIADIC) != 0;
    uint32_t minimum_operands = variadic ? parameter_count : parameter_count + 1u;
    if (!plan || !operation || !callee ||
        (operation->opcode != XI_CALL && operation->opcode != XI_TAIL_CALL) ||
        (!variadic && operation->operand_count != minimum_operands) ||
        (variadic && operation->operand_count < minimum_operands))
        return report(error, error_size, "XR_SEM_0019",
                      "direct-local managed aggregate call shape is incomplete");
    for (uint32_t ordinal = 0; ordinal < callee->parameter_count; ordinal++) {
        uint32_t parameter_index = callee->parameter_begin + ordinal;
        uint32_t operand_index = operation->operand_begin + ordinal + 1u;
        const XrSemanticParameterRecord *parameter =
            xr_semantic_plan_parameter(plan, parameter_index);
        const XrSemanticOperandRecord *operand =
            operand_index < plan->operand_count ? &plan->operands[operand_index] : NULL;
        if (!parameter || (parameter->flags & XR_SEM_PARAMETER_VARIADIC) != 0 ||
            parameter->mode != XR_PARAM_READ ||
            parameter->ownership != XI_OWN_BORROWED ||
            parameter->transfer_mode != XR_TRANSFER_SHARE)
            continue;
        uint32_t stack[64] = {0};
        uint32_t managed_fields = 0;
        if (!verify_managed_aggregate_field_graph(plan, parameter->type, stack, 0,
                                                  &managed_fields) ||
            managed_fields == 0)
            continue;
        if (parameter->function >= plan->function_count ||
            &plan->functions[parameter->function] != callee || parameter->ordinal != ordinal ||
            !operand || parameter->type != operand->type || parameter->mode != XR_PARAM_READ ||
            parameter->ownership != XI_OWN_BORROWED ||
            parameter->transfer_mode != XR_TRANSFER_SHARE ||
            (parameter->flags & (uint8_t) ~XR_SEM_PARAMETER_REQUIRED) != 0 ||
            parameter->reserved != 0 || operand->role != XR_SEM_OPERAND_ARGUMENT ||
            operand->parameter != (int16_t) ordinal || operand->parameter_mode != XR_PARAM_READ ||
            operand->transfer_mode != XR_TRANSFER_SHARE ||
            operand->ownership_action != XR_SEM_OPERAND_BORROW ||
            operand->access != XR_CALL_ARG_PLAIN || operand->origin != XI_PLACE_ORIGIN_NONE ||
            operand->lifetime != XI_PLACE_LIFETIME_NONE ||
            operand->escape != XI_PLACE_ESCAPE_NONE ||
            operand->flags != XR_SEM_OPERAND_CALL_CONTRACT)
            return report(error, error_size, "XR_SEM_0019",
                          "direct-local managed aggregate borrow authority is inconsistent");
    }
    return true;
}

static bool verify_call_targets(const XrSemanticPlan *plan, const uint32_t *definitions,
                                const XrSemanticGraph *graph, uint32_t value_count, char *error,
                                size_t error_size) {
    XrFrozenSharedStoreIndex stores;
    if (!frozen_shared_store_index_build(plan, &stores))
        return report(error, error_size, "XR_SEM_0019",
                      "call-target verifier cannot allocate shared-store index");
    uint32_t cursor = 0;
    uint32_t program_cursor = 0;
    for (uint32_t operation = 0; operation < plan->operation_count; operation++) {
        const XrSemanticProgramCallBinding *program_binding =
            program_cursor < plan->program_call_binding_count &&
                    plan->program_call_bindings[program_cursor].operation == operation
                ? &plan->program_call_bindings[program_cursor]
                : NULL;
        bool program_bound = program_binding != NULL;
        bool program_external =
            program_bound && program_binding->program_dependency != XR_SEMANTIC_INDEX_NONE;
        uint32_t direct_function =
            program_bound ? (program_external ? XR_SEMANTIC_INDEX_NONE
                                               : program_binding->target_function)
                          : resolve_frozen_direct_call_target(plan, definitions, graph, &stores,
                                                              value_count, operation);
        const char *native_module = NULL;
        const char *native_member = NULL;
        bool native_yieldable = !program_bound && resolve_frozen_native_yieldable_target(
                                                      plan, definitions, value_count, operation,
                                                      &native_module, &native_member);
        const char *source_module = NULL;
        const char *source_selector = NULL;
        bool source_namespace =
            (!program_bound || program_external) &&
            resolve_frozen_source_namespace_target(plan, definitions, value_count, &stores,
                                                    operation, &source_module, &source_selector);
        const char *native_namespace_module = NULL;
        const char *native_namespace_selector = NULL;
        bool native_namespace =
            !program_bound && resolve_frozen_native_namespace_yieldable_target(
                                  plan, definitions, value_count, &stores, operation,
                                  &native_namespace_module, &native_namespace_selector);
        uint32_t builtin_instance_type = XR_SEMANTIC_INDEX_NONE;
        const char *builtin_instance = program_bound
                                           ? NULL
                                           : resolve_frozen_builtin_instance_yieldable_target(
                                                 plan, operation, &builtin_instance_type);
        uint32_t indirect_type =
            program_bound
                ? XR_SEMANTIC_INDEX_NONE
                : resolve_frozen_indirect_callable_type(plan, definitions, value_count, operation);
        uint32_t source_instance_function = XR_SEMANTIC_INDEX_NONE;
        uint32_t source_instance_type = XR_SEMANTIC_INDEX_NONE;
        uint32_t source_instance_class = XR_SEMANTIC_INDEX_NONE;
        const XrSemanticOperationRecord *source_call = &plan->operations[operation];
        if (!program_bound && source_call->opcode == XI_CALL_METHOD &&
            (source_call->semantic_immediate & 1) == 0 && source_call->metadata_count == 1 &&
            source_call->operand_count > 0 && source_call->function < plan->function_count) {
            const XrSemanticOperandRecord *receiver = &plan->operands[source_call->operand_begin];
            if (receiver->role == XR_SEM_OPERAND_RECEIVER && receiver->type < plan->type_count &&
                plan->types[receiver->type].source_class < plan->source_class_count) {
                uint32_t source_class_index = plan->types[receiver->type].source_class;
                const XrSemanticSourceClassRecord *source_class =
                    &plan->source_classes[source_class_index];
                if (xr_semantic_source_class_can_name_one_method(source_class->flags)) {
                    const char *selector = plan->metadata[source_call->metadata_begin];
                    for (uint32_t f = 0; selector && f < plan->function_count; f++) {
                        const XrSemanticFunctionRecord *candidate = &plan->functions[f];
                        if (candidate->source_class != source_class_index ||
                            candidate->source_kind != XR_SEM_SOURCE_FUNCTION_INSTANCE_METHOD ||
                            candidate->parameter_count != source_call->operand_count ||
                            strcmp(candidate->name, selector) != 0)
                            continue;
                        if (source_instance_function != XR_SEMANTIC_INDEX_NONE) {
                            source_instance_function = XR_SEMANTIC_INDEX_NONE;
                            break;
                        }
                        source_instance_function = f;
                        source_instance_type = receiver->type;
                        source_instance_class = source_class_index;
                    }
                }
            }
        }
        const XrSemanticCallTargetRecord *target =
            cursor < plan->call_target_count && plan->call_targets[cursor].operation == operation
                ? &plan->call_targets[cursor]
                : NULL;
        if (!program_bound && source_instance_function == XR_SEMANTIC_INDEX_NONE &&
            source_call->opcode == XI_CALL_METHOD && (source_call->semantic_immediate & 1) == 0 &&
            source_call->metadata_count == 1 && source_call->operand_count > 0 &&
            source_call->function < plan->function_count) {
            const XrSemanticFunctionRecord *caller = &plan->functions[source_call->function];
            const XrSemanticOperandRecord *receiver = &plan->operands[source_call->operand_begin];
            uint32_t candidate_class = caller->source_class;
            const XrSemanticSourceClassRecord *source_class =
                candidate_class < plan->source_class_count ? &plan->source_classes[candidate_class]
                                                           : NULL;
            uint8_t required =
                XR_SEM_SOURCE_CLASS_EXPLICIT_FINAL | XR_SEM_SOURCE_CLASS_RUNTIME_TYPE;
            const char *selector = plan->metadata[source_call->metadata_begin];
            if (source_class && (source_class->flags & required) == required &&
                (source_class->flags & XR_SEM_SOURCE_CLASS_GENERIC) == 0 &&
                caller->source_kind == XR_SEM_SOURCE_FUNCTION_INSTANCE_METHOD &&
                caller->parameter_count > 0 && receiver->role == XR_SEM_OPERAND_RECEIVER &&
                receiver->type < plan->type_count &&
                plan->types[receiver->type].source_class == XR_SEMANTIC_INDEX_NONE) {
                const XrSemanticParameterRecord *self = &plan->parameters[caller->parameter_begin];
                if (receiver->value == self->value && receiver->type == self->type) {
                    for (uint32_t f = 0; selector && f < plan->function_count; f++) {
                        const XrSemanticFunctionRecord *candidate = &plan->functions[f];
                        if (candidate->source_class != candidate_class ||
                            candidate->source_kind != XR_SEM_SOURCE_FUNCTION_INSTANCE_METHOD ||
                            candidate->parameter_count != source_call->operand_count ||
                            strcmp(candidate->name, selector) != 0)
                            continue;
                        if (source_instance_function != XR_SEMANTIC_INDEX_NONE) {
                            source_instance_function = XR_SEMANTIC_INDEX_NONE;
                            break;
                        }
                        source_instance_function = f;
                        source_instance_type = receiver->type;
                        source_instance_class = candidate_class;
                    }
                }
            }
        }
        if (cursor < plan->call_target_count && plan->call_targets[cursor].operation < operation) {
            xr_free(stores.rows);
            return report(error, error_size, "XR_SEM_0019",
                          "call-target table is not in operation order");
        }
        bool source_export = target && target->kind == XR_SEM_CALL_TARGET_SOURCE_EXPORT;
        /* Re-derived from the frozen plan through the shared class-shape
         * judgement, never read back from the row being checked. */
        uint32_t class_construction =
            program_bound ? XR_SEMANTIC_INDEX_NONE
                          : xr_semantic_class_construction_source_class(plan, source_call);
        bool imported_class_result = !program_bound && source_namespace &&
                                     source_call->opcode == XI_CALL &&
                                     xr_semantic_external_class_instance_type_is_exact(
                                         xr_semantic_plan_type(plan, source_call->result_type));
        if (program_bound && !target) {
            xr_free(stores.rows);
            return report(error, error_size, "XR_SEM_0019",
                          "bound program call has no call-target authority");
        }
        if ((direct_function != XR_SEMANTIC_INDEX_NONE || native_yieldable ||
             indirect_type != XR_SEMANTIC_INDEX_NONE || native_namespace || builtin_instance ||
             source_instance_function != XR_SEMANTIC_INDEX_NONE ||
             class_construction != XR_SEMANTIC_INDEX_NONE || imported_class_result) &&
            !target) {
            char detail[512];
            snprintf(detail, sizeof(detail),
                     "provable call has no call-target authority operation=%u function=%u "
                     "name=%s opcode=%u direct=%u native-yieldable=%u indirect=%u "
                     "native-namespace=%u builtin-instance=%u source-instance=%u "
                     "source-module=%s source-selector=%s class-construction=%u "
                     "imported-class-result=%u",
                     operation, source_call->function,
                     source_call->function < plan->function_count &&
                             plan->functions[source_call->function].name
                         ? plan->functions[source_call->function].name
                         : "",
                     source_call->opcode, direct_function, native_yieldable ? 1u : 0u,
                     indirect_type, native_namespace ? 1u : 0u, builtin_instance ? 1u : 0u,
                     source_instance_function, source_module ? source_module : "",
                     source_selector ? source_selector : "", class_construction,
                     imported_class_result ? 1u : 0u);
            xr_free(stores.rows);
            return report(error, error_size, "XR_SEM_0019", detail);
        }
        if (!target)
            continue;
        bool direct =
            direct_function != XR_SEMANTIC_INDEX_NONE && !native_yieldable &&
            target->function == direct_function && target->function < plan->function_count &&
            target->kind == XR_SEM_CALL_TARGET_DIRECT_LOCAL &&
            target->dependency == XR_SEMANTIC_INDEX_NONE &&
            target->source_export == XR_SEMANTIC_INDEX_NONE &&
            stable_id_zero(target->export_identity) && stable_id_zero(target->callee_function) &&
            target->callable_type == XR_SEMANTIC_INDEX_NONE;
        bool native = direct_function == XR_SEMANTIC_INDEX_NONE && native_yieldable &&
                      target->function == XR_SEMANTIC_INDEX_NONE &&
                      target->kind == XR_SEM_CALL_TARGET_NATIVE_YIELDABLE &&
                      target->dependency == XR_SEMANTIC_INDEX_NONE &&
                      target->source_export == XR_SEMANTIC_INDEX_NONE &&
                      stable_id_zero(target->export_identity) &&
                      stable_id_zero(target->callee_function) &&
                      target->callable_type == XR_SEMANTIC_INDEX_NONE;
        bool source_shape =
            source_export && source_namespace && direct_function == XR_SEMANTIC_INDEX_NONE &&
            !native_yieldable && target->function == XR_SEMANTIC_INDEX_NONE &&
            target->dependency < plan->dependency_count &&
            strcmp(plan->dependencies[target->dependency].module_path, source_module) == 0 &&
            !stable_id_zero(target->export_identity) && !stable_id_zero(target->callee_function) &&
            target->callable_type == XR_SEMANTIC_INDEX_NONE &&
            ((plan->operations[operation].opcode == XI_CALL_METHOD &&
              (plan->operations[operation].semantic_immediate & 1) == 0) ||
             plan->operations[operation].opcode == XI_CALL);
        bool indirect = indirect_type != XR_SEMANTIC_INDEX_NONE &&
                        direct_function == XR_SEMANTIC_INDEX_NONE && !native_yieldable &&
                        !source_namespace && target->kind == XR_SEM_CALL_TARGET_INDIRECT_CALLABLE &&
                        target->function == XR_SEMANTIC_INDEX_NONE &&
                        target->dependency == XR_SEMANTIC_INDEX_NONE &&
                        target->source_export == XR_SEMANTIC_INDEX_NONE &&
                        stable_id_zero(target->export_identity) &&
                        stable_id_zero(target->callee_function) &&
                        target->callable_type == indirect_type;
        bool native_namespace_shape =
            native_namespace && !source_namespace && direct_function == XR_SEMANTIC_INDEX_NONE &&
            !native_yieldable && indirect_type == XR_SEMANTIC_INDEX_NONE &&
            target->kind == XR_SEM_CALL_TARGET_NATIVE_NAMESPACE_YIELDABLE &&
            target->function == XR_SEMANTIC_INDEX_NONE &&
            target->dependency == XR_SEMANTIC_INDEX_NONE &&
            target->source_export == XR_SEMANTIC_INDEX_NONE &&
            stable_id_zero(target->export_identity) && stable_id_zero(target->callee_function) &&
            target->callable_type == XR_SEMANTIC_INDEX_NONE;
        bool builtin_instance_shape =
            builtin_instance && !source_namespace && !native_namespace &&
            direct_function == XR_SEMANTIC_INDEX_NONE && !native_yieldable &&
            indirect_type == XR_SEMANTIC_INDEX_NONE &&
            target->kind == XR_SEM_CALL_TARGET_BUILTIN_INSTANCE_YIELDABLE &&
            target->function == XR_SEMANTIC_INDEX_NONE &&
            target->dependency == XR_SEMANTIC_INDEX_NONE &&
            target->source_export == XR_SEMANTIC_INDEX_NONE &&
            stable_id_zero(target->export_identity) && stable_id_zero(target->callee_function) &&
            target->callable_type == builtin_instance_type;
        bool source_instance_shape =
            source_instance_function != XR_SEMANTIC_INDEX_NONE && !source_namespace &&
            !native_namespace && !builtin_instance && direct_function == XR_SEMANTIC_INDEX_NONE &&
            !native_yieldable && indirect_type == XR_SEMANTIC_INDEX_NONE &&
            source_instance_class < plan->source_class_count &&
            target->kind == xr_semantic_source_instance_method_call_kind(
                                plan->source_classes[source_instance_class].flags) &&
            target->function == source_instance_function &&
            target->dependency == XR_SEMANTIC_INDEX_NONE &&
            target->source_export == XR_SEMANTIC_INDEX_NONE &&
            stable_id_zero(target->export_identity) &&
            xr_stable_id_equal(target->callee_function,
                               plan->functions[source_instance_function].id) &&
            target->callable_type == source_instance_type;
        bool open_source_instance_shape =
            !program_bound && target->kind == XR_SEM_CALL_TARGET_SOURCE_INSTANCE_METHOD_OPEN &&
            source_call->opcode == XI_CALL_METHOD && (source_call->semantic_immediate & 1) == 0 &&
            source_call->metadata_count == 1 && source_call->operand_count > 0 &&
            target->function == XR_SEMANTIC_INDEX_NONE &&
            target->dependency < plan->dependency_count &&
            target->source_export == XR_SEMANTIC_INDEX_NONE &&
            !stable_id_zero(target->export_identity) && stable_id_zero(target->callee_function) &&
            target->callable_type < plan->type_count &&
            target->callable_type == plan->operands[source_call->operand_begin].type &&
            plan->operands[source_call->operand_begin].role == XR_SEM_OPERAND_RECEIVER &&
            plan->types[target->callable_type].source_class == XR_SEMANTIC_INDEX_NONE &&
            !stable_id_zero(plan->types[target->callable_type].source_class_identity);
        bool class_construction_shape =
            !program_bound && class_construction != XR_SEMANTIC_INDEX_NONE && !source_namespace &&
            !native_namespace && !builtin_instance && direct_function == XR_SEMANTIC_INDEX_NONE &&
            !native_yieldable && indirect_type == XR_SEMANTIC_INDEX_NONE &&
            source_instance_function == XR_SEMANTIC_INDEX_NONE &&
            target->kind == XR_SEM_CALL_TARGET_SOURCE_CLASS_CONSTRUCTOR &&
            target->function == XR_SEMANTIC_INDEX_NONE &&
            target->dependency == XR_SEMANTIC_INDEX_NONE &&
            target->source_export == XR_SEMANTIC_INDEX_NONE &&
            stable_id_zero(target->export_identity) && stable_id_zero(target->callee_function) &&
            target->callable_type == source_call->result_type;
        const XrSemanticTypeRecord *imported_result_type =
            xr_semantic_plan_type(plan, source_call->result_type);
        bool imported_class_construction_shape =
            !program_bound && source_namespace && source_call->opcode == XI_CALL &&
            source_selector && source_selector[0] != '\0' &&
            target->kind == XR_SEM_CALL_TARGET_SOURCE_CLASS_CONSTRUCTOR &&
            target->function == XR_SEMANTIC_INDEX_NONE &&
            target->dependency < plan->dependency_count &&
            strcmp(plan->dependencies[target->dependency].module_path, source_module) == 0 &&
            target->source_export != XR_SEMANTIC_INDEX_NONE &&
            !stable_id_zero(target->export_identity) &&
            target->callable_type == source_call->result_type &&
            xr_semantic_external_class_instance_type_is_exact(imported_result_type);
        if (direct &&
            !verify_direct_local_managed_aggregate_boundaries(
                plan, source_call, &plan->functions[target->function], error, error_size)) {
            xr_free(stores.rows);
            return false;
        }
        if ((!direct && !native && !source_shape && !indirect && !native_namespace_shape &&
             !builtin_instance_shape && !source_instance_shape && !open_source_instance_shape &&
             !class_construction_shape && !imported_class_construction_shape) ||
            target->reserved[0] != 0 || target->reserved[1] != 0 || target->reserved[2] != 0) {
            xr_free(stores.rows);
            return report(error, error_size, "XR_SEM_0019",
                          "call-target authority disagrees with frozen invocation facts");
        }
        char operation_id[XR_STABLE_ID_BYTES * 2 + 1];
        char function_id[XR_STABLE_ID_BYTES * 2 + 1];
        char expected_key[320];
        xr_stable_id_hex(plan->operations[operation].id, operation_id);
        int length;
        if (direct) {
            xr_stable_id_hex(plan->functions[target->function].id, function_id);
            length = snprintf(expected_key, sizeof(expected_key),
                              "call-target-v3:schema=%u:operation=%s:function=%s:kind=%u",
                              XR_SEMANTIC_SCHEMA_VERSION, operation_id, function_id,
                              (unsigned) target->kind);
        } else if (native) {
            length = snprintf(expected_key, sizeof(expected_key),
                              "call-target-v3:schema=%u:operation=%s:native=%s.%s:kind=%u",
                              XR_SEMANTIC_SCHEMA_VERSION, operation_id, native_module,
                              native_member, (unsigned) target->kind);
        } else if (source_shape) {
            char dependency_id[XR_STABLE_ID_BYTES * 2 + 1];
            char export_id[XR_STABLE_ID_BYTES * 2 + 1];
            char callee_id[XR_STABLE_ID_BYTES * 2 + 1];
            xr_stable_id_hex(plan->dependencies[target->dependency].id, dependency_id);
            xr_stable_id_hex(target->export_identity, export_id);
            xr_stable_id_hex(target->callee_function, callee_id);
            length = snprintf(expected_key, sizeof(expected_key),
                              "call-target-v4:schema=%u:operation=%s:dependency=%s:"
                              "export=%s:function=%s:kind=%u",
                              XR_SEMANTIC_SCHEMA_VERSION, operation_id, dependency_id, export_id,
                              callee_id, (unsigned) target->kind);
        } else if (indirect) {
            char type_id[XR_STABLE_ID_BYTES * 2 + 1];
            xr_stable_id_hex(plan->types[target->callable_type].id, type_id);
            length = snprintf(expected_key, sizeof(expected_key),
                              "call-target-v3:schema=%u:operation=%s:callable-type=%s:kind=%u",
                              XR_SEMANTIC_SCHEMA_VERSION, operation_id, type_id,
                              (unsigned) target->kind);
        } else if (native_namespace_shape) {
            length =
                snprintf(expected_key, sizeof(expected_key),
                         "call-target-v5:schema=%u:operation=%s:native-namespace=%s.%s:kind=%u",
                         XR_SEMANTIC_SCHEMA_VERSION, operation_id, native_namespace_module,
                         native_namespace_selector, (unsigned) target->kind);
        } else if (builtin_instance_shape) {
            char type_id[XR_STABLE_ID_BYTES * 2 + 1];
            xr_stable_id_hex(plan->types[target->callable_type].id, type_id);
            const char *selector = plan->metadata[plan->operations[operation].metadata_begin];
            length = snprintf(expected_key, sizeof(expected_key),
                              "call-target-v6:schema=%u:operation=%s:builtin-instance=%s.%s:"
                              "type=%s:kind=%u",
                              XR_SEMANTIC_SCHEMA_VERSION, operation_id, builtin_instance, selector,
                              type_id, (unsigned) target->kind);
        } else if (imported_class_construction_shape) {
            char dependency_id[XR_STABLE_ID_BYTES * 2 + 1];
            char export_id[XR_STABLE_ID_BYTES * 2 + 1];
            char class_id[XR_STABLE_ID_BYTES * 2 + 1];
            char constructor_id[XR_STABLE_ID_BYTES * 2 + 1];
            char type_id[XR_STABLE_ID_BYTES * 2 + 1];
            xr_stable_id_hex(plan->dependencies[target->dependency].id, dependency_id);
            xr_stable_id_hex(target->export_identity, export_id);
            xr_stable_id_hex(imported_result_type->source_class_identity, class_id);
            xr_stable_id_hex(target->callee_function, constructor_id);
            xr_stable_id_hex(plan->types[target->callable_type].id, type_id);
            length =
                stable_id_zero(target->callee_function)
                    ? snprintf(expected_key, sizeof(expected_key),
                               "call-target-v10:schema=%u:operation=%s:dependency=%s:"
                               "class-export=%s:source-class=%s:constructor=none:type=%s:"
                               "kind=%u",
                               XR_SEMANTIC_SCHEMA_VERSION, operation_id, dependency_id, export_id,
                               class_id, type_id, (unsigned) target->kind)
                    : snprintf(expected_key, sizeof(expected_key),
                               "call-target-v10:schema=%u:operation=%s:dependency=%s:"
                               "class-export=%s:source-class=%s:constructor=%s:type=%s:"
                               "kind=%u",
                               XR_SEMANTIC_SCHEMA_VERSION, operation_id, dependency_id, export_id,
                               class_id, constructor_id, type_id, (unsigned) target->kind);
        } else if (class_construction_shape) {
            char class_id[XR_STABLE_ID_BYTES * 2 + 1];
            char type_id[XR_STABLE_ID_BYTES * 2 + 1];
            xr_stable_id_hex(plan->source_classes[class_construction].id, class_id);
            xr_stable_id_hex(plan->types[target->callable_type].id, type_id);
            length = snprintf(expected_key, sizeof(expected_key),
                              "call-target-v9:schema=%u:operation=%s:source-class=%s:type=%s:"
                              "kind=%u",
                              XR_SEMANTIC_SCHEMA_VERSION, operation_id, class_id, type_id,
                              (unsigned) target->kind);
        } else if (source_instance_shape) {
            const char *selector = plan->metadata[plan->operations[operation].metadata_begin];
            char class_id[XR_STABLE_ID_BYTES * 2 + 1];
            char callee_id[XR_STABLE_ID_BYTES * 2 + 1];
            char type_id[XR_STABLE_ID_BYTES * 2 + 1];
            xr_stable_id_hex(plan->source_classes[source_instance_class].id, class_id);
            xr_stable_id_hex(target->callee_function, callee_id);
            xr_stable_id_hex(plan->types[target->callable_type].id, type_id);
            length = snprintf(expected_key, sizeof(expected_key),
                              "call-target-v7:schema=%u:operation=%s:source-class=%s:selector=%zu:%"
                              "s:function=%s:type=%s:kind=%u",
                              XR_SEMANTIC_SCHEMA_VERSION, operation_id, class_id, strlen(selector),
                              selector, callee_id, type_id, (unsigned) target->kind);
        } else {
            char dependency_id[XR_STABLE_ID_BYTES * 2 + 1];
            char class_id[XR_STABLE_ID_BYTES * 2 + 1];
            char method_id[XR_STABLE_ID_BYTES * 2 + 1];
            char type_id[XR_STABLE_ID_BYTES * 2 + 1];
            xr_stable_id_hex(plan->dependencies[target->dependency].id, dependency_id);
            xr_stable_id_hex(plan->types[target->callable_type].source_class_identity, class_id);
            xr_stable_id_hex(target->export_identity, method_id);
            xr_stable_id_hex(plan->types[target->callable_type].id, type_id);
            length = snprintf(expected_key, sizeof(expected_key),
                              "call-target-v8:schema=%u:operation=%s:dependency=%s:source-class=%s:"
                              "source-method=%s:type=%s:kind=%u",
                              XR_SEMANTIC_SCHEMA_VERSION, operation_id, dependency_id, class_id,
                              method_id, type_id, (unsigned) target->kind);
        }
        if (length < 0 || (size_t) length >= sizeof(expected_key) ||
            strcmp(target->canonical_key ? target->canonical_key : "", expected_key) != 0 ||
            !verify_id(target->canonical_key, target->id)) {
            xr_free(stores.rows);
            return report(error, error_size, "XR_SEM_0019",
                          "call-target stable identity is not canonical");
        }
        cursor++;
        if (program_bound)
            program_cursor++;
    }
    xr_free(stores.rows);
    return (cursor == plan->call_target_count &&
            program_cursor == plan->program_call_binding_count) ||
           report(error, error_size, "XR_SEM_0019", "unprovable call-target authority is present");
}

static bool fingerprint_zero(XrFingerprint fingerprint) {
    XrFingerprint zero = {{0}};
    return xr_fingerprint_equal(fingerprint, zero);
}

static const XrSemanticProgramFunctionBinding *
program_function_binding_for_semantic(const XrSemanticPlan *plan, uint32_t semantic_function) {
    uint32_t lower = 0;
    uint32_t upper = plan ? plan->program_function_binding_count : 0;
    while (lower < upper) {
        uint32_t middle = lower + (upper - lower) / 2;
        const XrSemanticProgramFunctionBinding *binding = &plan->program_function_bindings[middle];
        if (binding->semantic_function < semantic_function)
            lower = middle + 1;
        else
            upper = middle;
    }
    return lower < (plan ? plan->program_function_binding_count : 0) &&
                   plan->program_function_bindings[lower].semantic_function == semantic_function
               ? &plan->program_function_bindings[lower]
               : NULL;
}

static XrStableId expected_program_generation(uint32_t program_schema, XrFingerprint fingerprint) {
    static const uint8_t domain[] = "xray-generation-closure-id-v1\0";
    XrSHA256Context context;
    uint8_t schema[4];
    uint8_t digest[XR_FINGERPRINT_BYTES];
    XrStableId result;
    for (uint32_t i = 0; i < sizeof(schema); i++)
        schema[i] = (uint8_t) (program_schema >> (i * 8u));
    xr_sha256_init(&context);
    xr_sha256_update(&context, domain, sizeof(domain) - 1u);
    xr_sha256_update(&context, schema, sizeof(schema));
    xr_sha256_update(&context, fingerprint.bytes, sizeof(fingerprint.bytes));
    xr_sha256_final(&context, digest);
    memcpy(result.bytes, digest, sizeof(result.bytes));
    return result;
}

static const XrSemanticProgramTypeBinding *program_type_binding_for_row(const XrSemanticPlan *plan,
                                                                        uint32_t program_row) {
    const XrSemanticProgramTypeBinding *match = NULL;
    for (uint32_t i = 0; plan && i < plan->program_type_binding_count; i++) {
        const XrSemanticProgramTypeBinding *candidate = &plan->program_type_bindings[i];
        if (candidate->program_row != program_row)
            continue;
        if (match)
            return NULL;
        match = candidate;
    }
    return match;
}

static bool verify_program_provenance_layout(const XrSemanticPlan *plan, char *error,
                                             size_t error_size) {
    XrSemanticProgramProvenance zero = {0};
    bool present = plan->program_provenance.schema != 0 || plan->program_type_bindings ||
                   plan->program_type_binding_count != 0 || plan->program_type_field_bindings ||
                   plan->program_type_field_binding_count != 0 || plan->program_function_bindings ||
                   plan->program_function_binding_count != 0 ||
                   plan->program_dependency_bindings ||
                   plan->program_dependency_binding_count != 0 || plan->program_call_bindings ||
                   plan->program_call_binding_count != 0;
    if (!present)
        return memcmp(&plan->program_provenance, &zero, sizeof(zero)) == 0;
    const XrSemanticProgramProvenance *provenance = &plan->program_provenance;
    if (plan->program_type_binding_count > XR_PROGRAM_SEMANTIC_CLOSURE_MAX_TYPES ||
        plan->program_type_field_binding_count > XR_PROGRAM_SEMANTIC_CLOSURE_MAX_TYPE_FIELDS ||
        plan->program_function_binding_count > XR_PROGRAM_SEMANTIC_CLOSURE_MAX_FUNCTIONS ||
        plan->program_dependency_binding_count > XR_PROGRAM_SEMANTIC_CLOSURE_MAX_DEPENDENCIES ||
        plan->program_call_binding_count > XR_PROGRAM_SEMANTIC_CLOSURE_MAX_CALLS ||
        plan->program_type_field_binding_count > plan->type_child_count ||
        (plan->type_count && !plan->types) || (plan->type_child_count && !plan->type_children) ||
        (plan->function_count && !plan->functions) || (plan->operation_count && !plan->operations))
        return report(error, error_size, "XR_EXEC_5003", "program provenance exceeds hard budgets");
    if (provenance->schema != XR_SEMANTIC_PROGRAM_PROVENANCE_SCHEMA_VERSION ||
        provenance->program_schema != XR_PROGRAM_SEMANTIC_CLOSURE_SCHEMA_VERSION ||
        (provenance->program_family != XR_PROGRAM_SEMANTIC_FAMILY_SCALAR_DIRECT_CALL &&
         provenance->program_family !=
             XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_AGGREGATE_DIRECT_CALL &&
         provenance->program_family !=
             XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_PRODUCT_DIRECT_CALL &&
         provenance->program_family !=
             XR_PROGRAM_SEMANTIC_FAMILY_SCALAR_MODULE_GRAPH_DIRECT_CALL) ||
        fingerprint_zero(provenance->program_fingerprint) ||
        stable_id_zero(provenance->generation_identity) ||
        stable_id_zero(provenance->program_module) ||
        !xr_stable_id_equal(provenance->generation_identity,
                            expected_program_generation(provenance->program_schema,
                                                        provenance->program_fingerprint)) ||
        provenance->type_count != plan->program_type_binding_count ||
        provenance->type_field_count != plan->program_type_field_binding_count ||
        provenance->program_module_row >= provenance->module_count ||
        provenance->program_dependency_binding_count !=
            plan->program_dependency_binding_count ||
        plan->program_function_binding_count > provenance->function_count ||
        plan->program_call_binding_count > provenance->call_count ||
        provenance->type_count > plan->type_count || provenance->reserved != 0 ||
        (provenance->type_count && !plan->program_type_bindings) ||
        (provenance->type_field_count && !plan->program_type_field_bindings) ||
        (plan->program_function_binding_count && !plan->program_function_bindings) ||
        (provenance->program_dependency_binding_count &&
         !plan->program_dependency_bindings) ||
        (plan->program_call_binding_count && !plan->program_call_bindings))
        return report(error, error_size, "XR_SEM_0019",
                      "program provenance row layout is incomplete");
    if ((provenance->program_family == XR_PROGRAM_SEMANTIC_FAMILY_SCALAR_DIRECT_CALL &&
         (provenance->type_count != 0 || provenance->type_field_count != 0 ||
          provenance->function_count != 2 || provenance->call_count != 1 ||
          provenance->module_count != 1 || provenance->dependency_count != 0 ||
          provenance->program_module_row != 0 ||
          plan->program_function_binding_count != 2 || plan->program_call_binding_count != 1 ||
          plan->program_dependency_binding_count != 0)) ||
        (provenance->program_family ==
             XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_AGGREGATE_DIRECT_CALL &&
         (provenance->type_count < 2 || provenance->type_field_count == 0 ||
          provenance->function_count != 2 || provenance->call_count != 1 ||
          provenance->module_count != 1 || provenance->dependency_count != 0 ||
          provenance->program_module_row != 0 ||
          plan->program_function_binding_count != 2 || plan->program_call_binding_count != 1 ||
          plan->program_dependency_binding_count != 0)) ||
        (provenance->program_family ==
             XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_PRODUCT_DIRECT_CALL &&
         (provenance->type_count != 3 || provenance->type_field_count != 6 ||
          provenance->function_count != 3 || provenance->call_count != 2 ||
          provenance->module_count != 1 || provenance->dependency_count != 0 ||
          provenance->program_module_row != 0 ||
          plan->program_function_binding_count != 3 || plan->program_call_binding_count != 2 ||
          plan->program_dependency_binding_count != 0)) ||
        (provenance->program_family ==
             XR_PROGRAM_SEMANTIC_FAMILY_SCALAR_MODULE_GRAPH_DIRECT_CALL &&
         (provenance->type_count != 1 || provenance->type_field_count != 0 ||
          provenance->function_count != 2 || provenance->call_count != 1 ||
          provenance->module_count != 2 || provenance->dependency_count != 1 ||
          plan->program_function_binding_count != 1 ||
          plan->program_call_binding_count > 1 ||
          plan->program_dependency_binding_count != plan->program_call_binding_count ||
          plan->program_dependency_binding_count != plan->dependency_count)))
        return report(error, error_size, "XR_SEM_0019",
                      "program provenance family shape is invalid");
    uint8_t *seen_type_rows =
        provenance->type_count ? (uint8_t *) xr_calloc(provenance->type_count, 1) : NULL;
    uint8_t *seen_type_field_rows = provenance->type_field_count
                                        ? (uint8_t *) xr_calloc(provenance->type_field_count, 1)
                                        : NULL;
    uint8_t *seen_function_rows =
        provenance->function_count ? (uint8_t *) xr_calloc(provenance->function_count, 1) : NULL;
    uint8_t *seen_dependency_rows =
        provenance->dependency_count ? (uint8_t *) xr_calloc(provenance->dependency_count, 1)
                                     : NULL;
    uint8_t *seen_call_rows =
        provenance->call_count ? (uint8_t *) xr_calloc(provenance->call_count, 1) : NULL;
    if ((provenance->type_count && !seen_type_rows) ||
        (provenance->type_field_count && !seen_type_field_rows) ||
        (provenance->function_count && !seen_function_rows) ||
        (provenance->dependency_count && !seen_dependency_rows) ||
        (provenance->call_count && !seen_call_rows)) {
        xr_free(seen_type_rows);
        xr_free(seen_type_field_rows);
        xr_free(seen_function_rows);
        xr_free(seen_dependency_rows);
        xr_free(seen_call_rows);
        return report(error, error_size, "XR_EXEC_5003",
                      "program provenance verification allocation failed");
    }
    bool valid = true;
    uint32_t aggregate_count = 0;
    uint32_t product_count = 0;
    for (uint32_t i = 0; valid && i < provenance->type_count; i++) {
        const XrSemanticProgramTypeBinding *binding = &plan->program_type_bindings[i];
        const XrSemanticTypeRecord *semantic =
            binding->semantic_type < plan->type_count ? &plan->types[binding->semantic_type] : NULL;
        const uint8_t required =
            XR_PROGRAM_SEMANTIC_TYPE_NONNULLABLE | XR_PROGRAM_SEMANTIC_TYPE_NONGENERIC |
            XR_PROGRAM_SEMANTIC_TYPE_VALUE | XR_PROGRAM_SEMANTIC_TYPE_POINTER_FREE;
        bool scalar = binding->kind == XR_PROGRAM_SEMANTIC_TYPE_EXACT_SCALAR;
        bool aggregate = binding->kind == XR_PROGRAM_SEMANTIC_TYPE_LEAF_VALUE_AGGREGATE;
        bool product = binding->kind == XR_PROGRAM_SEMANTIC_TYPE_LEAF_VALUE_PRODUCT;
        const XrExactScalarDesc *scalar_desc =
            scalar ? xr_exact_scalar_by_id((XrExactScalarId) binding->exact_scalar) : NULL;
        valid =
            semantic && binding->program_row < provenance->type_count &&
            !seen_type_rows[binding->program_row] && !stable_id_zero(binding->program_type) &&
            binding->reserved == 0 && binding->flags == required &&
            binding->field_begin <= provenance->type_field_count &&
            binding->field_count <= provenance->type_field_count - binding->field_begin &&
            (i == 0 || plan->program_type_bindings[i - 1].semantic_type < binding->semantic_type) &&
            ((scalar && scalar_desc && binding->field_count == 0 &&
              semantic->kind == (scalar_desc->family == XR_EXACT_SCALAR_FAMILY_INTEGER
                                     ? XR_KIND_INT
                                     : XR_KIND_FLOAT) &&
              semantic->scalar_rep == scalar_desc->native_type &&
              semantic->builtin_type == XR_TID_NULL &&
              stable_id_zero(binding->source_class_identity) &&
              semantic->source_class == XR_SEMANTIC_INDEX_NONE &&
              stable_id_zero(semantic->source_class_identity) &&
              stable_id_zero(semantic->source_enum_identity) && !semantic->source_enum_key &&
              semantic->child_count == 0 && semantic->aggregate_extent == 0 &&
              semantic->aggregate_align == 0 && semantic->enum_layout_id == 0 &&
              semantic->enum_member_count == 0 && semantic->enum_flags == 0 &&
              semantic->reserved_enum == 0 && semantic->flags == 0) ||
             (aggregate && binding->exact_scalar == XR_EXACT_SCALAR_NONE &&
              binding->field_count != 0 && semantic->kind == XR_KIND_INSTANCE &&
              semantic->builtin_type == XR_TID_NULL &&
              semantic->source_class < plan->source_class_count &&
              !stable_id_zero(binding->source_class_identity) &&
              xr_stable_id_equal(binding->source_class_identity, semantic->source_class_identity) &&
              xr_stable_id_equal(binding->source_class_identity,
                                 plan->source_classes[semantic->source_class].id) &&
              stable_id_zero(semantic->source_enum_identity) && !semantic->source_enum_key &&
              semantic->child_count == binding->field_count &&
              semantic->aggregate_extent == binding->field_count &&
              semantic->aggregate_align == 0 && semantic->enum_layout_id == 0 &&
              semantic->enum_member_count == 0 && semantic->scalar_rep == XR_SCALAR_REP_NONE &&
              semantic->enum_flags == 0 && semantic->reserved_enum == 0 &&
              semantic->flags == (XR_SEM_TYPE_VALUE | XR_SEM_TYPE_AGGREGATE_EXACT)) ||
             (product && binding->exact_scalar == XR_EXACT_SCALAR_NONE &&
              binding->field_count == 6 && semantic->kind == XR_KIND_TUPLE &&
              semantic->builtin_type == XR_TID_NULL &&
              stable_id_zero(binding->source_class_identity) &&
              semantic->source_class == XR_SEMANTIC_INDEX_NONE &&
              stable_id_zero(semantic->source_class_identity) &&
              stable_id_zero(semantic->source_enum_identity) && !semantic->source_enum_key &&
              semantic->child_count == 6 && semantic->aggregate_extent == 6 &&
              semantic->aggregate_align == 0 && semantic->enum_layout_id == 0 &&
              semantic->enum_member_count == 0 && semantic->scalar_rep == XR_SCALAR_REP_NONE &&
              semantic->enum_flags == 0 && semantic->reserved_enum == 0 &&
              semantic->flags == XR_SEM_TYPE_VALUE));
        if (valid) {
            seen_type_rows[binding->program_row] = 1u;
            aggregate_count += aggregate ? 1u : 0u;
            product_count += product ? 1u : 0u;
        }
    }
    for (uint32_t i = 0; valid && i < provenance->type_count; i++) {
        const XrSemanticProgramTypeBinding *owner = program_type_binding_for_row(plan, i);
        if (!owner)
            valid = false;
        for (uint32_t ordinal = 0; valid && owner && ordinal < owner->field_count; ordinal++) {
            uint32_t field_index = owner->field_begin + ordinal;
            const XrSemanticProgramTypeFieldBinding *field =
                field_index < provenance->type_field_count
                    ? &plan->program_type_field_bindings[field_index]
                    : NULL;
            const XrSemanticProgramTypeBinding *child =
                field ? program_type_binding_for_row(plan, field->field_program_row) : NULL;
            valid = field && child && !seen_type_field_rows[field_index] &&
                    field->owner_program_row == owner->program_row &&
                    field->declaration_ordinal == ordinal &&
                    xr_stable_id_equal(field->program_owner_type, owner->program_type) &&
                    xr_stable_id_equal(field->program_field_type, child->program_type) &&
                    field->semantic_field_type == child->semantic_type &&
                    child->kind == XR_PROGRAM_SEMANTIC_TYPE_EXACT_SCALAR &&
                    owner->semantic_type < plan->type_count &&
                    plan->types[owner->semantic_type].child_begin <= plan->type_child_count &&
                    plan->types[owner->semantic_type].child_count <=
                        plan->type_child_count - plan->types[owner->semantic_type].child_begin &&
                    ordinal < plan->types[owner->semantic_type].child_count &&
                    plan->type_children[plan->types[owner->semantic_type].child_begin + ordinal] ==
                        child->semantic_type &&
                    (owner->kind != XR_PROGRAM_SEMANTIC_TYPE_LEAF_VALUE_PRODUCT ||
                     child->exact_scalar ==
                         (ordinal == 2 ? XR_EXACT_SCALAR_U8 : XR_EXACT_SCALAR_I64));
            if (valid)
                seen_type_field_rows[field_index] = 1u;
            if (valid)
                seen_type_rows[field->field_program_row] = 2u;
        }
    }
    if (provenance->program_family == XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_AGGREGATE_DIRECT_CALL) {
        valid = valid && aggregate_count == 1u;
        for (uint32_t i = 0; valid && i < provenance->type_count; i++) {
            const XrSemanticProgramTypeBinding *binding = program_type_binding_for_row(plan, i);
            valid = binding && (binding->kind == XR_PROGRAM_SEMANTIC_TYPE_EXACT_SCALAR
                                    ? seen_type_rows[i] == 2u
                                    : seen_type_rows[i] == 1u);
        }
        for (uint32_t i = 0; valid && i < provenance->type_field_count; i++)
            valid = seen_type_field_rows[i] != 0;
    }
    if (provenance->program_family == XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_PRODUCT_DIRECT_CALL) {
        valid = valid && aggregate_count == 0 && product_count == 1u;
        for (uint32_t i = 0; valid && i < provenance->type_count; i++) {
            const XrSemanticProgramTypeBinding *binding = program_type_binding_for_row(plan, i);
            valid = binding && (binding->kind == XR_PROGRAM_SEMANTIC_TYPE_EXACT_SCALAR
                                    ? seen_type_rows[i] == 2u
                                    : binding->kind == XR_PROGRAM_SEMANTIC_TYPE_LEAF_VALUE_PRODUCT &&
                                          seen_type_rows[i] == 1u);
        }
        for (uint32_t i = 0; valid && i < provenance->type_field_count; i++)
            valid = seen_type_field_rows[i] != 0;
    }
    for (uint32_t i = 0; valid && i < plan->program_function_binding_count; i++) {
        const XrSemanticProgramFunctionBinding *binding = &plan->program_function_bindings[i];
        valid = binding->semantic_function < plan->function_count &&
                binding->program_row < provenance->function_count &&
                !seen_function_rows[binding->program_row] &&
                !stable_id_zero(binding->program_function) &&
                (binding->flags & ~(XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY |
                                    XR_PROGRAM_SEMANTIC_FUNCTION_EXPORTED)) == 0 &&
                memcmp(binding->reserved, (uint8_t[3]) {0}, sizeof(binding->reserved)) == 0 &&
                (i == 0 || plan->program_function_bindings[i - 1].semantic_function <
                               binding->semantic_function);
        if (valid)
            seen_function_rows[binding->program_row] = 1;
    }
    if (valid && provenance->program_family ==
                     XR_PROGRAM_SEMANTIC_FAMILY_SCALAR_MODULE_GRAPH_DIRECT_CALL) {
        const XrSemanticProgramFunctionBinding *local = &plan->program_function_bindings[0];
        valid = plan->program_call_binding_count == 0
                    ? local->flags == XR_PROGRAM_SEMANTIC_FUNCTION_EXPORTED
                    : local->flags == XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY;
    }
    if (valid &&
        provenance->program_family == XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_AGGREGATE_DIRECT_CALL) {
        uint32_t entry_count = 0;
        uint32_t callee_count = 0;
        for (uint32_t i = 0; i < provenance->function_count; i++) {
            const XrSemanticProgramFunctionBinding *binding = &plan->program_function_bindings[i];
            entry_count +=
                (binding->flags & XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY) != 0 ? 1u : 0u;
            callee_count += binding->flags == 0 ? 1u : 0u;
        }
        valid = entry_count == 1u && callee_count == 1u;
    }
    if (valid &&
        provenance->program_family == XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_PRODUCT_DIRECT_CALL) {
        uint32_t entry_count = 0;
        uint32_t callee_count = 0;
        for (uint32_t i = 0; i < provenance->function_count; i++) {
            const XrSemanticProgramFunctionBinding *binding = &plan->program_function_bindings[i];
            entry_count += binding->flags == XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY ? 1u : 0u;
            callee_count += binding->flags == 0 ? 1u : 0u;
        }
        valid = entry_count == 2u && callee_count == 1u;
    }
    for (uint32_t i = 0; valid && i < plan->program_dependency_binding_count; i++) {
        const XrSemanticProgramDependencyBinding *binding =
            &plan->program_dependency_bindings[i];
        valid = !stable_id_zero(binding->resolver_binding) &&
                binding->program_row < provenance->dependency_count &&
                !seen_dependency_rows[binding->program_row] &&
                binding->semantic_dependency < plan->dependency_count &&
                binding->reserved == 0;
        if (valid)
            seen_dependency_rows[binding->program_row] = 1;
    }
    for (uint32_t i = 0; valid && i < plan->program_call_binding_count; i++) {
        const XrSemanticProgramCallBinding *binding = &plan->program_call_bindings[i];
        const XrSemanticOperationRecord *operation = binding->operation < plan->operation_count
                                                         ? &plan->operations[binding->operation]
                                                         : NULL;
        const XrSemanticProgramFunctionBinding *caller =
            operation ? program_function_binding_for_semantic(plan, operation->function) : NULL;
        bool external = binding->program_dependency != XR_SEMANTIC_INDEX_NONE;
        const XrSemanticProgramFunctionBinding *callee =
            external ? NULL
                     : program_function_binding_for_semantic(plan, binding->target_function);
        const XrSemanticProgramDependencyBinding *dependency =
            external && binding->program_dependency < plan->program_dependency_binding_count
                ? &plan->program_dependency_bindings[binding->program_dependency]
                : NULL;
        const XrSemanticCallTargetRecord *source_target = NULL;
        for (uint32_t t = 0; external && t < plan->call_target_count; t++) {
            const XrSemanticCallTargetRecord *candidate = &plan->call_targets[t];
            if (candidate->operation != binding->operation ||
                candidate->kind != XR_SEM_CALL_TARGET_SOURCE_EXPORT)
                continue;
            if (source_target) {
                source_target = NULL;
                break;
            }
            source_target = candidate;
        }
        valid = operation && operation->opcode == XI_CALL &&
                binding->program_row < provenance->call_count &&
                !seen_call_rows[binding->program_row] &&
                binding->reserved == 0 &&
                !stable_id_zero(binding->program_call) && !stable_id_zero(binding->callsite) &&
                caller &&
                xr_stable_id_equal(caller->program_function, binding->caller_program_function) &&
                ((!external && callee && stable_id_zero(binding->resolver_binding) &&
                  binding->program_dependency == XR_SEMANTIC_INDEX_NONE &&
                  binding->target_function < plan->function_count &&
                  xr_stable_id_equal(callee->program_function,
                                     binding->callee_program_function)) ||
                 (external && dependency && source_target &&
                  binding->target_function == XR_SEMANTIC_INDEX_NONE &&
                  !stable_id_zero(binding->resolver_binding) &&
                  xr_stable_id_equal(binding->resolver_binding,
                                     dependency->resolver_binding) &&
                  source_target->dependency == dependency->semantic_dependency)) &&
                (provenance->program_family !=
                     XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_AGGREGATE_DIRECT_CALL ||
                 ((caller->flags & XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY) != 0 &&
                  callee->flags == 0)) &&
                (provenance->program_family !=
                     XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_PRODUCT_DIRECT_CALL ||
                 (caller->flags == XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY && callee->flags == 0)) &&
                (i == 0 || plan->program_call_bindings[i - 1].operation < binding->operation);
        if (valid)
            seen_call_rows[binding->program_row] = 1;
    }
    xr_free(seen_type_rows);
    xr_free(seen_type_field_rows);
    xr_free(seen_function_rows);
    xr_free(seen_dependency_rows);
    xr_free(seen_call_rows);
    return valid ||
           report(error, error_size, "XR_SEM_0019", "program provenance binding is invalid");
}

static bool operation_is_static_suspend(const XrSemanticOperationRecord *operation) {
    return operation &&
           ((operation->effects & XI_EFFECT_MAY_SUSPEND) != 0 || operation->opcode == XI_GO);
}

static bool operation_propagates_suspend(const XrSemanticOperationRecord *operation) {
    return operation && (operation->opcode == XI_CALL || operation->opcode == XI_TAIL_CALL ||
                         operation->opcode == XI_CALL_METHOD);
}

typedef struct XrCoroutineAuthorityWork {
    uint8_t *state_counts;
    uint8_t *suspendable;
    uint8_t *dependency_unknown;
    uint32_t *target_by_operation;
    uint32_t *reverse_head;
    uint32_t *reverse_next;
    uint32_t *queue;
} XrCoroutineAuthorityWork;

static void coroutine_authority_work_dispose(XrCoroutineAuthorityWork *work) {
    xr_free(work->state_counts);
    xr_free(work->suspendable);
    xr_free(work->dependency_unknown);
    xr_free(work->target_by_operation);
    xr_free(work->reverse_head);
    xr_free(work->reverse_next);
    xr_free(work->queue);
    memset(work, 0, sizeof(*work));
}

static bool verify_coroutine_authority(const XrSemanticPlan *plan, char *error, size_t error_size,
                                       uint32_t *survey_rows) {
    XrCoroutineAuthorityWork work = {0};
    work.state_counts = (uint8_t *) xr_calloc(plan->operation_count, sizeof(*work.state_counts));
    work.suspendable = (uint8_t *) xr_calloc(plan->function_count, sizeof(*work.suspendable));
    work.dependency_unknown =
        (uint8_t *) xr_calloc(plan->function_count, sizeof(*work.dependency_unknown));
    work.target_by_operation = plan->operation_count
                                   ? (uint32_t *) xr_malloc((size_t) plan->operation_count *
                                                            sizeof(*work.target_by_operation))
                                   : NULL;
    work.reverse_head =
        plan->function_count
            ? (uint32_t *) xr_malloc((size_t) plan->function_count * sizeof(*work.reverse_head))
            : NULL;
    work.reverse_next =
        plan->call_target_count
            ? (uint32_t *) xr_malloc((size_t) plan->call_target_count * sizeof(*work.reverse_next))
            : NULL;
    work.queue = plan->function_count
                     ? (uint32_t *) xr_malloc((size_t) plan->function_count * sizeof(*work.queue))
                     : NULL;
    if ((plan->operation_count && (!work.state_counts || !work.target_by_operation)) ||
        (plan->function_count &&
         (!work.suspendable || !work.dependency_unknown || !work.reverse_head || !work.queue)) ||
        (plan->call_target_count && !work.reverse_next)) {
        coroutine_authority_work_dispose(&work);
        return report(error, error_size, "XR_EXEC_5003",
                      "coroutine authority verifier budget exhausted");
    }
    for (uint32_t operation = 0; operation < plan->operation_count; operation++)
        work.target_by_operation[operation] = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t function = 0; function < plan->function_count; function++)
        work.reverse_head[function] = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t target_index = 0; target_index < plan->call_target_count; target_index++) {
        const XrSemanticCallTargetRecord *target = &plan->call_targets[target_index];
        if (target->operation >= plan->operation_count ||
            work.target_by_operation[target->operation] != XR_SEMANTIC_INDEX_NONE) {
            coroutine_authority_work_dispose(&work);
            return report(error, error_size, "XR_SEM_0019",
                          "call-target operation relation is not one-to-one");
        }
        work.target_by_operation[target->operation] = target_index;
        work.reverse_next[target_index] = XR_SEMANTIC_INDEX_NONE;
        if ((target->kind == XR_SEM_CALL_TARGET_DIRECT_LOCAL ||
             target->kind == XR_SEM_CALL_TARGET_SOURCE_INSTANCE_METHOD_LOCAL) &&
            operation_propagates_suspend(&plan->operations[target->operation])) {
            if (target->function >= plan->function_count) {
                coroutine_authority_work_dispose(&work);
                return report(error, error_size, "XR_SEM_0019",
                              "direct call-target function is out of range");
            }
            work.reverse_next[target_index] = work.reverse_head[target->function];
            work.reverse_head[target->function] = target_index;
        } else if (target->kind == XR_SEM_CALL_TARGET_NATIVE_YIELDABLE) {
            const XrSemanticOperationRecord *operation = &plan->operations[target->operation];
            if (target->function != XR_SEMANTIC_INDEX_NONE || operation->opcode != XI_CALL ||
                operation->function >= plan->function_count) {
                coroutine_authority_work_dispose(&work);
                return report(error, error_size, "XR_SEM_0019",
                              "native yieldable call-target shape is invalid");
            }
            work.suspendable[operation->function] = 1;
        } else if (target->kind == XR_SEM_CALL_TARGET_SOURCE_EXPORT) {
            const XrSemanticOperationRecord *operation = &plan->operations[target->operation];
            if (target->function != XR_SEMANTIC_INDEX_NONE ||
                target->dependency >= plan->dependency_count ||
                (operation->opcode != XI_CALL_METHOD && operation->opcode != XI_CALL) ||
                operation->function >= plan->function_count) {
                coroutine_authority_work_dispose(&work);
                return report(error, error_size, "XR_SEM_0019",
                              "source-export call-target shape is invalid");
            }
            /* Whether an external source export suspends is not a fact this
             * standalone plan owns. The ordered module-set verifier below
             * completes that fact from the dependency's frozen plan. This
             * uncertainty also propagates through exact local callers: a
             * standalone plan cannot reject the caller state that the module
             * set may later prove necessary. */
            work.dependency_unknown[operation->function] = 1;
        } else if (target->kind == XR_SEM_CALL_TARGET_INDIRECT_CALLABLE) {
            const XrSemanticOperationRecord *operation = &plan->operations[target->operation];
            if (target->function != XR_SEMANTIC_INDEX_NONE ||
                target->callable_type >= plan->type_count ||
                plan->types[target->callable_type].kind != XR_KIND_FUNCTION ||
                operation->opcode != XI_CALL || operation->function >= plan->function_count) {
                coroutine_authority_work_dispose(&work);
                return report(error, error_size, "XR_SEM_0019",
                              "indirect callable call-target shape is invalid");
            }
            work.suspendable[operation->function] = 1;
        } else if (target->kind == XR_SEM_CALL_TARGET_NATIVE_NAMESPACE_YIELDABLE) {
            const XrSemanticOperationRecord *operation = &plan->operations[target->operation];
            if (target->function != XR_SEMANTIC_INDEX_NONE || operation->opcode != XI_CALL_METHOD ||
                operation->function >= plan->function_count) {
                coroutine_authority_work_dispose(&work);
                return report(error, error_size, "XR_SEM_0019",
                              "native namespace call-target shape is invalid");
            }
            work.suspendable[operation->function] = 1;
        } else if (target->kind == XR_SEM_CALL_TARGET_BUILTIN_INSTANCE_YIELDABLE) {
            const XrSemanticOperationRecord *operation = &plan->operations[target->operation];
            if (target->function != XR_SEMANTIC_INDEX_NONE ||
                target->callable_type >= plan->type_count ||
                xr_semantic_yieldable_builtin_id((unsigned) plan->types[target->callable_type].kind,
                                                 plan->types[target->callable_type].builtin_type) ==
                    XR_TID_NULL ||
                operation->opcode != XI_CALL_METHOD ||
                operation->function >= plan->function_count) {
                coroutine_authority_work_dispose(&work);
                return report(error, error_size, "XR_SEM_0019",
                              "builtin instance call-target shape is invalid");
            }
            work.suspendable[operation->function] = 1;
        } else if (target->kind == XR_SEM_CALL_TARGET_SOURCE_INSTANCE_METHOD_OPEN) {
            const XrSemanticOperationRecord *operation = &plan->operations[target->operation];
            if (target->function != XR_SEMANTIC_INDEX_NONE ||
                target->dependency >= plan->dependency_count ||
                target->callable_type >= plan->type_count || operation->opcode != XI_CALL_METHOD ||
                operation->function >= plan->function_count) {
                coroutine_authority_work_dispose(&work);
                return report(error, error_size, "XR_SEM_0019",
                              "open source method call-target shape is invalid");
            }
            work.suspendable[operation->function] = 1;
        }
    }
    for (uint32_t entity_index = 0; entity_index < plan->entity_count; entity_index++) {
        const XrSemanticEntityRecord *entity = &plan->entities[entity_index];
        if (entity->kind != XR_SEM_ENTITY_COROUTINE_STATE)
            continue;
        if (entity->subject >= plan->operation_count || ++work.state_counts[entity->subject] != 1) {
            coroutine_authority_work_dispose(&work);
            return report(error, error_size, "XR_SEM_0019",
                          "coroutine state operation relation is not one-to-one");
        }
    }
    for (uint32_t operation = 0; operation < plan->operation_count; operation++) {
        uint32_t function = plan->operations[operation].function;
        if (operation_is_static_suspend(&plan->operations[operation]))
            work.suspendable[function] = 1;
    }
    uint32_t queue_begin = 0;
    uint32_t queue_end = 0;
    for (uint32_t function = 0; function < plan->function_count; function++)
        if (work.suspendable[function])
            work.queue[queue_end++] = function;
    while (queue_begin < queue_end) {
        uint32_t callee = work.queue[queue_begin++];
        for (uint32_t target_index = work.reverse_head[callee];
             target_index != XR_SEMANTIC_INDEX_NONE;
             target_index = work.reverse_next[target_index]) {
            uint32_t operation = plan->call_targets[target_index].operation;
            uint32_t caller = plan->operations[operation].function;
            if (work.suspendable[caller])
                continue;
            work.suspendable[caller] = 1;
            work.queue[queue_end++] = caller;
        }
    }

    queue_begin = 0;
    queue_end = 0;
    for (uint32_t function = 0; function < plan->function_count; function++)
        if (work.dependency_unknown[function])
            work.queue[queue_end++] = function;
    while (queue_begin < queue_end) {
        uint32_t callee = work.queue[queue_begin++];
        for (uint32_t target_index = work.reverse_head[callee];
             target_index != XR_SEMANTIC_INDEX_NONE;
             target_index = work.reverse_next[target_index]) {
            uint32_t operation = plan->call_targets[target_index].operation;
            uint32_t caller = plan->operations[operation].function;
            if (work.dependency_unknown[caller])
                continue;
            work.dependency_unknown[caller] = 1;
            work.queue[queue_end++] = caller;
        }
    }

    const bool survey = semantic_survey_enabled();
    uint32_t refused = 0;
    for (uint32_t operation = 0; operation < plan->operation_count; operation++) {
        uint32_t target_index = work.target_by_operation[operation];
        bool dynamic_suspend = false;
        if (target_index != XR_SEMANTIC_INDEX_NONE) {
            const XrSemanticCallTargetRecord *target = &plan->call_targets[target_index];
            dynamic_suspend =
                ((plan->operations[operation].opcode == XI_CALL ||
                  plan->operations[operation].opcode == XI_CALL_METHOD) &&
                 ((target->kind == XR_SEM_CALL_TARGET_NATIVE_YIELDABLE) ||
                  ((target->kind == XR_SEM_CALL_TARGET_DIRECT_LOCAL ||
                    target->kind == XR_SEM_CALL_TARGET_SOURCE_INSTANCE_METHOD_LOCAL) &&
                   work.suspendable[target->function] != 0))) ||
                (plan->operations[operation].opcode == XI_CALL_METHOD &&
                 (target->kind == XR_SEM_CALL_TARGET_NATIVE_NAMESPACE_YIELDABLE ||
                  target->kind == XR_SEM_CALL_TARGET_BUILTIN_INSTANCE_YIELDABLE ||
                  target->kind == XR_SEM_CALL_TARGET_SOURCE_INSTANCE_METHOD_OPEN)) ||
                (plan->operations[operation].opcode == XI_CALL &&
                 target->kind == XR_SEM_CALL_TARGET_INDIRECT_CALLABLE);
        }
        bool expected =
            operation_is_static_suspend(&plan->operations[operation]) || dynamic_suspend;
        /* SOURCE_EXPORT suspension and exact local calls transitively rooted
         * in it are deliberately incomplete in a standalone plan. Both a
         * state and no state are valid only while no independent local/static
         * fact already proves suspension. The ordered module-set verifier
         * below closes the dependency fact before a TargetPlan can consume
         * either answer. */
        bool dependency_deferred = false;
        if (target_index != XR_SEMANTIC_INDEX_NONE) {
            const XrSemanticCallTargetRecord *target = &plan->call_targets[target_index];
            dependency_deferred =
                target->kind == XR_SEM_CALL_TARGET_SOURCE_EXPORT ||
                ((target->kind == XR_SEM_CALL_TARGET_DIRECT_LOCAL ||
                  target->kind == XR_SEM_CALL_TARGET_SOURCE_INSTANCE_METHOD_LOCAL) &&
                 target->function < plan->function_count &&
                 work.dependency_unknown[target->function] != 0 &&
                 work.suspendable[target->function] == 0);
        }
        if (dependency_deferred)
            continue;
        if (work.state_counts[operation] != (uint8_t) expected) {
            char detail[256];
            const XrSemanticOperationRecord *record = &plan->operations[operation];
            const char *selector =
                record->metadata_count != 0 && record->metadata_begin < plan->metadata_count
                    ? plan->metadata[record->metadata_begin]
                    : "";
            snprintf(detail, sizeof(detail),
                     "coroutine state count disagrees with grounded call authority "
                     "function=%u operation=%u opcode=%u selector=%s expected=%u actual=%u",
                     record->function, operation, record->opcode, selector, expected ? 1u : 0u,
                     work.state_counts[operation]);
            /* Each operation carries its own state-count judgement over a plan
             * this loop only reads, so surveying the rest costs nothing but the
             * walk and answers how many call sites a module gets wrong rather
             * than only the lowest-numbered one. */
            if (refused++ == 0)
                report(error, error_size, "XR_SEM_0019", detail);
            if (!survey) {
                coroutine_authority_work_dispose(&work);
                return false;
            }
            semantic_survey_row("coroutine_state_count", detail);
            if (survey_rows)
                (*survey_rows)++;
        }
    }
    coroutine_authority_work_dispose(&work);
    return refused == 0;
}

static bool verify_parameter_and_capture_definitions(const XrSemanticPlan *plan,
                                                     const uint32_t *definitions,
                                                     uint32_t value_count, char *error,
                                                     size_t error_size) {
    for (uint32_t i = 0; i < plan->parameter_count; i++) {
        const XrSemanticParameterRecord *parameter = &plan->parameters[i];
        if (parameter->value >= value_count ||
            definitions[parameter->value] == XR_SEMANTIC_INDEX_NONE) {
            return report(error, error_size, "XR_SEM_0013", "parameter has no SSA definition");
        }
        uint32_t definition_index = definitions[parameter->value];
        if (definition_index >= plan->operation_count)
            continue;
        const XrSemanticOperationRecord *definition = &plan->operations[definition_index];
        if (definition->function != parameter->function || definition->opcode != XI_PARAM ||
            definition->result_type != parameter->type ||
            definition->semantic_immediate != parameter->ordinal ||
            definition->parameter_mode != parameter->mode ||
            definition->parameter_ownership != parameter->ownership ||
            definition->transfer_mode != parameter->transfer_mode) {
            return report(error, error_size, "XR_SEM_0013",
                          "parameter disagrees with its SSA definition");
        }
    }
    for (uint32_t i = 0; i < plan->capture_count; i++) {
        const XrSemanticCaptureRecord *capture = &plan->captures[i];
        if (capture->source != XR_SEM_CAPTURE_LOCAL_VALUE)
            continue;
        if (capture->source_value >= value_count ||
            definitions[capture->source_value] == XR_SEMANTIC_INDEX_NONE) {
            return report(error, error_size, "XR_SEM_0018",
                          "capture disagrees with its typed SSA source");
        }
        uint32_t definition_index = definitions[capture->source_value];
        uint32_t source_function;
        uint32_t source_type;
        if (definition_index < plan->operation_count) {
            source_function = plan->operations[definition_index].function;
            source_type = plan->operations[definition_index].result_type;
        } else {
            uint32_t parameter_index = definition_index - plan->operation_count;
            if (parameter_index >= plan->parameter_count) {
                return report(error, error_size, "XR_SEM_0018",
                              "capture parameter source is invalid");
            }
            source_function = plan->parameters[parameter_index].function;
            source_type = plan->parameters[parameter_index].type;
        }
        if (source_function != capture->source_function || source_type != capture->source_type) {
            return report(error, error_size, "XR_SEM_0018",
                          "capture disagrees with its typed SSA source");
        }
    }
    return true;
}

static bool verify_operation_uses(const XrSemanticPlan *plan, const XrSemanticGraph *graph,
                                  const uint32_t *definitions, uint32_t value_count, char *error,
                                  size_t error_size) {
    for (uint32_t i = 0; i < plan->operation_count; i++) {
        const XrSemanticOperationRecord *operation = &plan->operations[i];
        for (uint16_t operand = 0; operand < operation->operand_count; operand++) {
            uint32_t cursor = operation->operand_begin + operand;
            const XrSemanticOperandRecord *record = &plan->operands[cursor];
            if (record->value >= value_count ||
                definitions[record->value] == XR_SEMANTIC_INDEX_NONE ||
                record->type >= plan->type_count) {
                return report(error, error_size, "XR_SEM_0015",
                              "operand has no matching typed SSA definition");
            }
            uint32_t definition_index = definitions[record->value];
            uint32_t definition_type =
                definition_index < plan->operation_count
                    ? plan->operations[definition_index].result_type
                    : plan->parameters[definition_index - plan->operation_count].type;
            if (definition_type != record->type) {
                return report(error, error_size, "XR_SEM_0015",
                              "operand has no matching typed SSA definition");
            }
            if (!verify_operand_contract(operation, record, operand, error, error_size))
                return false;
            if (!verify_ssa_use(plan, graph, definitions, i, operand, error, error_size))
                return false;
        }
    }
    return true;
}

static bool verify_block_controls(const XrSemanticPlan *plan, const XrSemanticGraph *graph,
                                  const uint32_t *definitions, uint32_t value_count, char *error,
                                  size_t error_size) {
    for (uint32_t b = 0; b < plan->block_count; b++) {
        const XrSemanticBlockRecord *block = &plan->blocks[b];
        if (block->control_value == XR_SEMANTIC_INDEX_NONE)
            continue;
        if (block->control_value >= value_count ||
            definitions[block->control_value] == XR_SEMANTIC_INDEX_NONE) {
            return report(error, error_size, "XR_SEM_0016",
                          "block control value has no SSA definition");
        }
        uint32_t definition_index = definitions[block->control_value];
        const XrSemanticOperationRecord *definition =
            definition_index < plan->operation_count ? &plan->operations[definition_index] : NULL;
        uint32_t definition_function =
            definition ? definition->function
                       : plan->parameters[definition_index - plan->operation_count].function;
        if (definition_function != block->function ||
            (definition && xr_semantic_graph_is_reachable(graph, b) &&
             !xr_semantic_graph_dominates(graph, definition->block, b))) {
            return report(error, error_size, "XR_SEM_0016",
                          "block control definition does not dominate the terminator");
        }
    }
    return true;
}

static bool verify_operations(const XrSemanticPlan *plan, const uint8_t *edge_mask,
                              const XrSemanticGraph *graph, char *error, size_t error_size) {
    uint32_t *definitions = NULL;
    uint32_t value_count = 0;
    if (!build_definition_map(plan, &definitions, &value_count, error, error_size))
        return false;
    bool valid =
        verify_operation_records(plan, edge_mask, definitions, value_count, error, error_size) &&
        verify_parameter_and_capture_definitions(plan, definitions, value_count, error,
                                                 error_size) &&
        verify_operation_uses(plan, graph, definitions, value_count, error, error_size) &&
        verify_block_controls(plan, graph, definitions, value_count, error, error_size) &&
        verify_source_export_rows(plan, definitions, value_count, error, error_size) &&
        verify_call_targets(plan, definitions, graph, value_count, error, error_size);
    xr_free(definitions);
    return valid;
}

static bool verify_constants(const XrSemanticPlan *plan, char *error, size_t error_size) {
    for (uint32_t i = 0; i < plan->constant_count; i++) {
        const XrSemanticConstantRecord *constant = &plan->constants[i];
        bool has_text = constant->string != NULL;
        if (constant->type >= plan->type_count || constant->kind <= XR_SEM_CONST_NONE ||
            constant->kind > XR_SEM_CONST_ENUM_NAMESPACE)
            return report(error, error_size, "XR_SEM_0009",
                          "constant kind or type is not exactly supported");
        if ((constant->kind == XR_SEM_CONST_STRING || constant->kind == XR_SEM_CONST_BIGINT ||
             constant->kind == XR_SEM_CONST_ENUM_NAMESPACE) != has_text)
            return report(error, error_size, "XR_SEM_0009",
                          "constant string payload does not match its kind");
        if (constant->kind == XR_SEM_CONST_BIGINT) {
            /* The digits are the whole value, so they are checked here rather
             * than trusted: an optional sign and at least one decimal digit,
             * nothing else. */
            if (!xr_bigint_literal_is_wellformed_core(constant->string))
                return report(error, error_size, "XR_SEM_0009",
                              "BigInt constant is not a well-formed integer literal");
        }
        if (constant->kind == XR_SEM_CONST_BOOL && constant->integer != 0 && constant->integer != 1)
            return report(error, error_size, "XR_SEM_0009",
                          "boolean constant is not canonically encoded");
        if (constant->kind == XR_SEM_CONST_RUNE &&
            (constant->integer < 0 || constant->integer > 0x10FFFF ||
             (constant->integer >= 0xD800 && constant->integer <= 0xDFFF)))
            return report(error, error_size, "XR_SEM_0009",
                          "rune constant is not a Unicode scalar value");
    }
    return true;
}

static bool compute_plan_suspendable_functions(const XrSemanticPlan *plan,
                                               uint8_t **suspendable_out, char *error,
                                               size_t error_size) {
    uint8_t *suspendable =
        plan->function_count ? (uint8_t *) xr_calloc(plan->function_count, 1) : NULL;
    uint32_t *head = plan->function_count
                         ? (uint32_t *) xr_malloc((size_t) plan->function_count * sizeof(*head))
                         : NULL;
    uint32_t *next = plan->call_target_count
                         ? (uint32_t *) xr_malloc((size_t) plan->call_target_count * sizeof(*next))
                         : NULL;
    uint32_t *queue = plan->function_count
                          ? (uint32_t *) xr_malloc((size_t) plan->function_count * sizeof(*queue))
                          : NULL;
    if ((plan->function_count && (!suspendable || !head || !queue)) ||
        (plan->call_target_count && !next)) {
        xr_free(suspendable);
        xr_free(head);
        xr_free(next);
        xr_free(queue);
        return report(error, error_size, "XR_EXEC_5003",
                      "source dependency suspendability budget exhausted");
    }
    for (uint32_t f = 0; f < plan->function_count; f++)
        head[f] = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < plan->call_target_count; i++)
        next[i] = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < plan->entity_count; i++) {
        const XrSemanticEntityRecord *entity = &plan->entities[i];
        if (entity->kind == XR_SEM_ENTITY_COROUTINE_STATE &&
            entity->subject < plan->operation_count)
            suspendable[plan->operations[entity->subject].function] = 1;
    }
    for (uint32_t i = 0; i < plan->call_target_count; i++) {
        const XrSemanticCallTargetRecord *target = &plan->call_targets[i];
        if ((target->kind == XR_SEM_CALL_TARGET_DIRECT_LOCAL ||
             target->kind == XR_SEM_CALL_TARGET_SOURCE_INSTANCE_METHOD_LOCAL) &&
            target->function < plan->function_count && target->operation < plan->operation_count &&
            operation_propagates_suspend(&plan->operations[target->operation])) {
            next[i] = head[target->function];
            head[target->function] = i;
        }
    }
    uint32_t begin = 0, end = 0;
    for (uint32_t f = 0; f < plan->function_count; f++)
        if (suspendable[f])
            queue[end++] = f;
    while (begin < end) {
        uint32_t callee = queue[begin++];
        for (uint32_t edge = head[callee]; edge != XR_SEMANTIC_INDEX_NONE; edge = next[edge]) {
            uint32_t caller = plan->operations[plan->call_targets[edge].operation].function;
            if (!suspendable[caller]) {
                suspendable[caller] = 1;
                queue[end++] = caller;
            }
        }
    }
    xr_free(head);
    xr_free(next);
    xr_free(queue);
    *suspendable_out = suspendable;
    return true;
}

static uint8_t semantic_operation_coroutine_state_count(const XrSemanticPlan *plan,
                                                        uint32_t operation) {
    uint8_t count = 0;
    for (uint32_t i = 0; plan && i < plan->entity_count; i++) {
        const XrSemanticEntityRecord *entity = &plan->entities[i];
        if (entity->kind == XR_SEM_ENTITY_COROUTINE_STATE &&
            entity->subject_kind == XR_SEM_ENTITY_SUBJECT_OPERATION &&
            entity->subject == operation && count != UINT8_MAX)
            count++;
    }
    return count;
}

static bool verify_module_set_coroutine_authority(const XrSemanticPlan *plan,
                                                  const XrSemanticPlan *const *dependencies,
                                                  uint32_t dependency_count,
                                                  uint8_t *const *dependency_suspendable,
                                                  char *error, size_t error_size) {
    XrCoroutineAuthorityWork work = {0};
    work.state_counts = (uint8_t *) xr_calloc(plan->operation_count, sizeof(*work.state_counts));
    work.suspendable = (uint8_t *) xr_calloc(plan->function_count, sizeof(*work.suspendable));
    work.target_by_operation = plan->operation_count
                                   ? (uint32_t *) xr_malloc((size_t) plan->operation_count *
                                                            sizeof(*work.target_by_operation))
                                   : NULL;
    work.reverse_head =
        plan->function_count
            ? (uint32_t *) xr_malloc((size_t) plan->function_count * sizeof(*work.reverse_head))
            : NULL;
    work.reverse_next =
        plan->call_target_count
            ? (uint32_t *) xr_malloc((size_t) plan->call_target_count * sizeof(*work.reverse_next))
            : NULL;
    work.queue = plan->function_count
                     ? (uint32_t *) xr_malloc((size_t) plan->function_count * sizeof(*work.queue))
                     : NULL;
    if ((plan->operation_count && (!work.state_counts || !work.target_by_operation)) ||
        (plan->function_count && (!work.suspendable || !work.reverse_head || !work.queue)) ||
        (plan->call_target_count && !work.reverse_next)) {
        coroutine_authority_work_dispose(&work);
        return report(error, error_size, "XR_EXEC_5003",
                      "module-set coroutine verifier budget exhausted");
    }
    for (uint32_t operation = 0; operation < plan->operation_count; operation++)
        work.target_by_operation[operation] = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t function = 0; function < plan->function_count; function++)
        work.reverse_head[function] = XR_SEMANTIC_INDEX_NONE;

    for (uint32_t target_index = 0; target_index < plan->call_target_count; target_index++) {
        const XrSemanticCallTargetRecord *target = &plan->call_targets[target_index];
        if (target->operation >= plan->operation_count ||
            work.target_by_operation[target->operation] != XR_SEMANTIC_INDEX_NONE) {
            coroutine_authority_work_dispose(&work);
            return report(error, error_size, "XR_SEM_0019",
                          "module-set call-target operation relation is not one-to-one");
        }
        work.target_by_operation[target->operation] = target_index;
        work.reverse_next[target_index] = XR_SEMANTIC_INDEX_NONE;
        const XrSemanticOperationRecord *operation = &plan->operations[target->operation];
        if ((target->kind == XR_SEM_CALL_TARGET_DIRECT_LOCAL ||
             target->kind == XR_SEM_CALL_TARGET_SOURCE_INSTANCE_METHOD_LOCAL) &&
            operation_propagates_suspend(operation)) {
            if (target->function >= plan->function_count) {
                coroutine_authority_work_dispose(&work);
                return report(error, error_size, "XR_SEM_0019",
                              "module-set direct call target is out of range");
            }
            work.reverse_next[target_index] = work.reverse_head[target->function];
            work.reverse_head[target->function] = target_index;
            continue;
        }
        bool directly_suspendable = target->kind == XR_SEM_CALL_TARGET_NATIVE_YIELDABLE ||
                                    target->kind == XR_SEM_CALL_TARGET_INDIRECT_CALLABLE ||
                                    target->kind == XR_SEM_CALL_TARGET_NATIVE_NAMESPACE_YIELDABLE ||
                                    target->kind == XR_SEM_CALL_TARGET_BUILTIN_INSTANCE_YIELDABLE ||
                                    target->kind == XR_SEM_CALL_TARGET_SOURCE_INSTANCE_METHOD_OPEN;
        if (target->kind == XR_SEM_CALL_TARGET_SOURCE_EXPORT) {
            const XrSemanticPlan *dependency =
                target->dependency < dependency_count ? dependencies[target->dependency] : NULL;
            const XrSemanticSourceExportRecord *source_export =
                dependency && target->source_export < dependency->source_export_count
                    ? &dependency->source_exports[target->source_export]
                    : NULL;
            if (!source_export || !dependency_suspendable ||
                !dependency_suspendable[target->dependency] ||
                source_export->kind != XR_SEM_SOURCE_EXPORT_FUNCTION ||
                source_export->function >= dependency->function_count) {
                coroutine_authority_work_dispose(&work);
                return report(error, error_size, "XR_SEM_0019",
                              "module-set source export suspendability is unavailable");
            }
            directly_suspendable =
                dependency_suspendable[target->dependency][source_export->function] != 0;
        }
        if (directly_suspendable) {
            if (operation->function >= plan->function_count) {
                coroutine_authority_work_dispose(&work);
                return report(error, error_size, "XR_SEM_0019",
                              "module-set suspendable caller is out of range");
            }
            work.suspendable[operation->function] = 1;
        }
    }

    for (uint32_t entity_index = 0; entity_index < plan->entity_count; entity_index++) {
        const XrSemanticEntityRecord *entity = &plan->entities[entity_index];
        if (entity->kind == XR_SEM_ENTITY_COROUTINE_STATE)
            work.state_counts[entity->subject]++;
    }
    for (uint32_t operation = 0; operation < plan->operation_count; operation++) {
        uint32_t function = plan->operations[operation].function;
        if (operation_is_static_suspend(&plan->operations[operation]) &&
            function < plan->function_count)
            work.suspendable[function] = 1;
    }
    uint32_t queue_begin = 0;
    uint32_t queue_end = 0;
    for (uint32_t function = 0; function < plan->function_count; function++)
        if (work.suspendable[function])
            work.queue[queue_end++] = function;
    while (queue_begin < queue_end) {
        uint32_t callee = work.queue[queue_begin++];
        for (uint32_t target_index = work.reverse_head[callee];
             target_index != XR_SEMANTIC_INDEX_NONE;
             target_index = work.reverse_next[target_index]) {
            uint32_t operation = plan->call_targets[target_index].operation;
            uint32_t caller = plan->operations[operation].function;
            if (work.suspendable[caller])
                continue;
            work.suspendable[caller] = 1;
            work.queue[queue_end++] = caller;
        }
    }

    for (uint32_t operation_index = 0; operation_index < plan->operation_count; operation_index++) {
        const XrSemanticOperationRecord *operation = &plan->operations[operation_index];
        uint32_t target_index = work.target_by_operation[operation_index];
        bool dynamic_suspend = false;
        if (target_index != XR_SEMANTIC_INDEX_NONE) {
            const XrSemanticCallTargetRecord *target = &plan->call_targets[target_index];
            if (target->kind == XR_SEM_CALL_TARGET_SOURCE_EXPORT) {
                const XrSemanticPlan *dependency = dependencies[target->dependency];
                const XrSemanticSourceExportRecord *source_export =
                    &dependency->source_exports[target->source_export];
                dynamic_suspend =
                    dependency_suspendable[target->dependency][source_export->function] != 0;
            } else {
                dynamic_suspend =
                    ((operation->opcode == XI_CALL || operation->opcode == XI_CALL_METHOD) &&
                     (target->kind == XR_SEM_CALL_TARGET_NATIVE_YIELDABLE ||
                      ((target->kind == XR_SEM_CALL_TARGET_DIRECT_LOCAL ||
                        target->kind == XR_SEM_CALL_TARGET_SOURCE_INSTANCE_METHOD_LOCAL) &&
                       work.suspendable[target->function] != 0))) ||
                    (operation->opcode == XI_CALL_METHOD &&
                     (target->kind == XR_SEM_CALL_TARGET_NATIVE_NAMESPACE_YIELDABLE ||
                      target->kind == XR_SEM_CALL_TARGET_BUILTIN_INSTANCE_YIELDABLE ||
                      target->kind == XR_SEM_CALL_TARGET_SOURCE_INSTANCE_METHOD_OPEN)) ||
                    (operation->opcode == XI_CALL &&
                     target->kind == XR_SEM_CALL_TARGET_INDIRECT_CALLABLE);
            }
        }
        bool expected = (target_index == XR_SEMANTIC_INDEX_NONE ||
                         plan->call_targets[target_index].kind != XR_SEM_CALL_TARGET_SOURCE_EXPORT)
                            ? operation_is_static_suspend(operation) || dynamic_suspend
                            : dynamic_suspend;
        if (work.state_counts[operation_index] != (uint8_t) expected) {
            char detail[256];
            snprintf(detail, sizeof(detail),
                     "module-set coroutine state disagrees with frozen dependency authority "
                     "function=%u operation=%u opcode=%u expected=%u actual=%u",
                     operation->function, operation_index, operation->opcode, expected ? 1u : 0u,
                     work.state_counts[operation_index]);
            coroutine_authority_work_dispose(&work);
            return report(error, error_size, "XR_SEM_0019", detail);
        }
    }
    coroutine_authority_work_dispose(&work);
    return true;
}

/* Keep module-set verification on the canonical cross-module admission rule. */
static bool parameter_type_admits_argument(const XrSemanticPlan *callee,
                                           const XrSemanticTypeRecord *parameter_type,
                                           const XrSemanticTypeRecord *operand_type) {
    return xr_semantic_parameter_type_admits_argument(callee, parameter_type, operand_type);
}

static bool verify_dependency_rows(const XrSemanticPlan *plan, char *error, size_t error_size) {
    for (uint32_t i = 0; i < plan->dependency_count; i++) {
        const XrSemanticDependencyRecord *record = &plan->dependencies[i];
        if (!record->module_path || !record->module_path[0] || stable_id_zero(record->module))
            return report(error, error_size, "XR_SEM_0019", "source dependency row is incomplete");
        char expected[512];
        char module_id[XR_STABLE_ID_BYTES * 2 + 1];
        char fingerprint[XR_FINGERPRINT_BYTES * 2 + 1];
        xr_stable_id_hex(record->module, module_id);
        for (unsigned b = 0; b < XR_FINGERPRINT_BYTES; b++)
            snprintf(fingerprint + b * 2, sizeof(fingerprint) - b * 2, "%02x",
                     record->semantic_fingerprint.bytes[b]);
        int length = snprintf(expected, sizeof(expected),
                              "dependency-v1:schema=%u:path=%zu:%s:module=%s:semantic=%s",
                              XR_SEMANTIC_SCHEMA_VERSION, strlen(record->module_path),
                              record->module_path, module_id, fingerprint);
        if (length <= 0 || (size_t) length >= sizeof(expected) ||
            strcmp(record->canonical_key ? record->canonical_key : "", expected) != 0 ||
            !verify_id(record->canonical_key, record->id))
            return report(error, error_size, "XR_SEM_0019",
                          "source dependency identity is not canonical");
        for (uint32_t before = 0; before < i; before++)
            if (xr_stable_id_equal(plan->dependencies[before].module, record->module))
                return report(error, error_size, "XR_SEM_0019",
                              "source dependency module is duplicated");
    }
    return true;
}

static bool source_namespace_dependency_row(const XrSemanticPlan *plan,
                                            const XrSemanticOperationRecord *operation,
                                            uint32_t *out_dependency) {
    if (!plan || !operation || !out_dependency || operation->opcode != XI_IMPORT_REF ||
        operation->function != 0 || operation->operand_count != 0 ||
        operation->import_resolution != XR_SEM_IMPORT_RESOLUTION_SOURCE_MODULE ||
        operation->metadata_count != 2 || operation->metadata_begin + 1u >= plan->metadata_count)
        return false;
    const char *module_path = plan->metadata[operation->metadata_begin];
    const char *member = plan->metadata[operation->metadata_begin + 1u];
    if (!module_path || !module_path[0] || !member || member[0] != '\0')
        return false;
    uint32_t match = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < plan->dependency_count; i++) {
        const XrSemanticDependencyRecord *dependency = &plan->dependencies[i];
        if (!dependency->module_path || strcmp(dependency->module_path, module_path) != 0)
            continue;
        if (match != XR_SEMANTIC_INDEX_NONE)
            return false;
        match = i;
    }
    if (match == XR_SEMANTIC_INDEX_NONE)
        return false;
    *out_dependency = match;
    return true;
}

typedef struct XrSemanticAuthorityContext {
    const XrSemanticPlan *plan;
    const XrSemanticGraph *graph;
} XrSemanticAuthorityContext;

/* A check that walks a per-operation loop knows its own refusals apart and
 * reports each of them, so it returns how many rows it printed and the driver
 * below adds none of its own. A check that answers once for the whole plan
 * returns zero and the driver prints the single row. Counting rows rather than
 * refused checks is what makes the census comparable with the two passes
 * downstream, which both count individual gaps. */
static bool semantic_authority_coroutine(const XrSemanticAuthorityContext *ctx, char *error,
                                         size_t error_size, uint32_t *rows) {
    return verify_coroutine_authority(ctx->plan, error, error_size, rows);
}

static bool semantic_authority_dependency_rows(const XrSemanticAuthorityContext *ctx, char *error,
                                               size_t error_size, uint32_t *rows) {
    (void) rows;
    return verify_dependency_rows(ctx->plan, error, error_size);
}

static bool semantic_authority_constants(const XrSemanticAuthorityContext *ctx, char *error,
                                         size_t error_size, uint32_t *rows) {
    (void) rows;
    return verify_constants(ctx->plan, error, error_size);
}

static bool semantic_authority_ownership(const XrSemanticAuthorityContext *ctx, char *error,
                                         size_t error_size, uint32_t *rows) {
    (void) rows;
    return xr_ownership_certificate_check(ctx->plan, ctx->graph, error, error_size);
}

static bool semantic_authority_coroutine_lifecycle(const XrSemanticAuthorityContext *ctx,
                                                   char *error, size_t error_size, uint32_t *rows) {
    (void) rows;
    return verify_coroutine_lifecycle_entities(ctx->plan, ctx->graph, error, error_size);
}

static const struct {
    const char *name;
    bool (*run)(const XrSemanticAuthorityContext *, char *, size_t, uint32_t *);
} k_semantic_authority_checks[] = {
    {"coroutine_authority", semantic_authority_coroutine},
    {"dependency_rows", semantic_authority_dependency_rows},
    {"constants", semantic_authority_constants},
    {"ownership_certificate", semantic_authority_ownership},
    {"coroutine_lifecycle", semantic_authority_coroutine_lifecycle},
};

static bool run_semantic_authority_checks(const XrSemanticAuthorityContext *ctx, char *error,
                                          size_t error_size) {
    bool survey = false;
    uint32_t refused_rows = 0;
    size_t count = sizeof(k_semantic_authority_checks) / sizeof(*k_semantic_authority_checks);
    for (size_t i = 0; i < count; i++) {
        char detail[512] = {0};
        uint32_t rows = 0;
        if (k_semantic_authority_checks[i].run(ctx, detail, sizeof(detail), &rows))
            continue;
        if (refused_rows == 0) {
            survey = semantic_survey_enabled();
            if (error && error_size)
                snprintf(error, error_size, "%s", detail);
        }
        if (!survey)
            return false;
        if (rows == 0) {
            semantic_survey_row(k_semantic_authority_checks[i].name, detail);
            rows = 1;
        }
        refused_rows += rows;
    }
    if (refused_rows) {
        fprintf(stderr, "[refusal-survey] semantic-plan authority gaps: %u\n", refused_rows);
        return false;
    }
    return true;
}

bool xr_semantic_plan_verify(const XrSemanticPlan *plan, char *error, size_t error_size) {
    if (!plan || !plan->frozen || plan->schema != XR_SEMANTIC_SCHEMA_VERSION)
        return report(error, error_size, "XR_SEM_0004",
                      "verifier requires a frozen exact-version SemanticPlan");
    XrFingerprint current_registry, current_stdlib_registry;
    xr_semantic_op_registry_fingerprint(&current_registry);
    xr_stdlib_metadata_registry_fingerprint(&current_stdlib_registry);
    if (!xr_semantic_op_registry_verify(error, error_size) ||
        !xr_fingerprint_equal(plan->operation_registry_fingerprint, current_registry) ||
        !xr_fingerprint_equal(plan->stdlib_registry_fingerprint, current_stdlib_registry))
        return report(error, error_size, "XR_SEM_0017",
                      "SemanticPlan registry authority is missing or incompatible");
    if (plan->type_count > 1000000u || plan->source_class_count > 100000u ||
        plan->source_method_count > 100000u || plan->function_count > 100000u ||
        plan->block_count > 2000000u || plan->operation_count > 10000000u ||
        plan->parameter_count > 25600000u || plan->capture_count > 6400000u ||
        plan->edge_count > 40000000u || plan->operand_count > 40000000u ||
        plan->entity_count > 80000000u || plan->call_target_count > plan->operation_count ||
        plan->dependency_count > plan->function_count + plan->operation_count ||
        plan->source_export_count > plan->function_count + plan->source_class_count)
        return report(error, error_size, "XR_EXEC_5003", "SemanticPlan exceeds hard budgets");
    /* Provenance indexes the primary plan tables. Establish their global
     * budgets before the
     * provenance verifier may dereference selected rows. */
    if (!verify_program_provenance_layout(plan, error, error_size))
        return false;
    uint8_t *block_edge_mask = (uint8_t *) xr_calloc(plan->block_count, sizeof(*block_edge_mask));
    uint8_t *operation_edge_mask =
        (uint8_t *) xr_calloc(plan->operation_count, sizeof(*operation_edge_mask));
    if ((plan->block_count && !block_edge_mask) ||
        (plan->operation_count && !operation_edge_mask)) {
        xr_free(block_edge_mask);
        xr_free(operation_edge_mask);
        return report(error, error_size, "XR_EXEC_5003", "edge verifier budget exhausted");
    }
    XrSemanticGraph graph = {0};
    /* Everything up to and including verify_operations establishes that the
     * tables are dense, in range, and joined by the SSA, operand and
     * call-target relations the judgements after it read on faith. A survey
     * cannot continue past a refusal here: the next judgement would index a
     * plan whose bounds nothing has established. */
    bool verified = xr_semantic_plan_verify_identity_set(plan, error, error_size) &&
                    verify_entities(plan, error, error_size) &&
                    verify_source_classes(plan, error, error_size) &&
                    verify_types(plan, error, error_size) &&
                    verify_functions(plan, error, error_size) &&
                    verify_source_methods(plan, error, error_size) &&
                    verify_edges(plan, block_edge_mask, operation_edge_mask, error, error_size) &&
                    verify_blocks(plan, block_edge_mask, error, error_size) &&
                    xr_semantic_graph_build(plan, &graph, error, error_size) &&
                    verify_operations(plan, operation_edge_mask, &graph, error, error_size);
    if (verified) {
        /* These four ask a structurally sound plan whether it states the facts
         * a backend needs. Each allocates its own working state and reads
         * nothing the others write, so the survey runs all of them and reports
         * every one that refused. */
        const XrSemanticAuthorityContext authority = {plan, &graph};
        verified = run_semantic_authority_checks(&authority, error, error_size);
    }
    xr_semantic_graph_dispose(&graph);
    xr_free(block_edge_mask);
    xr_free(operation_edge_mask);
    if (!verified)
        return false;
    XrFingerprint actual;
    xr_semantic_plan_compute_fingerprint(plan, &actual);
    if (!xr_fingerprint_equal(actual, plan->fingerprint))
        return report(error, error_size, "XR_SEM_0004",
                      "frozen SemanticPlan fingerprint changed after freeze");
    return true;
}

static bool verify_graph_program_module_set(const XrSemanticPlan *plan,
                                            const XrSemanticPlan *const *dependencies,
                                            uint32_t dependency_count, char *error,
                                            size_t error_size) {
    const XrSemanticProgramProvenance *entry =
        plan ? xr_semantic_plan_program_provenance(plan) : NULL;
    if (!entry || entry->program_family !=
                      XR_PROGRAM_SEMANTIC_FAMILY_SCALAR_MODULE_GRAPH_DIRECT_CALL)
        return true;
    const XrSemanticProgramTypeBinding *local_type =
        plan->program_type_binding_count == 1 ? &plan->program_type_bindings[0] : NULL;
    const XrSemanticProgramFunctionBinding *local_function =
        plan->program_function_binding_count == 1 ? &plan->program_function_bindings[0] : NULL;
    const XrSemanticSourceExportRecord *local_export =
        plan->source_export_count == 1 ? &plan->source_exports[0] : NULL;
    bool local_i64 = local_type && local_type->program_row == 0 &&
                     local_type->kind == XR_PROGRAM_SEMANTIC_TYPE_EXACT_SCALAR &&
                     local_type->exact_scalar == XR_EXACT_SCALAR_I64;
    if (dependency_count == 0)
        return local_i64 && local_function &&
                       local_function->flags ==
                           XR_PROGRAM_SEMANTIC_FUNCTION_EXPORTED &&
                       local_export &&
                       local_export->kind == XR_SEM_SOURCE_EXPORT_FUNCTION &&
                       local_export->function == local_function->semantic_function &&
                       plan->program_call_binding_count == 0 &&
                       plan->program_dependency_binding_count == 0
                   ? true
                   : report(error, error_size, "XR_SEM_0019",
                            "graph producer program partition is not exact");
    if (dependency_count != 1 || !dependencies || !dependencies[0])
        return report(error, error_size, "XR_SEM_0019",
                      "graph entry program dependency set is not exact");
    const XrSemanticPlan *producer_plan = dependencies[0];
    const XrSemanticProgramProvenance *producer =
        xr_semantic_plan_program_provenance(producer_plan);
    const XrSemanticProgramFunctionBinding *entry_function =
        plan->program_function_binding_count == 1 ? &plan->program_function_bindings[0] : NULL;
    const XrSemanticProgramFunctionBinding *producer_function =
        producer_plan->program_function_binding_count == 1
            ? &producer_plan->program_function_bindings[0]
            : NULL;
    const XrSemanticProgramDependencyBinding *program_dependency =
        plan->program_dependency_binding_count == 1 ? &plan->program_dependency_bindings[0]
                                                    : NULL;
    const XrSemanticProgramCallBinding *call =
        plan->program_call_binding_count == 1 ? &plan->program_call_bindings[0] : NULL;
    const XrSemanticCallTargetRecord *target = NULL;
    for (uint32_t i = 0; call && i < plan->call_target_count; i++) {
        const XrSemanticCallTargetRecord *candidate = &plan->call_targets[i];
        if (candidate->operation != call->operation ||
            candidate->kind != XR_SEM_CALL_TARGET_SOURCE_EXPORT)
            continue;
        if (target)
            return report(error, error_size, "XR_SEM_0019",
                          "graph entry has ambiguous source-export authority");
        target = candidate;
    }
    const XrSemanticSourceExportRecord *source_export =
        target && target->source_export < producer_plan->source_export_count
            ? &producer_plan->source_exports[target->source_export]
            : NULL;
    const XrSemanticProgramFunctionBinding *exported_program_function =
        source_export && source_export->function < producer_plan->function_count
            ? program_function_binding_for_semantic(producer_plan, source_export->function)
            : NULL;
    const XrSemanticProgramTypeBinding *entry_type =
        program_type_binding_for_row(plan, 0);
    const XrSemanticProgramTypeBinding *producer_type =
        program_type_binding_for_row(producer_plan, 0);
    bool valid =
        local_i64 && entry_type && entry_type->program_row == 0 &&
        entry_type->kind == XR_PROGRAM_SEMANTIC_TYPE_EXACT_SCALAR &&
        entry_type->exact_scalar == XR_EXACT_SCALAR_I64 && producer_type &&
        producer_type->program_row == 0 &&
        producer_type->kind == XR_PROGRAM_SEMANTIC_TYPE_EXACT_SCALAR &&
        producer_type->exact_scalar == XR_EXACT_SCALAR_I64 && source_export &&
        source_export->kind == XR_SEM_SOURCE_EXPORT_FUNCTION && producer_function &&
        source_export->function == producer_function->semantic_function &&
        producer && producer->program_family == entry->program_family &&
        producer->schema == entry->schema && producer->program_schema == entry->program_schema &&
        producer->type_count == entry->type_count &&
        producer->type_field_count == entry->type_field_count &&
        producer->function_count == entry->function_count &&
        producer->call_count == entry->call_count && producer->module_count == entry->module_count &&
        producer->dependency_count == entry->dependency_count &&
        xr_fingerprint_equal(producer->program_fingerprint, entry->program_fingerprint) &&
        xr_stable_id_equal(producer->generation_identity, entry->generation_identity) &&
        producer->program_module_row != entry->program_module_row &&
        !xr_stable_id_equal(producer->program_module, entry->program_module) &&
        producer_function &&
        producer_function->flags == XR_PROGRAM_SEMANTIC_FUNCTION_EXPORTED &&
        producer_plan->program_call_binding_count == 0 &&
        producer_plan->program_dependency_binding_count == 0 &&
        producer_plan->dependency_count == 0 && producer_plan->source_export_count == 1 &&
        entry_function && plan->source_export_count == 0 &&
        entry_function->flags == XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY &&
        !xr_stable_id_equal(entry_function->program_function,
                            producer_function->program_function) &&
        entry_function->program_row != producer_function->program_row && program_dependency &&
        program_dependency->program_row == 0 && program_dependency->semantic_dependency == 0 &&
        call && call->program_row == 0 && call->program_dependency == 0 &&
        call->target_function == XR_SEMANTIC_INDEX_NONE && target && target->dependency == 0 &&
        source_export && source_export->kind == XR_SEM_SOURCE_EXPORT_FUNCTION &&
        exported_program_function == producer_function &&
        xr_stable_id_equal(entry_function->program_function, call->caller_program_function) &&
        xr_stable_id_equal(producer_function->program_function,
                           call->callee_program_function) &&
        xr_stable_id_equal(program_dependency->resolver_binding, call->resolver_binding) &&
        entry_type && producer_type && entry_type->program_row == 0 &&
        producer_type->program_row == 0 &&
        xr_stable_id_equal(entry_type->program_type, producer_type->program_type);
    return valid || report(error, error_size, "XR_SEM_0019",
                           "graph program module-set authority is not exact");
}

bool xr_semantic_plan_verify_module_set(const XrSemanticPlan *plan,
                                        const XrSemanticPlan *const *dependencies,
                                        uint32_t dependency_count, char *error, size_t error_size) {
    if (!xr_semantic_plan_verify(plan, error, error_size))
        return false;
    if (dependency_count != plan->dependency_count || (dependency_count != 0 && !dependencies))
        return report(error, error_size, "XR_SEM_0019",
                      "source dependency vector does not exactly cover the plan");
    if (dependency_count == 0)
        return verify_graph_program_module_set(plan, dependencies, dependency_count, error,
                                               error_size);

    uint8_t *used = (uint8_t *) xr_calloc(dependency_count, sizeof(*used));
    uint8_t **suspendable = (uint8_t **) xr_calloc(dependency_count, sizeof(*suspendable));
    if (!used || !suspendable) {
        xr_free(used);
        xr_free(suspendable);
        return report(error, error_size, "XR_EXEC_5003",
                      "source dependency verifier budget exhausted");
    }
    bool valid = true;
    char authority_detail[512] = {0};
    for (uint32_t row = 0; valid && row < plan->dependency_count; row++) {
        const XrSemanticDependencyRecord *record = &plan->dependencies[row];
        const XrSemanticPlan *match = dependencies[row];
        const XrSemanticEntityRecord *module = verify_plan_module_entity(match);
        if (!match || !xr_semantic_plan_is_verified(match) || !module ||
            !xr_stable_id_equal(module->id, record->module) ||
            !xr_fingerprint_equal(record->semantic_fingerprint,
                                  xr_semantic_plan_fingerprint(match))) {
            valid = false;
            break;
        }
        if (!xr_semantic_plan_verify(match, error, error_size) ||
            !compute_plan_suspendable_functions(match, &suspendable[row], error, error_size)) {
            for (uint32_t i = 0; i < dependency_count; i++)
                xr_free(suspendable[i]);
            xr_free(suspendable);
            xr_free(used);
            return false;
        }
    }
    for (uint32_t operation_index = 0; valid && operation_index < plan->operation_count;
         operation_index++) {
        const XrSemanticOperationRecord *operation = &plan->operations[operation_index];
        if (operation->opcode != XI_IMPORT_REF ||
            operation->import_resolution != XR_SEM_IMPORT_RESOLUTION_SOURCE_MODULE ||
            operation->metadata_count != 2 ||
            operation->metadata_begin + 1u >= plan->metadata_count ||
            plan->metadata[operation->metadata_begin + 1u][0] != '\0')
            continue;
        uint32_t dependency = XR_SEMANTIC_INDEX_NONE;
        if (!source_namespace_dependency_row(plan, operation, &dependency) ||
            dependency >= dependency_count) {
            valid = false;
            break;
        }
        used[dependency] = 1;
    }
    uint32_t *definitions = NULL;
    uint32_t value_count = 0;
    XrFrozenSharedStoreIndex stores = {0};
    if (valid && (!build_definition_map(plan, &definitions, &value_count, error, error_size) ||
                  !frozen_shared_store_index_build(plan, &stores))) {
        xr_free(definitions);
        xr_free(stores.rows);
        for (uint32_t i = 0; i < dependency_count; i++)
            xr_free(suspendable[i]);
        xr_free(suspendable);
        xr_free(used);
        return report(error, error_size, "XR_EXEC_5003",
                      "source call identity verifier allocation failed");
    }
    for (uint32_t operation = 0; valid && operation < plan->operation_count; operation++) {
        uint32_t value = plan->operations[operation].result_value;
        if (value >= value_count) {
            valid = false;
            break;
        }
        definitions[value] = operation;
    }
    /* Each call target is judged against its own frozen dependency and reads
     * nothing the other targets write, so the survey walks them all and reports
     * every call site that disagrees rather than only the first. A refusal that
     * is structural rather than an authority gap -- a dependency or operation
     * index out of range -- still stops the walk, because the reads after it
     * would have no bound. */
    const bool survey = semantic_survey_enabled();
    uint32_t refused_rows = 0;
    for (uint32_t target_index = 0; (valid || survey) && target_index < plan->call_target_count;
         target_index++) {
        const XrSemanticCallTargetRecord *target = &plan->call_targets[target_index];
        bool target_valid = true;
        if (target->kind == XR_SEM_CALL_TARGET_SOURCE_CLASS_CONSTRUCTOR &&
            target->dependency != XR_SEMANTIC_INDEX_NONE) {
            if (target->dependency >= dependency_count ||
                target->operation >= plan->operation_count ||
                target->source_export >= dependencies[target->dependency]->source_export_count) {
                valid = false;
                break;
            }
            const XrSemanticPlan *match = dependencies[target->dependency];
            const XrSemanticOperationRecord *operation = &plan->operations[target->operation];
            const XrSemanticSourceExportRecord *source_export =
                &match->source_exports[target->source_export];
            uint32_t constructor = XR_SEMANTIC_INDEX_NONE;
            uint32_t source_class = xr_semantic_imported_class_construction_source_class(
                plan, match, &plan->dependencies[target->dependency], source_export, operation,
                &constructor);
            const XrSemanticFunctionRecord *callee =
                constructor != XR_SEMANTIC_INDEX_NONE ? &match->functions[constructor] : NULL;
            XrStableId zero = {{0}};
            bool callee_identity = callee ? xr_stable_id_equal(target->callee_function, callee->id)
                                          : xr_stable_id_equal(target->callee_function, zero);
            if (source_class == XR_SEMANTIC_INDEX_NONE ||
                target->function != XR_SEMANTIC_INDEX_NONE ||
                target->callable_type != operation->result_type ||
                !xr_stable_id_equal(target->export_identity, source_export->id) ||
                !callee_identity) {
                snprintf(authority_detail, sizeof(authority_detail),
                         "imported source class construction disagrees with frozen dependency "
                         "target=%u operation=%u dependency=%u export=%u",
                         target_index, target->operation, target->dependency,
                         target->source_export);
                valid = false;
                if (!survey)
                    break;
                semantic_survey_row("source_imported_class_constructor", authority_detail);
                refused_rows++;
                continue;
            }
            used[target->dependency] = 1;
            continue;
        }
        if (target->kind == XR_SEM_CALL_TARGET_SOURCE_INSTANCE_METHOD_OPEN) {
            if (target->dependency >= dependency_count ||
                target->operation >= plan->operation_count ||
                target->callable_type >= plan->type_count) {
                valid = false;
                break;
            }
            const XrSemanticPlan *match = dependencies[target->dependency];
            const XrSemanticOperationRecord *operation = &plan->operations[target->operation];
            const XrSemanticTypeRecord *receiver_type = &plan->types[target->callable_type];
            const XrSemanticSourceMethodRecord *method = NULL;
            for (uint32_t sm = 0; sm < match->source_method_count; sm++) {
                if (!xr_stable_id_equal(match->source_methods[sm].id, target->export_identity))
                    continue;
                if (method) {
                    method = NULL;
                    break;
                }
                method = &match->source_methods[sm];
            }
            const XrSemanticSourceClassRecord *source_class =
                method && method->source_class < match->source_class_count
                    ? &match->source_classes[method->source_class]
                    : NULL;
            const char *selector =
                operation->metadata_count == 1 ? plan->metadata[operation->metadata_begin] : NULL;
            uint8_t method_required =
                XR_SEM_SOURCE_METHOD_INSTANCE | XR_SEM_SOURCE_METHOD_OPEN_DOMAIN;
            if (!method || !source_class || !selector || operation->opcode != XI_CALL_METHOD ||
                operation->operand_count != method->parameter_count ||
                method->function >= match->function_count ||
                !suspendable[target->dependency][method->function] ||
                strcmp(selector, method->name) != 0 ||
                (method->flags & method_required) != method_required ||
                (source_class->flags & XR_SEM_SOURCE_CLASS_RUNTIME_TYPE) == 0 ||
                (source_class->flags &
                 (XR_SEM_SOURCE_CLASS_EXPLICIT_FINAL | XR_SEM_SOURCE_CLASS_GENERIC)) != 0 ||
                !xr_stable_id_equal(source_class->id, receiver_type->source_class_identity)) {
                snprintf(authority_detail, sizeof(authority_detail),
                         "open instance-method call disagrees with frozen dependency "
                         "target=%u operation=%u dependency=%u selector=%s",
                         target_index, target->operation, target->dependency,
                         selector ? selector : "");
                valid = false;
                if (!survey)
                    break;
                semantic_survey_row("source_open_method_call", authority_detail);
                refused_rows++;
                continue;
            }
            used[target->dependency] = 1;
            continue;
        }
        if (target->kind != XR_SEM_CALL_TARGET_SOURCE_EXPORT)
            continue;
        if (target->dependency >= dependency_count) {
            valid = false;
            break;
        }
        const XrSemanticPlan *match = dependencies[target->dependency];
        const XrSemanticSourceExportRecord *source_export =
            target->source_export < match->source_export_count
                ? &match->source_exports[target->source_export]
                : NULL;
        const XrSemanticOperationRecord *operation =
            target->operation < plan->operation_count ? &plan->operations[target->operation] : NULL;
        const XrSemanticFunctionRecord *callee =
            source_export && source_export->kind == XR_SEM_SOURCE_EXPORT_FUNCTION &&
                    source_export->function < match->function_count
                ? &match->functions[source_export->function]
                : NULL;
        const XrSemanticTypeRecord *caller_result =
            operation && operation->result_type < plan->type_count
                ? &plan->types[operation->result_type]
                : NULL;
        const XrSemanticTypeRecord *callee_result =
            callee && callee->return_type < match->type_count ? &match->types[callee->return_type]
                                                              : NULL;
        const char *source_module = NULL;
        const char *selector = NULL;
        bool exact_source_call = operation && resolve_frozen_source_namespace_target(
                                                  plan, definitions, value_count, &stores,
                                                  target->operation, &source_module, &selector);
        if (!source_export || source_export->kind != XR_SEM_SOURCE_EXPORT_FUNCTION || !operation ||
            !callee || !xr_stable_id_equal(source_export->exported_entity, callee->id) ||
            !exact_source_call || !source_module || !selector ||
            strcmp(source_module, plan->dependencies[target->dependency].module_path) != 0 ||
            strcmp(selector, source_export->name) != 0 || callee->parameter_count == UINT16_MAX ||
            operation->operand_count != (uint16_t) (callee->parameter_count + 1u) ||
            !caller_result || !callee_result ||
            !xr_stable_id_equal(caller_result->id, callee_result->id) ||
            (semantic_operation_coroutine_state_count(plan, target->operation) == 1) !=
                (suspendable[target->dependency][source_export->function] != 0) ||
            !xr_stable_id_equal(target->export_identity, source_export->id) ||
            !xr_stable_id_equal(target->callee_function, callee->id)) {
            uint32_t call_value = operation && operation->operand_count > 0
                                      ? plan->operands[operation->operand_begin].value
                                      : XR_SEMANTIC_INDEX_NONE;
            uint32_t call_definition =
                call_value < value_count ? definitions[call_value] : XR_SEMANTIC_INDEX_NONE;
            const XrSemanticOperationRecord *call_producer =
                call_definition < plan->operation_count ? &plan->operations[call_definition] : NULL;
            snprintf(authority_detail, sizeof(authority_detail),
                     "source export call disagrees with frozen import or dependency "
                     "target=%u operation=%u opcode=%u dependency=%u export=%u "
                     "source_call=%u module=%s selector=%s expected=%s state=%u callee_suspend=%u "
                     "callee_value=%u definition=%u definition_opcode=%u definition_immediate=%lld",
                     target_index, target->operation, operation ? operation->opcode : 0u,
                     target->dependency, target->source_export, exact_source_call ? 1u : 0u,
                     source_module ? source_module : "", selector ? selector : "",
                     source_export && source_export->name ? source_export->name : "",
                     semantic_operation_coroutine_state_count(plan, target->operation),
                     source_export && source_export->function < match->function_count
                         ? suspendable[target->dependency][source_export->function]
                         : 0u,
                     call_value, call_definition, call_producer ? call_producer->opcode : 0u,
                     (long long) (call_producer ? call_producer->semantic_immediate : 0));
            valid = false;
            if (!survey)
                break;
            semantic_survey_row("source_export_call", authority_detail);
            refused_rows++;
            continue;
        }
        for (uint32_t ordinal = 0; target_valid && ordinal < callee->parameter_count; ordinal++) {
            uint32_t parameter_index = callee->parameter_begin + ordinal;
            uint32_t operand_index = operation->operand_begin + ordinal + 1u;
            const XrSemanticParameterRecord *parameter = parameter_index < match->parameter_count
                                                             ? &match->parameters[parameter_index]
                                                             : NULL;
            const XrSemanticOperandRecord *operand =
                operand_index < plan->operand_count ? &plan->operands[operand_index] : NULL;
            const XrSemanticTypeRecord *operand_type =
                operand && operand->type < plan->type_count ? &plan->types[operand->type] : NULL;
            const XrSemanticTypeRecord *parameter_type =
                parameter && parameter->type < match->type_count ? &match->types[parameter->type]
                                                                 : NULL;
            bool reference = parameter && parameter->mode == XR_PARAM_REF;
            bool read = parameter && parameter->mode == XR_PARAM_READ;
            bool addressable = operand && (operand->flags & XR_SEM_OPERAND_ADDRESSABLE) != 0;
            /* Which of the eleven agreements failed, by name. This used to be
             * one conjunction reported as "argument N disagrees", which names
             * the argument and nothing else: a type mismatch, a borrow the
             * callee never asked for and an operand row belonging to a
             * different call all printed the same sentence, and separating
             * them meant rebuilding with temporary printfs. */
            const char *disagreement = NULL;
            if (!parameter || !operand || !operand_type || !parameter_type)
                disagreement = "row-out-of-range";
            else if (parameter->function != source_export->function ||
                     parameter->ordinal != ordinal)
                disagreement = "parameter-row-is-another-function";
            else if (!parameter_type_admits_argument(match, parameter_type, operand_type))
                disagreement = "argument-type";
            else if (operand->role != XR_SEM_OPERAND_ARGUMENT ||
                     operand->parameter != (int16_t) ordinal)
                disagreement = "operand-is-not-this-argument";
            else if (operand->parameter_mode != parameter->mode)
                disagreement = "parameter-mode";
            else if (operand->transfer_mode != parameter->transfer_mode)
                disagreement = "transfer-mode";
            else if ((operand->flags & XR_SEM_OPERAND_CALL_CONTRACT) == 0)
                disagreement = "no-call-contract";
            else if (!read && !reference)
                disagreement = "parameter-mode-is-neither-read-nor-ref";
            else if (read && (operand->access != XR_CALL_ARG_PLAIN || addressable))
                disagreement = "read-argument-access";
            else if (reference && (operand->access != XR_CALL_ARG_REF || !addressable ||
                                   operand->ownership_action != XR_SEM_OPERAND_BORROW))
                disagreement = "ref-argument-access";
            if (disagreement) {
                valid = false;
                target_valid = false;
                snprintf(authority_detail, sizeof(authority_detail),
                         "source export call argument disagrees with frozen dependency "
                         "target=%u operation=%u ordinal=%u dependency=%u export=%u failed=%s "
                         "caller_mode=%u callee_mode=%u caller_transfer=%u callee_transfer=%u "
                         "access=%u addressable=%u ownership=%u flags=%u",
                         target_index, target->operation, ordinal, target->dependency,
                         target->source_export, disagreement,
                         operand ? (unsigned) operand->parameter_mode : 0u,
                         parameter ? (unsigned) parameter->mode : 0u,
                         operand ? (unsigned) operand->transfer_mode : 0u,
                         parameter ? (unsigned) parameter->transfer_mode : 0u,
                         operand ? (unsigned) operand->access : 0u, addressable ? 1u : 0u,
                         operand ? (unsigned) operand->ownership_action : 0u,
                         operand ? (unsigned) operand->flags : 0u);
                /* The two rows come from two plans, so an argument-type
                 * disagreement is either a real type mismatch or one type
                 * spelled the same and keyed differently across the module
                 * boundary. Only the two canonical keys tell those apart. */
                if (disagreement && strcmp(disagreement, "argument-type") == 0) {
                    size_t used = strlen(authority_detail);
                    snprintf(authority_detail + used, sizeof(authority_detail) - used,
                             " caller_key=%s callee_key=%s",
                             operand_type && operand_type->canonical_key
                                 ? operand_type->canonical_key
                                 : "<none>",
                             parameter_type && parameter_type->canonical_key
                                 ? parameter_type->canonical_key
                                 : "<none>");
                }
            }
        }
        if (!target_valid) {
            if (!survey)
                break;
            semantic_survey_row("source_export_call_argument", authority_detail);
            refused_rows++;
            continue;
        }
        used[target->dependency] = 1;
    }
    if (refused_rows)
        fprintf(stderr, "[refusal-survey] module-set authority gaps: %u\n", refused_rows);
    xr_free(definitions);
    xr_free(stores.rows);
    for (uint32_t i = 0; valid && i < dependency_count; i++)
        valid = used[i] != 0;
    if (valid && !verify_module_set_coroutine_authority(plan, dependencies, dependency_count,
                                                        suspendable, error, error_size)) {
        for (uint32_t i = 0; i < dependency_count; i++)
            xr_free(suspendable[i]);
        xr_free(suspendable);
        xr_free(used);
        return false;
    }
    for (uint32_t i = 0; i < dependency_count; i++)
        xr_free(suspendable[i]);
    xr_free(suspendable);
    xr_free(used);
    if (valid && !verify_graph_program_module_set(plan, dependencies, dependency_count, error,
                                                  error_size))
        return false;
    if (!valid)
        return report(error, error_size, "XR_SEM_0019",
                      authority_detail[0]
                          ? authority_detail
                          : "ordered source authority is not grounded by verified dependencies");
    return true;
}
