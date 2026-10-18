/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_analyzer_diagnostics_binding.c - Real diagnostics and creator transfers
 */
#include "frontend/analyzer/xanalyzer_diagnostics.h"
#include "frontend/analyzer/xanalyzer_storage.h"
#include "frontend/analyzer/xanalyzer_visitor_internal.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#define xr_compile_resources_work xr_test_actual_resources_work
#include "../base/parser_resources_observer.h"
#undef xr_compile_resources_work
static bool record_work;
static uint64_t boundaries[32768];
static size_t boundary_count;
XR_FUNC XrCompileResourceStatus xr_compile_resources_work(XrCompileResources *resources,
                                                         uint64_t units) {
    XrCompileResourceStatus status = xr_test_actual_resources_work(resources, units);
    if (record_work && status == XR_COMPILE_RESOURCE_OK && units) {
        XrCompileResourceStats current = {0};
        CHECK(xr_compile_resources_stats(resources, &current) == XR_COMPILE_RESOURCE_OK);
        CHECK(boundary_count < sizeof(boundaries) / sizeof(boundaries[0]));
        boundaries[boundary_count++] = current.work;
    }
    return status;
}
static const XrCompileResourceLimits ample = {1048576, 1048576, 1048576};
static XrCompileResourceStats stats(XrCompileResources *resources) {
    XrCompileResourceStats result = {0};
    CHECK(xr_compile_resources_stats(resources, &result) == XR_COMPILE_RESOURCE_OK);
    return result;
}
static XrCompileResourceStatus session_open(XrCompileResources *resources,
                                           XrCompilerSession **output) {
    switch (xr_compile_session_new(resources, output)) {
        case XR_COMPILER_SESSION_OK: return XR_COMPILE_RESOURCE_OK;
        case XR_COMPILER_SESSION_BAD_ARGUMENT: return XR_COMPILE_RESOURCE_BAD_ARGUMENT;
        case XR_COMPILER_SESSION_BUDGET: return XR_COMPILE_RESOURCE_BUDGET;
        case XR_COMPILER_SESSION_OUT_OF_MEMORY: return XR_COMPILE_RESOURCE_OUT_OF_MEMORY;
    }
    CHECK(false); return XR_COMPILE_RESOURCE_BAD_ARGUMENT;
}

/* Registry reads consume real work; a cut during verification is a resource
 * result, and must be observed before comparing a nullable lookup result. */
static XrCompileResourceStatus checked_lookup(XaAnalyzer *analyzer, uint32_t id,
                                             XaSymbol *expected) {
    XaSymbol *actual = xa_symbol_registry_lookup(analyzer->symbol_registry, id);
    XrCompileResourceStatus status = xr_compile_state_status(xa_symbol_registry_state(analyzer->symbol_registry));
    if (status == XR_COMPILE_RESOURCE_OK) CHECK(actual == expected);
    return status;
}

