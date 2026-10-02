/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_sdk_lease.c - Hold production SDK authority during external compilation
 */
#include "toolchain/xr_xir_runtime_sdk.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <string.h>
int main(int argc,char **argv) {
    if (argc!=3) return 1;
    FILE *file=fopen(argv[2],"rb");
    if (!file || fseek(file,0,SEEK_END)) return 2;
    long length=ftell(file);
    if (length<=0 || length>2097152 || fseek(file,0,SEEK_SET)) {fclose(file);return 3;}
    char *bytes=xr_malloc((size_t)length);
    if (!bytes) {fclose(file);return 4;}
    size_t actual=fread(bytes,1,(size_t)length,file);int closed=fclose(file);
    if (actual!=(size_t)length || closed) {xr_free(bytes);return 5;}
    XrXirRuntimeSdkRequest request={argv[1],bytes,(size_t)length,16777216,67108864};
    XrXirRuntimeSdk *sdk=NULL;XrXirRuntimeSdkStatus status=xr_xir_runtime_sdk_load(&request,&sdk);
    xr_free(bytes);if (status!=XR_XIR_SDK_OK) return 6+(int)status;
    const XrXirRuntimeSdkFacts *facts=xr_xir_runtime_sdk_facts(sdk);
    printf("READY %u %u %u\n",facts->value_abi,facts->crt,facts->file_count);
    puts(xr_xir_runtime_sdk_root(sdk));
    const char *libraries[]={"lib/xray_xir_admission.lib","lib/xray_xir_declarations.lib",
        "lib/xray_xir_scalar.lib","lib/xray_xir_runtime_host.lib"};
    for (size_t i=0;i<4;++i) {
        const char *path=xr_xir_runtime_sdk_file(sdk,libraries[i]);
        if (!path) {xr_xir_runtime_sdk_free(sdk);return 20;}
        puts(path);
    }
    if (fflush(stdout)) {xr_xir_runtime_sdk_free(sdk);return 21;}
    int signal=fgetc(stdin);xr_xir_runtime_sdk_free(sdk);
    return signal=='q' ? 0 : 22;
}
