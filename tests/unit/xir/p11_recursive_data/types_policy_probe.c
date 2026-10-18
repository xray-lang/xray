#define main reference_original_main
#include "../p11_ctfe_reference/test_ctfe_reference.c"
#undef main
int main(void) {
    XrXirCompileContext context={0};CHECK(xr_compile_resources_new(&reference_limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();XrCompileResourceStats baseline=reference_stats(&context);
    reference_tuple_type_policy(&context);reference_nullable_type_policy(&context);reference_array_type_policy(&context);
    fprintf(stderr,"Before Source live=%llu blocks=%zu bytes=%zu\n",(unsigned long long)reference_stats(&context).live_bytes,source_fixture_compile_live,source_fixture_compile_bytes);
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    CHECK(reference_source(&context,"array_deep_gap",&source,&diagnostic)==XR_XIR_OK);
    uint8_t identity[32]={0};XrXirArtifact *lowered=reference_lower(&context,&source,identity);
    XrXirDomain *reader=NULL;CHECK(xr_xir_compile_domain_new(context.resources,UINT64_MAX,&reader)==XR_XIR_VALUE_OK);
    XrXirCtfeRequest request={.artifact=lowered,.function=reference_run_function(xr_xir_compile_artifact_module(lowered)),
        .target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},.limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,3}};
    memcpy(request.checked_identity,identity,32);XrXirValue value={0};
    CHECK(xr_xir_compile_ctfe_leaf(&request,&value)==XR_XIR_CTFE_OK);
    CtfeReferenceCase selected={"array_deep_gap",XR_XIR_UNIT,3,XR_XIR_OK,NULL,0};reference_array_value(&value,&selected,reader);
    xr_xir_value_drop(&value);fprintf(stderr,"After successful result drop live=%llu blocks=%zu bytes=%zu\n",(unsigned long long)reference_stats(&context).live_bytes,source_fixture_compile_live,source_fixture_compile_bytes);--request.limits.output_bytes;
    CHECK(xr_xir_compile_ctfe_leaf(&request,&value)==XR_XIR_CTFE_OUTPUT_LIMIT && !value.type && !value.payload);
    fprintf(stderr,"After output-limit live=%llu blocks=%zu bytes=%zu\n",(unsigned long long)reference_stats(&context).live_bytes,source_fixture_compile_live,source_fixture_compile_bytes);xr_xir_compile_artifact_free(lowered);xr_xir_domain_drop(reader);fprintf(stderr,"After all owning drops live=%llu blocks=%zu bytes=%zu\n",(unsigned long long)reference_stats(&context).live_bytes,source_fixture_compile_live,source_fixture_compile_bytes);
    CHECK(reference_stats(&context).live_bytes==baseline.live_bytes);xr_compile_resources_release(context.resources);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    fprintf(stderr,"Canonical Tuple/Nullable self cycles refuse; variable Array data admits; no fabricated Values; original owner physical0\n");return 0;
}
