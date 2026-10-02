/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_compile_emit_owner.c - C output retains its compiler allocation owner
 */
#include "xir/xxir_emit_c.h"
#include "xir/xxir_generic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
#include "xir_compile_program_fixture.h"
int main(int argc,char **argv) {
    CHECK(argc==2);
    const XrCompileResourceLimits limits={UINT64_MAX,UINT64_MAX,UINT64_MAX};
    XrXirCompileContext context={0};
    context.limits=xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    XrXirArtifact *lowered=owner_lowered(&context);
    XrXirCSource output={0};
    CHECK(xr_xir_compile_emit_c(lowered,"compile_owner",1048576,&output)==XR_XIR_OK);
    xr_xir_compile_artifact_free(lowered);
    xr_compile_resources_release(context.resources);
    CHECK(output.text && output.length && output.text[output.length]=='\0');
    CHECK(!strstr(output.text,"({"));
    FILE *file=fopen(argv[1],"wb"); CHECK(file);
    CHECK(fwrite(output.text,1,output.length,file)==output.length);
    CHECK(fclose(file)==0);
    xr_xir_compile_c_source_free(&output);
    CHECK(!output.text && !output.length);
    return 0;
}
