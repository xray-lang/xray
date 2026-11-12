/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_cleanup_suspend.inc.c - Cleanup substitutions at actual exits
 */
typedef struct EffectCleanupSuspend {
    EffectCheckBuild *build;
    uint32_t caller, parameter, head;
    bool complete, used;
} EffectCleanupSuspend;

static XrXirStatus effect_cleanup_suspend_origin(void *opaque, const XirExecutionSite *site) {
    EffectCleanupSuspend *projection = opaque;
    EffectCheckBuild *build = projection->build;
    EffectCheckedRoots *roots = build->roots;
    if (site->kind != XIR_EXECUTION_CALL || site->protocol != 1 || site->index >= roots->calls)
        return XR_XIR_BAD_STRUCTURE;
    EffectCheckCall reference = roots->call[site->index];
    bool formula = roots->node[reference.output].function == projection->caller;
    if (reference.count && (!formula || site->capture_environment != XIR_EXECUTION_CURRENT_ENVIRONMENT)) {
        projection->complete = false;
        return XR_XIR_OK;
    }
    for (uint32_t a = projection->head; a != UINT32_MAX; a = roots->suspend_actual[a].next)
        if (roots->suspend_actual[a].parameter == projection->parameter &&
            roots->suspend_actual[a].source == reference.output) return XR_XIR_OK;
    if (roots->suspend_actuals == roots->suspend_actual_capacity) {
        EffectCheckSuspendActual *values = xir_compile_array_grow(build->memory, roots->suspend_actual,
            roots->suspend_actuals, sizeof(*values), &roots->suspend_actual_capacity, &build->status);
        if (!values) return build->status;
        roots->suspend_actual = values;
    }
    uint32_t index = roots->suspend_actuals++;
    roots->suspend_actual[index] = (EffectCheckSuspendActual){reference.output,
        projection->parameter, projection->head, formula};
    projection->head = index;
    return XR_XIR_OK;
}

static bool effect_cleanup_contents_input(const EffectCheckedRoots *roots,
    uint32_t function, uint32_t value, uint32_t parameter) {
    const EffectCheckOrigins *origins = &roots->storage_origins;
    if (!origins->offsets || function >= roots->templates ||
        value >= origins->offsets[function + 1] - origins->offsets[function]) return false;
    uint32_t node = origins->offsets[function] + value;
    for (uint32_t e = origins->heads[node]; e != UINT32_MAX; e = origins->edges[e].next)
        if (origins->edges[e].parameter_contents && origins->edges[e].function == function &&
            origins->edges[e].value == parameter) return true;
    return false;
}

static XrXirStatus effect_cleanup_suspend_actual(EffectCleanupSuspend *projection,
    const XirStorageFlowView *storage, XirStorageApplication application,
    const XirExecutionSite *site, uint32_t parameter) {
    EffectCheckedRoots *roots = projection->build->roots;
    XirExecutionArgument actual = {0};
    XrXirStatus status = xir_execution_argument(roots, site, parameter, &actual);
    if (status != XR_XIR_OK) return status;
    XrXirType type = actual.cell ? xr_xir_cell_element(xir_execution_types(roots), actual.type) : actual.type;
    if (!xr_xir_type_is_callable(xir_execution_types(roots), type)) return XR_XIR_OK;
    XirExecutionValue reference = {actual.function, actual.value, actual.environment};
    projection->parameter = parameter;
    if (actual.cell) {
        if (actual.caller_handle == UINT32_MAX) {
            projection->complete = false;
            return XR_XIR_OK;
        }
        bool bound = false;
        for (uint32_t b = 0; b < application.count; ++b) {
            XirStorageBinding binding = storage->bindings[application.first + b];
            if (binding.formal_function != site->declaration ||
                !effect_cleanup_contents_input(roots, binding.formal_function, binding.formal_value, parameter)) continue;
            reference = (XirExecutionValue){binding.actual_function == site->function ?
                site->caller_instance : binding.actual_function, binding.actual_value, binding.actual_environment};
            bound = true;
            break;
        }
        if (!bound) {projection->complete = false; return XR_XIR_OK;}
        projection->parameter += roots->function[site->target].parameters;
    }
    bool complete = false;
    uint32_t remainder = 0;
    status = execution_visit_origins(roots, reference.function, reference.value, reference.environment,
        effect_cleanup_suspend_origin, projection, &complete, &remainder,
        (ExecutionOriginRequest){.kind = EXECUTION_ORIGINS_EFFECTS, .allow_bottom=true});
    if (status == XR_XIR_OK && (!complete || remainder)) projection->complete = false;
    return status;
}

/* The polynomial and its SCC schedule remain shared. Only actual arguments
 * differ: Cell captures use their ordered contents, and value captures retain
 * their own immutable identity. Incomplete substitutions keep the old bound. */
static XrXirStatus effect_cleanup_suspend_project(EffectCheckBuild *build) {
    EffectCheckedRoots *roots = build->roots;
    if (!roots->storage || !roots->cleanup_uses) return XR_XIR_OK;
    XirStorageFlowView storage = {0};
    XrXirStatus status = xir_storage_flow_view(roots->storage, &storage);
    if (status != XR_XIR_OK) return status;
    EffectCleanupSuspend *rows = xir_compile_calloc(build->memory, roots->calls, sizeof(*rows), &status);
    bool *seen = status == XR_XIR_OK ? xir_compile_calloc(build->memory, roots->cleanup_uses, sizeof(*seen), &status) : NULL;
    bool projected = false;
    for (uint32_t c = 0; status == XR_XIR_OK && c < roots->calls; ++c)
        rows[c] = (EffectCleanupSuspend){.build=build, .caller=roots->node[roots->call[c].output].function,
            .head=UINT32_MAX, .complete=true};
    for (uint32_t a = 0; status == XR_XIR_OK && a < storage.application_count; ++a) {
        if (!effect_check_poll(build)) {status = build->status; break;}
        XirStorageApplication app = storage.applications[a];
        if (app.context_only || app.kind != XIR_EXECUTION_CLEANUP_USE || app.outcome != XIR_EXECUTION_NORMAL) continue;
        if (app.site >= roots->cleanup_uses || app.first > storage.binding_count ||
            app.count > storage.binding_count - app.first) {status = XR_XIR_BAD_STRUCTURE; break;}
        uint32_t c = roots->cleanup_use[app.site].registration;
        seen[app.site] = rows[c].used = true;
        XirExecutionSite site = execution_cleanup_site(roots, app.site);
        for (uint32_t p = 0; status == XR_XIR_OK && p < roots->call[c].count; ++p)
            status = effect_cleanup_suspend_actual(&rows[c], &storage, app, &site, p);
    }
    for (uint32_t u = 0; status == XR_XIR_OK && u < roots->cleanup_uses; ++u)
        if (!seen[u]) rows[roots->cleanup_use[u].registration].complete = false;
    for (uint32_t c = 0; status == XR_XIR_OK && c < roots->calls; ++c) {
        if (!rows[c].used || !rows[c].complete) continue;
        roots->call[c].suspend_actuals = rows[c].head;
        roots->call[c].suspend_projected = true;
        for (uint32_t a = rows[c].head; a != UINT32_MAX; a = roots->suspend_actual[a].next)
            if (!effect_check_dependency(build, roots->suspend_actual[a].source, roots->call[c].output)) {
                status = build->status; break;
            }
        projected = true;
    }
    xr_compile_resources_free(rows);
    xr_compile_resources_free(seen);
    if (status == XR_XIR_OK && projected) effect_suspend_reset(roots);
    return status;
}
