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
#include "xr_scalar_call_semantics.h"
#include "../../base/xmalloc.h"
#include "../../base/xsha256.h"
#include "../../shared/xr_exact_scalar_registry.h"
#include "../../runtime/value/xtype.h"
#include "../../frontend/parser/xast_types.h"
#include <limits.h>
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

static void verifier_hash_framed_bytes(XrSHA256Context *context, const uint8_t *bytes,
                                       size_t size) {
    verifier_hash_u64(context, (uint64_t) size);
    if (size)
        xr_sha256_update(context, bytes, size);
}

static void verifier_hash_framed_id(XrSHA256Context *context, XrStableId id) {
    verifier_hash_framed_bytes(context, id.bytes, sizeof(id.bytes));
}

static void verifier_hash_framed_fingerprint(XrSHA256Context *context, XrFingerprint fingerprint) {
    verifier_hash_framed_bytes(context, fingerprint.bytes, sizeof(fingerprint.bytes));
}

static XrStableId verifier_finish_id(XrSHA256Context *context) {
    uint8_t digest[XR_FINGERPRINT_BYTES];
    XrStableId id;
    xr_sha256_final(context, digest);
    memcpy(id.bytes, digest, sizeof(id.bytes));
    return id;
}

static void verifier_hash_locator(XrSHA256Context *context,
                                  XrProgramSemanticSourceLocator locator) {
    verifier_hash_u32(context, locator.kind);
    verifier_hash_u32(context, locator.start_line);
    verifier_hash_u32(context, locator.start_column);
    verifier_hash_u32(context, locator.end_line);
    verifier_hash_u32(context, locator.end_column);
}

static XrStableId verifier_source_module_identity(XrFingerprint authority_fingerprint) {
    static const uint8_t domain[] = "xray-source-module-identity-v1";
    XrSHA256Context context;
    xr_sha256_init(&context);
    verifier_hash_framed_bytes(&context, domain, sizeof(domain) - 1u);
    verifier_hash_u32(&context, UINT32_C(1));
    verifier_hash_framed_fingerprint(&context, authority_fingerprint);
    return verifier_finish_id(&context);
}

static XrFingerprint verifier_empty_export_fingerprint(XrStableId module_identity) {
    static const uint8_t domain[] = "xray-source-module-empty-exports-v1";
    XrSHA256Context context;
    XrFingerprint result;
    xr_sha256_init(&context);
    verifier_hash_framed_bytes(&context, domain, sizeof(domain) - 1u);
    verifier_hash_u32(&context, UINT32_C(1));
    verifier_hash_framed_id(&context, module_identity);
    verifier_hash_u32(&context, 0);
    xr_sha256_final(&context, result.bytes);
    return result;
}

static XrStableId verifier_leaf_aggregate_declaration(const XrProgramSemanticClosure *closure,
                                                      const XrProgramSemanticTypeRecord *row,
                                                      const XrProgramSemanticModuleRecord *module) {
    static const uint8_t domain[] = "xray-source-leaf-value-declaration-v1";
    XrSHA256Context context;
    xr_sha256_init(&context);
    verifier_hash_framed_bytes(&context, domain, sizeof(domain) - 1u);
    verifier_hash_u32(&context, XR_PROGRAM_SEMANTIC_CLOSURE_SCHEMA_VERSION);
    verifier_hash_framed_id(&context, module->module_identity);
    verifier_hash_framed_fingerprint(&context, module->source_fingerprint);
    verifier_hash_locator(&context, row->declaration_locator);
    verifier_hash_u32(&context, row->field_count);
    for (uint32_t i = 0; i < row->field_count; i++) {
        const XrProgramSemanticTypeFieldRecord *field = &closure->type_fields[row->field_begin + i];
        verifier_hash_u32(&context, field->declaration_ordinal);
        verifier_hash_framed_id(&context, field->field_type);
    }
    return verifier_finish_id(&context);
}

static XrStableId verifier_leaf_aggregate_instance(XrStableId declaration) {
    static const uint8_t domain[] = "xray-source-nongeneric-leaf-value-instance-v1";
    XrSHA256Context context;
    xr_sha256_init(&context);
    verifier_hash_framed_bytes(&context, domain, sizeof(domain) - 1u);
    verifier_hash_u32(&context, XR_PROGRAM_SEMANTIC_CLOSURE_SCHEMA_VERSION);
    verifier_hash_framed_id(&context, declaration);
    return verifier_finish_id(&context);
}

static XrStableId verifier_leaf_function_declaration(const XrProgramSemanticModuleRecord *module,
                                                     const XrProgramSemanticFunctionRecord *row) {
    static const uint8_t domain[] = "xray-source-leaf-value-function-declaration-v1";
    XrSHA256Context context;
    xr_sha256_init(&context);
    verifier_hash_framed_bytes(&context, domain, sizeof(domain) - 1u);
    verifier_hash_u32(&context, XR_PROGRAM_SEMANTIC_CLOSURE_SCHEMA_VERSION);
    verifier_hash_framed_id(&context, module->module_identity);
    verifier_hash_framed_fingerprint(&context, module->source_fingerprint);
    verifier_hash_locator(&context, row->declaration_locator);
    verifier_hash_framed_fingerprint(&context, row->signature_fingerprint);
    return verifier_finish_id(&context);
}

static XrStableId verifier_leaf_function_instance(XrStableId declaration, XrFingerprint signature) {
    static const uint8_t domain[] = "xray-source-nongeneric-leaf-function-instance-v1";
    XrSHA256Context context;
    xr_sha256_init(&context);
    verifier_hash_framed_bytes(&context, domain, sizeof(domain) - 1u);
    verifier_hash_u32(&context, XR_PROGRAM_SEMANTIC_CLOSURE_SCHEMA_VERSION);
    verifier_hash_framed_id(&context, declaration);
    verifier_hash_framed_fingerprint(&context, signature);
    return verifier_finish_id(&context);
}

static XrFingerprint verifier_scalar_policy(void) {
    static const uint8_t domain[] = "xray-pre-xi-scalar-authority-policy-v1";
    XrSHA256Context context;
    XrFingerprint result;
    xr_sha256_init(&context);
    verifier_hash_framed_bytes(&context, domain, sizeof(domain) - 1u);
    verifier_hash_u32(&context, UINT32_C(1));
    verifier_hash_u32(&context, UINT32_C(2));
    verifier_hash_u32(&context, UINT32_C(1));
    xr_sha256_final(&context, result.bytes);
    return result;
}