static XrCompileResourceStatus exercise_bindings(XaAnalyzer *analyzer, XaInferContext *ctx,
                                                XaSymbol **bound_output) {
    XrCompileState *state = xa_symbol_registry_state(analyzer->symbol_registry);
    XrCompileResourceStatus status = xr_compile_state_status(state);
    XaSymbol *pending = NULL, *bound = NULL;
    if (status == XR_COMPILE_RESOURCE_OK) {
        pending = xa_symbol_new(analyzer->symbol_registry, "run", XA_SYM_FUNCTION);
        if (!pending) status = xr_compile_state_status(state);
    }
    if (status == XR_COMPILE_RESOURCE_OK) {
        pending->location = (XrLocation) {.line = 3, .column = 1};
        bool accepted = xa_visit_add_symbol_checked(ctx, &pending, 3);
        status = xr_compile_state_status(state);
        if (status == XR_COMPILE_RESOURCE_OK) {
            CHECK(accepted && pending && pending->scope == analyzer->global_scope && pending->id == 1);
            *bound_output = pending; bound = pending; pending = NULL;
            XaSymbol *again = bound;
            bool reused = xa_visit_add_symbol_checked(ctx, &again, 3);
            status = xr_compile_state_status(state);
            if (status == XR_COMPILE_RESOURCE_OK) {
                CHECK(reused && again == bound);
                status = checked_lookup(analyzer, 1, bound);
            } else CHECK(!reused && !again);
        } else CHECK(!accepted && !pending);
    }
    const char *names[] = {"run", "target", "run", "value", "value"};
    const XaSymbolKind kinds[] = {XA_SYM_FUNCTION, XA_SYM_VARIABLE, XA_SYM_FUNCTION, XA_SYM_VARIABLE, XA_SYM_VARIABLE};
    const unsigned lines[] = {4, 5, 3, 6, 7};
    for (unsigned i = 0; i < 5 && status == XR_COMPILE_RESOURCE_OK; ++i) {
        pending = xa_symbol_new(analyzer->symbol_registry, names[i], kinds[i]);
        if (!pending) { status = xr_compile_state_status(state); break; }
        uint32_t id = pending->id;
        pending->location = (XrLocation) {.line = lines[i], .column = 1};
        int before = analyzer->diagnostic_count;
        bool accepted = xa_visit_add_symbol_checked(ctx, &pending, (int) lines[i]);
        status = xr_compile_state_status(state);
        if (status != XR_COMPILE_RESOURCE_OK) { CHECK(!accepted && !pending); break; }
        if (i < 2) {
            CHECK(!accepted && !pending && analyzer->diagnostic_count == before + 1);
            CHECK(analyzer->diagnostics_tail->code == XR_ERR_CMP_REDEFINED_VAR);
            CHECK(analyzer->diagnostics_tail->location.line == lines[i]);
            CHECK(!strcmp(analyzer->diagnostics_tail->location.file, "binding.xray"));
            const char *expected = i == 0 ? "Symbol 'run' is redefined in the same scope" :
                "Symbol 'target' redeclares the compiler-owned target namespace";
            CHECK(!strcmp(analyzer->diagnostics_tail->message, expected));
            status = checked_lookup(analyzer, id, NULL);
        } else {
            CHECK(accepted && pending && pending->scope == analyzer->global_scope);
            CHECK(analyzer->diagnostic_count == before + (i == 4 ? 1 : 0));
            status = checked_lookup(analyzer, id, pending);
            pending = NULL;
        }
        if (status == XR_COMPILE_RESOURCE_OK) status = checked_lookup(analyzer, 1, bound);
    }
    if (pending && !pending->scope) xa_symbol_free(pending);
    return status;
}

