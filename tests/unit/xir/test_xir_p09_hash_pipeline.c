/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_hash_pipeline.c - Checked replay, canonical lowering and C11 production
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_source_fixture_owner.h"
#include "p09_hash_pipeline.h"
#include "p09_hash_program.h"
int main(int argc, char **argv) {
    SourceFixtureOwner owner = {0}; source_fixture_owner_new(&owner);
    XrXirArtifact *lowered = hash_pipeline_lower(&owner.context);
    if (argc == 2) {
        XrXirCSource source = {0};
        CHECK(xr_xir_compile_emit_c(lowered, "p09_hash", 1048576, &source) == XR_XIR_OK);
        CHECK(!strstr(source.text, "({")); FILE *file = fopen(argv[1], "wb"); CHECK(file);
        CHECK(fwrite(source.text, 1, source.length, file) == source.length && !fclose(file));
        printf("Hash descriptor real generated portable C11 bytes=%zu\n", source.length);
        xr_xir_compile_c_source_free(&source); xr_xir_compile_artifact_free(lowered);
    } else {
        XrXirProgram *program = NULL;
        CHECK(xr_xir_compile_vm_program_take(&lowered, &program) == XR_XIR_OK && !lowered);
        hash_program_run(program);
    }
    source_fixture_owner_free(&owner);
    puts("Map/Set descriptor Checked replay, specialization, layout and late owned call PASS"); return 0;
}
