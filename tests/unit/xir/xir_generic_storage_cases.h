/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_generic_storage_cases.h - Intrinsic storage from authentic generic owners
 *
 * KEY CONCEPT:
 *   Intrinsic copy/save is independent of interface and Sendable obligations.
 */
#ifndef XIR_GENERIC_STORAGE_CASES_H
#define XIR_GENERIC_STORAGE_CASES_H
#include "xir/xxir_constraint_proof.h"
#include "xir/xxir_types.h"
#include "xir_source_fixture_owner.h"
static void generic_storage_cases(void) {
    XrXirConstraint facts[2]={{XR_XIR_CONSTRAINT_SENDABLE,NULL,0},{0}};
    XrXirConstraint requirements[2]={{0},{XR_XIR_CONSTRAINT_SENDABLE,NULL,0}};
    XrXirGeneric generic={0}; generic.constraints=facts; generic.parameter_count=2;
    XrXirFunction function={0};
    XrXirNominalDeclaration declarations[2]={0};
    declarations[0].constraints=facts; declarations[0].parameter_count=2;
    declarations[1].constraints=requirements; declarations[1].parameter_count=2;
    XrXirNominalTable table={declarations,2,NULL};
    XrXirType p0=XR_XIR_TYPE_PARAMETER_BASE,p1=XR_XIR_TYPE_PARAMETER_BASE+1;
    XrXirType arguments[]={p1,p0};
    XrXirTypeNode nodes[2]={0};
    nodes[0].kind=XR_XIR_TYPE_ARRAY; nodes[0].element=p1; nodes[0].parameter_span=2;
    nodes[1].kind=XR_XIR_TYPE_NOMINAL; nodes[1].parameter_span=2;
    nodes[1].nominal=(XrXirNominalType){1,arguments,2,NULL,0};
    XrXirTypes types={nodes,2,&table,NULL};
    XrXirModule module={0}; module.types=&types; module.functions=&function;
    module.function_count=1; module.generics=&generic;
    XrXirProofContext context={&module,{XR_XIR_CONTEXT_FUNCTION,0,0}};
    SourceFixtureOwner owner = {0}; source_fixture_owner_new(&owner);
    XrXirCompileContext original = owner.context; XrCompileResourceStats stats = {0};
    CHECK(xr_xir_compile_type_storage_prove(&owner.context, &context, p1)==XR_XIR_OK);
    CHECK(!memcmp(&owner.context, &original, sizeof(original)));
    CHECK(xr_compile_resources_stats(owner.context.resources, &stats) == XR_COMPILE_RESOURCE_OK);
    CHECK(stats.live_bytes == owner.baseline.live_bytes);
    CHECK(xr_xir_compile_type_markers_prove(&owner.context, &context, p1, XR_XIR_CONSTRAINT_SENDABLE)==XR_XIR_BAD_TYPE);
    CHECK(xr_xir_compile_type_storage_prove(&owner.context, &context, XR_XIR_CONSTRUCTED_TYPE_BASE)==XR_XIR_OK);
    CHECK(xr_xir_compile_type_storage_prove(&owner.context, &context, XR_XIR_CONSTRUCTED_TYPE_BASE+1)==XR_XIR_OK);
    arguments[0]=p0;arguments[1]=p1;
    CHECK(xr_xir_compile_type_storage_prove(&owner.context, &context, XR_XIR_CONSTRUCTED_TYPE_BASE+1)==XR_XIR_BAD_TYPE);
    CHECK(!memcmp(&owner.context, &original, sizeof(original)));
    CHECK(xr_compile_resources_stats(owner.context.resources, &stats) == XR_COMPILE_RESOURCE_OK);
    CHECK(stats.live_bytes == owner.baseline.live_bytes);
    context.owner=(XrXirDeclarationContext){XR_XIR_CONTEXT_NOMINAL,0,0};
    CHECK(xr_xir_compile_type_storage_prove(&owner.context, &context, p1)==XR_XIR_OK);
    context.owner.declaration=2;
    CHECK(xr_xir_compile_type_storage_prove(&owner.context, &context, p1)==XR_XIR_BAD_STRUCTURE);
    context.owner=(XrXirDeclarationContext){XR_XIR_CONTEXT_CLOSED,0,0};
    CHECK(xr_xir_compile_type_storage_prove(&owner.context, &context, p1)==XR_XIR_BAD_TYPE);
    context.owner=(XrXirDeclarationContext){XR_XIR_CONTEXT_FUNCTION,0,0};
    generic.parameter_count=1;
    CHECK(xr_xir_compile_type_storage_prove(&owner.context, &context, p1)==XR_XIR_BAD_TYPE);
    generic.parameter_count=2;
    CHECK(xr_xir_compile_type_storage_prove(&owner.context, &context, XR_XIR_UNIT)==XR_XIR_BAD_TYPE);
    CHECK(xr_xir_compile_type_markers_prove(&owner.context, &context, XR_XIR_UNIT, XR_XIR_CONSTRAINT_SENDABLE)==XR_XIR_OK);
    CHECK(xr_xir_compile_type_markers_prove(&owner.context, &context, XR_XIR_UNIT, XR_XIR_CONSTRAINT_ERROR)==XR_XIR_BAD_TYPE);
    XrCompileResourceStats baseline = owner.baseline; source_fixture_owner_free(&owner);
    /* A zero work allowance rejects even a scalar proof. No scratch room
     * still admits scalar proof, but rejects the constructed obligation. */
    for (unsigned mode = 0; mode < 2; ++mode) {
        const XrCompileResourceLimits limits = {
            UINT64_C(64) * 1024 * 1024,
            mode ? baseline.live_bytes : UINT64_C(8) * 1024 * 1024,
            mode ? UINT64_C(128000000) : baseline.work};
        XrXirCompileContext limited = {0}; limited.limits = xr_xir_compile_default_limits();
        CHECK(xr_compile_resources_new(&limits, &limited.resources) == XR_COMPILE_RESOURCE_OK);
        XrCompileResourceStats before = {0}, after = {0};
        CHECK(xr_compile_resources_stats(limited.resources, &before) == XR_COMPILE_RESOURCE_OK);
        CHECK(xr_xir_compile_type_storage_prove(&limited, &context, p1) == (mode ? XR_XIR_OK : XR_XIR_BUDGET));
        if (mode) CHECK(xr_xir_compile_type_storage_prove(&limited, &context, XR_XIR_CONSTRUCTED_TYPE_BASE) == XR_XIR_BUDGET);
        CHECK(xr_compile_resources_stats(limited.resources, &after) == XR_COMPILE_RESOURCE_OK);
        CHECK(after.live_bytes == before.live_bytes && after.allocated_bytes == before.allocated_bytes);
        xr_compile_resources_release(limited.resources);
    }
}
#endif /* XIR_GENERIC_STORAGE_CASES_H */
