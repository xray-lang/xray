/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_task_constructed_emit.c - Actual constructed-result native bodies
 */
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_emit_c.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "constructed producer FAIL %d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_effect_execution_owner.h"
#include "xir_task_constructed_pipeline.h"
int main(int argc, char **argv) {
    CHECK(argc == 2 || argc == 3); bool baseline = argc == 2 && !strcmp(argv[1], "baseline"); CHECK(baseline || argc == 3);
    const XrXirCompileContext *context = effects_source_owner(UINT64_C(67108864), UINT64_C(128000000));
    uint32_t entries[CONSTRUCTED_CASES][2] = {{0}}; XrXirArtifact *lowered = constructed_lower(context, entries, baseline);
    if (baseline) { effects_source_owners_free(); return 0; }
    XrXirCSource source = {0}; CHECK(xr_xir_compile_emit_c(lowered, "task_constructed_native", 4194304, &source) == XR_XIR_OK);
    xr_xir_compile_artifact_free(lowered); CHECK(source.text && !strstr(source.text, "({"));
    FILE *file = fopen(argv[1], "wb"); CHECK(file);
    CHECK(fwrite(source.text, 1, source.length, file) == source.length && !fclose(file));
    file = fopen(argv[2], "wb"); CHECK(file);
    CHECK(fprintf(file, "XR_DATA const XrXirProgramSpec task_constructed_native_program;\nstatic const uint32_t constructed_entries[%u][2]={\n", CONSTRUCTED_CASES) > 0);
    for (unsigned i = 0; i < CONSTRUCTED_CASES; ++i) CHECK(fprintf(file, "{%uu,%uu},\n", entries[i][0], entries[i][1]) > 0);
    CHECK(fputs("};\n", file) >= 0 && !fclose(file));
    printf("TASK_CONSTRUCTED_GENERATED families=%u C11bytes=%zu VMexecuted=0\n", CONSTRUCTED_CASES, source.length);
    xr_xir_compile_c_source_free(&source); effects_source_owners_free(); return 0;
}
