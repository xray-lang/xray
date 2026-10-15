/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_task_result_producer.c - Real two-module compound Task code
 */
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_emit_c.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "Task result producer %d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_effect_execution_owner.h"
#include "xir_mixed_task_source_prepare.h"
int main(int argc, char **argv) {
    CHECK(argc == 2);
    const XrXirCompileContext *context = effects_source_owner(UINT64_C(64)*1024*1024, UINT64_C(128000000));
    TaskSourcePrepare state = {.context=context}; CHECK(task_source_prepare(&state));
    xr_xir_compile_artifact_free(state.lowered); state.lowered = NULL;
    FILE *file = fopen(argv[1], "wb"); CHECK(file && fwrite(state.generated.text, 1, state.generated.length, file) == state.generated.length);
    CHECK(fprintf(file, "\nconst uint32_t p20_task_result_entries[3] = {%uu,%uu,%uu};\n", state.entries[0], state.entries[1], state.entries[2]) > 0);
    CHECK(!fclose(file));
    printf("P20 compound Task real Source modules=%u C11bytes=%zu SourceDead independentPacketDead VMexecuted=0\n",
        (unsigned)XR_MIXED_TASK_RESULT_MODULES, state.generated.length);
    task_source_cleanup(&state); effects_source_owners_free(); return 0;
}
