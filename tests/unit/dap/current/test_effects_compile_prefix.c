/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_effects_compile_prefix.c - Original finite effects compiler before promise FI
 */
#define main effects_prefix_original_checks
#include "../../xir/test_xir_effects_source.c"
#undef main
/* Retain the original input and finite owner, then stop before the complete
 * promise and ownership fault-injection cases of the original entry. */
int main(void) {
    SourceTestFiles files={0};source_files_open(&files);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,files.root};
    XrXirSourceRequest request={NULL,files.entry,&authority,NULL,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=effect_execution_check(&request,&result,&diagnostic);
    if (status!=XR_XIR_OK) fprintf(stderr,"effects prefix status=%u at=%d:%d: %s\n",
        (unsigned)status,diagnostic.line,diagnostic.column,diagnostic.message);
    if (status==XR_XIR_OK) source_effects(result.checked);
    xr_xir_compile_source_result_free(&result);
    source_manifest_raw(&request,"");source_files_close(&files);
    CHECK(!runtime_live && !runtime_bytes);effects_source_owners_free();
    CHECK(status==XR_XIR_OK);
    puts("Original Effects compiler prefix passed; complete promise FI remains a separate full entry");
    return 0;
}
