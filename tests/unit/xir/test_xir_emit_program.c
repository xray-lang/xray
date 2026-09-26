/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_emit_program.c - Emit compiler-owned native program descriptors
 *
 * KEY CONCEPT:
 *   Native tests consume emitted code and data after all compiler owners die.
 */
#include "xir/xxir_emit_c.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_program_fixture.h"
int main(int argc, char **argv) {
    FILE *file = argc == 2 ? fopen(argv[1], "wb") : NULL;
    CHECK(argc == 1 || (argc == 2 && file));
    for (uint32_t mode = 0; mode < 3; ++mode) {
        XrXirArtifact *artifact = program_fixture(mode);
        char prefix[32]; CHECK(snprintf(prefix, sizeof(prefix), "program%u", mode) > 0);
        XrXirCSource source = {0};
        CHECK(xr_xir_emit_c(artifact, prefix, 200000, &source) == XR_XIR_OK);
        xr_xir_artifact_free(artifact);
        CHECK(strstr(source.text, "XrXirProgramSpec") && strstr(source.text, "xr_xir_instance_slot_write"));
        CHECK(!strstr(source.text, "xr_xir_vm") && !strstr(source.text, "({"));
        if (file) CHECK(fwrite(source.text, 1, source.length, file) == source.length);
        xr_xir_c_source_free(&source);
    }
    if (file) CHECK(fclose(file) == 0);
    puts("Native program code and immutable declarations emitted");
    return 0;
}
