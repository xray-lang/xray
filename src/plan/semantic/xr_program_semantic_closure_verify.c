/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_program_semantic_closure_verify.c - Independent program closure verifier
 *
 * KEY CONCEPT:
 *   The verifier reconstructs row identities, graph closure, the aggregate
 *   fingerprint, and GenerationClosureId without calling builder algorithms.
 */

#include "xr_program_semantic_closure_internal.h"
#include "../../base/xmalloc.h"
#include "../../base/xsha256.h"
#include <stdio.h>
#include <string.h>

static bool reject(char *error, size_t error_size, const char *code, const char *detail) {
    if (error && error_size)
        snprintf(error, error_size, "%s: %s", code, detail);
    return false;
}

static bool verifier_bytes_are_zero(const uint8_t *bytes, size_t size) {
    uint8_t combined = 0;
    for (size_t i = 0; i < size; i++)
        combined |= bytes[i];
    return combined == 0;
}

static bool verifier_stable_id_zero(XrStableId id) {
    return verifier_bytes_are_zero(id.bytes, sizeof(id.bytes));
}

static bool verifier_fingerprint_zero(XrFingerprint fingerprint) {
    return verifier_bytes_are_zero(fingerprint.bytes, sizeof(fingerprint.bytes));
}

static int verifier_id_compare(XrStableId left, XrStableId right) {
    return memcmp(left.bytes, right.bytes, sizeof(left.bytes));
}

static bool verifier_id_equal(XrStableId left, XrStableId right) {
    return verifier_id_compare(left, right) == 0;
}

static void verifier_hash_u32(XrSHA256Context *context, uint32_t value) {
    uint8_t bytes[4];
    for (uint32_t i = 0; i < sizeof(bytes); i++)
        bytes[i] = (uint8_t) (value >> (i * 8u));
    xr_sha256_update(context, bytes, sizeof(bytes));
}

static void verifier_hash_u64(XrSHA256Context *context, uint64_t value) {
    uint8_t bytes[8];
    for (uint32_t i = 0; i < sizeof(bytes); i++)
        bytes[i] = (uint8_t) (value >> (i * 8u));
    xr_sha256_update(context, bytes, sizeof(bytes));
}

static void verifier_hash_id(XrSHA256Context *context, XrStableId id) {
    xr_sha256_update(context, id.bytes, sizeof(id.bytes));
}

static void verifier_hash_fingerprint(XrSHA256Context *context, XrFingerprint fingerprint) {
    xr_sha256_update(context, fingerprint.bytes, sizeof(fingerprint.bytes));
}

static XrStableId verifier_finish_id(XrSHA256Context *context) {
    uint8_t digest[XR_FINGERPRINT_BYTES];
    XrStableId id;
    xr_sha256_final(context, digest);
    memcpy(id.bytes, digest, sizeof(id.bytes));
    return id;
}

static XrStableId verifier_type_identity(XrFingerprint policy_fingerprint,
                                         const XrProgramSemanticTypeRecord *row) {
    static const uint8_t domain[] = "xray-program-semantic-type-v1\0";
    XrSHA256Context context;
    xr_sha256_init(&context);
    xr_sha256_update(&context, domain, sizeof(domain) - 1u);
    verifier_hash_u32(&context, XR_PROGRAM_SEMANTIC_CLOSURE_SCHEMA_VERSION);
    verifier_hash_fingerprint(&context, policy_fingerprint);
    verifier_hash_id(&context, row->module_identity);
    verifier_hash_id(&context, row->declaration_identity);
    verifier_hash_id(&context, row->concrete_instance_identity);
    verifier_hash_fingerprint(&context, row->shape_fingerprint);
    verifier_hash_fingerprint(&context, row->ownership_fingerprint);
    return verifier_finish_id(&context);
}

static XrStableId verifier_function_identity(XrFingerprint policy_fingerprint,
                                             const XrProgramSemanticFunctionRecord *row) {
    static const uint8_t domain[] = "xray-program-semantic-function-v1\0";
    XrSHA256Context context;
    xr_sha256_init(&context);
    xr_sha256_update(&context, domain, sizeof(domain) - 1u);
    verifier_hash_u32(&context, XR_PROGRAM_SEMANTIC_CLOSURE_SCHEMA_VERSION);
    verifier_hash_fingerprint(&context, policy_fingerprint);
    verifier_hash_id(&context, row->module_identity);
    verifier_hash_id(&context, row->declaration_identity);
    verifier_hash_id(&context, row->concrete_instance_identity);
    verifier_hash_fingerprint(&context, row->signature_fingerprint);
    verifier_hash_fingerprint(&context, row->effect_fingerprint);
    verifier_hash_u64(&context, row->capability_mask);
    return verifier_finish_id(&context);
}