static XrStableId verifier_scalar_function_declaration(const XrProgramSemanticModuleRecord *module,
                                                       const XrProgramSemanticFunctionRecord *row) {
    static const uint8_t domain[] = "xray-source-scalar-function-declaration-v1";
    XrSHA256Context context;
    xr_sha256_init(&context);
    verifier_hash_framed_bytes(&context, domain, sizeof(domain) - 1u);
    verifier_hash_u32(&context, UINT32_C(1));
    verifier_hash_framed_id(&context, module->module_identity);
    verifier_hash_framed_fingerprint(&context, module->source_fingerprint);
    verifier_hash_locator(&context, row->declaration_locator);
    verifier_hash_framed_fingerprint(&context, row->signature_fingerprint);
    return verifier_finish_id(&context);
}

static XrStableId verifier_scalar_function_instance(const XrProgramSemanticModuleRecord *module,
                                                    XrStableId declaration,
                                                    XrFingerprint signature) {
    static const uint8_t domain[] = "xray-source-nongeneric-scalar-instance-v1";
    XrSHA256Context context;
    xr_sha256_init(&context);
    verifier_hash_framed_bytes(&context, domain, sizeof(domain) - 1u);
    verifier_hash_u32(&context, UINT32_C(1));
    verifier_hash_framed_id(&context, module->module_identity);
    verifier_hash_framed_id(&context, declaration);
    verifier_hash_framed_fingerprint(&context, signature);
    return verifier_finish_id(&context);
}

static XrFingerprint verifier_scalar_signature(uint32_t parameter_count) {
    XrScalarI64FunctionContract contract;
    if (parameter_count > 1u ||
        !xr_scalar_i64_function_contract(parameter_count == 0 ? XR_SCALAR_I64_FUNCTION_NULLARY
                                                              : XR_SCALAR_I64_FUNCTION_UNARY,
                                         &contract))
        return (XrFingerprint) {{0}};
    return contract.signature_fingerprint;
}

static XrFingerprint verifier_scalar_effect(void) {
    XrScalarI64FunctionContract contract;
    if (!xr_scalar_i64_function_contract(XR_SCALAR_I64_FUNCTION_NULLARY, &contract))
        return (XrFingerprint) {{0}};
    return contract.effect_fingerprint;
}

static XrFingerprint verifier_scalar_call_contract(void) {
    XrScalarI64FunctionContract contract;
    XrFingerprint result = {{0}};
    if (!xr_scalar_i64_function_contract(XR_SCALAR_I64_FUNCTION_UNARY, &contract) ||
        !xr_scalar_i64_call_contract(&contract, &result))
        return (XrFingerprint) {{0}};
    return result;
}

static XrStableId verifier_type_identity(XrFingerprint policy_fingerprint,
                                         const XrProgramSemanticTypeRecord *row) {
    if (row->kind == XR_PROGRAM_SEMANTIC_TYPE_EXACT_SCALAR) {
        static const uint8_t scalar_domain[] = "xray-program-exact-scalar-type-v1\0";
        XrSHA256Context scalar_context;
        xr_sha256_init(&scalar_context);
        xr_sha256_update(&scalar_context, scalar_domain, sizeof(scalar_domain) - 1u);
        verifier_hash_u32(&scalar_context, XR_PROGRAM_SEMANTIC_CLOSURE_SCHEMA_VERSION);
        verifier_hash_u32(&scalar_context, row->exact_scalar);
        verifier_hash_fingerprint(&scalar_context, row->shape_fingerprint);
        verifier_hash_fingerprint(&scalar_context, row->ownership_fingerprint);
        return verifier_finish_id(&scalar_context);
    }
    static const uint8_t domain[] = "xray-program-semantic-type-v1\0";
    XrSHA256Context context;
    xr_sha256_init(&context);
    xr_sha256_update(&context, domain, sizeof(domain) - 1u);
    /* Independently rebuild the current general type identity frame. */
    verifier_hash_u32(&context, UINT32_C(1));
    verifier_hash_fingerprint(&context, policy_fingerprint);
    verifier_hash_id(&context, row->module_identity);
    verifier_hash_id(&context, row->declaration_identity);
    verifier_hash_id(&context, row->concrete_instance_identity);
    verifier_hash_fingerprint(&context, row->shape_fingerprint);
    verifier_hash_fingerprint(&context, row->ownership_fingerprint);
    return verifier_finish_id(&context);
}

static XrStableId verifier_exact_scalar_registry_owner(void) {
    static const uint8_t domain[] = "xray-exact-scalar-registry-authority-v1\0";
    XrSHA256Context context;
    xr_sha256_init(&context);
    xr_sha256_update(&context, domain, sizeof(domain) - 1u);
    verifier_hash_u32(&context, XR_PROGRAM_SEMANTIC_CLOSURE_SCHEMA_VERSION);
    return verifier_finish_id(&context);
}

static XrStableId verifier_exact_scalar_declaration(uint8_t exact_scalar) {
    static const uint8_t domain[] = "xray-exact-scalar-declaration-v1\0";
    XrSHA256Context context;
    xr_sha256_init(&context);
    xr_sha256_update(&context, domain, sizeof(domain) - 1u);
    verifier_hash_u32(&context, exact_scalar);
    return verifier_finish_id(&context);
}

static XrStableId verifier_exact_scalar_instance(XrStableId declaration, uint8_t exact_scalar) {
    static const uint8_t domain[] = "xray-exact-scalar-instance-v1\0";
    XrSHA256Context context;
    xr_sha256_init(&context);
    xr_sha256_update(&context, domain, sizeof(domain) - 1u);
    verifier_hash_id(&context, declaration);
    verifier_hash_u32(&context, exact_scalar);
    return verifier_finish_id(&context);
}

static bool verifier_fingerprint_equal(XrFingerprint left, XrFingerprint right) {
    return memcmp(left.bytes, right.bytes, sizeof(left.bytes)) == 0;
}

