/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xaot_driver.c - AOT native compilation driver (Xi IR pipeline)
 *
 * KEY CONCEPT:
 *   Full pipeline from source file to generated C program:
 *   1. Module graph discovery (topo-sorted via XrModuleGraph)
 *   2. Cross-module analysis with shared XaAnalyzer (typed imports)
 *   3. Per-module: canonicalize → Xi IR lower → optimize → select_rep
 *   4. Cross-module import resolution via export table
 *   5. C code generation via xi_cgen
 *   6. Main() generation calling module inits in topo order
 *
 * RELATED MODULES:
 *   - xi_cgen.h: Xi IR → C code generation
 *   - xaot_driver.h: public API
 *   - xcmd_build.c: CLI entry that invokes xaot_build + CC
 */

#include "xaot_driver.h"
#include "../../include/xray.h"
#include "../../include/xray_vm.h"
#include "../runtime/xisolate_api.h"
#include "../module/xmodule_graph.h"
#include "../module/xmodule_resolver.h"
#include "../module/xmodule.h"
#include "../base/xmalloc.h"
#include "../base/xhash.h"
#include "../base/xfileio.h"
#include "../base/xmemstream.h"
#include "../base/xglobal_indices.h"
#include "../os/os_dir.h"
#include "../ir/xi.h"
#include "../ir/xi_evidence.h"
#include "../ir/xi_op_name.h"
#include "../ir/xi_pipeline.h"
#include "../ir/xi_import_resolve.h"
#include "xi_cgen.h"
#include "xi_cgen_verify_output.h"
#include "xi_backend_plan_contract.h"
#include "xi_lto.h"
#include "xaot_bundle.h"
#include "xaot_boundary.h"
#include "xaot_link.h"
#include "xaot_prepare.h"
#include "xaot_verify.h"
#include "../analysis/xglobal_producer.h"
#include "../frontend/canonical/xcanon.h"
#include "../frontend/analyzer/xanalyzer.h"
#include "../frontend/analyzer/xanalyzer_mono.h"
#include "../toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include <limits.h>
#include <time.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static const uint32_t xaot_evidence_cache_phases[XG_EVIDENCE_CACHE_PHASE_COUNT] = {
    XG_EVIDENCE_CACHE_DECLARATIONS,
    XG_EVIDENCE_CACHE_SEMANTIC_GRAPH,
    XG_EVIDENCE_CACHE_BODY_SUMMARY,
    XG_EVIDENCE_CACHE_GLOBAL_EVIDENCE,
};

static bool xaot_func_has_explicit_vector_ops(const XiFunc *func) {
    if (!func)
        return false;
    for (uint32_t bi = 0; bi < func->nblocks; bi++) {
        const XiBlock *block = func->blocks[bi];
        for (uint32_t vi = 0; block && vi < block->nvalues; vi++) {
            const XiValue *value = block->values[vi];
            if (value && value->op >= XI_VEC_LOAD && value->op <= XI_VEC_REDUCE_ADD &&
                xi_vec_shape_is_explicit(value->aux_int))
                return true;
        }
    }
    for (uint16_t i = 0; i < func->nchildren; i++) {
        if (xaot_func_has_explicit_vector_ops(func->children[i]))
            return true;
    }
    return false;
}

static bool xaot_bundle_has_explicit_vector_ops(XiModule **modules, int nmodules) {
    for (int i = 0; modules && i < nmodules; i++) {
        if (modules[i] && xaot_func_has_explicit_vector_ops(modules[i]->init))
            return true;
    }
    return false;
}

static bool xaot_target_uses_x86_vector_islands(const XaotTarget *target) {
    return target &&
           (target->simd_features & (XAOT_SIMD_FEATURE_AVX2 | XAOT_SIMD_FEATURE_AVX512)) != 0;
}

static const char *xaot_view_origin_name(uint8_t origin) {
    switch (origin) {
        case XI_VIEW_ORIGIN_PARAM:
            return "param";
        case XI_VIEW_ORIGIN_RECEIVER:
            return "receiver";
        case XI_VIEW_ORIGIN_STATIC:
            return "static";
        case XI_VIEW_ORIGIN_LOCAL:
            return "local";
        case XI_VIEW_ORIGIN_MULTI:
            return "multi";
        case XI_VIEW_ORIGIN_FOREIGN:
            return "foreign";
        case XI_VIEW_ORIGIN_ALLOCATION:
            return "allocation";
        case XI_VIEW_ORIGIN_UNKNOWN:
            return "unknown";
        case XI_VIEW_ORIGIN_NONE:
        default:
            return "none";
    }
}

static void xaot_dump_value_view_evidence(FILE *out, const XiFunc *func, const XiValue *value) {
    const XiViewEvidence *view;
    if (!out || !func || !value || !value->view_evidence.complete)
        return;
    view = &value->view_evidence;
    fprintf(out,
            "view-evidence function=%s value=v%u op=%s origin=%s source-param=%d "
            "source-operand=%d root=v%u element=%u capability=%s lifetime=%s "
            "invalidation=%u complete=yes\n",
            func->name ? func->name : "<anonymous>", value->id, xi_op_name(value->op),
            xaot_view_origin_name(view->origin), (int) view->source_param,
            (int) view->source_operand, view->root_value_id, view->element_type_id,
            view->capability == 2 ? "write" : "read", view->lifetime == 2 ? "static" : "caller",
            view->invalidation_set_id);
}

static void xaot_dump_func_view_evidence(FILE *out, const XiFunc *func) {
    if (!out || !func)
        return;
    for (uint16_t pi = 0; pi < func->nparams; pi++) {
        const XiValue *param = func->params[pi];
        if (!param || !param->type || !XR_TYPE_IS_SLICE(param->type))
            continue;
        fprintf(out,
                "view-param function=%s param=%u value=v%u origin=caller element=%u "
                "capability=%s lifetime=caller complete=yes\n",
                func->name ? func->name : "<anonymous>", (unsigned) pi, param->id,
                param->type->container.element_type
                    ? param->type->container.element_type->semantic_type_id
                    : 0,
                param->param_mode == XR_PARAM_REF ? "write" : "read");
    }
    if (func->view_return_complete) {
        fprintf(out, "view-return function=%s origin=%s source-param=%d complete=yes\n",
                func->name ? func->name : "<anonymous>",
                xaot_view_origin_name(func->view_return_source), (int) func->view_return_param);
    }
    for (uint32_t bi = 0; bi < func->nblocks; bi++) {
        const XiBlock *block = func->blocks[bi];
        if (!block)
            continue;
        for (const XiPhi *phi = block->phis; phi; phi = phi->next)
            xaot_dump_value_view_evidence(out, func, &phi->value);
        for (uint32_t vi = 0; vi < block->nvalues; vi++)
            xaot_dump_value_view_evidence(out, func, block->values[vi]);
        xaot_dump_value_view_evidence(out, func, block->control);
    }
}

static const char *xaot_codegen_policy_name(uint8_t policy) {
    switch ((XiInlinePolicy) policy) {
        case XI_INLINE_PREFER:
            return "inline";
        case XI_INLINE_PRESERVE_CALL:
            return "noinline";
        case XI_INLINE_AUTO:
        default:
            return NULL;
    }
}

static const char *xaot_codegen_policy_capability(uint8_t policy) {
    return policy == XI_INLINE_PREFER ? "forceInline" : "preserveCall";
}

static void xaot_dump_codegen_value(FILE *out, const XaotBundle *bundle, const XiFunc *func,
                                    const XiValue *value) {
    if (!out || !func || !value)
        return;
    if (value->op == XI_CODEGEN_OPAQUE) {
        fprintf(out,
                "codegen-control function=%s kind=opaque value=v%u source-line=%u "
                "stage=lowered backend=aot-c provider-capability=valueOpaque "
                "lowering=xrt_codegen_opaque reason=canonical-xi-op\n",
                func->name ? func->name : "<anonymous>", value->id, value->line);
        return;
    }
    if (value->op == XI_CODEGEN_COMPILER_FENCE) {
        fprintf(out,
                "codegen-control function=%s kind=compilerFence value=v%u source-line=%u "
                "stage=lowered backend=aot-c provider-capability=compilerFence "
                "lowering=xrt_codegen_compiler_fence reason=canonical-xi-op\n",
                func->name ? func->name : "<anonymous>", value->id, value->line);
        return;
    }
    if (value->op == XI_CALL || value->op == XI_CALL_METHOD || value->op == XI_CALL_METHOD_DIRECT) {
        const XiFunc *target = xaot_boundary_resolve_direct_call_target(bundle, func, value, NULL);
        const char *policy = target ? xaot_codegen_policy_name(target->inline_policy) : NULL;
        if (!policy)
            return;
        fprintf(out,
                "codegen-edge function=%s caller=%s callee=%s kind=%s call=v%u source-line=%u "
                "stage=lowered backend=aot-c provider-capability=%s "
                "lowering=%s reason=surviving-direct-edge\n",
                func->name ? func->name : "<anonymous>", func->name ? func->name : "<anonymous>",
                target->name ? target->name : "<anonymous>", policy, value->id, value->line,
                xaot_codegen_policy_capability(target->inline_policy),
                target->inline_policy == XI_INLINE_PREFER ? "XR_AINLINE" : "XR_NOINLINE");
    }
}

static void xaot_dump_func_codegen_evidence(FILE *out, const XaotBundle *bundle,
                                            const XiFunc *func) {
    const char *policy;
    if (!out || !func)
        return;
    policy = xaot_codegen_policy_name(func->inline_policy);
    if (policy) {
        fprintf(out,
                "codegen-control function=%s kind=%s stage=lowered backend=aot-c "
                "provider-capability=%s lowering=%s reason=source-policy\n",
                func->name ? func->name : "<anonymous>", policy,
                xaot_codegen_policy_capability(func->inline_policy),
                func->inline_policy == XI_INLINE_PREFER ? "XR_AINLINE" : "XR_NOINLINE");
    }
    for (uint32_t bi = 0; bi < func->nblocks; bi++) {
        const XiBlock *block = func->blocks[bi];
        if (!block)
            continue;
        for (uint32_t vi = 0; vi < block->nvalues; vi++)
            xaot_dump_codegen_value(out, bundle, func, block->values[vi]);
        xaot_dump_codegen_value(out, bundle, func, block->control);
    }
}

static void xaot_dump_func_local_evidence(FILE *out, const XaotBundle *bundle, const XiFunc *func,
                                          uint32_t depth) {
    if (!out || !func)
        return;
    fprintf(out, "xi-evidence function=%s depth=%u stage=%s revision=%llu/%llu/%llu/%llu\n",
            func->name ? func->name : "<anonymous>", depth, xi_stage_name(func->stage),
            (unsigned long long) func->ir_revision, (unsigned long long) func->cfg_version,
            (unsigned long long) func->memory_revision, (unsigned long long) func->call_revision);
    xi_evidence_dump(func, out);
    xaot_dump_func_view_evidence(out, func);
    xaot_dump_func_codegen_evidence(out, bundle, func);
    for (uint16_t i = 0; i < func->nchildren; i++)
        xaot_dump_func_local_evidence(out, bundle, func->children[i], depth + 1);
}

static char *xaot_dump_local_evidence(const XaotBundle *bundle, XiFunc **funcs, int nfuncs) {
    char *buf = NULL;
    size_t bufsz = 0;
    FILE *out = xr_open_memstream(&buf, &bufsz);
    if (!out)
        return NULL;
    for (int i = 0; funcs && i < nfuncs; i++)
        xaot_dump_func_local_evidence(out, bundle, funcs[i], 0);
    bool write_failed = ferror(out) != 0;
    int close_status = xr_close_memstream(out, &buf, &bufsz);
    if (write_failed || close_status != 0) {
        xr_free(buf);
        return NULL;
    }
    return buf;
}

static bool xaot_str_has_suffix(const char *text, const char *suffix) {
    size_t text_len;
    size_t suffix_len;
    if (!text || !suffix)
        return false;
    text_len = strlen(text);
    suffix_len = strlen(suffix);
    return text_len >= suffix_len && memcmp(text + text_len - suffix_len, suffix, suffix_len) == 0;
}

static const char *xaot_freestanding_stdlib_module_suggestion(const char *module_name) {
    if (module_name && strcmp(module_name, "parallel") == 0) {
        return "parallel uses the hosted CPU batch executor; freestanding code must use explicit "
               "raw loops or a platform-specific runtime";
    }
    return "only prelude, math, mem, simd, and codegen are in the freestanding allowlist";
}

static bool xaot_reject_freestanding_stdlib_graph(const XrModuleGraph *graph) {
    if (!graph)
        return true;
    for (int i = 0; i < graph->spec_count; i++) {
        const XrModuleSpec *spec = &graph->specs[i];
        const char *module_name = spec->canonical;
        if (spec->kind != XR_MOD_STDLIB || !module_name)
            continue;
        if (xa_freestanding_stdlib_module_allowed(module_name))
            continue;
        fprintf(stderr, "Error: freestanding profile rejects stdlib module '%s'; %s\n", module_name,
                xaot_freestanding_stdlib_module_suggestion(module_name));
        return false;
    }
    return true;
}

