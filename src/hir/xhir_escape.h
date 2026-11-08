/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#ifndef XHIR_ESCAPE_H
#define XHIR_ESCAPE_H
#include "../xir/xxir.h"
#include "../xir/xxir_storage_flow.h"

/* Parameter spellings are synchronous declaration borrows. No AST or caller
 * storage survives the analysis. Physical name offsets include hidden captures
 * and receivers, while environment roles come from declaration identities.
 * A NULL table checks admitted external declarations without source spellings. */
typedef struct XrHirEscapeDeclaration {
    const char *const *names;
    uint32_t count, captures;
} XrHirEscapeDeclaration;

typedef struct XrHirEscapeDiagnostic {
    uint32_t code, function, instruction;
    char message[512];
} XrHirEscapeDiagnostic;

typedef struct XrHirEscapeFacts XrHirEscapeFacts;
typedef struct XrHirEscapeBuilder XrHirEscapeBuilder;

/* Declaration spellings are copied. Body operands are a synchronous borrow
 * through the final observer, and every instruction is submitted once. */
XR_FUNC XrXirStatus xr_hir_escape_builder_new(const XrXirCompileContext *, const XrXirModule *,
    const XrHirEscapeDeclaration *, XrCompileRequestControl *, XrHirEscapeBuilder **);
XR_FUNC XrXirStatus xr_hir_escape_builder_observe(XrHirEscapeBuilder *, uint32_t, uint32_t,
    const XrXirInstruction *);
XR_FUNC void xr_hir_escape_builder_free(XrHirEscapeBuilder *);
/* Non-final rounds register numeric requests without freezing exposure. A
 * storage-only remainder requests another shared receiving round. The final
 * round owns point captures before storage seals; finish only reads numbers. */
XR_FUNC XrXirStatus xr_hir_escape_builder_prepare_points(XrHirEscapeBuilder *, XirStorageFlow *,
    const XirExecutionOwner *, bool, bool *);
XR_FUNC XrXirStatus xr_hir_escape_builder_finish(XrHirEscapeBuilder *, XirStorageFlow *,
    const XirExecutionOwner *, XrHirEscapeDiagnostic *, XrHirEscapeFacts **);
XR_FUNC XrXirStatus xr_hir_escape_restriction(const XrHirEscapeFacts *, uint32_t,
    uint32_t, uint32_t, XirRestrictionFormula *);
XR_FUNC XrXirStatus xr_hir_escape_facts_view(const XrHirEscapeFacts *, XirRestrictionView *);

XR_FUNC XrXirStatus xr_hir_escape_check(const XrXirCompileContext *, const XrXirModule *,
    const XrHirEscapeDeclaration *, XrCompileRequestControl *, XrHirEscapeDiagnostic *, XrHirEscapeFacts **);
XR_FUNC void xr_hir_escape_facts_free(XrHirEscapeFacts *);
XR_FUNC XrXirStatus xr_hir_escape_restricted(const XrHirEscapeFacts *, uint32_t, uint32_t, bool *);
XR_FUNC XrXirStatus xr_hir_escape_flows(const XrHirEscapeFacts *, uint32_t,
    const XrXirValueFlow **, uint32_t *);
XR_FUNC XrXirStatus xr_hir_escape_facts_copy(const XrXirCompileContext *, const XrHirEscapeFacts *,
    XrCompileRequestControl *, XrHirEscapeFacts **);
/* The private receiving owner changes only after a complete successful map.
 * This relocates an existing classification and never derives a new rule. */
XR_FUNC XrXirStatus xr_hir_escape_facts_relocate(const XrXirCompileContext *, XrHirEscapeFacts *,
    uint32_t, uint32_t, const uint32_t *, XrCompileRequestControl *);
XR_FUNC XrXirStatus xr_hir_escape_function_match(const XrXirFunction *, const XrXirFunction *, const char **);
XR_FUNC XrXirStatus xr_hir_escape_callable_match(const XrXirFunction *, uint32_t,
    const XrXirTypeNode *, const char **);
#endif // XHIR_ESCAPE_H
