/* Complete prior packet identity rejects before allocation and keeps output. */
#ifndef XIR_LIBRARY_PRIOR_PACKET_REJECTION_H
#define XIR_LIBRARY_PRIOR_PACKET_REJECTION_H
static void library_prior_packet_rejection(const XrXirCompileContext *context,
    const uint8_t *bytes,size_t length) {
    CHECK(length>=64 && ((bytes[8]==24 && bytes[12]==63)||(bytes[8]==25 && (bytes[12]==64 || bytes[12]==65))||(bytes[8]==26 && bytes[12]==71)));
    size_t attempts=source_program_compile_attempts;
    size_t blocks=source_program_compile_live,physical=source_program_compile_bytes;
    size_t rblocks=runtime_live,rphysical=runtime_bytes;
    XrXirArtifact *artifact=NULL;XrXirDiagnostic diagnostic={0};
    CHECK(xr_xir_compile_checked_read(context,bytes,length,&artifact,&diagnostic)==XR_XIR_BAD_STRUCTURE);
    CHECK(!artifact && diagnostic.status==XR_XIR_BAD_STRUCTURE);
    artifact=(XrXirArtifact *)(uintptr_t)1;
    CHECK(xr_xir_compile_checked_read(context,bytes,length,&artifact,&diagnostic)==XR_XIR_BAD_STRUCTURE);
    CHECK(artifact==(XrXirArtifact *)(uintptr_t)1 && diagnostic.status==XR_XIR_BAD_STRUCTURE);
    CHECK(source_program_compile_attempts==attempts && source_program_compile_live==blocks &&
        source_program_compile_bytes==physical && runtime_live==rblocks && runtime_bytes==rphysical);
}
#endif
