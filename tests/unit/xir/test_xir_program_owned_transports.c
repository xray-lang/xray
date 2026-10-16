/* The same receiver consumes real internal ownership and an external packet. */
#define main storage_fixture_entry
#include "test_xir_program_input_storage.c"
#undef main
#include "xir/xxir_program_input_internal.h"

static unsigned receive_decodes;
static XrXirStatus observed_packet_decode(const XrXirCompileContext *context,
    const void *bytes,size_t length,const XrXirTarget *target,
    XrXirArtifact **output,XrXirDiagnostic *diagnostic) {
    ++receive_decodes;
    return xr_xir_compile_checked_read_lower(context,bytes,length,target,output,diagnostic);
}
#define xr_xir_compile_checked_read_lower observed_packet_decode
#include "xir/xxir_program_input.c"
#undef xr_xir_compile_checked_read_lower

extern const XrXirProgramSpec owned_native_program;
int main(void) {
    const XrCompileResourceLimits limits={67108864,8388608,128000000};
    XrXirCompileContext context={0};context.limits=xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats baseline={0},after={0};
    CHECK(xr_compile_resources_stats(context.resources,&baseline)==XR_COMPILE_RESOURCE_OK);
    XrXirArtifact *artifact=program_input(&context);
    xr_xir_compile_checked_packet_free(&artifact->checked_packet);
    memset(artifact->checked_identity,0,sizeof(artifact->checked_identity));
    XrXirProgram *program=NULL;
    CHECK(receive_decodes==0);
    CHECK(xr_xir_compile_vm_program_take(&artifact,&program)==XR_XIR_OK && !artifact);
    CHECK(receive_decodes==0);result42(program);xr_xir_compile_program_drop(program);program=NULL;
    CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(after.live_bytes==baseline.live_bytes);

    XrXirStatus status=XR_XIR_OK;
    XrXirProgramSpec spec=owned_native_program;
    spec.abi_version=30;
    XrCompileResourceStats rejected_before={0},rejected_after={0};
    CHECK(xr_compile_resources_stats(context.resources,&rejected_before)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_xir_compile_program_seal(&context,&spec,&program)==XR_XIR_BAD_LAYOUT && !program);
    CHECK(receive_decodes==0);
    CHECK(xr_compile_resources_stats(context.resources,&rejected_after)==XR_COMPILE_RESOURCE_OK);
    CHECK(rejected_after.work==rejected_before.work &&
        rejected_after.allocation_count==rejected_before.allocation_count);
    spec.abi_version=XR_XIR_PROGRAM_ABI_VERSION;
    uint8_t *packet=xir_compile_copy(&context,spec.packet.bytes,spec.packet.length,&status);
    CHECK(status==XR_XIR_OK && packet);
    uint8_t identity[32];memcpy(identity,spec.packet.identity,sizeof(identity));
    spec.packet.bytes=packet;spec.packet.identity=identity;
    CHECK(xr_xir_compile_program_seal(&context,&spec,&program)==XR_XIR_OK && program);
    CHECK(receive_decodes==1);
    xr_compile_resources_free(packet);memset(identity,0,sizeof(identity));memset(&spec,0,sizeof(spec));
    result42(program);xr_xir_compile_program_drop(program);
    CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(after.live_bytes==baseline.live_bytes);
    xr_compile_resources_release(context.resources);
    puts("Internal packet death decodes0; old Program30 rejects; external decodes1; producer/packet death returns42; live recovered");
    return 0;
}
