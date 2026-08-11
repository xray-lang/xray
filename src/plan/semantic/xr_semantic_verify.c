/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_verify.c - Independent SemanticPlan verifier
 */

#include "xr_semantic_verify.h"
#include "xr_semantic_graph.h"
#include "xr_semantic_ops.h"
#include "xr_semantic_plan_internal.h"
#include "../ownership/xr_ownership_check.h"
#include "../ownership/xr_ownership_certificate_internal.h"
#include "../../base/xmalloc.h"
#include "../../ir/xi.h"
#include "../../ir/xi_own.h"
#include "../../ir/xi_ops_gen.h"
#include "../../runtime/value/xtype.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool report(char *error, size_t size, const char *code, const char *detail) {
    if (error && size)
        snprintf(error, size, "%s: %s", code, detail);
    return false;
}

static bool range_valid(uint32_t begin, uint32_t count, uint32_t limit) {
    return begin <= limit && count <= limit - begin;
}

static bool is_power_of_two_u32(uint32_t value) {
    return value != 0 && (value & (value - 1u)) == 0;
}

static bool verify_id(const char *key, XrStableId actual) {
    XrStableId expected;
    XrFingerprint digest;
    return key && xr_stable_id_from_key(key, &expected, &digest) &&
           xr_stable_id_equal(expected, actual);
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
            coverage->field = (uint8_t *) xr_calloc((count), 1);                                  \
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
    int length = snprintf(expected, sizeof(expected),
                          "entity-v1:schema=%u:kind=%u:parent=%s:declaration=%s:"
                          "function=%s:operation=%s:ordinal=0:type=%s:ownership=%u:alias=%d",
                          XR_SEMANTIC_SCHEMA_VERSION, (unsigned) XR_SEM_ENTITY_LOAN, parent_id,
                          declaration_id, function_id, operation_id, type_id,
                          (unsigned) operation->result_ownership,
                          (int) operation->result_alias_operand);
    return length > 0 && (size_t) length < sizeof(expected) &&
           strcmp(entity->canonical_key, expected) == 0;
}

static bool verify_entity_record(const XrSemanticPlan *plan, const XrSemanticEntityRecord *entity,
                                 XrEntityCoverage *coverage) {
    const XrSemanticEntityRecord *parent = entity->parent == XR_SEMANTIC_INDEX_NONE
                                               ? NULL
                                               : &plan->entities[entity->parent];
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
                    (plan->types[entity->subject].flags &
                     XR_SEM_TYPE_AGGREGATE_EXACT) != 0) &&
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
                   ((plan->operations[entity->subject].effects & XI_EFFECT_MAY_SUSPEND) != 0 ||
                    plan->operations[entity->subject].opcode == XI_GO) &&
                   entity->ordinal != 0 &&
                   mark_entity(coverage->coroutine_states, plan->operation_count,
                               entity->subject);
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
                               (plan->types[i].flags &
                                XR_SEM_TYPE_AGGREGATE_EXACT) != 0;
        if (coverage->types[i] != 1 ||
            (aggregate_shape && coverage->shapes[i] != 1))
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
            coverage->closures[i] != (uint8_t) closure ||
            (native && coverage->natives[i] != 1))
            return false;
    }
    for (uint32_t i = 0; i < plan->operation_count; i++) {
        bool allocation = plan->operations[i].allocation_key != NULL;
        bool debug_span = plan->operations[i].source_file != NULL;
        bool operation_loan = borrowed_result_has_loan(plan, i);
        if (coverage->operations[i] != 1 ||
            coverage->allocations[i] != (uint8_t) allocation ||
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
    XrCoroutineEntityKey *keys = count
                                     ? (XrCoroutineEntityKey *) xr_malloc((size_t) count *
                                                                          sizeof(*keys))
                                     : NULL;
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
        uint32_t expected = i == 0 || keys[i - 1].function != keys[i].function
                                ? 1
                                : keys[i - 1].state + 1;
        if (keys[i].state != expected) {
            valid = false;
            break;
        }
    }
    xr_free(keys);
    return valid;
}

