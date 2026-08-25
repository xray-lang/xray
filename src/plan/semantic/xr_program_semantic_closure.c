/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_program_semantic_closure.c - Program semantic closure builder and freezer
 *
 * KEY CONCEPT:
 *   The builder is the only mutation owner. Freeze canonicalizes every table,
 *   seals a target-neutral fingerprint and generation closure identity, then
 *   hands the result to the independently implemented verifier.
 */

#include "xr_program_semantic_closure_internal.h"
#include "xr_source_semantic_identity.h"
#include "../../base/xmalloc.h"
#include "../../base/xsha256.h"
#include "../../shared/xr_exact_scalar_registry.h"
#include "../../runtime/value/xtype.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool fail(char *error, size_t error_size, const char *code, const char *detail) {
    if (error && error_size)
        snprintf(error, error_size, "%s: %s", code, detail);
    return false;
}

static bool bytes_are_zero(const uint8_t *bytes, size_t size) {
    uint8_t combined = 0;
    for (size_t i = 0; i < size; i++)
        combined |= bytes[i];
    return combined == 0;
}

static bool stable_id_is_zero(XrStableId id) {
    return bytes_are_zero(id.bytes, sizeof(id.bytes));
}

static bool fingerprint_is_zero(XrFingerprint fingerprint) {
    return bytes_are_zero(fingerprint.bytes, sizeof(fingerprint.bytes));
}

static bool stable_id_equal(XrStableId left, XrStableId right) {
    return memcmp(left.bytes, right.bytes, sizeof(left.bytes)) == 0;
}

static void hash_u32(XrSHA256Context *context, uint32_t value) {
    uint8_t bytes[4];
    for (uint32_t i = 0; i < sizeof(bytes); i++)
        bytes[i] = (uint8_t) (value >> (i * 8u));
    xr_sha256_update(context, bytes, sizeof(bytes));
}

static void hash_u64(XrSHA256Context *context, uint64_t value) {
    uint8_t bytes[8];
    for (uint32_t i = 0; i < sizeof(bytes); i++)
        bytes[i] = (uint8_t) (value >> (i * 8u));
    xr_sha256_update(context, bytes, sizeof(bytes));
}

static void hash_stable_id(XrSHA256Context *context, XrStableId id) {
    xr_sha256_update(context, id.bytes, sizeof(id.bytes));
}

static void hash_fingerprint(XrSHA256Context *context, XrFingerprint fingerprint) {
    xr_sha256_update(context, fingerprint.bytes, sizeof(fingerprint.bytes));
}

static void hash_framed_bytes(XrSHA256Context *context, const uint8_t *bytes, size_t size) {
    hash_u64(context, (uint64_t) size);
    if (size)
        xr_sha256_update(context, bytes, size);
}

static void hash_framed_id(XrSHA256Context *context, XrStableId id) {
    hash_framed_bytes(context, id.bytes, sizeof(id.bytes));
}

static void hash_framed_fingerprint(XrSHA256Context *context, XrFingerprint fingerprint) {
    hash_framed_bytes(context, fingerprint.bytes, sizeof(fingerprint.bytes));
}

static XrStableId finish_stable_id(XrSHA256Context *context) {
    uint8_t digest[XR_FINGERPRINT_BYTES];
    XrStableId id;
    xr_sha256_final(context, digest);
    memcpy(id.bytes, digest, sizeof(id.bytes));
    return id;
}

static XrStableId derive_type_identity(XrFingerprint policy_fingerprint,
                                       const XrProgramSemanticTypeInput *input) {
    if (input->kind == XR_PROGRAM_SEMANTIC_TYPE_EXACT_SCALAR) {
        static const uint8_t scalar_domain[] = "xray-program-exact-scalar-type-v1\0";
        XrSHA256Context scalar_context;
        xr_sha256_init(&scalar_context);
        xr_sha256_update(&scalar_context, scalar_domain, sizeof(scalar_domain) - 1u);
        hash_u32(&scalar_context, XR_PROGRAM_SEMANTIC_CLOSURE_SCHEMA_VERSION);
        hash_u32(&scalar_context, input->exact_scalar);
        hash_fingerprint(&scalar_context, input->shape_fingerprint);
        hash_fingerprint(&scalar_context, input->ownership_fingerprint);
        return finish_stable_id(&scalar_context);
    }
    static const uint8_t domain[] = "xray-program-semantic-type-v1\0";
    XrSHA256Context context;
    xr_sha256_init(&context);
    xr_sha256_update(&context, domain, sizeof(domain) - 1u);
    /* Opaque and aggregate rows use the current general type identity frame. */
    hash_u32(&context, UINT32_C(1));
    hash_fingerprint(&context, policy_fingerprint);
    hash_stable_id(&context, input->module_identity);
    hash_stable_id(&context, input->declaration_identity);
    hash_stable_id(&context, input->concrete_instance_identity);
    hash_fingerprint(&context, input->shape_fingerprint);
    hash_fingerprint(&context, input->ownership_fingerprint);
    return finish_stable_id(&context);
}

static XrStableId exact_scalar_registry_owner(void) {
    static const uint8_t domain[] = "xray-exact-scalar-registry-authority-v1\0";
    XrSHA256Context context;
    xr_sha256_init(&context);
    xr_sha256_update(&context, domain, sizeof(domain) - 1u);
    hash_u32(&context, XR_PROGRAM_SEMANTIC_CLOSURE_SCHEMA_VERSION);
    return finish_stable_id(&context);
}

static XrStableId exact_scalar_declaration(uint8_t exact_scalar) {
    static const uint8_t domain[] = "xray-exact-scalar-declaration-v1\0";
    XrSHA256Context context;
    xr_sha256_init(&context);
    xr_sha256_update(&context, domain, sizeof(domain) - 1u);
    hash_u32(&context, exact_scalar);
    return finish_stable_id(&context);
}

static XrStableId exact_scalar_instance(XrStableId declaration, uint8_t exact_scalar) {
    static const uint8_t domain[] = "xray-exact-scalar-instance-v1\0";
    XrSHA256Context context;
    xr_sha256_init(&context);
    xr_sha256_update(&context, domain, sizeof(domain) - 1u);
    hash_stable_id(&context, declaration);
    hash_u32(&context, exact_scalar);
    return finish_stable_id(&context);
}