static bool xaot_evidence_cache_phase_dir(const char *cache_dir, uint32_t phase, char *out,
                                          size_t out_sz) {
    int n;
    if (!cache_dir || !out || out_sz == 0)
        return false;
    n = snprintf(out, out_sz, "%s/evidence/%s", cache_dir, xg_evidence_cache_phase_name(phase));
    return n >= 0 && (size_t) n < out_sz;
}

static bool xaot_payload_materializes_for_preproducer(const char *payload,
                                                      const XgEvidenceCacheRequestKey *request,
                                                      bool *out_materialized,
                                                      XgGlobalEvidence *out_global_evidence,
                                                      bool *out_global_evidence_initialized) {
    XgGlobalEvidence materialized;
    XgEvidenceCachePayloadInfo info;
    XgEvidenceCacheKey materialized_key;
    bool ok;
    if (out_materialized)
        *out_materialized = false;
    if (!payload || !request || !xg_evidence_cache_payload_parse(payload, &info))
        return false;
    if (info.phase != request->phase ||
        info.request_hash != xg_evidence_cache_request_key_hash(request) ||
        !xg_evidence_cache_request_key_matches(&info.request_key, request))
        return false;
    memset(&materialized, 0, sizeof(materialized));
    ok = xg_evidence_cache_payload_materialize(payload, &materialized);
    if (ok) {
        materialized_key = xg_global_evidence_cache_key(&materialized, request->phase);
        ok = xg_evidence_cache_key_matches(&materialized_key, &info.key);
    }
    if (ok && request->phase == XG_EVIDENCE_CACHE_GLOBAL_EVIDENCE && out_global_evidence &&
        out_global_evidence_initialized) {
        if (*out_global_evidence_initialized)
            xg_global_evidence_free(out_global_evidence);
        *out_global_evidence = materialized;
        *out_global_evidence_initialized = true;
        memset(&materialized, 0, sizeof(materialized));
    }
    xg_global_evidence_free(&materialized);
    if (ok && out_materialized)
        *out_materialized = true;
    return ok;
}

static bool xaot_probe_preproducer_payload(const char *cache_dir,
                                           const XgEvidenceCacheRequestKey *request,
                                           bool *out_materialized,
                                           XgGlobalEvidence *out_global_evidence,
                                           bool *out_global_evidence_initialized) {
    char phase_dir[PATH_MAX];
    XrDirIter *it;
    XrDirEntry entry;
    bool hit = false;
    if (out_materialized)
        *out_materialized = false;
    if (!cache_dir || !request ||
        !xaot_evidence_cache_phase_dir(cache_dir, request->phase, phase_dir, sizeof(phase_dir)))
        return false;
    it = xr_dir_open(phase_dir);
    if (!it)
        return false;
    while (xr_dir_next(it, &entry)) {
        char payload_path[PATH_MAX];
        char *payload;
        size_t size = 0;
        int n;
        if (entry.is_dir || !xaot_str_has_suffix(entry.name, ".xgpayload"))
            continue;
        n = snprintf(payload_path, sizeof(payload_path), "%s/%s", phase_dir, entry.name);
        if (n < 0 || (size_t) n >= sizeof(payload_path))
            continue;
        payload = xr_file_read_all(payload_path, "rb", &size);
        if (!payload)
            continue;
        (void) size;
        if (xaot_payload_materializes_for_preproducer(payload, request, out_materialized,
                                                      out_global_evidence,
                                                      out_global_evidence_initialized)) {
            hit = true;
            xr_free(payload);
            break;
        }
        xr_free(payload);
    }
    xr_dir_close(it);
    return hit;
}

static void xaot_probe_preproducer_evidence_cache(const char *cache_dir,
                                                  const XgBuildKey *build_key, bool verbose,
                                                  bool force_rebuild,
                                                  XgGlobalEvidence *out_global_evidence,
                                                  bool *out_global_evidence_initialized) {
    uint32_t request_hits = 0;
    uint32_t request_misses = 0;
    uint32_t materialized = 0;
    if (!cache_dir || !build_key)
        return;
    for (uint32_t i = 0; i < XG_EVIDENCE_CACHE_PHASE_COUNT; i++) {
        uint32_t phase = xaot_evidence_cache_phases[i];
        XgEvidenceCacheRequestKey request =
            xg_evidence_cache_request_key_from_build_key(build_key, phase);
        bool is_materialized = false;
        bool hit = false;
        if (!force_rebuild)
            hit = xaot_probe_preproducer_payload(cache_dir, &request, &is_materialized,
                                                 out_global_evidence,
                                                 out_global_evidence_initialized);
        if (hit) {
            request_hits++;
            if (is_materialized)
                materialized++;
        } else {
            request_misses++;
        }
        if (verbose) {
            printf("[xi-native] evidence cache preproducer %s: %s (request=%016llx "
                   "materialized=%s)%s\n",
                   xg_evidence_cache_phase_name(phase), hit ? "hit" : "miss",
                   (unsigned long long) xg_evidence_cache_request_key_hash(&request),
                   is_materialized ? "yes" : "no", force_rebuild ? " rebuild" : "");
        }
    }
    if (verbose) {
        printf("[xi-native] evidence cache preproducer summary: request_hits=%u "
               "request_misses=%u materialized=%u%s\n",
               request_hits, request_misses, materialized, force_rebuild ? " rebuild" : "");
    }
}

typedef struct XaotDiscoveredPackagePayload {
    char *path;
    char *payload;
    XgModuleSummary *modules;
    uint32_t module_count;
} XaotDiscoveredPackagePayload;

static void xaot_free_discovered_package_payload_fields(XaotDiscoveredPackagePayload *item) {
    if (!item)
        return;
    xr_free(item->path);
    xr_free(item->payload);
    xr_free(item->modules);
    memset(item, 0, sizeof(*item));
}

static void xaot_free_discovered_package_payloads(XaotDiscoveredPackagePayload *items,
                                                  uint32_t count) {
    if (!items)
        return;
    for (uint32_t i = 0; i < count; i++)
        xaot_free_discovered_package_payload_fields(&items[i]);
    xr_free(items);
}

static bool xaot_reserve_discovered_package_payloads(XaotDiscoveredPackagePayload **items,
                                                     uint32_t *cap, uint32_t needed) {
    uint32_t new_cap;
    XaotDiscoveredPackagePayload *new_items;
    if (!items || !cap)
        return false;
    if (*cap >= needed)
        return true;
    new_cap = *cap ? *cap : 4;
    while (new_cap < needed) {
        if (new_cap > UINT32_MAX / 2)
            return false;
        new_cap *= 2;
    }
    new_items =
        (XaotDiscoveredPackagePayload *) xr_realloc(*items, (size_t) new_cap * sizeof(**items));
    if (!new_items)
        return false;
    memset(new_items + *cap, 0, (size_t) (new_cap - *cap) * sizeof(*new_items));
    *items = new_items;
    *cap = new_cap;
    return true;
}

static bool xaot_reserve_module_summaries(XgModuleSummary **items, uint32_t *cap, uint32_t needed) {
    uint32_t new_cap;
    XgModuleSummary *new_items;
    if (!items || !cap)
        return false;
    if (*cap >= needed)
        return true;
    new_cap = *cap ? *cap : 4;
    while (new_cap < needed) {
        if (new_cap > UINT32_MAX / 2)
            return false;
        new_cap *= 2;
    }
    new_items = (XgModuleSummary *) xr_realloc(*items, (size_t) new_cap * sizeof(**items));
    if (!new_items)
        return false;
    memset(new_items + *cap, 0, (size_t) (new_cap - *cap) * sizeof(*new_items));
    *items = new_items;
    *cap = new_cap;
    return true;
}

static bool xaot_module_identity_seen(const XgModuleSummary *modules, uint32_t count,
                                      const XgModuleSummary *candidate) {
    if (!modules || !candidate)
        return false;
    for (uint32_t i = 0; i < count; i++) {
        if (xg_module_summary_identity_matches(&modules[i], candidate))
            return true;
    }
    return false;
}

static bool xaot_collect_imported_package_modules_from_payloads(const char *const *payloads,
                                                                uint32_t payload_count,
                                                                XgModuleSummary **out_modules,
                                                                uint32_t *out_module_count) {
    XgModuleSummary *modules = NULL;
    uint32_t count = 0;
    uint32_t cap = 0;
    bool ok = false;
    if (out_modules)
        *out_modules = NULL;
    if (out_module_count)
        *out_module_count = 0;
    if (!out_modules || !out_module_count || (payload_count > 0 && !payloads))
        return false;
    for (uint32_t i = 0; i < payload_count; i++) {
        XgEvidenceCachePayloadInfo info;
        XgGlobalEvidence package;
        if (!payloads[i] || !xg_evidence_cache_payload_parse(payloads[i], &info) ||
            info.phase != XG_EVIDENCE_CACHE_GLOBAL_EVIDENCE)
            goto done;
        memset(&package, 0, sizeof(package));
        if (!xg_evidence_cache_payload_materialize(payloads[i], &package)) {
            xg_global_evidence_free(&package);
            goto done;
        }
        for (uint32_t j = 0; j < package.nmodules; j++) {
            if (!xg_module_summary_identity_complete(&package.modules[j]) ||
                xaot_module_identity_seen(modules, count, &package.modules[j]) ||
                !xaot_reserve_module_summaries(&modules, &cap, count + 1)) {
                xg_global_evidence_free(&package);
                goto done;
            }
            modules[count++] = package.modules[j];
        }
        xg_global_evidence_free(&package);
    }
    *out_modules = modules;
    *out_module_count = count;
    modules = NULL;
    ok = true;

done:
    xr_free(modules);
    return ok;
}

static int xaot_discovered_package_payload_compare(const XaotDiscoveredPackagePayload *a,
                                                   const XaotDiscoveredPackagePayload *b) {
    const char *ap = a && a->path ? a->path : "";
    const char *bp = b && b->path ? b->path : "";
    if (!a || !b)
        return strcmp(ap, bp);
    if (a->module_count != b->module_count)
        return a->module_count > b->module_count ? -1 : 1;
    return strcmp(ap, bp);
}

static void xaot_sort_discovered_package_payloads(XaotDiscoveredPackagePayload *items,
                                                  uint32_t count) {
    for (uint32_t i = 1; i < count; i++) {
        XaotDiscoveredPackagePayload item = items[i];
        uint32_t j = i;
        while (j > 0 && xaot_discovered_package_payload_compare(&item, &items[j - 1]) < 0) {
            items[j] = items[j - 1];
            j--;
        }
        items[j] = item;
    }
}

static bool xaot_mark_package_dependency_closure(const XrModuleGraph *graph, int idx,
                                                 bool *marked) {
    if (!graph || !marked || idx < 0 || idx >= graph->spec_count)
        return false;
    if (marked[idx])
        return true;
    marked[idx] = true;
    for (int i = 0; i < graph->specs[idx].dep_count; i++) {
        if (!xaot_mark_package_dependency_closure(graph, graph->specs[idx].dep_indices[i], marked))
            return false;
    }
    return true;
}

static int xaot_find_package_payload_root(const XrModuleGraph *graph,
                                          const XgModuleSummary *root_module) {
    if (!graph || !root_module)
        return -1;
    for (int i = 0; i < graph->spec_count; i++) {
        XgModuleSummary expected;
        if (i == graph->entry_index || graph->specs[i].kind != XR_MOD_PACKAGE)
            continue;
        if (!xg_module_summary_from_module_spec(&expected, root_module->module_id,
                                                &graph->specs[i]))
            continue;
        if (xg_module_summary_identity_matches(root_module, &expected))
            return i;
    }
    return -1;
}

