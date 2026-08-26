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
#include "../plan/semantic/xr_program_semantic_closure.h"
#include "../plan/semantic/xr_source_semantic_identity.h"
#include "../plan/semantic/xr_semantic_plan.h"
#include "../plan/target/xr_target_profile.h"
#include "../toolchain/xcompiler_session.h"

#include <stdio.h>
#include <string.h>

typedef struct XaotPreparedXsm {
    uint32_t module_index;
    XrStableId module_id;
    XrCacheKey key;
    const XrSemanticPlan *plan;
    const XrSemanticPlan **dependencies;
    uint32_t dependency_count;
    XrCacheBlob blob;
    uint8_t *encoded;
    size_t size;
    bool hit;
} XaotPreparedXsm;

typedef struct XaotModuleSummaryTaskState {
    XaotPreparedXsm *artifacts;
    uint32_t artifact_count;
} XaotModuleSummaryTaskState;

/* One build's owned derivation state. The cache store and the facts shared by
 * every module are read-only for the duration; only the graph, the identity
 * vector, and the report accumulate. */
typedef struct XaotModuleSummaryBuild {
    XrDependencyGraph *graph;
    XrCacheStore *store;
    XiModule *const *modules;
    int module_count;
    XrStableId *ids;
    XrCacheKey *keys;
    const XrProgramSemanticClosure *program_closure;
    XrModuleSummaryFacts shared;
    bool rebuild;
    XaotModuleSummaryCacheStats *stats;
    XrSHA256Context artifact_order;
    XrSHA256Context publish_order;
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
    hash_text(&ctx, "xray-module-summary-declarations-v2");
    hash_u32(&ctx, count);
    for (uint32_t i = 0; i < count; i++) {
        const XrSemanticSourceExportRecord *record = xr_semantic_plan_source_export(plan, i);
        if (!record) {
            hash_u32(&ctx, UINT32_C(0xFFFFFFFF));
            continue;
        }
        hash_text(&ctx, record->name);
        xr_sha256_update(&ctx, record->id.bytes, sizeof(record->id.bytes));
        xr_sha256_update(&ctx, record->exported_entity.bytes,
                         sizeof(record->exported_entity.bytes));
        hash_u32(&ctx, record->function);
        hash_u32(&ctx, record->source_class);
        hash_u32(&ctx, record->shared_slot);
        hash_u32(&ctx, record->kind);
    }
    xr_sha256_final(&ctx, out->bytes);
}