static XrStableId verifier_call_identity(XrFingerprint policy_fingerprint,
                                         const XrProgramSemanticCallRecord *row) {
    static const uint8_t domain[] = "xray-program-semantic-call-v1\0";
    XrSHA256Context context;
    xr_sha256_init(&context);
    xr_sha256_update(&context, domain, sizeof(domain) - 1u);
    verifier_hash_u32(&context, XR_PROGRAM_SEMANTIC_CLOSURE_SCHEMA_VERSION);
    verifier_hash_fingerprint(&context, policy_fingerprint);
    verifier_hash_id(&context, row->callsite_identity);
    verifier_hash_id(&context, row->caller_function);
    verifier_hash_id(&context, row->callee_function);
    verifier_hash_fingerprint(&context, row->contract_fingerprint);
    return verifier_finish_id(&context);
}

static int find_module(const XrProgramSemanticClosure *closure, XrStableId id) {
    uint32_t low = 0;
    uint32_t high = closure->module_count;
    while (low < high) {
        uint32_t mid = low + (high - low) / 2u;
        int order = verifier_id_compare(closure->modules[mid].module_identity, id);
        if (order < 0)
            low = mid + 1u;
        else
            high = mid;
    }
    return low < closure->module_count &&
                   verifier_id_equal(closure->modules[low].module_identity, id)
               ? (int) low
               : -1;
}

static int find_function(const XrProgramSemanticClosure *closure, XrStableId id) {
    uint32_t low = 0;
    uint32_t high = closure->function_count;
    while (low < high) {
        uint32_t mid = low + (high - low) / 2u;
        int order = verifier_id_compare(closure->functions[mid].id, id);
        if (order < 0)
            low = mid + 1u;
        else
            high = mid;
    }
    return low < closure->function_count && verifier_id_equal(closure->functions[low].id, id)
               ? (int) low
               : -1;
}

static bool has_direct_dependency(const XrProgramSemanticClosure *closure,
                                  XrStableId source, XrStableId dependency) {
    for (uint32_t i = 0; i < closure->dependency_count; i++) {
        const XrProgramSemanticDependencyRecord *row = &closure->dependencies[i];
        if (verifier_id_equal(row->source_module, source) &&
            verifier_id_equal(row->dependency_module, dependency))
            return true;
    }
    return false;
}

static bool verify_module_rows(const XrProgramSemanticClosure *closure,
                               char *error, size_t error_size) {
    for (uint32_t i = 0; i < closure->module_count; i++) {
        const XrProgramSemanticModuleRecord *row = &closure->modules[i];
        if (verifier_stable_id_zero(row->module_identity) ||
            verifier_fingerprint_zero(row->source_fingerprint) ||
            verifier_fingerprint_zero(row->export_fingerprint) ||
            (i && verifier_id_compare(closure->modules[i - 1u].module_identity,
                                      row->module_identity) >= 0))
            return reject(error, error_size, "XR_SEM_0019",
                          "program module table is incomplete or non-canonical");
    }
    for (uint32_t i = 0; i < closure->dependency_count; i++) {
        const XrProgramSemanticDependencyRecord *row = &closure->dependencies[i];
        bool ordered = true;
        if (i) {
            const XrProgramSemanticDependencyRecord *previous = &closure->dependencies[i - 1u];
            int source_order = verifier_id_compare(previous->source_module, row->source_module);
            ordered = source_order < 0 ||
                      (source_order == 0 &&
                       verifier_id_compare(previous->dependency_module,
                                           row->dependency_module) < 0);
        }
        if (!ordered || find_module(closure, row->source_module) < 0 ||
            find_module(closure, row->dependency_module) < 0 ||
            verifier_id_equal(row->source_module, row->dependency_module) ||
            verifier_fingerprint_zero(row->contract_fingerprint))
            return reject(error, error_size, "XR_SEM_0019",
                          "program dependency table is incomplete or non-canonical");
    }
    return true;
}