static bool xaot_payload_matches_package_graph(const char *payload, const XrModuleGraph *graph,
                                               uint32_t profile, XgModuleSummary **out_modules,
                                               uint32_t *out_module_count) {
    XgEvidenceCachePayloadInfo info;
    XgBuildKey expected_key;
    XgEvidenceCacheRequestKey expected_request;
    XgGlobalEvidence package;
    bool *closure = NULL;
    const XrModuleSpec **ordered_specs = NULL;
    XgModuleSummary *modules = NULL;
    uint32_t closure_count = 0;
    uint32_t ordered_count = 0;
    int root_idx;
    bool ok = false;
    if (out_modules)
        *out_modules = NULL;
    if (out_module_count)
        *out_module_count = 0;
    if (!payload || !graph || !out_modules || !out_module_count)
        return false;
    if (!xg_evidence_cache_payload_parse(payload, &info) ||
        info.phase != XG_EVIDENCE_CACHE_GLOBAL_EVIDENCE)
        return false;
    memset(&package, 0, sizeof(package));
    if (!xg_evidence_cache_payload_materialize(payload, &package))
        return false;
    if (package.nmodules == 0)
        goto done;
    for (uint32_t i = 0; i < package.nmodules; i++) {
        if (package.modules[i].module_id != i + 1 ||
            !xg_module_summary_identity_complete(&package.modules[i]))
            goto done;
    }
    root_idx = xaot_find_package_payload_root(graph, &package.modules[package.nmodules - 1]);
    if (root_idx < 0)
        goto done;
    closure = (bool *) xr_calloc((size_t) graph->spec_count, sizeof(*closure));
    ordered_specs = (const XrModuleSpec **) xr_calloc(package.nmodules, sizeof(*ordered_specs));
    if (!closure || !ordered_specs)
        goto done;
    if (!xaot_mark_package_dependency_closure(graph, root_idx, closure))
        goto done;
    for (int i = 0; i < graph->spec_count; i++) {
        if (closure[i])
            closure_count++;
    }
    if (closure_count != package.nmodules)
        goto done;
    for (int ti = 0; ti < graph->topo_count; ti++) {
        int idx = graph->topo_order[ti];
        if (idx >= 0 && idx < graph->spec_count && closure[idx])
            ordered_specs[ordered_count++] = &graph->specs[idx];
    }
    if (ordered_count != package.nmodules)
        goto done;
    for (uint32_t i = 0; i < package.nmodules; i++) {
        XgModuleSummary expected_module;
        if (!xg_module_summary_from_module_spec(&expected_module, i + 1, ordered_specs[i]) ||
            !xg_module_summary_identity_matches(&package.modules[i], &expected_module))
            goto done;
    }
    if (!xg_build_key_from_ordered_module_specs(&expected_key, ordered_specs, package.nmodules,
                                                profile, 0))
        goto done;
    expected_request = xg_evidence_cache_request_key_from_build_key(
        &expected_key, XG_EVIDENCE_CACHE_GLOBAL_EVIDENCE);
    if (!xg_evidence_cache_request_key_matches(&info.request_key, &expected_request))
        goto done;
    modules = (XgModuleSummary *) xr_malloc((size_t) package.nmodules * sizeof(*modules));
    if (!modules)
        goto done;
    memcpy(modules, package.modules, (size_t) package.nmodules * sizeof(*modules));
    *out_modules = modules;
    *out_module_count = package.nmodules;
    modules = NULL;
    ok = true;

done:
    xr_free(modules);
    xr_free(ordered_specs);
    xr_free(closure);
    xg_global_evidence_free(&package);
    return ok;
}

static bool
xaot_discovered_package_payload_overlaps_seen(const XaotDiscoveredPackagePayload *items,
                                              uint32_t count,
                                              const XaotDiscoveredPackagePayload *candidate) {
    for (uint32_t i = 0; i < count; i++) {
        for (uint32_t j = 0; candidate && j < candidate->module_count; j++) {
            for (uint32_t k = 0; k < items[i].module_count; k++) {
                if (xg_module_summary_identity_matches(&items[i].modules[k],
                                                       &candidate->modules[j]))
                    return true;
            }
        }
    }
    return false;
}

static bool xaot_collect_imported_package_payloads_from_cache(
    const char *cache_dir, const XrModuleGraph *graph, uint32_t profile,
    XaotDiscoveredPackagePayload **out_items, uint32_t *out_count) {
    char phase_dir[PATH_MAX];
    XrDirIter *it;
    XrDirEntry entry;
    XaotDiscoveredPackagePayload *items = NULL;
    uint32_t count = 0;
    uint32_t cap = 0;
    if (out_items)
        *out_items = NULL;
    if (out_count)
        *out_count = 0;
    if (!cache_dir || !cache_dir[0] || !graph || !out_items || !out_count)
        return true;
    if (!xaot_evidence_cache_phase_dir(cache_dir, XG_EVIDENCE_CACHE_GLOBAL_EVIDENCE, phase_dir,
                                       sizeof(phase_dir)))
        return false;
    it = xr_dir_open(phase_dir);
    if (!it)
        return true;
    while (xr_dir_next(it, &entry)) {
        char payload_path[PATH_MAX];
        char *payload;
        XgModuleSummary *modules = NULL;
        uint32_t module_count = 0;
        size_t size = 0;
        int n;
        if (entry.is_dir || !xaot_str_has_suffix(entry.name, ".xgpayload"))
            continue;
        n = snprintf(payload_path, sizeof(payload_path), "%s/%s", phase_dir, entry.name);
        if (n < 0 || (size_t) n >= sizeof(payload_path))
            continue;
        payload = xr_file_read_all(payload_path, "rb", &size);
        if (!payload)
            continue;
        (void) size;
        if (!xaot_payload_matches_package_graph(payload, graph, profile, &modules, &module_count)) {
            xr_free(payload);
            continue;
        }
        if (!xaot_reserve_discovered_package_payloads(&items, &cap, count + 1)) {
            xr_free(modules);
            xr_free(payload);
            xr_dir_close(it);
            xaot_free_discovered_package_payloads(items, count);
            return false;
        }
        items[count].path = xr_strdup(payload_path);
        if (!items[count].path) {
            xr_free(modules);
            xr_free(payload);
            xr_dir_close(it);
            xaot_free_discovered_package_payloads(items, count);
            return false;
        }
        items[count].payload = payload;
        items[count].modules = modules;
        items[count].module_count = module_count;
        count++;
    }
    xr_dir_close(it);
    xaot_sort_discovered_package_payloads(items, count);
    if (count > 1) {
        uint32_t write = 0;
        for (uint32_t i = 0; i < count; i++) {
            if (xaot_discovered_package_payload_overlaps_seen(items, write, &items[i])) {
                xaot_free_discovered_package_payload_fields(&items[i]);
                continue;
            }
            if (write != i)
                items[write] = items[i];
            write++;
        }
        count = write;
    }
    *out_items = items;
    *out_count = count;
    return true;
}

#ifdef _WIN32
#include <stdlib.h>
#define realpath(path, resolved) _fullpath((resolved), (path), PATH_MAX)
#endif

#include "xaot_stdlib_generated.inc.c"

/* Create a full-runtime isolate for AOT compilation.
 * Equivalent to XR_ISOLATE_PROFILE_RUN without depending on the
 * isolate-profile factory in src/api/. */
static XrVMRuntime *create_isolate(void) {
    XrVMConfig params;
    xray_vm_config_init(&params);
    return xray_vm_new_full(&params);
}

/* ========== Module Name Helpers ========== */

/* Derive a stable C-safe module prefix.  Basenames collide for package layouts
 * like entry main.xr importing package src/main.xr, so suffix the readable
 * basename with the canonical module identity hash. */
static char *derive_module_name(const XrModuleSpec *spec) {
    const char *path;
    const char *identity;
    uint64_t identity_hash;
    char suffix[18];
    char *name;
    XR_DCHECK(spec != NULL, "derive_module_name: NULL spec");
    path = spec && spec->source_path ? spec->source_path : "module";
    identity = spec && spec->canonical ? spec->canonical : path;
    identity_hash = identity ? xr_hash_bytes64(identity, strlen(identity)) : 0;
    snprintf(suffix, sizeof(suffix), "_%016llx", (unsigned long long) identity_hash);
    const char *base = strrchr(path, '/');
    const char *backslash = strrchr(path, '\\');
    if (backslash && (!base || backslash > base))
        base = backslash;
    base = base ? base + 1 : path;
    size_t len = strlen(base);
    if (len > 3 && strcmp(base + len - 3, ".xr") == 0)
        len -= 3;
    name = (char *) xr_malloc(len + strlen(suffix) + 1);
    if (!name)
        return NULL;
    for (size_t i = 0; i < len; i++)
        name[i] = (base[i] == '-' || base[i] == '.') ? '_' : base[i];
    memcpy(name + len, suffix, strlen(suffix) + 1);
    return name;
}

/* Graph-based import resolution: delegate to shared xi_import_resolve utility */

/* ========== Feature Inference ========== */

/* Map import module name to XaotStdlibSet flag.
 * Import names are bare identifiers (e.g. "math", "crypto");
 * relative paths (starting with "./") are user modules, not stdlib.
 * Json is a builtin type and not an import module. */
static XaotStdlibSet stdlib_flag_for_import(const char *name) {
    if (!name || name[0] == '.')
        return 0;

    struct {
        const char *name;
        XaotStdlibSet flag;
    } table[] = {
        {"regex", XAOT_STDLIB_REGEX},   {"math", XAOT_STDLIB_MATH},
        {"time", XAOT_STDLIB_TIME},     {"datetime", XAOT_STDLIB_DATETIME},
        {"path", XAOT_STDLIB_PATH},     {"io", XAOT_STDLIB_IO},
        {"os", XAOT_STDLIB_OS},         {"net", XAOT_STDLIB_NET},
        {"http", XAOT_STDLIB_HTTP},     {"crypto", XAOT_STDLIB_CRYPTO},
        {"base64", XAOT_STDLIB_BASE64}, {"encoding", XAOT_STDLIB_ENCODING},
        {"url", XAOT_STDLIB_URL},       {"csv", XAOT_STDLIB_CSV},
        {"toml", XAOT_STDLIB_TOML},     {"yaml", XAOT_STDLIB_YAML},
        {"xml", XAOT_STDLIB_XML},       {"compress", XAOT_STDLIB_COMPRESS},
        {"log", XAOT_STDLIB_LOG},
    };
    for (int i = 0; i < (int) (sizeof(table) / sizeof(table[0])); i++) {
        if (strcmp(name, table[i].name) == 0)
            return table[i].flag;
    }
    return 0;
}

static void features_apply_generated_stdlib_caps(XaotFeatureSet *fs, const char *symbol) {
    uint32_t caps = xaot_stdlib_generated_caps_for_symbol(symbol);
    if (!fs || caps == 0)
        return;
    if (caps & XAOT_STDLIB_CAP_CORO)
        fs->need_coro = true;
    if (caps & XAOT_STDLIB_CAP_TIMER)
        fs->need_timer = true;
    if (caps & XAOT_STDLIB_CAP_CHANNEL)
        fs->need_channel = true;
    if (caps & XAOT_STDLIB_CAP_NETPOLL)
        fs->need_netpoll = true;
    if (caps & XAOT_STDLIB_CAP_TASK)
        fs->need_task = true;
    if (caps & XAOT_STDLIB_CAP_WORK_QUEUE)
        fs->need_work_queue = true;
    if (caps & XAOT_STDLIB_CAP_RESULT_GROUP)
        fs->need_result_group = true;
    if (caps & XAOT_STDLIB_CAP_OBJECTS)
        fs->need_objects = true;
    if (caps & XAOT_STDLIB_CAP_DEEP_COPY)
        fs->need_deep_copy = true;
    if (caps & XAOT_STDLIB_CAP_EXCEPTION)
        fs->need_exception = true;
    if (caps & XAOT_STDLIB_CAP_STACKTRACE)
        fs->need_stacktrace = true;
    if (caps & XAOT_STDLIB_CAP_INSTANCEOF)
        fs->need_instanceof = true;
    if (caps & XAOT_STDLIB_CAP_SCOPE)
        fs->need_scope = true;
}

static void features_add_stdlib_symbol(XaotFeatureSet *fs, const char *symbol) {
    if (!fs || !symbol || !symbol[0] || strlen(symbol) >= XAOT_STDLIB_SYMBOL_NAME_MAX)
        return;
    features_apply_generated_stdlib_caps(fs, symbol);
    for (uint16_t i = 0; i < fs->n_stdlib_symbols; i++) {
        if (strcmp(fs->stdlib_symbols[i], symbol) == 0)
            return;
    }
    if (fs->n_stdlib_symbols >= XAOT_MAX_STDLIB_SYMBOLS)
        return;
    memcpy(fs->stdlib_symbols[fs->n_stdlib_symbols], symbol, strlen(symbol) + 1);
    fs->n_stdlib_symbols++;
}

static void features_add_stdlib_module(XaotFeatureSet *fs, const char *module) {
    XaotStdlibSet flag;
    if (!fs || !module || !module[0])
        return;
    flag = stdlib_flag_for_import(module);
    if (flag)
        fs->stdlib |= flag;
}

static void features_apply_stdlib_symbol(XaotFeatureSet *fs, const char *symbol) {
    const char *dot;
    char module[XAOT_STDLIB_SYMBOL_NAME_MAX];
    size_t module_len;
    if (!fs || !symbol || !symbol[0])
        return;
    dot = strchr(symbol, '.');
    if (dot && dot != symbol) {
        module_len = (size_t) (dot - symbol);
        if (module_len < sizeof(module)) {
            memcpy(module, symbol, module_len);
            module[module_len] = '\0';
            features_add_stdlib_module(fs, module);
        }
    }
    features_add_stdlib_symbol(fs, symbol);
}

static void features_add_extern_dylib(XaotFeatureSet *fs, const char *dylib) {
    if (!fs || !dylib || !dylib[0] || strlen(dylib) >= XAOT_EXTERN_DYLIB_NAME_MAX)
        return;
    for (uint16_t i = 0; i < fs->n_extern_dylibs; i++) {
        if (strcmp(fs->extern_dylibs[i], dylib) == 0)
            return;
    }
    if (fs->n_extern_dylibs >= XAOT_MAX_EXTERN_DYLIBS)
        return;
    memcpy(fs->extern_dylibs[fs->n_extern_dylibs], dylib, strlen(dylib) + 1);
    fs->n_extern_dylibs++;
}

