/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xaot_module_summary.c - Per-module summary publication for a native build
 */

#include "xaot_module_summary.h"

#include "../../include/xray_version.h"
#include "../base/xmalloc.h"
#include "../base/xsha256.h"
#include "../incremental/xr_cache_artifact_verify.h"
#include "../incremental/xr_module_summary_build.h"
#include "../ir/xi_module.h"
#include "../module/xmodule_graph.h"
#include "../plan/format/xr_xsm_schema.h"
#include "../plan/semantic/xr_semantic_plan.h"
#include "../plan/target/xr_target_profile.h"
#include "../toolchain/xcompiler_session.h"

#include <stdio.h>
#include <string.h>

typedef struct XaotModuleSummaryReport {
    uint32_t cache_hits;
    uint32_t cache_published;
    uint32_t cache_missed;
} XaotModuleSummaryReport;

/* One build's owned derivation state. The cache store and the facts shared by
 * every module are read-only for the duration; only the graph, the identity
 * vector, and the report accumulate. */
typedef struct XaotModuleSummaryBuild {
    XrDependencyGraph *graph;
    XrCacheStore *store;
    XiModule *const *modules;
    int module_count;
    XrStableId *ids;
    XrModuleSummaryFacts shared;
    bool rebuild;
    XaotModuleSummaryReport report;
} XaotModuleSummaryBuild;

static void hash_u32(XrSHA256Context *ctx, uint32_t value) {
    uint8_t bytes[4];
    for (unsigned i = 0; i < sizeof(bytes); i++)
        bytes[i] = (uint8_t) (value >> (i * 8));
    xr_sha256_update(ctx, bytes, sizeof(bytes));
}

static void hash_text(XrSHA256Context *ctx, const char *text) {
    size_t length = text ? strlen(text) : 0u;
    hash_u32(ctx, (uint32_t) length);
    if (length > 0)
        xr_sha256_update(ctx, (const uint8_t *) text, length);
}

/* Compiler identity: an artifact produced by a different compiler build or a
 * different plan schema must never be mistaken for this build's output. */
static void toolchain_fingerprint(XrFingerprint *out) {
    XrSHA256Context ctx;
    xr_sha256_init(&ctx);
    hash_text(&ctx, "xray-module-summary-toolchain-v1");
    hash_text(&ctx, XRAY_VERSION_STRING);
    hash_u32(&ctx, XR_SEMANTIC_SCHEMA_VERSION);
    xr_sha256_final(&ctx, out->bytes);
}

/* Build configuration selectors that the TargetProfile does not already own. */
static void configuration_fingerprint(const XaotBuildOptions *options, XrFingerprint *out) {
    XrSHA256Context ctx;
    xr_sha256_init(&ctx);
    hash_text(&ctx, "xray-module-summary-configuration-v1");
    hash_u32(&ctx, (uint32_t) options->profile);
    hash_u32(&ctx, (uint32_t) options->artifact_kind);
    hash_u32(&ctx, (uint32_t) options->c_dialect);
    hash_u32(&ctx, (uint32_t) options->type_name_profile);
    xr_sha256_final(&ctx, out->bytes);
}

/* Ordered dependency identity. The plan's own fingerprint already binds these
 * rows; the separate digest keeps the cache key's dependency component exact
 * instead of folding it into one opaque whole-plan value. */
static void dependency_fingerprint(const XrSemanticPlan *plan, XrFingerprint *out) {
    XrSHA256Context ctx;
    uint32_t count = (uint32_t) xr_semantic_plan_dependency_count(plan);
    xr_sha256_init(&ctx);
    hash_text(&ctx, "xray-module-summary-dependencies-v1");
    hash_u32(&ctx, count);
    for (uint32_t i = 0; i < count; i++) {
        const XrSemanticDependencyRecord *record = xr_semantic_plan_dependency(plan, i);
        if (!record) {
            hash_u32(&ctx, UINT32_C(0xFFFFFFFF));
            continue;
        }
        hash_text(&ctx, record->canonical_key);
        xr_sha256_update(&ctx, record->module.bytes, sizeof(record->module.bytes));
        xr_sha256_update(&ctx, record->semantic_fingerprint.bytes,
                         sizeof(record->semantic_fingerprint.bytes));
    }
    xr_sha256_final(&ctx, out->bytes);
}

/* Ordered exported declaration identity. Names and slots cannot describe an
 * export's type, so this feeds the cache key only; the summary's public facets
 * still carry the whole-plan fingerprint. */