static bool verify_type_rows(const XrProgramSemanticClosure *closure,
                             char *error, size_t error_size) {
    for (uint32_t i = 0; i < closure->type_count; i++) {
        const XrProgramSemanticTypeRecord *row = &closure->types[i];
        if (verifier_stable_id_zero(row->declaration_identity) ||
            verifier_stable_id_zero(row->concrete_instance_identity) ||
            verifier_fingerprint_zero(row->shape_fingerprint) ||
            verifier_fingerprint_zero(row->ownership_fingerprint) ||
            find_module(closure, row->module_identity) < 0 ||
            !verifier_id_equal(row->id,
                               verifier_type_identity(closure->policy_fingerprint, row)) ||
            (i && verifier_id_compare(closure->types[i - 1u].id, row->id) >= 0))
            return reject(error, error_size, "XR_SEM_0013",
                          "concrete type identity is incomplete or non-canonical");
        for (uint32_t j = 0; j < i; j++)
            if (verifier_id_equal(closure->types[j].module_identity, row->module_identity) &&
                verifier_id_equal(closure->types[j].declaration_identity,
                                  row->declaration_identity) &&
                verifier_id_equal(closure->types[j].concrete_instance_identity,
                                  row->concrete_instance_identity))
                return reject(error, error_size, "XR_SEM_0019",
                              "concrete type declaration is duplicated");
    }
    return true;
}

static bool verify_function_rows(const XrProgramSemanticClosure *closure,
                                 char *error, size_t error_size) {
    uint32_t roots = 0;
    for (uint32_t i = 0; i < closure->function_count; i++) {
        const XrProgramSemanticFunctionRecord *row = &closure->functions[i];
        if (verifier_stable_id_zero(row->declaration_identity) ||
            verifier_stable_id_zero(row->concrete_instance_identity) ||
            verifier_fingerprint_zero(row->signature_fingerprint) ||
            verifier_fingerprint_zero(row->effect_fingerprint) ||
            find_module(closure, row->module_identity) < 0 ||
            (row->flags & ~(XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY |
                            XR_PROGRAM_SEMANTIC_FUNCTION_EXPORTED)) != 0 ||
            memcmp(row->reserved, (uint8_t[7]) {0}, sizeof(row->reserved)) != 0 ||
            !verifier_id_equal(
                row->id, verifier_function_identity(closure->policy_fingerprint, row)) ||
            (i && verifier_id_compare(closure->functions[i - 1u].id, row->id) >= 0))
            return reject(error, error_size, "XR_SEM_0013",
                          "concrete function identity is incomplete or non-canonical");
        for (uint32_t j = 0; j < i; j++)
            if (verifier_id_equal(closure->functions[j].module_identity,
                                  row->module_identity) &&
                 verifier_id_equal(closure->functions[j].declaration_identity,
                                   row->declaration_identity) &&
                 verifier_id_equal(closure->functions[j].concrete_instance_identity,
                                   row->concrete_instance_identity))
                return reject(error, error_size, "XR_SEM_0019",
                              "concrete function declaration is duplicated");
        if (row->flags != 0)
            roots++;
    }
    return roots > 0 || reject(error, error_size, "XR_SEM_0019",
                               "program closure requires a concrete entry or export root");
}

static bool verify_call_rows(const XrProgramSemanticClosure *closure,
                             char *error, size_t error_size) {
    for (uint32_t i = 0; i < closure->call_count; i++) {
        const XrProgramSemanticCallRecord *row = &closure->calls[i];
        int caller = find_function(closure, row->caller_function);
        int callee = find_function(closure, row->callee_function);
        if (verifier_stable_id_zero(row->callsite_identity) ||
            verifier_fingerprint_zero(row->contract_fingerprint) || caller < 0 || callee < 0 ||
            !verifier_id_equal(row->id,
                               verifier_call_identity(closure->policy_fingerprint, row)) ||
            (i && verifier_id_compare(closure->calls[i - 1u].id, row->id) >= 0))
            return reject(error, error_size, "XR_SEM_0013",
                          "resolved call identity is incomplete or non-canonical");
        for (uint32_t j = 0; j < i; j++)
            if (verifier_id_equal(closure->calls[j].callsite_identity,
                                  row->callsite_identity))
                return reject(error, error_size, "XR_SEM_0019",
                              "resolved callsite identity is duplicated");
        XrStableId caller_module = closure->functions[(uint32_t) caller].module_identity;
        XrStableId callee_module = closure->functions[(uint32_t) callee].module_identity;
        if (!verifier_id_equal(caller_module, callee_module) &&
            !has_direct_dependency(closure, caller_module, callee_module))
            return reject(error, error_size, "XR_SEM_0019",
                          "cross-module call lacks an exact dependency contract");
    }
    return true;
}

