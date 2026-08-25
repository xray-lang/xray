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
#include "../../base/xmalloc.h"
#include "../../base/xsha256.h"
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

static XrStableId finish_stable_id(XrSHA256Context *context) {
    uint8_t digest[XR_FINGERPRINT_BYTES];
    XrStableId id;
    xr_sha256_final(context, digest);
    memcpy(id.bytes, digest, sizeof(id.bytes));
    return id;
}

static XrStableId derive_type_identity(XrFingerprint policy_fingerprint,
                                       const XrProgramSemanticTypeInput *input) {
    static const uint8_t domain[] = "xray-program-semantic-type-v1\0";
    XrSHA256Context context;
    xr_sha256_init(&context);
    xr_sha256_update(&context, domain, sizeof(domain) - 1u);
    hash_u32(&context, XR_PROGRAM_SEMANTIC_CLOSURE_SCHEMA_VERSION);
    hash_fingerprint(&context, policy_fingerprint);
    hash_stable_id(&context, input->module_identity);
    hash_stable_id(&context, input->declaration_identity);
    hash_stable_id(&context, input->concrete_instance_identity);
    hash_fingerprint(&context, input->shape_fingerprint);
    hash_fingerprint(&context, input->ownership_fingerprint);
    return finish_stable_id(&context);
}

static XrStableId derive_function_identity(XrFingerprint policy_fingerprint,
                                           const XrProgramSemanticFunctionInput *input) {
    static const uint8_t domain[] = "xray-program-semantic-function-v1\0";
    XrSHA256Context context;
    xr_sha256_init(&context);
    xr_sha256_update(&context, domain, sizeof(domain) - 1u);
    hash_u32(&context, XR_PROGRAM_SEMANTIC_CLOSURE_SCHEMA_VERSION);
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
    hash_u32(&context, XR_PROGRAM_SEMANTIC_CLOSURE_SCHEMA_VERSION);
    hash_fingerprint(&context, policy_fingerprint);
    hash_stable_id(&context, input->callsite_identity);
    hash_stable_id(&context, input->caller_function);
    hash_stable_id(&context, input->callee_function);
    hash_fingerprint(&context, input->contract_fingerprint);
    return finish_stable_id(&context);
}

static bool limits_are_valid(const XrProgramSemanticClosureLimits *limits) {
    return limits && limits->max_modules > 0 &&
           limits->max_modules <= XR_PROGRAM_SEMANTIC_CLOSURE_MAX_MODULES &&
           limits->max_dependencies <= XR_PROGRAM_SEMANTIC_CLOSURE_MAX_DEPENDENCIES &&
           limits->max_types <= XR_PROGRAM_SEMANTIC_CLOSURE_MAX_TYPES &&
           limits->max_functions > 0 &&
           limits->max_functions <= XR_PROGRAM_SEMANTIC_CLOSURE_MAX_FUNCTIONS &&
           limits->max_calls <= XR_PROGRAM_SEMANTIC_CLOSURE_MAX_CALLS;
}