static void features_apply_capability_plan(XaotFeatureSet *fs, uint32_t capability) {
    if (!fs || capability == 0)
        return;
    switch (capability) {
        case XG_CAP_COROUTINE:
            fs->need_coro = true;
            break;
        case XG_CAP_CHANNEL:
            fs->need_channel = true;
            break;
        case XG_CAP_EXCEPTION:
            fs->need_exception = true;
            break;
        case XG_CAP_OBJECTS:
            fs->need_objects = true;
            break;
        case XG_CAP_DEEP_COPY:
            fs->need_deep_copy = true;
            break;
        case XG_CAP_INSTANCEOF:
            /* AOT `is` over generated type ids is header-only. Keep the evidence row
             * for audits, but do not link the runtime type archive. */
            break;
        case XG_CAP_SYS_THREAD:
            fs->need_sys_thread = true;
            break;
        case XG_CAP_SCOPE:
            fs->need_scope = true;
            break;
        case XG_CAP_TIMER:
            fs->need_timer = true;
            break;
        case XG_CAP_NETPOLL:
            fs->need_netpoll = true;
            break;
        case XG_CAP_TASK:
            fs->need_task = true;
            break;
        case XG_CAP_ATOMIC:
            fs->need_atomic = true;
            break;
        case XG_CAP_WORK_QUEUE:
            fs->need_work_queue = true;
            break;
        case XG_CAP_RESULT_GROUP:
            fs->need_result_group = true;
            break;
        case XG_CAP_COUNTDOWN_LATCH:
            fs->need_countdown_latch = true;
            break;
        case XG_CAP_SEMAPHORE:
            fs->need_semaphore = true;
            break;
        case XG_CAP_EVENT_COUNT:
            fs->need_event_count = true;
            break;
        case XG_CAP_GENERATOR:
            fs->need_generator = true;
            fs->need_coro = true;
            break;
        case XG_CAP_STACKTRACE:
            fs->need_stacktrace = true;
            break;
        case XG_CAP_PARALLEL:
            fs->need_parallel = true;
            break;
        default:
            break;
    }
}

static void features_apply_capability_plans(XaotFeatureSet *fs, const XaotBundle *bundle) {
    if (!fs || !bundle)
        return;
    for (uint32_t i = 0; i < bundle->ncapability_plans; i++)
        features_apply_capability_plan(fs, bundle->capability_plans[i].capability);
}

/* The parallel requirement is read from the prepared IR, which is also where
 * the entry plan reads it; sharing one scan keeps the link feature set and the
 * C emitter's runtime-bridge decision from disagreeing. */
static void features_apply_ir_intrinsics(XaotFeatureSet *fs, const XaotBundle *bundle) {
    if (!fs || !bundle)
        return;
    if (xaot_bundle_uses_parallel_intrinsic(bundle))
        fs->need_parallel = true;
}

static void features_apply_link_dependency_plans(XaotFeatureSet *fs, const XaotBundle *bundle) {
    if (!fs || !bundle)
        return;
    for (uint32_t i = 0; i < bundle->nlink_dependency_plans; i++) {
        const XaotLinkDependencyPlan *plan = &bundle->link_dependency_plans[i];
        if (plan->kind == XG_LINK_DEP_STDLIB_MODULE)
            features_add_stdlib_module(fs, plan->name);
        else if (plan->kind == XG_LINK_DEP_STDLIB_SYMBOL)
            features_apply_stdlib_symbol(fs, plan->name);
    }
}

/* Foreign link inputs are derived from the same canonical declarations that
 * Cgen renders.  Global dependency summaries remain the source for stdlib
 * closure only; keeping extern dylibs here would recreate an independent,
 * potentially stale declaration path. */
static void features_apply_extern_decls(XaotFeatureSet *fs, const XaotBundle *bundle) {
    if (!fs || !bundle)
        return;
    for (uint32_t i = 0; i < bundle->nextern_decls; i++) {
        const XaotExternDecl *decl = &bundle->extern_decls[i];
        if (decl->library && decl->library[0])
            features_add_extern_dylib(fs, decl->library);
    }
}

static bool reject_profile_capability_plans(const XaotBundle *bundle) {
    if (!bundle)
        return false;
    for (uint32_t i = 0; i < bundle->ncapability_plans; i++) {
        const XaotCapabilityPlan *plan = &bundle->capability_plans[i];
        XaotBackendContractIssue issue = XAOT_BACKEND_CONTRACT_OK;
        if (xaot_backend_contract_capability_plan_allowed(plan, &issue))
            continue;
        if (issue != XAOT_BACKEND_CONTRACT_CAPABILITY_PROFILE_REJECTED) {
            fprintf(stderr, "Error: backend capability plan contract failed: %s\n",
                    xaot_backend_contract_issue_name(issue));
            return false;
        }
        fprintf(stderr, "Error: %s profile rejects runtime capability '%s'\n",
                xg_build_profile_name(bundle->global_evidence_plan.profile),
                xg_capability_name(plan->capability));
        return false;
    }
    return true;
}

static bool reject_profile_metadata_plans(const XaotBundle *bundle) {
    if (!bundle)
        return false;
    for (uint32_t i = 0; i < bundle->nmetadata_plans; i++) {
        const XaotMetadataReachabilityPlan *plan = &bundle->metadata_plans[i];
        XaotBackendContractIssue issue = XAOT_BACKEND_CONTRACT_OK;
        if (xaot_backend_contract_metadata_plan_allowed(plan, &issue))
            continue;
        if (issue != XAOT_BACKEND_CONTRACT_METADATA_PROFILE_REJECTED) {
            fprintf(stderr, "Error: backend metadata plan contract failed: %s\n",
                    xaot_backend_contract_issue_name(issue));
            return false;
        }
        fprintf(stderr, "Error: %s profile rejects metadata '%s'\n",
                xg_build_profile_name(bundle->global_evidence_plan.profile),
                xg_metadata_name(plan->metadata));
        return false;
    }
    return true;
}

static bool reject_profile_static_data_plans(const XaotBundle *bundle) {
    if (!bundle)
        return false;
    for (uint32_t i = 0; i < bundle->nstatic_data_plans; i++) {
        const XaotStaticDataPlan *plan = &bundle->static_data_plans[i];
        XaotBackendContractIssue issue = XAOT_BACKEND_CONTRACT_OK;
        if (xaot_backend_contract_static_data_plan_allowed(plan, &issue))
            continue;
        if (issue != XAOT_BACKEND_CONTRACT_STATIC_DATA_PROFILE_REJECTED) {
            fprintf(stderr, "Error: backend static-data plan contract failed: %s\n",
                    xaot_backend_contract_issue_name(issue));
            return false;
        }
        fprintf(stderr, "Error: %s profile rejects static data '%s'\n",
                xg_build_profile_name(bundle->global_evidence_plan.profile),
                xg_static_data_name(plan->static_data));
        return false;
    }
    return true;
}

static bool add_stdlib_manifest_entries(XaotLinkManifest *manifest, XaotStdlibSet stdlib) {
    struct {
        XaotStdlibSet flag;
        const char *name;
    } table[] = {
        {XAOT_STDLIB_JSON, "json"}, {XAOT_STDLIB_NET, "net"},   {XAOT_STDLIB_HTTP, "http"},
        {XAOT_STDLIB_CSV, "csv"},   {XAOT_STDLIB_TOML, "toml"}, {XAOT_STDLIB_YAML, "yaml"},
        {XAOT_STDLIB_XML, "xml"},
    };

    for (uint32_t i = 0; i < (uint32_t) (sizeof(table) / sizeof(table[0])); i++) {
        if ((stdlib & table[i].flag) &&
            !xaot_link_manifest_add_unique(manifest, XAOT_LINK_STDLIB_OBJECT, table[i].name))
            return false;
    }
    return true;
}

static bool add_stdlib_symbol_manifest_entries(XaotLinkManifest *manifest,
                                               const XaotFeatureSet *features,
                                               bool freestanding_profile) {
    for (uint16_t i = 0; i < features->n_stdlib_symbols; i++) {
        if (freestanding_profile &&
            xaot_stdlib_generated_symbol_is_freestanding_header_only(features->stdlib_symbols[i]))
            continue;
        if (!xaot_link_manifest_add_unique(manifest, XAOT_LINK_STDLIB_SYMBOL,
                                           features->stdlib_symbols[i]))
            return false;
    }
    return true;
}

#if defined(XR_OS_LINUX)
static bool stdlib_symbol_is_sys_dylib(const char *symbol) {
    return symbol &&
           (strcmp(symbol, "sys.__dylibOpen") == 0 || strcmp(symbol, "sys.__dylibSymbol") == 0 ||
            strcmp(symbol, "sys.__dylibClose") == 0 || strcmp(symbol, "sys.__dylibLastError") == 0);
}
#endif

static bool add_stdlib_platform_system_lib_manifest_entries(XaotLinkManifest *manifest,
                                                            const XaotFeatureSet *features) {
#if defined(XR_OS_LINUX)
    for (uint16_t i = 0; features && i < features->n_stdlib_symbols; i++) {
        if (stdlib_symbol_is_sys_dylib(features->stdlib_symbols[i])) {
            return xaot_link_manifest_add_unique(manifest, XAOT_LINK_SYSTEM_LIB, "dl");
        }
    }
#else
    (void) manifest;
    (void) features;
#endif
    return true;
}

static bool xaot_target_is_windows(const XaotTarget *target) {
    if (target && target->os && strcmp(target->os, "windows") == 0)
        return true;
#if defined(XR_OS_WINDOWS)
    return target && target->os && strcmp(target->os, "native") == 0;
#else
    return false;
#endif
}

static bool add_target_platform_system_lib_manifest_entries(XaotLinkManifest *manifest,
                                                            const XaotTarget *target) {
    if (xaot_target_is_windows(target)) {
        return xaot_link_manifest_add_unique(manifest, XAOT_LINK_SYSTEM_LIB, "ws2_32") &&
               xaot_link_manifest_add_unique(manifest, XAOT_LINK_SYSTEM_LIB, "bcrypt") &&
               xaot_link_manifest_add_unique(manifest, XAOT_LINK_SYSTEM_LIB,
                                             "api-ms-win-core-synch-l1-2-0");
    }
    return true;
}

static bool extern_dylib_is_link_name(const char *dylib) {
    return dylib && dylib[0] && strchr(dylib, '/') == NULL && strstr(dylib, ".so") == NULL &&
           strstr(dylib, ".dylib") == NULL && strstr(dylib, ".dll") == NULL;
}

static bool add_extern_dylib_manifest_entries(XaotLinkManifest *manifest,
                                              const XaotFeatureSet *features) {
    for (uint16_t i = 0; features && i < features->n_extern_dylibs; i++) {
        const char *dylib = features->extern_dylibs[i];
        if (!dylib || !dylib[0])
            continue;
        if (extern_dylib_is_link_name(dylib)) {
            if (!xaot_link_manifest_add_unique(manifest, XAOT_LINK_SYSTEM_LIB, dylib))
                return false;
        } else if (!xaot_link_manifest_add_unique(manifest, XAOT_LINK_NATIVE_INPUT, dylib)) {
            return false;
        }
    }
    return true;
}

static bool add_stdlib_core_object_manifest_entries(XaotLinkManifest *manifest,
                                                    const XaotFeatureSet *features) {
    for (uint16_t i = 0; i < features->n_stdlib_symbols; i++) {
        const char *symbol = features->stdlib_symbols[i];
        const char *generated_object = xaot_stdlib_generated_object_for_symbol(symbol);
        if (generated_object) {
            if (!xaot_link_manifest_add_unique(manifest, XAOT_LINK_STDLIB_OBJECT, generated_object))
                return false;
            continue;
        }
    }
    return true;
}

static bool add_stdlib_generated_define_manifest_entries(XaotLinkManifest *manifest,
                                                         const XaotFeatureSet *features) {
    for (uint16_t i = 0; i < features->n_stdlib_symbols; i++) {
        const char *define = xaot_stdlib_generated_define_for_symbol(features->stdlib_symbols[i]);
        if (define && !xaot_link_manifest_add_unique(manifest, XAOT_LINK_DEFINE, define))
            return false;
    }
    return true;
}

static bool add_runtime_cap(XaotLinkManifest *manifest, const char *cap) {
    return xaot_link_manifest_add_unique(manifest, XAOT_LINK_RUNTIME_CAP, cap);
}

static bool add_aot_runtime_archive(XaotLinkManifest *manifest) {
    return xaot_link_manifest_add_unique(manifest, XAOT_LINK_RUNTIME_OBJECT, "xray_rt_coro");
}

