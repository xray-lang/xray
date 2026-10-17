/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_derivation_snapshot_match.c - Complete declaration snapshot fields
 *
 * KEY CONCEPT:
 *   Literal metadata correspondence grants no module or execution permission.
 */
#include "xir/xxir_internal.h"
#include "xir/xxir_derivation_internal.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);}}while(0)
typedef struct SnapshotAllocation {void *pointer;size_t bytes;} SnapshotAllocation;
static SnapshotAllocation snapshot_allocations[128];
static size_t snapshot_live,snapshot_bytes;
static size_t snapshot_attempts,snapshot_fail_at=SIZE_MAX;
static bool snapshot_injected;
static void *snapshot_malloc(size_t bytes) {
    if(snapshot_attempts++==snapshot_fail_at){snapshot_injected=true;return NULL;}
    void *memory=xr_malloc(bytes);if(!memory)return NULL;
    CHECK(snapshot_live<128 && bytes<=SIZE_MAX-snapshot_bytes);
    snapshot_allocations[snapshot_live++]=(SnapshotAllocation){memory,bytes};
    snapshot_bytes+=bytes;return memory;
}
static void snapshot_free(void *memory) {
    if(memory) {
        size_t n=0;while(n<snapshot_live && snapshot_allocations[n].pointer!=memory)++n;
        CHECK(n<snapshot_live && snapshot_allocations[n].bytes<=snapshot_bytes);
        snapshot_bytes-=snapshot_allocations[n].bytes;
        snapshot_allocations[n]=snapshot_allocations[--snapshot_live];
    }
    xr_free(memory);
}
#undef xr_malloc
#undef xr_free
#define xr_malloc(bytes) snapshot_malloc(bytes)
#define xr_free(memory) snapshot_free(memory)
#include "base/xcompile_resources.c"
#include "xir/xxir_effects.c"
#include "xir_derivation_snapshot_literal_cases.h"
#include "xir_derivation_snapshot_copy_cases.h"

int main(void) {
    const XrCompileResourceLimits caps={8388608,4194304,4000000};
    XrXirCompileContext context={0};
    CHECK(xr_compile_resources_new(&caps,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();
    XrXirDerivationMember members[2]={{0,XR_XIR_DERIVE_READ},{0,XR_XIR_DERIVE_WRITE}};
    XrXirDerivationPolicy policies[2];
    for(unsigned n=0;n<2;++n)policies[n]=(XrXirDerivationPolicy){.target=XR_XIR_I64,
        .members=&members[n],.member_count=1,
        .origin={.module={"snapshot",8},.file={"field.xr",8},.field_path={"secret",6},.line=7,.column=3}};
    XrXirDerivationTable tables[2]={{.policies=&policies[0],.policy_count=1},
        {.policies=&policies[1],.policy_count=1}};
    for(unsigned n=0;n<2;++n)CHECK(xir_derivations_structure_verify(&context,&tables[n])==XR_XIR_OK);
    XrXirDeclarations declarations[2]={{.derivations=&tables[0]},{.derivations=&tables[1]}};
    XrCompileResourceStats before={0},after={0};
    CHECK(xr_compile_resources_stats(context.resources,&before)==XR_COMPILE_RESOURCE_OK);
    XrXirStatus strict=effect_body_declarations_match(&context,&declarations[0],&declarations[1],0,NULL);
    bool same=true;
    XrXirStatus changed=effect_body_declarations_match(&context,&declarations[0],&declarations[1],0,&same);
    CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK);
    bool complete=strict==XR_XIR_BAD_STRUCTURE && changed==XR_XIR_OK && !same &&
        after.live_bytes==before.live_bytes && after.work>before.work;
    fprintf(stderr,"Derivation snapshot READ/WRITE literals strict%u expected%u changed%u same%u work%llu/%llu\n",
        (unsigned)strict,(unsigned)XR_XIR_BAD_STRUCTURE,(unsigned)changed,(unsigned)same,
        (unsigned long long)after.work,(unsigned long long)before.work);
    if(complete){snapshot_literal_matrix(&context);snapshot_work_bounds();snapshot_copy_controls();}
    xr_compile_resources_release(context.resources);
    CHECK(!snapshot_live && !snapshot_bytes);CHECK(complete);return 0;
}