static XrCompileResourceStatus pipeline(bool dead, const XrCompileResourceLimits *limits,
                                      XrCompileResourceStats *observed) {
    XrCompileResources *resources = NULL;
    XrCompilerSession *session = NULL;
    XaAnalyzer analyzer = {0};
    XaSymbol *bound = NULL;
    bool producer_closed = false;
    XrCompileResourceStatus status = xr_compile_resources_new(limits, &resources);
    if (status == XR_COMPILE_RESOURCE_OK) status = session_open(resources, &session);
    XrCompileState *state = session ? xr_compile_session_compile_state(session) : NULL;
    if (status == XR_COMPILE_RESOURCE_OK) status = xa_analyzer_storage_open(state, &analyzer);
    if (status == XR_COMPILE_RESOURCE_OK && dead) {
        xr_compile_session_free(session); session = NULL;
        xr_compile_resources_release(resources); producer_closed = true;
    }
    char file[] = "owned.xray", message[] = "first";
    XrLocation location = {.file = file, .line = 11, .column = 2};
    XaInferContext ctx = {.analyzer = &analyzer, .file_path = "binding.xray"};
    if (status == XR_COMPILE_RESOURCE_OK) {
        xa_analyzer_add_diagnostic(&analyzer, XR_DIAG_SEV_ERROR, 7, message, &location);
        status = xr_compile_state_status(state);
        if (status == XR_COMPILE_RESOURCE_OK) {
            CHECK(analyzer.diagnostic_count == 1 && analyzer.diagnostics == analyzer.diagnostics_tail);
            CHECK(analyzer.diagnostics->message != message && analyzer.diagnostics->location.file != file);
        } else CHECK(!analyzer.diagnostics && !analyzer.diagnostics_tail && !analyzer.diagnostic_count);
    }
    XaDiagnostic *first = analyzer.diagnostics;
    memset(file, '!', sizeof(file) - 1); memset(message, '?', sizeof(message) - 1);
    if (status == XR_COMPILE_RESOURCE_OK) {
        XrLocation duplicate = {.file = "owned.xray", .line = 11, .column = 99};
        xa_analyzer_add_diagnostic(&analyzer, XR_DIAG_SEV_ERROR, 7, "first", &duplicate);
        status = xr_compile_state_status(state);
        CHECK(first == analyzer.diagnostics && first == analyzer.diagnostics_tail && analyzer.diagnostic_count == 1);
        CHECK(first->location.column == 2 && !strcmp(first->location.file, "owned.xray") && !strcmp(first->message, "first"));
    }
    if (status == XR_COMPILE_RESOURCE_OK) {
        xa_analyzer_add_diagnostic(&analyzer, XR_DIAG_SEV_WARNING, 8, NULL, NULL);
        status = xr_compile_state_status(state);
        if (status == XR_COMPILE_RESOURCE_OK) CHECK(first->next && analyzer.diagnostic_count == 2 && !first->next->message);
        else CHECK(!first->next && analyzer.diagnostic_count == 1 && analyzer.diagnostics_tail == first);
    }
    if (status == XR_COMPILE_RESOURCE_OK) {
        XaDiagnostic *second = first->next;
        status = xa_analyzer_append_diagnostic_suffix(&analyzer, second, " [default]");
        CHECK(first->next == second && analyzer.diagnostics_tail == second && analyzer.diagnostic_count == 2);
        CHECK(second->code == 8 && second->severity == XR_DIAG_SEV_WARNING && !second->reported && !second->location.file);
        if (status == XR_COMPILE_RESOURCE_OK) CHECK(!strcmp(second->message, " [default]"));
        else CHECK(!second->message);
    }
    if (status == XR_COMPILE_RESOURCE_OK) {
        const char *old = first->message; first->reported = true;
        status = xa_analyzer_append_diagnostic_suffix(&analyzer, first, first->message + 1);
        CHECK(first->reported && first->code == 7 && first->location.column == 2);
        if (status == XR_COMPILE_RESOURCE_OK) CHECK(!strcmp(first->message, "firstirst"));
        else CHECK(first->message == old && !strcmp(first->message, "first"));
    }
    if (status == XR_COMPILE_RESOURCE_OK)
        status = exercise_bindings(&analyzer, &ctx, &bound);
    if (analyzer.symbol_registry && status != XR_COMPILE_RESOURCE_OK) {
        XaDiagnostic *old_head = analyzer.diagnostics, *old_tail = analyzer.diagnostics_tail;
        int old_count = analyzer.diagnostic_count;
        size_t attempts = attempt_count; XrCompileResourceStats before = stats(resources);
        xa_analyzer_add_diagnostic(&analyzer, XR_DIAG_SEV_ERROR, 1,
            (const char *)(uintptr_t) 1, (XrLocation *)(uintptr_t) 1);
        CHECK(xa_analyzer_append_diagnostic_suffix(&analyzer, (XaDiagnostic *)(uintptr_t) 1,
            (const char *)(uintptr_t) 1) == status);
        XaSymbol *borrowed = bound;
        CHECK(!xa_visit_add_symbol_checked(&ctx, &borrowed, 0) && !borrowed);
        if (bound) CHECK(bound->scope == analyzer.global_scope && bound->id == 1);
        XrCompileResourceStats after = stats(resources);
        CHECK(attempts == attempt_count && before.allocated_bytes == after.allocated_bytes && before.live_bytes == after.live_bytes && before.work == after.work);
        CHECK(analyzer.diagnostics == old_head && analyzer.diagnostics_tail == old_tail && analyzer.diagnostic_count == old_count);
    }
    if (state) CHECK(xr_compile_state_status(state) == status);
    if (resources) *observed = stats(resources);
    size_t attempts = attempt_count;
    xa_analyzer_clear_diagnostics(&analyzer); xa_analyzer_clear_diagnostics(&analyzer);
    CHECK(!analyzer.diagnostics && !analyzer.diagnostics_tail && !analyzer.diagnostic_count && attempts == attempt_count);
    xa_analyzer_storage_close(&analyzer);
    xr_compile_session_free(session);
    if (!producer_closed) xr_compile_resources_release(resources);
    CHECK(!physical_live && !allocation_count);
    return status;
}