static void declaration_fingerprint(const XrSemanticPlan *plan, XrFingerprint *out) {
    XrSHA256Context ctx;
    uint32_t count = (uint32_t) xr_semantic_plan_source_export_count(plan);
    xr_sha256_init(&ctx);
    hash_text(&ctx, "xray-module-summary-declarations-v1");
    hash_u32(&ctx, count);
    for (uint32_t i = 0; i < count; i++) {
        const XrSemanticSourceExportRecord *record = xr_semantic_plan_source_export(plan, i);
        if (!record) {
            hash_u32(&ctx, UINT32_C(0xFFFFFFFF));
            continue;
        }
        hash_text(&ctx, record->name);
        xr_sha256_update(&ctx, record->id.bytes, sizeof(record->id.bytes));
        hash_u32(&ctx, record->function);
        hash_u32(&ctx, record->shared_slot);
    }
    xr_sha256_final(&ctx, out->bytes);
}

static bool rejected_publish(XrCachePublishStatus status) {
    return status == XR_CACHE_PUBLISH_REJECTED || status == XR_CACHE_PUBLISH_CONFLICT ||
           status == XR_CACHE_PUBLISH_TOO_LARGE;
}

static const XrSemanticPlan *module_semantic_plan(const XiModule *module);

static const XrSemanticEntityRecord *semantic_module_entity(
    const XrSemanticPlan *plan) {
    const XrSemanticEntityRecord *match = NULL;
    size_t entity_count = xr_semantic_plan_entity_count(plan);
    for (size_t i = 0; i < entity_count; i++) {
        const XrSemanticEntityRecord *entity =
            xr_semantic_plan_entity(plan, (uint32_t) i);
        if (!entity || entity->kind != XR_SEM_ENTITY_MODULE)
            continue;
        if (match)
            return NULL;
        match = entity;
    }
    return match;
}

static bool collect_semantic_dependencies(
    const XaotModuleSummaryBuild *build, const XrSemanticPlan *plan,
    const XrSemanticPlan ***out_dependencies, uint32_t *out_count) {
    if (out_dependencies)
        *out_dependencies = NULL;
    if (out_count)
        *out_count = 0;
    if (!build || !plan || !out_dependencies || !out_count ||
        build->module_count < 0)
        return false;
    size_t dependency_size = xr_semantic_plan_dependency_count(plan);
    if (dependency_size > UINT32_MAX)
        return false;
    uint32_t dependency_count = (uint32_t) dependency_size;
    if (dependency_count == 0)
        return true;
    const XrSemanticPlan **dependencies =
        (const XrSemanticPlan **) xr_calloc(dependency_count,
                                             sizeof(*dependencies));
    if (!dependencies)
        return false;
    for (uint32_t row = 0; row < dependency_count; row++) {
        const XrSemanticDependencyRecord *dependency =
            xr_semantic_plan_dependency(plan, row);
        const XrSemanticPlan *match = NULL;
        for (int module = 0; dependency && module < build->module_count;
             module++) {
            const XrSemanticPlan *candidate =
                module_semantic_plan(build->modules[module]);
            const XrSemanticEntityRecord *identity =
                candidate ? semantic_module_entity(candidate) : NULL;
            if (!candidate || !identity ||
                !xr_stable_id_equal(identity->id, dependency->module) ||
                !xr_fingerprint_equal(
                    xr_semantic_plan_fingerprint(candidate),
                    dependency->semantic_fingerprint))
                continue;
            if (match) {
                xr_free(dependencies);
                return false;
            }
            match = candidate;
        }
        if (!match) {
            xr_free(dependencies);
            return false;
        }
        dependencies[row] = match;
    }
    *out_dependencies = dependencies;
    *out_count = dependency_count;
    return true;
}

/* Round-trips one module's XSM artifact. A hit skips re-encoding and
 * re-publishing an artifact already proven identical. No stage of this build is
 * skipped: the plan was produced and verified before the cache was consulted. */
