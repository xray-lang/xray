/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_float_pipeline.h - One Checked specialization and lowering path
 */
#ifndef XIR_SOURCE_FLOAT_PIPELINE_H
#define XIR_SOURCE_FLOAT_PIPELINE_H
#include "xir/xxir_generic.h"
static XrXirArtifact *source_float_lower(XrXirArtifact *checked) {
    const XrXirCompileContext *context = xr_xir_compile_artifact_context(checked);
    CHECK(context && context->resources);
    XrCompileResources *resources = context->resources;
    XrXirArtifact *specialized = NULL, *lowered = NULL;
    XrXirDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_compile_specialize(checked, &specialized, &diagnostic);
    if (status != XR_XIR_OK) fprintf(stderr, "float specialization %u f%u i%u\n", status, diagnostic.function, diagnostic.instruction);
    CHECK(status == XR_XIR_OK);
    CHECK(xr_xir_compile_artifact_context(specialized)->resources == resources);
    xr_xir_compile_artifact_free(checked);
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(specialized, &target, &lowered, NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_artifact_context(lowered)->resources == resources);
    xr_xir_compile_artifact_free(specialized); return lowered;
}
static void source_float_find(const XrXirModule *module, uint32_t *functions) {
    static const char *const names[] = {NULL, "result", "advance", "current", "captured", "suspended"};
    functions[FLOAT_ENTRY] = module->declarations->entry_function;
    for (unsigned n = 1; n < FLOAT_FUNCTION_COUNT; ++n) {
        functions[n] = UINT32_MAX;
        for (uint32_t f = 0; f < module->function_count; ++f)
            if (module->functions[f].name_length == strlen(names[n]) &&
                !memcmp(module->functions[f].name, names[n], strlen(names[n]))) functions[n] = f;
        CHECK(functions[n] != UINT32_MAX);
    }
}
#endif // XIR_SOURCE_FLOAT_PIPELINE_H
