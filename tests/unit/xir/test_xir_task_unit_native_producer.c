/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 */
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_emit_c.h"
#include "xir/xxir_task.h"
#include "xir/xxir_types.h"
#include "xir/xxir_error.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_go_source_native_compile_owner.h"
#include "xir_task_unit_native_oracles.h"
#include "xir_task_unit_native_pipeline.h"
int main(int argc, char **argv) {
    CHECK(argc == 2); char path[2048], symbol[128];
    CHECK(snprintf(path, sizeof(path), "%s/task_unit_generated.h", argv[1]) > 0);
    FILE *header = fopen(path, "wb"); CHECK(header);
    for (unsigned i = 0; i < sizeof(unit_native_oracles) / sizeof(*unit_native_oracles); ++i) {
        uint32_t entry = UINT32_MAX;
        XrXirArtifact *lowered = unit_native_lower(&unit_native_oracles[i], &entry);
        CHECK(snprintf(symbol, sizeof(symbol), "task_unit_native_%s", unit_native_oracles[i].name) > 0);
        XrXirCSource c = {0}; CHECK(xr_xir_compile_emit_c(lowered, symbol, 4194304, &c) == XR_XIR_OK);
        xr_xir_compile_artifact_free(lowered);
        CHECK(c.text && !c.text[c.length] && !strstr(c.text, "({"));
        CHECK(snprintf(path, sizeof(path), "%s/%s.c", argv[1], symbol) > 0);
        FILE *file = fopen(path, "wb"); CHECK(file);
        CHECK(fwrite(c.text, 1, c.length, file) == c.length && !fclose(file));
        CHECK(fprintf(header, "XR_DATA const XrXirProgramSpec %s_program;\n", symbol) > 0);
        printf("UNIT_NATIVE_SOURCE %s entry=%u bytes=%zu normalUnitNoRESULT=1\n", unit_native_oracles[i].name, entry, c.length);
        xr_xir_compile_c_source_free(&c);
        if (i == 7) {
            CHECK(fprintf(header, "static const uint32_t task_unit_native_entries[8] = {") > 0);
        }
        /* Entry identities are kept separately until the declarations are complete. */
        static uint32_t entries[8]; entries[i] = entry;
        if (i == 7) {
            for (unsigned n = 0; n < 8; ++n) CHECK(fprintf(header, "%s%uu", n ? ", " : "", entries[n]) > 0);
            CHECK(fprintf(header, "};\nstatic const XrXirProgramSpec *const task_unit_native_specs[8] = {") > 0);
            for (unsigned n = 0; n < 8; ++n) CHECK(fprintf(header, "%s&task_unit_native_%s_program", n ? ", " : "", unit_native_oracles[n].name) > 0);
            CHECK(fprintf(header, "};\n") > 0);
        }
    }
    CHECK(!fclose(header)); effects_source_owners_free(); CHECK(!effects_compile_live && !effects_compile_bytes); return 0;
}
