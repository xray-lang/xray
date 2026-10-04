/* Source packets, producer ownership and code projection share one ledger. */
#ifndef XIR_SOURCE_NESTED_NULLABLE_PIPELINE_H
#define XIR_SOURCE_NESTED_NULLABLE_PIPELINE_H
static bool sn_metadata_profile;
static void sn_metadata_checkpoint(const XrXirCompileContext *context,const char *stage,XrXirStatus status) {
    if (!sn_metadata_profile) return;
    XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(context->resources,&stats)==XR_COMPILE_RESOURCE_OK);
    fprintf(stderr,"Source nested metadata baseline %s status=%u attempts=%zu allocated=%llu work=%llu live=%llu peak=%llu\n",
        stage,status,source_nested_compile_attempts,(unsigned long long)stats.allocated_bytes,
        (unsigned long long)stats.work,(unsigned long long)stats.live_bytes,(unsigned long long)stats.peak_bytes);
}
static XrXirStatus sn_build(const XrXirCompileContext *context,const char *output,
    XrXirProgram **program,uint32_t functions[SN_COUNT]) {
    XrCompilerSession *session=NULL;XrXirSourceProduct *product=NULL;XrXirArtifact *closed=NULL;
    XrXirSourceProductDiagnostic diagnostic={0};XrXirCSource source={0};
    XrCompilerSessionStatus session_status=xr_compile_session_new(context->resources,&session);
    XrXirStatus status=session_status==XR_COMPILER_SESSION_OK?XR_XIR_OK:
        session_status==XR_COMPILER_SESSION_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BUDGET;
    const XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_SOURCE_NESTED_FIXTURES};
    const XrXirSourceProductRequest request={{session,XR_SOURCE_NESTED_FIXTURES "/root.xr",&authority,
        context,NULL,NULL,XR_XIR_PROGRAM,NULL},{XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION}};
    if (status==XR_XIR_OK) status=xr_xir_compile_source_product_build(&request,&product,&diagnostic);
    sn_metadata_checkpoint(context,"build",status);
    if (status==XR_XIR_OK) {
        CHECK(product && xr_xir_compile_source_product_view(product)->complete);
        CHECK(xr_xir_compile_source_product_context(product)->resources==context->resources);
        status=xr_xir_compile_source_product_verify(product,1048576,NULL);
        sn_metadata_checkpoint(context,"verify",status);
    }
    XrXirSourceProductPacketView packet={0};
    if (status==XR_XIR_OK) status=xr_xir_compile_source_product_packet(product,XR_XIR_SOURCE_PRODUCT_CLOSED,&packet);
    if (status==XR_XIR_OK) status=xr_xir_compile_checked_read(context,packet.bytes,packet.length,&closed,NULL);
    sn_metadata_checkpoint(context,"closed-read",status);
    if (status==XR_XIR_OK) {
        sn_find(xr_xir_compile_artifact_module(closed),functions);
        status=xr_xir_compile_source_product_emit(product,"source_nested_nullable",1048576,&source);
        sn_metadata_checkpoint(context,"emit",status);
    }
    if (status==XR_XIR_OK && output) {
        CHECK(source.text && source.length<1048576);
        FILE *file=fopen(output,"wb");CHECK(file && fwrite(source.text,1,source.length,file)==source.length && !fclose(file));
    }
    if (status==XR_XIR_OK) status=xr_xir_compile_source_product_vm_take(product,program);
    sn_metadata_checkpoint(context,"vm-take",status);
    if (status!=XR_XIR_OK) {
        if (status!=XR_XIR_OUT_OF_MEMORY && status!=XR_XIR_BUDGET)
            fprintf(stderr,"Source nested pipeline status=%u stage=%u %u:%u: %s\n",status,diagnostic.stage,
                diagnostic.source.line,diagnostic.source.column,diagnostic.source.message);
        CHECK(!*program);
    }
    xr_xir_compile_c_source_free(&source);xr_xir_compile_artifact_free(closed);
    xr_xir_compile_source_product_free(product);xr_xir_compile_source_product_diagnostic_free(&diagnostic);
    xr_compile_session_free(session);return status;
}
#endif // XIR_SOURCE_NESTED_NULLABLE_PIPELINE_H