static bool verify_module_graph(const XrProgramSemanticClosure *closure,
                                char *error, size_t error_size) {
    uint32_t count = closure->module_count;
    uint32_t *begin = (uint32_t *) xr_calloc((size_t) count + 1u, sizeof(*begin));
    uint32_t *targets = (uint32_t *) xr_malloc((size_t) closure->dependency_count *
                                               sizeof(*targets));
    uint32_t *indegree = (uint32_t *) xr_calloc(count, sizeof(*indegree));
    uint32_t *queue = (uint32_t *) xr_malloc((size_t) count * sizeof(*queue));
    uint8_t *reachable = (uint8_t *) xr_calloc(count, sizeof(*reachable));
    if (!begin || !indegree || !queue || !reachable ||
        (closure->dependency_count && !targets)) {
        xr_free(begin);
        xr_free(targets);
        xr_free(indegree);
        xr_free(queue);
        xr_free(reachable);
        return reject(error, error_size, "XR_EXEC_5003",
                      "program module graph verification allocation failed");
    }
    for (uint32_t i = 0; i < closure->dependency_count; i++) {
        uint32_t source = (uint32_t) find_module(closure, closure->dependencies[i].source_module);
        uint32_t target =
            (uint32_t) find_module(closure, closure->dependencies[i].dependency_module);
        begin[source + 1u]++;
        indegree[target]++;
    }
    for (uint32_t i = 1; i <= count; i++)
        begin[i] += begin[i - 1u];
    uint32_t *cursor = (uint32_t *) xr_malloc((size_t) count * sizeof(*cursor));
    if (count && !cursor) {
        xr_free(begin);
        xr_free(targets);
        xr_free(indegree);
        xr_free(queue);
        xr_free(reachable);
        return reject(error, error_size, "XR_EXEC_5003",
                      "program module graph cursor allocation failed");
    }
    memcpy(cursor, begin, (size_t) count * sizeof(*cursor));
    for (uint32_t i = 0; i < closure->dependency_count; i++) {
        uint32_t source = (uint32_t) find_module(closure, closure->dependencies[i].source_module);
        targets[cursor[source]++] =
            (uint32_t) find_module(closure, closure->dependencies[i].dependency_module);
    }
    xr_free(cursor);
    uint32_t head = 0;
    uint32_t tail = 0;
    for (uint32_t i = 0; i < count; i++)
        if (indegree[i] == 0)
            queue[tail++] = i;
    uint32_t processed = 0;
    while (head < tail) {
        uint32_t node = queue[head++];
        processed++;
        for (uint32_t edge = begin[node]; edge < begin[node + 1u]; edge++)
            if (--indegree[targets[edge]] == 0)
                queue[tail++] = targets[edge];
    }
    head = 0;
    tail = 0;
    for (uint32_t i = 0; i < closure->function_count; i++) {
        if (closure->functions[i].flags == 0)
            continue;
        uint32_t root =
            (uint32_t) find_module(closure, closure->functions[i].module_identity);
        if (!reachable[root]) {
            reachable[root] = 1u;
            queue[tail++] = root;
        }
    }
    while (head < tail) {
        uint32_t node = queue[head++];
        for (uint32_t edge = begin[node]; edge < begin[node + 1u]; edge++) {
            uint32_t target = targets[edge];
            if (!reachable[target]) {
                reachable[target] = 1u;
                queue[tail++] = target;
            }
        }
    }
    bool complete = processed == count;
    for (uint32_t i = 0; complete && i < count; i++)
        complete = reachable[i] != 0;
    xr_free(begin);
    xr_free(targets);
    xr_free(indegree);
    xr_free(queue);
    xr_free(reachable);
    return complete || reject(error, error_size, "XR_SEM_0019",
                              "program module dependency graph is cyclic or unreachable");
}

