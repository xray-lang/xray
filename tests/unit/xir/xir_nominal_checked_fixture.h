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
static XrXirArtifact *nominal_checked_fixture(unsigned mode) {
    XrXirArtifact *base = checked_fixture(), *checked = NULL;
    XrXirModule built = *xr_xir_artifact_module(base); built.stage = XR_XIR_BUILT;
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
    XrXirFunctionIdentity identities[9];
    CHECK(built.function_count == 9);
    memcpy(identities, built.declarations->functions, sizeof(identities));
    if (mode == 3) { identities[4].nominal_owner = 1; identities[4].method_kind = XR_XIR_MEMBER_HELPER; }
    XrXirDeclarations declarations = *built.declarations;
    declarations.functions = identities; built.declarations = &declarations;
    CHECK(xr_xir_check(&built, NULL, &checked, NULL) == XR_XIR_OK && checked);
    memset(&f, 0xCC, sizeof(f)); memset(nodes, 0xCC, sizeof(nodes)); memset(arguments, 0xCC, sizeof(arguments));
    xr_xir_artifact_free(base);
    CHECK(xr_xir_artifact_verify(checked, NULL, NULL) == XR_XIR_OK);
    return checked;
}
static inline XrXirArtifact *nominal_lowered_fixture(unsigned mode) {
    XrXirArtifact *checked = nominal_checked_fixture(mode), *closed = NULL, *lowered = NULL;
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_specialize(checked, NULL, &closed, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(checked);
    CHECK(xr_xir_lower(closed, &target, NULL, &lowered, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(closed);
    CHECK(xr_xir_artifact_verify(lowered, NULL, NULL) == XR_XIR_OK);
    return lowered;
}
#endif // XIR_NOMINAL_CHECKED_FIXTURE_H
