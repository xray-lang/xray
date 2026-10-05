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
static XrXirArtifact *enum_checked_fixture(const XrXirCompileContext *context) {
    XrXirArtifact *base = checked_fixture(context), *checked = NULL;
    XrXirModule built = *xr_xir_compile_artifact_module(base); built.stage = XR_XIR_BUILT;
    EnumMetadataFixture f; enum_metadata_fixture(&f);
    XrXirTypes types = {NULL, 0, &f.table, NULL}; built.types = &types;
    CheckedAtomicPool atomic_pool;checked_atomic_pool(&built,&atomic_pool);
    CHECK(xr_xir_compile_check(context, &built, &checked, NULL) == XR_XIR_OK);
    memset(&f, 0xCC, sizeof(f)); xr_xir_compile_artifact_free(base);base=NULL;
    return checked;
}
#endif // XIR_ENUM_CHECKED_FIXTURE_H