bool xr_program_semantic_exact_scalar_type_input(uint8_t exact_scalar,
                                                 XrProgramSemanticTypeInput *out) {
    if (out)
        memset(out, 0, sizeof(*out));
    if (!out || !xr_exact_scalar_by_id((XrExactScalarId) exact_scalar))
        return false;
    XrStableId declaration = exact_scalar_declaration(exact_scalar);
    *out = (XrProgramSemanticTypeInput) {
        .module_identity = exact_scalar_registry_owner(),
        .declaration_identity = declaration,
        .concrete_instance_identity = exact_scalar_instance(declaration, exact_scalar),
        .kind = XR_PROGRAM_SEMANTIC_TYPE_EXACT_SCALAR,
        .exact_scalar = exact_scalar,
        .flags = XR_PROGRAM_SEMANTIC_TYPE_NONNULLABLE | XR_PROGRAM_SEMANTIC_TYPE_NONGENERIC |
                 XR_PROGRAM_SEMANTIC_TYPE_VALUE | XR_PROGRAM_SEMANTIC_TYPE_POINTER_FREE,
    };
    return true;
}

static void derive_typed_type_fingerprints(const XrProgramSemanticTypeInput *input,
                                           XrFingerprint *shape, XrFingerprint *ownership) {
    static const uint8_t shape_domain[] = "xray-program-semantic-typed-shape-v1\0";
    static const uint8_t ownership_domain[] = "xray-program-semantic-leaf-ownership-v1\0";
    XrSHA256Context context;
    xr_sha256_init(&context);
    xr_sha256_update(&context, shape_domain, sizeof(shape_domain) - 1u);
    hash_u32(&context, XR_PROGRAM_SEMANTIC_CLOSURE_SCHEMA_VERSION);
    hash_u32(&context, input->kind);
    hash_u32(&context, input->exact_scalar);
    hash_u32(&context, input->flags);
    hash_u32(&context, input->field_count);
    for (uint32_t i = 0; i < input->field_count; i++) {
        hash_u32(&context, input->fields[i].declaration_ordinal);
        hash_stable_id(&context, input->fields[i].field_type);
    }
    xr_sha256_final(&context, shape->bytes);

    xr_sha256_init(&context);
    xr_sha256_update(&context, ownership_domain, sizeof(ownership_domain) - 1u);
    hash_u32(&context, XR_PROGRAM_SEMANTIC_CLOSURE_SCHEMA_VERSION);
    hash_u32(&context, input->kind);
    hash_u32(&context, input->flags);
    /* Copy/move by value, no drop, no root, and no interior pointer. */
    hash_u32(&context, UINT32_C(1));
    hash_u32(&context, UINT32_C(1));
    hash_u32(&context, UINT32_C(0));
    hash_u32(&context, UINT32_C(0));
    hash_u32(&context, UINT32_C(0));
    xr_sha256_final(&context, ownership->bytes);
}

static bool locator_is_valid(XrProgramSemanticSourceLocator locator);

static bool typed_type_input_is_valid(const XrProgramSemanticClosure *closure,
                                      const XrProgramSemanticTypeInput *input) {
    const uint8_t required = XR_PROGRAM_SEMANTIC_TYPE_NONNULLABLE |
                             XR_PROGRAM_SEMANTIC_TYPE_NONGENERIC | XR_PROGRAM_SEMANTIC_TYPE_VALUE |
                             XR_PROGRAM_SEMANTIC_TYPE_POINTER_FREE;
    if (!closure || !input || input->reserved != 0 || input->flags != required ||
        !fingerprint_is_zero(input->shape_fingerprint) ||
        !fingerprint_is_zero(input->ownership_fingerprint))
        return false;
    if (input->kind == XR_PROGRAM_SEMANTIC_TYPE_EXACT_SCALAR)
        return input->field_count == 0 && !input->fields &&
               xr_exact_scalar_by_id((XrExactScalarId) input->exact_scalar) &&
               input->declaration_locator.kind == 0 && input->declaration_locator.start_line == 0 &&
               input->declaration_locator.start_column == 0 &&
               input->declaration_locator.end_line == 0 &&
               input->declaration_locator.end_column == 0;
    if (input->kind != XR_PROGRAM_SEMANTIC_TYPE_LEAF_VALUE_AGGREGATE ||
        input->exact_scalar != XR_EXACT_SCALAR_NONE || input->field_count == 0 || !input->fields ||
        !locator_is_valid(input->declaration_locator) ||
        input->field_count > closure->limits.max_type_fields - closure->type_field_count)
        return false;
    for (uint32_t i = 0; i < input->field_count; i++) {
        const XrProgramSemanticTypeFieldInput *field = &input->fields[i];
        bool found = false;
        if (field->reserved != 0 || field->declaration_ordinal != i ||
            stable_id_is_zero(field->field_type))
            return false;
        for (uint32_t j = 0; j < closure->type_count; j++) {
            const XrProgramSemanticTypeRecord *candidate = &closure->types[j];
            if (stable_id_equal(candidate->id, field->field_type) &&
                candidate->kind == XR_PROGRAM_SEMANTIC_TYPE_EXACT_SCALAR) {
                found = true;
                break;
            }
        }
        if (!found)
            return false;
    }
    return true;
}

static XrStableId derive_function_identity(XrFingerprint policy_fingerprint,
                                           const XrProgramSemanticFunctionInput *input) {
    static const uint8_t domain[] = "xray-program-semantic-function-v1\0";
    XrSHA256Context context;
    xr_sha256_init(&context);
    xr_sha256_update(&context, domain, sizeof(domain) - 1u);
    hash_u32(&context, UINT32_C(1));
    hash_fingerprint(&context, policy_fingerprint);
    hash_stable_id(&context, input->module_identity);
    hash_stable_id(&context, input->declaration_identity);
    hash_stable_id(&context, input->concrete_instance_identity);
    hash_fingerprint(&context, input->signature_fingerprint);
    hash_fingerprint(&context, input->effect_fingerprint);
    hash_u64(&context, input->capability_mask);
    return finish_stable_id(&context);
}

static XrStableId derive_call_identity(XrFingerprint policy_fingerprint,
                                       const XrProgramSemanticCallInput *input) {
    static const uint8_t domain[] = "xray-program-semantic-call-v1\0";
    XrSHA256Context context;
    xr_sha256_init(&context);
    xr_sha256_update(&context, domain, sizeof(domain) - 1u);
    hash_u32(&context, UINT32_C(1));
    hash_fingerprint(&context, policy_fingerprint);
    hash_stable_id(&context, input->callsite_identity);
    hash_stable_id(&context, input->caller_function);
    hash_stable_id(&context, input->callee_function);
    hash_fingerprint(&context, input->contract_fingerprint);
    return finish_stable_id(&context);
}

