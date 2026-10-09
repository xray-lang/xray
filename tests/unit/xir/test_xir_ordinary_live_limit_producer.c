/* Publish genuine generated C only after Source producers and Checked borrows die. */
#include "xir/xxir_emit_c.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "ordinary producer %d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_instance_compile_observer.h"
#define OLL_SOURCE 1
#include "ordinary_live_limit_compile.h"

int main(int argc, char **argv) {
    CHECK(argc == 3); instance_compile_zero();
    XrXirCompileContext context; oll_context(&context);
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    OllFixture fixture;
    XrXirArtifact *lowered = oll_lower(&context, oll_source(&context), &target, &fixture);
    XrXirCSource generated = {0};
    CHECK(xr_xir_compile_emit_c(lowered, "ordinary_live_limit", 4194304, &generated) == XR_XIR_OK);
    CHECK(generated.text && generated.length && !generated.text[generated.length] && !strstr(generated.text, "({"));
    xr_xir_compile_artifact_free(lowered);
    FILE *file = fopen(argv[1], "wb"); CHECK(file);
    CHECK(fwrite(generated.text, 1, generated.length, file) == generated.length && !fclose(file));
    FILE *header = fopen(argv[2], "wb"); CHECK(header);
    CHECK(fprintf(header, "XR_DATA const XrXirProgramSpec ordinary_live_limit_program;\n"
        "static const OllFixture ordinary_live_limit_fixture = {%uu,%uu,%uu,%uu};\n",
        fixture.entry, fixture.run, fixture.recovery, fixture.root) > 0);
    CHECK(!fclose(header));
    xr_xir_compile_c_source_free(&generated); xr_compile_resources_release(context.resources);
    instance_compile_zero();
    printf("ORDINARY_LIVE_LIMIT_PRODUCER modules=%u functions=%u rounds=2 source_dead=1 lowered_dead=1 physical=0/0\n",
        OLL_MODULES, OLL_FUNCTIONS);
    return 0;
}
