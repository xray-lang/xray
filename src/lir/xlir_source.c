/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_source.c - One owned source-to-LIR compilation boundary
 *
 * KEY CONCEPT:
 *   The complete root is lowered before either backend can consume it.
 *   Failure transfers diagnostic owners without allocating during cleanup.
 */
#include "xlir_source.h"
#include "xlir_builder.h"
#include "xlir_inline.h"
#include "xlir_sccp.h"
#include "xlir_test_plan.h"
#include "toolchain/xcompiler_session.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_specialization_bindings.h"
#include <string.h>

static XrLirStatus source_optimize(XrLirLowered *lowered) {
    XrLirModule *optimized = NULL;
    XrLirStatus status = xr_lir_sccp(lowered->module, &optimized);
    if (status != XR_LIR_OK) return status;
    xr_lir_module_destroy(lowered->module);
    lowered->module = optimized;
    lowered->entry.module = optimized;
    optimized = NULL;
    status = xr_lir_inline(lowered->module, &optimized);
    if (status != XR_LIR_OK) return status;
    xr_lir_module_destroy(lowered->module);
    lowered->module = optimized;
    lowered->entry.module = optimized;
    return XR_LIR_OK;
}

static uint32_t source_find_test(const XrXirArtifact *checked, const char *name) {
    const XrXirModule *module = xr_xir_compile_artifact_module(checked);
    if (!module || !module->declarations || !module->declarations->functions)
        return UINT32_MAX;
    const XrXirDeclarations *declarations = module->declarations;
    size_t name_length = strlen(name);
    uint32_t selected = UINT32_MAX;
    for (uint32_t i = 0; i < module->function_count; ++i) {
        const XrXirFunctionIdentity *identity = &declarations->functions[i];
        const XrXirFunction *function = &module->functions[i];
        if (identity->module != declarations->root_module ||
            identity->test_role != XR_XIR_TEST_ROLE_TEST ||
            function->name_length != name_length || memcmp(function->name, name, name_length))
            continue;
        if (selected != UINT32_MAX) return UINT32_MAX;
        selected = i;
    }
    return selected;
}

typedef struct SourceTestSelection {
    uint32_t source_function;
    uint32_t closed_function;
} SourceTestSelection;

static XrXirStatus source_map_function(const XrXirSpecializationMapView *map, uint32_t source,
    uint32_t *output) {
    if (source >= map->function_count || (!map->identity && !map->functions))
        return XR_XIR_BAD_STRUCTURE;
    uint32_t closed = map->identity ? source : map->functions[source];
    uint32_t origin;
    if (closed == UINT32_MAX ||
        xr_xir_compile_specialization_origin(map->closed, closed, &origin) != XR_XIR_OK ||
        origin != source)
        return XR_XIR_BAD_STRUCTURE;
    *output = closed;
    return XR_XIR_OK;
}

static XrXirStatus source_receive_test(void *context, const XrXirSpecializationMapView *map) {
    SourceTestSelection *selection = context;
    return source_map_function(map, selection->source_function, &selection->closed_function);
}

struct XrLirTestPlan {
    XrLirLowered lowered;
    uint32_t role_count;
    XrLirTestBinding *roles;
};

_Static_assert(_Alignof(XrLirTestPlan) >= _Alignof(XrLirTestBinding),
    "test role records follow their plan at natural alignment");

static bool source_root_role(const XrXirDeclarations *declarations, uint32_t function) {
    const XrXirFunctionIdentity *identity = &declarations->functions[function];
    return identity->module == declarations->root_module &&
        identity->test_role != XR_XIR_TEST_ROLE_NONE;
}

static XrLirTestPlan *source_test_plan(const XrXirArtifact *checked, XrCompileResources *resources) {
    const XrXirModule *module = xr_xir_compile_artifact_module(checked);
    if (!module || !module->declarations || !module->declarations->functions) return NULL;
    const XrXirDeclarations *declarations = module->declarations;
    uint32_t count = 0;
    size_t size = sizeof(XrLirTestPlan);
    for (uint32_t f = 0; f < module->function_count; ++f) {
        if (!source_root_role(declarations, f)) continue;
        size_t length = module->functions[f].name_length;
        if (SIZE_MAX - size < sizeof(XrLirTestBinding) ||
            SIZE_MAX - size - sizeof(XrLirTestBinding) <= length) return NULL;
        size += sizeof(XrLirTestBinding) + length + 1;
        ++count;
    }
    XrLirTestPlan *plan = NULL;
    if (xr_compile_resources_alloc(resources, size, (void **)&plan) != XR_COMPILE_RESOURCE_OK)
        return NULL;
    *plan = (XrLirTestPlan){.role_count = count, .roles = (void *)(plan + 1)};
    char *name = (char *)(plan->roles + count);
    uint32_t row = 0;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        if (!source_root_role(declarations, f)) continue;
        const XrXirFunction *function = &module->functions[f];
        size_t length = function->name_length;
        memcpy(name, function->name, length);
        name[length] = 0;
        plan->roles[row++] = (XrLirTestBinding){(XrXirTestRole)declarations->functions[f].test_role,
            f, name, length};
        name += length + 1;
    }
    return plan;
}

