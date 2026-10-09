/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_dependency_snapshot.c - Transactional observation copy, six physical allocation failures
 */
#include "xir/xxir_source_query_internal.h"
#include "xir/xxir_source_dependencies.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
#include "xir_source_compile_owner.h"
static XrXirSourceSnapshot *empty(XrXirCompileContext *context) {
    XrCompileResourceLimits limits={1048576,1048576,1048576};
    CHECK(xr_compile_resources_new(&limits,&context->resources)==XR_COMPILE_RESOURCE_OK);
    context->limits=xr_xir_compile_default_limits();
    XrXirConstruction *construction=NULL;
    CHECK(xr_xir_compile_construction_new(context,NULL,NULL,0,&construction)==XR_XIR_OK);
    XrXirSourceQueryModule modules[2]={{"root-id","root.xr",{{0}}},{"io-id","io",{{0}}}};
    XrXirSourceView view={0}; view.modules=modules; view.module_count=2;
    XrXirSourceSnapshot *snapshot=NULL;
    CHECK(xr_xir_compile_source_snapshot_copy_v2(context,&view,construction,&snapshot)==XR_XIR_OK);
    xr_xir_compile_construction_free(construction);return snapshot;
}
static void check(const XrXirSourceSnapshot *snapshot) {
    const XrXirSourceDependencies *facts=xr_xir_compile_source_snapshot_dependencies(snapshot);
    CHECK(facts && facts->count==2 && facts->entry==1 && !strcmp(facts->entry_path,"root.xr"));
    CHECK(facts->entries[0].module==1 && facts->entries[0].kind==XR_XIR_DEPENDENCY_STDLIB);
    CHECK(!facts->entries[0].imports && !facts->entries[0].import_count && !strcmp(facts->entries[0].path,"io"));
    CHECK(facts->entries[1].module==0 && facts->entries[1].import_count==1 && facts->entries[1].imports[0]==1);
    CHECK(!strcmp(facts->entries[1].path,"root.xr"));
}
static XrXirSourceSnapshot *clone(const XrXirSourceSnapshot *source,XrXirCompileContext *context) {
    XrCompileResourceLimits limits={1048576,1048576,1048576};
    CHECK(xr_compile_resources_new(&limits,&context->resources)==XR_COMPILE_RESOURCE_OK);
    context->limits=xr_xir_compile_default_limits();XrXirSourceSnapshot *copy=NULL;
    CHECK(xr_xir_compile_source_snapshot_copy_v2(context,xr_xir_compile_source_snapshot_view(source),
        xr_xir_compile_source_snapshot_construction(source),&copy)==XR_XIR_OK);
    CHECK(xr_xir_compile_source_snapshot_dependencies_copy(copy,
        xr_xir_compile_source_snapshot_dependencies(source))==XR_XIR_OK);return copy;
}
int main(void) {
    char dependency[]="io",root[]="root.xr";uint32_t edge=1;
    XrXirSourceDependency rows[2]={{1,XR_XIR_DEPENDENCY_STDLIB,dependency,NULL,0},
        {0,XR_XIR_DEPENDENCY_FILE,root,&edge,1}};
    XrXirSourceDependencies facts={rows,2,1,root};
    /* 1 scratch ordinal map + 1 rows + 1 entry string + 2 row strings + 1 edge array. */
    for (size_t point=0;point<6;++point) {
        XrXirCompileContext context={0};XrXirSourceSnapshot *snapshot=empty(&context);
        size_t live=source_fixture_compile_live,bytes=source_fixture_compile_bytes;
        source_fixture_compile_attempts=0;source_fixture_compile_fail_at=point;source_fixture_compile_injected=false;
        CHECK(xr_xir_compile_source_snapshot_dependencies_copy(snapshot,&facts)==XR_XIR_OUT_OF_MEMORY);
        CHECK(source_fixture_compile_injected && source_fixture_compile_attempts==point+1);
        CHECK(!xr_xir_compile_source_snapshot_dependencies(snapshot));
        CHECK(source_fixture_compile_live==live && source_fixture_compile_bytes==bytes);
        source_fixture_compile_fail_at=SIZE_MAX;
        CHECK(xr_xir_compile_source_snapshot_dependencies_copy(snapshot,&facts)==XR_XIR_OK);check(snapshot);
        xr_xir_compile_source_snapshot_free(snapshot);xr_compile_resources_release(context.resources);
        CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    }
    XrXirCompileContext a={0},b={0},c={0};XrXirSourceSnapshot *first=empty(&a);
    rows[1].module=1;
    CHECK(xr_xir_compile_source_snapshot_dependencies_copy(first,&facts)==XR_XIR_BAD_STRUCTURE);
    rows[1].module=0;edge=0;
    CHECK(xr_xir_compile_source_snapshot_dependencies_copy(first,&facts)==XR_XIR_BAD_STRUCTURE);
    edge=1;facts.entry=2;
    CHECK(xr_xir_compile_source_snapshot_dependencies_copy(first,&facts)==XR_XIR_BAD_STRUCTURE);
    facts.entry=1;rows[0].imports=&edge;
    CHECK(xr_xir_compile_source_snapshot_dependencies_copy(first,&facts)==XR_XIR_BAD_STRUCTURE);
    rows[0].imports=NULL;source_fixture_compile_attempts=0;
    XrCompileResourceStats before={0},after={0};
    CHECK(xr_compile_resources_stats(a.resources,&before)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_xir_compile_source_snapshot_dependencies_copy(first,&facts)==XR_XIR_OK);
    CHECK(source_fixture_compile_attempts==6);check(first);
    CHECK(xr_compile_resources_stats(a.resources,&after)==XR_COMPILE_RESOURCE_OK);
    const uint64_t memory_header=sizeof(void *), allocation_header=sizeof(CompileAllocation);
    const uint64_t rows_bytes=2*sizeof(XrXirSourceDependency), root_bytes=sizeof("root.xr"), dep_bytes=sizeof("io");
    const uint64_t scratch=2*sizeof(uint32_t), edges=sizeof(uint32_t);
    const uint64_t payloads=5*memory_header+rows_bytes+2*root_bytes+dep_bytes+edges;
    const uint64_t owned=5*allocation_header+payloads;
    /* Descriptor + scratch zeroing + row checks + edge checks + owned zeroing,
     * copy bytes, string scans, row iteration and final atomic publication. */
    const uint64_t work=sizeof(facts)+(1+scratch)+rows_bytes+2+edges+
        5+payloads+rows_bytes+2*root_bytes+dep_bytes+edges+
        2*root_bytes+dep_bytes+2+sizeof(facts)+sizeof(bool);
    CHECK(after.allocated_bytes-before.allocated_bytes==owned+allocation_header+scratch);
    CHECK(after.live_bytes-before.live_bytes==owned && after.work-before.work==work);
    size_t attempts=source_fixture_compile_attempts;
    CHECK(xr_xir_compile_source_snapshot_dependencies_copy(first,NULL)==XR_XIR_BAD_STRUCTURE);
    CHECK(xr_xir_compile_source_snapshot_dependencies_copy(first,&facts)==XR_XIR_BAD_STRUCTURE);
    CHECK(source_fixture_compile_attempts==attempts);check(first);
    dependency[0]='X';root[0]='X';edge=0;memset(rows,0,sizeof(rows));check(first);
    XrXirSourceSnapshot *second=clone(first,&b);
    xr_xir_compile_source_snapshot_free(first);xr_compile_resources_release(a.resources);check(second);
    XrXirSourceSnapshot *third=clone(second,&c);
    xr_xir_compile_source_snapshot_free(second);xr_compile_resources_release(b.resources);check(third);
    xr_compile_resources_release(c.resources);check(third);xr_xir_compile_source_snapshot_free(third);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    puts("dependency snapshot fixed FI=6 cross-owner copies=2 physical=0/0");return 0;
}
