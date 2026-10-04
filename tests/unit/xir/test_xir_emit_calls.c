/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_emit_calls.c - Generate real resumable C for host compilation
 *
 * KEY CONCEPT:
 *   Suspensions and calls are emitted from the same verified instruction graphs.
 */
#include "xir/xxir_emit_c.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_source_fixture_owner.h"
#include "xir_call_fixture.h"
#include "xir_native_inbox_fixture.h"
int main(int argc, char **argv) {
    SourceFixtureOwner compiler = {0}; source_fixture_owner_new(&compiler);
    FILE *file = argc == 2 ? fopen(argv[1], "wb") : NULL;
    CHECK(argc == 1 || (argc == 2 && file));
    for (uint32_t mode = 0; mode < 14; ++mode) {
        XrXirArtifact *artifact = call_fixture(&compiler.context, mode);
        XrXirCSource source = {0};
        CHECK(xr_xir_compile_emit_leaf_c(artifact, "leaf", 65536, &source) == XR_XIR_BAD_STAGE);
        CHECK(!source.text && !source.length);
        char prefix[32];
        CHECK(snprintf(prefix, sizeof(prefix), "fixture_calls%u", mode) > 0);
        CHECK(xr_xir_compile_emit_c(artifact, prefix, 1, &source) == XR_XIR_BUDGET);
        CHECK(!source.text && !source.length);
        CHECK(xr_xir_compile_emit_c(artifact, prefix, 65536, &source) == XR_XIR_OK);
        xr_xir_compile_artifact_free(artifact);
        CHECK(!strstr(source.text, "({"));
        if (mode == 2)
            CHECK(strstr(source.text, "xr_xir_call_numeric_fault(numeric_status, false)"));
        if (file) CHECK(fwrite(source.text, 1, source.length, file) == source.length);
        xr_xir_compile_c_source_free(&source);
    }
    for (uint32_t mode=0;mode<5;++mode) {
        XrXirArtifact *artifact=native_inbox_fixture(&compiler.context,mode);
        XrXirCSource source={0}; char prefix[32];
        CHECK(snprintf(prefix,sizeof(prefix),"fixture_inbox%u",mode)>0);
        CHECK(xr_xir_compile_emit_c(artifact,prefix,65536,&source)==XR_XIR_OK);
        xr_xir_compile_artifact_free(artifact);
        CHECK(!strstr(source.text,"({"));
        if (file) CHECK(fwrite(source.text,1,source.length,file)==source.length);
        xr_xir_compile_c_source_free(&source);
    }
    if (file) CHECK(fclose(file) == 0);
    source_fixture_owner_free(&compiler);
    puts("Resumable C generation and mandatory output verification passed");
    return 0;
}