static void matrix(bool dead) {
    reset(); boundary_count = 0; record_work = true;
    XrCompileResourceStats baseline = {0};
    CHECK(pipeline(dead, &ample, &baseline) == XR_COMPILE_RESOURCE_OK);
    record_work = false; size_t sites = attempt_count, cuts = boundary_count;
    CHECK(sites && cuts);
    XrCompileResourceLimits exact = {baseline.allocated_bytes, baseline.peak_bytes, baseline.work};
    for (unsigned axis = 0; axis < 3; ++axis) for (int delta = -1; delta <= 1; ++delta) {
        XrCompileResourceLimits limits = exact;
        uint64_t *limit = axis == 0 ? &limits.allocated_bytes : axis == 1 ? &limits.live_bytes : &limits.work;
        *limit = (uint64_t) ((int64_t) *limit + delta);
        reset(); XrCompileResourceStats observed = {0};
        CHECK(pipeline(dead, &limits, &observed) == (delta < 0 ? XR_COMPILE_RESOURCE_BUDGET : XR_COMPILE_RESOURCE_OK));
    }
    for (size_t i = 0; i < sites; ++i) {
        reset(); fail_at = i; XrCompileResourceStats observed = {0};
        CHECK(pipeline(dead, &ample, &observed) == XR_COMPILE_RESOURCE_OUT_OF_MEMORY);
    }
    for (size_t i = 0; i < cuts; ++i) {
        reset(); XrCompileResourceLimits limits = ample; limits.work = boundaries[i] - 1;
        XrCompileResourceStats observed = {0};
        CHECK(pipeline(dead, &limits, &observed) == XR_COMPILE_RESOURCE_BUDGET);
    }
    printf("diagnostics/binding dead%d: %zu actual allocation faults/%zu actual work cuts; axes/sticky/atomic/physical0 PASS\n", dead, sites, cuts);
}

