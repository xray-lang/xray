/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_module_graph_logical_source.c - Governed source and owned ordinary edges
 */
#include "base/xmalloc.h"
#include "base/xcompile_resources.h"
#include "module/xmodule_graph.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
typedef struct GraphBlock { void *pointer;size_t bytes; } GraphBlock;
static GraphBlock blocks[512];
static size_t attempts,fail_at=SIZE_MAX,live,live_bytes;
static void *graph_malloc(size_t bytes) {
    if (attempts++==fail_at)return NULL;
    void *out=xr_malloc(bytes);CHECK(out);
    size_t i=0;while(i<512 && blocks[i].pointer)++i;CHECK(i<512);
    blocks[i]=(GraphBlock){out,bytes};++live;live_bytes+=bytes;return out;
}
static void graph_free(void *out) {
    if(!out)return;
    size_t i=0;while(i<512 && blocks[i].pointer!=out)++i;CHECK(i<512);
    --live;live_bytes-=blocks[i].bytes;blocks[i]=(GraphBlock){0};xr_free(out);
}
#pragma push_macro("xr_malloc")
#pragma push_macro("xr_free")
#undef xr_malloc
#undef xr_free
#define xr_malloc(bytes) graph_malloc(bytes)
#define xr_free(pointer) graph_free(pointer)
#include "base/xcompile_resources.c"
#pragma pop_macro("xr_free")
#pragma pop_macro("xr_malloc")
#include "module/xmodule_graph.c"
static const XrModuleIdentityAuthority script={XR_MODULE_IDENTITY_MEMORY,"logical-root",NULL};
static const XrModuleIdentityAuthority prelude={XR_MODULE_IDENTITY_STDLIB,"prelude",NULL};
static const char ordering[]="export enum Ordering { Relaxed, Acquire, Release, AcquireRelease, SeqCst }";
static XrCompileResourceLimits limits(void) {
    return (XrCompileResourceLimits){UINT64_C(32)<<20,UINT64_C(8)<<20,UINT64_C(64)<<20};
}
static XrModuleStatus probe(XrCompileResourceLimits caps,unsigned mode,XrCompileResourceStats *stats) {
    XrCompileResources *resources=NULL;XrCompilerSession *session=NULL;
    XrModuleResolver *resolver=NULL;XrModuleGraph *graph=NULL;
    XrCompileResourceStatus opened=xr_compile_resources_new(&caps,&resources);
    XrModuleStatus status=module_resource_status(opened);
    if(status==XR_MODULE_OK) {
        XrCompilerSessionStatus ss=xr_compile_session_new(resources,&session);
        status=ss==XR_COMPILER_SESSION_OK?XR_MODULE_OK:ss==XR_COMPILER_SESSION_OUT_OF_MEMORY?XR_MODULE_OUT_OF_MEMORY:XR_MODULE_BUDGET;
    }
    if(status==XR_MODULE_OK)status=xr_compile_module_resolver_new(resources,&(XrModuleResolverConfig){0},&resolver);
    if(status==XR_MODULE_OK)status=xr_compile_module_graph_new(resources,session,resolver,&graph);
    if(status==XR_MODULE_OK)status=xr_compile_module_graph_build_source(graph,&script,"export fn entry()->i64{return 41}",NULL);
    if(status==XR_MODULE_OK)status=xr_compile_module_graph_topological_sort(graph);
    if(status==XR_MODULE_OK)status=xr_compile_module_graph_include_logical_source(graph,&prelude,
        "prelude/builtin_symbols.def","<generated Ordering>",ordering,NULL);
    if(status==XR_MODULE_OK) {
        CHECK(graph->entry_index==0 && graph->spec_count==2 && !graph->topo_order && !graph->topo_count);
        CHECK(graph->specs[1].kind==XR_MOD_STDLIB && graph->specs[1].authority.kind==XR_MODULE_IDENTITY_STDLIB);
        CHECK(!strcmp(graph->specs[1].canonical,"stdlib-module-v1:module=7:prelude:path=27:prelude/builtin_symbols.def"));
        CHECK(graph->specs[1].ast && graph->specs[1].status==XR_MODSPEC_RESOLVED);
        status=xr_compile_module_graph_add_dependency(graph,0,1);
    }
    if(status==XR_MODULE_OK)status=xr_compile_module_graph_topological_sort(graph);
    if(status==XR_MODULE_OK) {
        CHECK(graph->topo_count==2 && graph->topo_order[0]==1 && graph->topo_order[1]==0 && graph->entry_index==0);
        int *order=graph->topo_order;
        status=xr_compile_module_graph_add_dependency(graph,0,1);
        if(status==XR_MODULE_OK)CHECK(graph->topo_order==order && graph->specs[0].dep_count==1);
    }
    if(status==XR_MODULE_OK) {
        int *order=graph->topo_order;
        status=xr_compile_module_graph_include_logical_source(graph,&prelude,
            "prelude/builtin_symbols.def","<generated Ordering>",ordering,NULL);
        if(status==XR_MODULE_OK)CHECK(graph->entry_index==0 && graph->spec_count==2 && graph->topo_order==order);
    }
    if(status==XR_MODULE_OK && mode) {
        if(mode==1)status=xr_compile_module_graph_include_logical_source(graph,&prelude,
            "prelude/builtin_symbols.def","<generated Ordering>","export enum Ordering { Wrong }",NULL);
        else if(mode==2)status=xr_compile_module_graph_include_logical_source(graph,&prelude,
            "prelude/builtin_symbols.def","<changed locator>",ordering,NULL);
        else if(mode==3) {
            CHECK(xr_compile_module_graph_add_dependency(graph,-1,1)==XR_MODULE_INVALID && graph->topo_order);
            CHECK(xr_compile_module_graph_add_dependency(graph,0,2)==XR_MODULE_INVALID && graph->topo_order);
            status=xr_compile_module_graph_add_dependency(graph,1,0);
            if(status==XR_MODULE_OK)status=xr_compile_module_graph_topological_sort(graph);
            if(status==XR_MODULE_INVALID)CHECK(graph->has_cycle);
        } else if(mode==4) {
            const XrModuleIdentityAuthority wrong={XR_MODULE_IDENTITY_STDLIB,"prelude","/not-a-real-root"};
            status=xr_compile_module_graph_include_logical_source(graph,&wrong,
                "prelude/builtin_symbols.def","<generated Ordering>",ordering,NULL);
        } else CHECK(false);
    }
    if(stats && resources)CHECK(xr_compile_resources_stats(resources,stats)==XR_COMPILE_RESOURCE_OK);
    xr_compile_module_graph_free(graph);xr_compile_module_resolver_free(resolver);xr_compile_session_free(session);
    if(resources) {XrCompileResourceStats end={0};XrCompileResourceStatus es=xr_compile_resources_stats(resources,&end);if(es!=XR_COMPILE_RESOURCE_OK || end.live_bytes!=sizeof(XrCompileResources))fprintf(stderr,"cleanup status=%d module=%d mode=%u fail=%zu attempts=%zu live=%llu blocks=%zu\n",(int)es,(int)status,mode,fail_at,attempts,(unsigned long long)end.live_bytes,live);CHECK(es==XR_COMPILE_RESOURCE_OK && end.live_bytes==sizeof(XrCompileResources));}
    xr_compile_resources_release(resources);CHECK(!live && !live_bytes);return status;
}
int main(void) {
    XrCompileResourceStats stats={0};attempts=0;
    CHECK(probe(limits(),0,&stats)==XR_MODULE_OK);
    size_t sites=attempts;CHECK(sites && sites<20000);
    for(unsigned mode=1;mode<=4;++mode)CHECK(probe(limits(),mode,NULL)==XR_MODULE_INVALID);
    for(size_t i=0;i<sites;++i) {
        attempts=0;fail_at=i;CHECK(probe(limits(),0,NULL)==XR_MODULE_OUT_OF_MEMORY && attempts>i);fail_at=SIZE_MAX;
    }
    for(unsigned axis=0;axis<3;++axis) {
        XrCompileResourceLimits exact=limits();
        if(!axis)exact.allocated_bytes=stats.allocated_bytes;else if(axis==1)exact.live_bytes=stats.peak_bytes;else exact.work=stats.work;
        CHECK(probe(exact,0,NULL)==XR_MODULE_OK);
        if(!axis)--exact.allocated_bytes;else if(axis==1)--exact.live_bytes;else --exact.work;
        CHECK(probe(exact,0,NULL)==XR_MODULE_BUDGET);
    }
    printf("logical graph include: actual OOM=%zu allocated=%llu peak=%llu work=%llu 3 axes physical=0\n",sites,
        (unsigned long long)stats.allocated_bytes,(unsigned long long)stats.peak_bytes,(unsigned long long)stats.work);
    return 0;
}
