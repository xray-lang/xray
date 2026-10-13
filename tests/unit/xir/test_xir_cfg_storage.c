/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_cfg_storage.c - Fresh CFG and SSA proofs in nonzero allocations
 *
 * KEY CONCEPT:
 *   Actual receiving allocations start dirty. Every current graph is rebuilt
 *   without reusing permission or graph facts from a previous function.
 */
#include "xir_construction_fixture.h"
#include "xir/xxir_internal.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1); } } while (0)

typedef struct CfgStorageAllocation { void *pointer; size_t bytes; } CfgStorageAllocation;
typedef struct CfgStorageMark { size_t blocks, bytes; } CfgStorageMark;
static CfgStorageAllocation rp_blocks[8192];
static size_t rp_live, rp_live_bytes, rp_attempts, rp_fail_at=SIZE_MAX;
static bool rp_injected, rp_allocation_poison;
static CfgStorageMark rp_mark(void) { return (CfgStorageMark){rp_live,rp_live_bytes}; }
static void rp_balanced(CfgStorageMark mark) {
    CHECK(rp_live==mark.blocks && rp_live_bytes==mark.bytes);
}
static void *rp_malloc(size_t bytes) {
    if (rp_attempts++==rp_fail_at) { rp_injected=true;return NULL; }
    void *memory=xr_malloc(bytes);
    if (memory) {
        if (rp_allocation_poison) memset(memory,0xA5,bytes);
        CHECK(rp_live<8192 && bytes<=SIZE_MAX-rp_live_bytes);
        rp_blocks[rp_live++]=(CfgStorageAllocation){memory,bytes};rp_live_bytes+=bytes;
    }
    return memory;
}
static void rp_free(void *memory) {
    if (!memory) return;
    size_t at=0;while (at<rp_live && rp_blocks[at].pointer!=memory) ++at;
    CHECK(at<rp_live && rp_blocks[at].bytes<=rp_live_bytes);
    rp_live_bytes-=rp_blocks[at].bytes;rp_blocks[at]=rp_blocks[--rp_live];xr_free(memory);
}
/* Only the physical ledger implementation sees the counted allocator. */
#pragma push_macro("xr_malloc")
#pragma push_macro("xr_free")
#undef xr_malloc
#undef xr_free
#define xr_malloc(bytes) rp_malloc(bytes)
#define xr_free(memory) rp_free(memory)
#include "base/xcompile_resources.c"
#pragma pop_macro("xr_free")
#pragma pop_macro("xr_malloc")

