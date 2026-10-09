/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 */
#include "../../xir/xir_construction_fixture.h"
#include "module/xmodule_graph.h"
#include "module/xlockfile.h"
#include <windows.h>
#include "toolchain/xcompiler_session.h"
#include "frontend/parser/xast.h"
#include "xir/xxir_library_catalog.h"
#include "base/xsha256.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while(0)
typedef struct Allocation { void *pointer; size_t bytes; } Allocation;
static Allocation allocations[8192];
static size_t attempts, fail_at = SIZE_MAX, physical_live, physical_total, physical_peak, block_count;
static void *observed_alloc(size_t bytes) {
    if (attempts++ == fail_at) return NULL;
    void *pointer = xr_malloc(bytes); CHECK(pointer);
    size_t i = 0; while (i < 8192 && allocations[i].pointer) ++i; CHECK(i < 8192);
    allocations[i] = (Allocation){pointer,bytes}; ++block_count; physical_live += bytes; physical_total += bytes;
    if (physical_live > physical_peak) physical_peak = physical_live; return pointer;
}
static void observed_free(void *pointer) {
    if (!pointer) return;
    size_t i = 0; while (i < 8192 && allocations[i].pointer != pointer) ++i; CHECK(i < 8192);
    physical_live -= allocations[i].bytes; --block_count; allocations[i] = (Allocation){0}; xr_free(pointer);
}
#undef xr_malloc
#undef xr_free
#define xr_malloc(bytes) observed_alloc(bytes)
#define xr_free(pointer) observed_free(pointer)
#define xr_compile_resources_work real_resources_work
#define xr_compile_resources_alloc real_resources_alloc
#define xr_compile_resources_calloc real_resources_calloc
#define xr_compile_resources_resize real_resources_resize
#include "base/xcompile_resources.c"
#undef xr_compile_resources_work
#undef xr_compile_resources_alloc
#undef xr_compile_resources_calloc
#undef xr_compile_resources_resize
static bool recording;
static size_t graph_growth_site = SIZE_MAX;
static uint64_t boundaries[200000]; static size_t boundary_count;
static XrCompileResourceStatus record(XrCompileResources *resources, XrCompileResourceStatus status) {
    if (recording && status == XR_COMPILE_RESOURCE_OK) {
        XrCompileResourceStats measured; CHECK(xr_compile_resources_stats(resources,&measured) == XR_COMPILE_RESOURCE_OK);
        if (!boundary_count || boundaries[boundary_count-1] != measured.work) {
            CHECK(boundary_count < 200000); boundaries[boundary_count++] = measured.work;
        }
    }
    return status;
}
XR_FUNC XrCompileResourceStatus xr_compile_resources_work(XrCompileResources *r,uint64_t n) { return record(r,real_resources_work(r,n)); }
XR_FUNC XrCompileResourceStatus xr_compile_resources_alloc(XrCompileResources *r,size_t n,void **out) { return record(r,real_resources_alloc(r,n,out)); }
XR_FUNC XrCompileResourceStatus xr_compile_resources_calloc(XrCompileResources *r,size_t n,size_t s,void **out) { return record(r,real_resources_calloc(r,n,s,out)); }
XR_FUNC XrCompileResourceStatus xr_compile_resources_resize(XrCompileResources *r,void **p,size_t n) {
    if (n == 32*sizeof(XrModuleSpec)) graph_growth_site = attempts;
    return record(r,real_resources_resize(r,p,n));
}
static const XrCompileResourceLimits unlimited = {UINT64_MAX,UINT64_MAX,UINT64_MAX};
static XrCompileResourceStats measured;
static unsigned char packet_bytes[4096], canary;
static XrXirLibraryInput library_input;
static XrXirLibraryModuleInput library_binding;
static char main_path[4096], cycle_path[4096], missing_path[4096];
static const char *root_path;
static void reset(size_t failure) {
    CHECK(!physical_live && !block_count); attempts = physical_total = physical_peak = 0; fail_at = failure;
}
static XrCompileResourceStats stats(XrCompileResources *owner) {
    XrCompileResourceStats s; CHECK(xr_compile_resources_stats(owner,&s) == XR_COMPILE_RESOURCE_OK);
    CHECK(s.live_bytes == physical_live && s.allocated_bytes == physical_total && s.peak_bytes == physical_peak); return s;
}
static void make_packet(void) {
    reset(SIZE_MAX); XrCompileResources *owner = NULL;
    CHECK(xr_compile_resources_new(&unlimited,&owner) == XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext context = {owner,xr_xir_compile_default_limits()};
    const XrXirInstruction init_ops[] = {{XR_XIR_RETURN,XR_XIR_UNIT,{0,0},{0,0},0,{0,0}}};
    const XrXirInstruction ops[] = {{XR_XIR_CONST_INT,XR_XIR_I64,{0,0},{0,0},42,{0,0}}, {XR_XIR_RETURN,XR_XIR_UNIT,{0,0},{0,0},0,{0,0}}};
    const XrXirBlock init_block = {0,1,0,0}, block = {0,2,0,0};
    const XrXirFunction functions[] = {{"init",4,NULL,0,XR_XIR_UNIT,&init_block,1,init_ops,1,NULL,0}, {"answer",6,NULL,0,XR_XIR_I64,&block,1,ops,2,NULL,0}};
    const char *canonical = "stdlib-module-v1:module=2:io:path=12:io/output.xr";
    XrXirSourceModule source = {canonical,(uint32_t)strlen(canonical),NULL,0,0};
    const XrXirFunctionIdentity names[] = {{0,0,0,0,0,0,0, 0, 0},{0,1,0,0,0,0,0, 0, 0}};
    XrXirDeclarations declarations = {&source,1,names,NULL,0,NULL,0,UINT32_MAX,UINT32_MAX,NULL};
    XrXirModule module = {XR_XIR_BUILT,functions,2,&declarations,NULL,NULL,NULL,XR_XIR_LIBRARY,NULL};
    XrXirArtifact *artifact = NULL; XrXirCheckedPacket packet = {0};
    CHECK(xir_fixture_check(&context, &module, &artifact, NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_checked_write(artifact,&packet,NULL) == XR_XIR_OK && packet.length < sizeof(packet_bytes));
    memcpy(packet_bytes,packet.bytes,packet.length);
    library_binding = (XrXirLibraryModuleInput){{XR_MODULE_IDENTITY_STDLIB,"io",root_path},"io/output.xr"};
    library_input = (XrXirLibraryInput){packet_bytes,packet.length,{0}, &library_binding,1};
    xr_sha256(packet_bytes,packet.length,library_input.sha256);
    xr_xir_compile_artifact_free(artifact); xr_xir_compile_checked_packet_free(&packet); xr_compile_resources_release(owner);
    CHECK(!physical_live && !block_count);
}
static XrModuleStatus from_session(XrCompilerSessionStatus s) {
    return s == XR_COMPILER_SESSION_OK ? XR_MODULE_OK : s == XR_COMPILER_SESSION_OUT_OF_MEMORY ? XR_MODULE_OUT_OF_MEMORY :
        s == XR_COMPILER_SESSION_BUDGET ? XR_MODULE_BUDGET : XR_MODULE_INVALID;
}
static XrModuleStatus from_xir(XrXirStatus s) {
    return s == XR_XIR_OK ? XR_MODULE_OK : s == XR_XIR_OUT_OF_MEMORY ? XR_MODULE_OUT_OF_MEMORY :
        s == XR_XIR_BUDGET ? XR_MODULE_BUDGET : XR_MODULE_INVALID;
}
static XrModuleStatus pipeline(XrCompileResources *owner, unsigned kind, bool validate) {
    XrCompilerSession *session = NULL; XrModuleResolver *resolver = NULL; XrModuleGraph *graph = NULL; XrXirLibraryCatalog *catalog = NULL;
    XrModuleStatus status = from_session(xr_compile_session_new(owner,&session));
    if (status != XR_MODULE_OK) return status;
    XrModuleResolverConfig config = {0};
    if (kind == 2) {
        XrXirCompileContext context = {owner,xr_xir_compile_default_limits()};
        status = from_xir(xr_xir_compile_library_catalog_new_v2(&context,&library_input,1,&catalog));
        config.stdlib_path = root_path; config.catalog = catalog;
    }
    if (status == XR_MODULE_OK) {
        resolver = (void *)&canary;
        status = xr_compile_module_resolver_new(owner,&config,&resolver);
        if (status != XR_MODULE_OK) { CHECK(resolver == (void *)&canary); resolver = NULL; }
    }
    if (status == XR_MODULE_OK) {
        graph = (void *)&canary;
        status = xr_compile_module_graph_new(owner,session,resolver,&graph);
        if (status != XR_MODULE_OK) { CHECK(graph == (void *)&canary); graph = NULL; }
    }
    if (status == XR_MODULE_OK) {
        XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT,NULL,root_path};
        if (kind == 0) {
            authority = (XrModuleIdentityAuthority){XR_MODULE_IDENTITY_MEMORY,"owner-test",NULL};
            status = xr_compile_module_graph_build_source(graph,&authority,"var value = 42\n",NULL);
        } else if (kind == 2) status = xr_compile_module_graph_build_logical_source(graph,&authority,"main.xr",main_path,
            "import \"std/io/output\"\nvar value = 42\n",NULL);
        else status = xr_compile_module_graph_build(graph,kind == 3 ? cycle_path : main_path,&authority,NULL);
        if (status == XR_MODULE_OK) status = xr_compile_module_graph_topological_sort(graph);
    }
    if (validate && (status == XR_MODULE_OK || (kind == 3 && status == XR_MODULE_INVALID))) {
        CHECK(graph->resources == owner && graph->resolver->resources == owner);
        CHECK(graph->spec_count == (kind == 0 ? 1 : kind == 1 ? 3 : 2));
        CHECK(graph->topo_count == graph->spec_count && graph->specs[0].ast && graph->specs[0].ast->type == AST_PROGRAM);
        if (kind == 2) {
            CHECK(graph->specs[1].representation == XR_MODULE_CHECKED_LIBRARY && !graph->specs[1].ast);
            CHECK(xr_xir_compile_artifact_context(graph->specs[1].resource->checked)->resources == owner);
            CHECK(graph->topo_order[0] == 1);
        } else if (kind == 1) CHECK(graph->topo_order[0] == 2 && graph->topo_order[1] == 1 && graph->topo_order[2] == 0);
        else if (kind == 3) CHECK(graph->has_cycle && strstr(graph->cycle_desc,"cycle_a.xr -> cycle_b.xr -> cycle_a.xr"));
    }
    xr_compile_module_graph_free(graph); xr_compile_module_resolver_free(resolver); xr_xir_compile_library_catalog_free(catalog);
    xr_compile_session_free(session); return status;
}
static size_t run(unsigned kind, const XrCompileResourceLimits *limits, size_t failure, XrModuleStatus expected) {
    reset(failure); XrCompileResources *owner = NULL;
    XrCompileResourceStatus created = xr_compile_resources_new(limits,&owner);
    if (created != XR_COMPILE_RESOURCE_OK) {
        CHECK(!owner && ((created == XR_COMPILE_RESOURCE_OUT_OF_MEMORY && expected == XR_MODULE_OUT_OF_MEMORY) ||
            (created == XR_COMPILE_RESOURCE_BUDGET && expected == XR_MODULE_BUDGET))); return attempts;
    }
    DWORD handles_before = 0, handles_after = 0;
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&handles_before));
    XrModuleStatus status = pipeline(owner,kind,expected == XR_MODULE_OK || (kind == 3 && expected == XR_MODULE_INVALID));
    if (status != expected) fprintf(stderr,"kind=%u failure=%zu work=%llu expected=%d actual=%d\n",kind,failure,(unsigned long long)limits->work,expected,status);
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&handles_after) && handles_before == handles_after);
    CHECK(status == expected && block_count == 1);
    measured = stats(owner); xr_compile_resources_release(owner); CHECK(!physical_live && !block_count); return attempts;
}
static void qualify(unsigned kind) {
    boundary_count = 0; recording = true;
    XrModuleStatus expected = kind == 3 ? XR_MODULE_INVALID : XR_MODULE_OK;
    size_t sites = run(kind,&unlimited,SIZE_MAX,expected); XrCompileResourceStats baseline = measured;
    recording = false; size_t submitted = boundary_count;
    for (size_t failure = 0; failure < sites; ++failure) {
        run(kind,&unlimited,failure,XR_MODULE_OUT_OF_MEMORY); CHECK(attempts == failure+1);
    }
    for (size_t i = 0; i < submitted; ++i) {
        XrCompileResourceLimits limits = unlimited; limits.work = boundaries[i]-1;
        run(kind,&limits,SIZE_MAX,XR_MODULE_BUDGET);
    }
    for (unsigned axis = 0; axis < 3; ++axis) for (unsigned below = 0; below < 2; ++below) {
        XrCompileResourceLimits limits = unlimited;
        if (!axis) limits.allocated_bytes = baseline.allocated_bytes-below;
        else if (axis == 1) limits.live_bytes = baseline.peak_bytes-below;
        else limits.work = baseline.work-below;
        run(kind,&limits,SIZE_MAX,below ? XR_MODULE_BUDGET : expected);
    }
    printf("module kind=%u: %zu real OOM sites, %zu actual work submission cutoffs, bytes/live/work exact-minus1, physical zero PASS\n",kind,sites,submitted); fflush(stdout);
}
static void rejection_and_lifetime(void) {
    reset(SIZE_MAX); XrCompileResources *first = NULL, *second = NULL;
    CHECK(xr_compile_resources_new(&unlimited,&first) == XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_new(&unlimited,&second) == XR_COMPILE_RESOURCE_OK);
    XrOsIoPolicy policy = xr_compile_io_policy(first); XrLockfile *lockfile = NULL;
    CHECK(xr_lockfile_new_owned(&policy,&lockfile) == XR_OS_IO_OK);
    XrModuleResolverConfig config = {0}; config.lockfile = lockfile;
    XrModuleResolver *resolver = (void *)&canary; size_t before = attempts;
    CHECK(xr_compile_module_resolver_new(second,&config,&resolver) == XR_MODULE_INVALID && resolver == (void *)&canary && attempts == before);
    config.lockfile = NULL; resolver = NULL;
    CHECK(xr_compile_module_resolver_new(second,&config,&resolver) == XR_MODULE_OK);
    CHECK(xr_compile_module_resolver_set_lockfile(resolver,lockfile) == XR_MODULE_INVALID && !resolver->config.lockfile);
    XrCompilerSession *session = NULL; CHECK(xr_compile_session_new(first,&session) == XR_COMPILER_SESSION_OK);
    XrModuleGraph *graph = (void *)&canary; before = attempts;
    CHECK(xr_compile_module_graph_new(first,session,resolver,&graph) == XR_MODULE_INVALID && graph == (void *)&canary && before == attempts);
    XrXirCompileContext context = {first,xr_xir_compile_default_limits()}; XrXirLibraryCatalog *catalog = NULL;
    CHECK(xr_xir_compile_library_catalog_new_v2(&context,&library_input,1,&catalog) == XR_XIR_OK);
    XrModuleResolver *other = (void *)&canary; config.catalog = catalog; before = attempts;
    CHECK(xr_compile_module_resolver_new(second,&config,&other) == XR_MODULE_INVALID && other == (void *)&canary && before == attempts);
    xr_compile_module_resolver_free(resolver); xr_compile_session_free(session); xr_xir_compile_library_catalog_free(catalog);
    xr_lockfile_free_owned(lockfile); xr_compile_resources_release(first); xr_compile_resources_release(second);
    CHECK(!physical_live && !block_count);

    reset(SIZE_MAX); first = NULL; CHECK(xr_compile_resources_new(&unlimited,&first) == XR_COMPILE_RESOURCE_OK);
    config = (XrModuleResolverConfig){0}; resolver = NULL;
    CHECK(xr_compile_module_resolver_new(first,&config,&resolver) == XR_MODULE_OK);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT,NULL,root_path}; XrModuleId id = {0};
    CHECK(xr_compile_module_resolver_resolve(resolver,"./dep",main_path,&authority,&id,NULL) == XR_MODULE_OK);
    char other_root[4096], other_path[4096];
    CHECK(snprintf(other_root,sizeof(other_root),"%s/other",root_path) > 0);
    CHECK(snprintf(other_path,sizeof(other_path),"%s/main.xr",other_root) > 0);
    XrModuleIdentityAuthority foreign = {XR_MODULE_IDENTITY_SCRIPT,NULL,other_root};
    XrModuleId unchanged; memset(&unchanged,0xa5,sizeof(unchanged)); XrModuleId copy = unchanged;
    CHECK(xr_compile_module_resolver_resolve(resolver,"./dep",other_path,&foreign,&unchanged,NULL) == XR_MODULE_INVALID);
    CHECK(!memcmp(&unchanged,&copy,sizeof(copy)));
    XrModuleId cached = {0}; CHECK(xr_compile_module_resolver_resolve(resolver,"./dep",main_path,&authority,&cached,NULL) == XR_MODULE_OK);
    CHECK(!strcmp(cached.canonical,id.canonical) && cached.canonical != id.canonical);
    xr_compile_module_id_cleanup(&cached); xr_compile_resources_release(first); xr_compile_module_resolver_free(resolver);
    CHECK(strstr(id.source_path,"dep.xr") && !strcmp(id.logical_path,"dep.xr"));
    xr_compile_module_id_cleanup(&id); CHECK(!physical_live && !block_count);
    puts("module: foreign session/catalog/lockfile rejected before alloc; cache authority and returned ownership PASS");
}
static void topology_retry(void) {
    for (size_t site = 0; site < 4; ++site) {
        reset(SIZE_MAX); XrCompileResources *owner = NULL;
        CHECK(xr_compile_resources_new(&unlimited,&owner) == XR_COMPILE_RESOURCE_OK);
        XrCompilerSession *session = NULL; XrModuleResolver *resolver = NULL; XrModuleGraph *graph = NULL;
        XrModuleResolverConfig config = {0};
        CHECK(xr_compile_session_new(owner,&session) == XR_COMPILER_SESSION_OK);
        CHECK(xr_compile_module_resolver_new(owner,&config,&resolver) == XR_MODULE_OK);
        CHECK(xr_compile_module_graph_new(owner,session,resolver,&graph) == XR_MODULE_OK);
        XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT,NULL,root_path};
        CHECK(xr_compile_module_graph_build(graph,main_path,&authority,NULL) == XR_MODULE_OK);
        CHECK(xr_compile_module_graph_topological_sort(graph) == XR_MODULE_OK);
        int *order = graph->topo_order; int saved[3], topo[3], scc[3]; memcpy(saved,order,sizeof(saved));
        for (int i = 0; i < 3; ++i) { topo[i] = graph->specs[i].topo_index; scc[i] = graph->specs[i].scc_id; }
        size_t before = physical_live; fail_at = attempts+site;
        CHECK(xr_compile_module_graph_topological_sort(graph) == XR_MODULE_OUT_OF_MEMORY);
        CHECK(graph->topo_order == order && !memcmp(saved,order,sizeof(saved)) && before == physical_live);
        for (int i = 0; i < 3; ++i) CHECK(topo[i] == graph->specs[i].topo_index && scc[i] == graph->specs[i].scc_id);
        fail_at = SIZE_MAX;
        int found = -1; CHECK(xr_compile_module_graph_find(graph,graph->specs[1].canonical,&found) == XR_MODULE_OK && found == 1);
        bool owns = false; CHECK(xr_compile_module_graph_owns_top_level_decl(graph,&graph->specs[0],graph->specs[0].ast->as.program.statements[0],&owns) == XR_MODULE_OK && owns);
        XrCompileResourceStats current = stats(owner);
        CHECK(xr_compile_resources_work(owner,UINT64_MAX-current.work) == XR_COMPILE_RESOURCE_OK);
        found = 123; owns = true;
        CHECK(xr_compile_module_graph_find(graph,"absent",&found) == XR_MODULE_BUDGET && found == 123);
        CHECK(xr_compile_module_graph_find_source(graph,main_path,&found) == XR_MODULE_BUDGET && found == 123);
        CHECK(xr_compile_module_graph_owns_top_level_decl(graph,&graph->specs[0],graph->specs[0].ast,&owns) == XR_MODULE_BUDGET && owns);
        CHECK(xr_compile_module_graph_topological_sort(graph) == XR_MODULE_BUDGET && graph->topo_order == order);
        XrModuleGraph *next = (void *)&canary;
        CHECK(xr_compile_module_graph_new(owner,session,resolver,&next) == XR_MODULE_BUDGET && next == (void *)&canary);
        xr_compile_resources_release(owner);
        CHECK(graph->specs[0].ast->type == AST_PROGRAM && !strcmp(graph->specs[1].logical_path,"dep.xr"));
        xr_compile_module_graph_free(graph); xr_compile_module_resolver_free(resolver); xr_compile_session_free(session);
        CHECK(!physical_live && !block_count);
    }
    puts("module: topology retry OOM preserves order, depleted query canaries, shared request exhaustion and lifetime PASS");
}
static void many_modules(void) {
    char path[4096]; CHECK(snprintf(path,sizeof(path),"%s/many.xr",root_path) > 0);
    size_t growth = SIZE_MAX;
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        reset(attempt ? growth : SIZE_MAX); XrCompileResources *owner = NULL;
        CHECK(xr_compile_resources_new(&unlimited,&owner) == XR_COMPILE_RESOURCE_OK);
        XrCompilerSession *session = NULL; XrModuleResolver *resolver = NULL; XrModuleGraph *graph = NULL;
        XrModuleResolverConfig config = {0};
        CHECK(xr_compile_session_new(owner,&session) == XR_COMPILER_SESSION_OK);
        CHECK(xr_compile_module_resolver_new(owner,&config,&resolver) == XR_MODULE_OK);
        CHECK(xr_compile_module_graph_new(owner,session,resolver,&graph) == XR_MODULE_OK);
        XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT,NULL,root_path};
        XrModuleStatus status = xr_compile_module_graph_build(graph,path,&authority,NULL);
        if (!attempt) {
            CHECK(status == XR_MODULE_OK && graph->spec_count == 21 && graph->spec_capacity == 32 && graph->specs[0].dep_count == 20);
            for (int i = 0; i < 20; ++i) CHECK(graph->specs[0].dep_indices[i] == i+1 && graph->specs[i+1].ast);
            CHECK(xr_compile_module_graph_topological_sort(graph) == XR_MODULE_OK && graph->topo_order[20] == 0);
            growth = graph_growth_site; CHECK(growth != SIZE_MAX);
        } else CHECK(status == XR_MODULE_OUT_OF_MEMORY && graph->resolution_status == XR_MODULE_OUT_OF_MEMORY && attempts == growth+1);
        xr_compile_module_graph_free(graph); xr_compile_module_resolver_free(resolver); xr_compile_session_free(session);
        CHECK(block_count == 1); (void)stats(owner); xr_compile_resources_release(owner); CHECK(!physical_live && !block_count);
    }
    puts("module: 21-file growth keeps source edges valid; actual spec resize OOM cleaned with physical zero PASS");
}
int main(int argc, char **argv) {
    CHECK(argc == 2); root_path = argv[1];
    CHECK(snprintf(main_path,sizeof(main_path),"%s/main.xr",root_path) > 0);
    CHECK(snprintf(cycle_path,sizeof(cycle_path),"%s/cycle_a.xr",root_path) > 0);
    CHECK(snprintf(missing_path,sizeof(missing_path),"%s/missing.xr",root_path) > 0);
    make_packet();
    for (unsigned kind = 0; kind < 4; ++kind) qualify(kind);
    rejection_and_lifetime(); topology_retry(); many_modules();
    puts("real module owner gates PASS"); return 0;
}
