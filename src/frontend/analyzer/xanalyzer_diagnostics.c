/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_diagnostics.c - Atomic diagnostics on the original compiler state
 */
#include "xanalyzer_diagnostics.h"
#include <limits.h>
#include <stddef.h>
#include <stdio.h>

typedef struct XaDiagnosticOwner {
    XrCompileState *state;
    XaDiagnostic diagnostic;
} XaDiagnosticOwner;

static XaDiagnosticOwner *xa_diagnostic_owner(XaDiagnostic *diagnostic) {
    return (XaDiagnosticOwner *) ((char *) diagnostic - offsetof(XaDiagnosticOwner, diagnostic));
}

XR_FUNC void xa_diagnostic_dispose(XaDiagnostic *diagnostic) {
    if (!diagnostic)
        return;
    XaDiagnosticOwner *owner = xa_diagnostic_owner(diagnostic);
    XrCompileState *state = owner->state;
    xr_compile_state_free((void *) diagnostic->message);
    xr_compile_state_free((void *) diagnostic->location.file);
    xr_compile_state_free(owner);
    xr_compile_state_release(state);
}

XR_FUNC void xa_analyzer_clear_diagnostics(XaAnalyzer *analyzer) {
    if (!analyzer)
        return;
    XaDiagnostic *diagnostic = analyzer->diagnostics;
    analyzer->diagnostics = NULL;
    analyzer->diagnostics_tail = NULL;
    analyzer->diagnostic_count = 0;
    while (diagnostic) {
        XaDiagnostic *next = diagnostic->next;
        xa_diagnostic_dispose(diagnostic);
        diagnostic = next;
    }
}

/* Charge before each borrowed byte read, including the two terminators. */
static XrCompileResourceStatus xa_diagnostic_same_text(XrCompileState *state, const char *left,
                                        const char *right, bool *equal) {
    *equal = left == right;
    if (*equal || !left || !right)
        return XR_COMPILE_RESOURCE_OK;
    for (size_t i = 0;; ++i) {
        XrCompileResourceStatus status = xr_compile_state_work(state, 2);
        if (status != XR_COMPILE_RESOURCE_OK)
            return status;
        if (left[i] != right[i])
            return XR_COMPILE_RESOURCE_OK;
        if (!left[i]) {
            *equal = true;
            return XR_COMPILE_RESOURCE_OK;
        }
    }
}

static XrCompileResourceStatus xa_diagnostic_find_duplicate(XaAnalyzer *analyzer,
    XrDiagSeverity severity, int code, const char *message, const XrLocation *loc,
    bool *duplicate) {
    XrCompileState *state = xa_symbol_registry_state(analyzer->symbol_registry);
    for (XaDiagnostic *d = analyzer->diagnostics; d; d = d->next) {
        XrCompileResourceStatus status = xr_compile_state_work(state, 1);
        if (status != XR_COMPILE_RESOURCE_OK)
            return status;
        if (d->severity != severity || d->code != code ||
            (loc && d->location.line != loc->line) ||
            (!loc && (d->location.line || d->location.column)))
            continue;
        bool equal = false;
        status = xa_diagnostic_same_text(state, d->location.file, loc ? loc->file : NULL, &equal);
        if (status != XR_COMPILE_RESOURCE_OK)
            return status;
        if (!equal)
            continue;
        status = xa_diagnostic_same_text(state, d->message, message, &equal);
        if (status != XR_COMPILE_RESOURCE_OK)
            return status;
        if (equal) {
            *duplicate = true;
            return XR_COMPILE_RESOURCE_OK;
        }
    }
    return XR_COMPILE_RESOURCE_OK;
}

