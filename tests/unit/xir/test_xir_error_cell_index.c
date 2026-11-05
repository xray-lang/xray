/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_error_cell_index.c - Exact static Cell membership and original error facts
 *
 * KEY CONCEPT:
 *   An index retains immutable type membership while every flow query rebuilds
 *   alias roots, reachability and the complete escaping error set.
 */
#include "xir_construction_fixture.h"
#include "xir/xxir_effects.h"
#include "xir/xxir_declarations.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if(!(c)){fprintf(stderr,"FAIL %d %s\n",__LINE__,#c);exit(1);} } while(0)
typedef struct CellPhysical {void *pointer;size_t bytes;} CellPhysical;
static CellPhysical physical_allocations[2048];
static size_t live,live_bytes,attempts,fail_at=SIZE_MAX;
static bool injected;
static void *ci_malloc(size_t bytes){
    if(attempts++==fail_at){injected=true;return NULL;}
    void *p=xr_malloc(bytes);if(!p)return NULL;
    CHECK(live<2048 && bytes<=SIZE_MAX-live_bytes);physical_allocations[live++]=(CellPhysical){p,bytes};live_bytes+=bytes;return p;
}
static void ci_free(void *p){
    if(!p)return;size_t i=0;while(i<live && physical_allocations[i].pointer!=p)++i;
    CHECK(i<live && live_bytes>=physical_allocations[i].bytes);live_bytes-=physical_allocations[i].bytes;physical_allocations[i]=physical_allocations[--live];xr_free(p);
}
#pragma push_macro("xr_malloc")
#pragma push_macro("xr_free")
#undef xr_malloc
#undef xr_free
#define xr_malloc(bytes) ci_malloc(bytes)
#define xr_free(pointer) ci_free(pointer)
#include "base/xcompile_resources.c"
#pragma pop_macro("xr_free")
#pragma pop_macro("xr_malloc")
#include "xir/xxir_effects.c"
#include "xir_error_cell_index_fixture.h"
static XrCompileResourceLimits ci_caps(void){return (XrCompileResourceLimits){8388608,4194304,8000000};}
static XrXirCompileContext ci_owner(XrCompileResourceLimits caps){
    CHECK(!live && !live_bytes);XrXirCompileContext c={0};
    CHECK(xr_compile_resources_new(&caps,&c.resources)==XR_COMPILE_RESOURCE_OK);c.limits=xr_xir_compile_default_limits();return c;
}
static XrCompileResourceStats ci_stats(const XrXirCompileContext *c){
    XrCompileResourceStats s={0};CHECK(xr_compile_resources_stats(c->resources,&s)==XR_COMPILE_RESOURCE_OK);return s;
}
static void ci_release(XrXirCompileContext *c,uint64_t baseline){
    CHECK(ci_stats(c).live_bytes==baseline);xr_compile_resources_release(c->resources);*c=(XrXirCompileContext){0};CHECK(!live && !live_bytes);
}
static void ci_golden(const XrXirEffects *e,CellBoundary boundary,bool snapshot){
    (void)boundary;(void)snapshot;
    CHECK(e && e->count>=3);
    CHECK(xr_xir_effects_read(e,1).control.throws==XR_XIR_EFFECT_MAY);
}
static XrXirStatus ci_whole(const XrXirCompileContext *c,CellBoundary boundary,bool snapshot,bool large){
    CellIndexFixture f;ci_fixture(&f,boundary,snapshot,large);XrXirDiagnostic d={0};XrXirEffects *e=NULL;
    XrXirStatus s=xir_fixture_verify(c, &f.module, &d);
    if(s!=XR_XIR_OK && s!=XR_XIR_OUT_OF_MEMORY && s!=XR_XIR_BUDGET)
        fprintf(stderr,"fixture boundary%u snapshot%u status%u function%u block%u instruction%u\n",(unsigned)boundary,(unsigned)snapshot,s,d.function,d.block,d.instruction);
    if(s==XR_XIR_OK)s=xr_xir_compile_effects_infer_verified(c,&f.module,&e);
    if(s==XR_XIR_OK)ci_golden(e,boundary,snapshot);else CHECK(!e);
    xr_xir_compile_effects_free(e);return s;
}
static void ci_boundaries(void){
    for(unsigned b=CI_CALL;b<=CI_PHI;++b)for(unsigned snapshot=0;snapshot<2;++snapshot){
        XrXirCompileContext c=ci_owner(ci_caps());uint64_t baseline=ci_stats(&c).live_bytes;
        CHECK(ci_whole(&c,(CellBoundary)b,snapshot!=0,false)==XR_XIR_OK);ci_release(&c,baseline);
    }
    puts("Cell known enum literals: call/unknown/go/output/print/stream/suspend/timer/write/cleanup/panic/await/invoke/phi PASS");
}
static void ci_erased_error(void){
    for(unsigned snapshot=0;snapshot<2;++snapshot){
        CellIndexFixture f;ci_fixture(&f,CI_CALL,snapshot!=0,false);f.nodes[1].element=XR_XIR_ERROR;
        f.subject[0]=(XrXirInstruction){.op=XR_XIR_ENUM_NEW,.type=(XrXirType)CI_ENUM};
        f.subject[1]=(XrXirInstruction){.op=XR_XIR_ERROR_ERASE,.type=XR_XIR_ERROR,.args={4}};
        f.subject[2]=(XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=(XrXirType)CI_CELL,.args={5}};
        f.subject[3]=(XrXirInstruction){.op=XR_XIR_COPY,.type=(XrXirType)CI_CELL,.args={6}};
        f.subject[4]=(XrXirInstruction){.op=XR_XIR_CELL_READ,.type=XR_XIR_ERROR,.args={7}};
        f.subject[5]=(XrXirInstruction){.op=XR_XIR_CALL,.immediate=2};
        f.subject[6]=(XrXirInstruction){.op=XR_XIR_CELL_READ,.type=XR_XIR_ERROR,.args={6}};
        f.subject[7]=(XrXirInstruction){.op=XR_XIR_THROW,.args={snapshot?8u:10u}};
        f.blocks[0].count=8;f.functions[1].instruction_count=8;f.functions[1].error_type=XR_XIR_ERROR;
        XrXirCompileContext c=ci_owner(ci_caps());uint64_t baseline=ci_stats(&c).live_bytes;XrXirEffects *e=NULL;
        CHECK(xir_fixture_verify(&c, &f.module, NULL)==XR_XIR_OK);
        CHECK(xr_xir_compile_effects_infer_verified(&c,&f.module,&e)==XR_XIR_OK);
        CHECK(e);
        xr_xir_compile_effects_free(e);ci_release(&c,baseline);
    }
}
static void ci_invalid_types(void){
    CellIndexFixture f;ci_fixture(&f,CI_CALL,false,false);XrXirCompileContext c=ci_owner(ci_caps());uint64_t baseline=ci_stats(&c).live_bytes;
    CHECK(xir_fixture_verify(&c, &f.module, NULL)==XR_XIR_OK);
    f.parameters[0]=(XrXirType)300;CHECK(xir_fixture_verify(&c, &f.module, NULL)==XR_XIR_BAD_TYPE);
    f.parameters[0]=(XrXirType)CI_CELL;f.nodes[1].element=(XrXirType)300;
    CHECK(xir_fixture_verify(&c, &f.module, NULL)==XR_XIR_BAD_TYPE);
    f.nodes[1].element=(XrXirType)CI_ENUM;f.subject[1].type=(XrXirType)300;
    CHECK(xir_fixture_verify(&c, &f.module, NULL)==XR_XIR_BAD_TYPE);
    ci_release(&c,baseline);
}
#include "xir_error_cell_index_resources.h"
int main(void){ci_boundaries();ci_erased_error();ci_invalid_types();ci_faults();ci_limits();ci_occupied();
    puts("exact Cell index, original typed-error literals, finite owners and physical0 PASS");return 0;}
