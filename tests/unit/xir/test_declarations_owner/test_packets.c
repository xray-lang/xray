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
#include "../xir_library_compile_owner.h"
int main(int argc,char **argv) {
    CHECK(argc==2);LibraryCompileOwner compiler={0};CHECK(library_compile_owner_new(&compiler,&library_compile_limits)==XR_XIR_OK);
    XrXirCompileContext context=compiler.context;
    XrCompileResourceStats before={0},after={0};CHECK(xr_compile_resources_stats(context.resources,&before)==XR_COMPILE_RESOURCE_OK);
    const char *families[]={"old","current","old63","old64","old65","old71"};
    for(unsigned i=0;i<18;++i)for(size_t current=0;current<sizeof(families)/sizeof(*families);++current) {
        char path[1024];CHECK(snprintf(path,sizeof(path),"%s/%02u-%s.chk",argv[1],i,families[current])>0);
        FILE *file=fopen(path,"rb");CHECK(file && !fseek(file,0,SEEK_END));long size=ftell(file);
        CHECK(size>64 && size<16384 && !fseek(file,0,SEEK_SET));uint8_t bytes[16384];
        CHECK(fread(bytes,1,(size_t)size,file)==(size_t)size && !fclose(file));
        XrXirArtifact *artifact=NULL;
        XrCompileResourceStats reject_before=library_compile_stats(&context);
        XrXirStatus status=xr_xir_compile_checked_read(&context,bytes,(size_t)size,&artifact,NULL);
        if(current!=1) {
            CHECK(status==XR_XIR_BAD_STRUCTURE && !artifact);
            XrCompileResourceStats reject_after=library_compile_stats(&context);
            CHECK(reject_after.allocation_count==reject_before.allocation_count &&
                reject_after.allocated_bytes==reject_before.allocated_bytes &&
                reject_after.live_bytes==reject_before.live_bytes &&
                reject_after.peak_bytes==reject_before.peak_bytes);
            continue;
        }
        if(status!=XR_XIR_OK)fprintf(stderr,"vector=%u status=%u\n",i,status);
        CHECK(status==XR_XIR_OK);XrXirCheckedPacket written={0};
        CHECK(xr_xir_compile_checked_write(artifact,&written,NULL)==XR_XIR_OK);
        CHECK(written.length==(size_t)size && !memcmp(written.bytes,bytes,(size_t)size));
        xr_xir_compile_checked_packet_free(&written);xr_xir_compile_artifact_free(artifact);
        CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK && after.live_bytes==before.live_bytes);
    }
    after=library_compile_stats(&context);
    fprintf(stderr,"role finite allocated=%llu peak=%llu work=%llu\n",(unsigned long long)after.allocated_bytes,(unsigned long long)after.peak_bytes,(unsigned long long)after.work);
    library_compile_owner_drop(&compiler);library_compile_observer_free();puts("18 complete current27/72 models and five historical families: rejection, roundtrip, live refund PASS");return 0;
}