static void verifier_typed_type_fingerprints(const XrProgramSemanticClosure *closure,
                                             const XrProgramSemanticTypeRecord *row,
                                             XrFingerprint *shape, XrFingerprint *ownership) {
    static const uint8_t shape_domain[] = "xray-program-semantic-typed-shape-v1\0";
    static const uint8_t ownership_domain[] = "xray-program-semantic-leaf-ownership-v1\0";
    XrSHA256Context context;
    xr_sha256_init(&context);
    xr_sha256_update(&context, shape_domain, sizeof(shape_domain) - 1u);
    verifier_hash_u32(&context, XR_PROGRAM_SEMANTIC_CLOSURE_SCHEMA_VERSION);
    verifier_hash_u32(&context, row->kind);
    verifier_hash_u32(&context, row->exact_scalar);
    verifier_hash_u32(&context, row->flags);
    verifier_hash_u32(&context, row->field_count);
    for (uint32_t i = 0; i < row->field_count; i++) {
        const XrProgramSemanticTypeFieldRecord *field = &closure->type_fields[row->field_begin + i];
        verifier_hash_u32(&context, field->declaration_ordinal);
        verifier_hash_id(&context, field->field_type);
    }
    xr_sha256_final(&context, shape->bytes);

    xr_sha256_init(&context);
    xr_sha256_update(&context, ownership_domain, sizeof(ownership_domain) - 1u);
    verifier_hash_u32(&context, XR_PROGRAM_SEMANTIC_CLOSURE_SCHEMA_VERSION);
    verifier_hash_u32(&context, row->kind);
    verifier_hash_u32(&context, row->flags);
    verifier_hash_u32(&context, UINT32_C(1));
    verifier_hash_u32(&context, UINT32_C(1));
    verifier_hash_u32(&context, UINT32_C(0));
    verifier_hash_u32(&context, UINT32_C(0));
    verifier_hash_u32(&context, UINT32_C(0));
    xr_sha256_final(&context, ownership->bytes);
}

static XrStableId verifier_function_identity(XrFingerprint policy_fingerprint,
                                             const XrProgramSemanticFunctionRecord *row) {
    static const uint8_t domain[] = "xray-program-semantic-function-v1\0";
    XrSHA256Context context;
    xr_sha256_init(&context);
    xr_sha256_update(&context, domain, sizeof(domain) - 1u);
    verifier_hash_u32(&context, UINT32_C(1));
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
    verifier_hash_u32(&context, UINT32_C(1));
    verifier_hash_fingerprint(&context, policy_fingerprint);
    verifier_hash_id(&context, row->callsite_identity);
    verifier_hash_id(&context, row->caller_function);
    verifier_hash_id(&context, row->callee_function);
    verifier_hash_fingerprint(&context, row->contract_fingerprint);
    return verifier_finish_id(&context);
}

static bool verifier_locator_valid(XrProgramSemanticSourceLocator locator) {
    if (locator.kind == 0 || locator.kind > INT32_MAX || locator.start_line == 0 ||
        locator.start_line > INT32_MAX || locator.start_column == 0 ||
        locator.start_column > INT32_MAX || locator.end_line == 0 || locator.end_line > INT32_MAX ||
        locator.end_column == 0 || locator.end_column > INT32_MAX)
        return false;
    return locator.end_line > locator.start_line ||
           (locator.end_line == locator.start_line && locator.end_column > locator.start_column);
}

static bool verifier_function_locator_valid(XrProgramSemanticSourceLocator locator) {
    return locator.kind == AST_FUNCTION_DECL && verifier_locator_valid(locator);
}

static bool verifier_type_locator_valid(XrProgramSemanticSourceLocator locator) {
    return locator.kind == AST_STRUCT_DECL && verifier_locator_valid(locator);
}

static bool verifier_call_locator_valid(XrProgramSemanticSourceLocator locator) {
    return locator.kind == AST_CALL_EXPR && verifier_locator_valid(locator);
}

static bool verifier_point_before(uint32_t left_line, uint32_t left_column, uint32_t right_line,
                                  uint32_t right_column) {
    return left_line < right_line || (left_line == right_line && left_column < right_column);
}

static bool verifier_locator_contains(XrProgramSemanticSourceLocator outer,
                                      XrProgramSemanticSourceLocator inner) {
    return verifier_point_before(outer.start_line, outer.start_column, inner.start_line,
                                 inner.start_column) &&
           verifier_point_before(inner.end_line, inner.end_column, outer.end_line,
                                 outer.end_column);
}

static bool verifier_locator_equal(XrProgramSemanticSourceLocator left,
                                   XrProgramSemanticSourceLocator right) {
    return left.kind == right.kind && left.start_line == right.start_line &&
           left.start_column == right.start_column && left.end_line == right.end_line &&
           left.end_column == right.end_column;
}

static XrStableId verifier_source_callsite_identity(const XrProgramSemanticModuleRecord *module,
                                                    const XrProgramSemanticFunctionRecord *caller,
                                                    XrProgramSemanticSourceLocator locator) {
    static const uint8_t domain[] = "xray-source-program-callsite-v2";
    XrSHA256Context context;
    xr_sha256_init(&context);
    verifier_hash_framed_bytes(&context, domain, sizeof(domain) - 1u);
    verifier_hash_u32(&context, UINT32_C(1));
    verifier_hash_framed_fingerprint(&context, module->source_fingerprint);
    verifier_hash_framed_id(&context, module->module_identity);
    verifier_hash_framed_id(&context, caller->declaration_identity);
    verifier_hash_u32(&context, locator.kind);
    verifier_hash_u32(&context, locator.start_line);
    verifier_hash_u32(&context, locator.start_column);
    verifier_hash_u32(&context, locator.end_line);
    verifier_hash_u32(&context, locator.end_column);
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

static int find_type(const XrProgramSemanticClosure *closure, XrStableId id) {
    uint32_t low = 0;
    uint32_t high = closure ? closure->type_count : 0;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2u;
        int order = verifier_id_compare(closure->types[middle].id, id);
        if (order < 0)
            low = middle + 1u;
        else
            high = middle;
    }
    return low < (closure ? closure->type_count : 0) &&
                   verifier_id_equal(closure->types[low].id, id)
               ? (int) low
               : -1;
}

static bool has_direct_dependency(const XrProgramSemanticClosure *closure, XrStableId source,
                                  XrStableId dependency) {
    for (uint32_t i = 0; i < closure->dependency_count; i++) {
        const XrProgramSemanticDependencyRecord *row = &closure->dependencies[i];
        if (verifier_id_equal(row->source_module, source) &&
            verifier_id_equal(row->dependency_module, dependency))
            return true;
    }
    return false;
}

