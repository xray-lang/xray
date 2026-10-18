/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_query.h - Owned facts from the source semantic owner
 *
 * KEY CONCEPT:
 *   Queries observe immutable checked decisions and cannot execute or infer.
 */
#ifndef XXIR_SOURCE_QUERY_H
#define XXIR_SOURCE_QUERY_H
#include "xxir.h"
#include "../base/xstable_id.h"
typedef enum XrXirSourceDiagnosticOrigin {
    XR_XIR_SOURCE_DIAGNOSTIC_DEFAULT, XR_XIR_SOURCE_DIAGNOSTIC_PARSE,
    XR_XIR_SOURCE_DIAGNOSTIC_INPUT
} XrXirSourceDiagnosticOrigin;
typedef struct XrXirSourceDiagnostic {
    XrXirStatus status;
    uint32_t module;
    int line, column;
    char message[192];
} XrXirSourceDiagnostic;
/* Separate failure ownership leaves the diagnostic and semantic owner layouts
 * unchanged. Coordinates project actual parser text before teardown. */
typedef struct XrXirSourceFailure {
    char *path;
    XrXirSourceDiagnosticOrigin origin;
    bool has_utf16_position;
    uint32_t utf16_line,utf16_column;
} XrXirSourceFailure;
XR_FUNC bool xr_xir_compile_source_failure_empty(const XrXirSourceFailure *);
XR_FUNC void xr_xir_compile_source_failure_free(XrXirSourceFailure *);
typedef struct XrXirSourceRange {
    uint32_t module;
    int line, column, end_line, end_column;
} XrXirSourceRange;

/* Source-owned display coordinates use the sealed instruction order. Zero
 * statement means no source execution site; it never grants frame authority. */
#define XR_XIR_SOURCE_DEBUG_ENTRY 1u
/* This site is reached only after an ordinary catch's type and pattern have
 * accepted the error. It is not a prediction at an earlier throw and excludes
 * panic handlers. Empty bodies use their existing join instruction. */
#define XR_XIR_SOURCE_DEBUG_HANDLER_ACCEPT 2u
#define XR_XIR_SOURCE_DEBUG_UNIT 1u
#define XR_XIR_SOURCE_DEBUG_CELL 2u
/* A reserved nested-function name shadows lookup before initialization. It has
 * no runtime value ordinal and never grants observation or write authority. */
#define XR_XIR_SOURCE_DEBUG_UNAVAILABLE 4u
typedef struct XrXirSourceDebugBinding {
    uint32_t declaration, value, parent, flags;
} XrXirSourceDebugBinding;
typedef struct XrXirSourceDebugPoint {
    XrXirSourceRange range;
    uint32_t statement, flags, environment;
} XrXirSourceDebugPoint;

/* A source throw may lower to a local error jump. The operand is observed
 * before routing or cleanup; synthetic propagation is not a new source throw. */
typedef struct XrXirSourceDebugThrow { uint32_t instruction, operand; } XrXirSourceDebugThrow;

/* Half-open selector instructions precede accepted. Rejected names the next
 * selector or the propagation instruction; UINT32_MAX means unconditional
 * acceptance. Coordinates describe existing code and grant no evaluation. */
typedef struct XrXirSourceDebugSelector {
    uint32_t first, accepted, rejected;
} XrXirSourceDebugSelector;

/* Handler identities are one-based within one function. A nonzero parent is
 * the later-built outer propagation route. The Error operand is its existing
 * join value; a consumed route has no propagation instruction or parent.
 * Propagation input is a zero-based PHI pair, or UINT32_MAX without a parent. */
typedef struct XrXirSourceDebugHandler {
    uint32_t error, parent, propagation;
    const XrXirSourceDebugSelector *selectors;
    uint32_t selector_count;
    uint32_t propagation_input;
} XrXirSourceDebugHandler;