static bool locator_is_valid(XrProgramSemanticSourceLocator locator) {
    if (locator.kind == 0 || locator.kind > INT32_MAX || locator.start_line == 0 ||
        locator.start_line > INT32_MAX || locator.start_column == 0 ||
        locator.start_column > INT32_MAX || locator.end_line == 0 || locator.end_line > INT32_MAX ||
        locator.end_column == 0 || locator.end_column > INT32_MAX)
        return false;
    return locator.end_line > locator.start_line ||
           (locator.end_line == locator.start_line && locator.end_column > locator.start_column);
}

static bool function_locator_is_valid(XrProgramSemanticSourceLocator locator) {
    return locator_is_valid(locator);
}

static XrStableId derive_source_callsite_identity(const XrProgramSemanticModuleRecord *module,
                                                  const XrProgramSemanticFunctionRecord *caller,
                                                  XrProgramSemanticSourceLocator locator) {
    XrStableId identity = {{0}};
    (void) xr_source_semantic_callsite_identity(module->source_fingerprint, module->module_identity,
                                                caller->declaration_identity, locator, &identity);
    return identity;
}

static const XrProgramSemanticFunctionRecord *
find_function_record(const XrProgramSemanticClosure *closure, XrStableId id) {
    for (uint32_t i = 0; i < closure->function_count; i++)
        if (stable_id_equal(closure->functions[i].id, id))
            return &closure->functions[i];
    return NULL;
}

static const XrProgramSemanticModuleRecord *
find_module_record(const XrProgramSemanticClosure *closure, XrStableId id) {
    for (uint32_t i = 0; i < closure->module_count; i++)
        if (stable_id_equal(closure->modules[i].module_identity, id))
            return &closure->modules[i];
    return NULL;
}

static bool locator_equal(XrProgramSemanticSourceLocator left,
                          XrProgramSemanticSourceLocator right) {
    return left.kind == right.kind && left.start_line == right.start_line &&
           left.start_column == right.start_column && left.end_line == right.end_line &&
           left.end_column == right.end_column;
}

static bool limits_are_valid(const XrProgramSemanticClosureLimits *limits) {
    return limits && limits->max_modules > 0 &&
           limits->max_modules <= XR_PROGRAM_SEMANTIC_CLOSURE_MAX_MODULES &&
           limits->max_dependencies <= XR_PROGRAM_SEMANTIC_CLOSURE_MAX_DEPENDENCIES &&
           limits->max_types <= XR_PROGRAM_SEMANTIC_CLOSURE_MAX_TYPES &&
           limits->max_type_fields <= XR_PROGRAM_SEMANTIC_CLOSURE_MAX_TYPE_FIELDS &&
           limits->max_functions > 0 &&
           limits->max_functions <= XR_PROGRAM_SEMANTIC_CLOSURE_MAX_FUNCTIONS &&
           limits->max_function_parameters <= XR_PROGRAM_SEMANTIC_CLOSURE_MAX_FUNCTION_PARAMETERS &&
           limits->max_calls <= XR_PROGRAM_SEMANTIC_CLOSURE_MAX_CALLS;
}

static bool grow_table(void **storage, uint32_t *capacity, uint32_t limit, size_t element_size) {
    if (!storage || !capacity || *capacity >= limit || element_size == 0)
        return false;
    uint32_t next = *capacity ? *capacity * 2u : 4u;
    if (next < *capacity || next > limit)
        next = limit;
    if ((size_t) next > SIZE_MAX / element_size)
        return false;
    void *resized = xr_realloc(*storage, (size_t) next * element_size);
    if (!resized)
        return false;
    *storage = resized;
    *capacity = next;
    return true;
}

static bool collecting(const XrProgramSemanticClosure *closure) {
    return closure && closure->state == XR_PROGRAM_SEMANTIC_CLOSURE_COLLECTING;
}

static void begin_mutation(XrProgramSemanticClosure *closure) {
    if (closure)
        closure->failure_kind = XR_PROGRAM_SEMANTIC_CLOSURE_FAILURE_INVALID;
}

static bool resource_failure(XrProgramSemanticClosure *closure, char *error, size_t error_size,
                             const char *detail) {
    if (closure)
        closure->failure_kind = XR_PROGRAM_SEMANTIC_CLOSURE_FAILURE_RESOURCE;
    return fail(error, error_size, "XR_EXEC_5003", detail);
}

static void mutation_succeeded(XrProgramSemanticClosure *closure) {
    if (closure)
        closure->failure_kind = XR_PROGRAM_SEMANTIC_CLOSURE_FAILURE_NONE;
}

bool xr_program_semantic_closure_create(const XrProgramSemanticClosureLimits *limits,
                                        XrFingerprint policy_fingerprint,
                                        XrProgramSemanticClosure **out, char *error,
                                        size_t error_size) {
    if (out)
        *out = NULL;
    if (!out || !limits_are_valid(limits) || fingerprint_is_zero(policy_fingerprint))
        return fail(error, error_size, "XR_SEM_0019",
                    "program semantic closure requires exact policy and hard budgets");
    XrProgramSemanticClosure *closure = (XrProgramSemanticClosure *) xr_calloc(1, sizeof(*closure));
    if (!closure)
        return fail(error, error_size, "XR_EXEC_5003",
                    "program semantic closure allocation failed");
    atomic_init(&closure->references, 1);
    closure->schema = XR_PROGRAM_SEMANTIC_CLOSURE_SCHEMA_VERSION;
    closure->family = XR_PROGRAM_SEMANTIC_FAMILY_GENERAL;
    closure->limits = *limits;
    closure->policy_fingerprint = policy_fingerprint;
    closure->state = XR_PROGRAM_SEMANTIC_CLOSURE_COLLECTING;
    *out = closure;
    return true;
}

bool xr_program_semantic_closure_set_family(XrProgramSemanticClosure *closure,
                                            XrProgramSemanticFamily family, char *error,
                                            size_t error_size) {
    begin_mutation(closure);
    if (!collecting(closure) || family <= 0 || family >= XR_PROGRAM_SEMANTIC_FAMILY_COUNT)
        return fail(error, error_size, "XR_SEM_0019", "program semantic family is invalid");
    closure->family = (uint32_t) family;
    mutation_succeeded(closure);
    return true;
}

