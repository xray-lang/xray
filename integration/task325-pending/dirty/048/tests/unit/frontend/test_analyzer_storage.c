/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_analyzer_storage.c - Independent ownership, fault and resource witnesses
 */
#include "frontend/analyzer/xanalyzer.h"
#include "frontend/analyzer/xanalyzer_storage.h"
#include "frontend/analyzer/xanalyzer_export_query.h"
#include "frontend/analyzer/xanalyzer_native_input.h"
#include "frontend/analyzer/xa_native_effect.h"
#include "frontend/analyzer/xanalyzer_vtable.h"
#include "toolchain/xcompiler_session.h"
#include "toolchain/xcompiler_arena_backing.h"
#include "base/xarena.h"
#include "base/xtoml.h"
#include "runtime/value/xstruct_layout.h"
#include "source_identity/native_point.h"
#include "frontend/parser/xparse.h"
#include "frontend/parser/xtype_ref.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#define xr_compile_resources_work xr_test_actual_resources_work
#include "../base/parser_resources_observer.h"
#undef xr_compile_resources_work
static bool record_vtable_work;
static uint64_t vtable_work_bounds[65536];
static size_t vtable_work_count;
/* Observe the real ledger call and return its unmodified status. */
XR_FUNC XrCompileResourceStatus xr_compile_resources_work(XrCompileResources *resources, uint64_t units) {
    XrCompileResourceStatus status = xr_test_actual_resources_work(resources, units);
    if (record_vtable_work && status == XR_COMPILE_RESOURCE_OK && units) {
        XrCompileResourceStats current = {0};
        CHECK(xr_compile_resources_stats(resources, &current) == XR_COMPILE_RESOURCE_OK);
        CHECK(vtable_work_count < sizeof(vtable_work_bounds) / sizeof(vtable_work_bounds[0]));
        vtable_work_bounds[vtable_work_count++] = current.work;
    }
    return status;
}

static const XrCompileResourceLimits generous = {1048576, 1048576, 1048576};

static XrCompileResourceStatus session_result(XrCompilerSessionStatus status) {
    switch (status) {
        case XR_COMPILER_SESSION_OK: return XR_COMPILE_RESOURCE_OK;
        case XR_COMPILER_SESSION_BAD_ARGUMENT: return XR_COMPILE_RESOURCE_BAD_ARGUMENT;
        case XR_COMPILER_SESSION_BUDGET: return XR_COMPILE_RESOURCE_BUDGET;
        case XR_COMPILER_SESSION_OUT_OF_MEMORY: return XR_COMPILE_RESOURCE_OUT_OF_MEMORY;
    }
    CHECK(false);
    return XR_COMPILE_RESOURCE_BAD_ARGUMENT;
}

static XrCompileResourceStats stats(XrCompileResources *resources) {
    XrCompileResourceStats result = {0};
    CHECK(xr_compile_resources_stats(resources, &result) == XR_COMPILE_RESOURCE_OK);
    CHECK(result.live_bytes == physical_live && result.allocated_bytes == physical_total);
    CHECK(result.peak_bytes == physical_peak);
    return result;
}

static void empty_storage(const XaAnalyzer *analyzer) {
    CHECK(!analyzer->consteval_arena && !analyzer->callable_target_arena && !analyzer->type_pool);
    CHECK(analyzer->semantic_revision == 73);
}

static XrCompileResourceStatus pipeline(const XrCompileResourceLimits *limits,
    XrCompileResourceStats *result) {
    XrCompileResources *resources = NULL;
    XrCompilerSession *session = NULL;
    XaAnalyzer analyzer = {0};
    analyzer.semantic_revision = 73;
    XrCompileResourceStatus status = xr_compile_resources_new(limits, &resources);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    status = session_result(xr_compile_session_new(resources, &session));
    if (status != XR_COMPILE_RESOURCE_OK) {
        xr_compile_resources_release(resources);
        return status;
    }
    XrCompileState *state = xr_compile_session_compile_state(session);
    status = xa_analyzer_storage_open(state, &analyzer);
    if (status == XR_COMPILE_RESOURCE_OK) {
        CHECK(xr_compile_state_resources(analyzer.type_pool->state) == resources);
        CHECK(xr_compiler_arena_matches_state(analyzer.consteval_arena, state));
        CHECK(xr_compiler_arena_matches_state(analyzer.callable_target_arena, state));
        XrType *type = xr_pool_alloc_type(analyzer.type_pool, XR_KIND_INT);
        if (type) CHECK(type->kind == XR_KIND_INT && type->id == 1);
        char *constant = xr_arena_strdup(analyzer.consteval_arena, "ctfe");
        char *target = xr_arena_strdup(analyzer.callable_target_arena, "callee");
        char *name = xr_pool_strdup(analyzer.type_pool, "owned");
        if (constant) CHECK(!strcmp(constant, "ctfe"));
        if (target) CHECK(!strcmp(target, "callee"));
        if (name) CHECK(!strcmp(name, "owned"));
        status = xr_compile_state_status(state);
        if (status == XR_COMPILE_RESOURCE_OK) CHECK(type && constant && target && name);
    } else {
        empty_storage(&analyzer);
        size_t attempts = attempt_count;
        XrCompileResourceStats failed = stats(resources);
        CHECK(xa_analyzer_storage_open(state, &analyzer) == status);
        CHECK(attempt_count == attempts);
        XrCompileResourceStats retry = stats(resources);
        CHECK(retry.work == failed.work && retry.allocated_bytes == failed.allocated_bytes);
    }
    *result = stats(resources);
    size_t attempts = attempt_count;
    xa_analyzer_storage_close(&analyzer);
    xa_analyzer_storage_close(&analyzer);
    empty_storage(&analyzer);
    CHECK(attempt_count == attempts);
    xr_compile_session_free(session);
    XrCompileResourceStats released = stats(resources);
    CHECK(released.work == result->work && released.allocated_bytes == result->allocated_bytes);
    xr_compile_resources_release(resources);
    CHECK(!physical_live && !allocation_count);
    return status;
}

static void producer_death_and_shared_failure(void) {
    reset();
    XrCompileResources *resources = NULL;
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_resources_new(&generous, &resources) == XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_session_new(resources, &session) == XR_COMPILER_SESSION_OK);
    XrCompileState *state = xr_compile_session_compile_state(session);
    XaAnalyzer analyzer = {0};
    analyzer.semantic_revision = 73;
    CHECK(xa_analyzer_storage_open(state, &analyzer) == XR_COMPILE_RESOURCE_OK);
    xr_compile_session_free(session);
    xr_compile_resources_release(resources);
    CHECK(!strcmp(xr_arena_strdup(analyzer.consteval_arena, "after"), "after"));
    CHECK(!strcmp(xr_pool_strdup(analyzer.type_pool, "producer"), "producer"));
    CHECK(xr_pool_alloc_type(analyzer.type_pool, XR_KIND_BOOL)->id == 1);
    XrCompileResourceStats before = stats(resources);
    CHECK(xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BUDGET) == XR_COMPILE_RESOURCE_BUDGET);
    CHECK(!xr_arena_alloc_raw(analyzer.consteval_arena, 8));
    CHECK(!xr_arena_alloc(analyzer.callable_target_arena, 8));
    CHECK(!xr_pool_alloc(analyzer.type_pool, 8));
    XrCompileResourceStats after = stats(resources);
    CHECK(after.work == before.work && after.allocated_bytes == before.allocated_bytes);
    size_t attempts = attempt_count;
    xa_analyzer_storage_close(&analyzer);
    empty_storage(&analyzer);
    CHECK(attempt_count == attempts && !physical_live && !allocation_count);
}