static bool publish_succeeded(XrCachePublishStatus status) {
    return status == XR_CACHE_PUBLISH_OK ||
           status == XR_CACHE_PUBLISH_EXISTS;
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

static int find_module_index(const XaotModuleSummaryBuild *build,
                             XrStableId module_id) {
    for (int module = 0; module < build->module_count; module++)
        if (xr_stable_id_equal(build->ids[module], module_id))
            return module;
    return -1;
}

static void task_diagnostic(XrModuleTaskOutput *output, uint32_t task,
                            XrStableId module, const char *reason) {
    char id[XR_STABLE_ID_BYTES * 2u + 1u];
    xr_stable_id_hex(module, id);
    (void) snprintf(output->diagnostic, sizeof(output->diagnostic),
                    "task=%u module=%s %s", task, id, reason);
}

static const uint8_t *prepared_bytes(const XaotPreparedXsm *artifact) {
    return artifact->hit ? artifact->blob.bytes : artifact->encoded;
}

static bool prepare_module_summary_task(
    const XrModuleTaskGraph *graph, uint32_t task_index,
    XrModuleTaskOutput *output, void *task_state, void *context) {
    XaotModuleSummaryBuild *build = (XaotModuleSummaryBuild *) context;
    XaotModuleSummaryTaskState *state =
        (XaotModuleSummaryTaskState *) task_state;
    XrModuleTaskView view;
    if (!build || !state ||
        !xr_module_task_graph_task(graph, task_index, &view))
        return false;
    state->artifact_count = view.member_count;
    state->artifacts = (XaotPreparedXsm *) xr_calloc(
        view.member_count, sizeof(*state->artifacts));
    if (!state->artifacts) {
        task_diagnostic(output, task_index, view.members[0],
                        "artifact allocation failed");
        return false;
    }

    XrSHA256Context fingerprint;
    xr_sha256_init(&fingerprint);
    hash_text(&fingerprint, "xray-module-summary-task-artifacts-v1");
    hash_u32(&fingerprint, view.member_count);
    for (uint32_t member = 0; member < view.member_count; member++) {
        XrStableId module_id = view.members[member];
        int module_index = find_module_index(build, module_id);
        if (module_index < 0) {
            task_diagnostic(output, task_index, module_id,
                            "has no exact module authority");
            return false;
        }
        XaotPreparedXsm *artifact = &state->artifacts[member];
        artifact->module_index = (uint32_t) module_index;
        artifact->module_id = module_id;
        artifact->key = build->keys[module_index];
        artifact->plan = module_semantic_plan(build->modules[module_index]);
        if (!artifact->plan ||
            !xr_semantic_plan_is_verified(artifact->plan)) {
            task_diagnostic(output, task_index, module_id,
                            "has no verified SemanticPlan");
            return false;
        }

        xr_sha256_update(&fingerprint, module_id.bytes,
                         sizeof(module_id.bytes));
        xr_sha256_update(&fingerprint, artifact->key.bytes,
                         sizeof(artifact->key.bytes));
        if (!build->store) {
            XrFingerprint semantic =
                xr_semantic_plan_fingerprint(artifact->plan);
            xr_sha256_update(&fingerprint, semantic.bytes,
                             sizeof(semantic.bytes));
            continue;
        }

        if (!collect_semantic_dependencies(
                build, artifact->plan, &artifact->dependencies,
                &artifact->dependency_count)) {
            task_diagnostic(output, task_index, module_id,
                            "dependency authority is incomplete");
            return false;
        }
        XrCacheXsmArtifactVerifyContext requirements = {
            .expected_key = artifact->key,
            .semantic_plan = artifact->plan,
            .semantic_dependencies = artifact->dependencies,
            .semantic_dependency_count = artifact->dependency_count,
        };
        if (!build->rebuild) {
            XrCacheLoadStatus load = xr_cache_store_load(
                build->store, XR_CACHE_ARTIFACT_XSM, artifact->key,
                xr_cache_verify_xsm_artifact, &requirements,
                &artifact->blob);
            artifact->hit = load == XR_CACHE_LOAD_HIT;
        }
        if (artifact->hit) {
            artifact->size = artifact->blob.size;
        } else {
            char encode_error[256];
            if (!xr_xsm_encode(artifact->plan, &artifact->encoded,
                               &artifact->size, encode_error,
                               sizeof(encode_error)) ||
                !xr_cache_verify_xsm_artifact(
                    XR_CACHE_ARTIFACT_XSM, artifact->key,
                    artifact->encoded, artifact->size, &requirements)) {
                task_diagnostic(output, task_index, module_id,
                                "fresh XSM authority is invalid");
                return false;
            }
        }
        if (artifact->size > UINT32_MAX) {
            task_diagnostic(output, task_index, module_id,
                            "XSM artifact is too large");
            return false;
        }
        hash_u32(&fingerprint, (uint32_t) artifact->size);
        xr_sha256_update(&fingerprint, prepared_bytes(artifact),
                         artifact->size);
    }
    xr_sha256_final(&fingerprint, output->artifact_fingerprint.bytes);
    return true;
}

static bool preflight_module_summary_task(
    const XrModuleTaskGraph *graph, uint32_t task_index,
    const XrModuleTaskOutput *output, const void *task_state, void *context,
    char *error, size_t error_size) {
    XaotModuleSummaryBuild *build = (XaotModuleSummaryBuild *) context;
    const XaotModuleSummaryTaskState *state =
        (const XaotModuleSummaryTaskState *) task_state;
    XrModuleTaskView view;
    XrSHA256Context fingerprint;
    if (!build || !state || !output || !output->complete ||
        !output->succeeded ||
        !xr_module_task_graph_task(graph, task_index, &view) ||
        state->artifact_count != view.member_count) {
        if (error && error_size)
            (void) snprintf(error, error_size,
                            "task=%u prepared XSM authority is incomplete",
                            task_index);
        return false;
    }

    xr_sha256_init(&fingerprint);
    hash_text(&fingerprint, "xray-module-summary-task-artifacts-v1");
    hash_u32(&fingerprint, view.member_count);
    for (uint32_t member = 0; member < view.member_count; member++) {
        const XaotPreparedXsm *artifact = &state->artifacts[member];
        if (artifact->module_index >= (uint32_t) build->module_count ||
            !xr_stable_id_equal(artifact->module_id,
                                view.members[member]) ||
            !xr_stable_id_equal(
                artifact->module_id, build->ids[artifact->module_index]) ||
            !xr_cache_key_equal(
                artifact->key, build->keys[artifact->module_index]) ||
            artifact->plan !=
                module_semantic_plan(build->modules[artifact->module_index]) ||
            !xr_semantic_plan_is_verified(artifact->plan)) {
            if (error && error_size)
                (void) snprintf(
                    error, error_size,
                    "task=%u member=%u prepared XSM identity is invalid",
                    task_index, member);
            return false;
        }
        xr_sha256_update(&fingerprint, artifact->module_id.bytes,
                         sizeof(artifact->module_id.bytes));
        xr_sha256_update(&fingerprint, artifact->key.bytes,
                         sizeof(artifact->key.bytes));
        if (!build->store) {
            XrFingerprint semantic =
                xr_semantic_plan_fingerprint(artifact->plan);
            xr_sha256_update(&fingerprint, semantic.bytes,
                             sizeof(semantic.bytes));
            continue;
        }

        const uint8_t *bytes = prepared_bytes(artifact);
        XrCacheXsmArtifactVerifyContext requirements = {
            .expected_key = artifact->key,
            .semantic_plan = artifact->plan,
            .semantic_dependencies = artifact->dependencies,
            .semantic_dependency_count = artifact->dependency_count,
        };
        if (!bytes || artifact->size == 0 || artifact->size > UINT32_MAX ||
            !xr_cache_verify_xsm_artifact(
                XR_CACHE_ARTIFACT_XSM, artifact->key, bytes,
                artifact->size, &requirements)) {
            if (error && error_size)
                (void) snprintf(
                    error, error_size,
                    "task=%u member=%u prepared XSM bytes are invalid",
                    task_index, member);
            return false;
        }
        hash_u32(&fingerprint, (uint32_t) artifact->size);
        xr_sha256_update(&fingerprint, bytes, artifact->size);
    }
    XrFingerprint prepared;
    xr_sha256_final(&fingerprint, prepared.bytes);
    if (!xr_fingerprint_equal(prepared, output->artifact_fingerprint)) {
        if (error && error_size)
            (void) snprintf(error, error_size,
                            "task=%u prepared XSM fingerprint is invalid",
                            task_index);
        return false;
    }
    return true;
}

static bool publish_module_summary_task(
    const XrModuleTaskGraph *graph, uint32_t task_index,
    const XrModuleTaskOutput *output, void *task_state, void *context,
    char *error, size_t error_size) {
    (void) graph;
    XaotModuleSummaryBuild *build = (XaotModuleSummaryBuild *) context;
    XaotModuleSummaryTaskState *state =
        (XaotModuleSummaryTaskState *) task_state;
    if (!build || !state || !output || !output->complete ||
        !output->succeeded)
        return false;
    for (uint32_t index = 0; index < state->artifact_count; index++) {
        XaotPreparedXsm *artifact = &state->artifacts[index];
        xr_sha256_update(&build->artifact_order, artifact->module_id.bytes,
                         sizeof(artifact->module_id.bytes));
        xr_sha256_update(&build->artifact_order, artifact->key.bytes,
                         sizeof(artifact->key.bytes));
        if (build->store) {
            hash_u32(&build->artifact_order, (uint32_t) artifact->size);
            xr_sha256_update(&build->artifact_order,
                             prepared_bytes(artifact), artifact->size);
        } else {
            XrFingerprint semantic =
                xr_semantic_plan_fingerprint(artifact->plan);
            xr_sha256_update(&build->artifact_order, semantic.bytes,
                             sizeof(semantic.bytes));
        }
        build->stats->merged_modules++;
        if (!build->store)
            continue;
        if (artifact->hit) {
            build->stats->hits++;
            continue;
        }

        build->stats->misses++;
        build->stats->recomputed_modules++;
        xr_sha256_update(&build->publish_order, artifact->module_id.bytes,
                         sizeof(artifact->module_id.bytes));
        xr_sha256_update(&build->publish_order, artifact->key.bytes,
                         sizeof(artifact->key.bytes));
        XrCacheXsmArtifactVerifyContext requirements = {
            .expected_key = artifact->key,
            .semantic_plan = artifact->plan,
            .semantic_dependencies = artifact->dependencies,
            .semantic_dependency_count = artifact->dependency_count,
        };
        XrCachePublishStatus status = xr_cache_store_publish(
            build->store, XR_CACHE_ARTIFACT_XSM, artifact->key,
            artifact->encoded, artifact->size,
            xr_cache_verify_xsm_artifact, &requirements);
        if (!publish_succeeded(status)) {
            char id[XR_STABLE_ID_BYTES * 2u + 1u];
            xr_stable_id_hex(artifact->module_id, id);
            if (error && error_size)
                (void) snprintf(
                    error, error_size,
                    "task=%u module=%s XSM publication status=%d",
                    task_index, id, (int) status);
            return false;
        }
        if (status == XR_CACHE_PUBLISH_OK)
            build->stats->published++;
    }
    return true;
}

static void finalize_module_summary_task(void *task_state, void *context) {
    (void) context;
    XaotModuleSummaryTaskState *state =
        (XaotModuleSummaryTaskState *) task_state;
    if (!state)
        return;
    for (uint32_t index = 0; index < state->artifact_count; index++) {
        xr_cache_blob_release(&state->artifacts[index].blob);
        xr_free(state->artifacts[index].encoded);
        xr_free(state->artifacts[index].dependencies);
    }
    xr_free(state->artifacts);
    memset(state, 0, sizeof(*state));
}

static const XrSemanticPlan *module_semantic_plan(const XiModule *module) {
    return module && module->init ? module->init->semantic_plan : NULL;
}

static const XrProgramSemanticModuleRecord *program_module_for_spec(
    const XrProgramSemanticClosure *closure, const XrModuleSpec *spec) {
    XrProgramSemanticModuleInput source;
    if (!closure || !spec || !spec->canonical ||
        !xr_source_semantic_module_authority(spec->canonical,
                                             spec->source_content_fingerprint, &source, NULL))
        return NULL;
    const XrProgramSemanticModuleRecord *match = NULL;
    for (uint32_t row = 0; row < xr_program_semantic_closure_module_count(closure); row++) {
        const XrProgramSemanticModuleRecord *candidate =
            xr_program_semantic_closure_module(closure, row);
        if (!candidate || !xr_stable_id_equal(candidate->module_identity,
                                              source.module_identity))
            continue;
        if (match ||
            !xr_fingerprint_equal(candidate->module_authority_fingerprint,
                                  source.module_authority_fingerprint) ||
            !xr_fingerprint_equal(candidate->source_fingerprint, source.source_fingerprint))
            return NULL;
        /* Source authority reconstructs the module and source rows only. The
         * verified PSC family owns its exact empty or typed export fingerprint;
         * the complete PSC/GCI keys every XSM below. */
        match = candidate;
    }
    return match;
}

static bool add_module_node(XaotModuleSummaryBuild *build, int topo_index,
                            const XrModuleSpec *spec, const XrSemanticPlan *plan) {
    XrModuleSummaryFacts facts = build->shared;
    const XrSemanticProgramProvenance *program =
        xr_semantic_plan_program_provenance(plan);
    if (build->program_closure) {
        XrFingerprint product =
            xr_program_semantic_closure_fingerprint(build->program_closure);
        XrGenerationClosureId generation =
            xr_program_semantic_closure_generation_id(build->program_closure);
        if (!program_module_for_spec(build->program_closure, spec) ||
            (program && program->schema != 0 &&
             (!xr_fingerprint_equal(program->program_fingerprint, product) ||
              memcmp(program->generation_identity.bytes, generation.bytes,
                     sizeof(generation.bytes)) != 0))) {
            fprintf(stderr, "Error: product program authority does not bind module '%s'\n",
                    spec && spec->canonical ? spec->canonical : "<unknown>");
            return false;
        }
        facts.program_semantics = product;
        memcpy(facts.generation.bytes, generation.bytes, sizeof(generation.bytes));
    } else if (program && program->schema != 0) {
        facts.program_semantics = program->program_fingerprint;
        facts.generation = program->generation_identity;
    }
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
    if (!xr_dependency_graph_add_node(build->graph, &summary)) {
        fprintf(stderr, "Error: cannot record the module summary for '%s'\n", spec->canonical);
        xr_module_summary_finalize(&summary);
        return false;
    }
    build->ids[topo_index] = summary.module_id;
    build->keys[topo_index] = key;
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
                                int module_count,
                                const XrProgramSemanticClosure *program_closure,
                                const XaotBuildOptions *options) {
    const XrTargetProfile *profile = xr_compiler_session_target_profile(session);
    char error[256];
    if (!profile || !xr_target_profile_verify(profile, error, sizeof(error))) {
        fprintf(stderr, "Error: module summaries require an exact verified TargetProfile\n");
        return false;
    }

    build->store = xr_compiler_session_cache_store(session);
    build->modules = modules;
    build->module_count = module_count;
    build->program_closure = program_closure;
    build->rebuild = options->incremental_cache_rebuild;
    if (program_closure &&
        (!xr_program_semantic_closure_is_frozen(program_closure) ||
         !xr_program_semantic_closure_is_verified(program_closure) ||
         !xr_program_semantic_closure_verify(program_closure, error, sizeof(error)) ||
         xr_program_semantic_closure_module_count(program_closure) != (size_t) module_count)) {
        fprintf(stderr, "Error: module summaries require one exact product program closure\n");
        return false;
    }
    build->shared.target = xr_target_profile_fingerprint(profile);
    toolchain_fingerprint(&build->shared.toolchain);
    configuration_fingerprint(options, &build->shared.configuration);

    build->ids = (XrStableId *) xr_calloc((size_t) module_count, sizeof(*build->ids));
    build->keys = (XrCacheKey *) xr_calloc((size_t) module_count,
                                           sizeof(*build->keys));
    int *topo_of_spec = (int *) xr_calloc((size_t) graph->spec_count, sizeof(*topo_of_spec));
    if (!build->ids || !build->keys || !topo_of_spec) {
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
                                   const XrProgramSemanticClosure *program_closure,
                                   const XaotBuildOptions *options, bool verbose,
                                   XaotModuleSummaryCacheStats *stats) {
    if (stats)
        memset(stats, 0, sizeof(*stats));
    if (!session || !graph || !modules || !options || !stats || module_count <= 0 ||
        graph->topo_count != module_count) {
        fprintf(stderr, "Error: module summary publication received an inconsistent module set\n");
        return false;
    }

    XaotModuleSummaryBuild build;
    XrDependencyGraph summary_graph;
    memset(&build, 0, sizeof(build));
    xr_dependency_graph_init(&summary_graph);
    build.graph = &summary_graph;
    build.stats = stats;

    bool ok = build_summary_graph(&build, session, graph, modules, module_count,
                                  program_closure, options);
    if (ok && program_closure) {
        stats->program_modules =
            (uint32_t) xr_program_semantic_closure_module_count(program_closure);
        stats->program_dependencies =
            (uint32_t) xr_program_semantic_closure_dependency_count(program_closure);
        stats->program_fingerprint =
            xr_program_semantic_closure_fingerprint(program_closure);
        stats->generation_identity =
            xr_program_semantic_closure_generation_id(program_closure);
    }
    XrFingerprint graph_fingerprint;
    memset(&graph_fingerprint, 0, sizeof(graph_fingerprint));
    if (ok && (!xr_dependency_graph_validate(&summary_graph) ||
               !xr_dependency_graph_fingerprint(&summary_graph, &graph_fingerprint))) {
        fprintf(stderr, "Error: the module summary dependency graph failed validation\n");
        ok = false;
    }
    if (ok) {
        xr_sha256_init(&build.artifact_order);
        hash_text(&build.artifact_order,
                  "xray-module-summary-artifact-order-v1");
        xr_sha256_init(&build.publish_order);
        hash_text(&build.publish_order,
                  "xray-module-summary-publish-order-v1");
        XrCompilerSessionModuleTaskBatch task_batch = {
            .dependency_graph = &summary_graph,
            .worker_limit = options->target_plan_workers,
            .task_state_size = sizeof(XaotModuleSummaryTaskState),
            .prepare = prepare_module_summary_task,
            .preflight = preflight_module_summary_task,
            .publish = publish_module_summary_task,
            .finalize = finalize_module_summary_task,
            .context = &build,
        };
        XrCompilerSessionModuleTaskStats task_stats;
        char error[512];
        if (!xr_compiler_session_publish_module_tasks(
                session, &task_batch, &task_stats, error, sizeof(error))) {
            fprintf(stderr, "Error: module summary task publication failed: %s\n",
                    error[0] ? error : "unknown error");
            ok = false;
        } else {
            stats->workers = task_stats.execution.worker_count;
            stats->tasks = task_stats.task_count;
            xr_sha256_final(
                &build.artifact_order,
                stats->artifact_order_fingerprint.bytes);
            xr_sha256_final(
                &build.publish_order,
                stats->publish_order_fingerprint.bytes);
        }
    }
    xr_dependency_graph_finalize(&summary_graph);
    xr_free(build.keys);
    xr_free(build.ids);

    if (ok && verbose) {
        char graph_hex[XR_FINGERPRINT_BYTES * 2 + 1];
        char artifact_hex[XR_FINGERPRINT_BYTES * 2 + 1];
        char publish_hex[XR_FINGERPRINT_BYTES * 2 + 1];
        xr_fingerprint_hex(graph_fingerprint, graph_hex);
        xr_fingerprint_hex(stats->artifact_order_fingerprint,
                           artifact_hex);
        xr_fingerprint_hex(stats->publish_order_fingerprint,
                           publish_hex);
        printf("[module-summary] modules=%d graph=%s xsm-hits=%u xsm-published=%u xsm-missed=%u workers=%u tasks=%u xsm-recomputed=%u xsm-artifacts=%s xsm-publish-order=%s\n",
               module_count, graph_hex, stats->hits, stats->published,
               stats->misses, stats->workers, stats->tasks,
               stats->recomputed_modules, artifact_hex, publish_hex);
        if (program_closure) {
            char program_hex[XR_FINGERPRINT_BYTES * 2 + 1];
            char generation_hex[XR_STABLE_ID_BYTES * 2 + 1];
            XrStableId generation_id;
            xr_fingerprint_hex(stats->program_fingerprint, program_hex);
            memcpy(generation_id.bytes, stats->generation_identity.bytes,
                   sizeof(generation_id.bytes));
            xr_stable_id_hex(generation_id, generation_hex);
            printf("[program-closure] modules=%u dependencies=%u psc=%s gci=%s\n",
                   stats->program_modules, stats->program_dependencies, program_hex,
                   generation_hex);
        }
    }
    return ok;
}