static XrXirStatus source_receive_test_plan(void *context, const XrXirSpecializationMapView *map) {
    XrLirTestPlan *plan = context;
    for (uint32_t i = 0; i < plan->role_count; ++i) {
        XrLirTestBinding *role = &plan->roles[i];
        XrXirStatus status = source_map_function(map, role->function, &role->function);
        if (status != XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}

static XrLirSourceStatus source_compile(const XrLirSourceRequest *request, const char *test_name,
    XrLirLowered *output, XrLirTestPlan **plan_output, XrLirSourceFailure *failure) {
    if (!request || !request->resources || !request->entry_path || !request->entry_path[0] ||
        (plan_output ? *plan_output != NULL :
            !output || output->module || output->entry.module || output->entry.id) || !failure ||
        failure->stage != XR_LIR_SOURCE_OK || failure->lower.owned_path ||
        !xr_xir_compile_source_failure_empty(&failure->source_failure))
        return XR_LIR_SOURCE_BAD_ARGUMENT;

    XrCompilerSession *session = NULL;
    XrModuleIdentityAuthority authority = {0};
    char *root = NULL;
    XrXirSourceResult source = {0};
    XrXirArtifact *closed = NULL;
    XrLirLowered lowered = {0};
    XrLirTestPlan *plan = NULL;
    XrLirSourceStatus status = XR_LIR_SOURCE_PREPARATION_FAILED;
    if (xr_compile_session_new(request->resources, &session) != XR_COMPILE_RESOURCE_OK)
        goto cleanup;
    if (!request->authority &&
        xr_compile_module_identity_script_authority_from_source(request->resources,
            request->entry_path, &authority, &root) != XR_MODULE_OK)
        goto cleanup;

    XrXirCompileContext context = {request->resources, xr_xir_compile_default_limits()};
    XrXirSourceRequest input = {.session = session, .entry_path = request->entry_path,
        .authority = request->authority ? request->authority : &authority,
        .context = &context, .stdlib_path = request->stdlib_path, .lockfile = request->lockfile,
        .linkage_kind = XR_XIR_PROGRAM, .target = request->target};
    status = XR_LIR_SOURCE_CHECK_FAILED;
    if (xr_xir_compile_source_check(&input, &source, &failure->source,
            &failure->source_failure) != XR_XIR_OK)
        goto cleanup;

    SourceTestSelection test = {UINT32_MAX, UINT32_MAX};
    if (test_name) {
        status = XR_LIR_SOURCE_TEST_SELECTION_FAILED;
        test.source_function = source_find_test(source.checked, test_name);
        if (test.source_function == UINT32_MAX) goto cleanup;
    }
    if (plan_output) {
        status = XR_LIR_SOURCE_PREPARATION_FAILED;
        plan = source_test_plan(source.checked, request->resources);
        if (!plan) goto cleanup;
    }
    status = XR_LIR_SOURCE_SPECIALIZATION_FAILED;
    XrXirDiagnostic specialization = {0};
    if (test_name) {
        XrXirSpecializationReceiver receiver = {&test, source_receive_test};
        failure->specialization_status = xr_xir_compile_specialize_receive(source.checked, &receiver,
            &closed, &specialization, NULL);
    } else if (plan) {
        XrXirSpecializationReceiver receiver = {plan, source_receive_test_plan};
        failure->specialization_status = xr_xir_compile_specialize_receive(source.checked, &receiver,
            &closed, &specialization, NULL);
    } else {
        failure->specialization_status = xr_xir_compile_specialize(source.checked, &closed,
            &specialization, NULL);
    }
    if (failure->specialization_status != XR_XIR_OK) goto cleanup;

    status = XR_LIR_SOURCE_LOWER_FAILED;
    XrLirLowerRequest lower_request = {closed, source.snapshot, false};
    XrLirStatus lower_status;
    if (plan)
        lower_status = xr_lir_lower_test_plan_checked(&lower_request, plan->roles, plan->role_count,
            &lowered, &failure->lower);
    else if (test_name)
        lower_status = xr_lir_lower_test_checked(&lower_request, test.closed_function, &lowered, &failure->lower);
    else
        lower_status = xr_lir_lower_program_checked(&lower_request, &lowered, &failure->lower);
    if (lower_status != XR_LIR_OK) goto cleanup;

    status = XR_LIR_SOURCE_OK;

cleanup:
    xr_xir_compile_artifact_free(closed);
    xr_xir_compile_source_result_free(&source);
    xr_compile_session_free(session);
    xr_compile_resources_free(root);
    if (status == XR_LIR_SOURCE_OK && request->optimize &&
        source_optimize(&lowered) != XR_LIR_OK)
        status = XR_LIR_SOURCE_OPTIMIZATION_FAILED;
    if (status == XR_LIR_SOURCE_OK) {
        if (plan) {
            plan->lowered = lowered;
            *plan_output = plan;
        } else {
            *output = lowered;
        }
    } else {
        xr_lir_lowered_free(&lowered);
        xr_lir_test_plan_free(plan);
        failure->stage = status;
    }
    return status;
}

XR_FUNC XrLirSourceStatus xr_lir_compile_source(const XrLirSourceRequest *request,
    XrLirLowered *output, XrLirSourceFailure *failure) {
    return source_compile(request, NULL, output, NULL, failure);
}

XR_FUNC XrLirSourceStatus xr_lir_compile_test_source(const XrLirSourceRequest *request,
    const char *test_name, XrLirLowered *output, XrLirSourceFailure *failure) {
    if (!test_name || !test_name[0]) return XR_LIR_SOURCE_BAD_ARGUMENT;
    return source_compile(request, test_name, output, NULL, failure);
}

XR_FUNC XrLirSourceStatus xr_lir_compile_test_plan(const XrLirSourceRequest *request,
    XrLirTestPlan **output, XrLirSourceFailure *failure) {
    if (!output) return XR_LIR_SOURCE_BAD_ARGUMENT;
    return source_compile(request, NULL, NULL, output, failure);
}

XR_FUNC XrLirFunction xr_lir_test_plan_initializer(const XrLirTestPlan *plan) {
    return plan ? plan->lowered.entry : (XrLirFunction){0};
}

XR_FUNC uint32_t xr_lir_test_plan_role_count(const XrLirTestPlan *plan) {
    return plan ? plan->role_count : 0;
}

XR_FUNC XrLirStatus xr_lir_test_plan_role_at(const XrLirTestPlan *plan, uint32_t ordinal,
    XrLirTestPlanRole *output) {
    if (!plan || ordinal >= plan->role_count || !output) return XR_LIR_INVALID_ARGUMENT;
    const XrLirTestBinding *role = &plan->roles[ordinal];
    XrLirTestPlanRole view = {role->role, role->name, role->name_length, {0}};
    if (role->role != XR_XIR_TEST_ROLE_SKIP) {
        XrLirStatus status = xr_lir_function_at(plan->lowered.module, role->function, &view.function);
        if (status != XR_LIR_OK) return status;
    }
    *output = view;
    return XR_LIR_OK;
}

XR_FUNC XrLirStatus xr_lir_test_plan_check_role(const XrLirTestPlan *plan, uint32_t ordinal,
    XrLirFunction function) {
    XrLirTestPlanRole role;
    if (!function.id || xr_lir_test_plan_role_at(plan, ordinal, &role) != XR_LIR_OK ||
        function.module != role.function.module || function.id != role.function.id)
        return XR_LIR_INVALID_ARGUMENT;
    return XR_LIR_OK;
}

XR_FUNC void xr_lir_test_plan_free(XrLirTestPlan *plan) {
    if (!plan) return;
    xr_lir_lowered_free(&plan->lowered);
    xr_compile_resources_free(plan);
}

XR_FUNC void xr_lir_source_failure_free(XrLirSourceFailure *failure) {
    if (!failure) return;
    xr_xir_compile_source_failure_free(&failure->source_failure);
    xr_lir_lower_failure_free(&failure->lower);
    memset(failure, 0, sizeof(*failure));
}