XrProgramSemanticClosure *xr_program_semantic_closure_retain(XrProgramSemanticClosure *closure) {
    if (!closure || closure->state != XR_PROGRAM_SEMANTIC_CLOSURE_FROZEN || !closure->verified)
        return NULL;
    uint_least32_t references = atomic_load_explicit(&closure->references, memory_order_relaxed);
    while (references != 0 && references != UINT_LEAST32_MAX) {
        if (atomic_compare_exchange_weak_explicit(&closure->references, &references,
                                                  references + 1u, memory_order_relaxed,
                                                  memory_order_relaxed))
            return closure;
    }
    return NULL;
}

void xr_program_semantic_closure_free(XrProgramSemanticClosure *closure) {
    if (!closure)
        return;
    if (atomic_fetch_sub_explicit(&closure->references, 1, memory_order_acq_rel) != 1)
        return;
    xr_free(closure->modules);
    xr_free(closure->dependencies);
    xr_free(closure->types);
    xr_free(closure->type_fields);
    xr_free(closure->functions);
    xr_free(closure->function_parameters);
    xr_free(closure->calls);
    memset(closure, 0, sizeof(*closure));
    xr_free(closure);
}

XrProgramSemanticClosureFailureKind
xr_program_semantic_closure_failure_kind(const XrProgramSemanticClosure *closure) {
    return closure ? (XrProgramSemanticClosureFailureKind) closure->failure_kind
                   : XR_PROGRAM_SEMANTIC_CLOSURE_FAILURE_INVALID;
}

bool xr_program_semantic_closure_add_module(XrProgramSemanticClosure *closure,
                                            const XrProgramSemanticModuleInput *input, char *error,
                                            size_t error_size) {
    begin_mutation(closure);
    if (!collecting(closure) || !input || stable_id_is_zero(input->module_identity) ||
        fingerprint_is_zero(input->module_authority_fingerprint) ||
        fingerprint_is_zero(input->source_fingerprint) ||
        fingerprint_is_zero(input->export_fingerprint))
        return fail(error, error_size, "XR_SEM_0019", "program module authority is incomplete");
    for (uint32_t i = 0; i < closure->module_count; i++)
        if (stable_id_equal(closure->modules[i].module_identity, input->module_identity))
            return fail(error, error_size, "XR_SEM_0019", "program module authority is duplicated");
    if (closure->module_count == closure->module_capacity &&
        !grow_table((void **) &closure->modules, &closure->module_capacity,
                    closure->limits.max_modules, sizeof(*closure->modules)))
        return resource_failure(closure, error, error_size, "program module budget is exhausted");
    closure->modules[closure->module_count++] = (XrProgramSemanticModuleRecord) {
        .module_identity = input->module_identity,
        .module_authority_fingerprint = input->module_authority_fingerprint,
        .source_fingerprint = input->source_fingerprint,
        .export_fingerprint = input->export_fingerprint,
    };
    mutation_succeeded(closure);
    return true;
}

bool xr_program_semantic_closure_add_dependency(XrProgramSemanticClosure *closure,
                                                const XrProgramSemanticDependencyInput *input,
                                                char *error, size_t error_size) {
    begin_mutation(closure);
    if (!collecting(closure) || !input || stable_id_is_zero(input->source_module) ||
        stable_id_is_zero(input->dependency_module) ||
        stable_id_equal(input->source_module, input->dependency_module) ||
        fingerprint_is_zero(input->contract_fingerprint))
        return fail(error, error_size, "XR_SEM_0019", "program dependency authority is incomplete");
    for (uint32_t i = 0; i < closure->dependency_count; i++) {
        const XrProgramSemanticDependencyRecord *row = &closure->dependencies[i];
        if (stable_id_equal(row->source_module, input->source_module) &&
            stable_id_equal(row->dependency_module, input->dependency_module))
            return fail(error, error_size, "XR_SEM_0019",
                        "program dependency authority is duplicated");
    }
    if (closure->dependency_count == closure->dependency_capacity &&
        !grow_table((void **) &closure->dependencies, &closure->dependency_capacity,
                    closure->limits.max_dependencies, sizeof(*closure->dependencies)))
        return resource_failure(closure, error, error_size,
                                "program dependency budget is exhausted");
    closure->dependencies[closure->dependency_count++] = (XrProgramSemanticDependencyRecord) {
        .source_module = input->source_module,
        .dependency_module = input->dependency_module,
        .contract_fingerprint = input->contract_fingerprint,
    };
    mutation_succeeded(closure);
    return true;
}

bool xr_program_semantic_closure_add_type(XrProgramSemanticClosure *closure,
                                          const XrProgramSemanticTypeInput *input,
                                          XrStableId *type_identity, char *error,
                                          size_t error_size) {
    begin_mutation(closure);
    if (type_identity)
        memset(type_identity, 0, sizeof(*type_identity));
    bool opaque = input && input->kind == XR_PROGRAM_SEMANTIC_TYPE_OPAQUE;
    bool typed = input && input->kind > XR_PROGRAM_SEMANTIC_TYPE_OPAQUE &&
                 input->kind < XR_PROGRAM_SEMANTIC_TYPE_KIND_COUNT;
    if (!collecting(closure) || !input || !type_identity ||
        stable_id_is_zero(input->module_identity) ||
        stable_id_is_zero(input->declaration_identity) ||
        stable_id_is_zero(input->concrete_instance_identity) || (!opaque && !typed) ||
        (opaque && (input->fields || input->field_count || input->exact_scalar || input->flags ||
                    input->reserved || fingerprint_is_zero(input->shape_fingerprint) ||
                    fingerprint_is_zero(input->ownership_fingerprint))) ||
        (typed && !typed_type_input_is_valid(closure, input)))
        return fail(error, error_size, "XR_SEM_0019", "concrete type authority is incomplete");
    XrProgramSemanticTypeInput normalized = *input;
    if (typed)
        derive_typed_type_fingerprints(input, &normalized.shape_fingerprint,
                                       &normalized.ownership_fingerprint);
    XrStableId id = derive_type_identity(closure->policy_fingerprint, &normalized);
    for (uint32_t i = 0; i < closure->type_count; i++) {
        const XrProgramSemanticTypeRecord *row = &closure->types[i];
        if (stable_id_equal(row->id, id) ||
            (stable_id_equal(row->module_identity, input->module_identity) &&
             stable_id_equal(row->declaration_identity, input->declaration_identity) &&
             stable_id_equal(row->concrete_instance_identity, input->concrete_instance_identity)))
            return fail(error, error_size, "XR_SEM_0019",
                        "concrete type authority is duplicated or conflicting");
    }
    if (closure->type_count == closure->type_capacity &&
        !grow_table((void **) &closure->types, &closure->type_capacity, closure->limits.max_types,
                    sizeof(*closure->types)))
        return resource_failure(closure, error, error_size, "concrete type budget is exhausted");
    if (input->field_count &&
        closure->type_field_count + input->field_count > closure->type_field_capacity &&
        !grow_table((void **) &closure->type_fields, &closure->type_field_capacity,
                    closure->limits.max_type_fields, sizeof(*closure->type_fields)))
        return resource_failure(closure, error, error_size,
                                "concrete type field budget is exhausted");
    while (input->field_count &&
           closure->type_field_count + input->field_count > closure->type_field_capacity) {
        if (!grow_table((void **) &closure->type_fields, &closure->type_field_capacity,
                        closure->limits.max_type_fields, sizeof(*closure->type_fields)))
            return resource_failure(closure, error, error_size,
                                    "concrete type field budget is exhausted");
    }
    uint32_t field_begin = closure->type_field_count;
    for (uint32_t i = 0; i < input->field_count; i++) {
        closure->type_fields[closure->type_field_count++] = (XrProgramSemanticTypeFieldRecord) {
            .owner_type = id,
            .field_type = input->fields[i].field_type,
            .declaration_ordinal = input->fields[i].declaration_ordinal,
        };
    }
    closure->types[closure->type_count++] = (XrProgramSemanticTypeRecord) {
        .id = id,
        .module_identity = input->module_identity,
        .declaration_identity = input->declaration_identity,
        .concrete_instance_identity = input->concrete_instance_identity,
        .declaration_locator = input->declaration_locator,
        .shape_fingerprint = normalized.shape_fingerprint,
        .ownership_fingerprint = normalized.ownership_fingerprint,
        .field_begin = field_begin,
        .field_count = input->field_count,
        .kind = input->kind,
        .exact_scalar = input->exact_scalar,
        .flags = input->flags,
    };
    *type_identity = id;
    mutation_succeeded(closure);
    return true;
}