static bool verify_call_graph(const XrProgramSemanticClosure *closure,
                              char *error, size_t error_size) {
    uint32_t count = closure->function_count;
    uint32_t *begin = (uint32_t *) xr_calloc((size_t) count + 1u, sizeof(*begin));
    uint32_t *targets =
        (uint32_t *) xr_malloc((size_t) closure->call_count * sizeof(*targets));
    uint32_t *queue = (uint32_t *) xr_malloc((size_t) count * sizeof(*queue));
    uint8_t *reachable = (uint8_t *) xr_calloc(count, sizeof(*reachable));
    if (!begin || !queue || !reachable || (closure->call_count && !targets)) {
        xr_free(begin);
        xr_free(targets);
        xr_free(queue);
        xr_free(reachable);
        return reject(error, error_size, "XR_EXEC_5003",
                      "program call graph verification allocation failed");
    }
    for (uint32_t i = 0; i < closure->call_count; i++) {
        uint32_t caller = (uint32_t) find_function(closure, closure->calls[i].caller_function);
        begin[caller + 1u]++;
    }
    for (uint32_t i = 1; i <= count; i++)
        begin[i] += begin[i - 1u];
    uint32_t *cursor = (uint32_t *) xr_malloc((size_t) count * sizeof(*cursor));
    if (count && !cursor) {
        xr_free(begin);
        xr_free(targets);
        xr_free(queue);
        xr_free(reachable);
        return reject(error, error_size, "XR_EXEC_5003",
                      "program call graph cursor allocation failed");
    }
    memcpy(cursor, begin, (size_t) count * sizeof(*cursor));
    for (uint32_t i = 0; i < closure->call_count; i++) {
        uint32_t caller = (uint32_t) find_function(closure, closure->calls[i].caller_function);
        targets[cursor[caller]++] =
            (uint32_t) find_function(closure, closure->calls[i].callee_function);
    }
    xr_free(cursor);
    uint32_t head = 0;
    uint32_t tail = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (closure->functions[i].flags != 0) {
            queue[tail++] = i;
            reachable[i] = 1u;
        }
    }
    while (head < tail) {
        uint32_t node = queue[head++];
        for (uint32_t edge = begin[node]; edge < begin[node + 1u]; edge++) {
            uint32_t target = targets[edge];
            if (!reachable[target]) {
                reachable[target] = 1u;
                queue[tail++] = target;
            }
        }
    }
    bool complete = true;
    for (uint32_t i = 0; complete && i < count; i++)
        complete = reachable[i] != 0;
    xr_free(begin);
    xr_free(targets);
    xr_free(queue);
    xr_free(reachable);
    return complete || reject(error, error_size, "XR_SEM_0019",
                              "concrete function call graph is not closed from the entry");
}

static void verifier_closure_fingerprint(const XrProgramSemanticClosure *closure,
                                         XrFingerprint *out) {
    static const uint8_t domain[] = "xray-program-semantic-closure-v1\0";
    XrSHA256Context context;
    xr_sha256_init(&context);
    xr_sha256_update(&context, domain, sizeof(domain) - 1u);
    verifier_hash_u32(&context, closure->schema);
    verifier_hash_fingerprint(&context, closure->policy_fingerprint);
    verifier_hash_u32(&context, closure->module_count);
    verifier_hash_u32(&context, closure->dependency_count);
    verifier_hash_u32(&context, closure->type_count);
    verifier_hash_u32(&context, closure->function_count);
    verifier_hash_u32(&context, closure->call_count);
    for (uint32_t i = 0; i < closure->module_count; i++) {
        verifier_hash_id(&context, closure->modules[i].module_identity);
        verifier_hash_fingerprint(&context, closure->modules[i].source_fingerprint);
        verifier_hash_fingerprint(&context, closure->modules[i].export_fingerprint);
    }
    for (uint32_t i = 0; i < closure->dependency_count; i++) {
        verifier_hash_id(&context, closure->dependencies[i].source_module);
        verifier_hash_id(&context, closure->dependencies[i].dependency_module);
        verifier_hash_fingerprint(&context, closure->dependencies[i].contract_fingerprint);
    }
    for (uint32_t i = 0; i < closure->type_count; i++) {
        verifier_hash_id(&context, closure->types[i].id);
        verifier_hash_id(&context, closure->types[i].module_identity);
        verifier_hash_id(&context, closure->types[i].declaration_identity);
        verifier_hash_id(&context, closure->types[i].concrete_instance_identity);
        verifier_hash_fingerprint(&context, closure->types[i].shape_fingerprint);
        verifier_hash_fingerprint(&context, closure->types[i].ownership_fingerprint);
    }
    for (uint32_t i = 0; i < closure->function_count; i++) {
        verifier_hash_id(&context, closure->functions[i].id);
        verifier_hash_id(&context, closure->functions[i].module_identity);
        verifier_hash_id(&context, closure->functions[i].declaration_identity);
        verifier_hash_id(&context, closure->functions[i].concrete_instance_identity);
        verifier_hash_fingerprint(&context, closure->functions[i].signature_fingerprint);
        verifier_hash_fingerprint(&context, closure->functions[i].effect_fingerprint);
        verifier_hash_u64(&context, closure->functions[i].capability_mask);
        xr_sha256_update(&context, &closure->functions[i].flags, 1u);
    }
    for (uint32_t i = 0; i < closure->call_count; i++) {
        verifier_hash_id(&context, closure->calls[i].id);
        verifier_hash_id(&context, closure->calls[i].callsite_identity);
        verifier_hash_id(&context, closure->calls[i].caller_function);
        verifier_hash_id(&context, closure->calls[i].callee_function);
        verifier_hash_fingerprint(&context, closure->calls[i].contract_fingerprint);
    }
    xr_sha256_final(&context, out->bytes);
}

