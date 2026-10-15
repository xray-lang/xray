/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_generation.h - Owned Source records and declaration publication
 *
 * KEY CONCEPT:
 *   Compiler records retain their exact Source, AST and Checked decisions.
 *   Preparing a record cannot replace published declaration observations.
 */
#ifndef XXIR_SOURCE_GENERATION_H
#define XXIR_SOURCE_GENERATION_H
#include "xxir_source.h"
struct AstNode;
typedef struct XrXirSourceGenerations XrXirSourceGenerations;
typedef struct XrXirSourceGenerationConfig {
    XrXirCompileContext context;
    const char *namespace_id, *stdlib_path;
    struct XrLockfile *lockfile;
    const struct XrXirLibraryCatalog *libraries;
} XrXirSourceGenerationConfig;
typedef struct XrXirSourceGenerationView {
    uint64_t serial, parent;
    bool published;
    const char *source, *identity;
    size_t length;
    XrFingerprint fingerprint;
    const struct AstNode *ast;
    const XrXirSourceResult *result;
} XrXirSourceGenerationView;
typedef struct XrXirSourceGenerationBinding {
    uint64_t generation;
    const XrXirSourceDeclaration *declaration;
} XrXirSourceGenerationBinding;
/* Copies configuration strings and pins the supplied cumulative ledger.
 * Catalog and lockfile are borrowed and must outlive preparation calls.
 * All operations require caller exclusion; namespace distinguishes sessions. */
XR_FUNC XrXirStatus xr_xir_compile_source_generations_new(
    const XrXirSourceGenerationConfig *config, XrXirSourceGenerations **output);
XR_FUNC void xr_xir_compile_source_generations_free(XrXirSourceGenerations *owner);
/* Checks one real input through the ordinary Source kernel. One candidate may
 * be pending. Its AST, bytes, identity, Checked result and complete latest-name
 * table are owned before return. Failure preserves the empty serial output and
 * all published records; serials consumed by failed attempts are never reused.
 * MEMORY logical_path must be empty. This entry does not yet seed historical
 * declarations into semantic checking; cross-input compilation remains separate. */
XR_FUNC XrXirStatus xr_xir_compile_source_generation_prepare(XrXirSourceGenerations *owner,
    const XrXirSourceText *text, uint64_t *serial, XrXirSourceDiagnostic *diagnostic);
/* Source publication allocates and charges nothing. The execution coordinator
 * calls this only after the corresponding Instance candidate returned normally.
 * These declaration observations do not authenticate runtime publication. */
XR_FUNC XrXirStatus xr_xir_compile_source_generation_publish(XrXirSourceGenerations *owner, uint64_t serial);
XR_FUNC XrXirStatus xr_xir_compile_source_generation_abort(XrXirSourceGenerations *owner, uint64_t serial);
/* Views borrow their exact record until abort or owner destruction. Bindings
 * select only published root declarations and retain their birth generation;
 * query facts grant no access, construction or execution authority. Failure
 * preserves outputs. An absent name returns BAD_STRUCTURE. */
XR_FUNC XrXirStatus xr_xir_compile_source_generation_view(const XrXirSourceGenerations *owner,
    uint64_t serial, XrXirSourceGenerationView *output);
XR_FUNC XrXirStatus xr_xir_compile_source_generation_lookup(const XrXirSourceGenerations *owner,
    const char *name, XrXirSourceGenerationBinding *output);
#endif // XXIR_SOURCE_GENERATION_H
