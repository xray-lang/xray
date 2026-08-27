/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_source_semantic_identity.c - Canonical source identity authority
 */

#include "xr_source_semantic_identity.h"
#include "../../base/xsha256.h"
#include "../../runtime/value/xtype.h"
#include "../../shared/xr_exact_scalar_registry.h"
#include <string.h>

static void put_u32(XrSHA256Context *context, uint32_t value) {
    uint8_t bytes[4];
    for (uint32_t i = 0; i < sizeof(bytes); i++)
        bytes[i] = (uint8_t) (value >> (i * 8u));
    xr_sha256_update(context, bytes, sizeof(bytes));
}

static void put_u64(XrSHA256Context *context, uint64_t value) {
    uint8_t bytes[8];
    for (uint32_t i = 0; i < sizeof(bytes); i++)
        bytes[i] = (uint8_t) (value >> (i * 8u));
    xr_sha256_update(context, bytes, sizeof(bytes));
}

static void put_bytes(XrSHA256Context *context, const uint8_t *bytes, size_t size) {
    put_u64(context, (uint64_t) size);
    if (size)
        xr_sha256_update(context, bytes, size);
}

static void begin(XrSHA256Context *context, const char *domain) {
    xr_sha256_init(context);
    put_bytes(context, (const uint8_t *) domain, strlen(domain));
    put_u32(context, XR_SOURCE_SEMANTIC_IDENTITY_VERSION);
}

static void begin_program_identity(XrSHA256Context *context, const char *domain) {
    xr_sha256_init(context);
    put_bytes(context, (const uint8_t *) domain, strlen(domain));
    put_u32(context, XR_PROGRAM_SEMANTIC_CLOSURE_SCHEMA_VERSION);
}

static void put_id(XrSHA256Context *context, XrStableId id) {
    put_bytes(context, id.bytes, sizeof(id.bytes));
}

static void put_fingerprint(XrSHA256Context *context, XrFingerprint fingerprint) {
    put_bytes(context, fingerprint.bytes, sizeof(fingerprint.bytes));
}

static void put_locator(XrSHA256Context *context, XrProgramSemanticSourceLocator locator) {
    put_u32(context, locator.kind);
    put_u32(context, locator.start_line);
    put_u32(context, locator.start_column);
    put_u32(context, locator.end_line);
    put_u32(context, locator.end_column);
}

static bool bytes_zero(const uint8_t *bytes, size_t size) {
    uint8_t combined = 0;
    for (size_t i = 0; i < size; i++)
        combined |= bytes[i];
    return combined == 0;
}

static XrFingerprint finish_fingerprint(XrSHA256Context *context) {
    XrFingerprint result;
    xr_sha256_final(context, result.bytes);
    return result;
}

static XrStableId finish_id(XrSHA256Context *context) {
    uint8_t digest[XR_FINGERPRINT_BYTES];
    XrStableId result;
    xr_sha256_final(context, digest);
    memcpy(result.bytes, digest, sizeof(result.bytes));
    return result;
}

XR_FUNC bool xr_source_semantic_module_authority(const char *canonical_module,
                                                 XrFingerprint source_fingerprint,
                                                 XrProgramSemanticModuleInput *out,
                                                 XrFingerprint *authority_fingerprint) {
    if (out)
        memset(out, 0, sizeof(*out));
    if (authority_fingerprint)
        memset(authority_fingerprint, 0, sizeof(*authority_fingerprint));
    if (!out || !canonical_module || canonical_module[0] == '\0' ||
        bytes_zero(source_fingerprint.bytes, sizeof(source_fingerprint.bytes)))
        return false;
    XrSHA256Context context;
    begin(&context, "xray-source-module-authority-v1");
    put_bytes(&context, (const uint8_t *) canonical_module, strlen(canonical_module));
    XrFingerprint authority = finish_fingerprint(&context);
    begin(&context, "xray-source-module-identity-v1");
    put_fingerprint(&context, authority);
    XrStableId identity = finish_id(&context);
    begin(&context, "xray-source-module-empty-exports-v1");
    put_id(&context, identity);
    put_u32(&context, 0);
    *out = (XrProgramSemanticModuleInput) {
        .module_identity = identity,
        .module_authority_fingerprint = authority,
        .source_fingerprint = source_fingerprint,
        .export_fingerprint = finish_fingerprint(&context),
    };
    if (authority_fingerprint)
        *authority_fingerprint = authority;
    return true;
}

