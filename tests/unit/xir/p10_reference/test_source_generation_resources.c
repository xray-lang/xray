/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_generation_resources.c - Full Source generation physical faults
 */
#include "xir/xxir_source_generation.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1); } } while (0)
typedef struct Allocation { void *pointer; size_t bytes; } Allocation;
static Allocation allocations[20000];
static size_t attempts, fail_at = SIZE_MAX;
static uint64_t physical;
static void *observed_alloc(size_t bytes) {
    if (attempts++ == fail_at) return NULL;
    void *memory = xr_malloc(bytes); if (!memory) return NULL;
    size_t i = 0; while (i < 20000 && allocations[i].pointer) ++i;
    CHECK(i < 20000); allocations[i] = (Allocation){memory, bytes}; physical += bytes;
    return memory;
}
static void observed_free(void *memory) {
    if (!memory) return;
    size_t i = 0; while (i < 20000 && allocations[i].pointer != memory) ++i;
    CHECK(i < 20000); physical -= allocations[i].bytes; allocations[i] = (Allocation){0};
    xr_free(memory);
}
#undef xr_malloc
#undef xr_free
#define xr_malloc(n) observed_alloc(n)
#define xr_free(p) observed_free(p)
#include "base/xcompile_resources.c"

static const XrCompileResourceLimits limits = {
    UINT64_C(64)*1024*1024, UINT64_C(8)*1024*1024, UINT64_C(128000000)};
static XrXirStatus pipeline(const XrCompileResourceLimits *bounds,XrCompileResourceStats *stats,bool comptime) {
    XrCompileResources *resources=NULL;XrXirSourceGenerations *owner=NULL;
    XrCompileResourceStatus opened=xr_compile_resources_new(bounds,&resources);
    if (opened!=XR_COMPILE_RESOURCE_OK) {
        CHECK(!resources && !physical);
        return opened==XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BUDGET;
    }
    XrXirCompileContext context={resources,xr_xir_compile_default_limits()};
    XrXirSourceGenerationConfig config={context,"generation-fi",NULL,NULL,NULL};
    XrXirStatus status=xr_xir_compile_source_generations_new(&config,&owner);
    if (status!=XR_XIR_OK) goto done;
    const char *first=comptime ? "const value=comptime (6*7)\nexport fn answer()->i64{return value}" :
        "const value=5\nexport fn answer()->i64{return value}";
    XrXirSourceText text={NULL,first,strlen(first)};uint64_t serial=0;
    status=xr_xir_compile_source_generation_prepare(owner,&text,&serial,NULL);
    if (status!=XR_XIR_OK) { CHECK(!serial);goto done; }
    CHECK(xr_xir_compile_source_generation_publish(owner,serial)==XR_XIR_OK);
    XrXirSourceGenerationDefinition definition={0};
    status=xr_xir_compile_source_generation_definition(owner,serial,"value",&definition);
    if (status!=XR_XIR_OK) goto done;
    const XrXirModule *module=xr_xir_compile_artifact_module(definition.checked);
    CHECK(definition.kind==XR_XIR_SOURCE_DEFINITION_BINDING && definition.stored_type==XR_XIR_I64 &&
        definition.syntax && definition.ordinal<module->declarations->slot_count &&
        module->declarations->slots[definition.ordinal].type==definition.stored_type);
    XrXirSourceGenerationBinding before={0},after={0};
    status=xr_xir_compile_source_generation_lookup(owner,"value",&before);
    if (status!=XR_XIR_OK) goto done;
    CHECK(before.generation==serial && before.declaration->type.type==XR_XIR_I64);
    const char second[]="const value=\"next\"\nexport fn answer()->string{return value}";
    text=(XrXirSourceText){NULL,second,sizeof(second)-1};uint64_t candidate=0;
    status=xr_xir_compile_source_generation_prepare(owner,&text,&candidate,NULL);
    if (status!=XR_XIR_OK) {
        CHECK(!candidate);
        if (status==XR_XIR_OUT_OF_MEMORY) {
            CHECK(xr_xir_compile_source_generation_lookup(owner,"value",&after)==XR_XIR_OK);
            CHECK(before.generation==after.generation && before.declaration==after.declaration);
        }
        goto done;
    }
    CHECK(candidate>serial && xr_xir_compile_source_generation_lookup(owner,"value",&after)==XR_XIR_OK);
    CHECK(before.generation==after.generation && before.declaration==after.declaration);
    CHECK(xr_xir_compile_source_generation_abort(owner,candidate)==XR_XIR_OK);
    candidate=0;status=xr_xir_compile_source_generation_prepare(owner,&text,&candidate,NULL);
    if (status!=XR_XIR_OK) { CHECK(!candidate);goto done; }
    CHECK(xr_xir_compile_source_generation_publish(owner,candidate)==XR_XIR_OK);
    status=xr_xir_compile_source_generation_definition(owner,candidate,"value",&definition);
    if (status!=XR_XIR_OK) goto done;
    CHECK(definition.kind==XR_XIR_SOURCE_DEFINITION_BINDING && definition.stored_type==XR_XIR_STRING);
    status=xr_xir_compile_source_generation_lookup(owner,"value",&after);
    if (status==XR_XIR_OK) CHECK(after.generation==candidate && after.declaration->type.type==XR_XIR_STRING);
done:
    xr_xir_compile_source_generations_free(owner);
    CHECK(xr_compile_resources_stats(resources,stats)==XR_COMPILE_RESOURCE_OK);
    xr_compile_resources_release(resources);CHECK(!physical);return status;
}
int main(int argc,char **argv) {
    bool comptime=false,baseline=false;
    for (int i=1;i<argc;++i) {
        if (!strcmp(argv[i],"--comptime")) {CHECK(!comptime);comptime=true;}
        else {CHECK(!strcmp(argv[i],"--baseline") && !baseline);baseline=true;}
    }
    XrCompileResourceStats normal={0},observed={0};
    CHECK(pipeline(&limits,&normal,comptime)==XR_XIR_OK);
    size_t sites=attempts;CHECK(sites && sites<20000);
    printf("source generation sites=%zu allocated=%llu peak=%llu work=%llu\n",sites,
        (unsigned long long)normal.allocated_bytes,(unsigned long long)normal.peak_bytes,(unsigned long long)normal.work);
    fflush(stdout);
    if (baseline) return 0;
    for (size_t i=0;i<sites;++i) {
        attempts=0;fail_at=i;XrXirStatus status=pipeline(&limits,&observed,comptime);
        if (status!=XR_XIR_OUT_OF_MEMORY) fprintf(stderr,"site=%zu status=%u\n",i,(unsigned)status);
        CHECK(status==XR_XIR_OUT_OF_MEMORY);
    }
    fail_at=SIZE_MAX;attempts=0;
    XrCompileResourceLimits exact={normal.allocated_bytes,normal.peak_bytes,normal.work};
    CHECK(pipeline(&exact,&observed,comptime)==XR_XIR_OK);
    CHECK(observed.allocated_bytes==normal.allocated_bytes && observed.peak_bytes==normal.peak_bytes && observed.work==normal.work);
    for (unsigned axis=0;axis<3;++axis) {
        XrCompileResourceLimits short_limit=exact;
        if (!axis) --short_limit.allocated_bytes;
        else if (axis==1) --short_limit.live_bytes;
        else --short_limit.work;
        attempts=0;CHECK(pipeline(&short_limit,&observed,comptime)==XR_XIR_BUDGET);
    }
    CHECK(!physical);
    puts("Source generation full physical FI, staged old-binding preservation, exact/minus-one budgets and zero physical allocations passed");
    return 0;
}
