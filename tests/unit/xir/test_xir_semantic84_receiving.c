/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_semantic84_receiving.c - Early rejection of both complete predecessor grammars
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_compile_context.h"
#include "base/xsha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1); } } while (0)
#include "semantic84_retired_packets.h"
typedef struct Retired84Packet { const char *name; const uint8_t *bytes; size_t length; } Retired84Packet;
#include "semantic84_retired_table.h"
int main(void) {
    _Static_assert(XR_XIR_CHECKED_SCHEMA==32 && XR_XIR_CHECKED_CONTRACT==84, "sole current identity");
    const XrCompileResourceLimits limits={1048576,1048576,1048576};
    XrXirCompileContext context={0};context.limits=xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    for (size_t i=0;i<sizeof(retired84_packets)/sizeof(retired84_packets[0]);++i) {
        const Retired84Packet *packet=&retired84_packets[i];
        XrSHA256Context sha;uint8_t digest[32];
        xr_sha256_init(&sha);xr_sha256_update(&sha,packet->bytes,32);
        xr_sha256_update(&sha,packet->bytes+64,packet->length-64);xr_sha256_final(&sha,digest);
        CHECK(!memcmp(digest,packet->bytes+32,32));
        XrCompileResourceStats before={0},after={0};
        CHECK(xr_compile_resources_stats(context.resources,&before)==XR_COMPILE_RESOURCE_OK);
        for (unsigned occupied=0;occupied<2;++occupied) {
            XrXirArtifact *output=occupied ? (XrXirArtifact *)(uintptr_t)1 : NULL,*saved=output;
            CHECK(xr_xir_compile_checked_read(&context,packet->bytes,packet->length,&output,NULL)==XR_XIR_BAD_STRUCTURE);
            CHECK(output==saved);
        }
        CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK);
        CHECK(after.work==before.work+32 && after.live_bytes==before.live_bytes &&
            after.allocation_count==before.allocation_count && after.allocated_bytes==before.allocated_bytes);
    }
    xr_compile_resources_release(context.resources);
    printf("Complete retained packets=%zu; valid old SHA, empty/occupied output, fixed early work and zero allocations PASS\n",
        sizeof(retired84_packets)/sizeof(retired84_packets[0]));
    return 0;
}