static XrGenerationClosureId verifier_generation_id(XrFingerprint fingerprint) {
    static const uint8_t domain[] = "xray-generation-closure-id-v1\0";
    XrSHA256Context context;
    uint8_t digest[XR_FINGERPRINT_BYTES];
    XrGenerationClosureId id;
    xr_sha256_init(&context);
    xr_sha256_update(&context, domain, sizeof(domain) - 1u);
    verifier_hash_u32(&context, XR_PROGRAM_SEMANTIC_CLOSURE_SCHEMA_VERSION);
    verifier_hash_fingerprint(&context, fingerprint);
    xr_sha256_final(&context, digest);
    memcpy(id.bytes, digest, sizeof(id.bytes));
    return id;
}

bool xr_program_semantic_closure_verify(const XrProgramSemanticClosure *closure,
                                        char *error, size_t error_size) {
    static const uint8_t zero_reserved[2] = {0};
    bool valid_state = closure &&
                       ((closure->state == XR_PROGRAM_SEMANTIC_CLOSURE_VERIFYING &&
                         closure->verified == 0) ||
                        (closure->state == XR_PROGRAM_SEMANTIC_CLOSURE_FROZEN &&
                         closure->verified == 1u));
    if (!closure ||
        !valid_state || memcmp(closure->reserved, zero_reserved, sizeof(zero_reserved)) != 0 ||
        closure->schema != XR_PROGRAM_SEMANTIC_CLOSURE_SCHEMA_VERSION ||
        verifier_fingerprint_zero(closure->policy_fingerprint) ||
        verifier_fingerprint_zero(closure->fingerprint) ||
        verifier_bytes_are_zero(closure->generation_id.bytes,
                                sizeof(closure->generation_id.bytes)) ||
        closure->module_count == 0 || closure->function_count == 0 ||
        closure->limits.max_modules == 0 ||
        closure->limits.max_modules > XR_PROGRAM_SEMANTIC_CLOSURE_MAX_MODULES ||
        closure->limits.max_dependencies > XR_PROGRAM_SEMANTIC_CLOSURE_MAX_DEPENDENCIES ||
        closure->limits.max_types > XR_PROGRAM_SEMANTIC_CLOSURE_MAX_TYPES ||
        closure->limits.max_functions == 0 ||
        closure->limits.max_functions > XR_PROGRAM_SEMANTIC_CLOSURE_MAX_FUNCTIONS ||
        closure->limits.max_calls > XR_PROGRAM_SEMANTIC_CLOSURE_MAX_CALLS ||
        closure->module_count > closure->limits.max_modules ||
        closure->dependency_count > closure->limits.max_dependencies ||
        closure->type_count > closure->limits.max_types ||
        closure->function_count > closure->limits.max_functions ||
        closure->call_count > closure->limits.max_calls)
        return reject(error, error_size, "XR_SEM_0019",
                      "program semantic closure header is incomplete");
    if (!verify_module_rows(closure, error, error_size) ||
        !verify_type_rows(closure, error, error_size) ||
        !verify_function_rows(closure, error, error_size) ||
        !verify_call_rows(closure, error, error_size) ||
        !verify_module_graph(closure, error, error_size) ||
        !verify_call_graph(closure, error, error_size))
        return false;
    XrFingerprint expected;
    verifier_closure_fingerprint(closure, &expected);
    if (memcmp(expected.bytes, closure->fingerprint.bytes, sizeof(expected.bytes)) != 0)
        return reject(error, error_size, "XR_SEM_0013",
                      "program semantic closure fingerprint does not match its rows");
    XrGenerationClosureId generation = verifier_generation_id(expected);
    if (memcmp(generation.bytes, closure->generation_id.bytes, sizeof(generation.bytes)) != 0)
        return reject(error, error_size, "XR_SEM_0013",
                      "GenerationClosureId does not match the verified closed world");
    return true;
}