/* The explicit error edge, value and zero-based PHI pair prove which join receives this site;
 * pending invoke/await routes never infer a handler from nearby code. */
typedef struct XrXirSourceDebugRoute { uint32_t instruction, handler, edge, error, input; } XrXirSourceDebugRoute;

typedef struct XrXirSourceDebugFunction {
    uint32_t function, declaration, parameter_count, instruction_count;
    const XrXirSourceDebugPoint *points;
    const XrXirSourceDebugBinding *bindings;
    uint32_t binding_count;
    const XrXirSourceDebugThrow *throw_sites;
    uint32_t throw_count;
    const XrXirSourceDebugHandler *handlers;
    uint32_t handler_count;
    const XrXirSourceDebugRoute *routes;
    uint32_t route_count;
} XrXirSourceDebugFunction;

/* A producer-selected slot is a display coordinate, never a module grant. */
typedef struct XrXirSourceDebugGlobal {
    uint32_t declaration, slot;
} XrXirSourceDebugGlobal;

typedef struct XrXirSourceDebugView {
    const XrXirSourceDebugFunction *functions;
    uint32_t function_count;
    const XrXirSourceDebugGlobal *globals;
    uint32_t global_count;
} XrXirSourceDebugView;
typedef struct XrXirSourceType {
    XrXirType type;
    uint32_t generic_owner;
    bool known;
} XrXirSourceType;
typedef enum XrXirSourceDeclarationKind {
    XR_XIR_SOURCE_FUNCTION, XR_XIR_SOURCE_BINDING, XR_XIR_SOURCE_PARAMETER,
    XR_XIR_SOURCE_IMPORT, XR_XIR_SOURCE_MODULE, XR_XIR_SOURCE_TYPE,
    XR_XIR_SOURCE_TYPE_PARAMETER, XR_XIR_SOURCE_MEMBER, XR_XIR_SOURCE_INTRINSIC
} XrXirSourceDeclarationKind;
typedef struct XrXirSourceDeclaration {
    uint32_t id, parent, target;
    XrXirSourceDeclarationKind kind;
    const char *name;
    XrXirSourceRange range;
    XrXirSourceType type;
    const XrXirSourceType *parameters;
    uint32_t parameter_count;
    bool mutable, exported;
    uint32_t native_identity;
    const char *signature;
    uint32_t generic_parent, generic_parent_count, generic_parameter_count;
    const XrXirConstraint *generic_constraints;
    const uint32_t *type_parameter_kinds;
    /* Implicit receiver prefix copied from a verified Checked method identity.
     * Imported definitions retain absent lexical spans and no synthetic this. */
    uint32_t checked_receiver_count;
} XrXirSourceDeclaration;
typedef enum XrXirSourceAccess {
    XR_XIR_SOURCE_READ, XR_XIR_SOURCE_WRITE, XR_XIR_SOURCE_READ_WRITE,
    XR_XIR_SOURCE_CALL, XR_XIR_SOURCE_FUNCTION_VALUE, XR_XIR_SOURCE_TYPE_USE,
    /* A directive selects an export without executing or using it in code. */
    XR_XIR_SOURCE_IMPORT_SELECTOR
} XrXirSourceAccess;
typedef struct XrXirSourceReference {
    XrXirSourceRange range;
    uint32_t declaration, target;
    XrXirSourceAccess access;
} XrXirSourceReference;
typedef struct XrXirSourceExpression {
    uint32_t node;
    XrXirSourceRange range;
    XrXirSourceType type;
} XrXirSourceExpression;
typedef struct XrXirSourceQueryModule {
    const char *identity, *path;
    XrFingerprint fingerprint;
} XrXirSourceQueryModule;
typedef struct XrXirSourceView {
    bool complete;
    XrXirSourceDiagnostic diagnostic;
    const XrXirSourceQueryModule *modules;
    uint32_t module_count;
    const XrXirSourceDeclaration *declarations;
    uint32_t declaration_count;
    const XrXirSourceReference *references;
    uint32_t reference_count;
    const XrXirSourceExpression *expressions;
    uint32_t expression_count;
    const XrXirTypes *types;
    const XrXirImplementationTable *implementations;
} XrXirSourceView;
typedef struct XrXirSourceSnapshot XrXirSourceSnapshot;
/* Snapshot-owned display facts. These carry no execution or construction authority. */
typedef enum XrXirSourceSyntaxRole {
    XR_XIR_SOURCE_SYNTAX_NONE, XR_XIR_SOURCE_SYNTAX_NAME,
    XR_XIR_SOURCE_SYNTAX_METHOD, XR_XIR_SOURCE_SYNTAX_CLOSURE,
    XR_XIR_SOURCE_SYNTAX_RECEIVER, XR_XIR_SOURCE_SYNTAX_UNRESOLVED
} XrXirSourceSyntaxRole;
#define XR_XIR_SOURCE_SYNTAX_STATIC 1u
#define XR_XIR_SOURCE_SYNTAX_INFERRED_BINDING 2u
typedef struct XrXirSourceDeclarationSyntax {
    XrXirSourceRange name;
    XrXirSourceSyntaxRole role;
    uint32_t flags;
    /* Actual definition-scope order, including a real enclosing prefix.
     * NULL/count-zero is explicitly unavailable, never an invented name. */
    const char *const *type_parameters;
    uint32_t type_parameter_count;
} XrXirSourceDeclarationSyntax;
typedef enum XrXirSourceMarkerRole {
    XR_XIR_SOURCE_MARKER_REF = 1, XR_XIR_SOURCE_MARKER_MOVE, XR_XIR_SOURCE_MARKER_REF_ARGUMENT,
    /* One parser token both selects a payload field and declares this binding.
     * Its same-range MEMBER reference identifies the field independently. */
    XR_XIR_SOURCE_MARKER_PATTERN_FIELD_BINDING,
    /* An unaliased selector also declares a local binding. Its target reference
     * retains the independent export identity. Bare modules preserve their path. */
    XR_XIR_SOURCE_MARKER_IMPORT_BINDING,
    /* Parser-certified insertion point after a quoted module specifier. */
    XR_XIR_SOURCE_MARKER_IMPORT_IMPLICIT
} XrXirSourceMarkerRole;
typedef struct XrXirSourceMarker {
    XrXirSourceRange range;
    uint32_t declaration;
    XrXirSourceMarkerRole role;
} XrXirSourceMarker;
typedef struct XrXirSourceSyntaxView {
    const XrXirSourceDeclarationSyntax *declarations;
    uint32_t declaration_count;
    const XrXirSourceRange *references;
    uint32_t reference_count;
    const XrXirSourceMarker *markers;
    uint32_t marker_count;
} XrXirSourceSyntaxView;
XR_FUNC const XrXirSourceSyntaxView *xr_xir_compile_source_snapshot_syntax(const XrXirSourceSnapshot *snapshot);

typedef struct XrXirSourceResult {
    XrXirArtifact *checked;
    XrXirSourceSnapshot *snapshot;
} XrXirSourceResult;
XR_FUNC const XrXirSourceView *xr_xir_compile_source_snapshot_view(const XrXirSourceSnapshot *snapshot);
XR_FUNC const XrXirSourceDebugView *xr_xir_compile_source_snapshot_debug(const XrXirSourceSnapshot *snapshot);
/* These immutable observations carry no construction or invocation authority. */
XR_FUNC const XrXirConstruction *xr_xir_compile_source_snapshot_construction(const XrXirSourceSnapshot *snapshot);
XR_FUNC void xr_xir_compile_source_snapshot_free(XrXirSourceSnapshot *snapshot);
XR_FUNC void xr_xir_compile_source_result_free(XrXirSourceResult *result);
#endif // XXIR_SOURCE_QUERY_H
