/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_native_metadata_fixture.h - Metadata witnesses for trusted test callbacks
 *
 * KEY CONCEPT:
 *   Synthetic faulting bodies establish metadata only; callbacks are tested separately.
 */
#ifndef XIR_NATIVE_METADATA_FIXTURE_H
#define XIR_NATIVE_METADATA_FIXTURE_H
#include "xir/xxir_generic.h"
#include "xir_native_fixture_owner.h"
static XrXirStatus native_metadata_fixture(const XrXirCompileContext *context,
    const XrXirProgramSpec *spec, XrXirArtifact **output) {
    if (!context || !context->resources || !spec || !output || *output) return XR_XIR_BAD_STRUCTURE;
    CHECK(spec->entry_count <= 16);
    XrXirFunction functions[16] = {0};
    XrXirInstruction instructions[16][2] = {0};
    char names[16][24];
    const XrXirBlock block = {0, 2, 0, 0};
    for (uint32_t i = 0; i < spec->entry_count; ++i) {
        const XrXirCallEntry *entry = &spec->entries[i];
        int length = snprintf(names[i], sizeof(names[i]), "native_test_%u", i);
        CHECK(length > 0 && (size_t)length < sizeof(names[i]));
        instructions[i][0] = (XrXirInstruction) {XR_XIR_CONST_INT, XR_XIR_I64, {0}, {0}, 0, {0}};
        instructions[i][1] = (XrXirInstruction) {XR_XIR_MATCH_FAIL, XR_XIR_UNIT, {0}, {0}, 0, {0}};
        functions[i] = (XrXirFunction) {names[i], (uint32_t)length, entry->parameters,
            entry->parameter_count, entry->result, &block, 1, instructions[i], 2, NULL, 0};
    }
    XrXirModule module = {XR_XIR_BUILT, functions, spec->entry_count, spec->declarations, NULL, spec->types, NULL, XR_XIR_PROGRAM, NULL};
    XrXirArtifact *checked = NULL;
    XrXirStatus status = xr_xir_compile_check(context, &module, &checked, NULL);
    XrXirArtifact *closed=NULL;
    if (status == XR_XIR_OK) status=xr_xir_compile_specialize(checked,&closed,NULL);
    if (status == XR_XIR_OK) status = xr_xir_compile_artifact_verify(closed, NULL);
    if (status == XR_XIR_OK) status = xr_xir_compile_lower(closed, &spec->target, output, NULL);
    xr_xir_compile_artifact_free(closed);
    xr_xir_compile_artifact_free(checked);
    return status;
}
#endif // XIR_NATIVE_METADATA_FIXTURE_H
