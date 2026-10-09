/* Public compiler pipeline only; the current Lowered Task guard must not be bypassed. */
#include "xir_construction_fixture.h"
#include "xir/xxir_emit_c.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%d: %s\n", __LINE__, #x); exit(1); } } while (0)
#include "xir_go_cgen_fixture.h"
int main(int argc, char **argv) {
    CHECK(argc == 2);
    const XrCompileResourceLimits limits = {UINT64_C(67108864), UINT64_C(8388608), UINT64_C(128000000)};
    XrXirCompileContext context = {.limits = xr_xir_compile_default_limits()};
    CHECK(xr_compile_resources_new(&limits, &context.resources) == XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats baseline = {0};
    CHECK(xr_compile_resources_stats(context.resources, &baseline) == XR_COMPILE_RESOURCE_OK);
    GoCGenFixture fixture; go_cgen_fixture(&fixture);
    XrXirArtifact *checked = NULL, *decoded = NULL, *closed = NULL, *lowered = NULL;
    XrXirCheckedPacket packet = {0}; XrXirCSource output = {0}; XrXirDiagnostic diagnostic = {0};
    XrXirStatus status = xir_fixture_check(&context, &fixture.module, &checked, &diagnostic);
    if (status == XR_XIR_OK) status = xr_xir_compile_checked_write(checked, &packet, &diagnostic);
    if (status == XR_XIR_OK) status = xr_xir_compile_checked_read(&context, packet.bytes, packet.length, &decoded, &diagnostic);
    if (status == XR_XIR_OK) status = xr_xir_compile_specialize(decoded, &closed, &diagnostic);
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    if (status == XR_XIR_OK) status = xr_xir_compile_lower(closed, &target, &lowered, &diagnostic);
    if (status == XR_XIR_OK) status = xr_xir_compile_emit_c(lowered, "go_cgen", 4194304, &output);
    xr_xir_compile_artifact_free(lowered); xr_xir_compile_artifact_free(closed);
    xr_xir_compile_artifact_free(decoded); xr_xir_compile_artifact_free(checked);
    xr_xir_compile_checked_packet_free(&packet);
    if (status == XR_XIR_OK) {
        CHECK(output.text && output.text[output.length] == 0 && !strstr(output.text, "({"));
        FILE *file = fopen(argv[1], "wb"); CHECK(file);
        CHECK(fwrite(output.text, 1, output.length, file) == output.length); CHECK(fclose(file) == 0);
    } else {
        CHECK(!output.text && !output.length);
        fprintf(stderr, "public GO pipeline status=%u function=%u instruction=%u\n",
            status, diagnostic.function, diagnostic.instruction);
    }
    xr_xir_compile_c_source_free(&output);
    XrCompileResourceStats stats; CHECK(xr_compile_resources_stats(context.resources, &stats) == XR_COMPILE_RESOURCE_OK);
    CHECK(stats.live_bytes == baseline.live_bytes); xr_compile_resources_release(context.resources);
    return status == XR_XIR_OK ? 0 : 1;
}
