/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_catalog_owner.c - Observe catalog and identity physical ownership
 */
#include "xir/xxir.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_library_catalog.h"
#include "xir/xxir_effects.h"
#include "xir/xxir_types.h"
#include "xir/xxir_constraints.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
typedef struct Allocation { void *pointer; size_t bytes; } Allocation;
static Allocation allocations[4096];
static size_t attempts, fail_at = SIZE_MAX, live, total, peak, live_count;
static void *observe_alloc(size_t bytes) {
    if (attempts++ == fail_at) return NULL;
    void *memory = xr_malloc(bytes);
    CHECK(memory);
    size_t i = 0;
    while (i < 4096 && allocations[i].pointer) ++i;
    CHECK(i < 4096);
    allocations[i] = (Allocation){memory, bytes};
    ++live_count; live += bytes; total += bytes;
    if (live > peak) peak = live;
    return memory;
}
static void observe_free(void *memory) {
    if (!memory) return;
    size_t i = 0;
    while (i < 4096 && allocations[i].pointer != memory) ++i;
    CHECK(i < 4096);
    live -= allocations[i].bytes; --live_count;
    allocations[i] = (Allocation){0};
    xr_free(memory);
}
#undef xr_malloc
#undef xr_free
#define xr_malloc(bytes) observe_alloc(bytes)
#define xr_free(memory) observe_free(memory)
#include "base/xcompile_resources.c"

