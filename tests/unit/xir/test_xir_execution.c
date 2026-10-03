/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_execution.c - Scalar execution verification
 *
 * KEY CONCEPT:
 *   Scalar VM admission, frozen physical layout, and execution expectations.
 */

#include "xir_execution_fixture.h"
#include "xir_execution_cases.h"
#include "xir/xxir_vm.h"
#include "xir_integer_runtime_cases.h"
#include "xir_integer_admission_cases.h"
#include "xir_float_runtime_cases.h"
#include "xir_float_admission_cases.h"
#include "xir_decimal_cases.h"

static XrXirRunStatus run(void *owner, uint32_t function, XrXirRunContext *context,
                         const XrXirValue *arguments, uint32_t count, XrXirValue *result) {
    XrXirRunStatus status = xr_xir_compile_vm_run(owner, function, context, arguments, count, result);
    if (status == XR_XIR_RUN_BAD_ARTIFACT || status == XR_XIR_RUN_OUT_OF_MEMORY) {
        XrCompileResourceStats stats;
        CHECK(xr_compile_resources_stats(scalar_owner.context.resources, &stats) == XR_COMPILE_RESOURCE_OK);
        fprintf(stderr, "scalar VM function %u status %u: work %llu, allocated %llu, live %llu, steps %llu\n",
            function, (unsigned) status, (unsigned long long) stats.work,
            (unsigned long long) stats.allocated_bytes, (unsigned long long) stats.live_bytes,
            (unsigned long long) context->steps);
    }
    return status;
}

int main(void) {
    scalar_compile_begin();
    { XrXirArtifact *uninitialized = uninitialized_leaf_fixture();
    XrXirRunContext context = {4, 65536, 0, 0, 0, 0};
    XrXirValue argument = {XR_XIR_I64, 0, -1}, result = {0};
    CHECK(xr_xir_compile_vm_run(uninitialized, 0, &context, &argument, 1, &result) == XR_XIR_RUN_OK);
    CHECK(result.type == XR_XIR_I64 && result.payload == -1);
    CHECK(!context.live_bytes && context.allocations == context.frees);
    xr_xir_compile_artifact_free(uninitialized); }
    decimal_cases();
    integer_runtime_cases();
    float_runtime_cases();
    floating_value_admission();
    integer_ir_rejections();
    XrXirArtifact *artifact = fixture_lowered();
    execution_cases(run, artifact);
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    XrXirLayout layout;
    CHECK(xr_xir_builtin_layout(XR_XIR_BOOL, &target, XR_XIR_LAYOUT_STORAGE, &layout) == XR_XIR_OK);
    CHECK(layout.size == 1 && layout.alignment == 1);
    CHECK(xr_xir_builtin_layout(XR_XIR_BOOL, &target, XR_XIR_LAYOUT_FRAME, &layout) == XR_XIR_OK);
    CHECK(layout.size == 8 && layout.alignment == 8);
    CHECK(xr_xir_builtin_layout(XR_XIR_I64, &target, XR_XIR_LAYOUT_PARAMETER, &layout) == XR_XIR_OK);
    CHECK(layout.size == 16 && layout.alignment == 8);
    CHECK(xr_xir_builtin_layout(XR_XIR_UNIT, &target, XR_XIR_LAYOUT_PARAMETER, &layout) == XR_XIR_BAD_LAYOUT);
    XrXirFunctionLayout *physical = (XrXirFunctionLayout *) xr_xir_compile_artifact_layout(artifact, 0);
    CHECK(physical->frame_bytes == 48 && physical->slot_count == 9);
    uint32_t *offsets = (uint32_t *) physical->offsets;
    CHECK(offsets[0] == 0 && offsets[3] == UINT32_MAX);
    offsets[0] = 1;
    CHECK(xr_xir_compile_artifact_verify(artifact, NULL) == XR_XIR_BAD_LAYOUT);
    XrXirRunContext context = {10, 48, 0, 0, 0, 0};
    XrXirValue result;
    CHECK(xr_xir_compile_vm_run(artifact, 0, &context, NULL, 0, &result) == XR_XIR_RUN_BAD_ARTIFACT);
    CHECK(context.allocations == 0);
    offsets[0] = 0;
    ++physical->frame_bytes;
    CHECK(xr_xir_compile_artifact_verify(artifact, NULL) == XR_XIR_BAD_LAYOUT);
    --physical->frame_bytes;
    ++physical->result.size;
    CHECK(xr_xir_compile_artifact_verify(artifact, NULL) == XR_XIR_BAD_LAYOUT);
    --physical->result.size;
    XrXirLayout *parameters = (XrXirLayout *) physical->parameters;
    ++parameters[0].alignment;
    CHECK(xr_xir_compile_artifact_verify(artifact, NULL) == XR_XIR_BAD_LAYOUT);
    --parameters[0].alignment;
    CHECK(xr_xir_compile_artifact_verify(artifact, NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_vm_run(artifact, 4, &context, NULL, 0, &result) == XR_XIR_RUN_OK);
    xr_xir_compile_artifact_free(artifact);
    artifact = NULL;
    CHECK(result.type == XR_XIR_I64 && result.payload == INT64_MIN);

    XrXirArtifact *checked = fixture_checked();
    CHECK(xr_xir_compile_vm_run(checked, 0, &context, NULL, 0, &result) == XR_XIR_RUN_BAD_ARTIFACT);
    XrXirTarget invalid = {0, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(checked, &invalid, &artifact, NULL) == XR_XIR_BAD_LAYOUT);
    CHECK(!artifact);
    invalid = (XrXirTarget) {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION + 1};
    CHECK(xr_xir_compile_lower(checked, &invalid, &artifact, NULL) == XR_XIR_BAD_LAYOUT);
    CHECK(!artifact);
    xr_xir_compile_artifact_free(checked);
    /* Lowering inherits the checked artifact limits, not the caller context. */
    uint64_t frame_limit = scalar_owner.context.limits.frame_bytes;
    scalar_owner.context.limits.frame_bytes = 47;
    checked = fixture_checked();
    scalar_owner.context.limits.frame_bytes = frame_limit;
    CHECK(xr_xir_compile_lower(checked, &target, &artifact, NULL) == XR_XIR_BUDGET);
    CHECK(!artifact);
    xr_xir_compile_artifact_free(checked);
    scalar_compile_owner_free(&scalar_owner);
    puts("XIR VM scalar expectations and layout rejection passed");
    return 0;
}
