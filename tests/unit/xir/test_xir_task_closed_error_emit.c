/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_task_closed_error_emit.c - Actual closed-error native bodies
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
#include "xir_task_closed_error_pipeline.h"
int main(int argc, char **argv) {
    CHECK(argc == 3);
    const XrXirCompileContext *context = effects_source_owner(UINT64_C(67108864), UINT64_C(128000000));
    uint32_t entries[CLOSED_ERROR_CASES][2] = {{0}}; XrXirArtifact *lowered = closed_error_lower(context, entries);
    XrXirCSource source = {0}; CHECK(xr_xir_compile_emit_c(lowered, "task_closed_error_native", 4194304, &source) == XR_XIR_OK);
    xr_xir_compile_artifact_free(lowered); CHECK(source.text && !strstr(source.text, "({"));
    FILE *file = fopen(argv[1], "wb"); CHECK(file);
    CHECK(fwrite(source.text, 1, source.length, file) == source.length && !fclose(file));
    file = fopen(argv[2], "wb"); CHECK(file);
    CHECK(fprintf(file, "XR_DATA const XrXirProgramSpec task_closed_error_native_program;\nstatic const uint32_t closed_error_entries[%u][2]={\n", CLOSED_ERROR_CASES) > 0);
    for (unsigned i = 0; i < CLOSED_ERROR_CASES; ++i) CHECK(fprintf(file, "{%uu,%uu},\n", entries[i][0], entries[i][1]) > 0);
    CHECK(fputs("};\n", file) >= 0 && !fclose(file));
    printf("TASK_CLOSED_ERROR_GENERATED families=%u C11bytes=%zu VMexecuted=0\n", CLOSED_ERROR_CASES, source.length);
    xr_xir_compile_c_source_free(&source); effects_source_owners_free(); return 0;
}