static XrCompileResourceStats stats(XrCompileResources *owner) {
    XrCompileResourceStats s;
    CHECK(xr_compile_resources_stats(owner, &s) == XR_COMPILE_RESOURCE_OK);
    CHECK(s.live_bytes == live && s.allocated_bytes == total && s.peak_bytes == peak);
    return s;
}
static void reset(size_t failure) {
    CHECK(!live && !live_count);
    attempts = total = peak = 0; fail_at = failure;
}
static const XrCompileResourceLimits unlimited = {UINT64_MAX, UINT64_MAX, UINT64_MAX};
#include "base/xsha256.h"
static const char *canonical[] = {
    "module-id-v1:kind=6:script:namespace=0::path=6:lib.xr",
    "stdlib-module-v1:module=2:io:path=12:io/output.xr"
};
static unsigned char packets[2][4096];
static XrXirLibraryInput inputs[2];
static char canary_byte;
static void make_packet(unsigned kind) {
    reset(SIZE_MAX);
    XrCompileResources *owner = NULL;
    CHECK(xr_compile_resources_new(&unlimited,&owner) == XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext context = {owner,xr_xir_compile_default_limits()};
    const XrXirInstruction init_ops[] = {{XR_XIR_RETURN,XR_XIR_UNIT,{0,0},{0,0},0,{0,0}}};
    const XrXirInstruction ops[] = {
        {XR_XIR_CONST_INT,XR_XIR_I64,{0,0},{0,0},42,{0,0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{0,0},{0,0},0,{0,0}}
    };
    const XrXirBlock init_block = {0,1,0,0}, block = {0,2,0,0};
    const XrXirFunction functions[] = {
        {"init",4,NULL,0,XR_XIR_UNIT,&init_block,1,init_ops,1,NULL,0},
        {"answer",6,NULL,0,XR_XIR_I64,&block,1,ops,2,NULL,0}
    };
    XrXirSourceModule source = {canonical[kind],(uint32_t)strlen(canonical[kind]),NULL,0,0};
    const XrXirFunctionIdentity names[] = {{0,0,0,0,0,0,0},{0,1,0,0,0,0,0}};
    XrXirDeclarations declarations = {&source,1,names,NULL,0,NULL,0,UINT32_MAX,UINT32_MAX,NULL};
    XrXirModule module = {XR_XIR_BUILT,functions,2,&declarations,NULL,NULL,NULL,XR_XIR_LIBRARY,NULL};
    XrXirArtifact *artifact = NULL; XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_check(&context,&module,&artifact,NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_checked_write(artifact,&packet,NULL) == XR_XIR_OK);
    CHECK(packet.length <= sizeof(packets[kind]));
    memcpy(packets[kind],packet.bytes,packet.length);
    inputs[kind] = (XrXirLibraryInput){
        {kind ? XR_MODULE_IDENTITY_STDLIB : XR_MODULE_IDENTITY_SCRIPT,kind ? "io" : NULL,"C:/catalog-owner"},
        kind ? "io/output.xr" : "lib.xr",packets[kind],packet.length,{0}};
    xr_sha256(packets[kind],packet.length,inputs[kind].sha256);
    xr_xir_compile_artifact_free(artifact); xr_xir_compile_checked_packet_free(&packet);
    xr_compile_resources_release(owner); CHECK(!live && !live_count);
}
static XrXirStatus catalog_run(XrCompileResources *owner, unsigned kind) {
    XrXirCompileContext context = {owner,xr_xir_compile_default_limits()};
    XrXirLibraryCatalog *output = (void *)&canary_byte;
    XrXirStatus status = xr_xir_compile_library_catalog_new(&context,&inputs[kind],1,&output);
    if (status != XR_XIR_OK) { CHECK(output == (void *)&canary_byte); return status; }
    size_t count = 0;
    const XrModuleResourceBinding *binding = xr_xir_compile_library_catalog_resources(output,&count);
    CHECK(count == 1 && binding && !strcmp(binding->canonical,canonical[kind]));
    CHECK(xr_xir_compile_library_catalog_context(output)->resources == owner);
    CHECK(xr_xir_compile_artifact_context(binding->checked)->resources == owner);
    CHECK(xr_xir_compile_artifact_module(binding->checked)->functions[1].instructions[0].immediate == 42);
    xr_xir_compile_library_catalog_free(output); return XR_XIR_OK;
}
static XrXirStatus module_status(XrModuleStatus status) {
    switch (status) {
    case XR_MODULE_OK: return XR_XIR_OK;
    case XR_MODULE_BUDGET: return XR_XIR_BUDGET;
    case XR_MODULE_OUT_OF_MEMORY: return XR_XIR_OUT_OF_MEMORY;
    default: fprintf(stderr,"Unexpected module status %d\n",status); return XR_XIR_BAD_STRUCTURE;
    }
}
static XrXirStatus identity_run(XrCompileResources *owner, unsigned kind) {
    char *identity = &canary_byte, *logical = &canary_byte;
    XrModuleStatus status;
    if (kind == 0) status = xr_compile_module_identity_from_logical(owner,&inputs[1].authority,inputs[1].logical_path,&identity);
    else if (kind == 1) status = xr_compile_module_identity_from_source(owner,&inputs[1].authority,
        "C:/catalog-owner/io/output.xr",&identity,&logical);
    else {
        XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_MEMORY,&canary_byte,&canary_byte};
        status = xr_compile_module_identity_script_authority_from_source(owner,"catalog-owner-source.xr",&authority,&logical);
        if (status != XR_MODULE_OK) CHECK(authority.kind == XR_MODULE_IDENTITY_MEMORY &&
            authority.namespace_id == &canary_byte && authority.physical_root == &canary_byte);
        else {
            CHECK(authority.kind == XR_MODULE_IDENTITY_SCRIPT && !authority.namespace_id && authority.physical_root == logical);
            CHECK(xr_module_identity_authority_valid(&authority)); xr_compile_resources_free(logical);
        }
        if (status != XR_MODULE_OK) CHECK(logical == &canary_byte);
        return module_status(status);
    }
    if (status != XR_MODULE_OK) { CHECK(identity == &canary_byte && logical == &canary_byte); return module_status(status); }
    CHECK(!strcmp(identity,canonical[1]) && xr_module_identity_valid(identity,NULL));
    if (kind == 1) { CHECK(!strcmp(logical,inputs[1].logical_path)); xr_compile_resources_free(logical); }
    xr_compile_resources_free(identity); return XR_XIR_OK;
}
typedef XrXirStatus (*Run)(XrCompileResources *, unsigned);
static XrCompileResourceStats observed_run(Run run, unsigned kind, const XrCompileResourceLimits *limits,
    size_t failure, XrXirStatus expected) {
    reset(failure); XrCompileResources *owner = NULL;
    XrCompileResourceStatus created = xr_compile_resources_new(limits,&owner);
    if (created != XR_COMPILE_RESOURCE_OK) {
        CHECK((created == XR_COMPILE_RESOURCE_OUT_OF_MEMORY && expected == XR_XIR_OUT_OF_MEMORY) ||
              (created == XR_COMPILE_RESOURCE_BUDGET && expected == XR_XIR_BUDGET));
        CHECK(!owner && !live); return (XrCompileResourceStats){0};
    }
    size_t bootstrap = live;
    XrXirStatus result = run(owner,kind);
    if (result != expected) fprintf(stderr,"kind=%u fail=%zu limit=%llu expected=%d got=%d\n",kind,failure,
        (unsigned long long)limits->work,expected,result);
    CHECK(result == expected && live == bootstrap && live_count == 1);
    XrCompileResourceStats measured = stats(owner);
    xr_compile_resources_release(owner); CHECK(!live && !live_count); return measured;
}
static void qualify(Run run, unsigned kind, const char *label) {
    XrCompileResourceStats baseline = observed_run(run,kind,&unlimited,SIZE_MAX,XR_XIR_OK);
    size_t sites = attempts;
    for (size_t failure = 0; failure < sites; ++failure) {
        (void)observed_run(run,kind,&unlimited,failure,XR_XIR_OUT_OF_MEMORY);
        CHECK(attempts == failure+1);
    }
    for (unsigned axis = 0; axis < 3; ++axis) {
        for (unsigned below = 0; below < 2; ++below) {
            XrCompileResourceLimits limits = unlimited;
            if (!axis) limits.allocated_bytes = baseline.allocated_bytes-below;
            else if (axis == 1) limits.live_bytes = baseline.peak_bytes-below;
            else limits.work = baseline.work-below;
            (void)observed_run(run,kind,&limits,SIZE_MAX,below ? XR_XIR_BUDGET : XR_XIR_OK);
        }
    }
    for (unsigned axis = 0; axis < 2; ++axis) {
        XrCompileResourceLimits limits = unlimited;
        if (!axis) limits.allocated_bytes = baseline.allocated_bytes;
        else limits.work = baseline.work;
        reset(SIZE_MAX); XrCompileResources *owner = NULL;
        CHECK(xr_compile_resources_new(&limits,&owner) == XR_COMPILE_RESOURCE_OK);
        CHECK(run(owner,kind) == XR_XIR_OK);
        size_t bootstrap = live;
        XrCompileResourceStats first = stats(owner);
        CHECK(run(owner,kind) == XR_XIR_BUDGET && live == bootstrap);
        XrCompileResourceStats second = stats(owner);
        CHECK(second.allocated_bytes == first.allocated_bytes && second.work >= first.work);
        xr_compile_resources_release(owner); CHECK(!live && !live_count);
    }
    for (uint64_t work = 1; work < baseline.work; ++work) {
        XrCompileResourceLimits limits = unlimited; limits.work = work;
        (void)observed_run(run,kind,&limits,SIZE_MAX,XR_XIR_BUDGET);
    }
    printf("%s: real allocation sites=%zu; allocated=%llu peak=%llu work=%llu; every OOM/work boundary and exact/minus1 PASS\n",
        label,sites,(unsigned long long)baseline.allocated_bytes,(unsigned long long)baseline.peak_bytes,
        (unsigned long long)baseline.work);
}
static void negative_and_lifetime(void) {
    reset(SIZE_MAX); XrCompileResources *owner = NULL;
    CHECK(xr_compile_resources_new(&unlimited,&owner) == XR_COMPILE_RESOURCE_OK);
    size_t bootstrap = live;
    XrXirCompileContext context = {owner,xr_xir_compile_default_limits()};
    XrXirLibraryCatalog *catalog = (void *)&canary_byte;
    CHECK(xr_xir_compile_library_catalog_new(NULL,inputs,1,&catalog) == XR_XIR_BAD_STRUCTURE);
    CHECK(catalog == (void *)&canary_byte);
    CHECK(xr_xir_compile_library_catalog_new(&context,inputs,SIZE_MAX,&catalog) == XR_XIR_BUDGET);
    CHECK(catalog == (void *)&canary_byte && live == bootstrap);
    for (unsigned kind = 0; kind < 5; ++kind) {
        XrXirLibraryInput wrong = inputs[1];
        if (!kind) wrong.sha256[0] ^= 1;
        else if (kind == 1) wrong.logical_path = "io/other.xr";
        else if (kind == 2) wrong.logical_path = "io/output.xr/";
        else if (kind == 3) wrong.authority.namespace_id = "other";
        else wrong.authority.kind = XR_MODULE_IDENTITY_PROJECT;
        CHECK(xr_xir_compile_library_catalog_new(&context,&wrong,1,&catalog) == (kind == 4 ? XR_XIR_BAD_STAGE : XR_XIR_BAD_STRUCTURE));
        CHECK(catalog == (void *)&canary_byte && live == bootstrap);
    }
    XrXirLibraryInput duplicate[2] = {inputs[1],inputs[1]};
    CHECK(xr_xir_compile_library_catalog_new(&context,duplicate,2,&catalog) == XR_XIR_BAD_STRUCTURE);
    CHECK(catalog == (void *)&canary_byte && live == bootstrap);
    CHECK(xr_xir_compile_library_catalog_new(&context,inputs,2,&catalog) == XR_XIR_OK);
    size_t count; const XrModuleResourceBinding *binding = xr_xir_compile_library_catalog_resources(catalog,&count);
    CHECK(count == 2);
    xr_compile_resources_release(owner);
    memset(packets,0,sizeof(packets));
    CHECK(!strcmp(binding[1].canonical,canonical[1]));
    CHECK(xr_xir_compile_artifact_module(binding[1].checked)->functions[1].instructions[0].immediate == 42);
    xr_xir_compile_library_catalog_free(catalog); CHECK(!live && !live_count);
}
static void identity_grammar(void) {
    reset(SIZE_MAX); XrCompileResources *owner = NULL;
    CHECK(xr_compile_resources_new(&unlimited,&owner) == XR_COMPILE_RESOURCE_OK);
    const XrModuleIdentityAuthority authorities[] = {
        {XR_MODULE_IDENTITY_PROJECT,"project","C:/root"},
        {XR_MODULE_IDENTITY_PACKAGE,"owner/pkg@1.0+build","C:/root"},
        {XR_MODULE_IDENTITY_MEMORY,"scratch",NULL},
        {XR_MODULE_IDENTITY_SCRIPT,NULL,"C:/root"}
    };
    const char *expected[] = {
        "module-id-v1:kind=7:project:namespace=7:project:path=6:lib.xr",
        "module-id-v1:kind=7:package:namespace=19:owner/pkg@1.0+build:path=6:lib.xr",
        "memory-module-v1:id=7:scratch",
        "module-id-v1:kind=6:script:namespace=0::path=6:lib.xr"
    };
    for (unsigned i = 0; i < 4; ++i) {
        char *identity = &canary_byte; XrModuleIdentityKind kind;
        CHECK(xr_compile_module_identity_from_logical(owner,&authorities[i],i == 2 ? "" : "lib.xr",&identity) == XR_MODULE_OK);
        CHECK(!strcmp(identity,expected[i])); CHECK(xr_module_identity_valid(identity,&kind) && kind == authorities[i].kind);
        xr_compile_resources_free(identity);
    }
    const char *bad[] = {"x/","/x","x//y","../x","x/./y","x\\y",""};
    for (size_t i = 0; i < sizeof(bad)/sizeof(bad[0]); ++i) {
        char *identity = &canary_byte;
        CHECK(xr_compile_module_identity_from_logical(owner,&authorities[0],bad[i],&identity) == XR_MODULE_INVALID);
        CHECK(identity == &canary_byte);
    }
    CHECK(!xr_module_identity_valid("stdlib-module-v1:module=2:io:path=3:io/",NULL));
    char *identity = &canary_byte, *logical = &canary_byte;
    CHECK(xr_compile_module_identity_from_source(owner,&authorities[0],"C:/root2/file.xr",&identity,&logical) == XR_MODULE_INVALID);
    CHECK(identity == &canary_byte && logical == &canary_byte);
    CHECK(xr_compile_module_identity_from_source(owner,&authorities[0],"C:/root/file.xr",&identity,&identity) == XR_MODULE_INVALID);
    XrModuleIdentityAuthority root_authority = {XR_MODULE_IDENTITY_PROJECT,"project","/"};
    CHECK(xr_compile_module_identity_from_source(owner,&root_authority,"/lib.xr",&identity,&logical) == XR_MODULE_OK);
    CHECK(!strcmp(identity,expected[0]) && !strcmp(logical,"lib.xr"));
    xr_compile_resources_free(identity); xr_compile_resources_free(logical); logical = &canary_byte;
    const char *roots[] = {"/","C:/","//server/share/","\\\\server\\share\\"};
    const char *sources[] = {"/lib.xr","C:/lib.xr","//server/share/lib.xr","\\\\server\\share\\lib.xr"};
    const char *outside[] = {"/../lib.xr","D:/lib.xr","//server/shared/lib.xr","\\\\server\\shared\\lib.xr"};
    for (unsigned i = 0; i < 4; ++i) {
        root_authority.physical_root = roots[i]; identity = logical = &canary_byte;
        CHECK(xr_compile_module_identity_from_source(owner,&root_authority,sources[i],&identity,&logical) == XR_MODULE_OK);
        CHECK(!strcmp(identity,expected[0]) && !strcmp(logical,"lib.xr"));
        xr_compile_resources_free(identity); xr_compile_resources_free(logical); identity = logical = &canary_byte;
        CHECK(xr_compile_module_identity_from_source(owner,&root_authority,outside[i],&identity,&logical) == XR_MODULE_INVALID);
        CHECK(identity == &canary_byte && logical == &canary_byte);
        CHECK(xr_compile_module_identity_from_source(owner,&root_authority,roots[i],&identity,&logical) == XR_MODULE_INVALID);
        CHECK(identity == &canary_byte && logical == &canary_byte);
    }
    XrModuleIdentityAuthority script = {XR_MODULE_IDENTITY_MEMORY,&canary_byte,&canary_byte};
#ifdef XR_OS_WINDOWS
    CHECK(xr_compile_module_identity_script_authority_from_source(owner,"\xff",&script,&logical) == XR_MODULE_INVALID);
#else
    CHECK(xr_compile_module_identity_script_authority_from_source(owner,"catalog-owner-absent/sub/file.xr",&script,&logical) == XR_MODULE_NOT_FOUND);
#endif
    CHECK(logical == &canary_byte && script.kind == XR_MODULE_IDENTITY_MEMORY);
    xr_compile_resources_release(owner); CHECK(!live && !live_count);
}
static void identity_lifetime_and_views(void) {
    reset(SIZE_MAX); XrCompileResources *owner = NULL;
    CHECK(xr_compile_resources_new(&unlimited,&owner) == XR_COMPILE_RESOURCE_OK);
    char *identity = NULL, *logical = NULL;
    CHECK(xr_compile_module_identity_from_source(owner,&inputs[1].authority,
        "C:/catalog-owner/io/output.xr",&identity,&logical) == XR_MODULE_OK);
    xr_compile_resources_release(owner);
    CHECK(!strcmp(identity,canonical[1]) && !strcmp(logical,"io/output.xr"));
    xr_compile_resources_free(logical);
    const char *name; size_t length;
    CHECK(xr_module_identity_stdlib_namespace(identity,&name,&length) && length == 2 && !memcmp(name,"io",2));
    size_t count = strlen(identity); char truncated[128]; CHECK(count < sizeof(truncated));
    for (size_t n = 0; n < count; ++n) {
        memcpy(truncated,identity,n); truncated[n] = 0;
        CHECK(!xr_module_identity_valid(truncated,NULL));
        CHECK(!xr_module_identity_stdlib_namespace(truncated,&name,&length) && !name && !length);
    }
    CHECK(!xr_module_identity_valid("memory-module-v1:id=18446744073709551616:x",NULL));
    CHECK(!xr_module_identity_valid("memory-module-v1:id=18446744073709551615:x",NULL));
    CHECK(!xr_module_identity_valid("memory-module-v1:id=07:scratch",NULL));
    xr_compile_resources_free(identity); CHECK(!live && !live_count);
}
static void independent_work(void) {
    /* Each namespace/path byte is charged by the shared grammar, length scans,
     * formatting, and actual allocation. These closed-form counts are separate
     * from both the implementation and its measured threshold search. */
    reset(SIZE_MAX); XrCompileResources *owner = NULL;
    CHECK(xr_compile_resources_new(&unlimited,&owner) == XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats before = stats(owner);
    CHECK(identity_run(owner,0) == XR_XIR_OK);
    XrCompileResourceStats after = stats(owner);
    uint64_t ns = 2, path = 12;
    uint64_t authority = 1 + (ns+1) + 1 + (ns+1) + 1;
    uint64_t grammar = (path+1) + 1 + (path+1);
    uint64_t sizing = (ns+1) + (path+1) + sizeof("stdlib-module-v1:module=") + sizeof(":path=") + 1;
    uint64_t formatting = strlen(canonical[1])+1;
    CHECK(after.work-before.work == authority+grammar+sizing+1+formatting);
    xr_compile_resources_release(owner); CHECK(!live && !live_count);
}
int main(void) {
    make_packet(0); make_packet(1); identity_grammar(); independent_work(); identity_lifetime_and_views();
    qualify(catalog_run,0,"script catalog"); qualify(catalog_run,1,"stdlib catalog");
    qualify(identity_run,0,"logical identity"); qualify(identity_run,1,"source identity");
    FILE *source = fopen("catalog-owner-source.xr","wb"); CHECK(source);
    CHECK(fputs("fn answer() -> Int { return 42; }\n",source) >= 0 && !fclose(source));
    qualify(identity_run,2,"physical script authority");
    CHECK(!remove("catalog-owner-source.xr"));
    negative_and_lifetime(); puts("Catalog/identity physical zero and lifetime PASS"); return 0;
}
