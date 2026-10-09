/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_error_cfg_pending.c - Process only changed CFG input rows
 *
 * KEY CONCEPT:
 *   Changed incoming rows reschedule a block, including self and late edges.
 *   Complete admission, literal facts, and real resources remain mandatory.
 */
#include "xir_construction_fixture.h"
#include "xir/xxir_effects.h"
#include "xir/xxir_declarations.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"FAIL %d %s\n",__LINE__,#c);exit(1); } } while (0)
typedef struct EdgePhysical { void *pointer;size_t bytes; } EdgePhysical;
static EdgePhysical pc_physical[2048];
static size_t live,live_bytes,attempts,fail_at=SIZE_MAX;
static bool injected;
static void *pc_malloc(size_t bytes) {
    if (attempts++==fail_at) { injected=true;return NULL; }
    void *p=xr_malloc(bytes);if (!p) return NULL;
    CHECK(live<2048 && bytes<=SIZE_MAX-live_bytes);
    pc_physical[live++]=(EdgePhysical){p,bytes};live_bytes+=bytes;return p;
}
static void pc_free(void *p) {
    if (!p) return;
    size_t i=0;while (i<live && pc_physical[i].pointer!=p) ++i;
    CHECK(i<live && live_bytes>=pc_physical[i].bytes);
    live_bytes-=pc_physical[i].bytes;pc_physical[i]=pc_physical[--live];xr_free(p);
}
#pragma push_macro("xr_malloc")
#pragma push_macro("xr_free")
#undef xr_malloc
#undef xr_free
#define xr_malloc(bytes) pc_malloc(bytes)
#define xr_free(pointer) pc_free(pointer)
#include "base/xcompile_resources.c"
#pragma pop_macro("xr_free")
#pragma pop_macro("xr_malloc")
#include "xir/xxir_effects.c"
#include "xir_error_cfg_pending_fixture.h"
static XrCompileResourceLimits pc_caps(void) { return (XrCompileResourceLimits){8388608,4194304,8000000}; }
static XrXirCompileContext pc_owner(XrCompileResourceLimits caps) {
    CHECK(!live && !live_bytes);XrXirCompileContext c={0};
    CHECK(xr_compile_resources_new(&caps,&c.resources)==XR_COMPILE_RESOURCE_OK);
    c.limits=xr_xir_compile_default_limits();return c;
}
static XrCompileResourceStats pc_stats(const XrXirCompileContext *c) {
    XrCompileResourceStats s={0};CHECK(xr_compile_resources_stats(c->resources,&s)==XR_COMPILE_RESOURCE_OK);return s;
}
static void pc_release(XrXirCompileContext *c, uint64_t baseline) {
    CHECK(pc_stats(c).live_bytes==baseline);xr_compile_resources_release(c->resources);
    *c=(XrXirCompileContext){0};CHECK(!live && !live_bytes);
}
/* A lexical Cell formal stays conditional. Each authenticated REGISTER
 * supplies its actual owner; this fixture supplies an owned local Cell. */