static bool round_trip_semantic_artifact(XaotModuleSummaryBuild *build, XrCacheKey key,
                                         const XrSemanticPlan *plan) {
    if (!build->store)
        return true;

    const XrSemanticPlan **dependencies = NULL;
    uint32_t dependency_count = 0;
    if (!collect_semantic_dependencies(build, plan, &dependencies,
                                       &dependency_count)) {
        fprintf(stderr, "Error: module summary dependency authority is incomplete\n");
        return false;
    }
    XrCacheXsmArtifactVerifyContext requirements = {
        .expected_key = key,
        .semantic_plan = plan,
        .semantic_dependencies = dependencies,
        .semantic_dependency_count = dependency_count,
    };
    bool hit = false;
    if (!build->rebuild) {
        XrCacheBlob blob;
        memset(&blob, 0, sizeof(blob));
        XrCacheLoadStatus status = xr_cache_store_load(
            build->store, XR_CACHE_ARTIFACT_XSM, key,
            xr_cache_verify_xsm_artifact, &requirements, &blob);
        xr_cache_blob_release(&blob);
        hit = status == XR_CACHE_LOAD_HIT;
    }
    if (hit) {
        build->report.cache_hits++;
        xr_free(dependencies);
        return true;
    }
    build->report.cache_missed++;

    uint8_t *bytes = NULL;
    size_t size = 0;
    char error[256];
    if (!xr_xsm_encode(plan, &bytes, &size, error, sizeof(error))) {
        fprintf(stderr, "Error: cannot encode module summary artifact: %s\n", error);
        xr_free(dependencies);
        return false;
    }
    XrCachePublishStatus status = xr_cache_store_publish(
        build->store, XR_CACHE_ARTIFACT_XSM, key, bytes, size,
        xr_cache_verify_xsm_artifact, &requirements);
    xr_free(bytes);
    xr_free(dependencies);
    if (rejected_publish(status)) {
        fprintf(stderr, "Error: module summary artifact publication failed with status %d\n",
                (int) status);
        return false;
    }
    if (status == XR_CACHE_PUBLISH_OK)
        build->report.cache_published++;
    return true;
}

static const XrSemanticPlan *module_semantic_plan(const XiModule *module) {
    return module && module->init ? module->init->semantic_plan : NULL;
}

static bool add_module_node(XaotModuleSummaryBuild *build, int topo_index,
                            const XrModuleSpec *spec, const XrSemanticPlan *plan) {
    XrModuleSummaryFacts facts = build->shared;
    facts.semantics = xr_semantic_plan_fingerprint(plan);
    facts.semantic_schema = xr_semantic_plan_schema(plan);
    dependency_fingerprint(plan, &facts.dependencies);
    declaration_fingerprint(plan, &facts.declarations);

    XrModuleSummary summary;
    XrCacheKey key;
    if (!xr_module_summary_build(&summary, &key, spec->canonical, &facts)) {
        fprintf(stderr, "Error: cannot derive a module summary for '%s'\n", spec->canonical);
        return false;
    }
    if (!round_trip_semantic_artifact(build, key, plan)) {
        xr_module_summary_finalize(&summary);
        return false;
    }
    if (!xr_dependency_graph_add_node(build->graph, &summary)) {
        fprintf(stderr, "Error: cannot record the module summary for '%s'\n", spec->canonical);
        xr_module_summary_finalize(&summary);
        return false;
    }
    build->ids[topo_index] = summary.module_id;
    xr_module_summary_finalize(&summary);
    return true;
}

static bool add_module_edges(XaotModuleSummaryBuild *build, const XrModuleGraph *graph,
                             const int *topo_of_spec, int module_count) {
    XrModuleFacetMask relation[XR_MODULE_FACET_COUNT];
    xr_module_summary_full_relation(relation);

    for (int consumer = 0; consumer < module_count; consumer++) {
        const XrModuleSpec *spec = &graph->specs[graph->topo_order[consumer]];
        for (int edge = 0; edge < spec->dep_count; edge++) {
            int dependency_spec = spec->dep_indices[edge];
            if (dependency_spec < 0 || dependency_spec >= graph->spec_count)
                return false;
            /* A dependency outside the sorted set would silently drop an
             * observation edge, so it fails the whole publication. */
            int dependency = topo_of_spec[dependency_spec];
            if (dependency < 0 || dependency >= module_count)
                return false;
            if (dependency == consumer ||
                xr_dependency_graph_find_edge(build->graph, build->ids[consumer],
                                              build->ids[dependency])) {
                continue;
            }
            if (!xr_dependency_graph_add_edge(build->graph, build->ids[consumer],
                                              build->ids[dependency], relation))
                return false;
        }
    }
    return true;
}

