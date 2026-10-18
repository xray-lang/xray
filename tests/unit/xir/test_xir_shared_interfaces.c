/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_shared_interfaces.c - Independent shared identity reception
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_types.h"
#include "base/xmalloc.h"
#include "base/xsha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if (!(c)) {fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}} while (0)
#include "xir_effect_execution_owner.h"
#include "shared_current_checked.h"

static XrXirCompileContext context_new(void) {
    const XrCompileResourceLimits limits={67108864,8388608,128000000};
    XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    return context;
}
static void current(const uint8_t *bytes,size_t length) {
    XrXirCompileContext context=context_new();
    XrCompileResourceStats baseline={0};
    CHECK(xr_compile_resources_stats(context.resources,&baseline)==XR_COMPILE_RESOURCE_OK);
    XrXirArtifact *artifact=NULL;XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_checked_read(&context,bytes,length,&artifact,&diagnostic);
    if (status!=XR_XIR_OK) fprintf(stderr,"current reception status=%d function=%u instruction=%u\n",
        status,diagnostic.function,diagnostic.instruction);
    CHECK(status==XR_XIR_OK && artifact);
    XrXirCheckedPacket packet={0};
    CHECK(xr_xir_compile_checked_write(artifact,&packet,NULL)==XR_XIR_OK);
    CHECK(packet.length==length && !memcmp(packet.bytes,bytes,length));
    xr_xir_compile_checked_packet_free(&packet);xr_xir_compile_artifact_free(artifact);
    XrCompileResourceStats stats={0};
    CHECK(xr_compile_resources_stats(context.resources,&stats)==XR_COMPILE_RESOURCE_OK && stats.live_bytes==baseline.live_bytes);
    xr_compile_resources_release(context.resources);
    CHECK(!effects_compile_live && !effects_compile_bytes);
}
static uint32_t read_word(FILE *file) {
    uint8_t bytes[4];CHECK(fread(bytes,1,4,file)==4);
    return (uint32_t)bytes[0]|(uint32_t)bytes[1]<<8|(uint32_t)bytes[2]<<16|(uint32_t)bytes[3]<<24;
}
static void reject(const uint8_t *bytes,size_t length) {
    for (unsigned occupied=0;occupied<2;++occupied) {
        XrXirCompileContext context=context_new();
        XrCompileResourceStats before={0},after={0};
        CHECK(xr_compile_resources_stats(context.resources,&before)==XR_COMPILE_RESOURCE_OK);
        XrXirArtifact *sentinel=occupied?(XrXirArtifact *)(uintptr_t)1:NULL,*output=sentinel;
        CHECK(xr_xir_compile_checked_read(&context,bytes,length,&output,NULL)==XR_XIR_BAD_STRUCTURE);
        CHECK(output==sentinel && xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK);
        CHECK(before.allocation_count==after.allocation_count && before.allocated_bytes==after.allocated_bytes);
        CHECK(before.live_bytes==after.live_bytes && after.work-before.work==(occupied?0u:32u));
        xr_compile_resources_release(context.resources);
        CHECK(!effects_compile_live && !effects_compile_bytes);
    }
}
static void all_parents(void) {
    FILE *file=NULL;
#if defined(_MSC_VER)
    CHECK(!fopen_s(&file,SHARED_PACKET_FILE,"rb") && file);
#else
    file=fopen(SHARED_PACKET_FILE,"rb");CHECK(file);
#endif
    uint8_t magic[8];CHECK(fread(magic,1,8,file)==8 && !memcmp(magic,"XRPRNT01",8));
    uint32_t count=read_word(file);CHECK(count==1284);
    for (uint32_t i=0;i<count;++i) {
        uint32_t length=read_word(file);CHECK(length>=64 && length<=1048576);
        uint8_t expected[32],digest[32];CHECK(fread(expected,1,32,file)==32);
        uint8_t *bytes=xr_malloc(length);CHECK(bytes);
        CHECK(fread(bytes,1,length,file)==length);xr_sha256(bytes,length,digest);
        CHECK(!memcmp(digest,expected,32));reject(bytes,length);xr_free(bytes);
    }
    CHECK(fgetc(file)==EOF && !fclose(file));
}
int main(void) {
    _Static_assert(XR_XIR_CHECKED_SCHEMA==32 && XR_XIR_CHECKED_CONTRACT==90,"shared wire identity");
    _Static_assert(XR_XIR_TYPE_MAP==9 && XR_XIR_TYPE_SET==10 && XR_XIR_TYPE_SLICE==11 &&
        XR_XIR_TYPE_MUT_SLICE==12 && XR_XIR_TYPE_ITERATOR==13 && XR_XIR_TYPE_CHANNEL==14,"unique type tags");
    current(shared_channel_int,sizeof(shared_channel_int));
    current(shared_channel_unit,sizeof(shared_channel_unit));all_parents();
    puts("Independent current Channel bytes and 1284 complete parent packets: bounded reception PASS");return 0;
}
