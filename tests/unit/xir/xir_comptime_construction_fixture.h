/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_comptime_construction_fixture.h - Rebuild authentic compile-time facts
 *
 * KEY CONCEPT:
 *   Keep the live source owner's nominal and materialization facts while
 *   replacing only the caller-supplied compile-time table.
 */
#ifndef XIR_COMPTIME_CONSTRUCTION_FIXTURE_H
#define XIR_COMPTIME_CONSTRUCTION_FIXTURE_H
#include "xir/xxir_internal.h"
#include "xir/xxir_construction_internal.h"

static XrXirStatus xir_fixture_comptime_construction(const XrXirCompileContext *context,
    const XrXirArtifact *owner, const XrXirComptimeTable *table, XrXirConstruction **output) {
    const XrXirConstruction *original = owner->construction;
    const XrXirConstructionSpec input = {original->rows, original->count,
        original->materializations, original->materialization_count, table};
    // The source owner stays live until the receiving constructor copies every vector.
    return xr_xir_compile_construction_new_v4(context, owner->module.types, &input,xr_xir_compile_construction_slots(original), output);
}
#endif // XIR_COMPTIME_CONSTRUCTION_FIXTURE_H
