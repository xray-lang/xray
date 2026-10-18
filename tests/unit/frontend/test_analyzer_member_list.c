/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_analyzer_member_list.c - Real completion-list ownership and failure paths
 */
#include "frontend/analyzer/xanalyzer_member_list.h"
#include "frontend/analyzer/xanalyzer_builtins.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#define xr_compile_resources_work xr_test_actual_resources_work
#include "../base/parser_resources_observer.h"
#undef xr_compile_resources_work
#include "analyzer_registry_fixture.h"
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

static XrCompileResourceStatus pipeline(bool class_members, bool dead,
    const XrCompileResourceLimits *limits, XrCompileResourceStats *observed) {
    XrCompileResources *resources = NULL;
    XrCompilerSession *session = NULL;
    XaSymbolRegistry *registry = NULL;
    XaScope *scope = NULL;
    XrClassInfo *base = NULL, *derived = NULL;
    XaSymbol *symbols[4] = {0};
    XaMemberList result = {0};
    bool producer_closed = false;
    XrCompileResourceStatus status = xr_compile_resources_new(limits, &resources);
    if (status == XR_COMPILE_RESOURCE_OK) status = session_open(resources, &session);
    XrCompileState *state = session ? xr_compile_session_compile_state(session) : NULL;
    if (status == XR_COMPILE_RESOURCE_OK) status = open_registry_with_pool(state, &registry);
    if (status == XR_COMPILE_RESOURCE_OK) {
        scope = xa_scope_new(registry, XA_SCOPE_GLOBAL, NULL);
        if (!scope) status = xr_compile_state_status(state);
    }
    if (class_members && status == XR_COMPILE_RESOURCE_OK) {
        base = xa_class_info_new(state, "Base");
        if (!base) status = xr_compile_state_status(state);
        if (status == XR_COMPILE_RESOURCE_OK) {
            derived = xa_class_info_new(state, "Derived");
            if (!derived) status = xr_compile_state_status(state);
            else derived->base = base;
        }
        for (unsigned i = 0; i < 4 && status == XR_COMPILE_RESOURCE_OK; ++i) {
            const char *names[] = {"value", "run", "baseValue", "run"};
            symbols[i] = xa_symbol_new(registry, names[i], i % 2 ? XA_SYM_METHOD : XA_SYM_FIELD);
            if (!symbols[i]) { status = xr_compile_state_status(state); break; }
            status = xa_scope_add_symbol(scope, symbols[i]);
            if (status != XR_COMPILE_RESOURCE_OK) break;
            XrClassInfo *owner = i < 2 ? derived : base;
            status = i % 2 ? xa_class_info_add_method(state, owner, symbols[i])
                           : xa_class_info_add_field(state, owner, symbols[i]);
        }
    }
    if (status == XR_COMPILE_RESOURCE_OK && dead) {
        xr_compile_session_free(session); session = NULL;
        xr_compile_resources_release(resources);
        xa_symbol_registry_release(registry);
        producer_closed = true;
    }
    if (status == XR_COMPILE_RESOURCE_OK && class_members) {
        status = xa_member_list_class(registry, derived, &result);
        if (status == XR_COMPILE_RESOURCE_OK) {
            CHECK(!result.owns_symbols && result.count == 4);
            for (int i = 0; i < 4; ++i) CHECK(result.items[i] == symbols[i]);
        }
    } else if (status == XR_COMPILE_RESOURCE_OK) {
        char names[35][20];
        XaBuiltinMember members[35] = {0};
        for (unsigned i = 0; i < 35; ++i) {
            snprintf(names[i], sizeof(names[i]), "member_%u", i);
            members[i].name = i == 7 ? NULL : names[i];
            members[i].is_method = i % 2 != 0;
            members[i].is_static = i % 3 == 0;
            members[i].is_internal = i % 5 == 0;
        }
        XaBuiltinType metadata = {.name = "FixtureType", .members = members, .member_count = 35};
        status = xa_member_list_builtins(registry, &metadata, &result);
        memset(names, '!', sizeof(names));
        if (status == XR_COMPILE_RESOURCE_OK) {
            CHECK(result.owns_symbols && result.count == 34);
            int index = 0;
            for (unsigned i = 0; i < 35; ++i) {
                if (i == 7) continue;
                char expected[20]; snprintf(expected, sizeof(expected), "member_%u", i);
                XaSymbol *symbol = result.items[index];
                CHECK(!strcmp(symbol->name, expected) && symbol->id == (uint32_t) index + 1);
                CHECK(symbol->is_builtin && !symbol->scope);
                CHECK(symbol->kind == (i % 2 ? XA_SYM_METHOD : XA_SYM_FIELD));
                ++index;
            }
        }
    }
    if (status != XR_COMPILE_RESOURCE_OK)
        CHECK(!result.items && !result.count && !result.owns_symbols);
    if (registry && status != XR_COMPILE_RESOURCE_OK) {
        CHECK(!result.items && !result.count && !result.owns_symbols);
        XaMemberList unchanged = {0};
        size_t attempts = attempt_count; XrCompileResourceStats before = stats(resources);
        CHECK(xa_member_list_builtins(registry, (const XaBuiltinType *)(uintptr_t)1, &unchanged) == status);
        CHECK(xa_member_list_class(registry, (const XrClassInfo *)(uintptr_t)1, &unchanged) == status);
        XrCompileResourceStats after = stats(resources);
        CHECK(attempts == attempt_count && before.allocated_bytes == after.allocated_bytes &&
              before.live_bytes == after.live_bytes && before.work == after.work);
        CHECK(!unchanged.items && !unchanged.count && !unchanged.owns_symbols);
    }
    XrCompileResourceStats before = {0};
    if (resources) before = stats(resources);
    size_t attempts = attempt_count;
    xa_member_list_dispose(&result);
    CHECK(!result.items && !result.count && !result.owns_symbols);
    xa_member_list_dispose(&result);
    if (resources) {
        XrCompileResourceStats after = stats(resources);
        CHECK(attempts == attempt_count && before.allocated_bytes == after.allocated_bytes && before.work == after.work);
    }
    if (state)
        CHECK(xr_compile_state_status(state) == status);
    if (status == XR_COMPILE_RESOURCE_OK && class_members) {
        for (int i = 0; i < 4; ++i) CHECK(symbols[i]->scope == scope && symbols[i]->id == (uint32_t) i + 1);
        CHECK(derived->fields[0] == symbols[0] && base->methods[0] == symbols[3]);
    }
    xa_class_info_free(derived); xa_class_info_free(base);
    for (unsigned i = 0; i < 4; ++i) if (symbols[i] && !symbols[i]->scope) xa_symbol_free(symbols[i]);
    if (resources) *observed = stats(resources);
    xa_scope_free(scope);
    if (!producer_closed) xa_symbol_registry_release(registry);
    xr_compile_session_free(session);
    if (!producer_closed) xr_compile_resources_release(resources);
    CHECK(!physical_live && !allocation_count);
    return status;
}