static bool verify_entities(const XrSemanticPlan *plan, char *error, size_t error_size) {
    XrEntityCoverage coverage = {0};
    if (!entity_coverage_init(plan, &coverage))
        return report(error, error_size, "XR_EXEC_5003", "entity verifier budget exhausted");
    for (uint32_t i = 0; i < plan->entity_count; i++) {
        const XrSemanticEntityRecord *entity = &plan->entities[i];
        bool valid = verify_id(entity->canonical_key, entity->id) &&
                     entity->kind < XR_SEM_ENTITY_KIND_COUNT && entity->flags == 0 &&
                     (entity->parent == XR_SEMANTIC_INDEX_NONE ||
                      entity->parent < plan->entity_count) &&
                     (i == 0 || xr_stable_id_compare(plan->entities[i - 1].id, entity->id) < 0) &&
                     verify_entity_record(plan, entity, &coverage);
        if (!valid) {
            entity_coverage_dispose(&coverage);
            return report(error, error_size, "XR_SEM_0019",
                          "stable entity identity relation is invalid");
        }
    }
    bool complete = verify_entity_coverage(plan, &coverage) &&
                    verify_coroutine_entity_sequence(plan);
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
            (type->kind != XR_KIND_INSTANCE ||
             (type->flags & XR_SEM_TYPE_VALUE) == 0))
            return report(error, error_size, "XR_SEM_0012",
                          "exact aggregate flag is invalid");
        if (type->kind == XR_KIND_FIXED_ARRAY) {
            if (type->child_count != 1 || type->aggregate_extent == 0 ||
                type->aggregate_align != 0)
                return report(error, error_size, "XR_SEM_0012",
                              "fixed-array aggregate facts are invalid");
        } else if (type->kind == XR_KIND_TUPLE ||
                   type->kind == XR_KIND_STRUCT_OBJECT) {
            if (type->aggregate_extent != type->child_count ||
                type->aggregate_align != 0)
                return report(error, error_size, "XR_SEM_0012",
                              "structural aggregate facts are invalid");
        } else if ((type->flags & XR_SEM_TYPE_AGGREGATE_EXACT) != 0) {
            if (type->aggregate_extent != type->child_count ||
                (type->aggregate_align != 0 &&
                 !is_power_of_two_u32(type->aggregate_align)))
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
           report(error, error_size, "XR_SEM_0012",
                  "type child table is not exactly partitioned");
}

static bool verify_functions(const XrSemanticPlan *plan, char *error, size_t error_size) {
    uint32_t parameter_cursor = 0;
    uint32_t capture_cursor = 0;
    uint32_t block_cursor = 0;
    uint32_t value_cursor = 0;
    uint32_t *child_counts = (uint32_t *) xr_calloc(plan->function_count, sizeof(*child_counts));
    if (plan->function_count && !child_counts)
        return report(error, error_size, "XR_EXEC_5003",
                      "function relation verifier budget exhausted");
#define XR_FUNCTION_FAIL(code, detail)                                                             \
    do {                                                                                           \
        xr_free(child_counts);                                                                     \
        return report(error, error_size, (code), (detail));                                        \
    } while (0)
    for (uint32_t i = 0; i < plan->function_count; i++) {
        const XrSemanticFunctionRecord *function = &plan->functions[i];
        if (!verify_id(function->canonical_key, function->id) || !function->name)
            XR_FUNCTION_FAIL("XR_SEM_0002", "function identity is invalid");
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
        bool closure_binding =
            operation->opcode == XI_CLOSURE_NEW ||
            (operation->opcode == XI_STACK_ALLOC &&
             operation->semantic_immediate == XI_CLOSURE_NEW);
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
            operation->effects != contract->effects ||
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
            !range_valid(operation->operand_begin, operation->operand_count,
                         plan->operand_count) ||
            !range_valid(operation->metadata_begin, operation->metadata_count,
                         plan->metadata_count) ||
            (operation->constant != XR_SEMANTIC_INDEX_NONE &&
             operation->constant >= plan->constant_count) ||
            (closure_binding !=
             (operation->callable_function != XR_SEMANTIC_INDEX_NONE)) ||
            (closure_binding &&
             (operation->callable_function >= plan->function_count ||
              plan->functions[operation->callable_function].parent != operation->function))) {
            return report(error, error_size, "XR_SEM_0015", "operation record is invalid");
        }
        if (!operation_debug_span_valid(plan, i))
            return report(error, error_size, "XR_SEM_0019", "operation debug span is invalid");
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

static uint32_t resolve_frozen_direct_call_target(const XrSemanticPlan *plan,
                                                  const uint32_t *definitions,
                                                  uint32_t value_count,
                                                  uint32_t operation_index) {
    const XrSemanticOperationRecord *call = &plan->operations[operation_index];
    if ((call->opcode != XI_CALL && call->opcode != XI_TAIL_CALL) ||
        call->operand_count == 0)
        return XR_SEMANTIC_INDEX_NONE;
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
            (producer->opcode == XI_STACK_ALLOC &&
             producer->semantic_immediate == XI_CLOSURE_NEW);
        if (closure_binding)
            return producer->callable_function;
        if (producer->opcode != XI_COPY ||
            producer->semantic_immediate != XI_COPY_KIND_IDENTITY ||
            producer->operand_count != 1 || producer->result_alias_operand != 0)
            return XR_SEMANTIC_INDEX_NONE;
        value = plan->operands[producer->operand_begin].value;
    }
    return XR_SEMANTIC_INDEX_NONE;
}