XR_FUNC bool xr_source_semantic_callsite_identity(XrFingerprint source_fingerprint,
                                                  XrStableId module_identity,
                                                  XrStableId caller_declaration,
                                                  XrProgramSemanticSourceLocator locator,
                                                  XrStableId *out) {
    if (out)
        memset(out, 0, sizeof(*out));
    if (!out || bytes_zero(source_fingerprint.bytes, sizeof(source_fingerprint.bytes)) ||
        bytes_zero(module_identity.bytes, sizeof(module_identity.bytes)) ||
        bytes_zero(caller_declaration.bytes, sizeof(caller_declaration.bytes)) ||
        locator.kind == 0 || locator.start_line == 0 || locator.start_column == 0 ||
        locator.end_line == 0 || locator.end_column == 0 ||
        (locator.end_line < locator.start_line) ||
        (locator.end_line == locator.start_line && locator.end_column <= locator.start_column))
        return false;
    XrSHA256Context context;
    begin(&context, "xray-source-program-callsite-v2");
    put_fingerprint(&context, source_fingerprint);
    put_id(&context, module_identity);
    put_id(&context, caller_declaration);
    put_u32(&context, locator.kind);
    put_u32(&context, locator.start_line);
    put_u32(&context, locator.start_column);
    put_u32(&context, locator.end_line);
    put_u32(&context, locator.end_column);
    *out = finish_id(&context);
    return true;
}

XR_FUNC bool xr_source_semantic_scalar_i64_export_fingerprint(
    const XrProgramSemanticModuleInput *module, XrStableId exported_declaration,
    XrStableId exported_function, XrFingerprint signature, XrFingerprint effect,
    uint64_t capability_mask, XrFingerprint *out) {
    if (out)
        memset(out, 0, sizeof(*out));
    if (!out || !module ||
        bytes_zero(module->module_identity.bytes, sizeof(module->module_identity.bytes)) ||
        bytes_zero(module->module_authority_fingerprint.bytes,
                   sizeof(module->module_authority_fingerprint.bytes)) ||
        bytes_zero(module->source_fingerprint.bytes, sizeof(module->source_fingerprint.bytes)) ||
        bytes_zero(exported_declaration.bytes, sizeof(exported_declaration.bytes)) ||
        bytes_zero(exported_function.bytes, sizeof(exported_function.bytes)) ||
        bytes_zero(signature.bytes, sizeof(signature.bytes)) ||
        bytes_zero(effect.bytes, sizeof(effect.bytes)) || capability_mask != 0)
        return false;
    XrSHA256Context context;
    begin_program_identity(&context, "xray-source-scalar-module-export-v1");
    put_id(&context, module->module_identity);
    put_fingerprint(&context, module->module_authority_fingerprint);
    put_fingerprint(&context, module->source_fingerprint);
    put_id(&context, exported_declaration);
    put_id(&context, exported_function);
    put_fingerprint(&context, signature);
    put_fingerprint(&context, effect);
    put_u64(&context, capability_mask);
    put_u32(&context, XR_EXACT_SCALAR_I64);
    put_u32(&context, 1);
    put_u32(&context, XR_PARAM_READ);
    *out = finish_fingerprint(&context);
    return true;
}