static void occupied_output_and_foreign_state(void) {
    reset();
    XrCompileResources *owner = NULL, *other = NULL;
    XrCompileState *state = NULL, *foreign = NULL;
    CHECK(xr_compile_resources_new(&generous, &owner) == XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_new(&generous, &other) == XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_state_new(owner, &state) == XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_state_new(other, &foreign) == XR_COMPILE_RESOURCE_OK);
    XaAnalyzer analyzer = {0};
    analyzer.semantic_revision = 73;
    CHECK(xa_analyzer_storage_open(state, &analyzer) == XR_COMPILE_RESOURCE_OK);
    XrArena *constant = analyzer.consteval_arena, *callable = analyzer.callable_target_arena;
    XrTypePool *pool = analyzer.type_pool;
    XrCompileResourceStats owner_before;
    CHECK(xr_compile_resources_stats(owner, &owner_before) == XR_COMPILE_RESOURCE_OK);
    size_t attempts = attempt_count;
    CHECK(xa_analyzer_storage_open(foreign, &analyzer) == XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    CHECK(analyzer.consteval_arena == constant && analyzer.callable_target_arena == callable);
    CHECK(analyzer.type_pool == pool && analyzer.semantic_revision == 73);
    CHECK(attempt_count == attempts && xr_compile_state_status(state) == XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats owner_after;
    CHECK(xr_compile_resources_stats(owner, &owner_after) == XR_COMPILE_RESOURCE_OK);
    CHECK(owner_after.work == owner_before.work && owner_after.allocated_bytes == owner_before.allocated_bytes);
    CHECK(!strcmp(xr_arena_strdup(constant, "still"), "still"));
    xa_analyzer_storage_close(&analyzer);
    xr_compile_state_release(state);
    xr_compile_state_release(foreign);
    xr_compile_resources_release(owner);
    xr_compile_resources_release(other);
    CHECK(!physical_live && !allocation_count);
}

static XrCompileResourceStatus export_query_pipeline(const XrCompileResourceLimits *limits,
    XrCompileResourceStats *observed) {
    XrCompileResources *resources = NULL;
    XrCompilerSession *session = NULL;
    XrHashMap *exports = NULL;
    XaAnalyzer analyzer = {0}; XaSymbol expected = {0};
    XaSymbol *output = &expected;
    XrCompileResourceStatus status = xr_compile_resources_new(limits, &resources);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    status = session_result(xr_compile_session_new(resources, &session));
    analyzer.compiler_session = session;
    if (status == XR_COMPILE_RESOURCE_OK) {
        XrOsIoPolicy policy = xr_compile_io_policy(resources);
        XrOsIoStatus created = xr_hashmap_owned_new(&policy, &exports);
        if (xa_accept_export_io(&analyzer, created)) {
            CHECK(xa_accept_export_io(&analyzer, xr_hashmap_owned_set(exports, "answer", &expected)) ||
                xr_compile_session_resource_status(session) != XR_COMPILE_RESOURCE_OK);
            if (xr_compile_session_resource_status(session) == XR_COMPILE_RESOURCE_OK) {
                CHECK(xa_find_export_symbol(&analyzer, exports, "answer", &output) ||
                    xr_compile_session_resource_status(session) != XR_COMPILE_RESOURCE_OK);
                if (xr_compile_session_resource_status(session) == XR_COMPILE_RESOURCE_OK) {
                    CHECK(output == &expected);
                    CHECK(xa_find_export_symbol(&analyzer, exports, "missing", &output) ||
                        xr_compile_session_resource_status(session) != XR_COMPILE_RESOURCE_OK);
                    if (xr_compile_session_resource_status(session) == XR_COMPILE_RESOURCE_OK) CHECK(!output);
                }
            }
        }
        status = xr_compile_session_resource_status(session);
        if (status != XR_COMPILE_RESOURCE_OK) {
            XrCompileResourceStats failed = stats(resources);
            size_t attempts = attempt_count;
            output = &expected;
            CHECK(!xa_find_export_symbol(&analyzer, exports, "answer", &output));
            CHECK(output == &expected);
            CHECK(!xa_find_export_symbol(&analyzer, NULL, NULL, &output));
            CHECK(output == &expected && attempt_count == attempts);
            XrCompileResourceStats retry = stats(resources);
            CHECK(retry.work == failed.work && retry.allocated_bytes == failed.allocated_bytes);
        }
    }
    *observed = stats(resources);
    xr_hashmap_owned_free(exports);xr_compile_session_free(session);
    XrCompileResourceStats after = stats(resources);
    CHECK(after.work == observed->work && after.allocated_bytes == observed->allocated_bytes);
    xr_compile_resources_release(resources);
    CHECK(!physical_live && !allocation_count);
    return status;
}

static void export_query_resources(void) {
    reset();XrCompileResourceStats baseline = {0};
    CHECK(export_query_pipeline(&generous, &baseline) == XR_COMPILE_RESOURCE_OK);
    size_t sites = attempt_count;CHECK(sites > 0);
    for (size_t ordinal = 0; ordinal < sites; ++ordinal) {
        reset();fail_at = ordinal;XrCompileResourceStats observed = {0};
        CHECK(export_query_pipeline(&generous, &observed) == XR_COMPILE_RESOURCE_OUT_OF_MEMORY);
    }
    for (unsigned axis = 0; axis < 3; ++axis) for (int delta = -1; delta <= 1; ++delta) {
        reset();XrCompileResourceStats observed = {0};
        XrCompileResourceLimits limits = {baseline.allocated_bytes, baseline.peak_bytes, baseline.work};
        uint64_t *bound = axis == 0 ? &limits.allocated_bytes : axis == 1 ? &limits.live_bytes : &limits.work;
        CHECK(*bound > 1);*bound = (uint64_t)((int64_t)*bound + delta);
        CHECK(export_query_pipeline(&limits, &observed) ==
            (delta < 0 ? XR_COMPILE_RESOURCE_BUDGET : XR_COMPILE_RESOURCE_OK));
    }
    printf("export query: %zu allocation faults, three exact/minus1/plus1 axes, "
        "output preservation, sticky no-charge retry and physical zero passed\n", sites);
}

static XrCompileResourceStatus source_path_pipeline(const XrCompileResourceLimits *limits,
    XrCompileResourceStats *observed) {
    XrCompileResources *resources = NULL;XrCompilerSession *session = NULL;
    XrCompileResourceStatus status = xr_compile_resources_new(limits, &resources);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    status = session_result(xr_compile_session_new(resources, &session));
    XaAnalyzer analyzer = {0};analyzer.compiler_session = session;
    if (status == XR_COMPILE_RESOURCE_OK) {
        bool equal = false;
        bool accepted = xa_export_source_path_equal(&analyzer,
            XR_ANALYZER_SOURCE_FIXTURE "/left/same.xr",
            XR_ANALYZER_SOURCE_FIXTURE "/left/./same.xr", &equal);
        if (accepted) {
            CHECK(equal);
            equal = true;
            accepted = xa_export_source_path_equal(&analyzer,
                XR_ANALYZER_SOURCE_FIXTURE "/left/same.xr", "same.xr", &equal);
            if (accepted) {
                CHECK(!equal);
                equal = false;
                accepted = xa_export_source_path_equal(&analyzer,
                    "<embedded stdlib>/io/io.xr", "<embedded stdlib>/io/io.xr", &equal);
                if (accepted) {
                    CHECK(equal);
                    accepted = xa_export_identity_equal(&analyzer, "module/v1/left/same.xr",
                        "module/v1/right/same.xr", &equal);
                    if (accepted) CHECK(!equal);
                    else CHECK(equal);
                } else CHECK(!equal);
            } else CHECK(equal);
        } else CHECK(!equal);
        status = xr_compile_session_resource_status(session);
        CHECK(accepted == (status == XR_COMPILE_RESOURCE_OK));
        if (!accepted) {
            XrCompileResourceStats failed = stats(resources);size_t attempts = attempt_count;
            equal = true;
            CHECK(!xa_export_source_path_equal(&analyzer, "same.xr", "same.xr", &equal) && equal);
            CHECK(!xa_export_source_path_equal(&analyzer, NULL, NULL, &equal) && equal);
            XrCompileResourceStats retry = stats(resources);
            CHECK(attempt_count == attempts && retry.work == failed.work &&
                retry.allocated_bytes == failed.allocated_bytes);
        }
    }
    *observed = stats(resources);
    xr_compile_session_free(session);
    XrCompileResourceStats after = stats(resources);
    CHECK(after.work == observed->work && after.allocated_bytes == observed->allocated_bytes);
    xr_compile_resources_release(resources);CHECK(!physical_live && !allocation_count);
    return status;
}

static void source_path_resources(void) {
    reset();XrCompileResourceStats baseline = {0};
    CHECK(source_path_pipeline(&generous, &baseline) == XR_COMPILE_RESOURCE_OK);
    size_t sites = attempt_count;CHECK(sites > 0);
    for (size_t ordinal = 0; ordinal < sites; ++ordinal) {
        reset();fail_at = ordinal;XrCompileResourceStats observed = {0};
        CHECK(source_path_pipeline(&generous, &observed) == XR_COMPILE_RESOURCE_OUT_OF_MEMORY);
    }
    for (unsigned axis = 0; axis < 3; ++axis) for (int delta = -1; delta <= 1; ++delta) {
        reset();XrCompileResourceStats observed = {0};
        XrCompileResourceLimits limits = {baseline.allocated_bytes, baseline.peak_bytes, baseline.work};
        uint64_t *bound = axis == 0 ? &limits.allocated_bytes : axis == 1 ? &limits.live_bytes : &limits.work;
        CHECK(*bound > 1);*bound = (uint64_t)((int64_t)*bound + delta);
        CHECK(source_path_pipeline(&limits, &observed) ==
            (delta < 0 ? XR_COMPILE_RESOURCE_BUDGET : XR_COMPILE_RESOURCE_OK));
    }
    printf("source path: %zu allocation faults, actual aliases and same-suffix distinct files, "
        "exact virtual IDs, three axes, sticky output/no-charge retry, physical zero passed\n", sites);
}

static const char native_manifest[] =
    "[native]\nname='effects'\nversion='1.0.0'\nlicense='MIT'\nsource='local'\n"
    "audit_mode='exploratory'\nvm='unsupported'\n"
    "[[native.unit]]\nname='platform'\nkind='platform'\npurpose='host function'\n"
    "optimization='none'\nvisibility='default'\nwarnings='strict'\n"
    "[[native.symbol]]\nxray='quiet'\nnative='host_quiet'\nkind='function'\n"
    "calling_convention='c'\nunit='platform'\n[native.symbol.contract]\n"
    "params=[]\nreturn={ownership='value',nullable=false,validity='always'}\n"
    "effects=['foreign']\ncallbacks=[]\nfailure='none'\nallocation='none'\n"
    "blocking='never'\nsuspend='never'\nio='none'\nsync='none'\npanic='never'\nerror='none'\n"
    "[[native.symbol]]\nxray='invoke'\nnative='host_invoke'\nkind='function'\n"
    "calling_convention='c'\nunit='platform'\n[native.symbol.contract]\n"
    "params=[]\nreturn={ownership='value',nullable=false,validity='always'}\n"
    "effects=['foreign','alloc','may_block','suspend','io','sync','abort']\ncallbacks=[]\n"
    "failure='none'\nallocation='may'\nblocking='may'\nsuspend='may'\n"
    "io='readwrite'\nsync='external'\npanic='abort'\nerror='none'\n";

static XrCompileResourceStatus native_plan_parse(XrCompileResources *resources,
    const char *input, size_t length, XrNativePackagePlan **output) {
    XrOsIoPolicy policy = xr_compile_io_policy(resources);
    XrTomlParseLimits limits = {length + 1, 64};
    XrTomlValue *document = NULL;
    XrTomlParseStatus parsed = xtoml_parse_owned(&policy, input, length, &limits, &document);
    CHECK(parsed == XR_TOML_PARSE_OK || parsed == XR_TOML_PARSE_BUDGET ||
        parsed == XR_TOML_PARSE_OUT_OF_MEMORY);
    XrManifestStatus status = parsed == XR_TOML_PARSE_OK
        ? xr_native_package_plan_parse_owned(&policy, document,
            XR_ANALYZER_SOURCE_FIXTURE, output, NULL)
        : parsed == XR_TOML_PARSE_BUDGET ? XR_MANIFEST_BUDGET : XR_MANIFEST_OUT_OF_MEMORY;
    xtoml_owned_free(document);
    CHECK(status == XR_MANIFEST_OK || status == XR_MANIFEST_BUDGET ||
        status == XR_MANIFEST_OUT_OF_MEMORY);
    return status == XR_MANIFEST_OK ? XR_COMPILE_RESOURCE_OK :
        status == XR_MANIFEST_BUDGET ? XR_COMPILE_RESOURCE_BUDGET : XR_COMPILE_RESOURCE_OUT_OF_MEMORY;
}

static XrCompileResourceStatus native_plan_new(XrCompileResources *resources,
    XrNativePackagePlan **output) {
    return native_plan_parse(resources, native_manifest, sizeof(native_manifest) - 1, output);
}

static XrCompileResourceStatus native_input_pipeline(const XrCompileResourceLimits *limits,
    XrCompileResourceStats *observed) {
    XrCompileResources *resources = NULL;XrCompilerSession *session = NULL;
    XrNativePackagePlan *plan = NULL;
    XaNativeEffectAxioms snapshot = {0};
    XrCompileResourceStatus status = xr_compile_resources_new(limits, &resources);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    status = session_result(xr_compile_session_new(resources, &session));
    XaAnalyzer analyzer = {0};analyzer.compiler_session = session;analyzer.semantic_revision = 1;
    if (status == XR_COMPILE_RESOURCE_OK) status = native_plan_new(resources, &plan);
    /* The manifest factory has no parser state; its caller owns propagation. */
    if (session && status != XR_COMPILE_RESOURCE_OK)
        (void) xr_compile_state_fail(xr_compile_session_compile_state(session), status);
    if (status == XR_COMPILE_RESOURCE_OK) {
        CHECK(xa_analyzer_bind_native_plan(&analyzer, plan) == XR_COMPILE_RESOURCE_OK);
        CHECK(xa_analyzer_native_plan(&analyzer) == plan);
        XaSymbol quiet = {0};quiet.name = "quiet";
        XaNativeEffectAxioms facts = xa_native_effect_axioms(&analyzer, &quiet);
        if (xr_compile_session_resource_status(session) == XR_COMPILE_RESOURCE_OK) {
            CHECK(facts.has_contract && facts.effects == XA_SEM_EFFECT_FOREIGN);
            CHECK(!facts.allocates && !facts.suspends);
            XaSymbol invoke = {0};invoke.name = "invoke";
            snapshot = xa_native_effect_axioms(&analyzer, &invoke);
            if (xr_compile_session_resource_status(session) == XR_COMPILE_RESOURCE_OK) {
                CHECK(snapshot.has_contract && snapshot.allocates && snapshot.suspends);
                CHECK(snapshot.effects == (XA_SEM_EFFECT_FOREIGN | XA_SEM_EFFECT_ALLOC |
                    XA_SEM_EFFECT_SCHED_SUSPEND | XA_SEM_EFFECT_MAY_BLOCK | XA_SEM_EFFECT_ABORT |
                    XA_SEM_EFFECT_IO | XA_SEM_EFFECT_SYNC));
                XaSymbol missing = {0};missing.name = "missing";
                facts = xa_native_effect_axioms(&analyzer, &missing);
                CHECK(!facts.has_contract && !facts.effects && !facts.allocates && !facts.suspends);
            } else CHECK(!snapshot.has_contract && !snapshot.effects);
        } else CHECK(!facts.has_contract && !facts.effects);
        status = xr_compile_session_resource_status(session);
    }
    if (session && status != XR_COMPILE_RESOURCE_OK) {
        CHECK(xr_compile_session_resource_status(session) == status);
        XrCompileResourceStats failed = stats(resources);size_t attempts = attempt_count;
        XrNativeSymbol sentinel = {0};const XrNativeSymbol *output = &sentinel;
        CHECK(xa_analyzer_find_native_symbol(&analyzer, "invoke", &output) != XR_MANIFEST_OK);
        CHECK(output == &sentinel);
        CHECK(xa_analyzer_find_native_symbol(&analyzer, NULL, &output) != XR_MANIFEST_OK);
        CHECK(output == &sentinel && xa_analyzer_bind_native_plan(&analyzer, plan) == status);
        XaSymbol invoke = {0};invoke.name = "invoke";
        XaNativeEffectAxioms facts = xa_native_effect_axioms(&analyzer, &invoke);
        CHECK(!facts.has_contract && !facts.effects && !facts.allocates && !facts.suspends);
        XrCompileResourceStats retry = stats(resources);
        CHECK(attempt_count == attempts && retry.work == failed.work &&
            retry.allocated_bytes == failed.allocated_bytes);
    }
    *observed = stats(resources);
    xr_native_package_plan_free_owned(plan);xr_compile_session_free(session);
    XrCompileResourceStats after = stats(resources);
    CHECK(after.work == observed->work && after.allocated_bytes == observed->allocated_bytes);
    xr_compile_resources_release(resources);CHECK(!physical_live && !allocation_count);
    if (status == XR_COMPILE_RESOURCE_OK)
        CHECK(snapshot.has_contract && snapshot.allocates && snapshot.suspends);
    return status;
}

static void native_input_resources(void) {
    reset();XrCompileResourceStats baseline = {0};
    CHECK(native_input_pipeline(&generous, &baseline) == XR_COMPILE_RESOURCE_OK);
    size_t sites = attempt_count;CHECK(sites > 0);
    for (size_t ordinal = 0; ordinal < sites; ++ordinal) {
        reset();fail_at = ordinal;XrCompileResourceStats observed = {0};
        CHECK(native_input_pipeline(&generous, &observed) == XR_COMPILE_RESOURCE_OUT_OF_MEMORY);
    }
    for (unsigned axis = 0; axis < 3; ++axis) for (int delta = -1; delta <= 1; ++delta) {
        reset();XrCompileResourceStats observed = {0};
        XrCompileResourceLimits limits = {baseline.allocated_bytes, baseline.peak_bytes, baseline.work};
        uint64_t *bound = axis == 0 ? &limits.allocated_bytes : axis == 1 ? &limits.live_bytes : &limits.work;
        CHECK(*bound > 1);*bound = (uint64_t)((int64_t)*bound + delta);
        CHECK(native_input_pipeline(&limits, &observed) ==
            (delta < 0 ? XR_COMPILE_RESOURCE_BUDGET : XR_COMPILE_RESOURCE_OK));
    }
    printf("native input: %zu allocation faults, owned manifest after DOM death, actual pure/may effects, "
        "three axes, sticky output/no-charge retry and physical zero passed\n", sites);
}

static void native_input_binding_guards(void) {
    reset();XrCompileResources *owner = NULL, *foreign = NULL;XrCompilerSession *session = NULL;
    XrNativePackagePlan *plan = NULL, *other = NULL;
    CHECK(xr_compile_resources_new(&generous, &owner) == XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_new(&generous, &foreign) == XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_session_new(owner, &session) == XR_COMPILER_SESSION_OK);
    CHECK(native_plan_new(owner, &plan) == XR_COMPILE_RESOURCE_OK);
    CHECK(native_plan_new(foreign, &other) == XR_COMPILE_RESOURCE_OK);
    XaAnalyzer analyzer = {0};analyzer.compiler_session = session;analyzer.semantic_revision = 1;
    XrCompileResourceStats before = {0}, after = {0};size_t attempts = attempt_count;
    CHECK(xr_compile_resources_stats(owner, &before) == XR_COMPILE_RESOURCE_OK);
    CHECK(xa_analyzer_bind_native_plan(&analyzer, other) == XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    CHECK(!analyzer.native_plan_bound && !analyzer.native_plan);
    analyzer.semantic_revision = 2;
    CHECK(xa_analyzer_bind_native_plan(&analyzer, plan) == XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    CHECK(!analyzer.native_plan_bound && !analyzer.native_plan);
    analyzer.semantic_revision = 1;
    CHECK(xa_analyzer_bind_native_plan(&analyzer, plan) == XR_COMPILE_RESOURCE_OK);
    CHECK(xa_analyzer_bind_native_plan(&analyzer, plan) == XR_COMPILE_RESOURCE_OK);
    CHECK(xa_analyzer_bind_native_plan(&analyzer, NULL) == XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    CHECK(xa_analyzer_bind_native_plan(&analyzer, other) == XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    CHECK(analyzer.native_plan_bound && analyzer.native_plan == plan);
    CHECK(xr_compile_resources_stats(owner, &after) == XR_COMPILE_RESOURCE_OK);
    CHECK(attempt_count == attempts && before.work == after.work &&
        before.allocated_bytes == after.allocated_bytes && before.live_bytes == after.live_bytes);
    XaAnalyzer absent = {0};absent.compiler_session = session;absent.semantic_revision = 1;
    CHECK(xa_analyzer_bind_native_plan(&absent, NULL) == XR_COMPILE_RESOURCE_OK);
    CHECK(xa_analyzer_bind_native_plan(&absent, plan) == XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    XaSymbol invoke = {0};invoke.name = "invoke";
    XaNativeEffectAxioms facts = xa_native_effect_axioms(&absent, &invoke);
    CHECK(!facts.has_contract && !facts.effects);
    XrManifestDiagnostic diagnostic = {{'S', '\0'}};
    CHECK(xa_analyzer_validate_native_arity(&absent, "invoke", 0, &diagnostic) == XR_MANIFEST_OK);
    CHECK(diagnostic.message[0] == 'S');
    CHECK(xa_analyzer_note_native_layout_subject(&absent, "Point") == XR_MANIFEST_OK);
    bool matched = true;XrAggregateLayout empty = {0};
    CHECK(xa_analyzer_resolve_native_layout(&absent, "Point", &empty, &matched) == XR_MANIFEST_OK);
    CHECK(!matched);
    XrNativeSymbol sentinel = {0};const XrNativeSymbol *output = &sentinel;
    CHECK(xa_analyzer_find_native_symbol(&analyzer, NULL, &output) == XR_MANIFEST_OK && !output);
    output = &sentinel;
    CHECK(xa_analyzer_find_native_symbol(&analyzer, "missing", &output) == XR_MANIFEST_OK && !output);
    xr_native_package_plan_free_owned(plan);xr_native_package_plan_free_owned(other);
    xr_compile_session_free(session);xr_compile_resources_release(owner);xr_compile_resources_release(foreign);
    CHECK(!physical_live && !allocation_count);
}

static XrManifestStatus native_layout_operation(XaAnalyzer *analyzer, XrNativePackagePlan *plan,
    unsigned operation, const XrAggregateLayout *layout, bool *matched) {
    XrNativePackagePlan before = *plan;
    CHECK(plan->layout_count == 1);
    XrNativeLayoutAssertion previous = plan->layouts[0];
    XrCompileResources *resources = xr_compile_session_resources(analyzer->compiler_session);
    XrCompileResourceStats held = stats(resources);bool prior_match = *matched;
    XrManifestStatus status = operation == 0 || operation == 4
        ? xa_analyzer_note_native_layout_subject(analyzer, "Point")
        : xa_analyzer_resolve_native_layout(analyzer, operation == 1 ? "Other" : "Point", layout, matched);
    if (status != XR_MANIFEST_OK) {
        CHECK(!memcmp(plan, &before, sizeof(before)));
        CHECK(!memcmp(&plan->layouts[0], &previous, sizeof(previous)));
        CHECK(stats(resources).live_bytes == held.live_bytes && *matched == prior_match);
    }
    return status;
}

static XrCompileResourceStatus native_declaration_pipeline(const XrCompileResourceLimits *limits,
    XrCompileResourceStats *observed, uint64_t *work_stages) {
    static const char layout_manifest[] =
        "[[native.layout]]\nxray_type='Point'\nc_type='XrAnalyzerNativePoint'\n"
        "header='native_point.h'\nassert={size=true,align=true,fields=true}\n";
    char input[sizeof(native_manifest) + sizeof(layout_manifest) - 1];
    memcpy(input, native_manifest, sizeof(native_manifest) - 1);
    memcpy(input + sizeof(native_manifest) - 1, layout_manifest, sizeof(layout_manifest));
    XrCompileResources *resources = NULL;XrCompilerSession *session = NULL;
    XrNativePackagePlan *plan = NULL;
    XrCompileResourceStatus status = xr_compile_resources_new(limits, &resources);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    status = session_result(xr_compile_session_new(resources, &session));
    XaAnalyzer analyzer = {0};analyzer.compiler_session = session;analyzer.semantic_revision = 1;
    if (status == XR_COMPILE_RESOURCE_OK)
        status = native_plan_parse(resources, input, sizeof(input) - 1, &plan);
    if (session && status != XR_COMPILE_RESOURCE_OK)
        (void) xr_compile_state_fail(xr_compile_session_compile_state(session), status);
    if (status == XR_COMPILE_RESOURCE_OK) {
        CHECK(xa_analyzer_bind_native_plan(&analyzer, plan) == XR_COMPILE_RESOURCE_OK);
        if (work_stages) work_stages[0] = stats(resources).work;
        for (unsigned mode = 0; mode < 3 && status == XR_COMPILE_RESOURCE_OK; ++mode) {
            XrManifestDiagnostic diagnostic = {{'S', '\0'}};
            XrManifestStatus result = xa_analyzer_validate_native_arity(&analyzer,
                mode == 2 ? "missing" : "invoke", mode == 1 ? 1 : 0, &diagnostic);
            status = xr_compile_session_resource_status(session);
            if (status != XR_COMPILE_RESOURCE_OK) {
                CHECK(result != XR_MANIFEST_OK && result != XR_MANIFEST_INVALID);
                CHECK(diagnostic.message[0] == 'S');
            } else if (mode == 0) CHECK(result == XR_MANIFEST_OK && diagnostic.message[0] == 'S');
            else {
                CHECK(result == XR_MANIFEST_INVALID);
                /* A prior semantic rejection survives optional rendering
                 * exhaustion; a complete diagnostic is still atomic. */
                if (diagnostic.message[0] == 'S') CHECK(limits->work < generous.work);
                else CHECK(strstr(diagnostic.message,
                    mode == 1 ? "arity 1" : "E-NATIVE-UNDECLARED-SYMBOL"));
            }
            if (work_stages) work_stages[mode + 1] = stats(resources).work;
        }
        char left[] = "left", right[] = "right";const char *names[] = {left, right};
        XrAggregateLayout layout = {0};
        layout.field_count = 2;layout.field_names = names;
        layout.total_size = (uint16_t) sizeof(struct XrAnalyzerNativePoint);
        layout.alignment = _Alignof(struct XrAnalyzerNativePoint);
        layout.fields[0].offset = (uint16_t) offsetof(struct XrAnalyzerNativePoint, left);
        layout.fields[1].offset = (uint16_t) offsetof(struct XrAnalyzerNativePoint, right);
        for (unsigned operation = 0; operation < 5 && status == XR_COMPILE_RESOURCE_OK; ++operation) {
            bool matched = true;
            XrManifestStatus result = native_layout_operation(&analyzer, plan, operation, &layout, &matched);
            status = xr_compile_session_resource_status(session);
            CHECK((result == XR_MANIFEST_OK) == (status == XR_COMPILE_RESOURCE_OK));
            if (status != XR_COMPILE_RESOURCE_OK) break;
            const XrNativeLayoutAssertion *facts = &plan->layouts[0];
            CHECK(facts->declared);
            if (operation == 1) CHECK(!matched && !facts->resolved);
            if (operation >= 2) {
                CHECK(facts->resolved && facts->field_count == 2 && matched);
                CHECK(facts->expected_size == sizeof(struct XrAnalyzerNativePoint));
                CHECK(facts->expected_align == _Alignof(struct XrAnalyzerNativePoint));
                CHECK(facts->field_offsets[0] == offsetof(struct XrAnalyzerNativePoint, left));
                CHECK(facts->field_offsets[1] == offsetof(struct XrAnalyzerNativePoint, right));
                CHECK(!strcmp(facts->field_names[0], operation == 2 ? "left" : "Left"));
                if (operation == 2) { left[0] = 'L';CHECK(!strcmp(facts->field_names[0], "left")); }
            }
            if (work_stages) work_stages[operation + 4] = stats(resources).work;
        }
    }
    if (session && status != XR_COMPILE_RESOURCE_OK) {
        XrCompileResourceStats failed = stats(resources);size_t attempts = attempt_count;
        bool matched = true;XrAggregateLayout layout = {0};
        XrManifestDiagnostic diagnostic = {{'S', '\0'}};
        CHECK(xa_analyzer_validate_native_arity(&analyzer, "invoke", 0, &diagnostic) != XR_MANIFEST_OK);
        CHECK(diagnostic.message[0] == 'S');
        CHECK(xa_analyzer_note_native_layout_subject(&analyzer, "Point") != XR_MANIFEST_OK);
        CHECK(xa_analyzer_resolve_native_layout(&analyzer, "Point", &layout, &matched) != XR_MANIFEST_OK);
        CHECK(matched);
        CHECK(xa_analyzer_resolve_native_layout(&analyzer, NULL, NULL, NULL) != XR_MANIFEST_OK);
        XrCompileResourceStats retry = stats(resources);
        CHECK(attempt_count == attempts && retry.work == failed.work &&
            retry.allocated_bytes == failed.allocated_bytes);
    }
    *observed = stats(resources);
    xr_native_package_plan_free_owned(plan);xr_compile_session_free(session);
    XrCompileResourceStats after = stats(resources);
    CHECK(after.work == observed->work && after.allocated_bytes == observed->allocated_bytes);
    xr_compile_resources_release(resources);CHECK(!physical_live && !allocation_count);
    return status;
}

static void native_declaration_resources(void) {
    reset();XrCompileResourceStats baseline = {0};uint64_t stages[9] = {0};
    CHECK(native_declaration_pipeline(&generous, &baseline, stages) == XR_COMPILE_RESOURCE_OK);
    size_t sites = attempt_count;CHECK(sites > 0);
    for (size_t ordinal = 0; ordinal < sites; ++ordinal) {
        reset();fail_at = ordinal;XrCompileResourceStats observed = {0};
        CHECK(native_declaration_pipeline(&generous, &observed, NULL) == XR_COMPILE_RESOURCE_OUT_OF_MEMORY);
    }
    for (unsigned axis = 0; axis < 3; ++axis) for (int delta = -1; delta <= 1; ++delta) {
        reset();XrCompileResourceStats observed = {0};
        XrCompileResourceLimits limits = {baseline.allocated_bytes, baseline.peak_bytes, baseline.work};
        uint64_t *bound = axis == 0 ? &limits.allocated_bytes : axis == 1 ? &limits.live_bytes : &limits.work;
        CHECK(*bound > 1);*bound = (uint64_t)((int64_t)*bound + delta);
        CHECK(native_declaration_pipeline(&limits, &observed, NULL) ==
            (delta < 0 ? XR_COMPILE_RESOURCE_BUDGET : XR_COMPILE_RESOURCE_OK));
    }
    for (unsigned stage = 0; stage < 8; ++stage) {
        CHECK(stages[stage + 1] > stages[stage] + 2);
        uint64_t cuts[] = {stages[stage] + 1,
            stages[stage] + (stages[stage + 1] - stages[stage]) / 2, stages[stage + 1] - 1};
        for (unsigned cut = 0; cut < 3; ++cut) {
            reset();XrCompileResourceStats observed = {0};XrCompileResourceLimits limits = generous;
            limits.work = cuts[cut];
            CHECK(native_declaration_pipeline(&limits, &observed, NULL) == XR_COMPILE_RESOURCE_BUDGET);
        }
    }
    printf("native declarations: %zu allocation faults, precise healthy arity rejection, actual C size/align/offsets, "
        "owned names, transactional rollback, three axes and 24 operation work cuts, "
        "sticky no-charge retry and physical zero passed\n", sites);
}

static XrCompileResourceStatus ast_scope_pipeline(const XrCompileResourceLimits *limits,
    XrCompileResourceStats *observed, uint64_t *stages) {
    XrCompileResources *resources = NULL;XrCompilerSession *session = NULL;
    AstNode *programs[2] = {0};XrTypeRef *materialized = NULL;
    XrCompilerSessionScope outer = {0}, inner = {0};
    XaAnalyzer analyzer = {0};
    XrCompileResourceStatus status = xr_compile_resources_new(limits, &resources);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    status = session_result(xr_compile_session_new(resources, &session));
    analyzer.compiler_session = session;
    for (unsigned i = 0; i < 2 && status == XR_COMPILE_RESOURCE_OK; ++i) {
        XrParseStatus parsed = xr_compile_parse(session,
            i ? "const inner = 42\n" : "const outer = 1\n", &programs[i]);
        status = xr_compile_session_resource_status(session);
        if (status == XR_COMPILE_RESOURCE_OK) CHECK(parsed == XR_PARSE_OK && programs[i]);
        else CHECK(!programs[i] && (parsed == XR_PARSE_BUDGET || parsed == XR_PARSE_OUT_OF_MEMORY));
    }
    if (status == XR_COMPILE_RESOURCE_OK) {
        CHECK(!xr_compile_session_current_arena(session) && !xr_compile_session_string_pool(session));
        if (stages) stages[0] = stats(resources).work;
        status = xa_analyzer_ast_scope_open(&analyzer, programs[0], &outer);
        if (status == XR_COMPILE_RESOURCE_OK) {
            CHECK(outer.active && xr_compile_session_current_arena(session) == programs[0]->as.program.arena);
            struct XrCompileStringPool *outer_pool = xr_compile_session_string_pool(session);
            CHECK(outer_pool);
            if (stages) stages[1] = stats(resources).work;
            status = xa_analyzer_ast_scope_open(&analyzer, programs[1], &inner);
            if (status == XR_COMPILE_RESOURCE_OK) {
                CHECK(inner.active && xr_compile_session_current_arena(session) == programs[1]->as.program.arena);
                CHECK(xr_compile_session_string_pool(session) != outer_pool);
                if (stages) stages[2] = stats(resources).work;
                materialized = xr_tref_named(session, "materialized");
                status = xr_compile_session_resource_status(session);
                if (status == XR_COMPILE_RESOURCE_OK) {
                    CHECK(materialized && materialized->kind == XR_TREF_NAMED);
                    CHECK(!strcmp(materialized->name, "materialized"));
                    if (stages) stages[3] = stats(resources).work;
                }
            } else CHECK(!inner.active && !inner.session && !inner.token);
            if (inner.active) CHECK(xr_compile_session_pop_arena(&inner) == status);
            CHECK(xr_compile_session_current_arena(session) == programs[0]->as.program.arena);
            CHECK(xr_compile_session_string_pool(session) == outer_pool);
            if (status == XR_COMPILE_RESOURCE_OK) {
                XrCompilerSessionScope inherited = {0};
                CHECK(xa_analyzer_ast_scope_open(&analyzer, programs[0]->as.program.statements[0],
                    &inherited) == XR_COMPILE_RESOURCE_OK && !inherited.active);
                CHECK(xr_compile_session_current_arena(session) == programs[0]->as.program.arena);
            }
        } else CHECK(!outer.active && !outer.session && !outer.token);
    }
    if (session && status != XR_COMPILE_RESOURCE_OK) {
        XrCompileResourceStats failed = stats(resources);size_t attempts = attempt_count;
        XrArena *selected = xr_compile_session_current_arena(session);
        struct XrCompileStringPool *pool = xr_compile_session_string_pool(session);
        XrCompilerSessionScope untouched = {0};
        CHECK(xa_analyzer_ast_scope_open(&analyzer, (const AstNode *)(uintptr_t) 1, &untouched) == status);
        CHECK(!untouched.active && !untouched.session && !untouched.token);
        CHECK(xr_compile_session_current_arena(session) == selected && xr_compile_session_string_pool(session) == pool);
        XrCompileResourceStats retry = stats(resources);
        CHECK(attempt_count == attempts && retry.work == failed.work && retry.allocated_bytes == failed.allocated_bytes);
    }
    if (outer.active) CHECK(xr_compile_session_pop_arena(&outer) == status);
    CHECK(!outer.active && !inner.active && !xr_compile_session_current_arena(session));
    CHECK(!xr_compile_session_string_pool(session));
    *observed = stats(resources);xr_compile_session_free(session);
    if (status == XR_COMPILE_RESOURCE_OK) CHECK(!strcmp(materialized->name, "materialized"));
    xr_program_destroy(programs[1]);xr_program_destroy(programs[0]);
    XrCompileResourceStats after = stats(resources);
    CHECK(after.work == observed->work && after.allocated_bytes == observed->allocated_bytes);
    xr_compile_resources_release(resources);CHECK(!physical_live && !allocation_count);
    return status;
}

static void ast_scope_resources(void) {
    reset();XrCompileResourceStats baseline = {0};uint64_t stages[4] = {0};
    CHECK(ast_scope_pipeline(&generous, &baseline, stages) == XR_COMPILE_RESOURCE_OK);
    size_t sites = attempt_count;CHECK(sites > 0);
    for (size_t ordinal = 0; ordinal < sites; ++ordinal) {
        reset();fail_at = ordinal;XrCompileResourceStats observed = {0};
        CHECK(ast_scope_pipeline(&generous, &observed, NULL) == XR_COMPILE_RESOURCE_OUT_OF_MEMORY);
    }
    for (unsigned axis = 0; axis < 3; ++axis) for (int delta = -1; delta <= 1; ++delta) {
        reset();XrCompileResourceStats observed = {0};
        XrCompileResourceLimits limits = {baseline.allocated_bytes, baseline.peak_bytes, baseline.work};
        uint64_t *bound = axis == 0 ? &limits.allocated_bytes : axis == 1 ? &limits.live_bytes : &limits.work;
        CHECK(*bound > 1);*bound = (uint64_t)((int64_t)*bound + delta);
        CHECK(ast_scope_pipeline(&limits, &observed, NULL) ==
            (delta < 0 ? XR_COMPILE_RESOURCE_BUDGET : XR_COMPILE_RESOURCE_OK));
    }
    for (unsigned stage = 0; stage < 3; ++stage) {
        CHECK(stages[stage + 1] > stages[stage] + 2);
        uint64_t cuts[] = {stages[stage] + 1,
            stages[stage] + (stages[stage + 1] - stages[stage]) / 2, stages[stage + 1] - 1};
        for (unsigned cut = 0; cut < 3; ++cut) {
            reset();XrCompileResourceStats observed = {0};XrCompileResourceLimits limits = generous;
            limits.work = cuts[cut];
            CHECK(ast_scope_pipeline(&limits, &observed, NULL) == XR_COMPILE_RESOURCE_BUDGET);
        }
    }
    printf("analyzer AST scope: %zu allocation faults, real nested parsed arenas/pools, owned type-ref after Session death, "
        "three axes and 9 operation work cuts, sticky no-charge retry and physical zero passed\n", sites);
}

static void ast_scope_rejected_input(void) {
    for (unsigned mode = 0; mode < 2; ++mode) {
        reset();XrCompileResources *resources = NULL;XrCompilerSession *owner = NULL, *foreign = NULL;
        AstNode *programs[2] = {0};
        CHECK(xr_compile_resources_new(&generous, &resources) == XR_COMPILE_RESOURCE_OK);
        CHECK(xr_compile_session_new(resources, &owner) == XR_COMPILER_SESSION_OK);
        CHECK(xr_compile_session_new(resources, &foreign) == XR_COMPILER_SESSION_OK);
        CHECK(xr_compile_parse(owner, "const outer = 1", &programs[0]) == XR_PARSE_OK);
        CHECK(xr_compile_parse(foreign, "const other = 2", &programs[1]) == XR_PARSE_OK);
        XaAnalyzer analyzer = {0};analyzer.compiler_session = owner;
        XrCompilerSessionScope outer = {0}, rejected = {0};
        CHECK(xa_analyzer_ast_scope_open(&analyzer, programs[0], &outer) == XR_COMPILE_RESOURCE_OK);
        XrCompilerSessionScope before = outer;
        XrCompileResourceStats held = stats(resources);size_t attempts = attempt_count;
        struct XrCompileStringPool *pool = xr_compile_session_string_pool(owner);
        CHECK(xa_analyzer_ast_scope_open(&analyzer, programs[1], mode ? &outer : &rejected) ==
            XR_COMPILE_RESOURCE_BAD_ARGUMENT);
        CHECK(!memcmp(&before, &outer, sizeof(outer)) && !rejected.active && !rejected.session);
        CHECK(xr_compile_session_current_arena(owner) == programs[0]->as.program.arena);
        CHECK(xr_compile_session_string_pool(owner) == pool);
        CHECK(xr_compile_session_resource_status(foreign) == XR_COMPILE_RESOURCE_OK);
        XrCompileResourceStats after = stats(resources);
        CHECK(attempt_count == attempts && after.work == held.work &&
            after.allocated_bytes == held.allocated_bytes && after.live_bytes == held.live_bytes);
        CHECK(xr_compile_session_pop_arena(&outer) == XR_COMPILE_RESOURCE_BAD_ARGUMENT);
        CHECK(!outer.active && !xr_compile_session_current_arena(owner) && !xr_compile_session_string_pool(owner));
        xr_compile_session_free(owner);xr_compile_session_free(foreign);
        xr_program_destroy(programs[1]);xr_program_destroy(programs[0]);
        xr_compile_resources_release(resources);CHECK(!physical_live && !allocation_count);
    }
}

typedef struct VTableFixture {
    XaSymbol methods[5];
    char names[5][8];
    XaSymbol *root_methods[2], *child_methods[2], *leaf_methods[1];
    int32_t root_ids[2], child_ids[2], leaf_ids[1];
    XrClassInfo classes[3];
    XaVTableClassInput inputs[3];
} VTableFixture;

static void vtable_fixture_open(VTableFixture *fixture) {
    memset(fixture, 0, sizeof(*fixture));
    const char *names[] = {"alpha", "beta", "alpha", "gamma", "beta"};
    for (unsigned i = 0; i < 5; ++i) {
        memcpy(fixture->names[i], names[i], strlen(names[i]) + 1);
        fixture->methods[i].name = fixture->names[i];
    }
    fixture->root_methods[0] = &fixture->methods[0];fixture->root_methods[1] = &fixture->methods[1];
    fixture->child_methods[0] = &fixture->methods[2];fixture->child_methods[1] = &fixture->methods[3];
    fixture->leaf_methods[0] = &fixture->methods[4];
    fixture->root_ids[0] = 11;fixture->root_ids[1] = 22;
    fixture->child_ids[0] = 11;fixture->child_ids[1] = 33;fixture->leaf_ids[0] = 22;
    fixture->classes[0].methods = fixture->root_methods;fixture->classes[0].method_count = 2;
    fixture->classes[1].base = &fixture->classes[0];fixture->classes[1].methods = fixture->child_methods;
    fixture->classes[1].method_count = 2;fixture->classes[2].base = &fixture->classes[1];
    fixture->classes[2].methods = fixture->leaf_methods;fixture->classes[2].method_count = 1;
    fixture->inputs[0] = (XaVTableClassInput){&fixture->classes[0], fixture->root_ids};
    fixture->inputs[1] = (XaVTableClassInput){&fixture->classes[1], fixture->child_ids};
    fixture->inputs[2] = (XaVTableClassInput){&fixture->classes[2], fixture->leaf_ids};
}

static void vtable_fixture_check(const VTableFixture *fixture) {
    const int counts[] = {2, 3, 3};
    const int32_t ids[] = {11, 22, 33};const char *names[] = {"alpha", "beta", "gamma"};
    const unsigned targets[3][3] = {{0, 1, 0}, {2, 1, 3}, {2, 4, 3}};
    const bool overridden[3][3] = {{true, true, false}, {false, true, false}, {false, false, false}};
    for (unsigned owner = 0; owner < 3; ++owner) {
        CHECK(fixture->classes[owner].vtable_size == counts[owner]);
        for (int i = 0; i < counts[owner]; ++i) {
            const XaMethodSlot *slot = &fixture->classes[owner].vtable[i];
            CHECK(slot->vtable_index == i && slot->symbol_id == ids[i]);
            CHECK(!strcmp(slot->name, names[i]));
            CHECK(slot->symbol == &fixture->methods[targets[owner][i]]);
            CHECK(slot->is_overridden == overridden[owner][i] && slot->is_final != overridden[owner][i]);
        }
    }
}

static XrCompileResourceStatus vtable_pipeline(const XrCompileResourceLimits *limits,
    XrCompileResourceStats *observed, bool existing_base) {
    XrCompileResources *resources = NULL;XrCompilerSession *session = NULL;
    VTableFixture fixture;vtable_fixture_open(&fixture);
    XrCompileResourceStatus status = xr_compile_resources_new(limits, &resources);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    status = session_result(xr_compile_session_new(resources, &session));
    XrCompileState *state = xr_compile_session_compile_state(session);
    if (status == XR_COMPILE_RESOURCE_OK && existing_base)
        status = xa_class_vtable_build_owned(state, fixture.inputs, 1);
    if (status == XR_COMPILE_RESOURCE_OK) {
        XrClassInfo before[3];memcpy(before, fixture.classes, sizeof(before));
        XaMethodSlot parent_slots[2] = {0};
        if (existing_base) memcpy(parent_slots, fixture.classes[0].vtable, sizeof(parent_slots));
        XrCompileResourceStats held = stats(resources);
        status = xa_class_vtable_build_owned(state, fixture.inputs + (existing_base ? 1 : 0), existing_base ? 2 : 3);
        if (status == XR_COMPILE_RESOURCE_OK) {
            vtable_fixture_check(&fixture);
            for (unsigned i = 0; i < 5; ++i) fixture.names[i][0] = '!';
            vtable_fixture_check(&fixture);
        } else {
            CHECK(!memcmp(before, fixture.classes, sizeof(before)));
            if (existing_base) CHECK(!memcmp(parent_slots, fixture.classes[0].vtable, sizeof(parent_slots)));
            CHECK(stats(resources).live_bytes == held.live_bytes);
        }
    }
    if (session && status != XR_COMPILE_RESOURCE_OK) {
        XrCompileResourceStats failed = stats(resources);size_t attempts = attempt_count;
        CHECK(xa_class_vtable_build_owned(state, (const XaVTableClassInput *)(uintptr_t) 1, SIZE_MAX) == status);
        XrCompileResourceStats retry = stats(resources);
        CHECK(attempt_count == attempts && retry.work == failed.work && retry.allocated_bytes == failed.allocated_bytes);
    }
    *observed = stats(resources);xr_compile_session_free(session);
    if (status == XR_COMPILE_RESOURCE_OK) vtable_fixture_check(&fixture);
    for (unsigned i = 3; i > 0; --i) {
        xa_class_vtable_free_owned(&fixture.classes[i - 1]);
        xa_class_vtable_free_owned(&fixture.classes[i - 1]);
    }
    XrCompileResourceStats after = stats(resources);
    CHECK(after.work == observed->work && after.allocated_bytes == observed->allocated_bytes);
    xr_compile_resources_release(resources);CHECK(!physical_live && !allocation_count);
    return status;
}

static void vtable_resources(void) {
    for (unsigned prefix = 0; prefix < 2; ++prefix) {
        reset();XrCompileResourceStats baseline = {0};vtable_work_count = 0;record_vtable_work = true;
        CHECK(vtable_pipeline(&generous, &baseline, prefix != 0) == XR_COMPILE_RESOURCE_OK);
        record_vtable_work = false;size_t sites = attempt_count, cuts = vtable_work_count;
        CHECK(sites > 0 && cuts > 0);
        for (size_t ordinal = 0; ordinal < sites; ++ordinal) {
            reset();fail_at = ordinal;XrCompileResourceStats observed = {0};
            CHECK(vtable_pipeline(&generous, &observed, prefix != 0) == XR_COMPILE_RESOURCE_OUT_OF_MEMORY);
        }
        for (unsigned axis = 0; axis < 3; ++axis) for (int delta = -1; delta <= 1; ++delta) {
            reset();XrCompileResourceStats observed = {0};
            XrCompileResourceLimits limits = {baseline.allocated_bytes, baseline.peak_bytes, baseline.work};
            uint64_t *bound = axis == 0 ? &limits.allocated_bytes : axis == 1 ? &limits.live_bytes : &limits.work;
            CHECK(*bound > 1);*bound = (uint64_t)((int64_t)*bound + delta);
            CHECK(vtable_pipeline(&limits, &observed, prefix != 0) ==
                (delta < 0 ? XR_COMPILE_RESOURCE_BUDGET : XR_COMPILE_RESOURCE_OK));
        }
        for (size_t cut = 0; cut < cuts; ++cut) {
            reset();XrCompileResourceStats observed = {0};XrCompileResourceLimits limits = generous;
            CHECK(vtable_work_bounds[cut]);limits.work = vtable_work_bounds[cut] - 1;
            CHECK(vtable_pipeline(&limits, &observed, prefix != 0) == XR_COMPILE_RESOURCE_BUDGET);
        }
        printf("owned vtable prefix%u: %zu allocation faults, %zu actual work cutoffs, "
            "three-level independent slots/transitive flags, atomic old-parent preservation, "
            "owned names, three axes, sticky retry/producer death/physical zero passed\n", prefix, sites, cuts);
    }
}

static void vtable_rejected_input(void) {
    for (unsigned mode = 0; mode < 6; ++mode) {
        reset();XrCompileResources *resources = NULL;XrCompilerSession *session = NULL;
        VTableFixture fixture;vtable_fixture_open(&fixture);
        CHECK(xr_compile_resources_new(&generous, &resources) == XR_COMPILE_RESOURCE_OK);
        CHECK(xr_compile_session_new(resources, &session) == XR_COMPILER_SESSION_OK);
        XrCompileState *state = xr_compile_session_compile_state(session);
        CHECK(xa_class_vtable_build_owned(state, NULL, 0) == XR_COMPILE_RESOURCE_OK);
        if (mode == 0 || mode == 4)
            CHECK(xa_class_vtable_build_owned(state, fixture.inputs, 1) == XR_COMPILE_RESOURCE_OK);
        if (mode == 1) fixture.classes[1].base = &fixture.classes[1];
        if (mode == 2) fixture.child_ids[0] = 0;
        if (mode == 3) fixture.methods[2].name = "wrong";
        if (mode == 4) fixture.classes[0].base = &fixture.classes[0];
        XrClassInfo before[3];memcpy(before, fixture.classes, sizeof(before));
        XaMethodSlot parent_slots[2] = {0};
        if (fixture.classes[0].vtable) memcpy(parent_slots, fixture.classes[0].vtable, sizeof(parent_slots));
        XrCompileResourceStats held = stats(resources);
        XrCompileResourceStatus expected = mode == 5 ? XR_COMPILE_RESOURCE_BUDGET : XR_COMPILE_RESOURCE_BAD_ARGUMENT;
        CHECK(xa_class_vtable_build_owned(state, mode == 5 ? (const XaVTableClassInput *)(uintptr_t) 1 :
            fixture.inputs + (mode == 4 ? 1 : 0), mode == 5 ? SIZE_MAX : mode == 4 ? 2 : 3) == expected);
        CHECK(!memcmp(before, fixture.classes, sizeof(before)));
        if (fixture.classes[0].vtable) CHECK(!memcmp(parent_slots, fixture.classes[0].vtable, sizeof(parent_slots)));
        CHECK(stats(resources).live_bytes == held.live_bytes);
        for (unsigned i = 0; i < 3; ++i) xa_class_vtable_free_owned(&fixture.classes[i]);
        xr_compile_session_free(session);xr_compile_resources_release(resources);
        CHECK(!physical_live && !allocation_count);
    }
}

static void vtable_ledger_producer_death(void) {
    reset();XrCompileResources *resources = NULL;XrCompilerSession *session = NULL;
    VTableFixture fixture;vtable_fixture_open(&fixture);
    CHECK(xr_compile_resources_new(&generous, &resources) == XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_session_new(resources, &session) == XR_COMPILER_SESSION_OK);
    CHECK(xa_class_vtable_build_owned(xr_compile_session_compile_state(session), fixture.inputs, 3) ==
        XR_COMPILE_RESOURCE_OK);
    xr_compile_session_free(session);xr_compile_resources_release(resources);
    CHECK(physical_live && allocation_count);
    for (unsigned i = 0; i < 5; ++i) fixture.names[i][0] = '!';
    vtable_fixture_check(&fixture);
    for (unsigned i = 0; i < 3; ++i) xa_class_vtable_free_owned(&fixture.classes[i]);
    CHECK(!physical_live && !allocation_count);
}

int main(void) {
    reset();
    XrCompileResourceStats baseline = {0};
    CHECK(pipeline(&generous, &baseline) == XR_COMPILE_RESOURCE_OK);
    size_t sites = attempt_count;
    CHECK(sites >= 9);
    for (size_t ordinal = 0; ordinal < sites; ++ordinal) {
        reset();
        fail_at = ordinal;
        XrCompileResourceStats observed = {0};
        CHECK(pipeline(&generous, &observed) == XR_COMPILE_RESOURCE_OUT_OF_MEMORY);
        CHECK(!physical_live && !allocation_count);
    }
    XrCompileResourceLimits exact = {baseline.allocated_bytes, baseline.peak_bytes, baseline.work};
    reset();
    XrCompileResourceStats observed = {0};
    CHECK(pipeline(&exact, &observed) == XR_COMPILE_RESOURCE_OK);
    for (unsigned axis = 0; axis < 3; ++axis) {
        XrCompileResourceLimits below = exact;
        if (axis == 0) --below.allocated_bytes;
        if (axis == 1) --below.live_bytes;
        if (axis == 2) --below.work;
        reset();
        CHECK(pipeline(&below, &observed) == XR_COMPILE_RESOURCE_BUDGET);
        CHECK(!physical_live && !allocation_count);
    }
    producer_death_and_shared_failure();
    occupied_output_and_foreign_state();
    export_query_resources();
    source_path_resources();
    native_input_resources();
    native_input_binding_guards();
    native_declaration_resources();
    ast_scope_resources();
    ast_scope_rejected_input();
    vtable_resources();
    vtable_rejected_input();
    vtable_ledger_producer_death();
    printf("analyzer storage: %zu allocator faults, three exact/minus-one axes, "
        "producer death, shared sticky failure and occupied output passed\n", sites);
    return 0;
}
