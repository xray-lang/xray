/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_nominal_chain_fixture.h - Forward and repeated nominal field edges
 */
#ifndef XIR_NOMINAL_CHAIN_FIXTURE_H
#define XIR_NOMINAL_CHAIN_FIXTURE_H
#include "xir_checked_fixture.h"
#include "xir/xxir_nominal.h"
#include "xir/xxir_generic.h"
static XrXirArtifact *nominal_chain_fixture(uint32_t depth, uint32_t field_count) {
    CHECK(depth && depth <= 160 && field_count && field_count <= 2);
    XrXirArtifact *base = checked_fixture(), *checked = NULL;
    XrXirModule built = *xr_xir_artifact_module(base); built.stage = XR_XIR_BUILT;
    char names[160][16];
    XrXirNominalField fields[160][2];
    XrXirNominalDeclaration declarations[160];
    XrXirTypeNode nodes[160];
    for (uint32_t i = 0; i < depth; ++i) {
        int length = snprintf(names[i], sizeof(names[i]), "Value%u", i);
        CHECK(length > 0 && (size_t) length < sizeof(names[i]));
        XrXirType field = i + 1 == depth ? XR_XIR_STRING : (XrXirType) (XR_XIR_CONSTRUCTED_TYPE_BASE + i + 1);
        fields[i][0] = (XrXirNominalField) {{"left", 4}, field, 0};
        fields[i][1] = (XrXirNominalField) {{"right", 5}, field, XR_XIR_FIELD_PRIVATE};
        declarations[i] = (XrXirNominalDeclaration) {{"alpha", 5}, {names[i], (uint32_t) length},
            0, NULL, 0, fields[i], field_count, XR_XIR_NOMINAL_STRUCT, NULL, 0};
        nodes[i] = (XrXirTypeNode) {XR_XIR_TYPE_NOMINAL, XR_XIR_UNIT, NULL, 0, XR_XIR_UNIT, 0, 0,
            {i, NULL, 0, NULL, 0}};
    }
    XrXirNominalTable table = {declarations, depth, NULL};
    XrXirTypes types = {nodes, depth, &table}; built.types = &types;
    CHECK(xr_xir_check(&built, NULL, &checked, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(base);
    memset(names, 0xCC, sizeof(names)); memset(fields, 0xCC, sizeof(fields));
    return checked;
}
static inline XrXirArtifact *nominal_chain_lowered(void) {
    XrXirArtifact *checked = nominal_chain_fixture(3, 2), *closed = NULL, *lowered = NULL;
    CHECK(xr_xir_specialize(checked, NULL, &closed, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(checked);
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_lower(closed, &target, NULL, &lowered, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(closed);
    return lowered;
}
#endif // XIR_NOMINAL_CHAIN_FIXTURE_H
