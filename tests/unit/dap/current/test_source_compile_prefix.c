/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_compile_prefix.c - Original finite compiler feedback before runtime FI
 */
#define main source_prefix_original_checks
#include "../../xir/test_xir_source.c"
#undef main
/* The full original entry keeps every runtime and fault-injection obligation.
 * This entry stops at its unchanged finite Source-to-Lowered boundary. */
int main(void) {
    source_default_runtime_gaps();
    const XrXirCompileContext context=*source_fixture_source_owner(
        UINT64_C(64)*1024*1024,UINT64_C(128000000));
    XrXirArtifact *lowered=source_fixture_lower(&context,NULL);
    xr_xir_compile_artifact_free(lowered);source_fixture_source_owners_free();
    CHECK(!runtime_live && !runtime_bytes);
    puts("Original Source compiler prefix passed; complete runtime FI remains a separate full entry");
    return 0;
}
