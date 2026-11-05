/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_stdlib_checkedgen.c - Checked-only standard library generation
 *
 * KEY CONCEPT: The generator verifies and encodes Library input without CGen or VM.
 */
#include "base/xresource_limits.h"
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(test) do { if (!(test)) { fprintf(stderr,"Checked producer FAIL %d %s\n",__LINE__,#test); exit(1); } } while (0)
static void write_bytes(const char *path, const void *bytes, size_t length) {
    FILE *file = fopen(path,"wb"); CHECK(file);
    CHECK(fwrite(bytes,1,length,file) == length); CHECK(!fclose(file));
}
int main(int argc, char **argv) {
    CHECK(argc == 5);
    XrCompileResourceLimits caps = XR_RESOURCE_DEFAULT_COMPILER_LIMITS;
    XrXirCompileContext context = {0}; context.limits = xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_new(&caps,&context.resources) == XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats initial = {0}; CHECK(xr_compile_resources_stats(context.resources,&initial) == XR_COMPILE_RESOURCE_OK);
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(context.resources,&session) == XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_STDLIB,argv[3],argv[1]};
    XrXirSourceRequest request = {session,argv[2],&authority,&context,argv[1],NULL,XR_XIR_LIBRARY,NULL};
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_compile_source_check(&request,&result,&diagnostic,NULL);
    if (status != XR_XIR_OK) fprintf(stderr,"Checked library status=%u %s\n",status,diagnostic.message);
    CHECK(status == XR_XIR_OK);
    XrXirCheckedPacket library = {0}; CHECK(xr_xir_compile_checked_write(result.checked,&library,NULL) == XR_XIR_OK);
    XrXirArtifact *decoded = NULL;
    XrXirDiagnostic wire_diagnostic = {0};
    status = xr_xir_compile_checked_read(&context,library.bytes,library.length,&decoded,&wire_diagnostic);
    if (status != XR_XIR_OK) fprintf(stderr,"Checked wire status=%u function=%u block=%u instruction=%u reason=%u\n",
        status,wire_diagnostic.function,wire_diagnostic.block,wire_diagnostic.instruction,wire_diagnostic.reason);
    CHECK(status == XR_XIR_OK);
    xr_xir_compile_artifact_free(decoded);
    write_bytes(argv[4],library.bytes,library.length);
    xr_xir_compile_checked_packet_free(&library);
    xr_xir_compile_source_result_free(&result);
    xr_compile_session_free(session);
    XrCompileResourceStats final = {0};
    CHECK(xr_compile_resources_stats(context.resources,&final) == XR_COMPILE_RESOURCE_OK);
    CHECK(final.held_bytes == initial.held_bytes);
    xr_compile_resources_release(context.resources);
    return 0;
}