static void nullable_dedup_and_record_lifetime(void) {
    reset(); XrCompileResources *resources = NULL; XrCompilerSession *session = NULL;
    XaAnalyzer analyzer = {0};
    CHECK(xr_compile_resources_new(&ample, &resources) == XR_COMPILE_RESOURCE_OK);
    CHECK(session_open(resources, &session) == XR_COMPILE_RESOURCE_OK);
    CHECK(xa_analyzer_storage_open(xr_compile_session_compile_state(session), &analyzer) == XR_COMPILE_RESOURCE_OK);
    xa_analyzer_add_diagnostic(&analyzer, XR_DIAG_SEV_ERROR, 9, NULL, NULL);
    XrLocation zero = {0};
    xa_analyzer_add_diagnostic(&analyzer, XR_DIAG_SEV_ERROR, 9, NULL, &zero);
    CHECK(analyzer.diagnostic_count == 1);
    xa_analyzer_add_diagnostic(&analyzer, XR_DIAG_SEV_ERROR, 9, "", NULL);
    xa_analyzer_add_diagnostic(&analyzer, XR_DIAG_SEV_WARNING, 9, NULL, NULL);
    xa_analyzer_add_diagnostic(&analyzer, XR_DIAG_SEV_ERROR, 10, NULL, NULL);
    XrLocation empty = {.file = ""};
    xa_analyzer_add_diagnostic(&analyzer, XR_DIAG_SEV_ERROR, 9, NULL, &empty);
    CHECK(analyzer.diagnostic_count == 5);
    zero.column = 6;
    xa_analyzer_add_diagnostic(&analyzer, XR_DIAG_SEV_ERROR, 9, NULL, &zero);
    CHECK(analyzer.diagnostic_count == 5);
    int count = -1; XaDiagnostic *first = xa_analyzer_get_diagnostics(&analyzer, &count);
    CHECK(first && count == 5 && xa_analyzer_get_diagnostics(&analyzer, NULL) == first);
    CHECK(first->next->message && !first->next->message[0] && first->next->next->severity == XR_DIAG_SEV_WARNING);
    xa_analyzer_storage_close(&analyzer);
    xr_compile_session_free(session); xr_compile_resources_release(resources);
    CHECK(analyzer.diagnostics == first && first->code == 9 && first->next->message[0] == 0);
    CHECK(analyzer.diagnostics_tail->location.file && !analyzer.diagnostics_tail->location.file[0]);
    xa_analyzer_clear_diagnostics(&analyzer); CHECK(!physical_live && !allocation_count);
    count = 3; CHECK(!xa_analyzer_get_diagnostics(NULL, &count) && !count);
    puts("nullable source-order dedup and diagnostic-only state lifetime PASS");
}
static void real_binding_and_suffix_guards(void) {
    for (unsigned mode = 0; mode < 4; ++mode) {
        reset(); XrCompileResources *resources = NULL; XrCompilerSession *session = NULL;
        XaAnalyzer a = {0}, b = {0};
        CHECK(xr_compile_resources_new(&ample, &resources) == XR_COMPILE_RESOURCE_OK);
        CHECK(session_open(resources, &session) == XR_COMPILE_RESOURCE_OK);
        XrCompileState *state = xr_compile_session_compile_state(session);
        CHECK(xa_analyzer_storage_open(state, &a) == XR_COMPILE_RESOURCE_OK);
        CHECK(xa_analyzer_storage_open(state, &b) == XR_COMPILE_RESOURCE_OK);
        XaInferContext ctx = {.analyzer = &a, .file_path = "guards.xray"};
        if (mode == 0) {
            XaSymbol *target = xa_symbol_new(a.symbol_registry, "target", XA_SYM_MODULE);
            CHECK(target); target->is_builtin = true;
            CHECK(xa_visit_add_symbol_checked(&ctx, &target, 1));
            XaSymbol *owner = target; uint32_t id = target->id;
            target->is_builtin = false;
            CHECK(!xa_visit_add_symbol_checked(&ctx, &target, 1) && !target);
            CHECK(xr_compile_state_status(state) == XR_COMPILE_RESOURCE_OK);
            CHECK(owner->scope == a.global_scope && xa_symbol_registry_lookup(a.symbol_registry, id) == owner);
            CHECK(a.diagnostic_count == 1 && a.diagnostics->code == XR_ERR_CMP_REDEFINED_VAR);
        } else if (mode == 1) {
            XaSymbol *foreign = xa_symbol_new(b.symbol_registry, "foreign", XA_SYM_VARIABLE);
            CHECK(foreign && xa_scope_add_symbol(b.global_scope, foreign) == XR_COMPILE_RESOURCE_OK);
            XaSymbol *owner = foreign;
            CHECK(!xa_visit_add_symbol_checked(&ctx, &foreign, 1) && !foreign);
            CHECK(xr_compile_state_status(state) == XR_COMPILE_RESOURCE_BAD_ARGUMENT);
            CHECK(owner->scope == b.global_scope && !a.global_scope->owned_symbols);
        } else if (mode == 2) {
            xa_analyzer_add_diagnostic(&a, XR_DIAG_SEV_ERROR, 12, "left", NULL);
            xa_analyzer_add_diagnostic(&b, XR_DIAG_SEV_ERROR, 13, "right", NULL);
            CHECK(a.diagnostics && b.diagnostics);
            const char *left = a.diagnostics->message, *right = b.diagnostics->message;
            CHECK(xa_analyzer_append_diagnostic_suffix(&a, b.diagnostics, "suffix") == XR_COMPILE_RESOURCE_BAD_ARGUMENT);
            CHECK(a.diagnostics->message == left && b.diagnostics->message == right);
            CHECK(!strcmp(left, "left") && !strcmp(right, "right"));
            int count = 0; CHECK(xa_analyzer_get_diagnostics(&a, &count) == a.diagnostics && count == 1);
        } else {
            XaSymbol *builtin = xa_symbol_new(a.symbol_registry, "Result", XA_SYM_ENUM);
            CHECK(builtin); builtin->is_builtin = true; builtin->location.line = 1;
            CHECK(xa_visit_add_symbol_checked(&ctx, &builtin, 1));
            XaSymbol *redeclared = xa_symbol_new(a.symbol_registry, "Result", XA_SYM_FUNCTION);
            CHECK(redeclared); redeclared->location.line = 2;
            CHECK(!xa_visit_add_symbol_checked(&ctx, &redeclared, 2) && !redeclared);
            CHECK(a.diagnostic_count == 1 && !strcmp(a.diagnostics->message,
                "'Result' is a builtin prelude enum and cannot be redeclared"));
            CHECK(xa_scope_lookup_local(a.global_scope, "Result") == builtin);
        }
        xa_analyzer_clear_diagnostics(&a); xa_analyzer_clear_diagnostics(&b);
        xa_analyzer_storage_close(&a); xa_analyzer_storage_close(&b);
        xr_compile_session_free(session); xr_compile_resources_release(resources);
        CHECK(!physical_live && !allocation_count);
    }
    puts("reserved namespace before real-ID reuse; foreign owned creator; foreign diagnostic suffix; builtin enum rejection PASS");
}
int main(void) {
    matrix(false); matrix(true); nullable_dedup_and_record_lifetime(); real_binding_and_suffix_guards();
    return 0;
}