XR_FUNC bool xr_source_semantic_scalar_i64_import_binding(
    const XrProgramSemanticModuleInput *source, const XrProgramSemanticModuleInput *dependency,
    XrProgramSemanticSourceLocator import_locator, XrStableId exported_declaration,
    XrStableId exported_function, XrStableId return_type, XrFingerprint signature,
    XrFingerprint effect, uint64_t capability_mask, XrStableId *out) {
    if (out)
        memset(out, 0, sizeof(*out));
    if (!out || !source || !dependency ||
        bytes_zero(source->module_identity.bytes, sizeof(source->module_identity.bytes)) ||
        bytes_zero(source->module_authority_fingerprint.bytes,
                   sizeof(source->module_authority_fingerprint.bytes)) ||
        bytes_zero(source->source_fingerprint.bytes, sizeof(source->source_fingerprint.bytes)) ||
        bytes_zero(source->export_fingerprint.bytes, sizeof(source->export_fingerprint.bytes)) ||
        bytes_zero(dependency->module_identity.bytes, sizeof(dependency->module_identity.bytes)) ||
        bytes_zero(dependency->module_authority_fingerprint.bytes,
                   sizeof(dependency->module_authority_fingerprint.bytes)) ||
        bytes_zero(dependency->source_fingerprint.bytes,
                   sizeof(dependency->source_fingerprint.bytes)) ||
        bytes_zero(dependency->export_fingerprint.bytes,
                   sizeof(dependency->export_fingerprint.bytes)) ||
        import_locator.kind == 0 || import_locator.start_line == 0 ||
        import_locator.start_column == 0 || import_locator.end_line == 0 ||
        import_locator.end_column == 0 || (import_locator.end_line < import_locator.start_line) ||
        (import_locator.end_line == import_locator.start_line &&
         import_locator.end_column <= import_locator.start_column) ||
        bytes_zero(exported_declaration.bytes, sizeof(exported_declaration.bytes)) ||
        bytes_zero(exported_function.bytes, sizeof(exported_function.bytes)) ||
        bytes_zero(return_type.bytes, sizeof(return_type.bytes)) ||
        bytes_zero(signature.bytes, sizeof(signature.bytes)) ||
        bytes_zero(effect.bytes, sizeof(effect.bytes)) || capability_mask != 0)
        return false;
    XrSHA256Context context;
    begin_program_identity(&context, "xray-source-scalar-graph-resolver-binding-v1");
    put_id(&context, source->module_identity);
    put_fingerprint(&context, source->module_authority_fingerprint);
    put_fingerprint(&context, source->source_fingerprint);
    put_fingerprint(&context, source->export_fingerprint);
    put_id(&context, dependency->module_identity);
    put_fingerprint(&context, dependency->module_authority_fingerprint);
    put_fingerprint(&context, dependency->source_fingerprint);
    put_fingerprint(&context, dependency->export_fingerprint);
    put_u32(&context, XR_PROGRAM_SEMANTIC_DEPENDENCY_SELECTIVE_FUNCTION_IMPORT);
    put_locator(&context, import_locator);
    put_id(&context, exported_declaration);
    put_id(&context, exported_function);
    put_id(&context, return_type);
    put_fingerprint(&context, signature);
    put_fingerprint(&context, effect);
    put_u64(&context, capability_mask);
    put_u32(&context, 1);
    put_u32(&context, XR_PARAM_READ);
    *out = finish_id(&context);
    return true;
}

static bool source_graph_module_valid(const XrProgramSemanticModuleInput *module,
                                      bool require_exports) {
    return module &&
           !bytes_zero(module->module_identity.bytes, sizeof(module->module_identity.bytes)) &&
           !bytes_zero(module->module_authority_fingerprint.bytes,
                       sizeof(module->module_authority_fingerprint.bytes)) &&
           !bytes_zero(module->source_fingerprint.bytes,
                       sizeof(module->source_fingerprint.bytes)) &&
           (!require_exports ||
            !bytes_zero(module->export_fingerprint.bytes,
                        sizeof(module->export_fingerprint.bytes)));
}

static bool source_graph_locator_valid(XrProgramSemanticSourceLocator locator) {
    return locator.kind != 0 && locator.start_line != 0 && locator.start_column != 0 &&
           locator.end_line != 0 && locator.end_column != 0 &&
           (locator.end_line > locator.start_line ||
            (locator.end_line == locator.start_line &&
             locator.end_column > locator.start_column));
}

