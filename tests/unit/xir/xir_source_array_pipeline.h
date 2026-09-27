/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_array_pipeline.h - One Checked specialization and lowering path
 */
#ifndef XIR_SOURCE_ARRAY_PIPELINE_H
#define XIR_SOURCE_ARRAY_PIPELINE_H
#include "xir/xxir_generic.h"
static XrXirArtifact *source_array_lower(XrXirArtifact *checked) {
    XrXirArtifact *specialized = NULL, *lowered = NULL;
    XrXirDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_specialize(checked, NULL, &specialized, &diagnostic);
    if (status != XR_XIR_OK) fprintf(stderr, "array specialization %u f%u i%u\n", status, diagnostic.function, diagnostic.instruction);
    CHECK(status == XR_XIR_OK); xr_xir_artifact_free(checked);
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_lower(specialized, &target, NULL, &lowered, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(specialized); return lowered;
}
static void source_array_find(const XrXirModule *module, uint32_t *functions) {
    static const char *const names[] = {NULL, "result", "originalFirst", "advance", "rebound", "snapshot",
        "methodSnapshot", "reboundFault", "current", "assignResult", "local", "captured", "generic", "nested",
        "importedLength", "localLength", "selfPush", "setOrder", "orderTrace", "suspendedSnapshot", "suspendedSet"};
    functions[ARRAY_ENTRY] = module->declarations->entry_function;
    for (unsigned n = 1; n < ARRAY_FUNCTION_COUNT; ++n) {
        functions[n] = UINT32_MAX;
        for (uint32_t f = 0; f < module->function_count; ++f)
            if (module->functions[f].name_length == strlen(names[n]) &&
                !memcmp(module->functions[f].name, names[n], strlen(names[n]))) functions[n] = f;
        CHECK(functions[n] != UINT32_MAX);
    }
}
#endif // XIR_SOURCE_ARRAY_PIPELINE_H
