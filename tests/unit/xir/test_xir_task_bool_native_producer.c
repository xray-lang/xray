/* Genuine Source pipeline; no artifact or GeneratedC identity fabrication. */
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_emit_c.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%d: %s\n", __LINE__, #x); exit(1); } } while (0)
#include "xir_go_source_native_compile_owner.h"
#include "xir_task_bool_source_pipeline.h"
int main(int argc, char **argv) {
    CHECK(argc == 3);
    const XrXirCompileContext *context = effects_source_owner(UINT64_C(67108864), UINT64_C(128000000));
    uint32_t entries[3] = {UINT32_MAX, UINT32_MAX, UINT32_MAX};
    XrXirArtifact *lowered = go_bool_source_lower(context, entries);
    XrXirCSource output = {0};
    CHECK(xr_xir_compile_emit_c(lowered, "go_bool_native", 4194304, &output) == XR_XIR_OK);
    xr_xir_compile_artifact_free(lowered);
    CHECK(output.text && !output.text[output.length] && !strstr(output.text, "({"));
    FILE *file = fopen(argv[1], "wb"); CHECK(file);
    CHECK(fwrite(output.text, 1, output.length, file) == output.length && !fclose(file));
    file = fopen(argv[2], "wb"); CHECK(file);
    CHECK(fprintf(file, "XR_DATA const XrXirProgramSpec go_bool_native_program;\n"
        "static const uint32_t go_bool_entries[3] = {%uu, %uu, %uu};\n",
        entries[0], entries[1], entries[2]) > 0 && !fclose(file));
    printf("GO_BOOL_SOURCE main=%u escaped=%u parameter=%u bytes=%zu\n",
        entries[0], entries[1], entries[2], output.length);
    xr_xir_compile_c_source_free(&output); effects_source_owners_free();
    CHECK(!effects_compile_live && !effects_compile_bytes); return 0;
}
