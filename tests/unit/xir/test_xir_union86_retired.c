/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_union86_retired.c - Complete immutable predecessor rejection
 */
#include "xir/xxir_checked.h"
#include "base/xmalloc.h"
#include "base/xsha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if (!(c)) {fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}} while (0)
typedef struct RetiredPacket {const char *path;size_t bytes;const char *sha256;} RetiredPacket;
#include "union86_retired_table.h"
static void reject(const RetiredPacket *row) {
    CHECK(row->bytes>=64 && row->bytes<=1048576);
    char path[1024];CHECK(snprintf(path,sizeof(path),"%s/%s",UNION86_ROOT,row->path)>0);
    FILE *file=NULL;
#if defined(_MSC_VER)
    CHECK(fopen_s(&file,path,"rb")==0 && file);
#else
    file=fopen(path,"rb");CHECK(file);
#endif
    uint8_t *data=xr_malloc(row->bytes);CHECK(data);
    CHECK(fread(data,1,row->bytes,file)==row->bytes && fgetc(file)==EOF);CHECK(!fclose(file));
    uint8_t digest[32];char hex[65];xr_sha256(data,row->bytes,digest);
    for (unsigned i=0;i<32;++i) CHECK(snprintf(hex+2*i,3,"%02x",digest[i])==2);
    CHECK(!strcmp(hex,row->sha256));
    for (unsigned occupied=0;occupied<2;++occupied) {
        const XrCompileResourceLimits limits={67108864,8388608,128000000};XrCompileResources *resources=NULL;
        CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
        XrXirCompileContext context={resources,xr_xir_compile_default_limits()};
        XrCompileResourceStats before={0},after={0};
        CHECK(xr_compile_resources_stats(resources,&before)==XR_COMPILE_RESOURCE_OK);
        XrXirArtifact *sentinel=occupied?(XrXirArtifact *)(uintptr_t)1:NULL,*output=sentinel;
        CHECK(xr_xir_compile_checked_read(&context,data,row->bytes,&output,NULL)==XR_XIR_BAD_STRUCTURE);
        CHECK(output==sentinel && xr_compile_resources_stats(resources,&after)==XR_COMPILE_RESOURCE_OK);
        CHECK(after.allocation_count==before.allocation_count && after.allocated_bytes==before.allocated_bytes);
        CHECK(after.live_bytes==before.live_bytes && after.work-before.work==(occupied?0u:32u));
        xr_compile_resources_release(resources);
    }
    xr_free(data);
}
int main(void) {
    _Static_assert(XR_XIR_CHECKED_SCHEMA==30 && XR_XIR_CHECKED_CONTRACT==86,"combined receiving identity");
    for (size_t i=0;i<sizeof(retired_packets)/sizeof(retired_packets[0]);++i) reject(retired_packets+i);
    printf("Complete retired packets=%zu; empty/occupied output, no receiving allocation, bounded header work PASS\n",
        sizeof(retired_packets)/sizeof(retired_packets[0]));return 0;
}