static void pc_lexical_cleanup_root(const XrXirEffects *e) {
    CHECK(e && e->count==4 && e->contracts && e->cells);
    CHECK(!e->root[3].requires_root && e->root[3].unresolved);
    const XrXirRootFormula *child=&e->contracts[3].formula;
    CHECK(!child->constant_mask && child->term_count==1 && child->terms);
    CHECK(child->terms[0].kind==XR_XIR_ROOT_TERM_CELL_PARAMETER && child->terms[0].index==0);
    CHECK(e->contracts[3].parameter_count==1);
    CHECK(xr_xir_cell_provenance_role(e->cells,3,0)==XR_XIR_CELL_PROOF_LEXICAL_CLEANUP);
    CHECK(xr_xir_cell_provenance_origin(e->cells,3,0)==XR_XIR_CELL_ORIGIN_SCOPED);
    CHECK(e->root_witnesses[3].cause==XR_XIR_ROOT_CAUSE_NONE);
    CHECK(e->unresolved_witnesses[3].cause==XR_XIR_ROOT_CAUSE_CELL_PARAMETER &&
        e->unresolved_witnesses[3].callee==UINT32_MAX &&
        e->unresolved_witnesses[3].slot==0 && !e->unresolved_witnesses[3].distance);
    CHECK(!e->root[1].requires_root && !e->root[1].unresolved);
    CHECK(!e->contracts[1].formula.constant_mask && !e->contracts[1].formula.term_count);
}
static void pc_golden(const XrXirEffects *e,PendingCase mode,bool snapshot) {
    uint64_t bits=pc_expected(mode,snapshot);
    CHECK(e && e->count==(mode==PC_CLEANUP || mode==PC_PANIC ? 4u : 3u));
    CHECK(e->atom_count==2 && e->words==1 && e->errors[1]==bits);
    CHECK(!xr_xir_effects_error_unknown(e,1) && !xr_xir_effects_error_unidentified(e,1));
    CHECK(xr_xir_effects_error(e,1,(XrXirType)EE_ENUM,0)==((bits&4u)!=0));
    CHECK(xr_xir_effects_error(e,1,(XrXirType)EE_ENUM,1)==((bits&8u)!=0));
    CHECK(e->functions[1].throws==XR_XIR_EFFECT_MAY);
    CHECK(!e->errors[0] && e->errors[2]==(mode==PC_TEMPORARY ? 0u : 8u));
    if (e->count==4) CHECK(!e->errors[3]);
    for (uint32_t f=0;f<e->count;++f) {
        CHECK(e->task_errors[f]==XR_XIR_OK);
        if (e->count==4 && f==3) pc_lexical_cleanup_root(e);
        else CHECK(e->root[f].requires_root==(f==0) && !e->root[f].unresolved);
        CHECK(e->functions[f].suspend==XR_XIR_EFFECT_NONE);
    }
}
static XrXirStatus pc_whole(const XrXirCompileContext *c,PendingCase mode,bool snapshot) {
    EdgeFixture f;pc_fixture(&f,mode,snapshot);f.module.stage=XR_XIR_BUILT;
    XrXirArtifact *checked=NULL;XrXirEffects *e=NULL;XrXirDiagnostic d={0};
    XrXirStatus status=xir_fixture_check(c, &f.module, &checked, &d);
    if (status==XR_XIR_OK) {
        CHECK(checked && xr_xir_compile_artifact_module(checked)->stage==XR_XIR_CHECKED);
        status=xr_xir_compile_effects_analyze(checked,&e);
    } else CHECK(!checked);
    if (status==XR_XIR_OK) pc_golden(e,mode,snapshot);else CHECK(!e);
    if (status!=XR_XIR_OK && status!=XR_XIR_BUDGET && status!=XR_XIR_OUT_OF_MEMORY)
        fprintf(stderr,"pending mode%u snapshot%u status%u f%u b%u i%u\n",
            (unsigned)mode,(unsigned)snapshot,(unsigned)status,d.function,d.block,d.instruction);
    xr_xir_compile_effects_free(e);xr_xir_compile_artifact_free(checked);return status;
}
static void pc_cases(void) {
    for (unsigned mode=PC_FORWARD;mode<=PC_TEMPORARY;++mode) for (unsigned snapshot=0;snapshot<2;++snapshot) {
        XrXirCompileContext c=pc_owner(pc_caps());uint64_t baseline=pc_stats(&c).live_bytes;
        CHECK(pc_whole(&c,(PendingCase)mode,snapshot!=0)==XR_XIR_OK);pc_release(&c,baseline);
    }
    puts("pending literals: reverse CFG/self PHI/backedge/late join/filter/invoke/cleanup/panic/Cell snapshots");
}
static void pc_private_begin(const XrXirCompileContext *c,EdgeFixture *f,
    XrXirEffects *e,EffectTerms *terms,ErrorFlow *flow) {
    CHECK(xir_fixture_verify(c, &f->module, NULL)==XR_XIR_OK);
    *e=(XrXirEffects){0};e->count=f->module.function_count;
    XrXirCompileContext copy=*c;
    CHECK(effect_errors_seed(&f->module,e,&copy)==XR_XIR_OK);
    *terms=(EffectTerms){0};terms->remaining=flow->remaining;terms->types=f->types;
    flow->module=&f->module;flow->effects=e;flow->terms=terms;
}
static void pc_private_free(ErrorFlow *flow,EffectTerms *terms,XrXirEffects *e) {
    error_storage_free(flow);effect_terms_free(terms);
    xr_compile_resources_free(e->errors);xr_compile_resources_free(e->atoms);
    CHECK(!flow->storage && !flow->roots && !flow->cells && !flow->reachable);
    CHECK(!flow->slots && !flow->active && !flow->zero && !flow->active_count);
}
/* Controlled rows are private freshness probes, never permission witnesses. */
static void pc_pattern(const void *memory,size_t bytes,unsigned char pattern) {
    const unsigned char *p=memory;
    for (size_t i=0;i<bytes;++i) CHECK(p[i]==pattern);
}
static void pc_plain_row(const ErrorFlow *flow,const uint64_t *row) {
    CHECK(flow->values==545 && flow->active_count==1 && flow->stride==1 && flow->effects->words==1);
    for (uint32_t v=0;v<545;++v) CHECK(error_read(flow,row,v)[0]==(v==512 ? UINT64_C(4) : 0));
}
static void pc_edge_controls(void) {
    static const PendingCase modes[]={PC_REVERSE,PC_FILTER,PC_PHI_SELF,PC_PANIC};
    for (unsigned probe=0;probe<4;++probe) {
        EdgeFixture f;pc_fixture(&f,modes[probe],false);
        XrXirCompileContext c=pc_owner(pc_caps());uint64_t baseline=pc_stats(&c).live_bytes;
        XrXirEffects e={0};EffectTerms terms={0};ErrorFlow flow={0};flow.remaining=&c;
        pc_private_begin(&c,&f,&e,&terms,&flow);
        CHECK(error_function(&flow,1)==XR_XIR_OK && e.errors[1]==pc_expected(modes[probe],false));
        size_t bytes=flow.stride*sizeof(uint64_t);
        memset(flow.work,0,bytes);memset(flow.edge,0xa5,bytes);memset(flow.snapshot,0x5a,bytes);
        uint32_t from=probe==0 ? 31u : probe==1 ? 0u : probe==2 ? 1u : 3u;
        uint32_t to=probe==0 ? 30u : probe==1 ? f.subject[7].targets[0] : probe==2 ? 1u : 2u;
        const XrXirInstruction *branch=probe==1 ? &f.subject[7] : NULL;
        uint64_t *dest=flow.states+(size_t)to*flow.stride;
        memset(dest,0,bytes);flow.reachable[to]=0;flow.changed=false;
        if (probe==0) {
            CHECK(e.atom_count==2 && !f.blocks[from].panic);
            error_value(&flow,flow.work,512)[0]=4;
        } else if (probe==1) {
            CHECK(branch->op==XR_XIR_BRANCH && to==2);
            error_value(&flow,flow.work,2)[0]=12;
        } else if (probe==2) {
            CHECK(f.subject[f.blocks[to].first].op==XR_XIR_PHI);
            error_value(&flow,flow.work,5)[0]=4;error_value(&flow,flow.work,6)[0]=8;
        } else {
            CHECK(f.blocks[from].panic==to && f.blocks[from].frontier!=f.blocks[to].frontier);
            error_value(&flow,flow.work,3)[0]=4;error_value(&flow,flow.work,4)[0]=4;
            error_value(&flow,flow.work,5)[0]=4;
        }
        XrCompileResourceStats before=pc_stats(&c);size_t allocations=attempts;
        CHECK(error_edge(&flow,from,to,branch,true)==XR_XIR_OK);
        XrCompileResourceStats after=pc_stats(&c);
        CHECK(attempts==allocations && before.allocated_bytes==after.allocated_bytes && before.live_bytes==after.live_bytes);
        CHECK(flow.reachable[to]==3 && flow.changed);
        if (probe==0) {
            pc_plain_row(&flow,dest);pc_plain_row(&flow,flow.work);
            pc_pattern(flow.edge,bytes,0xa5);pc_pattern(flow.snapshot,bytes,0x5a);
            CHECK(after.work-before.work<UINT64_C(4000));
        } else if (probe==1) {
            CHECK(error_value(&flow,flow.edge,2)[0]==4 && error_value(&flow,dest,2)[0]==4);
            CHECK(error_value(&flow,flow.work,2)[0]==12);pc_pattern(flow.snapshot,bytes,0x5a);
        } else if (probe==2) {
            CHECK(error_value(&flow,flow.edge,5)[0]==8 && error_value(&flow,flow.edge,6)[0]==4);
            CHECK(error_value(&flow,flow.snapshot,5)[0]==4 && error_value(&flow,flow.snapshot,6)[0]==8);
            CHECK(error_value(&flow,flow.work,5)[0]==4 && error_value(&flow,flow.work,6)[0]==8);
            CHECK(error_value(&flow,dest,5)[0]==8 && error_value(&flow,dest,6)[0]==4);
        } else {
            CHECK(error_value(&flow,flow.edge,3)[0]==12 && error_value(&flow,flow.edge,4)[0]==12);
            CHECK(error_value(&flow,dest,3)[0]==12 && error_value(&flow,dest,4)[0]==12);
            CHECK(error_value(&flow,flow.edge,5)[0]==4 && error_value(&flow,dest,5)[0]==4);
            CHECK(error_value(&flow,flow.work,3)[0]==4 && error_value(&flow,flow.work,4)[0]==4);
            pc_pattern(flow.snapshot,bytes,0x5a);
        }
        pc_private_free(&flow,&terms,&e);pc_release(&c,baseline);
    }
    puts("strict edge literals: direct scratch poison/filter/self PHI/cross-frontier panic");
}
static void pc_direct_budget(void) {
    for (unsigned enough=0;enough<2;++enough) {
        XrCompileResourceLimits caps=pc_caps();
        XrXirCompileContext c=pc_owner(caps);uint64_t baseline=pc_stats(&c).live_bytes;
        EdgeFixture f;pc_fixture(&f,PC_REVERSE,false);
        XrXirEffects e={0};EffectTerms terms={0};ErrorFlow flow={0};flow.remaining=&c;
        pc_private_begin(&c,&f,&e,&terms,&flow);
        CHECK(error_function(&flow,1)==XR_XIR_OK && e.errors[1]==4 && e.atom_count==2);
        CHECK(flow.values==545 && flow.active_count==1 && flow.stride==1 && e.words==1);
        size_t bytes=flow.stride*sizeof(uint64_t);
        uint64_t *dest=flow.states+(size_t)30*flow.stride;
        memset(flow.work,0,bytes);error_value(&flow,flow.work,512)[0]=4;
        memset(dest,0,bytes);flow.reachable[30]=0;flow.changed=false;
        memset(flow.edge,0xa5,bytes);memset(flow.snapshot,0x5a,bytes);
        uint64_t remaining=enough ? UINT64_C(6) : UINT64_C(5);
        uint64_t used=pc_stats(&c).work;CHECK(used<=caps.work-remaining);
        /* Deliberate same-ledger admission fixture, without mutating its limits. */
        CHECK(xr_compile_resources_work(c.resources,caps.work-used-remaining)==XR_COMPILE_RESOURCE_OK);
        XrCompileResourceStats before=pc_stats(&c);size_t allocations=attempts;
        CHECK(error_edge(&flow,31,30,NULL,true)==(enough ? XR_XIR_OK : XR_XIR_BUDGET));
        XrCompileResourceStats after=pc_stats(&c);
        CHECK(after.work-before.work==(enough ? UINT64_C(6) : UINT64_C(3)));
        CHECK(attempts==allocations && before.allocated_bytes==after.allocated_bytes && before.live_bytes==after.live_bytes);
        pc_plain_row(&flow,flow.work);pc_pattern(flow.edge,bytes,0xa5);pc_pattern(flow.snapshot,bytes,0x5a);
        if (enough) { pc_plain_row(&flow,dest);CHECK(flow.reachable[30]==3 && flow.changed); }
        else { pc_pattern(dest,bytes,0);CHECK(!flow.reachable[30] && !flow.changed); }
        pc_private_free(&flow,&terms,&e);pc_release(&c,baseline);
    }
    puts("strict edge sameowner remaining6/5: no refund, failure state unchanged, allocator0");
}
static void pc_bulk(void) {
    for (unsigned reverse=0;reverse<2;++reverse) {
        EdgeFixture f;pc_fixture(&f,reverse ? PC_REVERSE : PC_FORWARD,false);
        XrXirCompileContext c=pc_owner(pc_caps());uint64_t baseline=pc_stats(&c).live_bytes;
        XrXirEffects e={0};EffectTerms terms={0};ErrorFlow flow={0};flow.remaining=&c;
        pc_private_begin(&c,&f,&e,&terms,&flow);
        CHECK(f.functions[1].instruction_count==545 && f.functions[1].block_count==32);
        uint64_t before=pc_stats(&c).work;
        CHECK(error_function(&flow,1)==XR_XIR_OK && e.errors[1]==4);
        uint64_t cost=pc_stats(&c).work-before;
        CHECK(cost<UINT64_C(700000));
        printf("pending scalar512 blocks32 reverse%u work%llu below frozen700000\n",reverse,(unsigned long long)cost);
        pc_private_free(&flow,&terms,&e);pc_release(&c,baseline);
    }
}
static void pc_freshness(void) {
    EdgeFixture f;pc_fixture(&f,PC_REVERSE,false);
    XrXirCompileContext c=pc_owner(pc_caps());uint64_t baseline=pc_stats(&c).live_bytes;
    XrXirEffects e={0};EffectTerms terms={0};ErrorFlow flow={0};flow.remaining=&c;
    pc_private_begin(&c,&f,&e,&terms,&flow);
    CHECK(error_function(&flow,1)==XR_XIR_OK && e.errors[1]==4);
    static const PendingCase cases[]={PC_PHI_SELF,PC_LATE_JOIN,PC_FILTER,PC_REVERSE};
    static const uint64_t literal[]={12,12,4,4};
    for (size_t i=0;i<sizeof(cases)/sizeof(*cases);++i) {
        pc_fixture(&f,cases[i],false);CHECK(xir_fixture_verify(&c, &f.module, NULL)==XR_XIR_OK);
        e.errors[1]=0;memset(flow.storage,0xff,flow.storage_capacity);
        CHECK(error_function(&flow,1)==XR_XIR_OK && e.errors[1]==literal[i]);
    }
    pc_private_free(&flow,&terms,&e);pc_release(&c,baseline);
}
static void pc_negative(void) {
    EdgeFixture f;pc_fixture(&f,PC_PHI_SELF,false);f.module.stage=XR_XIR_BUILT;
    XrXirCompileContext c=pc_owner(pc_caps());uint64_t baseline=pc_stats(&c).live_bytes;
    XrXirArtifact *checked=NULL;XrXirDiagnostic d={0};
    f.subject[5].targets[0]=UINT32_MAX;
    CHECK(xir_fixture_check(&c, &f.module, &checked, &d)==XR_XIR_BAD_STRUCTURE && !checked);
    pc_fixture(&f,PC_PHI_SELF,false);f.module.stage=XR_XIR_BUILT;f.parameters[1]=XR_XIR_I64;
    CHECK(xir_fixture_check(&c, &f.module, &checked, &d)==XR_XIR_BAD_TYPE && !checked);
    pc_release(&c,baseline);
}
/* Complete sameowner proofs still run; only exact empty equations skip rows. */
static void pc_empty_fixture(EdgeFixture *f,unsigned mode) {
    static const PendingCase shapes[]={PC_FORWARD,PC_REVERSE,PC_PHI_SELF,PC_FILTER,
        PC_FORWARD,PC_CLEANUP,PC_PANIC,PC_FORWARD};
    CHECK(mode<8);pc_fixture(f,shapes[mode],false);
    for (uint32_t i=0;i<f->functions[1].instruction_count;++i)
        if (f->subject[i].op==XR_XIR_THROW) f->subject[i]=(XrXirInstruction){.op=XR_XIR_RETURN};
    if (mode==4) {
        f->parameters[0]=XR_XIR_ERROR;
        f->subject[0]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={0}};
        f->blocks[0]=(XrXirBlock){.count=1};
        f->functions[1].parameters=f->parameters;f->functions[1].parameter_count=1;
        f->functions[1].result=XR_XIR_ERROR;f->functions[1].instruction_count=1;
        f->functions[1].block_count=1;f->functions[1].operands=NULL;f->functions[1].operand_count=0;
    } else if (mode==7) f->subject[0]=(XrXirInstruction){.op=XR_XIR_SUSPEND};
    f->module.stage=XR_XIR_BUILT;
}
static void pc_empty_golden(const XrXirEffects *e,unsigned mode) {
    CHECK(e && e->count==(mode==5 || mode==6 ? 4u : 3u));
    CHECK(e->atom_count==2 && e->words==1 && !e->errors[0] && !e->errors[1] && e->errors[2]==8);
    CHECK(e->functions[1].throws==XR_XIR_EFFECT_NONE);
    CHECK(e->functions[1].suspend==(mode==7 ? XR_XIR_EFFECT_MAY : XR_XIR_EFFECT_NONE));
    CHECK(!xr_xir_effects_error_unknown(e,1) && !xr_xir_effects_error_unidentified(e,1));
    if (e->count==4) CHECK(!e->errors[3] && e->functions[3].throws==XR_XIR_EFFECT_NONE);
    for (uint32_t f=0;f<e->count;++f) {
        CHECK(e->task_errors[f]==XR_XIR_OK);
        if (e->count==4 && f==3) pc_lexical_cleanup_root(e);
        else CHECK(e->root[f].requires_root==(f==0) && !e->root[f].unresolved);
    }
}
static XrXirStatus pc_empty_whole(const XrXirCompileContext *c,unsigned mode) {
    EdgeFixture f;pc_empty_fixture(&f,mode);
    XrXirArtifact *checked=NULL;XrXirEffects *e=NULL;XrXirDiagnostic d={0};
    XrXirStatus status=xir_fixture_check(c, &f.module, &checked, &d);
    if (status==XR_XIR_OK) status=xr_xir_compile_effects_analyze(checked,&e);
    else CHECK(!checked);
    if (status==XR_XIR_OK) pc_empty_golden(e,mode);else CHECK(!e);
    if (status!=XR_XIR_OK && status!=XR_XIR_OUT_OF_MEMORY && status!=XR_XIR_BUDGET)
        fprintf(stderr,"empty mode%u status%u f%u b%u i%u\n",mode,(unsigned)status,d.function,d.block,d.instruction);
    xr_xir_compile_effects_free(e);xr_xir_compile_artifact_free(checked);return status;
}
static void pc_empty_cases(void) {
    for (unsigned mode=0;mode<8;++mode) {
        XrXirCompileContext c=pc_owner(pc_caps());uint64_t baseline=pc_stats(&c).live_bytes;
        CHECK(pc_empty_whole(&c,mode)==XR_XIR_OK);pc_release(&c,baseline);
    }
    XrXirCompileContext c=pc_owner(pc_caps());uint64_t baseline=pc_stats(&c).live_bytes;
    EdgeFixture f;pc_empty_fixture(&f,5);
    f.cleanup[2]=(XrXirInstruction){.op=XR_XIR_THROW,.args={1}};
    XrXirArtifact *checked=NULL;
    CHECK(xir_fixture_check(&c, &f.module, &checked, NULL)==XR_XIR_BAD_TYPE && !checked);
    pc_release(&c,baseline);
    puts("empty whole literals: wide/reverse/PHI/filter/Error return/cleanup/panic/suspend and badcleanup");
}
static void pc_empty_cost(void) {
    for (unsigned reverse=0;reverse<2;++reverse) {
        EdgeFixture f;pc_empty_fixture(&f,reverse);
        XrXirCompileContext c=pc_owner(pc_caps());uint64_t baseline=pc_stats(&c).live_bytes;
        XrXirArtifact *checked=NULL;XrXirEffects *e=NULL;
        CHECK(xir_fixture_check(&c, &f.module, &checked, NULL)==XR_XIR_OK);
        const XrXirModule *owned=xr_xir_compile_artifact_module(checked);
        CHECK(owned && owned->stage==XR_XIR_CHECKED && xr_xir_compile_verify_v2(&c,owned,xr_xir_compile_artifact_construction(checked), NULL)==XR_XIR_OK);
        uint64_t before=pc_stats(&c).work;
        CHECK(xr_xir_compile_effects_infer_verified(&c,owned,&e)==XR_XIR_OK);
        pc_empty_golden(e,reverse);uint64_t cost=pc_stats(&c).work-before;
        CHECK(cost<UINT64_C(20000));
        printf("empty complete sameowner inference scalar512/block32 reverse%u work%llu below frozen20000\n",reverse,(unsigned long long)cost);
        xr_xir_compile_effects_free(e);xr_xir_compile_artifact_free(checked);pc_release(&c,baseline);
    }
}
static void pc_empty_classifier(void) {
    static const XrXirOp producers[]={XR_XIR_THROW,XR_XIR_CALL_DEFAULT,XR_XIR_CALL,
        XR_XIR_CALL_INDIRECT,XR_XIR_CALL_REQUIREMENT,XR_XIR_GO,XR_XIR_INVOKE_DEFAULT,
        XR_XIR_INVOKE,XR_XIR_INVOKE_INDIRECT,XR_XIR_INVOKE_ERROR,XR_XIR_TASK_AWAIT};
    static const XrXirOp invalid[]={XR_XIR_INVALID,XR_XIR_OP_COUNT,(XrXirOp)UINT32_MAX};
    XrXirCompileContext c=pc_owner(pc_caps());uint64_t baseline=pc_stats(&c).live_bytes;
    XrXirInstruction ops[3]={{.op=XR_XIR_CONST_INT},{.op=XR_XIR_ENUM_NEW},{.op=XR_XIR_RETURN}};
    XrXirFunction function={.instructions=ops,.instruction_count=3};bool empty=false;
    uint64_t before=pc_stats(&c).work;size_t allocations=attempts;
    CHECK(error_empty_escaping(&function,&c,&empty)==XR_XIR_OK && empty);
    CHECK(pc_stats(&c).work-before==3 && attempts==allocations);
    for (size_t i=0;i<sizeof(producers)/sizeof(*producers);++i) {
        ops[1].op=producers[i];empty=true;before=pc_stats(&c).work;
        CHECK(error_empty_escaping(&function,&c,&empty)==XR_XIR_OK && !empty);
        CHECK(pc_stats(&c).work-before==2 && attempts==allocations);
    }
    for (size_t i=0;i<sizeof(invalid)/sizeof(*invalid);++i) {
        ops[1].op=invalid[i];empty=true;before=pc_stats(&c).work;
        CHECK(error_empty_escaping(&function,&c,&empty)==XR_XIR_BAD_STRUCTURE && !empty);
        CHECK(pc_stats(&c).work-before==2 && attempts==allocations);
    }
    pc_release(&c,baseline);
    for (unsigned enough=0;enough<2;++enough) {
        XrCompileResourceLimits caps=pc_caps();c=pc_owner(caps);baseline=pc_stats(&c).live_bytes;
        ops[1].op=XR_XIR_ENUM_NEW;empty=true;
        uint64_t available=enough ? 3u : 2u;uint64_t used=pc_stats(&c).work;
        CHECK(xr_compile_resources_work(c.resources,caps.work-used-available)==XR_COMPILE_RESOURCE_OK);
        before=pc_stats(&c).work;allocations=attempts;
        CHECK(error_empty_escaping(&function,&c,&empty)==(enough ? XR_XIR_OK : XR_XIR_BUDGET));
        CHECK(empty==(enough!=0) && pc_stats(&c).work-before==available && attempts==allocations);
        pc_release(&c,baseline);
    }
    puts("empty classifier11 producers/3invalid and scan exact-minus1 no refund/allocator0");
}
static void pc_empty_nonempty_summary(void) {
    EdgeFixture f;pc_empty_fixture(&f,1);
    XrXirCompileContext c=pc_owner(pc_caps());uint64_t baseline=pc_stats(&c).live_bytes;
    XrXirArtifact *checked=NULL;XrXirEffects *e=NULL;
    CHECK(xir_fixture_check(&c, &f.module, &checked, NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_effects_analyze(checked,&e)==XR_XIR_OK);pc_empty_golden(e,1);
    const XrXirModule *owned=xr_xir_compile_artifact_module(checked);
    /* Private monotonicity control; never a public permission witness. */
    e->errors[1]=4;EffectGraph graph={0};
    CHECK(effect_graph_build(owned,e,&graph,&c)==XR_XIR_OK);
    EffectTerms terms={0};terms.remaining=&c;terms.types=*owned->types;
    ErrorFlow flow={0};flow.module=owned;flow.effects=e;flow.remaining=&c;flow.terms=&terms;
    CHECK(error_functions(owned,e,&graph,&flow)==XR_XIR_OK);
    CHECK(e->errors[1]==4 && flow.storage_capacity==4684);
    error_storage_free(&flow);effect_terms_free(&terms);effect_graph_free(&graph);
    xr_xir_compile_effects_free(e);xr_xir_compile_artifact_free(checked);pc_release(&c,baseline);
}
static void pc_empty_faults(void) {
    size_t sites=0;
    for (size_t pass=0;pass<=sites;++pass) {
        XrXirCompileContext c=pc_owner(pc_caps());uint64_t baseline=pc_stats(&c).live_bytes;
        attempts=0;injected=false;fail_at=pass ? pass-1 : SIZE_MAX;
        XrXirStatus status=pc_empty_whole(&c,1);size_t actual=attempts;fail_at=SIZE_MAX;
        if (!pass) { CHECK(status==XR_XIR_OK);sites=actual;CHECK(sites>0 && sites<4096); }
        else CHECK(injected && actual>=pass && status==XR_XIR_OUT_OF_MEMORY);
        CHECK(pc_stats(&c).live_bytes==baseline && pc_empty_whole(&c,1)==XR_XIR_OK);
        pc_release(&c,baseline);
    }
    printf("empty whole OOM sites%zu sameownerretry physical0\n",sites);
}
static XrCompileResourceStats pc_empty_measured(XrCompileResourceLimits caps,XrXirStatus expected) {
    XrXirCompileContext c=pc_owner(caps);uint64_t baseline=pc_stats(&c).live_bytes;
    CHECK(pc_empty_whole(&c,1)==expected);XrCompileResourceStats s=pc_stats(&c);
    pc_release(&c,baseline);return s;
}
static void pc_empty_limits(void) {
    XrCompileResourceStats a=pc_empty_measured(pc_caps(),XR_XIR_OK);
    XrCompileResourceLimits exact={a.allocated_bytes,a.peak_bytes,a.work};
    XrCompileResourceStats b=pc_empty_measured(exact,XR_XIR_OK);
    CHECK(a.allocated_bytes==b.allocated_bytes && a.peak_bytes==b.peak_bytes && a.work==b.work);
    CHECK(exact.allocated_bytes>1 && exact.live_bytes>1 && exact.work>1);
    XrCompileResourceLimits less=exact;--less.allocated_bytes;(void)pc_empty_measured(less,XR_XIR_BUDGET);
    less=exact;--less.live_bytes;(void)pc_empty_measured(less,XR_XIR_BUDGET);
    less=exact;--less.work;(void)pc_empty_measured(less,XR_XIR_BUDGET);
}
#include "xir_error_cfg_pending_resources.h"
#include "xir_error_compact_rows_controls.h"
#include "xir_error_closed_carriers_controls.h"
#include "xir_error_callee_first_controls.h"
#include "xir_error_unit_cell_controls.h"
int main(void) {
    pc_unit_cell_controls();
    pc_callee_first_controls();
    pc_compact_controls();pc_closed_carriers_controls();
    pc_empty_cases();pc_empty_cost();pc_empty_classifier();pc_empty_nonempty_summary();
    pc_empty_faults();pc_empty_limits();
    pc_cases();pc_edge_controls();pc_direct_budget();pc_bulk();pc_freshness();pc_negative();pc_occupied();
    pc_faults(PC_PHI_SELF);pc_faults(PC_LATE_JOIN);
    pc_limits(PC_PHI_SELF);pc_limits(PC_LATE_JOIN);
    CHECK(!live && !live_bytes);puts("typed-error CFG pending ownership PASS");return 0;
}
