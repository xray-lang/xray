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
#include "xxir_source_ast_owner.h"
#include "xxir_instance_generation.h"
struct AstNode;
struct XrXirProgram;
typedef struct XrXirSourceGenerations XrXirSourceGenerations;
typedef struct XrXirSourceGenerationConfig {
    XrXirCompileContext context;
    const char *namespace_id, *stdlib_path;
    struct XrLockfile *lockfile;
    const struct XrXirLibraryCatalog *libraries;
} XrXirSourceGenerationConfig;
/* AST ownership stays exact. Rooted locators and authority are borrowed only
 * during preparation; the ordinary graph owns their admitted copies. NULL
 * authority selects this owner's MEMORY namespace and requires empty paths. */
typedef struct XrXirSourceReplInput {
    const XrXirSourceAstOwner *ast;
    const XrModuleIdentityAuthority *authority;
    const char *absolute_path, *logical_path;
    struct XrLockfile *lockfile;
    /* Optional empty output receives the actual owned parser/semantic failure
     * path. It borrows only during prepare; free with compile_resources_free. */
    char **failure_path;
} XrXirSourceReplInput;

typedef struct XrXirSourceGenerationView {
    uint64_t serial, parent;
    bool published, interactive, type_query;
    const XrXirSourceDeclaration *repl_result;
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
/* Prepared immutable display observations, never execution authority. */
typedef struct XrXirSourceGenerationName {
    XrXirSourceGenerationBinding binding;
    const char *name, *type_name;
    bool implicit_result;
} XrXirSourceGenerationName;

typedef enum XrXirSourceDefinitionKind {
    XR_XIR_SOURCE_DEFINITION_BINDING, XR_XIR_SOURCE_DEFINITION_UNIT,
    XR_XIR_SOURCE_DEFINITION_FUNCTION, XR_XIR_SOURCE_DEFINITION_NOMINAL,
    XR_XIR_SOURCE_DEFINITION_INTERFACE, XR_XIR_SOURCE_DEFINITION_MODULE,
    XR_XIR_SOURCE_DEFINITION_IMPORT
} XrXirSourceDefinitionKind;
typedef struct XrXirSourceGenerationDefinition {
    uint64_t generation;
    XrXirSourceDefinitionKind kind;
    uint32_t module, ordinal, dependency, reader, writer;
    XrXirType logical_type, stored_type;
    bool mutable;
    const char *name, *imported;
    const struct AstNode *syntax, *dependency_syntax;
    const XrXirArtifact *checked;
} XrXirSourceGenerationDefinition;
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
 * MEMORY logical_path must be empty. Only this owner's published Checked and
 * actual registered bindings seed historical checking. Receiving preserves
 * birth modules and private lexical scope; it does not prove runtime storage
 * correspondence or authorize replaying historical initialization. */
XR_FUNC XrXirStatus xr_xir_compile_source_generation_prepare(XrXirSourceGenerations *owner,
    const XrXirSourceText *text, uint64_t *serial, XrXirSourceDiagnostic *diagnostic);
/* Adds ordinary root-slot reader functions to this real input before complete
 * Built checking. Each reader performs actual SLOT_LOAD/Cell read/return under
 * its authentic module and is checked for no suspension. Reader ordinals are
 * exact Checked observations, not runtime authority or historical receiving. */
XR_FUNC XrXirStatus xr_xir_compile_source_generation_prepare_bindings(XrXirSourceGenerations *owner,
    const XrXirSourceText *text, uint64_t *serial, XrXirSourceDiagnostic *diagnostic);
/* Retains the complete immutable AST owner through abort or record disposal.
 * No source bytes are rendered, reparsed or replayed; fingerprint is zero.
 * MEMORY path rules and actual parser/ledger admission remain unchanged.
 * Failure releases any temporary pin and preserves input and empty serial. */
XR_FUNC XrXirStatus xr_xir_compile_source_generation_prepare_ast_bindings(XrXirSourceGenerations *owner,
    const XrXirSourceAstOwner *ast, uint64_t *serial, XrXirSourceDiagnostic *diagnostic);
/* Interactive Source preparation uses the same immutable AST and checker.
 * An actual non-Unit tail owns an ordinary immutable root slot and reader;
 * the next input's it selects that exact published declaration. Ordinary and
 * interactive histories cannot be mixed in one owner. */
XR_FUNC XrXirStatus xr_xir_compile_source_generation_prepare_repl_ast_bindings(XrXirSourceGenerations *owner,
    const XrXirSourceReplInput *input, uint64_t *serial, XrXirSourceDiagnostic *diagnostic);
/* Evaluates one actual expression through typeName in the common checker.
 * Its ordinary result reader returns the real String; this temporary record
 * may only be aborted, never published into lexical history. */
XR_FUNC XrXirStatus xr_xir_compile_source_generation_prepare_repl_type_ast_bindings(XrXirSourceGenerations *,
    const XrXirSourceReplInput *, uint64_t *, XrXirSourceDiagnostic *);
/* Source publication allocates and charges nothing. The execution coordinator
 * calls this only after the corresponding Instance candidate returned normally.
 * These declaration observations do not authenticate runtime publication. */
XR_FUNC XrXirStatus xr_xir_compile_source_generation_publish(XrXirSourceGenerations *owner, uint64_t serial);
XR_FUNC XrXirStatus xr_xir_compile_source_generation_abort(XrXirSourceGenerations *owner, uint64_t serial);
/* Binds one prepared record to a real sealed Program. The ordinary compiler
 * derives its exact specialized/Lowered proof and compares the sealed identity.
 * Every active foreign mutable slot must occur in the exact history receiving
 * map. This coverage admits no new library storage; Instance preparation still
 * verifies the actual published birth routes before execution.
 * Success retains the Program until record disposal. Repeat binding to the
 * same Program is allocation-free; replacing a binding or binding after publish
 * is rejected. This code correspondence alone grants no storage permission. */
XR_FUNC XrXirStatus xr_xir_compile_source_generation_bind_program(XrXirSourceGenerations *owner,
    uint64_t serial, struct XrXirProgram *program);
/* Prepares the already bound Program using the actual prior receiving map.
 * Runtime-owned routes borrow existing birth slots and initialized modules;
 * ordinary value admission still rejects incompatible type-arena identities.
 * Failure preserves the empty runtime serial and all published generations. */
XR_FUNC XrXirCallStatus xr_xir_source_generation_prepare_instance(XrXirSourceGenerations *owner,
    uint64_t serial, XrXirInstance *instance, uint64_t *runtime_serial);
/* Views borrow their exact record until abort or owner destruction. Bindings
 * select only published root declarations and retain their birth generation;
 * query facts grant no access, construction or execution authority. Failure
 * preserves outputs. An absent name returns BAD_STRUCTURE. */
XR_FUNC XrXirStatus xr_xir_compile_source_generation_view(const XrXirSourceGenerations *owner,
    uint64_t serial, XrXirSourceGenerationView *output);
XR_FUNC XrXirStatus xr_xir_compile_source_generation_lookup(const XrXirSourceGenerations *owner,
    const char *name, XrXirSourceGenerationBinding *output);
/* Observes actual semantic registration in one exact prepared/published record.
 * Binding logical types and physical stored types remain distinct for Cell
 * owners. Ordinals and types belong only to the returned Checked owner; they
 * are not Lowered/Program ordinals or a runtime storage correspondence. Syntax
 * borrows the record's complete AST closure. Failure preserves the output.
 * This observation grants no additional historical checking authority. */
XR_FUNC XrXirStatus xr_xir_compile_source_generation_definition(const XrXirSourceGenerations *owner,
    uint64_t serial, const char *name, XrXirSourceGenerationDefinition *output);
/* Borrows the exact pending candidate's effective lexical-name table. Rows
 * retain their birth Source serial; select serial to enumerate this input.
 * The borrow ends on publication, abort or owner destruction. These query
 * selectors are not semantic ordinals or runtime authority; resolve each name
 * through definition before using its actual Checked declaration. Outputs
 * must be empty, and failure preserves them. */
XR_FUNC XrXirStatus xr_xir_compile_source_generation_candidate_bindings(const XrXirSourceGenerations *owner,
    uint64_t serial, const XrXirSourceGenerationBinding **output, size_t *count);
/* A zero serial selects published observations, including an empty owner.
 * A pending serial selects its prepared facts. Query-only records have none.
 * Rows and strings borrow the owner; failure preserves empty outputs. */
XR_FUNC XrXirStatus xr_xir_compile_source_generation_names(const XrXirSourceGenerations *,
    uint64_t serial, const XrXirSourceGenerationName **, size_t *);
#endif // XXIR_SOURCE_GENERATION_H