static void resources_matrix(bool class_members, bool dead) {
    reset(); boundary_count = 0; record_work = true;
    XrCompileResourceStats baseline = {0};
    CHECK(pipeline(class_members, dead, &ample, &baseline) == XR_COMPILE_RESOURCE_OK);
    record_work = false;
    size_t sites = attempt_count, cuts = boundary_count;
    CHECK(sites && cuts && physical_total);
    XrCompileResourceLimits exact = {baseline.allocated_bytes, baseline.peak_bytes, baseline.work};
    for (unsigned axis = 0; axis < 3; ++axis) for (int delta = -1; delta <= 1; ++delta) {
        XrCompileResourceLimits limits = exact;
        uint64_t *bound = axis == 0 ? &limits.allocated_bytes : axis == 1 ? &limits.live_bytes : &limits.work;
        *bound = (uint64_t) ((int64_t) *bound + delta);
        reset(); XrCompileResourceStats observed = {0};
        CHECK(pipeline(class_members, dead, &limits, &observed) ==
              (delta < 0 ? XR_COMPILE_RESOURCE_BUDGET : XR_COMPILE_RESOURCE_OK));
    }
    for (size_t i = 0; i < sites; ++i) {
        reset(); fail_at = i; XrCompileResourceStats observed = {0};
        CHECK(pipeline(class_members, dead, &ample, &observed) == XR_COMPILE_RESOURCE_OUT_OF_MEMORY);
    }
    for (size_t i = 0; i < cuts; ++i) {
        reset(); XrCompileResourceLimits limits = ample; limits.work = boundaries[i] - 1;
        XrCompileResourceStats observed = {0};
        CHECK(pipeline(class_members, dead, &limits, &observed) == XR_COMPILE_RESOURCE_BUDGET);
    }
    printf("member list class%d dead%d: all %zu allocator faults/%zu actual work cuts; axes+sticky+physical0 PASS\n",
           class_members, dead, sites, cuts);
}

