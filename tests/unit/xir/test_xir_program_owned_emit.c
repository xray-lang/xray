/* Generate the actual native counterpart of the owned receiving fixture. */
#define main storage_fixture_entry
#include "test_xir_program_input_storage.c"
#undef main
#include "xir/xxir_emit_c.h"

int main(int argc,char **argv) {
    CHECK(argc==2);
    const XrCompileResourceLimits limits={67108864,8388608,128000000};
    XrXirCompileContext context={0};context.limits=xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats baseline={0},after={0};
    CHECK(xr_compile_resources_stats(context.resources,&baseline)==XR_COMPILE_RESOURCE_OK);
    XrXirArtifact *artifact=program_input(&context);
    XrXirCSource source={0};
    CHECK(xr_xir_compile_emit_c(artifact,"owned_native",200000,&source)==XR_XIR_OK);
    xr_xir_compile_artifact_free(artifact);
    CHECK(!strstr(source.text,"({") && !strstr(source.text,"xr_xir_vm") &&
        !strstr(source.text,"XrXirProgramProof"));
    FILE *file=fopen(argv[1],"wb");CHECK(file);
    CHECK(fwrite(source.text,1,source.length,file)==source.length);
    CHECK(fclose(file)==0);
    xr_xir_compile_c_source_free(&source);
    CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(after.live_bytes==baseline.live_bytes);
    xr_compile_resources_release(context.resources);
    puts("Actual native fixture emitted after producer destruction; ledger live recovered");
    return 0;
}