static bool add_runtime_cap_manifest_entries(const XaotFeatureSet *features,
                                             const XaotTarget *target, XaotLinkManifest *manifest) {
    bool needs_aot_runtime = false;

    if (features->need_coro || features->need_scope) {
        if (!add_runtime_cap(manifest, "coro"))
            return false;
        if (features->need_generator &&
            !xaot_link_manifest_add_unique(manifest, XAOT_LINK_DEFINE, "XRT_ENABLE_GENERATORS"))
            return false;
        needs_aot_runtime = true;
    }
    if (features->need_sys_thread) {
        /* Thread handle destructor (xrt_coll.h dispatch) calls the extern
         * xr_thread_detach; only enable it when threads can actually be
         * spawned so thread-free binaries don't need the OS thread object. */
        if (!add_runtime_cap(manifest, "sys_thread") ||
            !xaot_link_manifest_add_unique(manifest, XAOT_LINK_DEFINE, "XRT_ENABLE_SYS_THREAD"))
            return false;
        needs_aot_runtime = true;
    }
    if (features->need_parallel) {
        if (!add_runtime_cap(manifest, "parallel"))
            return false;
        needs_aot_runtime = true;
    }
    if (features->need_scope) {
        if (!add_runtime_cap(manifest, "transfer"))
            return false;
        needs_aot_runtime = true;
    }
    if (features->need_channel) {
        if (!add_runtime_cap(manifest, "coro") || !add_runtime_cap(manifest, "channel"))
            return false;
        needs_aot_runtime = true;
    }
    if (features->need_timer) {
        if (!add_runtime_cap(manifest, "coro") || !add_runtime_cap(manifest, "timer"))
            return false;
        needs_aot_runtime = true;
    }
    if (features->need_netpoll) {
        if (!add_runtime_cap(manifest, "coro") || !add_runtime_cap(manifest, "netpoll"))
            return false;
        needs_aot_runtime = true;
    }
    if (features->need_task) {
        if (!add_runtime_cap(manifest, "task"))
            return false;
        needs_aot_runtime = true;
    }
    if (features->need_atomic) {
        if (!add_runtime_cap(manifest, "atomic"))
            return false;
        needs_aot_runtime = true;
    }
    if (features->need_work_queue) {
        if (!add_runtime_cap(manifest, "work_queue"))
            return false;
        needs_aot_runtime = true;
    }
    if (features->need_result_group) {
        if (!add_runtime_cap(manifest, "result_group"))
            return false;
        needs_aot_runtime = true;
    }
    if (features->need_countdown_latch) {
        if (!add_runtime_cap(manifest, "countdown_latch"))
            return false;
        needs_aot_runtime = true;
    }
    if (features->need_semaphore) {
        if (!add_runtime_cap(manifest, "semaphore"))
            return false;
        needs_aot_runtime = true;
    }
    if (features->need_event_count) {
        if (!add_runtime_cap(manifest, "event_count"))
            return false;
        needs_aot_runtime = true;
    }
    if (features->need_deep_copy) {
        if (!add_runtime_cap(manifest, "deep_copy"))
            return false;
        needs_aot_runtime = true;
    }
    if (features->need_instanceof) {
        if (!add_runtime_cap(manifest, "type"))
            return false;
        needs_aot_runtime = true;
    }
    if (features->need_stacktrace) {
        if (!add_runtime_cap(manifest, "stacktrace"))
            return false;
        needs_aot_runtime = true;
    }
    if (features->need_objects && needs_aot_runtime) {
        if (!add_runtime_cap(manifest, "objects"))
            return false;
    }
    if (needs_aot_runtime && !add_aot_runtime_archive(manifest))
        return false;
    if (needs_aot_runtime && !xaot_target_is_windows(target) &&
        !xaot_link_manifest_add_unique(manifest, XAOT_LINK_SYSTEM_LIB, "pthread"))
        return false;
    return true;
}

static bool xaot_fast_test_build_enabled(void) {
    const char *flag = getenv("XRAY_AOT_FAST_TEST_BUILD");
    return flag && flag[0] && strcmp(flag, "0") != 0;
}

static bool xaot_fast_test_can_skip_size_link_flags(const XaotFeatureSet *features) {
    (void) features;
    return false;
}

static bool build_link_manifest(const XaotFeatureSet *features, const XaotTarget *target,
                                XaotLinkManifest *manifest, bool freestanding_profile,
                                const XaotTargetCapabilityProvider *provider,
                                const XrEntryPlan *entry_plan) {
    bool ok = false;
    bool fast_test;

    if (!features || !target || !manifest)
        return false;
    if (!xaot_link_manifest_init(manifest, target))
        return false;

    if (!xaot_link_manifest_add_unique(manifest, XAOT_LINK_GENERATED_C_FILE, "<aot-generated-c>"))
        goto done;
    if (!xaot_link_manifest_add_unique(manifest, XAOT_LINK_DEFINE, "XRT_IMPL"))
        goto done;
    if (!xaot_target_is_windows(target) &&
        !xaot_link_manifest_add_unique(manifest, XAOT_LINK_SYSTEM_LIB, "m"))
        goto done;
    fast_test = xaot_fast_test_build_enabled();
    manifest->compile.suppress_warnings = fast_test;
    if (!fast_test || !xaot_fast_test_can_skip_size_link_flags(features)) {
        manifest->compile.function_sections = true;
        manifest->compile.data_sections = true;
        manifest->link.dead_strip = true;
    }

    if (provider && provider->abi_version != 0) {
        char provider_abi[64];
        char provider_caps[64];
        char provider_hooks[64];
        char provider_target_hash[96];
        snprintf(provider_abi, sizeof(provider_abi), "XRAY_PROVIDER_ABI=%u", provider->abi_version);
        snprintf(provider_caps, sizeof(provider_caps), "XRAY_PROVIDER_REQUIRED_CAPS=0x%x",
                 entry_plan ? entry_plan->required_capability_bits : 0);
        snprintf(provider_hooks, sizeof(provider_hooks), "XRAY_PROVIDER_REQUIRED_HOOKS=0x%x",
                 entry_plan ? entry_plan->provider_hook_bits : 0);
        snprintf(provider_target_hash, sizeof(provider_target_hash),
                 "XRAY_PROVIDER_TARGET_METADATA_HASH=0x%llxULL",
                 (unsigned long long) provider->target_metadata_hash);
        if (!xaot_link_manifest_add_unique(manifest, XAOT_LINK_DEFINE,
                                           "XRAY_TARGET_RUNTIME_PROVIDER=1") ||
            !xaot_link_manifest_add_unique(manifest, XAOT_LINK_DEFINE, provider_abi) ||
            !xaot_link_manifest_add_unique(manifest, XAOT_LINK_DEFINE, provider_caps) ||
            !xaot_link_manifest_add_unique(manifest, XAOT_LINK_DEFINE, provider_hooks) ||
            !xaot_link_manifest_add_unique(manifest, XAOT_LINK_DEFINE, provider_target_hash))
            goto done;
    } else if (!add_runtime_cap_manifest_entries(features, target, manifest)) {
        goto done;
    }

    if (!add_stdlib_manifest_entries(manifest, features->stdlib))
        goto done;
    if (!add_extern_dylib_manifest_entries(manifest, features))
        goto done;
    if (!add_target_platform_system_lib_manifest_entries(manifest, target))
        goto done;
    if (!add_stdlib_platform_system_lib_manifest_entries(manifest, features))
        goto done;
    if (!add_stdlib_core_object_manifest_entries(manifest, features))
        goto done;
    if (!add_stdlib_generated_define_manifest_entries(manifest, features))
        goto done;
    if (!add_stdlib_symbol_manifest_entries(manifest, features, freestanding_profile))
        goto done;
    ok = true;

done:
    if (!ok)
        xaot_link_manifest_free(manifest);
    return ok;
}

static int report_analyzer_diagnostics(XaAnalyzer *analyzer, const char *fallback_file) {
    int diag_count = 0;
    int error_count = 0;
    XaDiagnostic *diags = xa_analyzer_get_diagnostics(analyzer, &diag_count);
    (void) diag_count;
    for (XaDiagnostic *d = diags; d; d = d->next) {
        if (d->severity == XR_DIAG_SEV_ERROR)
            error_count++;
        const char *sev = "error";
        if (d->severity == XR_DIAG_SEV_WARNING)
            sev = "warning";
        else if (d->severity == XR_DIAG_SEV_INFO)
            sev = "info";
        else if (d->severity == XR_DIAG_SEV_HINT)
            sev = "hint";
        const char *file =
            d->location.file ? d->location.file : (fallback_file ? fallback_file : "?");
        fprintf(stderr, "%s:%u:%u: %s: %s\n", file, (unsigned) d->location.line,
                (unsigned) d->location.column, sev, d->message ? d->message : "");
    }
    return error_count;
}

static bool xaot_c90_build_options_supported(const XaotBuildOptions *options) {
    const XaotTarget *target = options ? options->target : NULL;
    if (!options || options->c_dialect != XI_CGEN_C_DIALECT_C90)
        return true;
    return options->profile == XAOT_BUILD_PROFILE_FREESTANDING && !options->emit_program_main &&
           target && target->pointer_bits == 64 && target->os &&
           (strcmp(target->os, "linux") == 0 || strcmp(target->os, "darwin") == 0) &&
           target->simd_mode == XAOT_SIMD_SCALAR && target->simd_features == 0;
}

static bool xaot_c90_link_manifest_supported(const XaotLinkManifest *manifest) {
    return manifest && manifest->n_runtime_caps == 0 && manifest->n_runtime_objects == 0 &&
           manifest->n_stdlib_objects == 0 && manifest->n_stdlib_symbols == 0 &&
           manifest->n_native_inputs == 0;
}

