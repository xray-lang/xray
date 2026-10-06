#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_emit_c.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_go_source_native_compile_owner.h"
#include "xir_go_source_native_pipeline.h"
int main(int argc, char **argv) {
    CHECK(argc == 3);
    const XrXirCompileContext *context = effects_source_owner(UINT64_C(67108864), UINT64_C(128000000));
    uint32_t entry = UINT32_MAX;
    XrXirArtifact *lowered = source17_lower(context, argv[2], &entry);
    char symbol[256]; CHECK(snprintf(symbol, sizeof(symbol), "source17_%s", argv[2]) > 0);
    XrXirCSource output = {0};
    XrXirStatus status = xr_xir_compile_emit_c(lowered, symbol, 4194304, &output);
    xr_xir_compile_artifact_free(lowered);
    if (status != XR_XIR_OK) fprintf(stderr, "%s emit=%u\n", argv[2], status);
    CHECK(status == XR_XIR_OK && output.text && !output.text[output.length] && !strstr(output.text, "({"));
    FILE *file = fopen(argv[1], "wb"); CHECK(file);
    CHECK(fwrite(output.text, 1, output.length, file) == output.length && !fclose(file));
    printf("%s entry=%u bytes=%zu\n", argv[2], entry, output.length);
    xr_xir_compile_c_source_free(&output);
    effects_source_owners_free();
    CHECK(!effects_compile_live && !effects_compile_bytes);
    return 0;
}