static bool verify_call_targets(const XrSemanticPlan *plan, const uint32_t *definitions,
                                uint32_t value_count, char *error, size_t error_size) {
    uint32_t cursor = 0;
    for (uint32_t operation = 0; operation < plan->operation_count; operation++) {
        uint32_t function = resolve_frozen_direct_call_target(
            plan, definitions, value_count, operation);
        if (function == XR_SEMANTIC_INDEX_NONE)
            continue;
        if (cursor >= plan->call_target_count)
            return report(error, error_size, "XR_SEM_0019",
                          "provable direct call has no call-target authority");
        const XrSemanticCallTargetRecord *target = &plan->call_targets[cursor++];
        if (target->operation != operation || target->function != function ||
            target->kind != XR_SEM_CALL_TARGET_DIRECT_LOCAL || target->reserved[0] != 0 ||
            target->reserved[1] != 0 || target->reserved[2] != 0)
            return report(error, error_size, "XR_SEM_0019",
                          "call-target authority disagrees with frozen SSA");
        char operation_id[XR_STABLE_ID_BYTES * 2 + 1];
        char function_id[XR_STABLE_ID_BYTES * 2 + 1];
        char expected_key[192];
        xr_stable_id_hex(plan->operations[operation].id, operation_id);
        xr_stable_id_hex(plan->functions[function].id, function_id);
        int length = snprintf(expected_key, sizeof(expected_key),
                              "call-target-v1:schema=%u:operation=%s:function=%s:kind=%u",
                              XR_SEMANTIC_SCHEMA_VERSION, operation_id, function_id,
                              (unsigned) XR_SEM_CALL_TARGET_DIRECT_LOCAL);
        if (length < 0 || (size_t) length >= sizeof(expected_key) ||
            strcmp(target->canonical_key ? target->canonical_key : "", expected_key) != 0 ||
            !verify_id(target->canonical_key, target->id))
            return report(error, error_size, "XR_SEM_0019",
                          "call-target stable identity is not canonical");
    }
    return cursor == plan->call_target_count ||
           report(error, error_size, "XR_SEM_0019",
                  "unprovable call-target authority is present");
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
        verify_call_targets(plan, definitions, value_count, error, error_size);
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
        if ((constant->kind == XR_SEM_CONST_STRING ||
             constant->kind == XR_SEM_CONST_ENUM_NAMESPACE) != has_text)
            return report(error, error_size, "XR_SEM_0009",
                          "constant string payload does not match its kind");
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

bool xr_semantic_plan_verify(const XrSemanticPlan *plan, char *error, size_t error_size) {
    if (!plan || !plan->frozen || plan->schema != XR_SEMANTIC_SCHEMA_VERSION)
        return report(error, error_size, "XR_SEM_0004",
                      "verifier requires a frozen exact-version SemanticPlan");
    XrFingerprint current_registry;
    xr_semantic_op_registry_fingerprint(&current_registry);
    if (!xr_semantic_op_registry_verify(error, error_size) ||
        !xr_fingerprint_equal(plan->operation_registry_fingerprint, current_registry))
        return report(error, error_size, "XR_SEM_0017",
                      "SemanticPlan operation registry is missing or incompatible");
    if (plan->type_count > 1000000u || plan->function_count > 100000u ||
        plan->block_count > 2000000u || plan->operation_count > 10000000u ||
        plan->parameter_count > 25600000u || plan->capture_count > 6400000u ||
        plan->edge_count > 40000000u || plan->operand_count > 40000000u ||
        plan->entity_count > 80000000u || plan->call_target_count > plan->operation_count)
        return report(error, error_size, "XR_EXEC_5003", "SemanticPlan exceeds hard budgets");
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
    bool verified = xr_semantic_plan_verify_identity_set(plan, error, error_size) &&
                    verify_entities(plan, error, error_size) &&
                    verify_types(plan, error, error_size) &&
                    verify_functions(plan, error, error_size) &&
                    verify_edges(plan, block_edge_mask, operation_edge_mask, error, error_size) &&
                    verify_blocks(plan, block_edge_mask, error, error_size) &&
                    xr_semantic_graph_build(plan, &graph, error, error_size) &&
                    verify_operations(plan, operation_edge_mask, &graph, error, error_size) &&
                    verify_constants(plan, error, error_size) &&
                    xr_ownership_certificate_check(plan, &graph, error, error_size);
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