XR_FUNC int xaot_build(const char *input_path, const XaotBuildOptions *options,
                       XaotBuildResult *result) {
    bool emit_plan_dump;
    bool emit_program_main;
    bool emit_global_evidence_dump;
    bool emit_local_evidence_dump;
    bool quiet;
    const char *evidence_cache_dir;
    bool evidence_cache_rebuild;
    bool evidence_cache_verbose;
    XaotBuildProfile profile;
    uint32_t xg_profile;
    uint64_t imported_summary_hash = 0;
    const char *const *imported_summary_payloads;
    uint32_t imported_summary_payload_count;
    XaotDiscoveredPackagePayload *discovered_imported_summary_payloads = NULL;
    uint32_t discovered_imported_summary_payload_count = 0;
    const char **discovered_imported_summary_payload_view = NULL;
    XgModuleSummary *imported_summary_modules = NULL;
    uint32_t imported_summary_module_count = 0;
    XiCgenTypeNameProfile type_name_profile;
    int nmodules = 0;
    int entry_index = -1;
    bool has_explicit_vector_ops = false;
    char **paths = NULL;
    char **mod_names = NULL;
    AstNode **mono_roots = NULL;
    XR_DCHECK(input_path != NULL, "xaot_build: NULL input_path");
    XR_DCHECK(options != NULL, "xaot_build: NULL options");
    XR_DCHECK(result != NULL, "xaot_build: NULL result");
    if (!input_path || !options || !options->target ||
        !xr_target_data_layout_validate(&options->target->data_layout) || !result)
        return 1;
    memset(result, 0, sizeof(*result));
    if (!xaot_c90_build_options_supported(options)) {
        fprintf(stderr,
                "Error: restricted C90 AOT requires a freestanding shared-library graph, an "
                "LP64 Linux/Darwin target, and scalar code generation\n");
        return 1;
    }
    emit_plan_dump = options->emit_plan_dump;
    emit_program_main = options->emit_program_main;
    emit_global_evidence_dump = options->emit_global_evidence_dump;
    emit_local_evidence_dump = options->emit_local_evidence_dump;
    quiet = options->quiet;
    evidence_cache_dir = options->evidence_cache_dir;
    evidence_cache_rebuild = options->evidence_cache_rebuild;
    evidence_cache_verbose = options->evidence_cache_verbose;
    profile = options->profile;
    xg_profile = profile == XAOT_BUILD_PROFILE_FREESTANDING ? XG_BUILD_FREESTANDING
                                                            : XG_BUILD_NATIVE_RELEASE;
    type_name_profile = options->type_name_profile;
    imported_summary_payloads = options->imported_summary_payloads;
    imported_summary_payload_count = options->imported_summary_payload_count;
    XgGlobalEvidence pre_mono_generic_evidence;
    bool pre_mono_generic_evidence_initialized = false;
    XgGlobalEvidence cached_global_evidence;
    bool cached_global_evidence_initialized = false;
    memset(&pre_mono_generic_evidence, 0, sizeof(pre_mono_generic_evidence));
    memset(&cached_global_evidence, 0, sizeof(cached_global_evidence));

    if (!quiet)
        printf("[xi-native] Building: %s\n", input_path);

    /* --- Build module graph (topo order, entry last) --- */
    XrVMRuntime *X = create_isolate();
    if (!X) {
        fprintf(stderr, "Error: failed to create isolate\n");
        return 1;
    }
    XrCompilerSession *session = xr_compiler_session_current_for_isolate(X);
    xr_compiler_session_set_native_package_plan(session, options->native_package_plan);
    if (!xr_compiler_session_set_target_data_layout(session, &options->target->data_layout)) {
        fprintf(stderr, "Error: failed to install target data layout in compiler session\n");
        xray_vm_delete(X);
        return 1;
    }

    xr_module_system_init_with_script(X, input_path);
    XrModuleRegistry *registry = xr_isolate_get_module_registry(X);
    XrModuleResolver *resolver = xr_module_registry_get_resolver(registry);
    XrModuleGraph *graph = xr_module_graph_new(session, resolver);
    if (!graph) {
        fprintf(stderr, "Error: failed to create module graph\n");
        xray_vm_delete(X);
        return 1;
    }

    char *build_err = NULL;
    if (xr_module_graph_build(graph, input_path, &build_err) != 0) {
        fprintf(stderr, "Error: module graph build failed: %s\n", build_err ? build_err : "?");
        xr_free(build_err);
        xr_module_graph_free(graph);
        xray_vm_delete(X);
        return 1;
    }
    xr_free(build_err);

    xr_module_graph_topological_sort(graph);
    if (graph->has_cycle) {
        fprintf(stderr, "Error: %s\n",
                graph->cycle_desc ? graph->cycle_desc : "circular dependency detected");
        xr_module_graph_free(graph);
        xray_vm_delete(X);
        return 1;
    }
    if (profile == XAOT_BUILD_PROFILE_FREESTANDING &&
        !xaot_reject_freestanding_stdlib_graph(graph)) {
        xr_module_graph_free(graph);
        xray_vm_delete(X);
        return 1;
    }
    nmodules = graph->topo_count;
    entry_index = -1;
    if (imported_summary_payload_count == 0 && evidence_cache_dir && evidence_cache_dir[0] &&
        !evidence_cache_rebuild) {
        if (!xaot_collect_imported_package_payloads_from_cache(
                evidence_cache_dir, graph, xg_profile, &discovered_imported_summary_payloads,
                &discovered_imported_summary_payload_count)) {
            fprintf(stderr, "Error: failed to discover imported package summary payloads\n");
            goto fail_free_graph;
        }
        if (discovered_imported_summary_payload_count > 0) {
            discovered_imported_summary_payload_view =
                (const char **) xr_calloc(discovered_imported_summary_payload_count,
                                          sizeof(*discovered_imported_summary_payload_view));
            if (!discovered_imported_summary_payload_view) {
                fprintf(stderr, "Error: failed to allocate imported package payload set\n");
                goto fail_free_graph;
            }
            for (uint32_t i = 0; i < discovered_imported_summary_payload_count; i++)
                discovered_imported_summary_payload_view[i] =
                    discovered_imported_summary_payloads[i].payload;
            imported_summary_payloads = discovered_imported_summary_payload_view;
            imported_summary_payload_count = discovered_imported_summary_payload_count;
            if (evidence_cache_verbose) {
                printf("[xi-native] evidence cache imported packages: discovered=%u\n",
                       imported_summary_payload_count);
            }
        }
    }
    if (imported_summary_payload_count > 0 &&
        !xg_imported_summary_hash_from_package_payloads(
            0, imported_summary_payloads, imported_summary_payload_count, &imported_summary_hash)) {
        fprintf(stderr, "Error: invalid imported package summary payload set\n");
        goto fail_free_graph;
    }
    if (imported_summary_payload_count > 0 &&
        !xaot_collect_imported_package_modules_from_payloads(
            imported_summary_payloads, imported_summary_payload_count, &imported_summary_modules,
            &imported_summary_module_count)) {
        fprintf(stderr, "Error: invalid imported package module identities\n");
        goto fail_free_graph;
    }
    if (evidence_cache_dir && evidence_cache_dir[0]) {
        XgBuildKey preproducer_key;
        if (xg_build_key_from_module_graph(&preproducer_key, graph, xg_profile,
                                           imported_summary_hash))
            xaot_probe_preproducer_evidence_cache(evidence_cache_dir, &preproducer_key,
                                                  evidence_cache_verbose, evidence_cache_rebuild,
                                                  &cached_global_evidence,
                                                  &cached_global_evidence_initialized);
    }

    /* Build parallel arrays for paths/names (graph topo order) */
    paths = (char **) xr_calloc(nmodules, sizeof(char *));
    mod_names = (char **) xr_calloc(nmodules, sizeof(char *));
    mono_roots = (AstNode **) xr_calloc(nmodules, sizeof(AstNode *));
    if (!paths || !mod_names || !mono_roots) {
        xr_free(paths);
        xr_free(mod_names);
        xr_free(mono_roots);
        xr_module_graph_free(graph);
        xray_vm_delete(X);
        return 1;
    }
    /* Resolve input_path to canonical form (handles symlinks like /tmp -> /private/tmp) */
    char real_input[PATH_MAX];
    if (!realpath(input_path, real_input))
        strncpy(real_input, input_path, PATH_MAX - 1);

    for (int ti = 0; ti < nmodules; ti++) {
        int idx = graph->topo_order[ti];
        XrModuleSpec *spec = &graph->specs[idx];
        paths[ti] = xr_strdup(spec->source_path);
        mod_names[ti] = derive_module_name(spec);
        mono_roots[ti] = (AstNode *) spec->ast;
        if (strcmp(spec->source_path, real_input) == 0)
            entry_index = ti;
    }
    /* Entry is the last module in topo order (all deps come first) */
    if (entry_index < 0)
        entry_index = nmodules - 1;

    if (!quiet && nmodules > 1) {
        printf("[xi-native] %d modules (topo order):\n", nmodules);
        for (int i = 0; i < nmodules; i++)
            printf("  [%d] %s%s\n", i, paths[i], i == entry_index ? " (entry)" : "");
    }

    /* --- Analyze all modules with shared analyzer (cross-module types) --- */
    XaAnalyzer *shared_analyzer = xa_analyzer_new(session);
    if (!shared_analyzer) {
        fprintf(stderr, "Error: failed to create shared analyzer\n");
        goto fail_free_graph;
    }
    xa_analyzer_set_build_profile(shared_analyzer, profile == XAOT_BUILD_PROFILE_FREESTANDING
                                                       ? XA_ANALYZER_BUILD_PROFILE_FREESTANDING
                                                       : XA_ANALYZER_BUILD_PROFILE_HOSTED);
    xa_analyzer_set_graph(shared_analyzer, graph);

    for (int ti = 0; ti < nmodules; ti++) {
        int idx = graph->topo_order[ti];
        XrModuleSpec *spec = &graph->specs[idx];
        if (!spec->ast || !spec->source_path)
            continue;
        xa_analyzer_analyze(shared_analyzer, spec->source_path, (XrAstNode *) spec->ast);
        int file_errors = report_analyzer_diagnostics(shared_analyzer, spec->source_path);
        if (file_errors > 0) {
            fprintf(stderr, "Error: semantic analysis failed for '%s'\n", spec->source_path);
            goto fail_free_analyzer;
        }
        XrHashMap *exports = NULL;
        if (!xa_analyzer_collect_export_symbols_checked(shared_analyzer, (XrAstNode *) spec->ast,
                                                        &exports)) {
            (void) report_analyzer_diagnostics(shared_analyzer, spec->source_path);
            fprintf(stderr, "Error: export metadata invalid for '%s'\n", spec->source_path);
            goto fail_free_analyzer;
        }
        spec->export_symbols = exports;
        xa_analyzer_clear_diagnostics(shared_analyzer);
    }

    if (cached_global_evidence_initialized) {
        if (evidence_cache_verbose)
            printf("[xi-native] evidence cache producer skip: pre_mono_generic_summary\n");
    } else {
        if (!xg_global_evidence_build_from_module_graph_with_imported_modules_and_analyzer(
                &pre_mono_generic_evidence, graph, xg_profile, imported_summary_hash,
                imported_summary_modules, imported_summary_module_count, shared_analyzer)) {
            fprintf(stderr, "Error: failed to build pre-monomorphization generic evidence\n");
            goto fail_free_analyzer;
        }
        pre_mono_generic_evidence_initialized = true;
    }

    /* Mirror the VM compiler entry: monomorphize after the first graph-aware
     * analysis, then analyze again so cloned declarations have concrete
     * signatures and value-struct layouts before Xi lowering. */
    for (int ti = 0; ti < nmodules; ti++) {
        int idx = graph->topo_order[ti];
        XrModuleSpec *spec = &graph->specs[idx];
        if (!spec->ast)
            continue;
        /* A budget failure must stop here rather than fall through to the
         * re-analysis below: the module's generic calls were left unexpanded,
         * so every later stage would report consequences of the first error. */
        if (!xa_mono_pass((AstNode *) spec->ast, mono_roots, nmodules, X, shared_analyzer)) {
            (void) report_analyzer_diagnostics(shared_analyzer, spec->source_path);
            fprintf(stderr, "Error: monomorphization budget exceeded for '%s'\n",
                    spec->source_path ? spec->source_path : "<unknown>");
            goto fail_free_analyzer;
        }
    }

    for (int ti = 0; ti < nmodules; ti++) {
        int idx = graph->topo_order[ti];
        XrModuleSpec *spec = &graph->specs[idx];
        if (!spec->ast || !spec->source_path)
            continue;
        xa_analyzer_analyze(shared_analyzer, spec->source_path, (XrAstNode *) spec->ast);
        int file_errors = report_analyzer_diagnostics(shared_analyzer, spec->source_path);
        if (file_errors > 0) {
            fprintf(stderr, "Error: post-monomorphization analysis failed for '%s'\n",
                    spec->source_path);
            goto fail_free_analyzer;
        }
        if (spec->export_symbols)
            xr_hashmap_free(spec->export_symbols);
        XrHashMap *exports = NULL;
        if (!xa_analyzer_collect_export_symbols_checked(shared_analyzer, (XrAstNode *) spec->ast,
                                                        &exports)) {
            (void) report_analyzer_diagnostics(shared_analyzer, spec->source_path);
            fprintf(stderr, "Error: post-monomorphization export metadata invalid for '%s'\n",
                    spec->source_path);
            goto fail_free_analyzer;
        }
        spec->export_symbols = exports;
        xa_analyzer_clear_diagnostics(shared_analyzer);
    }
    /* --- Compile all modules through Xi IR pipeline --- */
    XiPipelineConfig cfg = xi_pipeline_aot_config();
    /* AVX2/AVX-512 stay behind explicit function target boundaries for both
     * static and runtime-dispatch builds.  Besides keeping dispatch callers
     * baseline-safe, this prevents a static SIMD choice from changing the
     * optimizer's ISA for unrelated scalar functions in the same module. */
    cfg.preserve_wide_vector_boundaries = xaot_target_uses_x86_vector_islands(options->target);
    XiPipelineResult *pres_arr = (XiPipelineResult *) xr_calloc(nmodules, sizeof(XiPipelineResult));
    XiFunc **ir_funcs = (XiFunc **) xr_calloc(nmodules, sizeof(XiFunc *));
    XiModule **modules = (XiModule **) xr_calloc(nmodules, sizeof(XiModule *));
    XaotBundle aot_bundle;
    bool aot_bundle_initialized = false;
    XgGlobalEvidence global_evidence;
    bool global_evidence_initialized = false;
    XaotPrepareStats prepare_stats;
    char *plan_dump = NULL;
    char *global_evidence_dump = NULL;
    char *local_evidence_dump = NULL;
    char *residue_dump = NULL;
    char *evidence_cache_payloads[XG_EVIDENCE_CACHE_PHASE_COUNT];
    XgEvidenceCacheManifest evidence_cache_manifest;
    bool evidence_cache_manifest_valid = false;
    char *c_export_header = NULL;
    XaotLinkManifest link_manifest;
    bool link_manifest_initialized = false;
    memset(&aot_bundle, 0, sizeof(aot_bundle));
    memset(&global_evidence, 0, sizeof(global_evidence));
    memset(evidence_cache_payloads, 0, sizeof(evidence_cache_payloads));
    memset(&evidence_cache_manifest, 0, sizeof(evidence_cache_manifest));
    memset(&prepare_stats, 0, sizeof(prepare_stats));
    memset(&link_manifest, 0, sizeof(link_manifest));
    if (!pres_arr || !ir_funcs || !modules) {
        xr_free(pres_arr);
        xr_free(ir_funcs);
        xr_free(modules);
        xa_analyzer_set_graph(shared_analyzer, NULL);
        xa_analyzer_free(shared_analyzer);
        goto fail_free_graph;
    }

    /* Canonicalize before building final global evidence so lowering-time ids
     * are derived from the same AST shape that Xi lowering consumes. */
    for (int ti = 0; ti < nmodules; ti++) {
        int idx = graph->topo_order[ti];
        XrModuleSpec *spec = &graph->specs[idx];
        XrCompilerSessionScope canon_scope;
        bool has_canon_scope;
        if (!spec->ast || !spec->source_path)
            continue;
        has_canon_scope = spec->ast->type == AST_PROGRAM && spec->ast->as.program.arena &&
                          xr_compiler_session_push_arena(session, spec->ast->as.program.arena,
                                                         spec->source_path, &canon_scope);
        xr_canon_program((AstNode *) spec->ast, shared_analyzer, session);
        if (has_canon_scope)
            xr_compiler_session_pop_arena(&canon_scope);
    }

    if (cached_global_evidence_initialized) {
        global_evidence = cached_global_evidence;
        memset(&cached_global_evidence, 0, sizeof(cached_global_evidence));
        cached_global_evidence_initialized = false;
        global_evidence_initialized = true;
        if (evidence_cache_verbose)
            printf("[xi-native] evidence cache producer skip: global_evidence_summary\n");
    } else {
        if (!xg_global_evidence_build_from_module_graph_with_imported_modules_and_analyzer(
                &global_evidence, graph, xg_profile, imported_summary_hash,
                imported_summary_modules, imported_summary_module_count, shared_analyzer)) {
            fprintf(stderr, "Error: failed to build global evidence\n");
            goto fail_free_ir;
        }
        global_evidence_initialized = true;
        if (!xg_global_evidence_merge_generic_inst_roots(&global_evidence,
                                                         &pre_mono_generic_evidence)) {
            fprintf(stderr, "Error: failed to merge generic instantiation evidence\n");
            goto fail_free_ir;
        }
        xg_global_evidence_free(&pre_mono_generic_evidence);
        pre_mono_generic_evidence_initialized = false;
        if (imported_summary_payload_count > 0) {
            XgEvidencePackageImportReport import_report;
            if (!xg_global_evidence_import_package_payload_set(
                    &global_evidence, imported_summary_payloads, imported_summary_payload_count,
                    &import_report)) {
                fprintf(stderr, "Error: failed to import package summary payloads\n");
                goto fail_free_ir;
            }
            if (evidence_cache_verbose) {
                printf("[xi-native] evidence cache producer skip: imported_package_summary "
                       "payloads=%u modules=%u rows=%u\n",
                       import_report.payloads_imported, import_report.modules_remapped,
                       import_report.rows_imported);
            }
        }
    }
    xr_free(discovered_imported_summary_payload_view);
    discovered_imported_summary_payload_view = NULL;
    xaot_free_discovered_package_payloads(discovered_imported_summary_payloads,
                                          discovered_imported_summary_payload_count);
    discovered_imported_summary_payloads = NULL;
    discovered_imported_summary_payload_count = 0;
    xr_free(imported_summary_modules);
    imported_summary_modules = NULL;
    imported_summary_module_count = 0;
    evidence_cache_manifest = xg_global_evidence_cache_manifest(&global_evidence);
    evidence_cache_manifest_valid =
        evidence_cache_manifest.phase_mask == ((1u << XG_EVIDENCE_CACHE_PHASE_COUNT) - 1u);
    if (evidence_cache_manifest_valid) {
        static const uint32_t phases[] = {
            XG_EVIDENCE_CACHE_DECLARATIONS,
            XG_EVIDENCE_CACHE_SEMANTIC_GRAPH,
            XG_EVIDENCE_CACHE_BODY_SUMMARY,
            XG_EVIDENCE_CACHE_GLOBAL_EVIDENCE,
        };
        for (uint32_t i = 0; i < XG_EVIDENCE_CACHE_PHASE_COUNT; i++) {
            evidence_cache_payloads[i] =
                xg_global_evidence_cache_payload_dump(&global_evidence, phases[i]);
            if (!evidence_cache_payloads[i]) {
                fprintf(stderr, "Error: failed to dump evidence cache payload\n");
                goto fail_free_ir;
            }
        }
    }
    if (emit_global_evidence_dump) {
        global_evidence_dump = xg_global_evidence_dump(&global_evidence);
        if (!global_evidence_dump) {
            fprintf(stderr, "Error: failed to dump global evidence\n");
            goto fail_free_ir;
        }
    }
    cfg.run_canonicalize = false;
    cfg.global_evidence = &global_evidence;

    int total_funcs = 0;
    for (int ti = 0; ti < nmodules; ti++) {
        int idx = graph->topo_order[ti];
        XrModuleSpec *spec = &graph->specs[idx];
        if (!spec->ast || !spec->source_path) {
            pres_arr[ti].status = XI_PIPE_ERR_INTERNAL;
            fprintf(stderr, "Error: no AST for module '%s'\n", paths[ti]);
            goto fail_free_ir;
        }

        /* Compile using the shared analyzer (has cross-module type info) */
        cfg.source_file = spec->source_path;
        cfg.global_evidence_module_id = (uint32_t) (ti + 1);
        pres_arr[ti] = xi_pipeline_compile_program((AstNode *) spec->ast, shared_analyzer, X, &cfg);
        if (pres_arr[ti].status != XI_PIPE_OK) {
            fprintf(stderr, "Error: Xi pipeline failed for '%s' at %s: %s\n", paths[ti],
                    xi_pipeline_stage_str(pres_arr[ti].error.stage), pres_arr[ti].error.detail);
            goto fail_free_ir;
        }
        ir_funcs[ti] = pres_arr[ti].ir;
        XR_DCHECK(ir_funcs[ti] != NULL, "xaot_build: pipeline OK but NULL IR");
        total_funcs += 1 + ir_funcs[ti]->nchildren;

        XR_DCHECK(ir_funcs[ti]->module != NULL, "xaot_build: pipeline produced no module metadata");
        modules[ti] = ir_funcs[ti]->module;
        modules[ti]->path = paths[ti];
        modules[ti]->name = mod_names[ti];
        ir_funcs[ti]->module = NULL;
    }
    xa_analyzer_set_graph(shared_analyzer, NULL);

    /* Xi values and AOT plans retain pointers into the analyzer-owned type
     * pool. Keep that pool alive through import resolution, prepare, verify,
     * and C emission; releasing it here turns non-singleton types into dangling
     * pointers before code generation. */

    /* --- Resolve XI_IMPORT_REF using graph (before graph is freed) --- */
    for (int ti = 0; ti < nmodules; ti++) {
        xi_resolve_imports(ir_funcs[ti], graph, paths[ti], modules, nmodules);
    }

    /* --- Cross-module LTO: direct-bind imported callees --- */
    {
        XiLtoContext lto;
        if (xi_lto_context_init(&lto, modules, (uint32_t) nmodules))
            (void) xi_lto_link_modules(&lto);
        xi_lto_context_free(&lto);
    }

    /* --- AOT target prepare: build sidecar rep/ABI plan before C emission --- */
    if (!xaot_bundle_init(&aot_bundle, modules, (uint32_t) nmodules, (uint32_t) entry_index)) {
        fprintf(stderr, "Error: failed to initialize AOT bundle plan\n");
        goto fail_free_ir;
    }
    aot_bundle_initialized = true;
    aot_bundle.emit_program_main = emit_program_main;
    if (!xaot_bundle_set_target_data_layout(&aot_bundle,
                                            xr_compiler_session_target_data_layout(session))) {
        fprintf(stderr, "Error: failed to set AOT target data layout\n");
        goto fail_free_ir;
    }
    if (!xaot_bundle_set_target_simd_features(
            &aot_bundle, options->target ? options->target->simd_features : 0)) {
        fprintf(stderr, "Error: failed to set AOT target SIMD features\n");
        goto fail_free_ir;
    }
    if (options->capability_provider &&
        !xaot_bundle_set_capability_provider(&aot_bundle, options->capability_provider)) {
        fprintf(stderr, "Error: invalid AOT target capability provider\n");
        goto fail_free_ir;
    }
    if (!xaot_bundle_set_global_evidence(&aot_bundle, &global_evidence,
                                         global_evidence.key.profile)) {
        fprintf(stderr, "Error: failed to attach global evidence plan: %s\n",
                aot_bundle.error_msg ? aot_bundle.error_msg : "unknown error");
        goto fail_free_ir;
    }
    if (!xaot_prepare_bundle(&aot_bundle, &prepare_stats)) {
        fprintf(stderr, "Error: AOT prepare failed: %s\n",
                aot_bundle.error_msg ? aot_bundle.error_msg : "?");
        goto fail_free_ir;
    }
    {
        char verify_err[512];
        if (!xaot_verify_bundle(&aot_bundle, XAOT_VERIFY_AOT_READY, verify_err,
                                sizeof(verify_err))) {
            /* A requested semantic-evidence dump is produced before Xi/AOT
             * preparation.  Preserve that diagnostic on a later verifier
             * failure: fail-closed plans are specifically when the evidence
             * is most useful, and dropping it forces debugging to depend on
             * unstable Xi names or ad-hoc instrumentation. */
            if (emit_global_evidence_dump && global_evidence_dump)
                fputs(global_evidence_dump, stdout);
            fprintf(stderr, "Error: AOT verifier failed: %s\n", verify_err);
            goto fail_free_ir;
        }
    }
    if (!reject_profile_capability_plans(&aot_bundle))
        goto fail_free_ir;
    if (!reject_profile_metadata_plans(&aot_bundle))
        goto fail_free_ir;
    if (!reject_profile_static_data_plans(&aot_bundle))
        goto fail_free_ir;
    if (emit_local_evidence_dump) {
        local_evidence_dump = xaot_dump_local_evidence(&aot_bundle, ir_funcs, nmodules);
        if (!local_evidence_dump) {
            fprintf(stderr, "Error: failed to dump local Xi evidence\n");
            goto fail_free_ir;
        }
    }
    /* The plan dump is O(functions x values) diagnostics; only build it when
     * the caller actually wants it (--dump-xaot-plan). */
    if (emit_plan_dump) {
        plan_dump = xaot_bundle_dump_plan(&aot_bundle);
        if (!plan_dump) {
            fprintf(stderr, "Error: failed to dump AOT prepare plan\n");
            goto fail_free_ir;
        }
    }

    /* Graph ASTs must not be freed before compilation is done.
     * Now that pipeline is complete, free the graph (frees ASTs too). */
    xr_free(mono_roots);
    mono_roots = NULL;
    xr_module_graph_free(graph);
    graph = NULL;

    /* --- Create codegen context (no global state) --- */
    XiCgenCtx *cg_ctx = xi_cgen_ctx_new();
    if (!cg_ctx) {
        fprintf(stderr, "Error: failed to create codegen context\n");
        goto fail_free_ir;
    }
    has_explicit_vector_ops = xaot_bundle_has_explicit_vector_ops(modules, nmodules);
    xi_cgen_ctx_set_aot_bundle(cg_ctx, &aot_bundle);
    xi_cgen_ctx_set_target(cg_ctx, options->target, has_explicit_vector_ops);
    xi_cgen_ctx_set_emit_main(cg_ctx, emit_program_main);
    xi_cgen_ctx_set_freestanding_profile(cg_ctx, profile == XAOT_BUILD_PROFILE_FREESTANDING);
    xi_cgen_ctx_set_c_dialect(cg_ctx, options->c_dialect);
    xi_cgen_ctx_set_type_name_profile(cg_ctx, type_name_profile);
    /* Contract verification explicitly requests the same residue dump. */
    bool want_residue = options->emit_residue_dump;
    xi_cgen_ctx_set_residue_tracking(cg_ctx, want_residue);

    /* --- Resolve cross-module imports for C codegen --- */
    xi_cgen_resolve_module_imports(cg_ctx, modules, nmodules);

    /* --- Generate C: one translation unit per module --- */
    XaotModuleSource *sources =
        (XaotModuleSource *) xr_calloc((size_t) nmodules, sizeof(XaotModuleSource));
    if (!sources) {
        xi_cgen_ctx_free(cg_ctx);
        goto fail_free_ir;
    }
    int n_sources = 0;
    bool emit_ok = true;
    size_t total_c_bytes = 0;

    for (int m = 0; m < nmodules && emit_ok; m++) {
        char *buf = NULL;
        size_t bufsz = 0;
        FILE *mem = xr_open_memstream(&buf, &bufsz);
        if (!mem) {
            fprintf(stderr, "Error: xr_open_memstream failed\n");
            emit_ok = false;
            break;
        }
        if (nmodules == 1) {
            /* Single-module bundle stays a single self-contained unit (no
             * cross-module symbols, so it keeps file-static linkage). */
            xi_cgen_program(cg_ctx, mem, modules[m]);
        } else {
            /* Multi-module: emit module m as an independently compilable unit
             * (external cross-module symbols; entry unit carries main). */
            xi_cgen_module_tu(cg_ctx, mem, modules, nmodules, m, entry_index);
        }
        if (xi_cgen_has_error(cg_ctx)) {
            fprintf(stderr, "Error: AOT C code generation failed in module '%s'\n",
                    mod_names[m] ? mod_names[m] : "?");
            emit_ok = false;
        }
        if (xr_close_memstream(mem, &buf, &bufsz) != 0) {
            fprintf(stderr, "Error: xr_close_memstream failed\n");
            xr_free(buf);
            emit_ok = false;
            break;
        }
        /* Task 218 defense line 3: verify the generated translation unit is
         * structurally well-formed before it can be written to disk / handed
         * to the C toolchain. Fail-closed — a malformed TU raises an ICE with
         * a full on-disk dump and never reaches clang. No bypass. */
        const char *verify_timing = getenv("XRAY_CGEN_VERIFY_TIMING");
        clock_t verify_started = verify_timing ? clock() : (clock_t) 0;
        xi_cgen_verify_output_or_ice(buf, bufsz, mod_names[m] ? mod_names[m] : "module");
        if (options->c_dialect == XI_CGEN_C_DIALECT_C90) {
            XiCgenVerifyResult c90_result;
            if (!xi_cgen_verify_c90_output(buf, bufsz, &c90_result)) {
                fprintf(stderr,
                        "Error: restricted C90 code generation rejected module '%s' at line "
                        "%d: %s\n",
                        mod_names[m] ? mod_names[m] : "?", c90_result.line, c90_result.message);
                xr_free(buf);
                emit_ok = false;
                break;
            }
        }
        if (verify_timing) {
            clock_t verify_ticks = clock() - verify_started;
            unsigned long long verify_us =
                (unsigned long long) verify_ticks * 1000000ULL / CLOCKS_PER_SEC;
            fprintf(stderr, "[cgen-verify] cpu_us=%llu bytes=%zu tu=%s\n", verify_us, bufsz,
                    mod_names[m] ? mod_names[m] : "module");
        }
        sources[m].name = xr_strdup(mod_names[m] ? mod_names[m] : "module");
        sources[m].c_source = buf;
        n_sources++;
        total_c_bytes += bufsz;
    }

    if (!emit_ok || xi_cgen_has_error(cg_ctx)) {
        fprintf(stderr, "Error: AOT C code generation failed\n");
        xi_cgen_ctx_free(cg_ctx);
        for (int m = 0; m < n_sources; m++) {
            xr_free(sources[m].name);
            xr_free(sources[m].c_source);
        }
        xr_free(sources);
        goto fail_free_ir;
    }
    XiCgenStats cgen_stats = xi_cgen_stats(cg_ctx);
    XiCgenCoroFrameStats coro_frame_stats = xi_cgen_coro_frame_stats(cg_ctx);

    size_t c_export_header_sz = 0;
    FILE *header_mem = xr_open_memstream(&c_export_header, &c_export_header_sz);
    if (!header_mem) {
        fprintf(stderr, "Error: xr_open_memstream failed\n");
        xi_cgen_ctx_free(cg_ctx);
        for (int m = 0; m < n_sources; m++) {
            xr_free(sources[m].name);
            xr_free(sources[m].c_source);
        }
        xr_free(sources);
        goto fail_free_ir;
    }
    xi_cgen_c_export_header(cg_ctx, header_mem, modules, nmodules, "XRAY_AOT_C_EXPORTS_H");
    if (xr_close_memstream(header_mem, &c_export_header, &c_export_header_sz) != 0 ||
        xi_cgen_has_error(cg_ctx)) {
        fprintf(stderr, "Error: AOT C export header generation failed\n");
        xr_free(c_export_header);
        xi_cgen_ctx_free(cg_ctx);
        for (int m = 0; m < n_sources; m++) {
            xr_free(sources[m].name);
            xr_free(sources[m].c_source);
        }
        xr_free(sources);
        goto fail_free_ir;
    }
    /* Task 217 P2: snapshot per-function residue before the ctx is released. */
    if (options->emit_residue_dump)
        residue_dump = xi_cgen_residue_dump(cg_ctx);
    xi_cgen_ctx_free(cg_ctx);

    /* Build link features before freeing IR. Runtime capabilities, external
     * dylibs, and stdlib module/symbol closure all come from verified global
     * evidence plans. */
    XaotFeatureSet features;
    memset(&features, 0, sizeof(features));
    features_apply_capability_plans(&features, &aot_bundle);
    features_apply_ir_intrinsics(&features, &aot_bundle);
    features_apply_link_dependency_plans(&features, &aot_bundle);
    features_apply_extern_decls(&features, &aot_bundle);
    if (!build_link_manifest(&features, options->target, &link_manifest,
                             profile == XAOT_BUILD_PROFILE_FREESTANDING,
                             options->capability_provider, &aot_bundle.entry_plan)) {
        fprintf(stderr, "Error: failed to build AOT link manifest\n");
        goto fail_free_ir;
    }
    link_manifest_initialized = true;
    if (options->c_dialect == XI_CGEN_C_DIALECT_C90 &&
        !xaot_c90_link_manifest_supported(&link_manifest)) {
        fprintf(stderr,
                "Error: restricted C90 reachable graph requires runtime, stdlib, or native "
                "capabilities (runtime_caps=%u runtime_objects=%u stdlib_objects=%u "
                "stdlib_symbols=%u native_inputs=%u)\n",
                (unsigned) link_manifest.n_runtime_caps, (unsigned) link_manifest.n_runtime_objects,
                (unsigned) link_manifest.n_stdlib_objects,
                (unsigned) link_manifest.n_stdlib_symbols,
                (unsigned) link_manifest.n_native_inputs);
        for (int m = 0; m < n_sources; m++) {
            xr_free(sources[m].name);
            xr_free(sources[m].c_source);
        }
        xr_free(sources);
        goto fail_free_ir;
    }

    xaot_bundle_free(&aot_bundle);
    aot_bundle_initialized = false;
    xg_global_evidence_free(&global_evidence);
    global_evidence_initialized = false;

    /* Free IR and module metadata (no longer needed after C generation) */
    for (int m = 0; m < nmodules; m++) {
        xi_module_free(modules[m]);
        xi_pipeline_result_free(&pres_arr[m]);
    }
    xr_free(modules);
    xr_free(pres_arr);
    xr_free(ir_funcs);
    xa_analyzer_free(shared_analyzer);
    shared_analyzer = NULL;
    xray_vm_delete(X);
    X = NULL;

    if (!quiet) {
        printf("[xi-native] Generated %zu bytes of C (%d functions, %d modules in %d unit%s)\n",
               total_c_bytes, total_funcs, nmodules, n_sources, n_sources == 1 ? "" : "s");
    }

    /* Each source buffer is xr_malloc-owned (xr_close_memstream guarantees this
     * on every platform); ownership transfers into the result. */
    result->sources = sources;
    result->n_sources = n_sources;
    result->plan_dump = plan_dump;
    plan_dump = NULL;
    result->global_evidence_dump = global_evidence_dump;
    global_evidence_dump = NULL;
    result->local_evidence_dump = local_evidence_dump;
    local_evidence_dump = NULL;
    result->residue_dump = residue_dump;
    residue_dump = NULL;
    result->evidence_cache_manifest = evidence_cache_manifest;
    result->has_evidence_cache_manifest = evidence_cache_manifest_valid;
    for (uint32_t i = 0; i < XG_EVIDENCE_CACHE_PHASE_COUNT; i++) {
        result->evidence_cache_payloads[i] = evidence_cache_payloads[i];
        evidence_cache_payloads[i] = NULL;
    }
    result->c_export_header = c_export_header;
    c_export_header = NULL;
    result->link_manifest = link_manifest;
    memset(&link_manifest, 0, sizeof(link_manifest));
    link_manifest_initialized = false;
    result->total_compiled = total_funcs;
    result->total_aot = total_funcs;
    result->nmodules = nmodules;
    result->features = features;
    result->has_explicit_vector_ops = has_explicit_vector_ops;
    result->prepare_stats = prepare_stats;
    result->cgen_stats = cgen_stats;
    result->coro_frame_stats = coro_frame_stats;

    /* Cleanup module name arrays */
    for (int i = 0; i < nmodules; i++) {
        if (paths)
            xr_free(paths[i]);
        if (mod_names)
            xr_free(mod_names[i]);
    }
    xr_free(paths);
    xr_free(mod_names);
    return 0;

fail_free_ir:
    xr_free(plan_dump);
    xr_free(global_evidence_dump);
    xr_free(local_evidence_dump);
    xr_free(residue_dump);
    for (uint32_t i = 0; i < XG_EVIDENCE_CACHE_PHASE_COUNT; i++)
        xr_free(evidence_cache_payloads[i]);
    xr_free(c_export_header);
    if (link_manifest_initialized)
        xaot_link_manifest_free(&link_manifest);
    if (aot_bundle_initialized)
        xaot_bundle_free(&aot_bundle);
    if (global_evidence_initialized)
        xg_global_evidence_free(&global_evidence);
    for (int m = 0; m < nmodules; m++) {
        if (modules)
            xi_module_free(modules[m]);
        xi_pipeline_result_free(&pres_arr[m]);
    }
    xr_free(modules);
    xr_free(pres_arr);
    xr_free(ir_funcs);
    if (shared_analyzer) {
        xa_analyzer_set_graph(shared_analyzer, NULL);
        xa_analyzer_free(shared_analyzer);
        shared_analyzer = NULL;
    }
fail_free_analyzer:
    if (cached_global_evidence_initialized) {
        xg_global_evidence_free(&cached_global_evidence);
        cached_global_evidence_initialized = false;
    }
    if (pre_mono_generic_evidence_initialized) {
        xg_global_evidence_free(&pre_mono_generic_evidence);
        pre_mono_generic_evidence_initialized = false;
    }
    if (shared_analyzer) {
        xa_analyzer_set_graph(shared_analyzer, NULL);
        xa_analyzer_free(shared_analyzer);
        shared_analyzer = NULL;
    }
fail_free_graph:
    xr_free(imported_summary_modules);
    xr_free(discovered_imported_summary_payload_view);
    xaot_free_discovered_package_payloads(discovered_imported_summary_payloads,
                                          discovered_imported_summary_payload_count);
    if (graph)
        xr_module_graph_free(graph);
    if (X)
        xray_vm_delete(X);
    xr_free(mono_roots);
    for (int i = 0; i < nmodules; i++) {
        if (paths)
            xr_free(paths[i]);
        if (mod_names)
            xr_free(mod_names[i]);
    }
    xr_free(paths);
    xr_free(mod_names);
    return 1;
}