static bool collect_module_nodes(XaotModuleSummaryBuild *build, const XrModuleGraph *graph,
                                 XiModule *const *modules, int module_count, int *topo_of_spec) {
    for (int ti = 0; ti < module_count; ti++) {
        int spec_index = graph->topo_order[ti];
        if (spec_index < 0 || spec_index >= graph->spec_count)
            return false;
        const XrModuleSpec *spec = &graph->specs[spec_index];
        const XrSemanticPlan *plan = module_semantic_plan(modules[ti]);
        if (!spec->canonical || spec->canonical[0] == '\0' || !plan ||
            !xr_semantic_plan_is_verified(plan)) {
            fprintf(stderr, "Error: module '%s' has no verified SemanticPlan to summarize\n",
                    spec->canonical ? spec->canonical : "<unknown>");
            return false;
        }
        topo_of_spec[spec_index] = ti;
        if (!add_module_node(build, ti, spec, plan))
            return false;
    }
    return true;
}

static bool build_summary_graph(XaotModuleSummaryBuild *build, XrCompilerSession *session,
                                const XrModuleGraph *graph, XiModule *const *modules,
                                int module_count, const XaotBuildOptions *options) {
    const XrTargetProfile *profile = xr_compiler_session_target_profile(session);
    char error[256];
    if (!profile || !xr_target_profile_verify(profile, error, sizeof(error))) {
        fprintf(stderr, "Error: module summaries require an exact verified TargetProfile\n");
        return false;
    }

    build->store = xr_compiler_session_cache_store(session);
    build->modules = modules;
    build->module_count = module_count;
    build->rebuild = options->incremental_cache_rebuild;
    build->shared.target = xr_target_profile_fingerprint(profile);
    toolchain_fingerprint(&build->shared.toolchain);
    configuration_fingerprint(options, &build->shared.configuration);

    build->ids = (XrStableId *) xr_calloc((size_t) module_count, sizeof(*build->ids));
    int *topo_of_spec = (int *) xr_calloc((size_t) graph->spec_count, sizeof(*topo_of_spec));
    if (!build->ids || !topo_of_spec) {
        xr_free(topo_of_spec);
        return false;
    }
    for (int i = 0; i < graph->spec_count; i++)
        topo_of_spec[i] = -1;

    bool ok = collect_module_nodes(build, graph, modules, module_count, topo_of_spec);
    if (ok && !add_module_edges(build, graph, topo_of_spec, module_count)) {
        fprintf(stderr, "Error: the module summary dependency edges are incomplete\n");
        ok = false;
    }
    xr_free(topo_of_spec);
    return ok;
}

bool xaot_publish_module_summaries(XrCompilerSession *session, const XrModuleGraph *graph,
                                   XiModule *const *modules, int module_count,
                                   const XaotBuildOptions *options, bool verbose) {
    if (!session || !graph || !modules || !options || module_count <= 0 ||
        graph->topo_count != module_count) {
        fprintf(stderr, "Error: module summary publication received an inconsistent module set\n");
        return false;
    }

    XaotModuleSummaryBuild build;
    XrDependencyGraph summary_graph;
    memset(&build, 0, sizeof(build));
    xr_dependency_graph_init(&summary_graph);
    build.graph = &summary_graph;

    bool ok = build_summary_graph(&build, session, graph, modules, module_count, options);
    XrFingerprint graph_fingerprint;
    memset(&graph_fingerprint, 0, sizeof(graph_fingerprint));
    if (ok && (!xr_dependency_graph_validate(&summary_graph) ||
               !xr_dependency_graph_fingerprint(&summary_graph, &graph_fingerprint))) {
        fprintf(stderr, "Error: the module summary dependency graph failed validation\n");
        ok = false;
    }
    /* Publication deep-copies the verified graph and advances the workspace
     * generation only after it succeeds. */
    if (ok && !xr_compiler_session_publish_dependency_graph(session, &summary_graph)) {
        fprintf(stderr, "Error: cannot publish the module summary dependency graph\n");
        ok = false;
    }
    xr_dependency_graph_finalize(&summary_graph);
    xr_free(build.ids);

    if (ok && verbose) {
        char hex[XR_FINGERPRINT_BYTES * 2 + 1];
        xr_fingerprint_hex(graph_fingerprint, hex);
        printf("[module-summary] modules=%d graph=%s xsm-hits=%u xsm-published=%u xsm-missed=%u\n",
               module_count, hex, build.report.cache_hits, build.report.cache_published,
               build.report.cache_missed);
    }
    return ok;
}