bool xr_program_semantic_closure_add_function(XrProgramSemanticClosure *closure,
                                              const XrProgramSemanticFunctionInput *input,
                                              XrStableId *function_identity, char *error,
                                              size_t error_size) {
    begin_mutation(closure);
    if (function_identity)
        memset(function_identity, 0, sizeof(*function_identity));
    if (!collecting(closure) || !input || !function_identity ||
        stable_id_is_zero(input->module_identity) ||
        stable_id_is_zero(input->declaration_identity) ||
        stable_id_is_zero(input->concrete_instance_identity) ||
        !function_locator_is_valid(input->declaration_locator) ||
        fingerprint_is_zero(input->signature_fingerprint) ||
        fingerprint_is_zero(input->effect_fingerprint) ||
        (input->flags &
         ~(XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY | XR_PROGRAM_SEMANTIC_FUNCTION_EXPORTED)) != 0 ||
        memcmp(input->reserved, (uint8_t[7]) {0}, sizeof(input->reserved)) != 0 ||
        (closure->family == XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_AGGREGATE_DIRECT_CALL
             ? (stable_id_is_zero(input->return_type) || input->parameter_count > 1 ||
                input->parameter_count > closure->limits.max_function_parameters ||
                (input->parameter_count && !input->parameters))
             : (!stable_id_is_zero(input->return_type) || input->parameter_count != 0 ||
                input->parameters)))
        return fail(error, error_size, "XR_SEM_0019", "concrete function authority is incomplete");
    for (uint32_t i = 0; i < input->parameter_count; i++) {
        const XrProgramSemanticFunctionParameterInput *parameter = &input->parameters[i];
        if (stable_id_is_zero(parameter->type) || parameter->declaration_ordinal != i ||
            parameter->mode != XR_PARAM_READ ||
            memcmp(parameter->reserved, (uint8_t[3]) {0}, sizeof(parameter->reserved)) != 0)
            return fail(error, error_size, "XR_SEM_0019",
                        "concrete function parameter authority is invalid");
    }
    XrStableId id = derive_function_identity(closure->policy_fingerprint, input);
    for (uint32_t i = 0; i < closure->function_count; i++) {
        const XrProgramSemanticFunctionRecord *row = &closure->functions[i];
        if (stable_id_equal(row->id, id) ||
            (stable_id_equal(row->module_identity, input->module_identity) &&
             stable_id_equal(row->declaration_identity, input->declaration_identity) &&
             stable_id_equal(row->concrete_instance_identity, input->concrete_instance_identity)))
            return fail(error, error_size, "XR_SEM_0019",
                        "concrete function authority is duplicated or conflicting");
        if (stable_id_equal(row->module_identity, input->module_identity) &&
            locator_equal(row->declaration_locator, input->declaration_locator))
            return fail(error, error_size, "XR_SEM_0019",
                        "concrete function declaration locator is duplicated");
    }
    if (closure->function_count == closure->function_capacity &&
        !grow_table((void **) &closure->functions, &closure->function_capacity,
                    closure->limits.max_functions, sizeof(*closure->functions)))
        return resource_failure(closure, error, error_size,
                                "concrete function budget is exhausted");
    while (closure->function_parameter_count + input->parameter_count >
           closure->function_parameter_capacity) {
        if (!grow_table(
                (void **) &closure->function_parameters, &closure->function_parameter_capacity,
                closure->limits.max_function_parameters, sizeof(*closure->function_parameters)))
            return resource_failure(closure, error, error_size,
                                    "concrete function parameter budget is exhausted");
    }
    uint32_t parameter_begin = closure->function_parameter_count;
    for (uint32_t i = 0; i < input->parameter_count; i++)
        closure->function_parameters[closure->function_parameter_count++] =
            (XrProgramSemanticFunctionParameterRecord) {
                .owner_function = id,
                .type = input->parameters[i].type,
                .declaration_ordinal = input->parameters[i].declaration_ordinal,
                .mode = input->parameters[i].mode,
            };
    closure->functions[closure->function_count++] = (XrProgramSemanticFunctionRecord) {
        .id = id,
        .module_identity = input->module_identity,
        .declaration_identity = input->declaration_identity,
        .concrete_instance_identity = input->concrete_instance_identity,
        .declaration_locator = input->declaration_locator,
        .signature_fingerprint = input->signature_fingerprint,
        .effect_fingerprint = input->effect_fingerprint,
        .return_type = input->return_type,
        .parameter_begin = parameter_begin,
        .parameter_count = input->parameter_count,
        .capability_mask = input->capability_mask,
        .flags = input->flags,
    };
    *function_identity = id;
    mutation_succeeded(closure);
    return true;
}

