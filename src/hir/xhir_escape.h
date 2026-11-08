/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#ifndef XHIR_ESCAPE_H
#define XHIR_ESCAPE_H
#include "../xir/xxir.h"

/* Parameter spellings are synchronous declaration borrows. No AST or caller
 * storage survives the analysis. Hidden environment parameters precede names.
 * A NULL table checks admitted external declarations without source spellings. */
typedef struct XrHirEscapeDeclaration {
    const char *const *names;
    uint32_t count, captures;
} XrHirEscapeDeclaration;

typedef struct XrHirEscapeDiagnostic {
    uint32_t code, function, instruction;
    char message[512];
} XrHirEscapeDiagnostic;

XR_FUNC XrXirStatus xr_hir_escape_check(const XrXirCompileContext *, const XrXirModule *,
    const XrHirEscapeDeclaration *, XrCompileRequestControl *, XrHirEscapeDiagnostic *);
#endif // XHIR_ESCAPE_H