static bool grow_table(void **storage, uint32_t *capacity, uint32_t limit,
                       size_t element_size) {
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

bool xr_program_semantic_closure_create(const XrProgramSemanticClosureLimits *limits,
                                        XrFingerprint policy_fingerprint,
                                        XrProgramSemanticClosure **out, char *error,
                                        size_t error_size) {
    if (out)
        *out = NULL;
    if (!out || !limits_are_valid(limits) || fingerprint_is_zero(policy_fingerprint))
        return fail(error, error_size, "XR_SEM_0019",
                    "program semantic closure requires exact policy and hard budgets");
    XrProgramSemanticClosure *closure =
        (XrProgramSemanticClosure *) xr_calloc(1, sizeof(*closure));
    if (!closure)
        return fail(error, error_size, "XR_EXEC_5003",
                    "program semantic closure allocation failed");
    closure->schema = XR_PROGRAM_SEMANTIC_CLOSURE_SCHEMA_VERSION;
    closure->limits = *limits;
    closure->policy_fingerprint = policy_fingerprint;
    closure->state = XR_PROGRAM_SEMANTIC_CLOSURE_COLLECTING;
    *out = closure;
    return true;
}

void xr_program_semantic_closure_free(XrProgramSemanticClosure *closure) {
    if (!closure)
        return;
    xr_free(closure->modules);
    xr_free(closure->dependencies);
    xr_free(closure->types);
    xr_free(closure->functions);
    xr_free(closure->calls);
    memset(closure, 0, sizeof(*closure));
    xr_free(closure);
}

bool xr_program_semantic_closure_add_module(XrProgramSemanticClosure *closure,
                                            const XrProgramSemanticModuleInput *input,
                                            char *error, size_t error_size) {
    if (!collecting(closure) || !input || stable_id_is_zero(input->module_identity) ||
        fingerprint_is_zero(input->source_fingerprint) ||
        fingerprint_is_zero(input->export_fingerprint))
        return fail(error, error_size, "XR_SEM_0019",
                    "program module authority is incomplete");
    for (uint32_t i = 0; i < closure->module_count; i++)
        if (stable_id_equal(closure->modules[i].module_identity, input->module_identity))
            return fail(error, error_size, "XR_SEM_0019",
                        "program module authority is duplicated");
    if (closure->module_count == closure->module_capacity &&
        !grow_table((void **) &closure->modules, &closure->module_capacity,
                    closure->limits.max_modules, sizeof(*closure->modules)))
        return fail(error, error_size, "XR_EXEC_5003", "program module budget is exhausted");
    closure->modules[closure->module_count++] = (XrProgramSemanticModuleRecord) {
        .module_identity = input->module_identity,
        .source_fingerprint = input->source_fingerprint,
        .export_fingerprint = input->export_fingerprint,
    };
    return true;
}

bool xr_program_semantic_closure_add_dependency(
    XrProgramSemanticClosure *closure, const XrProgramSemanticDependencyInput *input,
    char *error, size_t error_size) {
    if (!collecting(closure) || !input || stable_id_is_zero(input->source_module) ||
        stable_id_is_zero(input->dependency_module) ||
        stable_id_equal(input->source_module, input->dependency_module) ||
        fingerprint_is_zero(input->contract_fingerprint))
        return fail(error, error_size, "XR_SEM_0019",
                    "program dependency authority is incomplete");
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
        return fail(error, error_size, "XR_EXEC_5003",
                    "program dependency budget is exhausted");
    closure->dependencies[closure->dependency_count++] =
        (XrProgramSemanticDependencyRecord) {
            .source_module = input->source_module,
            .dependency_module = input->dependency_module,
            .contract_fingerprint = input->contract_fingerprint,
        };
    return true;
}

bool xr_program_semantic_closure_add_type(XrProgramSemanticClosure *closure,
                                          const XrProgramSemanticTypeInput *input,
                                          XrStableId *type_identity, char *error,
                                          size_t error_size) {
    if (type_identity)
        memset(type_identity, 0, sizeof(*type_identity));
    if (!collecting(closure) || !input || !type_identity ||
        stable_id_is_zero(input->module_identity) ||
        stable_id_is_zero(input->declaration_identity) ||
        stable_id_is_zero(input->concrete_instance_identity) ||
        fingerprint_is_zero(input->shape_fingerprint) ||
        fingerprint_is_zero(input->ownership_fingerprint))
        return fail(error, error_size, "XR_SEM_0019", "concrete type authority is incomplete");
    XrStableId id = derive_type_identity(closure->policy_fingerprint, input);
    for (uint32_t i = 0; i < closure->type_count; i++) {
        const XrProgramSemanticTypeRecord *row = &closure->types[i];
        if (stable_id_equal(row->id, id) ||
            (stable_id_equal(row->module_identity, input->module_identity) &&
             stable_id_equal(row->declaration_identity, input->declaration_identity) &&
             stable_id_equal(row->concrete_instance_identity,
                             input->concrete_instance_identity)))
            return fail(error, error_size, "XR_SEM_0019",
                        "concrete type authority is duplicated or conflicting");
    }
    if (closure->type_count == closure->type_capacity &&
        !grow_table((void **) &closure->types, &closure->type_capacity,
                    closure->limits.max_types, sizeof(*closure->types)))
        return fail(error, error_size, "XR_EXEC_5003", "concrete type budget is exhausted");
    closure->types[closure->type_count++] = (XrProgramSemanticTypeRecord) {
        .id = id,
        .module_identity = input->module_identity,
        .declaration_identity = input->declaration_identity,
        .concrete_instance_identity = input->concrete_instance_identity,
        .shape_fingerprint = input->shape_fingerprint,
        .ownership_fingerprint = input->ownership_fingerprint,
    };
    *type_identity = id;
    return true;
}

bool xr_program_semantic_closure_add_function(
    XrProgramSemanticClosure *closure, const XrProgramSemanticFunctionInput *input,
    XrStableId *function_identity, char *error, size_t error_size) {
    if (function_identity)
        memset(function_identity, 0, sizeof(*function_identity));
    if (!collecting(closure) || !input || !function_identity ||
        stable_id_is_zero(input->module_identity) ||
        stable_id_is_zero(input->declaration_identity) ||
        stable_id_is_zero(input->concrete_instance_identity) ||
        fingerprint_is_zero(input->signature_fingerprint) ||
        fingerprint_is_zero(input->effect_fingerprint) ||
        (input->flags & ~(XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY |
                          XR_PROGRAM_SEMANTIC_FUNCTION_EXPORTED)) != 0 ||
        memcmp(input->reserved, (uint8_t[7]) {0}, sizeof(input->reserved)) != 0)
        return fail(error, error_size, "XR_SEM_0019",
                    "concrete function authority is incomplete");
    XrStableId id = derive_function_identity(closure->policy_fingerprint, input);
    for (uint32_t i = 0; i < closure->function_count; i++) {
        const XrProgramSemanticFunctionRecord *row = &closure->functions[i];
        if (stable_id_equal(row->id, id) ||
            (stable_id_equal(row->module_identity, input->module_identity) &&
             stable_id_equal(row->declaration_identity, input->declaration_identity) &&
             stable_id_equal(row->concrete_instance_identity,
                             input->concrete_instance_identity)))
            return fail(error, error_size, "XR_SEM_0019",
                        "concrete function authority is duplicated or conflicting");
    }
    if (closure->function_count == closure->function_capacity &&
        !grow_table((void **) &closure->functions, &closure->function_capacity,
                    closure->limits.max_functions, sizeof(*closure->functions)))
        return fail(error, error_size, "XR_EXEC_5003",
                    "concrete function budget is exhausted");
    closure->functions[closure->function_count++] = (XrProgramSemanticFunctionRecord) {
        .id = id,
        .module_identity = input->module_identity,
        .declaration_identity = input->declaration_identity,
        .concrete_instance_identity = input->concrete_instance_identity,
        .signature_fingerprint = input->signature_fingerprint,
        .effect_fingerprint = input->effect_fingerprint,
        .capability_mask = input->capability_mask,
        .flags = input->flags,
    };
    *function_identity = id;
    return true;
}

bool xr_program_semantic_closure_add_call(XrProgramSemanticClosure *closure,
                                          const XrProgramSemanticCallInput *input,
                                          XrStableId *call_identity, char *error,
                                          size_t error_size) {
    if (call_identity)
        memset(call_identity, 0, sizeof(*call_identity));
    if (!collecting(closure) || !input || !call_identity ||
        stable_id_is_zero(input->callsite_identity) ||
        stable_id_is_zero(input->caller_function) ||
        stable_id_is_zero(input->callee_function) ||
        fingerprint_is_zero(input->contract_fingerprint))
        return fail(error, error_size, "XR_SEM_0019", "resolved call authority is incomplete");
    XrStableId id = derive_call_identity(closure->policy_fingerprint, input);
    for (uint32_t i = 0; i < closure->call_count; i++) {
        const XrProgramSemanticCallRecord *row = &closure->calls[i];
        if (stable_id_equal(row->id, id) ||
            stable_id_equal(row->callsite_identity, input->callsite_identity))
            return fail(error, error_size, "XR_SEM_0019",
                        "resolved call authority is duplicated or conflicting");
    }
    if (closure->call_count == closure->call_capacity &&
        !grow_table((void **) &closure->calls, &closure->call_capacity,
                    closure->limits.max_calls, sizeof(*closure->calls)))
        return fail(error, error_size, "XR_EXEC_5003", "resolved call budget is exhausted");
    closure->calls[closure->call_count++] = (XrProgramSemanticCallRecord) {
        .id = id,
        .callsite_identity = input->callsite_identity,
        .caller_function = input->caller_function,
        .callee_function = input->callee_function,
        .contract_fingerprint = input->contract_fingerprint,
    };
    *call_identity = id;
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
    const XrProgramSemanticDependencyRecord *a =
        (const XrProgramSemanticDependencyRecord *) left;
    const XrProgramSemanticDependencyRecord *b =
        (const XrProgramSemanticDependencyRecord *) right;
    int result = compare_stable_id(a->source_module, b->source_module);
    return result ? result : compare_stable_id(a->dependency_module, b->dependency_module);
}

static int compare_types(const void *left, const void *right) {
    const XrProgramSemanticTypeRecord *a = (const XrProgramSemanticTypeRecord *) left;
    const XrProgramSemanticTypeRecord *b = (const XrProgramSemanticTypeRecord *) right;
    return compare_stable_id(a->id, b->id);
}

static int compare_functions(const void *left, const void *right) {
    const XrProgramSemanticFunctionRecord *a =
        (const XrProgramSemanticFunctionRecord *) left;
    const XrProgramSemanticFunctionRecord *b =
        (const XrProgramSemanticFunctionRecord *) right;
    return compare_stable_id(a->id, b->id);
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
    if (closure->function_count > 1u)
        qsort(closure->functions, closure->function_count, sizeof(*closure->functions),
              compare_functions);
    if (closure->call_count > 1u)
        qsort(closure->calls, closure->call_count, sizeof(*closure->calls), compare_calls);
}

static void compute_closure_fingerprint(const XrProgramSemanticClosure *closure,
                                        XrFingerprint *out) {
    static const uint8_t domain[] = "xray-program-semantic-closure-v1\0";
    XrSHA256Context context;
    xr_sha256_init(&context);
    xr_sha256_update(&context, domain, sizeof(domain) - 1u);
    hash_u32(&context, closure->schema);
    hash_fingerprint(&context, closure->policy_fingerprint);
    hash_u32(&context, closure->module_count);
    hash_u32(&context, closure->dependency_count);
    hash_u32(&context, closure->type_count);
    hash_u32(&context, closure->function_count);
    hash_u32(&context, closure->call_count);
    for (uint32_t i = 0; i < closure->module_count; i++) {
        hash_stable_id(&context, closure->modules[i].module_identity);
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
        hash_fingerprint(&context, closure->types[i].shape_fingerprint);
        hash_fingerprint(&context, closure->types[i].ownership_fingerprint);
    }
    for (uint32_t i = 0; i < closure->function_count; i++) {
        hash_stable_id(&context, closure->functions[i].id);
        hash_stable_id(&context, closure->functions[i].module_identity);
        hash_stable_id(&context, closure->functions[i].declaration_identity);
        hash_stable_id(&context, closure->functions[i].concrete_instance_identity);
        hash_fingerprint(&context, closure->functions[i].signature_fingerprint);
        hash_fingerprint(&context, closure->functions[i].effect_fingerprint);
        hash_u64(&context, closure->functions[i].capability_mask);
        xr_sha256_update(&context, &closure->functions[i].flags, 1u);
    }
    for (uint32_t i = 0; i < closure->call_count; i++) {
        hash_stable_id(&context, closure->calls[i].id);
        hash_stable_id(&context, closure->calls[i].callsite_identity);
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
    if (!collecting(closure))
        return fail(error, error_size, "XR_SEM_0019",
                    "program semantic closure is not collecting");
    canonicalize_tables(closure);
    compute_closure_fingerprint(closure, &closure->fingerprint);
    closure->generation_id = compute_generation_id(closure->fingerprint);
    closure->state = XR_PROGRAM_SEMANTIC_CLOSURE_VERIFYING;
    if (!xr_program_semantic_closure_verify(closure, error, error_size)) {
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

size_t xr_program_semantic_closure_function_count(const XrProgramSemanticClosure *closure) {
    return xr_program_semantic_closure_is_frozen(closure) ? closure->function_count : 0;
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

const XrProgramSemanticFunctionRecord *
xr_program_semantic_closure_function(const XrProgramSemanticClosure *closure, uint32_t index) {
    return xr_program_semantic_closure_is_frozen(closure) && index < closure->function_count
               ? &closure->functions[index]
               : NULL;
}

const XrProgramSemanticCallRecord *
xr_program_semantic_closure_call(const XrProgramSemanticClosure *closure, uint32_t index) {
    return xr_program_semantic_closure_is_frozen(closure) && index < closure->call_count
               ? &closure->calls[index]
               : NULL;
}