static void xaot_amalgamate_unit(FILE *out, const char *source, int module_ordinal) {
    enum {
        XAOT_AMALGAMATE_NORMAL = 0,
        XAOT_AMALGAMATE_STRING,
        XAOT_AMALGAMATE_CHAR,
        XAOT_AMALGAMATE_LINE_COMMENT,
        XAOT_AMALGAMATE_BLOCK_COMMENT,
    } state = XAOT_AMALGAMATE_NORMAL;
    const char *p = source ? source : "";
    while (*p) {
        if (state == XAOT_AMALGAMATE_NORMAL) {
            if (p[0] == '/' && p[1] == '/') {
                fputs("//", out);
                p += 2;
                state = XAOT_AMALGAMATE_LINE_COMMENT;
                continue;
            }
            if (p[0] == '/' && p[1] == '*') {
                fputs("/*", out);
                p += 2;
                state = XAOT_AMALGAMATE_BLOCK_COMMENT;
                continue;
            }
            if (*p == '"') {
                fputc(*p++, out);
                state = XAOT_AMALGAMATE_STRING;
                continue;
            }
            if (*p == '\'') {
                fputc(*p++, out);
                state = XAOT_AMALGAMATE_CHAR;
                continue;
            }
            bool token_start = p == source || (!isalnum((unsigned char) p[-1]) && p[-1] != '_');
            if (token_start && strncmp(p, "_xstr_", 6) == 0 && isdigit((unsigned char) p[6])) {
                fprintf(out, "_xstr_m%d_", module_ordinal);
                p += 6;
                continue;
            }
            if (token_start && strncmp(p, "_xbytes_", 8) == 0 && isdigit((unsigned char) p[8])) {
                fprintf(out, "_xbytes_m%d_", module_ordinal);
                p += 8;
                continue;
            }
            fputc(*p++, out);
            continue;
        }
        if (state == XAOT_AMALGAMATE_STRING || state == XAOT_AMALGAMATE_CHAR) {
            char quote = state == XAOT_AMALGAMATE_STRING ? '"' : '\'';
            if (*p == '\\' && p[1]) {
                fputc(*p++, out);
                fputc(*p++, out);
                continue;
            }
            char ch = *p++;
            fputc(ch, out);
            if (ch == quote)
                state = XAOT_AMALGAMATE_NORMAL;
            continue;
        }
        if (state == XAOT_AMALGAMATE_LINE_COMMENT) {
            char ch = *p++;
            fputc(ch, out);
            if (ch == '\n')
                state = XAOT_AMALGAMATE_NORMAL;
            continue;
        }
        if (p[0] == '*' && p[1] == '/') {
            fputs("*/", out);
            p += 2;
            state = XAOT_AMALGAMATE_NORMAL;
            continue;
        }
        fputc(*p++, out);
    }
}

