/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_query_retirement.c - Query borrows end before physical reclamation
 */
#include "base/xmalloc.h"
#include "toolchain/xcompiler_session.h"
#include "../../test_win_compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1); } } while (0)
#include "../../xir/xir_source_compile_owner.h"
#ifdef interface
#undef interface
#endif
#include "xir/xxir_source.c"
static size_t retirement_trial(size_t fail,bool comptime,bool late) {
    XrCompileResourceLimits caps={1048576,1048576,1048576};XrCompileResources *resources=NULL;
    CHECK(xr_compile_resources_new(&caps,&resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats baseline={0};CHECK(xr_compile_resources_stats(resources,&baseline)==XR_COMPILE_RESOURCE_OK);
    SourceContext ctx={0};ctx.compile=(XrXirCompileContext){resources,xr_xir_compile_default_limits()};
    uint32_t count=0,capacity=0,*array=NULL,*first=NULL,*second=NULL;
    size_t begin=source_fixture_compile_attempts;source_fixture_compile_fail_at=fail==SIZE_MAX || late ? SIZE_MAX : begin+fail;
    for (uint32_t i=0;i<49;++i) {
        uint32_t *next=source_query_append(&ctx,array,&count,&capacity,sizeof(*array));
        if (!next) break;
        array=next;array[count-1]=count;
        if (count==16) first=array;
        if (count==32) second=array;
    }
    size_t sites=source_fixture_compile_attempts-begin;source_fixture_compile_fail_at=SIZE_MAX;
    if (fail!=SIZE_MAX && !late) {
        CHECK(ctx.diagnostic.status==XR_XIR_OUT_OF_MEMORY && source_fixture_compile_injected);
        if (first) CHECK(first[0]==1 && first[15]==16);
        if (second) CHECK(second[0]==1 && second[31]==32);
    } else {
        CHECK(count==49 && capacity==64 && first && second && array!=first && array!=second);
        CHECK(first[0]==1 && first[15]==16 && second[0]==1 && second[31]==32);
        for (uint32_t i=0;i<count;++i) CHECK(array[i]==i+1);
        uint32_t retired=0;for (SourceMemory *m=ctx.retired_arrays;m;m=m->next) { CHECK(!m->previous);++retired; }
        CHECK(retired==2);XrCompileResourceStats before={0},after={0};
        CHECK(xr_compile_resources_stats(resources,&before)==XR_COMPILE_RESOURCE_OK);
        source_release_retired_arrays(&ctx);
        CHECK(!ctx.retired_arrays && xr_compile_resources_stats(resources,&after)==XR_COMPILE_RESOURCE_OK);
        CHECK(after.live_bytes<before.live_bytes && after.allocated_bytes==before.allocated_bytes && after.work==before.work);
        if (late) { begin=source_fixture_compile_attempts;source_fixture_compile_fail_at=fail==SIZE_MAX ? SIZE_MAX : begin+fail; }
        for (uint32_t i=count;i<65;++i) {
            uint32_t *next=source_query_append(&ctx,array,&count,&capacity,sizeof(*array));if (!next) break;array=next;array[count-1]=count;
        }
        if (late) { sites=source_fixture_compile_attempts-begin;source_fixture_compile_fail_at=SIZE_MAX; }
        if (late && fail!=SIZE_MAX) CHECK(ctx.diagnostic.status==XR_XIR_OUT_OF_MEMORY && source_fixture_compile_injected && count==64 && capacity==64 && !ctx.retired_arrays);
        else CHECK(count==65 && ctx.retired_arrays && capacity==128);
        for (uint32_t i=0;i<count;++i) CHECK(array[i]==i+1);
    }
    if (comptime) source_comptime_context_free(&ctx);else source_core_dispose(&ctx);
    CHECK(!ctx.memory && !ctx.retired_arrays);XrCompileResourceStats after={0};
    CHECK(xr_compile_resources_stats(resources,&after)==XR_COMPILE_RESOURCE_OK && after.live_bytes==baseline.live_bytes);
    xr_compile_resources_release(resources);CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    source_fixture_compile_injected=false;return sites;
}
int main(int argc,char **argv) {
    xr_test_suppress_dialogs();bool resources=argc==2 && !strcmp(argv[1],"--resources");CHECK(argc==1 || resources);
    for (uint32_t mode=0;mode<2;++mode) for (uint32_t phase=0;phase<2;++phase) {
        size_t sites=retirement_trial(SIZE_MAX,mode!=0,phase!=0);CHECK(sites==(phase ? 1 : 3));
        if (resources) for (size_t i=0;i<sites;++i) retirement_trial(i,mode!=0,phase!=0);
        else { retirement_trial(0,mode!=0,phase!=0);if (sites>1) retirement_trial(sites-1,mode!=0,phase!=0); }
        printf("Query retirement cleanup=%u phase=%u sites=%zu injection=%s: old borrows survive growth; quiescent release has no allocation/work refund; later growth and early-failure cleanup physical0 PASS\n",mode,phase,sites,resources ? "ALL" : "FIRST_LAST_ONLY");
    }
    return 0;
}