bool xr_program_semantic_closure_add_call(XrProgramSemanticClosure *closure,
                                          const XrProgramSemanticCallInput *input,
                                          XrStableId *call_identity, char *error,
                                          size_t error_size) {
    begin_mutation(closure);
    if (call_identity)
        memset(call_identity, 0, sizeof(*call_identity));
    if (!collecting(closure) || !input || !call_identity ||
        stable_id_is_zero(input->callsite_identity) || stable_id_is_zero(input->caller_function) ||
        stable_id_is_zero(input->callee_function) || !locator_is_valid(input->locator) ||
        fingerprint_is_zero(input->contract_fingerprint))
        return fail(error, error_size, "XR_SEM_0019", "resolved call authority is incomplete");
    const XrProgramSemanticFunctionRecord *caller =
        find_function_record(closure, input->caller_function);
    const XrProgramSemanticFunctionRecord *callee =
        find_function_record(closure, input->callee_function);
    const XrProgramSemanticModuleRecord *module =
        caller ? find_module_record(closure, caller->module_identity) : NULL;
    if (!caller || !callee || !module ||
        !stable_id_equal(input->callsite_identity,
                         derive_source_callsite_identity(module, caller, input->locator)))
        return fail(error, error_size, "XR_SEM_0019",
                    "resolved call locator does not match its stable callsite");
    XrStableId id = derive_call_identity(closure->policy_fingerprint, input);
    for (uint32_t i = 0; i < closure->call_count; i++) {
        const XrProgramSemanticCallRecord *row = &closure->calls[i];
        const XrProgramSemanticFunctionRecord *existing_caller =
            find_function_record(closure, row->caller_function);
        if (stable_id_equal(row->id, id) ||
            stable_id_equal(row->callsite_identity, input->callsite_identity))
            return fail(error, error_size, "XR_SEM_0019",
                        "resolved call authority is duplicated or conflicting");
        if (existing_caller &&
            stable_id_equal(existing_caller->module_identity, caller->module_identity) &&
            locator_equal(row->locator, input->locator))
            return fail(error, error_size, "XR_SEM_0019",
                        "resolved call source locator is duplicated");
    }
    if (closure->call_count == closure->call_capacity &&
        !grow_table((void **) &closure->calls, &closure->call_capacity, closure->limits.max_calls,
                    sizeof(*closure->calls)))
        return resource_failure(closure, error, error_size, "resolved call budget is exhausted");
    closure->calls[closure->call_count++] = (XrProgramSemanticCallRecord) {
        .id = id,
        .callsite_identity = input->callsite_identity,
        .locator = input->locator,
        .caller_function = input->caller_function,
        .callee_function = input->callee_function,
        .contract_fingerprint = input->contract_fingerprint,
    };
    *call_identity = id;
    mutation_succeeded(closure);
    return true;
}

static int compare_stable_id(XrStableId left, XrStableId right) {
    return memcmp(left.bytes, right.bytes, sizeof(left.bytes));
}

static int compare_modules(const void *left, const void *right) {
    const XrProgramSemanticModuleRecord *a = (const XrProgramSemanticModuleRecord *) left;
    const XrProgramSemanticModuleRecord *b = (const XrProgramSemanticModuleRecord *) right;
    return compare_stable_id(a->module_identity, b->module_identity);
}

static int compare_dependencies(const void *left, const void *right) {
    const XrProgramSemanticDependencyRecord *a = (const XrProgramSemanticDependencyRecord *) left;
    const XrProgramSemanticDependencyRecord *b = (const XrProgramSemanticDependencyRecord *) right;
    int result = compare_stable_id(a->source_module, b->source_module);
    return result ? result : compare_stable_id(a->dependency_module, b->dependency_module);
}

static int compare_types(const void *left, const void *right) {
    const XrProgramSemanticTypeRecord *a = (const XrProgramSemanticTypeRecord *) left;
    const XrProgramSemanticTypeRecord *b = (const XrProgramSemanticTypeRecord *) right;
    return compare_stable_id(a->id, b->id);
}

static int compare_type_fields(const void *left, const void *right) {
    const XrProgramSemanticTypeFieldRecord *a = (const XrProgramSemanticTypeFieldRecord *) left;
    const XrProgramSemanticTypeFieldRecord *b = (const XrProgramSemanticTypeFieldRecord *) right;
    int owner = compare_stable_id(a->owner_type, b->owner_type);
    if (owner)
        return owner;
    return a->declaration_ordinal < b->declaration_ordinal
               ? -1
               : a->declaration_ordinal > b->declaration_ordinal;
}

static int compare_functions(const void *left, const void *right) {
    const XrProgramSemanticFunctionRecord *a = (const XrProgramSemanticFunctionRecord *) left;
    const XrProgramSemanticFunctionRecord *b = (const XrProgramSemanticFunctionRecord *) right;
    return compare_stable_id(a->id, b->id);
}

static int compare_function_parameters(const void *left, const void *right) {
    const XrProgramSemanticFunctionParameterRecord *a =
        (const XrProgramSemanticFunctionParameterRecord *) left;
    const XrProgramSemanticFunctionParameterRecord *b =
        (const XrProgramSemanticFunctionParameterRecord *) right;
    int owner = compare_stable_id(a->owner_function, b->owner_function);
    if (owner)
        return owner;
    return a->declaration_ordinal < b->declaration_ordinal
               ? -1
               : a->declaration_ordinal > b->declaration_ordinal;
}

static int compare_calls(const void *left, const void *right) {
    const XrProgramSemanticCallRecord *a = (const XrProgramSemanticCallRecord *) left;
    const XrProgramSemanticCallRecord *b = (const XrProgramSemanticCallRecord *) right;
    return compare_stable_id(a->id, b->id);
}