bool xr_source_semantic_module_graph_policy(XrFingerprint *out) {
    if (out)
        memset(out, 0, sizeof(*out));
    if (!out)
        return false;
    XrSHA256Context context;
    begin_program_identity(&context, "xray-source-module-graph-policy-v1");
    put_u32(&context, XR_PROGRAM_SEMANTIC_CLOSURE_MAX_MODULES);
    put_u32(&context, XR_PROGRAM_SEMANTIC_CLOSURE_MAX_DEPENDENCIES);
    put_u32(&context, XR_PROGRAM_SEMANTIC_DEPENDENCY_SOURCE_MODULE_EDGE);
    /* Exactly one graph entry, all rows reachable, and no dependency cycle. */
    put_u32(&context, 1);
    put_u32(&context, 1);
    put_u32(&context, 1);
    *out = finish_fingerprint(&context);
    return true;
}

bool xr_source_semantic_module_graph_exports(
    const XrProgramSemanticModuleInput *module, XrFingerprint *out) {
    if (out)
        memset(out, 0, sizeof(*out));
    if (!out || !source_graph_module_valid(module, false))
        return false;
    XrSHA256Context context;
    begin_program_identity(&context, "xray-source-module-graph-exports-v1");
    put_id(&context, module->module_identity);
    put_fingerprint(&context, module->module_authority_fingerprint);
    put_fingerprint(&context, module->source_fingerprint);
    *out = finish_fingerprint(&context);
    return true;
}

bool xr_source_semantic_module_graph_import_binding(
    const XrProgramSemanticModuleInput *source,
    const XrProgramSemanticModuleInput *dependency,
    XrProgramSemanticSourceLocator import_locator, XrStableId *out) {
    if (out)
        memset(out, 0, sizeof(*out));
    if (!out || !source_graph_module_valid(source, true) ||
        !source_graph_module_valid(dependency, true) ||
        !source_graph_locator_valid(import_locator) ||
        memcmp(source->module_identity.bytes, dependency->module_identity.bytes,
               sizeof(source->module_identity.bytes)) == 0)
        return false;
    XrSHA256Context context;
    begin_program_identity(&context, "xray-source-module-graph-import-binding-v1");
    put_id(&context, source->module_identity);
    put_fingerprint(&context, source->module_authority_fingerprint);
    put_fingerprint(&context, source->source_fingerprint);
    put_fingerprint(&context, source->export_fingerprint);
    put_id(&context, dependency->module_identity);
    put_fingerprint(&context, dependency->module_authority_fingerprint);
    put_fingerprint(&context, dependency->source_fingerprint);
    put_fingerprint(&context, dependency->export_fingerprint);
    put_u32(&context, XR_PROGRAM_SEMANTIC_DEPENDENCY_SOURCE_MODULE_EDGE);
    put_locator(&context, import_locator);
    *out = finish_id(&context);
    return true;
}

bool xr_source_semantic_module_graph_dependency_contract(
    const XrProgramSemanticModuleInput *source,
    const XrProgramSemanticModuleInput *dependency,
    XrProgramSemanticSourceLocator import_locator, XrStableId resolver_binding,
    XrFingerprint *out) {
    if (out)
        memset(out, 0, sizeof(*out));
    if (!out || !source_graph_module_valid(source, true) ||
        !source_graph_module_valid(dependency, true) ||
        !source_graph_locator_valid(import_locator) ||
        bytes_zero(resolver_binding.bytes, sizeof(resolver_binding.bytes)))
        return false;
    XrSHA256Context context;
    begin_program_identity(&context,
                           "xray-source-module-graph-dependency-contract-v1");
    put_id(&context, source->module_identity);
    put_fingerprint(&context, source->module_authority_fingerprint);
    put_fingerprint(&context, source->source_fingerprint);
    put_fingerprint(&context, source->export_fingerprint);
    put_id(&context, dependency->module_identity);
    put_fingerprint(&context, dependency->module_authority_fingerprint);
    put_fingerprint(&context, dependency->source_fingerprint);
    put_fingerprint(&context, dependency->export_fingerprint);
    put_u32(&context, XR_PROGRAM_SEMANTIC_DEPENDENCY_SOURCE_MODULE_EDGE);
    put_locator(&context, import_locator);
    put_id(&context, resolver_binding);
    *out = finish_fingerprint(&context);
    return true;
}
