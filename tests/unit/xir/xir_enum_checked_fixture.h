/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_enum_checked_fixture.h - Enum declarations without premature value admission
 */
#ifndef XIR_ENUM_CHECKED_FIXTURE_H
#define XIR_ENUM_CHECKED_FIXTURE_H
#include "xir_checked_fixture.h"
#include "xir_enum_metadata_fixture.h"
static XrXirArtifact *enum_checked_fixture(void) {
    XrXirArtifact *base = checked_fixture(), *checked = NULL;
    XrXirModule built = *xr_xir_artifact_module(base); built.stage = XR_XIR_BUILT;
    EnumMetadataFixture f; enum_metadata_fixture(&f);
    XrXirTypes types = {NULL, 0, &f.table}; built.types = &types;
    CHECK(xr_xir_check(&built, NULL, &checked, NULL) == XR_XIR_OK);
    memset(&f, 0xCC, sizeof(f)); xr_xir_artifact_free(base);
    return checked;
}
#endif // XIR_ENUM_CHECKED_FIXTURE_H