static void detached_list_lifetime_and_guards(void) {
    for (unsigned mode = 0; mode < 5; ++mode) {
        reset(); XrCompileResources *resources = NULL; XrCompileState *state = NULL;
        XaSymbolRegistry *registry = NULL; XaMemberList list = {0};
        CHECK(xr_compile_resources_new(&ample, &resources) == XR_COMPILE_RESOURCE_OK);
        CHECK(xr_compile_state_new(resources, &state) == XR_COMPILE_RESOURCE_OK);
        CHECK(open_registry_with_pool(state, &registry) == XR_COMPILE_RESOURCE_OK);
        XaBuiltinMember members[] = {{.name = "method", .is_method = true}, {.name = "field"}};
        XaBuiltinType metadata = {.members = members, .member_count = 2};
        XrClassInfo *base = NULL, *derived = NULL;
        if (mode < 2) {
            CHECK(xa_member_list_builtins(registry, &metadata, &list) == XR_COMPILE_RESOURCE_OK);
            CHECK(!xa_symbol_registry_lookup(registry, list.items[0]->id));
            CHECK(xr_compile_state_status(state) == XR_COMPILE_RESOURCE_OK);
            if (mode == 1) {
                XaMemberList old = list;
                CHECK(xa_member_list_builtins(registry, &metadata, &list) == XR_COMPILE_RESOURCE_BAD_ARGUMENT);
                CHECK(list.items == old.items && list.count == old.count && list.owns_symbols == old.owns_symbols);
            }
        } else if (mode == 2) {
            metadata.member_count = -1;
            CHECK(xa_member_list_builtins(registry, &metadata, &list) == XR_COMPILE_RESOURCE_BAD_ARGUMENT);
            CHECK(!list.items && !list.count);
        } else {
            base = xa_class_info_new(state, "Base"); derived = xa_class_info_new(state, "Derived");
            CHECK(base && derived); derived->base = base;
            if (mode == 3) {
                base->base = derived;
                CHECK(xa_member_list_class(registry, derived, &list) == XR_COMPILE_RESOURCE_BAD_ARGUMENT);
                base->base = NULL;
            } else {
                derived->field_count = 1;
                CHECK(xa_member_list_class(registry, derived, &list) == XR_COMPILE_RESOURCE_BAD_ARGUMENT);
                derived->field_count = 0;
            }
            CHECK(!list.items && !list.count);
        }
        xa_class_info_free(derived); xa_class_info_free(base);
        xa_symbol_registry_release(registry); xr_compile_state_release(state);
        xr_compile_resources_release(resources);
        if (mode < 2) CHECK(!strcmp(list.items[0]->name, "method") && !strcmp(list.items[1]->name, "field"));
        xa_member_list_dispose(&list);
        CHECK(!physical_live && !allocation_count);
    }
    puts("detached real IDs remain unbound; owning list alone survives original handles; nonempty/malformed/cyclic outputs fail closed PASS");
}

int main(void) {
    resources_matrix(false, false); resources_matrix(false, true);
    resources_matrix(true, false); resources_matrix(true, true);
    detached_list_lifetime_and_guards();
    return 0;
}
