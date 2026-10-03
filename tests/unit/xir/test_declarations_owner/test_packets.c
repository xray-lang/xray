/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_packets.c - Independent current vectors and preserved old wire rejection
 */
#include "xir/xxir_checked.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
int main(int argc,char **argv) {
    CHECK(argc==2);XrCompileResourceLimits limits={UINT64_MAX,UINT64_MAX,UINT64_MAX};
    XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats before={0},after={0};CHECK(xr_compile_resources_stats(context.resources,&before)==XR_COMPILE_RESOURCE_OK);
    for(unsigned i=0;i<18;++i)for(unsigned current=0;current<2;++current) {
        char path[1024];CHECK(snprintf(path,sizeof(path),"%s/%02u-%s.chk",argv[1],i,current?"current":"old")>0);
        FILE *file=fopen(path,"rb");CHECK(file && !fseek(file,0,SEEK_END));long size=ftell(file);
        CHECK(size>64 && size<16384 && !fseek(file,0,SEEK_SET));uint8_t bytes[16384];
        CHECK(fread(bytes,1,(size_t)size,file)==(size_t)size && !fclose(file));
        XrXirArtifact *artifact=current?NULL:(XrXirArtifact *)(uintptr_t)1;
        XrXirStatus status=xr_xir_compile_checked_read(&context,bytes,(size_t)size,&artifact,NULL);
        if(!current) {CHECK(status==XR_XIR_BAD_STRUCTURE && artifact==(XrXirArtifact *)(uintptr_t)1);continue;}
        if(status!=XR_XIR_OK)fprintf(stderr,"vector=%u status=%u\n",i,status);
        CHECK(status==XR_XIR_OK);XrXirCheckedPacket written={0};
        CHECK(xr_xir_compile_checked_write(artifact,&written,NULL)==XR_XIR_OK);
        CHECK(written.length==(size_t)size && !memcmp(written.bytes,bytes,(size_t)size));
        xr_xir_compile_checked_packet_free(&written);xr_xir_compile_artifact_free(artifact);
        CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK && after.live_bytes==before.live_bytes);
    }
    xr_compile_resources_release(context.resources);puts("18 complete old/current packet pairs: rejection, roundtrip, live refund PASS");return 0;
}