static XrCompileResourceLimits rp_caps(void) {
    return (XrCompileResourceLimits){UINT64_C(8388608),UINT64_C(4194304),UINT64_C(4000000)};
}
static XrXirCompileContext rp_owner(XrCompileResourceLimits caps) {
    XrCompileResourceLimits maximum=rp_caps();
    CHECK(caps.allocated_bytes<=maximum.allocated_bytes && caps.live_bytes<=maximum.live_bytes &&
        caps.work<=maximum.work);
    XrXirCompileContext context={0};
    CHECK(xr_compile_resources_new(&caps,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();context.limits.functions=32;
    context.limits.parameters=16;context.limits.instructions=128;return context;
}
static XrCompileResourceStats rp_stats(const XrXirCompileContext *context) {
    XrCompileResourceStats result={0};
    CHECK(xr_compile_resources_stats(context->resources,&result)==XR_COMPILE_RESOURCE_OK);return result;
}
static void rp_owner_free(XrXirCompileContext *context, uint64_t baseline) {
    CHECK(rp_stats(context).live_bytes==baseline);xr_compile_resources_release(context->resources);
    *context=(XrXirCompileContext){0};
}

typedef enum CfgStorageCase {
    CFG_STORAGE_ROOT, CFG_STORAGE_CHAIN, CFG_STORAGE_JOIN, CFG_STORAGE_LOOP,
    CFG_STORAGE_PANIC, CFG_STORAGE_REUSE, CFG_STORAGE_ROOT_FORWARD,
    CFG_STORAGE_JOIN_BYPASS, CFG_STORAGE_LOOP_ESCAPE, CFG_STORAGE_PANIC_LOCAL,
    CFG_STORAGE_UNREACHABLE, CFG_STORAGE_TARGET, CFG_STORAGE_GEOMETRY
} CfgStorageCase;
typedef struct CfgStorageFixture {
    XrXirInstruction instructions[2][70];
    XrXirBlock blocks[2][66];
    uint32_t operands[4];
    XrXirFunction functions[2];
    XrXirModule module;
} CfgStorageFixture;
typedef struct CfgStorageProbe {
    XrCompileResourceStats before, after;
    size_t calls;
    bool injected;
} CfgStorageProbe;

static void cfg_storage_function(CfgStorageFixture *fixture, uint32_t f,
    uint32_t instructions, uint32_t blocks) {
    fixture->functions[f] = (XrXirFunction){.name="cfg-zero",.name_length=8,
        .result=XR_XIR_I64,.blocks=fixture->blocks[f],.block_count=blocks,
        .instructions=fixture->instructions[f],.instruction_count=instructions};
}
static void cfg_storage_chain(CfgStorageFixture *fixture, uint32_t f) {
    XrXirInstruction *ops=fixture->instructions[f];
    XrXirBlock *blocks=fixture->blocks[f];
    ops[0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=1};
    ops[1]=(XrXirInstruction){.op=XR_XIR_JUMP,.targets={1}};
    blocks[0]=(XrXirBlock){.count=2};
    for (uint32_t b=1;b<64;++b) {
        blocks[b]=(XrXirBlock){.first=b+1,.count=1};
        ops[b+1]=(XrXirInstruction){.op=XR_XIR_JUMP,.targets={b+1}};
    }
    blocks[64]=(XrXirBlock){.first=65,.count=2};
    ops[65]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=2};
    ops[66]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={65}};
    cfg_storage_function(fixture,f,67,65);
}
static void cfg_storage_join(CfgStorageFixture *fixture, uint32_t f) {
    XrXirInstruction *ops=fixture->instructions[f];
    XrXirBlock *blocks=fixture->blocks[f];
    ops[0]=(XrXirInstruction){.op=XR_XIR_CONST_BOOL,.type=XR_XIR_BOOL,.immediate=1};
    ops[1]=(XrXirInstruction){.op=XR_XIR_BRANCH,.args={0},.targets={1,2}};
    ops[2]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=11};
    ops[3]=(XrXirInstruction){.op=XR_XIR_JUMP,.targets={3}};
    ops[4]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=22};
    ops[5]=(XrXirInstruction){.op=XR_XIR_JUMP,.targets={3}};
    ops[6]=(XrXirInstruction){.op=XR_XIR_PHI,.type=XR_XIR_I64,.args={0,4}};
    ops[7]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={6}};
    for (uint32_t b=0;b<4;++b) blocks[b]=(XrXirBlock){.first=2*b,.count=2};
    fixture->operands[0]=1;fixture->operands[1]=2;
    fixture->operands[2]=2;fixture->operands[3]=4;
    cfg_storage_function(fixture,f,8,4);
    fixture->functions[f].operands=fixture->operands;
    fixture->functions[f].operand_count=4;
}
static void cfg_storage_loop(CfgStorageFixture *fixture) {
    XrXirInstruction *ops=fixture->instructions[0];
    XrXirBlock *blocks=fixture->blocks[0];
    ops[0]=(XrXirInstruction){.op=XR_XIR_CONST_BOOL,.type=XR_XIR_BOOL,.immediate=1};
    ops[1]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=1};
    ops[2]=(XrXirInstruction){.op=XR_XIR_JUMP,.targets={1}};
    ops[3]=(XrXirInstruction){.op=XR_XIR_BRANCH,.args={0},.targets={2,3}};
    ops[4]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=2};
    ops[5]=(XrXirInstruction){.op=XR_XIR_JUMP,.targets={1}};
    ops[6]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={1}};
    blocks[0]=(XrXirBlock){.count=3};blocks[1]=(XrXirBlock){.first=3,.count=1};
    blocks[2]=(XrXirBlock){.first=4,.count=2};blocks[3]=(XrXirBlock){.first=6,.count=1};
    cfg_storage_function(fixture,0,7,4);
}
static void cfg_storage_panic(CfgStorageFixture *fixture) {
    XrXirInstruction *ops=fixture->instructions[0];
    XrXirBlock *blocks=fixture->blocks[0];
    ops[0]=(XrXirInstruction){.op=XR_XIR_CONST_BOOL,.type=XR_XIR_BOOL,.immediate=1};
    ops[1]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=1};
    ops[2]=(XrXirInstruction){.op=XR_XIR_JUMP,.targets={1}};
    ops[3]=(XrXirInstruction){.op=XR_XIR_COPY,.type=XR_XIR_I64,.args={1}};
    ops[4]=(XrXirInstruction){.op=XR_XIR_BRANCH,.args={0},.targets={3,4}};
    ops[5]=(XrXirInstruction){.op=XR_XIR_PANIC_CATCH};
    ops[6]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={1}};
    ops[7]=ops[6];ops[8]=ops[6];
    blocks[0]=(XrXirBlock){.count=3};blocks[1]=(XrXirBlock){.first=3,.count=2,.panic=2};
    blocks[2]=(XrXirBlock){.first=5,.count=2};blocks[3]=(XrXirBlock){.first=7,.count=1};
    blocks[4]=(XrXirBlock){.first=8,.count=1};
    cfg_storage_function(fixture,0,9,5);
}
static void cfg_storage_fixture(CfgStorageFixture *fixture, CfgStorageCase mode) {
    memset(fixture,0,sizeof(*fixture));
    /* A declaration-free leaf is a Program input; a Library needs its module table. */
    fixture->module=(XrXirModule){.stage=XR_XIR_BUILT,.functions=fixture->functions,
        .function_count=1,.linkage_kind=XR_XIR_PROGRAM};
    if (mode==CFG_STORAGE_CHAIN || mode==CFG_STORAGE_ROOT_FORWARD ||
        mode==CFG_STORAGE_UNREACHABLE || mode==CFG_STORAGE_REUSE) cfg_storage_chain(fixture,0);
    else if (mode==CFG_STORAGE_JOIN || mode==CFG_STORAGE_JOIN_BYPASS ||
        mode==CFG_STORAGE_TARGET || mode==CFG_STORAGE_GEOMETRY) cfg_storage_join(fixture,0);
    else if (mode==CFG_STORAGE_LOOP || mode==CFG_STORAGE_LOOP_ESCAPE) cfg_storage_loop(fixture);
    else if (mode==CFG_STORAGE_PANIC || mode==CFG_STORAGE_PANIC_LOCAL) cfg_storage_panic(fixture);
    else {
        fixture->instructions[0][0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64};
        fixture->instructions[0][1]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={0}};
        fixture->blocks[0][0]=(XrXirBlock){.count=2};cfg_storage_function(fixture,0,2,1);
    }
    if (mode==CFG_STORAGE_REUSE) {
        cfg_storage_join(fixture,1);fixture->module.function_count=2;
    }
    if (mode==CFG_STORAGE_ROOT_FORWARD)
        fixture->instructions[0][0]=(XrXirInstruction){.op=XR_XIR_COPY,.type=XR_XIR_I64,.args={65}};
    if (mode==CFG_STORAGE_JOIN_BYPASS) fixture->instructions[0][7].args[0]=2;
    if (mode==CFG_STORAGE_LOOP_ESCAPE) fixture->instructions[0][6].args[0]=4;
    if (mode==CFG_STORAGE_PANIC_LOCAL) fixture->instructions[0][6].args[0]=3;
    if (mode==CFG_STORAGE_UNREACHABLE) fixture->instructions[0][1].targets[0]=2;
    if (mode==CFG_STORAGE_TARGET) fixture->instructions[0][1].targets[0]=4;
    if (mode==CFG_STORAGE_GEOMETRY) ++fixture->blocks[0][2].first;
}
static XrXirStatus cfg_storage_probe(CfgStorageCase mode, XrCompileResourceLimits caps,
    size_t failure, bool poison, CfgStorageProbe *output) {
    CfgStorageMark mark=rp_mark();XrXirCompileContext context=rp_owner(caps);
    CfgStorageFixture fixture;cfg_storage_fixture(&fixture,mode);
    output->before=rp_stats(&context);size_t first=rp_attempts;
    CHECK(!rp_allocation_poison && rp_fail_at==SIZE_MAX);
    CHECK(failure==SIZE_MAX || failure<SIZE_MAX-first);
    rp_allocation_poison=poison;rp_injected=false;
    rp_fail_at=failure==SIZE_MAX?SIZE_MAX:first+failure;
    XrXirStatus status=xir_fixture_verify(&context,&fixture.module,NULL);
    output->calls=rp_attempts-first;output->injected=rp_injected;
    rp_fail_at=SIZE_MAX;rp_allocation_poison=false;rp_injected=false;
    output->after=rp_stats(&context);CHECK(output->after.live_bytes==output->before.live_bytes);
    rp_owner_free(&context,output->before.live_bytes);rp_balanced(mark);return status;
}
static void cfg_storage_owned_case(void) {
    CfgStorageMark mark=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&context).live_bytes;
    CfgStorageFixture fixture;cfg_storage_fixture(&fixture,CFG_STORAGE_REUSE);
    CHECK(!rp_allocation_poison);rp_allocation_poison=true;
    XrXirArtifact *artifact=NULL;
    CHECK(xir_fixture_check(&context,&fixture.module,&artifact,NULL)==XR_XIR_OK && artifact);
    memset(&fixture,0xCC,sizeof(fixture));
    CHECK(xr_xir_compile_artifact_verify(artifact,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(artifact);rp_allocation_poison=false;
    rp_owner_free(&context,baseline);rp_balanced(mark);
}
static void cfg_storage_initialization_cases(void) {
    CfgStorageProbe plain, poisoned;
    for (uint32_t mode=CFG_STORAGE_ROOT;mode<=CFG_STORAGE_GEOMETRY;++mode) {
        XrXirStatus expected=mode<=CFG_STORAGE_REUSE?XR_XIR_OK:
            mode<=CFG_STORAGE_PANIC_LOCAL?XR_XIR_BAD_DOMINANCE:XR_XIR_BAD_STRUCTURE;
        XrXirStatus status=cfg_storage_probe((CfgStorageCase)mode,rp_caps(),SIZE_MAX,true,&poisoned);
        if (status!=expected) fprintf(stderr,"CFG dirty storage mode%u status%u expected%u\n",mode,status,expected);
        CHECK(status==expected && !poisoned.injected);
        CHECK(cfg_storage_probe((CfgStorageCase)mode,rp_caps(),SIZE_MAX,false,&plain)==expected);
        CHECK(plain.after.work==poisoned.after.work && plain.after.allocated_bytes==poisoned.after.allocated_bytes &&
            plain.after.peak_bytes==poisoned.after.peak_bytes && plain.calls==poisoned.calls);
    }
    CHECK(cfg_storage_probe(CFG_STORAGE_REUSE,rp_caps(),SIZE_MAX,true,&plain)==XR_XIR_OK);
    for (size_t fail=0;fail<plain.calls;++fail) {
        CHECK(cfg_storage_probe(CFG_STORAGE_REUSE,rp_caps(),fail,true,&poisoned)==XR_XIR_OUT_OF_MEMORY);
        CHECK(poisoned.injected);
    }
    for (uint32_t axis=0;axis<3;++axis) {
        XrCompileResourceLimits exact=rp_caps();
        if (axis==0) exact.allocated_bytes=plain.after.allocated_bytes;
        if (axis==1) exact.live_bytes=plain.after.peak_bytes;
        if (axis==2) exact.work=plain.after.work;
        CHECK(cfg_storage_probe(CFG_STORAGE_REUSE,exact,SIZE_MAX,true,&poisoned)==XR_XIR_OK);
        if (axis==0) --exact.allocated_bytes;
        if (axis==1) --exact.live_bytes;
        if (axis==2) --exact.work;
        CHECK(cfg_storage_probe(CFG_STORAGE_REUSE,exact,SIZE_MAX,true,&poisoned)==XR_XIR_BUDGET);
    }
    cfg_storage_owned_case();
    puts("CFG dirty allocation: root/65-block/join/loop/panic/reuse/SSA rejection; FI and three exact/minus1 caps physical0");
}
int main(void) {
    cfg_storage_initialization_cases();
    CHECK(!rp_live && !rp_live_bytes);return 0;
}