static void canonicalize_tables(XrProgramSemanticClosure *closure) {
    if (closure->module_count > 1u)
        qsort(closure->modules, closure->module_count, sizeof(*closure->modules), compare_modules);
    if (closure->dependency_count > 1u)
        qsort(closure->dependencies, closure->dependency_count, sizeof(*closure->dependencies),
              compare_dependencies);
    if (closure->type_count > 1u)
        qsort(closure->types, closure->type_count, sizeof(*closure->types), compare_types);
    if (closure->type_field_count > 1u)
        qsort(closure->type_fields, closure->type_field_count, sizeof(*closure->type_fields),
              compare_type_fields);
    uint32_t field = 0;
    for (uint32_t i = 0; i < closure->type_count; i++) {
        closure->types[i].field_begin = field;
        while (field < closure->type_field_count &&
               stable_id_equal(closure->type_fields[field].owner_type, closure->types[i].id))
            field++;
        closure->types[i].field_count = field - closure->types[i].field_begin;
    }
    if (closure->function_count > 1u)
        qsort(closure->functions, closure->function_count, sizeof(*closure->functions),
              compare_functions);
    if (closure->function_parameter_count > 1u)
        qsort(closure->function_parameters, closure->function_parameter_count,
              sizeof(*closure->function_parameters), compare_function_parameters);
    uint32_t parameter = 0;
    for (uint32_t i = 0; i < closure->function_count; i++) {
        closure->functions[i].parameter_begin = parameter;
        while (parameter < closure->function_parameter_count &&
               stable_id_equal(closure->function_parameters[parameter].owner_function,
                               closure->functions[i].id))
            parameter++;
        closure->functions[i].parameter_count = parameter - closure->functions[i].parameter_begin;
    }
    if (closure->call_count > 1u)
        qsort(closure->calls, closure->call_count, sizeof(*closure->calls), compare_calls);
}

static void compute_closure_fingerprint(const XrProgramSemanticClosure *closure,
                                        XrFingerprint *out) {
    static const uint8_t domain[] = "xray-program-semantic-closure-v4\0";
    XrSHA256Context context;
    xr_sha256_init(&context);
    xr_sha256_update(&context, domain, sizeof(domain) - 1u);
    hash_u32(&context, closure->schema);
    hash_u32(&context, closure->family);
    hash_fingerprint(&context, closure->policy_fingerprint);
    hash_u32(&context, closure->module_count);
    hash_u32(&context, closure->dependency_count);
    hash_u32(&context, closure->type_count);
    hash_u32(&context, closure->type_field_count);
    hash_u32(&context, closure->function_count);
    hash_u32(&context, closure->function_parameter_count);
    hash_u32(&context, closure->call_count);
    for (uint32_t i = 0; i < closure->module_count; i++) {
        hash_stable_id(&context, closure->modules[i].module_identity);
        hash_fingerprint(&context, closure->modules[i].module_authority_fingerprint);
        hash_fingerprint(&context, closure->modules[i].source_fingerprint);
        hash_fingerprint(&context, closure->modules[i].export_fingerprint);
    }
    for (uint32_t i = 0; i < closure->dependency_count; i++) {
        hash_stable_id(&context, closure->dependencies[i].source_module);
        hash_stable_id(&context, closure->dependencies[i].dependency_module);
        hash_fingerprint(&context, closure->dependencies[i].contract_fingerprint);
    }
    for (uint32_t i = 0; i < closure->type_count; i++) {
        hash_stable_id(&context, closure->types[i].id);
        hash_stable_id(&context, closure->types[i].module_identity);
        hash_stable_id(&context, closure->types[i].declaration_identity);
        hash_stable_id(&context, closure->types[i].concrete_instance_identity);
        hash_u32(&context, closure->types[i].declaration_locator.kind);
        hash_u32(&context, closure->types[i].declaration_locator.start_line);
        hash_u32(&context, closure->types[i].declaration_locator.start_column);
        hash_u32(&context, closure->types[i].declaration_locator.end_line);
        hash_u32(&context, closure->types[i].declaration_locator.end_column);
        hash_fingerprint(&context, closure->types[i].shape_fingerprint);
        hash_fingerprint(&context, closure->types[i].ownership_fingerprint);
        hash_u32(&context, closure->types[i].field_begin);
        hash_u32(&context, closure->types[i].field_count);
        hash_u32(&context, closure->types[i].kind);
        hash_u32(&context, closure->types[i].exact_scalar);
        hash_u32(&context, closure->types[i].flags);
    }
    for (uint32_t i = 0; i < closure->type_field_count; i++) {
        hash_stable_id(&context, closure->type_fields[i].owner_type);
        hash_stable_id(&context, closure->type_fields[i].field_type);
        hash_u32(&context, closure->type_fields[i].declaration_ordinal);
    }
    for (uint32_t i = 0; i < closure->function_count; i++) {
        hash_stable_id(&context, closure->functions[i].id);
        hash_stable_id(&context, closure->functions[i].module_identity);
        hash_stable_id(&context, closure->functions[i].declaration_identity);
        hash_stable_id(&context, closure->functions[i].concrete_instance_identity);
        hash_u32(&context, closure->functions[i].declaration_locator.kind);
        hash_u32(&context, closure->functions[i].declaration_locator.start_line);
        hash_u32(&context, closure->functions[i].declaration_locator.start_column);
        hash_u32(&context, closure->functions[i].declaration_locator.end_line);
        hash_u32(&context, closure->functions[i].declaration_locator.end_column);
        hash_fingerprint(&context, closure->functions[i].signature_fingerprint);
        hash_fingerprint(&context, closure->functions[i].effect_fingerprint);
        hash_stable_id(&context, closure->functions[i].return_type);
        hash_u32(&context, closure->functions[i].parameter_begin);
        hash_u32(&context, closure->functions[i].parameter_count);
        hash_u64(&context, closure->functions[i].capability_mask);
        xr_sha256_update(&context, &closure->functions[i].flags, 1u);
    }
    for (uint32_t i = 0; i < closure->function_parameter_count; i++) {
        hash_stable_id(&context, closure->function_parameters[i].owner_function);
        hash_stable_id(&context, closure->function_parameters[i].type);
        hash_u32(&context, closure->function_parameters[i].declaration_ordinal);
        hash_u32(&context, closure->function_parameters[i].mode);
    }
    for (uint32_t i = 0; i < closure->call_count; i++) {
        hash_stable_id(&context, closure->calls[i].id);
        hash_stable_id(&context, closure->calls[i].callsite_identity);
        hash_u32(&context, closure->calls[i].locator.kind);
        hash_u32(&context, closure->calls[i].locator.start_line);
        hash_u32(&context, closure->calls[i].locator.start_column);
        hash_u32(&context, closure->calls[i].locator.end_line);
        hash_u32(&context, closure->calls[i].locator.end_column);
        hash_stable_id(&context, closure->calls[i].caller_function);
        hash_stable_id(&context, closure->calls[i].callee_function);
        hash_fingerprint(&context, closure->calls[i].contract_fingerprint);
    }
    xr_sha256_final(&context, out->bytes);
}