XR_FUNC void xa_analyzer_add_diagnostic(XaAnalyzer *analyzer, XrDiagSeverity severity,
    int code, const char *message, XrLocation *loc) {
    XrCompileState *state = xa_symbol_registry_state(analyzer ? analyzer->symbol_registry : NULL);
    if (xr_compile_state_work(state, 1) != XR_COMPILE_RESOURCE_OK)
        return;
    bool duplicate = false;
    /* Preserve exact nullable file/message equality and source order. Column
     * remains irrelevant for located duplicates from independent checks. */
    XrCompileResourceStatus status = xa_diagnostic_find_duplicate(analyzer, severity, code, message, loc, &duplicate);
    if (status != XR_COMPILE_RESOURCE_OK || duplicate)
        return;
    if (analyzer->diagnostic_count < 0 || analyzer->diagnostic_count == INT_MAX) {
        (void) xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BUDGET);
        return;
    }
    void *memory = NULL;
    status = xr_compile_state_calloc(state, 1, sizeof(XaDiagnosticOwner), &memory);
    if (status != XR_COMPILE_RESOURCE_OK)
        return;
    XaDiagnosticOwner *owner = memory;
    XaDiagnostic *diagnostic = &owner->diagnostic;
    char *message_copy = NULL, *file_copy = NULL;
    if (message)
        status = xr_compile_state_strdup(state, message, &message_copy);
    if (status == XR_COMPILE_RESOURCE_OK && loc && loc->file)
        status = xr_compile_state_strdup(state, loc->file, &file_copy);
    if (status == XR_COMPILE_RESOURCE_OK)
        status = xr_compile_state_work(state, 1);
    if (status == XR_COMPILE_RESOURCE_OK)
        status = xr_compile_state_retain(state);
    if (status != XR_COMPILE_RESOURCE_OK) {
        xr_compile_state_free(file_copy);
        xr_compile_state_free(message_copy);
        xr_compile_state_free(owner);
        return;
    }
    owner->state = state;
    diagnostic->severity = severity;
    diagnostic->code = code;
    diagnostic->message = message_copy;
    if (loc)
        diagnostic->location = *loc;
    diagnostic->location.file = file_copy;
    if (analyzer->diagnostics_tail)
        analyzer->diagnostics_tail->next = diagnostic;
    else
        analyzer->diagnostics = diagnostic;
    analyzer->diagnostics_tail = diagnostic;
    ++analyzer->diagnostic_count;
}

XR_FUNC XrCompileResourceStatus xa_analyzer_append_diagnostic_suffix(
    XaAnalyzer *analyzer, XaDiagnostic *diagnostic, const char *suffix) {
    XrCompileState *state = xa_symbol_registry_state(analyzer ? analyzer->symbol_registry : NULL);
    XrCompileResourceStatus status = xr_compile_state_work(state, 1);
    if (status != XR_COMPILE_RESOURCE_OK)
        return status;
    if (!diagnostic || !suffix)
        return xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    XaDiagnostic *found = analyzer->diagnostics;
    while (found) {
        status = xr_compile_state_work(state, 1);
        if (status != XR_COMPILE_RESOURCE_OK)
            return status;
        if (found == diagnostic)
            break;
        found = found->next;
    }
    if (!found || xa_diagnostic_owner(found)->state != state)
        return xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    size_t length = 0, suffix_length = 0;
    const char *old = diagnostic->message ? diagnostic->message : "";
    status = xr_compile_state_string_length(state, old, &length);
    if (status == XR_COMPILE_RESOURCE_OK)
        status = xr_compile_state_string_length(state, suffix, &suffix_length);
    if (status != XR_COMPILE_RESOURCE_OK)
        return status;
    if (length > SIZE_MAX - suffix_length - 1)
        return xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BUDGET);
    void *memory = NULL;
    status = xr_compile_state_alloc(state, length + suffix_length + 1, &memory);
    if (status == XR_COMPILE_RESOURCE_OK)
        status = xr_compile_state_copy(state, memory, old, length);
    if (status == XR_COMPILE_RESOURCE_OK)
        status = xr_compile_state_copy(state, (char *) memory + length, suffix, suffix_length + 1);
    if (status == XR_COMPILE_RESOURCE_OK)
        status = xr_compile_state_work(state, 1);
    if (status != XR_COMPILE_RESOURCE_OK) {
        xr_compile_state_free(memory);
        return status;
    }
    xr_compile_state_free((void *) diagnostic->message);
    diagnostic->message = memory;
    return XR_COMPILE_RESOURCE_OK;
}

XR_FUNC XaDiagnostic *xa_analyzer_get_diagnostics(XaAnalyzer *analyzer, int *count) {
    if (!analyzer) {
        if (count) *count = 0;
        return NULL;
    }
    if (count) *count = analyzer->diagnostic_count;
    return analyzer->diagnostics;
}

XR_FUNC int xa_analyzer_print_errors(XaAnalyzer *analyzer, const char *fallback_path) {
    int errors = 0;
    if (!analyzer)
        return 0;
    for (XaDiagnostic *diagnostic = analyzer->diagnostics; diagnostic;
         diagnostic = diagnostic->next) {
        if (diagnostic->severity != XR_DIAG_SEV_ERROR)
            continue;
        errors++;
        const char *file = diagnostic->location.file ? diagnostic->location.file
                           : fallback_path           ? fallback_path
                                                     : "<module>";
        fprintf(stderr, "%s:%u:%u: error: %s\n", file, diagnostic->location.line,
                diagnostic->location.column, diagnostic->message ? diagnostic->message : "");
    }
    return errors;
}
