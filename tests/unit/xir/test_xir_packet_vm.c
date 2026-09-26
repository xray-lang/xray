/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_packet_vm.c - Source-free Checked consumer and native fixture producer
 *
 * KEY CONCEPT:
 *   The consumer links neither parser nor source compiler and needs only a packet.
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_emit_c.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_source_cases.h"
int main(int argc, char **argv) {
    CHECK(argc >= 1 && argc <= 3);
    FILE *file = fopen(argc == 3 ? argv[2] : XR_CHECKED_FIXTURE, "rb"); CHECK(file);
    CHECK(fseek(file, 0, SEEK_END) == 0);
    long size = ftell(file); CHECK(size > 0 && size < 262144);
    CHECK(fseek(file, 0, SEEK_SET) == 0);
    uint8_t *bytes = xr_malloc((size_t) size); CHECK(bytes);
    CHECK(fread(bytes, 1, (size_t) size, file) == (size_t) size && fclose(file) == 0);
    XrXirArtifact *checked = NULL, *lowered = NULL;
    CHECK(xr_xir_checked_read(bytes, (size_t) size, NULL, &checked, NULL) == XR_XIR_OK);
    memset(bytes, 0xCC, (size_t) size); xr_free(bytes);
    XrXirArtifact *specialized = NULL;
    CHECK(xr_xir_specialize(checked, NULL, &specialized, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(checked); checked = specialized;
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_lower(checked, &target, NULL, &lowered, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(checked);
    const XrXirModule *module = xr_xir_artifact_module(lowered);
    uint32_t result = UINT32_MAX, advance = UINT32_MAX;
    for (uint32_t i = 0; i < module->function_count; ++i) {
        if (module->functions[i].name_length == 6 && !memcmp(module->functions[i].name, "result", 6)) result = i;
        if (module->functions[i].name_length == 7 && !memcmp(module->functions[i].name, "advance", 7)) advance = i;
    }
    CHECK(result != UINT32_MAX && advance != UINT32_MAX);
    uint32_t entry = module->declarations->entry_function;
    XrXirCSource source;
    CHECK(xr_xir_emit_c(lowered, "fixture_source", 262144, &source) == XR_XIR_OK);
    if (argc >= 2) {
        file = fopen(argv[1], "wb"); CHECK(file);
        CHECK(fwrite(source.text, 1, source.length, file) == source.length);
        CHECK(fprintf(file, "\nconst uint32_t fixture_source_result = %uu;\nconst uint32_t fixture_source_advance = %uu;\n", result, advance) > 0);
        CHECK(fclose(file) == 0);
    }
    xr_xir_c_source_free(&source);
    XrXirProgram *program = NULL;
    CHECK(xr_xir_vm_program_take(&lowered, 262144, &program) == XR_XIR_OK && !lowered);
    XrXirValue results[2] = {{0}, {0}};
    source_pair(program, entry, result, advance, results);
    xr_xir_program_drop(program);
    source_result_drop(&results[0]); source_result_drop(&results[1]);
    puts("Source-free Checked consumer matched independent VM expectations and emitted native C");
    return 0;
}