XR_FUNC char *xaot_build_result_amalgamate(const XaotBuildResult *result, size_t *out_size) {
    char *text = NULL;
    size_t size = 0;
    if (out_size)
        *out_size = 0;
    if (!result || !result->sources || result->n_sources <= 0)
        return NULL;
    FILE *out = xr_open_memstream(&text, &size);
    if (!out)
        return NULL;
    int impl_source = -1;
    for (int i = 0; i < result->n_sources; i++) {
        if (strstr(result->sources[i].c_source, "#define XRT_IMPL")) {
            impl_source = i;
            break;
        }
    }
    if (impl_source >= 0) {
        if (result->n_sources > 1)
            fprintf(out, "/* ==== module: %s ==== */\n",
                    result->sources[impl_source].name ? result->sources[impl_source].name : "?");
        xaot_amalgamate_unit(out, result->sources[impl_source].c_source, impl_source);
        fputc('\n', out);
    }
    for (int i = 0; i < result->n_sources; i++) {
        if (i == impl_source)
            continue;
        if (result->n_sources > 1)
            fprintf(out, "/* ==== module: %s ==== */\n",
                    result->sources[i].name ? result->sources[i].name : "?");
        xaot_amalgamate_unit(out, result->sources[i].c_source, i);
        fputc('\n', out);
    }
    bool write_failed = ferror(out) != 0;
    int close_status = xr_close_memstream(out, &text, &size);
    if (write_failed || close_status != 0) {
        xr_free(text);
        return NULL;
    }
    if (out_size)
        *out_size = size;
    return text;
}

XR_FUNC void xaot_build_result_free(XaotBuildResult *result) {
    if (!result)
        return;
    if (result->sources) {
        for (int i = 0; i < result->n_sources; i++) {
            xr_free(result->sources[i].name);
            xr_free(result->sources[i].c_source);
        }
        xr_free(result->sources);
    }
    xr_free(result->plan_dump);
    xr_free(result->global_evidence_dump);
    xr_free(result->local_evidence_dump);
    xr_free(result->residue_dump);
    for (uint32_t i = 0; i < XG_EVIDENCE_CACHE_PHASE_COUNT; i++)
        xr_free(result->evidence_cache_payloads[i]);
    xr_free(result->c_export_header);
    xaot_link_manifest_free(&result->link_manifest);
    memset(result, 0, sizeof(*result));
}
