/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_nominal_checked_fixture.h - Owned module-bound nominal packet fixture
 */
#ifndef XIR_NOMINAL_CHECKED_FIXTURE_H
#define XIR_NOMINAL_CHECKED_FIXTURE_H
#include "xir_checked_fixture.h"
#include "xir/xxir_generic.h"
#include "xir_nominal_fixture.h"
static XrXirArtifact *nominal_checked_fixture(const XrXirCompileContext *context, unsigned mode) {
    XrXirArtifact *base = checked_fixture(context), *checked = NULL;
    XrXirModule built = *xr_xir_compile_artifact_module(base); built.stage = XR_XIR_BUILT;
    NominalFixture f; nominal_fixture(&f);
    f.declarations[1].module = (XrXirLiteral) {"beta", 4};
    XrXirType arguments[] = {XR_XIR_I64, XR_XIR_STRING};
    XrXirTypeNode nodes[] = {
        {XR_XIR_TYPE_ARRAY, (XrXirType) XR_XIR_TYPE_PARAMETER_BASE, NULL, 0, XR_XIR_UNIT, 0, 1, {0}},
        {XR_XIR_TYPE_NOMINAL, XR_XIR_UNIT, NULL, 0, XR_XIR_UNIT, 0, 0, {0, arguments, 1, NULL, 0}},
        {XR_XIR_TYPE_NOMINAL, XR_XIR_UNIT, NULL, 0, XR_XIR_UNIT, 0, 0, {0, arguments + 1, 1, NULL, 0}},
        {XR_XIR_TYPE_NOMINAL, XR_XIR_UNIT, NULL, 0, XR_XIR_UNIT, 0, 0, {1, arguments, 1, NULL, 0}}};
    XrXirTypes types = {mode == 2 ? nodes + 1 : mode ? nodes : NULL, mode == 3 ? 4u : mode == 2 ? 3u : mode ? 1u : 0u, &f.table, NULL};
    if (mode == 1 || mode == 3) f.fields[0].type = (XrXirType) XR_XIR_CONSTRUCTED_TYPE_BASE;
    built.types = &types;
    CheckedAtomicPool atomic_pool;checked_atomic_pool(&built,&atomic_pool);
    XrXirFunctionIdentity identities[9];
    CHECK(built.function_count == 9);
    memcpy(identities, built.declarations->functions, sizeof(identities));
    if (mode == 3) { identities[4].nominal_owner = 1; identities[4].method_kind = XR_XIR_MEMBER_HELPER; }
    XrXirDeclarations declarations = *built.declarations;
    declarations.functions = identities; built.declarations = &declarations;
    CHECK(xr_xir_compile_check(context, &built, &checked, NULL) == XR_XIR_OK && checked);
    memset(&f, 0xCC, sizeof(f)); memset(nodes, 0xCC, sizeof(nodes)); memset(arguments, 0xCC, sizeof(arguments));
    xr_xir_compile_artifact_free(base);base=NULL;
    CHECK(xr_xir_compile_artifact_verify(checked, NULL) == XR_XIR_OK);
    return checked;
}
static inline XrXirArtifact *nominal_lowered_fixture(const XrXirCompileContext *context, unsigned mode) {
    XrXirArtifact *checked = nominal_checked_fixture(context, mode), *closed = NULL, *lowered = NULL;
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_specialize(checked, &closed, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);checked=NULL;
    CHECK(xr_xir_compile_lower(closed, &target, &lowered, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);closed=NULL;
    CHECK(xr_xir_compile_artifact_verify(lowered, NULL) == XR_XIR_OK);
    return lowered;
}
#endif // XIR_NOMINAL_CHECKED_FIXTURE_H