static XrGenerationClosureId compute_generation_id(XrFingerprint fingerprint) {
    static const uint8_t domain[] = "xray-generation-closure-id-v1\0";
    XrSHA256Context context;
    uint8_t digest[XR_FINGERPRINT_BYTES];
    XrGenerationClosureId id;
    xr_sha256_init(&context);
    xr_sha256_update(&context, domain, sizeof(domain) - 1u);
    hash_u32(&context, XR_PROGRAM_SEMANTIC_CLOSURE_SCHEMA_VERSION);
    hash_fingerprint(&context, fingerprint);
    xr_sha256_final(&context, digest);
    memcpy(id.bytes, digest, sizeof(id.bytes));
    return id;
}

bool xr_program_semantic_closure_freeze(XrProgramSemanticClosure *closure, char *error,
                                        size_t error_size) {
    begin_mutation(closure);
    if (!collecting(closure))
        return fail(error, error_size, "XR_SEM_0019", "program semantic closure is not collecting");
    canonicalize_tables(closure);
    compute_closure_fingerprint(closure, &closure->fingerprint);
    closure->generation_id = compute_generation_id(closure->fingerprint);
    closure->state = XR_PROGRAM_SEMANTIC_CLOSURE_VERIFYING;
    mutation_succeeded(closure);
    if (!xr_program_semantic_closure_verify(closure, error, error_size)) {
        closure->failure_kind = XR_PROGRAM_SEMANTIC_CLOSURE_FAILURE_INVALID;
        closure->state = XR_PROGRAM_SEMANTIC_CLOSURE_FAILED;
        memset(&closure->fingerprint, 0, sizeof(closure->fingerprint));
        memset(&closure->generation_id, 0, sizeof(closure->generation_id));
        return false;
    }
    closure->verified = 1u;
    closure->state = XR_PROGRAM_SEMANTIC_CLOSURE_FROZEN;
    return true;
}

bool xr_program_semantic_closure_is_frozen(const XrProgramSemanticClosure *closure) {
    return closure && closure->state == XR_PROGRAM_SEMANTIC_CLOSURE_FROZEN;
}

bool xr_program_semantic_closure_is_verified(const XrProgramSemanticClosure *closure) {
    return xr_program_semantic_closure_is_frozen(closure) && closure->verified != 0;
}

uint32_t xr_program_semantic_closure_schema(const XrProgramSemanticClosure *closure) {
    return closure ? closure->schema : 0;
}

XrFingerprint xr_program_semantic_closure_fingerprint(const XrProgramSemanticClosure *closure) {
    return xr_program_semantic_closure_is_frozen(closure) ? closure->fingerprint
                                                          : (XrFingerprint) {{0}};
}

XrGenerationClosureId
xr_program_semantic_closure_generation_id(const XrProgramSemanticClosure *closure) {
    return xr_program_semantic_closure_is_frozen(closure) ? closure->generation_id
                                                          : (XrGenerationClosureId) {{0}};
}

bool xr_generation_closure_id_equal(XrGenerationClosureId left, XrGenerationClosureId right) {
    return memcmp(left.bytes, right.bytes, sizeof(left.bytes)) == 0;
}

size_t xr_program_semantic_closure_module_count(const XrProgramSemanticClosure *closure) {
    return xr_program_semantic_closure_is_frozen(closure) ? closure->module_count : 0;
}

size_t xr_program_semantic_closure_dependency_count(const XrProgramSemanticClosure *closure) {
    return xr_program_semantic_closure_is_frozen(closure) ? closure->dependency_count : 0;
}

size_t xr_program_semantic_closure_type_count(const XrProgramSemanticClosure *closure) {
    return xr_program_semantic_closure_is_frozen(closure) ? closure->type_count : 0;
}

XrProgramSemanticFamily
xr_program_semantic_closure_family(const XrProgramSemanticClosure *closure) {
    return xr_program_semantic_closure_is_frozen(closure)
               ? (XrProgramSemanticFamily) closure->family
               : 0;
}

size_t xr_program_semantic_closure_type_field_count(const XrProgramSemanticClosure *closure) {
    return xr_program_semantic_closure_is_frozen(closure) ? closure->type_field_count : 0;
}

size_t xr_program_semantic_closure_function_count(const XrProgramSemanticClosure *closure) {
    return xr_program_semantic_closure_is_frozen(closure) ? closure->function_count : 0;
}

size_t
xr_program_semantic_closure_function_parameter_count(const XrProgramSemanticClosure *closure) {
    return xr_program_semantic_closure_is_frozen(closure) ? closure->function_parameter_count : 0;
}

size_t xr_program_semantic_closure_call_count(const XrProgramSemanticClosure *closure) {
    return xr_program_semantic_closure_is_frozen(closure) ? closure->call_count : 0;
}

const XrProgramSemanticModuleRecord *
xr_program_semantic_closure_module(const XrProgramSemanticClosure *closure, uint32_t index) {
    return xr_program_semantic_closure_is_frozen(closure) && index < closure->module_count
               ? &closure->modules[index]
               : NULL;
}

const XrProgramSemanticDependencyRecord *
xr_program_semantic_closure_dependency(const XrProgramSemanticClosure *closure, uint32_t index) {
    return xr_program_semantic_closure_is_frozen(closure) && index < closure->dependency_count
               ? &closure->dependencies[index]
               : NULL;
}

const XrProgramSemanticTypeRecord *
xr_program_semantic_closure_type(const XrProgramSemanticClosure *closure, uint32_t index) {
    return xr_program_semantic_closure_is_frozen(closure) && index < closure->type_count
               ? &closure->types[index]
               : NULL;
}

const XrProgramSemanticTypeFieldRecord *
xr_program_semantic_closure_type_field(const XrProgramSemanticClosure *closure, uint32_t index) {
    return xr_program_semantic_closure_is_frozen(closure) && index < closure->type_field_count
               ? &closure->type_fields[index]
               : NULL;
}

const XrProgramSemanticFunctionRecord *
xr_program_semantic_closure_function(const XrProgramSemanticClosure *closure, uint32_t index) {
    return xr_program_semantic_closure_is_frozen(closure) && index < closure->function_count
               ? &closure->functions[index]
               : NULL;
}

const XrProgramSemanticFunctionParameterRecord *
xr_program_semantic_closure_function_parameter(const XrProgramSemanticClosure *closure,
                                               uint32_t index) {
    return xr_program_semantic_closure_is_frozen(closure) &&
                   index < closure->function_parameter_count
               ? &closure->function_parameters[index]
               : NULL;
}

const XrProgramSemanticCallRecord *
xr_program_semantic_closure_call(const XrProgramSemanticClosure *closure, uint32_t index) {
    return xr_program_semantic_closure_is_frozen(closure) && index < closure->call_count
               ? &closure->calls[index]
               : NULL;
}