static bool verify_module_rows(const XrProgramSemanticClosure *closure, char *error,
                               size_t error_size) {
    for (uint32_t i = 0; i < closure->module_count; i++) {
        const XrProgramSemanticModuleRecord *row = &closure->modules[i];
        XrFingerprint expected_exports = verifier_empty_export_fingerprint(row->module_identity);
        if (verifier_stable_id_zero(row->module_identity) ||
            verifier_fingerprint_zero(row->module_authority_fingerprint) ||
            !verifier_id_equal(row->module_identity, verifier_source_module_identity(
                                                         row->module_authority_fingerprint)) ||
            verifier_fingerprint_zero(row->source_fingerprint) ||
            verifier_fingerprint_zero(row->export_fingerprint) ||
            ((closure->family == XR_PROGRAM_SEMANTIC_FAMILY_SCALAR_DIRECT_CALL ||
              closure->family == XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_AGGREGATE_DIRECT_CALL) &&
             !verifier_fingerprint_equal(row->export_fingerprint, expected_exports)) ||
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
                      (source_order == 0 && verifier_id_compare(previous->dependency_module,
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

static bool verify_type_rows(const XrProgramSemanticClosure *closure, char *error,
                             size_t error_size) {
    for (uint32_t i = 0; i < closure->type_count; i++) {
        const XrProgramSemanticTypeRecord *row = &closure->types[i];
        const uint8_t required =
            XR_PROGRAM_SEMANTIC_TYPE_NONNULLABLE | XR_PROGRAM_SEMANTIC_TYPE_NONGENERIC |
            XR_PROGRAM_SEMANTIC_TYPE_VALUE | XR_PROGRAM_SEMANTIC_TYPE_POINTER_FREE;
        bool opaque = row->kind == XR_PROGRAM_SEMANTIC_TYPE_OPAQUE;
        bool scalar = row->kind == XR_PROGRAM_SEMANTIC_TYPE_EXACT_SCALAR;
        bool aggregate = row->kind == XR_PROGRAM_SEMANTIC_TYPE_LEAF_VALUE_AGGREGATE;
        bool typed_shape = scalar || aggregate;
        bool locator_empty =
            row->declaration_locator.kind == 0 && row->declaration_locator.start_line == 0 &&
            row->declaration_locator.start_column == 0 && row->declaration_locator.end_line == 0 &&
            row->declaration_locator.end_column == 0;
        XrStableId scalar_declaration =
            scalar ? verifier_exact_scalar_declaration(row->exact_scalar) : (XrStableId) {{0}};
        if (row->reserved != 0 || (!opaque && !typed_shape) ||
            row->field_begin > closure->type_field_count ||
            row->field_count > closure->type_field_count - row->field_begin ||
            (row->field_count != 0 && !closure->type_fields) ||
            (opaque && (row->field_count || row->exact_scalar || row->flags)) ||
            (scalar &&
             (row->field_count || row->flags != required || !locator_empty ||
              !xr_exact_scalar_by_id((XrExactScalarId) row->exact_scalar) ||
              !verifier_id_equal(row->module_identity, verifier_exact_scalar_registry_owner()) ||
              !verifier_id_equal(row->declaration_identity, scalar_declaration) ||
              !verifier_id_equal(
                  row->concrete_instance_identity,
                  verifier_exact_scalar_instance(scalar_declaration, row->exact_scalar)))) ||
            (aggregate &&
             (row->field_count == 0 || !verifier_type_locator_valid(row->declaration_locator) ||
              row->exact_scalar != XR_EXACT_SCALAR_NONE || row->flags != required)))
            return reject(error, error_size, "XR_SEM_0013",
                          "concrete type row bounds or kind are invalid");
        XrFingerprint expected_shape = {{0}};
        XrFingerprint expected_ownership = {{0}};
        if (typed_shape)
            verifier_typed_type_fingerprints(closure, row, &expected_shape, &expected_ownership);
        int aggregate_module = aggregate ? find_module(closure, row->module_identity) : -1;
        XrStableId expected_declaration = row->declaration_identity;
        XrStableId expected_instance = row->concrete_instance_identity;
        if (aggregate_module >= 0) {
            expected_declaration = verifier_leaf_aggregate_declaration(
                closure, row, &closure->modules[(uint32_t) aggregate_module]);
            expected_instance = verifier_leaf_aggregate_instance(expected_declaration);
        }
        if (verifier_stable_id_zero(row->declaration_identity) ||
            verifier_stable_id_zero(row->concrete_instance_identity) ||
            verifier_fingerprint_zero(row->shape_fingerprint) ||
            verifier_fingerprint_zero(row->ownership_fingerprint) ||
            (!scalar && find_module(closure, row->module_identity) < 0) ||
            (aggregate &&
             (!verifier_id_equal(row->declaration_identity, expected_declaration) ||
              !verifier_id_equal(row->concrete_instance_identity, expected_instance))) ||
            !verifier_id_equal(row->id, verifier_type_identity(closure->policy_fingerprint, row)) ||
            (typed_shape &&
             (!verifier_fingerprint_equal(row->shape_fingerprint, expected_shape) ||
              !verifier_fingerprint_equal(row->ownership_fingerprint, expected_ownership))) ||
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
    uint32_t field_cursor = 0;
    for (uint32_t i = 0; i < closure->type_count; i++) {
        const XrProgramSemanticTypeRecord *owner = &closure->types[i];
        if (owner->field_begin != field_cursor)
            return reject(error, error_size, "XR_SEM_0019",
                          "concrete type field ranges are not dense");
        for (uint32_t ordinal = 0; ordinal < owner->field_count; ordinal++, field_cursor++) {
            const XrProgramSemanticTypeFieldRecord *field = &closure->type_fields[field_cursor];
            int child = find_type(closure, field->field_type);
            if (!verifier_id_equal(field->owner_type, owner->id) ||
                field->declaration_ordinal != ordinal || field->reserved != 0 || child < 0 ||
                closure->types[child].kind != XR_PROGRAM_SEMANTIC_TYPE_EXACT_SCALAR)
                return reject(error, error_size, "XR_SEM_0019",
                              "concrete type field row is invalid");
        }
    }
    if (field_cursor != closure->type_field_count)
        return reject(error, error_size, "XR_SEM_0019",
                      "concrete type field table has unowned rows");
    return true;
}

static bool verify_bounded_family_types(const XrProgramSemanticClosure *closure, char *error,
                                        size_t error_size) {
    if (closure->family != XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_AGGREGATE_DIRECT_CALL)
        return true;
    uint8_t *referenced = (uint8_t *) xr_calloc(closure->type_count, sizeof(*referenced));
    if (!referenced)
        return reject(error, error_size, "XR_EXEC_5003",
                      "program family verification allocation failed");
    uint32_t aggregate_count = 0;
    bool valid = true;
    for (uint32_t i = 0; valid && i < closure->type_count; i++) {
        const XrProgramSemanticTypeRecord *row = &closure->types[i];
        if (row->kind == XR_PROGRAM_SEMANTIC_TYPE_LEAF_VALUE_AGGREGATE) {
            aggregate_count++;
            for (uint32_t field = 0; field < row->field_count; field++) {
                const XrProgramSemanticTypeFieldRecord *field_row =
                    &closure->type_fields[row->field_begin + field];
                int child = find_type(closure, field_row->field_type);
                valid = child >= 0 &&
                        closure->types[child].kind == XR_PROGRAM_SEMANTIC_TYPE_EXACT_SCALAR;
                if (valid)
                    referenced[child] = 1u;
            }
        } else if (row->kind != XR_PROGRAM_SEMANTIC_TYPE_EXACT_SCALAR) {
            valid = false;
        }
        if (row->kind == XR_PROGRAM_SEMANTIC_TYPE_EXACT_SCALAR) {
            for (uint32_t prior = 0; valid && prior < i; prior++)
                if (closure->types[prior].kind == XR_PROGRAM_SEMANTIC_TYPE_EXACT_SCALAR &&
                    closure->types[prior].exact_scalar == row->exact_scalar)
                    valid = false;
        }
    }
    for (uint32_t i = 0; valid && i < closure->type_count; i++)
        if (closure->types[i].kind == XR_PROGRAM_SEMANTIC_TYPE_EXACT_SCALAR && !referenced[i])
            valid = false;
    xr_free(referenced);
    return (valid && aggregate_count == 1u) ||
           reject(error, error_size, "XR_SEM_0019",
                  "leaf-value aggregate family type coverage is not exact");
}

static XrFingerprint
verifier_leaf_signature(XrStableId aggregate_type,
                        const XrProgramSemanticFunctionParameterRecord *parameters,
                        uint32_t parameter_count) {
    static const uint8_t domain[] = "xray-leaf-value-function-signature-v1";
    XrSHA256Context context;
    XrFingerprint result;
    xr_sha256_init(&context);
    verifier_hash_framed_bytes(&context, domain, sizeof(domain) - 1u);
    verifier_hash_u32(&context, XR_PROGRAM_SEMANTIC_CLOSURE_SCHEMA_VERSION);
    verifier_hash_framed_id(&context, aggregate_type);
    verifier_hash_u32(&context, parameter_count);
    for (uint32_t i = 0; i < parameter_count; i++) {
        verifier_hash_framed_id(&context, parameters[i].type);
        verifier_hash_u32(&context, parameters[i].mode);
    }
    xr_sha256_final(&context, result.bytes);
    return result;
}

static XrFingerprint verifier_leaf_effect(void) {
    static const uint8_t domain[] = "xray-leaf-value-pure-effect-v1";
    XrSHA256Context context;
    XrFingerprint result;
    xr_sha256_init(&context);
    verifier_hash_framed_bytes(&context, domain, sizeof(domain) - 1u);
    verifier_hash_u32(&context, XR_PROGRAM_SEMANTIC_CLOSURE_SCHEMA_VERSION);
    verifier_hash_u32(&context, 0);
    xr_sha256_final(&context, result.bytes);
    return result;
}

static XrFingerprint verifier_leaf_policy(void) {
    static const uint8_t domain[] = "xray-leaf-value-direct-call-policy-v1";
    XrSHA256Context context;
    XrFingerprint result;
    xr_sha256_init(&context);
    verifier_hash_framed_bytes(&context, domain, sizeof(domain) - 1u);
    verifier_hash_u32(&context, XR_PROGRAM_SEMANTIC_CLOSURE_SCHEMA_VERSION);
    verifier_hash_u32(&context, 1);
    verifier_hash_u32(&context, 2);
    verifier_hash_u32(&context, 1);
    xr_sha256_final(&context, result.bytes);
    return result;
}

static XrFingerprint verifier_leaf_call_contract(XrStableId aggregate_type, XrFingerprint signature,
                                                 XrFingerprint effect) {
    static const uint8_t domain[] = "xray-leaf-value-direct-call-contract-v1";
    XrSHA256Context context;
    XrFingerprint result;
    xr_sha256_init(&context);
    verifier_hash_framed_bytes(&context, domain, sizeof(domain) - 1u);
    verifier_hash_u32(&context, XR_PROGRAM_SEMANTIC_CLOSURE_SCHEMA_VERSION);
    verifier_hash_framed_id(&context, aggregate_type);
    verifier_hash_framed_fingerprint(&context, signature);
    verifier_hash_framed_fingerprint(&context, effect);
    xr_sha256_final(&context, result.bytes);
    return result;
}

static bool verify_function_rows(const XrProgramSemanticClosure *closure, char *error,
                                 size_t error_size) {
    uint32_t roots = 0;
    uint32_t leaf_entries = 0;
    uint32_t leaf_callees = 0;
    uint32_t scalar_entries = 0;
    uint32_t scalar_callees = 0;
    uint32_t parameter_cursor = 0;
    for (uint32_t i = 0; i < closure->function_count; i++) {
        const XrProgramSemanticFunctionRecord *row = &closure->functions[i];
        bool leaf = closure->family == XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_AGGREGATE_DIRECT_CALL;
        bool scalar = closure->family == XR_PROGRAM_SEMANTIC_FAMILY_SCALAR_DIRECT_CALL;
        bool leaf_entry = leaf && (row->flags & XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY) != 0;
        if (row->parameter_begin > closure->function_parameter_count ||
            row->parameter_count > closure->function_parameter_count - row->parameter_begin ||
            (row->parameter_count && !closure->function_parameters) ||
            (leaf ? (row->parameter_count > 1 || verifier_stable_id_zero(row->return_type))
                  : (!verifier_stable_id_zero(row->return_type) || row->parameter_count != 0)))
            return reject(error, error_size, "XR_SEM_0019",
                          "concrete function signature rows are out of bounds");
        const XrProgramSemanticFunctionParameterRecord *parameters =
            row->parameter_count ? &closure->function_parameters[row->parameter_begin] : NULL;
        XrFingerprint expected_signature =
            leaf     ? verifier_leaf_signature(row->return_type, parameters, row->parameter_count)
            : scalar ? verifier_scalar_signature(
                           row->flags == XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY ? 0u : 1u)
                     : row->signature_fingerprint;
        XrFingerprint expected_effect = leaf     ? verifier_leaf_effect()
                                        : scalar ? verifier_scalar_effect()
                                                 : row->effect_fingerprint;
        int function_module = find_module(closure, row->module_identity);
        XrStableId expected_declaration = row->declaration_identity;
        XrStableId expected_instance = row->concrete_instance_identity;
        if (leaf && function_module >= 0) {
            expected_declaration = verifier_leaf_function_declaration(
                &closure->modules[(uint32_t) function_module], row);
            expected_instance =
                verifier_leaf_function_instance(expected_declaration, expected_signature);
        } else if (scalar && function_module >= 0) {
            expected_declaration = verifier_scalar_function_declaration(
                &closure->modules[(uint32_t) function_module], row);
            expected_instance =
                verifier_scalar_function_instance(&closure->modules[(uint32_t) function_module],
                                                  expected_declaration, expected_signature);
        }
        if (verifier_stable_id_zero(row->declaration_identity) ||
            verifier_stable_id_zero(row->concrete_instance_identity) ||
            !verifier_function_locator_valid(row->declaration_locator) ||
            verifier_fingerprint_zero(row->signature_fingerprint) ||
            verifier_fingerprint_zero(row->effect_fingerprint) ||
            !verifier_fingerprint_equal(row->signature_fingerprint, expected_signature) ||
            !verifier_fingerprint_equal(row->effect_fingerprint, expected_effect) ||
            ((leaf || scalar) &&
             (!verifier_id_equal(row->declaration_identity, expected_declaration) ||
              !verifier_id_equal(row->concrete_instance_identity, expected_instance))) ||
            (leaf && (find_type(closure, row->return_type) < 0 ||
                      closure->types[find_type(closure, row->return_type)].kind !=
                          XR_PROGRAM_SEMANTIC_TYPE_LEAF_VALUE_AGGREGATE ||
                      row->capability_mask != 0 ||
                      (leaf_entry ? row->parameter_count != 0
                                  : row->flags != 0 || row->parameter_count != 1))) ||
            (scalar && (row->parameter_count != 0 || !verifier_stable_id_zero(row->return_type) ||
                        (row->flags != XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY && row->flags != 0) ||
                        row->capability_mask != 0)) ||
            function_module < 0 ||
            (row->flags &
             ~(XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY | XR_PROGRAM_SEMANTIC_FUNCTION_EXPORTED)) != 0 ||
            memcmp(row->reserved, (uint8_t[7]) {0}, sizeof(row->reserved)) != 0 ||
            !verifier_id_equal(row->id,
                               verifier_function_identity(closure->policy_fingerprint, row)) ||
            (i && verifier_id_compare(closure->functions[i - 1u].id, row->id) >= 0))
            return reject(error, error_size, "XR_SEM_0013",
                          "concrete function identity is incomplete or non-canonical");
        if (row->parameter_begin != parameter_cursor)
            return reject(error, error_size, "XR_SEM_0019",
                          "concrete function parameter ranges are not dense");
        for (uint32_t parameter = 0; parameter < row->parameter_count;
             parameter++, parameter_cursor++) {
            const XrProgramSemanticFunctionParameterRecord *record =
                &closure->function_parameters[parameter_cursor];
            int type = find_type(closure, record->type);
            if (!verifier_id_equal(record->owner_function, row->id) ||
                record->declaration_ordinal != parameter || record->mode != XR_PARAM_READ ||
                memcmp(record->reserved, (uint8_t[3]) {0}, sizeof(record->reserved)) != 0 ||
                type < 0 ||
                closure->types[type].kind != XR_PROGRAM_SEMANTIC_TYPE_LEAF_VALUE_AGGREGATE ||
                !verifier_id_equal(record->type, row->return_type))
                return reject(error, error_size, "XR_SEM_0019",
                              "concrete function parameter row is invalid");
        }
        for (uint32_t j = 0; j < i; j++)
            if (verifier_id_equal(closure->functions[j].module_identity, row->module_identity) &&
                verifier_id_equal(closure->functions[j].declaration_identity,
                                  row->declaration_identity) &&
                verifier_id_equal(closure->functions[j].concrete_instance_identity,
                                  row->concrete_instance_identity))
                return reject(error, error_size, "XR_SEM_0019",
                              "concrete function declaration is duplicated");
        for (uint32_t j = 0; j < i; j++)
            if (verifier_id_equal(closure->functions[j].module_identity, row->module_identity) &&
                verifier_locator_equal(closure->functions[j].declaration_locator,
                                       row->declaration_locator))
                return reject(error, error_size, "XR_SEM_0019",
                              "concrete function declaration locator is duplicated");
        if (row->flags != 0)
            roots++;
        if (leaf) {
            leaf_entries += leaf_entry ? 1u : 0u;
            leaf_callees += row->flags == 0 ? 1u : 0u;
        }
        if (scalar) {
            scalar_entries += row->flags == XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY ? 1u : 0u;
            scalar_callees += row->flags == 0 ? 1u : 0u;
        }
    }
    if (parameter_cursor != closure->function_parameter_count)
        return reject(error, error_size, "XR_SEM_0019",
                      "concrete function parameter table has unowned rows");
    if (closure->family == XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_AGGREGATE_DIRECT_CALL &&
        (leaf_entries != 1u || leaf_callees != 1u || roots != 1u))
        return reject(error, error_size, "XR_SEM_0019",
                      "leaf aggregate family requires one entry and one unary callee");
    if (closure->family == XR_PROGRAM_SEMANTIC_FAMILY_SCALAR_DIRECT_CALL &&
        (scalar_entries != 1u || scalar_callees != 1u || roots != 1u))
        return reject(error, error_size, "XR_SEM_0019",
                      "scalar family requires one entry and one unary callee");
    return roots > 0 || reject(error, error_size, "XR_SEM_0019",
                               "program closure requires a concrete entry or export root");
}

static bool verify_call_rows(const XrProgramSemanticClosure *closure, char *error,
                             size_t error_size) {
    for (uint32_t i = 0; i < closure->call_count; i++) {
        const XrProgramSemanticCallRecord *row = &closure->calls[i];
        int caller = find_function(closure, row->caller_function);
        int callee = find_function(closure, row->callee_function);
        XrFingerprint expected_contract = row->contract_fingerprint;
        if (callee >= 0 &&
            closure->family == XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_AGGREGATE_DIRECT_CALL) {
            const XrProgramSemanticFunctionRecord *callee_row =
                &closure->functions[(uint32_t) callee];
            expected_contract = verifier_leaf_call_contract(callee_row->return_type,
                                                            callee_row->signature_fingerprint,
                                                            callee_row->effect_fingerprint);
        } else if (callee >= 0 &&
                   closure->family == XR_PROGRAM_SEMANTIC_FAMILY_SCALAR_DIRECT_CALL) {
            expected_contract = verifier_scalar_call_contract();
        }
        if (verifier_stable_id_zero(row->callsite_identity) ||
            verifier_fingerprint_zero(row->contract_fingerprint) ||
            !verifier_fingerprint_equal(row->contract_fingerprint, expected_contract) ||
            !verifier_call_locator_valid(row->locator) || caller < 0 || callee < 0 ||
            !verifier_id_equal(row->id, verifier_call_identity(closure->policy_fingerprint, row)))
            return reject(error, error_size, "XR_SEM_0013",
                          "resolved call identity is incomplete or non-canonical");
        const XrProgramSemanticFunctionRecord *caller_row = &closure->functions[(uint32_t) caller];
        const XrProgramSemanticFunctionRecord *callee_row = &closure->functions[(uint32_t) callee];
        if (!verifier_locator_contains(caller_row->declaration_locator, row->locator))
            return reject(error, error_size, "XR_SEM_0019",
                          "resolved call locator is outside its caller declaration");
        if (closure->family == XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_AGGREGATE_DIRECT_CALL &&
            ((caller_row->flags & XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY) == 0 ||
             caller_row->parameter_count != 0 || callee_row->flags != 0 ||
             callee_row->parameter_count != 1))
            return reject(error, error_size, "XR_SEM_0019",
                          "leaf aggregate call does not join entry to unary callee");
        int caller_module_index = find_module(closure, caller_row->module_identity);
        if (caller_module_index < 0 ||
            !verifier_id_equal(
                row->callsite_identity,
                verifier_source_callsite_identity(&closure->modules[(uint32_t) caller_module_index],
                                                  caller_row, row->locator)))
            return reject(error, error_size, "XR_SEM_0019",
                          "resolved call locator does not match its stable callsite");
        for (uint32_t j = 0; j < i; j++) {
            int previous_caller = find_function(closure, closure->calls[j].caller_function);
            if (previous_caller >= 0 &&
                verifier_id_equal(closure->functions[(uint32_t) previous_caller].module_identity,
                                  caller_row->module_identity) &&
                verifier_locator_equal(closure->calls[j].locator, row->locator))
                return reject(error, error_size, "XR_SEM_0019",
                              "resolved call source locator is duplicated");
            if (verifier_id_equal(closure->calls[j].callsite_identity, row->callsite_identity))
                return reject(error, error_size, "XR_SEM_0019",
                              "resolved callsite identity is duplicated");
        }
        if (i && verifier_id_compare(closure->calls[i - 1u].id, row->id) >= 0)
            return reject(error, error_size, "XR_SEM_0013",
                          "resolved call identity is incomplete or non-canonical");
        XrStableId caller_module = caller_row->module_identity;
        XrStableId callee_module = closure->functions[(uint32_t) callee].module_identity;
        if (!verifier_id_equal(caller_module, callee_module) &&
            !has_direct_dependency(closure, caller_module, callee_module))
            return reject(error, error_size, "XR_SEM_0019",
                          "cross-module call lacks an exact dependency contract");
    }
    return true;
}

static bool verify_module_graph(const XrProgramSemanticClosure *closure, char *error,
                                size_t error_size) {
    uint32_t count = closure->module_count;
    uint32_t *begin = (uint32_t *) xr_calloc((size_t) count + 1u, sizeof(*begin));
    uint32_t *targets =
        (uint32_t *) xr_malloc((size_t) closure->dependency_count * sizeof(*targets));
    uint32_t *indegree = (uint32_t *) xr_calloc(count, sizeof(*indegree));
    uint32_t *queue = (uint32_t *) xr_malloc((size_t) count * sizeof(*queue));
    uint8_t *reachable = (uint8_t *) xr_calloc(count, sizeof(*reachable));
    if (!begin || !indegree || !queue || !reachable || (closure->dependency_count && !targets)) {
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
        uint32_t root = (uint32_t) find_module(closure, closure->functions[i].module_identity);
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

static bool verify_call_graph(const XrProgramSemanticClosure *closure, char *error,
                              size_t error_size) {
    uint32_t count = closure->function_count;
    uint32_t *begin = (uint32_t *) xr_calloc((size_t) count + 1u, sizeof(*begin));
    uint32_t *targets = (uint32_t *) xr_malloc((size_t) closure->call_count * sizeof(*targets));
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
    static const uint8_t domain[] = "xray-program-semantic-closure-v4\0";
    XrSHA256Context context;
    xr_sha256_init(&context);
    xr_sha256_update(&context, domain, sizeof(domain) - 1u);
    verifier_hash_u32(&context, closure->schema);
    verifier_hash_u32(&context, closure->family);
    verifier_hash_fingerprint(&context, closure->policy_fingerprint);
    verifier_hash_u32(&context, closure->module_count);
    verifier_hash_u32(&context, closure->dependency_count);
    verifier_hash_u32(&context, closure->type_count);
    verifier_hash_u32(&context, closure->type_field_count);
    verifier_hash_u32(&context, closure->function_count);
    verifier_hash_u32(&context, closure->function_parameter_count);
    verifier_hash_u32(&context, closure->call_count);
    for (uint32_t i = 0; i < closure->module_count; i++) {
        verifier_hash_id(&context, closure->modules[i].module_identity);
        verifier_hash_fingerprint(&context, closure->modules[i].module_authority_fingerprint);
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
        verifier_hash_u32(&context, closure->types[i].declaration_locator.kind);
        verifier_hash_u32(&context, closure->types[i].declaration_locator.start_line);
        verifier_hash_u32(&context, closure->types[i].declaration_locator.start_column);
        verifier_hash_u32(&context, closure->types[i].declaration_locator.end_line);
        verifier_hash_u32(&context, closure->types[i].declaration_locator.end_column);
        verifier_hash_fingerprint(&context, closure->types[i].shape_fingerprint);
        verifier_hash_fingerprint(&context, closure->types[i].ownership_fingerprint);
        verifier_hash_u32(&context, closure->types[i].field_begin);
        verifier_hash_u32(&context, closure->types[i].field_count);
        verifier_hash_u32(&context, closure->types[i].kind);
        verifier_hash_u32(&context, closure->types[i].exact_scalar);
        verifier_hash_u32(&context, closure->types[i].flags);
    }
    for (uint32_t i = 0; i < closure->type_field_count; i++) {
        verifier_hash_id(&context, closure->type_fields[i].owner_type);
        verifier_hash_id(&context, closure->type_fields[i].field_type);
        verifier_hash_u32(&context, closure->type_fields[i].declaration_ordinal);
    }
    for (uint32_t i = 0; i < closure->function_count; i++) {
        verifier_hash_id(&context, closure->functions[i].id);
        verifier_hash_id(&context, closure->functions[i].module_identity);
        verifier_hash_id(&context, closure->functions[i].declaration_identity);
        verifier_hash_id(&context, closure->functions[i].concrete_instance_identity);
        verifier_hash_u32(&context, closure->functions[i].declaration_locator.kind);
        verifier_hash_u32(&context, closure->functions[i].declaration_locator.start_line);
        verifier_hash_u32(&context, closure->functions[i].declaration_locator.start_column);
        verifier_hash_u32(&context, closure->functions[i].declaration_locator.end_line);
        verifier_hash_u32(&context, closure->functions[i].declaration_locator.end_column);
        verifier_hash_fingerprint(&context, closure->functions[i].signature_fingerprint);
        verifier_hash_fingerprint(&context, closure->functions[i].effect_fingerprint);
        verifier_hash_id(&context, closure->functions[i].return_type);
        verifier_hash_u32(&context, closure->functions[i].parameter_begin);
        verifier_hash_u32(&context, closure->functions[i].parameter_count);
        verifier_hash_u64(&context, closure->functions[i].capability_mask);
        xr_sha256_update(&context, &closure->functions[i].flags, 1u);
    }
    for (uint32_t i = 0; i < closure->function_parameter_count; i++) {
        verifier_hash_id(&context, closure->function_parameters[i].owner_function);
        verifier_hash_id(&context, closure->function_parameters[i].type);
        verifier_hash_u32(&context, closure->function_parameters[i].declaration_ordinal);
        verifier_hash_u32(&context, closure->function_parameters[i].mode);
    }
    for (uint32_t i = 0; i < closure->call_count; i++) {
        verifier_hash_id(&context, closure->calls[i].id);
        verifier_hash_id(&context, closure->calls[i].callsite_identity);
        verifier_hash_u32(&context, closure->calls[i].locator.kind);
        verifier_hash_u32(&context, closure->calls[i].locator.start_line);
        verifier_hash_u32(&context, closure->calls[i].locator.start_column);
        verifier_hash_u32(&context, closure->calls[i].locator.end_line);
        verifier_hash_u32(&context, closure->calls[i].locator.end_column);
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

bool xr_program_semantic_closure_verify(const XrProgramSemanticClosure *closure, char *error,
                                        size_t error_size) {
    bool valid_state =
        closure &&
        ((closure->state == XR_PROGRAM_SEMANTIC_CLOSURE_VERIFYING && closure->verified == 0) ||
         (closure->state == XR_PROGRAM_SEMANTIC_CLOSURE_FROZEN && closure->verified == 1u));
    if (!closure || !valid_state || closure->reserved != 0 ||
        closure->failure_kind != XR_PROGRAM_SEMANTIC_CLOSURE_FAILURE_NONE ||
        closure->schema != XR_PROGRAM_SEMANTIC_CLOSURE_SCHEMA_VERSION ||
        closure->family < XR_PROGRAM_SEMANTIC_FAMILY_GENERAL ||
        closure->family >= XR_PROGRAM_SEMANTIC_FAMILY_COUNT ||
        verifier_fingerprint_zero(closure->policy_fingerprint) ||
        verifier_fingerprint_zero(closure->fingerprint) ||
        verifier_bytes_are_zero(closure->generation_id.bytes,
                                sizeof(closure->generation_id.bytes)) ||
        closure->module_count == 0 || closure->function_count == 0 ||
        closure->limits.max_modules == 0 ||
        closure->limits.max_modules > XR_PROGRAM_SEMANTIC_CLOSURE_MAX_MODULES ||
        closure->limits.max_dependencies > XR_PROGRAM_SEMANTIC_CLOSURE_MAX_DEPENDENCIES ||
        closure->limits.max_types > XR_PROGRAM_SEMANTIC_CLOSURE_MAX_TYPES ||
        closure->limits.max_type_fields > XR_PROGRAM_SEMANTIC_CLOSURE_MAX_TYPE_FIELDS ||
        closure->limits.max_functions == 0 ||
        closure->limits.max_functions > XR_PROGRAM_SEMANTIC_CLOSURE_MAX_FUNCTIONS ||
        closure->limits.max_function_parameters >
            XR_PROGRAM_SEMANTIC_CLOSURE_MAX_FUNCTION_PARAMETERS ||
        closure->limits.max_calls > XR_PROGRAM_SEMANTIC_CLOSURE_MAX_CALLS ||
        closure->module_count > closure->limits.max_modules ||
        closure->dependency_count > closure->limits.max_dependencies ||
        closure->type_count > closure->limits.max_types ||
        closure->type_field_count > closure->limits.max_type_fields ||
        closure->function_count > closure->limits.max_functions ||
        closure->function_parameter_count > closure->limits.max_function_parameters ||
        closure->call_count > closure->limits.max_calls)
        return reject(error, error_size, "XR_SEM_0019",
                      "program semantic closure header is incomplete");
    if ((closure->module_count && !closure->modules) ||
        (closure->dependency_count && !closure->dependencies) ||
        (closure->type_count && !closure->types) ||
        (closure->type_field_count && !closure->type_fields) ||
        (closure->function_count && !closure->functions) ||
        (closure->function_parameter_count && !closure->function_parameters) ||
        (closure->call_count && !closure->calls))
        return reject(error, error_size, "XR_SEM_0019",
                      "program semantic closure table storage is incomplete");
    if ((closure->family == XR_PROGRAM_SEMANTIC_FAMILY_SCALAR_DIRECT_CALL &&
         (closure->module_count != 1 || closure->dependency_count != 0 ||
          closure->type_count != 0 || closure->type_field_count != 0 ||
          closure->function_count != 2 || closure->call_count != 1 ||
          !verifier_fingerprint_equal(closure->policy_fingerprint, verifier_scalar_policy()))) ||
        (closure->family == XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_AGGREGATE_DIRECT_CALL &&
         (closure->module_count != 1 || closure->dependency_count != 0 || closure->type_count < 2 ||
          closure->type_field_count == 0 || closure->function_count != 2 ||
          closure->function_parameter_count != 1 || closure->call_count != 1 ||
          !verifier_fingerprint_equal(closure->policy_fingerprint, verifier_leaf_policy()))))
        return reject(error, error_size, "XR_SEM_0019",
                      "program semantic family does not match its typed facts");
    if (!verify_module_rows(closure, error, error_size) ||
        !verify_type_rows(closure, error, error_size) ||
        !verify_bounded_family_types(closure, error, error_size) ||
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
