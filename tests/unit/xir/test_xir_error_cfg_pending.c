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
static void (*pc_free_hook)(void *,size_t);
static void (*pc_allocate_hook)(void *,size_t);
static void *pc_malloc(size_t bytes) {
    if (attempts++==fail_at) { injected=true;return NULL; }
    void *p=xr_malloc(bytes);if (!p) return NULL;
    CHECK(live<2048 && bytes<=SIZE_MAX-live_bytes);
    pc_physical[live++]=(EdgePhysical){p,bytes};live_bytes+=bytes;
    if (pc_allocate_hook) pc_allocate_hook(p,bytes);
    return p;
}
static void pc_free(void *p) {
    if (!p) return;
    size_t i=0;while (i<live && pc_physical[i].pointer!=p) ++i;
    CHECK(i<live && live_bytes>=pc_physical[i].bytes);
    if (pc_free_hook) pc_free_hook(p,pc_physical[i].bytes);
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
#include "xir_effect_input_fixture.h"
#include "xir_legacy_error_oracle.h"
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
/* The historical row extent remains exact; the queue has separate real bytes. */
static void pc_release(XrXirCompileContext *c, uint64_t baseline) {
    CHECK(pc_stats(c).live_bytes==baseline);xr_compile_resources_release(c->resources);
    *c=(XrXirCompileContext){0};CHECK(!live && !live_bytes);
}
/* A lexical Cell formal stays conditional. Each authenticated REGISTER
 * supplies its actual owner; this fixture supplies an owned local Cell. */
static void pc_lexical_cleanup_root(const XrXirEffects *e) {
    CHECK(e && e->count==4 && effect_roots_read(e)->contracts && e->cells);
    CHECK(!effect_roots_read(e)->root[3].requires_root && effect_roots_read(e)->root[3].unresolved);
    const XrXirRootFormula *child=&effect_roots_read(e)->contracts[3].formula;
    CHECK(!child->constant_mask && child->term_count==1 && child->terms);
    CHECK(child->terms[0].kind==XR_XIR_ROOT_TERM_CELL_PARAMETER && child->terms[0].index==0);
    CHECK(effect_roots_read(e)->contracts[3].parameter_count==1);
    CHECK(xr_xir_cell_provenance_role(e->cells,3,0)==XR_XIR_CELL_PROOF_LEXICAL_CLEANUP);
    CHECK(xr_xir_cell_provenance_origin(e->cells,3,0)==XR_XIR_CELL_ORIGIN_SCOPED);
    CHECK(effect_roots_read(e)->root_witnesses[3].cause==XR_XIR_ROOT_CAUSE_NONE);
    CHECK(effect_roots_read(e)->unresolved_witnesses[3].cause==XR_XIR_ROOT_CAUSE_CELL_PARAMETER &&
        effect_roots_read(e)->unresolved_witnesses[3].callee==UINT32_MAX &&
        effect_roots_read(e)->unresolved_witnesses[3].slot==0 && !effect_roots_read(e)->unresolved_witnesses[3].distance);
    CHECK(!effect_roots_read(e)->root[1].requires_root && !effect_roots_read(e)->root[1].unresolved);
    CHECK(!effect_roots_read(e)->contracts[1].formula.constant_mask && !effect_roots_read(e)->contracts[1].formula.term_count);
}
static void pc_golden(const XrXirEffects *e,PendingCase mode,bool snapshot) {
    (void)snapshot;
    CHECK(e && e->count==(mode==PC_CLEANUP || mode==PC_PANIC ? 4u : 3u));
    CHECK(effect_final_outcomes(e)->functions[1].throws==XR_XIR_EFFECT_MAY);
    for (uint32_t f=0;f<e->count;++f) {
        if (e->count==4 && f==3) pc_lexical_cleanup_root(e);
        else CHECK(effect_roots_read(e)->root[f].requires_root==(f==0) && !effect_roots_read(e)->root[f].unresolved);
        CHECK(effect_final_outcomes(e)->functions[f].suspend==XR_XIR_EFFECT_NONE);
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
/* Controlled rows are private freshness probes, never permission witnesses. */
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
    f->functions[1].error_type=XR_XIR_NEVER;
    f->module.stage=XR_XIR_BUILT;
}
static void pc_empty_records(const XrXirEffects *e,const EffectFinalOutcomes *facts,unsigned mode) {
    CHECK(facts);
    CHECK(e && e->count==(mode==5 || mode==6 ? 4u : 3u));
    CHECK(facts->functions[1].throws==XR_XIR_EFFECT_NONE);
    CHECK(facts->functions[1].suspend==(mode==7 ? XR_XIR_EFFECT_MAY : XR_XIR_EFFECT_NONE));
    if (e->count==4) CHECK(facts->functions[3].throws==XR_XIR_EFFECT_NONE);
    for (uint32_t f=0;f<e->count;++f) {
        if (e->count==4 && f==3) pc_lexical_cleanup_root(e);
        else CHECK(effect_roots_read(e)->root[f].requires_root==(f==0) && !effect_roots_read(e)->root[f].unresolved);
    }
}
static void pc_empty_golden(const XrXirEffects *effects,unsigned mode) {
    CHECK(effect_outcomes_empty(&effects->outcomes_work) && !effects->outcomes_owner);
    pc_empty_records(effects,effect_final_outcomes(effects),mode);
}
static void pc_empty_constructed(const XrXirEffects *effects,unsigned mode) {
    CHECK(effects->outcomes_owner && !effects->invocations->final->outcomes && !effect_final_outcomes(effects));
    pc_empty_records(effects,&effects->outcomes_work,mode);
    CHECK(!(xr_xir_effects_read(effects,0).status==XR_XIR_OK) && !(xr_xir_effects_read(effects,0).status==XR_XIR_OK && xr_xir_effects_read(effects,0).suspend_witness.cause!=0) && xr_xir_effects_go_safe(effects,0)==XR_XIR_BAD_STRUCTURE && xr_xir_effects_task_creation(effects,0)==XR_XIR_EFFECT_UNKNOWN);
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
/* Literal authority and error goldens do not come from the local producer.
 * Each counterfactual targets the complete final receiver, then restores the
 * exact owned field before destruction or the nearest positive check. */
static void pc_local_final_mutations(const XrXirCompileContext *c,
    EffectInvocationCertificate *certificate,const EffectInvocationLocalInput *inputs) {
    EffectInvocationFinal *equations=certificate->final;
    EffectInvocationNode *node=&equations->nodes[1];EffectInvocationNode saved=*node;
    for (unsigned mode=0;mode<10;++mode) {
        if (!mode) node->intrinsic_mask=XR_XIR_CALLABLE_ROOT_REQUIRED;
        if (mode==1) node->local_mask=XR_XIR_CALLABLE_ROOT_UNRESOLVED;
        if (mode==2) ++node->instruction_count;
        if (mode==3) node->parameter_count=1;
        if (mode==4) --node->cause_count;
        if (mode==5) node->input=(void *)(uintptr_t)1;
        if (mode==6) node->root=UINT32_MAX;
        if (mode==7) node->edge_head=0;
        if (mode==8) node->deferred=true;
        if (mode==9) node->local_deferred=true;
        CHECK(effect_invocation_local_equations_match(c,equations,inputs)==XR_XIR_BAD_STRUCTURE);
        *node=saved;
        CHECK(effect_invocation_local_equations_match(c,equations,inputs)==XR_XIR_OK);
    }
    EffectInvocationCause old=node->causes[0];node->causes[0].witness.cause=XR_XIR_ROOT_CAUSE_INITIALIZER;
    CHECK(effect_invocation_local_equations_match(c,equations,inputs)==XR_XIR_BAD_STRUCTURE);
    node->causes[0]=old;
    uint64_t bits=node->cells[0];node->cells[0]=UINT64_C(1)<<63;
    CHECK(effect_invocation_local_equations_match(c,equations,inputs)==XR_XIR_BAD_STRUCTURE);
    node->cells[0]=bits;
    EffectPhysicalFunction *physical=&certificate->body->physical->functions[1];
    CHECK(physical->count);uint32_t count=physical->count;--physical->count;
    CHECK(effect_body_physical_match(c,certificate->body->physical,certificate->body)==XR_XIR_BAD_STRUCTURE);
    physical->count=count;
    EffectFinalFunction *function=&certificate->final->summary.functions[0];
    XrXirRootFormula formula=function->formula;function->formula.constant_mask=0;
    CHECK(effect_invocation_local_result_match(c,equations,inputs,(&certificate->final->summary))==XR_XIR_BAD_STRUCTURE);
    function->formula=formula;
    /* Removing the entire initializer result remains invalid even if all
     * mutable receiving rows are made mutually consistent after a callback. */
    EffectInvocationNode init=equations->nodes[0];equations->nodes[0].intrinsic_mask=0;
    equations->nodes[0].local_mask=0;function->formula.constant_mask=0;
    CHECK(effect_invocation_certificate_result_match(c,certificate,(&certificate->final->summary))==XR_XIR_BAD_STRUCTURE);
    equations->nodes[0]=init;function->formula=formula;
    CHECK(effect_invocation_certificate_result_match(c,certificate,(&certificate->final->summary))==XR_XIR_OK);
}

static void pc_local_source_fields(const XrXirCompileContext *c,
    const EffectBodyMaterial *body,const XrXirModule *source) {
    CHECK(source && source->functions && source->function_count==3);
    CHECK(effect_body_source_match(c,body,source)==XR_XIR_OK);
    XrXirInstruction *row=(XrXirInstruction *)&source->functions[1].instructions[4];
    XrXirInstruction saved=*row;
    for (uint32_t word=0;word<10;++word) {
        if (!word) row->op=XR_XIR_RETURN;
        if (word==1) row->type=XR_XIR_BOOL;
        if (word==2) row->args[0]=1;
        if (word==3) row->args[1]=1;
        if (word==4) row->targets[0]=1;
        if (word==5) row->targets[1]=1;
        if (word==6) row->immediate=-1;
        if (word==7) row->immediate=INT64_MIN;
        if (word==8) row->type_arguments[0]=1;
        if (word==9) row->type_arguments[1]=1;
        CHECK(effect_body_source_match(c,body,source)==XR_XIR_BAD_STRUCTURE);
        /* Exact current-input equality also rejects fields that do not
         * change sparse physical membership or the apparent type category. */
        CHECK(effect_invocation_current_input_match(c,source,body)==XR_XIR_BAD_STRUCTURE);
        *row=saved;
    }
    CHECK(effect_invocation_current_input_match(c,source,body)==XR_XIR_OK);
}

static void pc_local_transfer_positive(void) {
    XrXirCompileContext c=pc_owner(pc_caps());uint64_t baseline=pc_stats(&c).live_bytes;
    EdgeFixture fixture;pc_empty_fixture(&fixture,0);
    fixture.subject[1]=(XrXirInstruction){.op=XR_XIR_COPY,.type=XR_XIR_I64,.args={0}};
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;
    CHECK(xir_fixture_check(&c,&fixture.module,&checked,NULL)==XR_XIR_OK);
    /* COPY remains a real transfer in Checked. SCALAR_COPY is emitted only
     * by lowering and cannot enter this Built admission control. */
    const XrXirModule *actual=xr_xir_compile_artifact_module(checked);
    CHECK(actual && actual->stage==XR_XIR_CHECKED && actual->functions[1].instruction_count>1 &&
        actual->functions[1].instructions[1].op==XR_XIR_COPY &&
        actual->functions[1].instructions[1].type==XR_XIR_I64 &&
        !actual->functions[1].instructions[1].args[0]);
    CHECK(effect_physical_opcode(actual->functions[1].instructions[1].op)&EFFECT_PHYSICAL_TRANSFER);
    CHECK(xr_xir_compile_effects_analyze(checked,&effects)==XR_XIR_OK);pc_empty_golden(effects,0);
    EffectInvocationLocalInput *inputs=NULL;
    CHECK(effect_invocation_local_inputs(&c,effects->invocations->body,&inputs)==XR_XIR_OK && !inputs);
    CHECK(effect_invocation_public_gate(&c,effects->invocations)==XR_XIR_OK);
    xr_xir_compile_effects_free(effects);xr_xir_compile_artifact_free(checked);pc_release(&c,baseline);
}

typedef struct PcLocalCallback {
    EffectFinalFunctions *receiving;
    uint32_t calls;
} PcLocalCallback;

/* The last genuine projection callback forges a consistent receiving ROOT
 * absence. Final publication must still derive the initializer independently. */
static void pc_erase_initializer_root(const EffectInvocationCertificate *certificate,
    EffectFinalFunctions *result) {
    EffectInvocationNode *node=&certificate->final->nodes[0];
    node->intrinsic_mask=node->local_mask=0;result->functions[0].formula.constant_mask=0;
    for (uint32_t domain=0;domain<4;++domain) {
        size_t at=(size_t)domain*result->count;
        result->witnesses[at]=(XrXirRootEffectWitness){0};
    }
    for (uint32_t cause=0;cause<6;++cause)
        node->causes[cause]=(EffectInvocationCause){.next=UINT32_MAX,.edge=UINT32_MAX,.distance=UINT32_MAX};
}

static XrXirStatus pc_local_callback_write(const XrXirCompileContext *work,
    const EffectInvocationCertificate *certificate,uint32_t function,
    const uint32_t *selected,EffectFinalFunctions *result,void *receiver) {
    PcLocalCallback *probe=receiver;
    XrXirStatus status=effect_invocation_publication_write(work,certificate,function,selected,
        result,&probe->receiving);
    if (status!=XR_XIR_OK) return status;
    ++probe->calls;
    if (function+1!=result->count) return XR_XIR_OK;
    pc_erase_initializer_root(certificate,result);
    return XR_XIR_OK;
}

static void pc_local_callback_boundary(void) {
    XrXirCompileContext c=pc_owner(pc_caps());uint64_t baseline=pc_stats(&c).live_bytes;
    EdgeFixture fixture;pc_empty_fixture(&fixture,0);
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;
    CHECK(xir_fixture_check(&c,&fixture.module,&checked,NULL)==XR_XIR_OK);
    CHECK(effect_infer_construct(&c,xr_xir_compile_artifact_module(checked),NULL,&effects)==XR_XIR_OK);pc_empty_constructed(effects,0);
    const XrXirModule *source=xr_xir_compile_artifact_module(checked);EffectGraph graph={0};
    CHECK(effect_graph_build(source,effects,&graph,&c)==XR_XIR_OK && graph.physical_owned);
    EffectBodyRequest body=effect_body_owned_request(&c,graph.material);
    EffectInvocationDriverInput input={&c,&body,effects,&graph,graph.material->declared,NULL};
    PcLocalCallback probe={0};EffectInvocationCertificate *certificate=NULL,*old=effects->invocations;
    EffectInvocationProjectionSink sink={c.resources,effects->count,&probe,pc_local_callback_write};
    CHECK(effect_invocation_derive(&input,&certificate,&sink,&probe.receiving)==XR_XIR_OK &&
        certificate && !probe.receiving && probe.calls==3 && !graph.physical_owned);
    CHECK(effect_invocation_current_input_match(&c,source,certificate->body)==XR_XIR_OK);
    CHECK(effect_invocation_publication_commit(&c,effects,&certificate)==XR_XIR_BAD_STRUCTURE &&
        certificate && effects->invocations==old && effect_roots_read(effects)->root[0].requires_root);
    pc_empty_constructed(effects,0);effect_invocation_certificate_free(certificate);effect_graph_free(&graph);
    xr_xir_compile_effects_free(effects);xr_xir_compile_artifact_free(checked);pc_release(&c,baseline);
}

static EffectInvocationCertificate *pc_release_target;
static unsigned pc_release_poison_calls;
static void pc_local_release_poison(void *memory,size_t bytes) {
    size_t expected=(size_t)(&pc_release_target->final->summary)->count*sizeof(EffectInvocationLocalInput);
    if (bytes!=sizeof(CompileAllocation)+expected) return;
    const EffectInvocationLocalInput *inputs=(const void *)((CompileAllocation *)memory+1);
    CHECK(inputs[0].facts.requires_root &&
        inputs[0].witnesses[0].cause==XR_XIR_ROOT_CAUSE_INITIALIZER);
    pc_free_hook=NULL;
    ++pc_release_poison_calls;
    pc_erase_initializer_root(pc_release_target,(&pc_release_target->final->summary));
}

/* The initializer's genuine Input remains unchanged. A real allocator free
 * coherently erases every receiving ROOT view after the expected-row proof.
 * Publication must still reject the forged absence before replacing Effects. */
static void pc_local_release_boundary(void) {
    XrXirCompileContext c=pc_owner(pc_caps());uint64_t baseline=pc_stats(&c).live_bytes;
    EdgeFixture fixture;pc_empty_fixture(&fixture,0);
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;
    CHECK(xir_fixture_check(&c,&fixture.module,&checked,NULL)==XR_XIR_OK);
    CHECK(effect_infer_construct(&c,xr_xir_compile_artifact_module(checked),NULL,&effects)==XR_XIR_OK);pc_empty_constructed(effects,0);
    const XrXirModule *source=xr_xir_compile_artifact_module(checked);EffectGraph graph={0};
    CHECK(effect_graph_build(source,effects,&graph,&c)==XR_XIR_OK && graph.physical_owned);
    EffectBodyRequest body=effect_body_owned_request(&c,graph.material);
    EffectInvocationDriverInput input={&c,&body,effects,&graph,graph.material->declared,NULL};
    EffectInvocationCertificate *certificate=NULL,*old=effects->invocations;
    EffectFinalFunctions *receiving=NULL;
    CHECK(effect_invocation_derive(&input,&certificate,NULL,&receiving)==XR_XIR_OK &&
        certificate && !receiving && !graph.physical_owned);
    CHECK(effect_invocation_current_input_match(&c,source,certificate->body)==XR_XIR_OK);
    CHECK(effect_invocation_publication_match(&c,certificate,(&certificate->final->summary))==XR_XIR_OK);
    pc_release_target=certificate;pc_release_poison_calls=0;pc_free_hook=pc_local_release_poison;
    XrXirStatus status=effect_invocation_publication_commit(&c,effects,&certificate);
    pc_free_hook=NULL;pc_release_target=NULL;
    printf("allocator release ROOT pollution: callbacks%u status%u publishedRoot%u\n",
        pc_release_poison_calls,(unsigned)status,(unsigned)effect_roots_read(effects)->root[0].requires_root);
    CHECK(pc_release_poison_calls<=1 && effect_roots_read(effects)->root[0].requires_root);
    if (pc_release_poison_calls)
        CHECK(status==XR_XIR_BAD_STRUCTURE && certificate && effects->invocations==old);
    else
        CHECK(status==XR_XIR_OK && !certificate && effects->invocations!=old);
    pc_empty_constructed(effects,0);effect_invocation_certificate_free(certificate);effect_graph_free(&graph);
    xr_xir_compile_effects_free(effects);xr_xir_compile_artifact_free(checked);pc_release(&c,baseline);
}

/* A genuine complete Library with one scalar formal needs the ordinary
 * binding algorithm. The independent dimensional test constructs no result. */
static void pc_local_formal_preflight(const XrXirCompileContext *c) {
    XrXirType parameter=XR_XIR_I64;
    XrXirInstruction operation={.op=XR_XIR_RETURN,.args={0}};
    XrXirBlock block={.count=1};
    XrXirFunction function={.name="formal",.name_length=6,.parameters=&parameter,
        .parameter_count=1,.result=XR_XIR_I64,.blocks=&block,.block_count=1,
        .instructions=&operation,.instruction_count=1};
    XrXirInstruction initializer={.op=XR_XIR_RETURN};
    XrXirFunction functions[2]={function,{.name="init",.name_length=4,
        .result=XR_XIR_UNIT,.blocks=&block,.block_count=1,
        .instructions=&initializer,.instruction_count=1}};
    XrXirFunctionIdentity identities[2]={{.exported=1},{0}};
    XrXirSourceModule module={.name="formal",.name_length=6,.initializer=1};
    XrXirDeclarations declarations={.modules=&module,.module_count=1,
        .functions=identities,.root_module=UINT32_MAX,.entry_function=UINT32_MAX};
    XrXirModule source={.stage=XR_XIR_BUILT,.functions=functions,.declarations=&declarations,
        .function_count=2,.linkage_kind=XR_XIR_LIBRARY};
    XrXirArtifact *checked=NULL;EffectBodyMaterial *body=NULL;
    CHECK(xir_fixture_check(c,&source,&checked,NULL)==XR_XIR_OK && checked);
    CHECK(effect_body_material_create(c,xr_xir_compile_artifact_module(checked),&body)==XR_XIR_OK && body);
    EffectInvocationLocalInput *input=NULL;
    XrCompileResourceStats before=pc_stats(c);size_t allocations=attempts;
    size_t saved_failure=fail_at;bool saved_injected=injected;
    fail_at=attempts;injected=false;
    CHECK(effect_invocation_local_inputs(c,body,&input)==XR_XIR_OK && !input && !injected);
    fail_at=saved_failure;injected=saved_injected;
    XrCompileResourceStats after=pc_stats(c);
    CHECK(attempts==allocations && after.allocation_count==before.allocation_count &&
        after.allocated_bytes==before.allocated_bytes && after.live_bytes==before.live_bytes &&
        after.work-before.work==13);
    EffectBodyFunction *head=&body->functions[0],saved=*head;
    for (unsigned mode=0;mode<4;++mode) {
        if (!mode) head->parameters=NULL;
        if (mode==1) head->blocks=NULL;
        if (mode==2) head->runs=NULL;
        if (mode==3) head->run_capacity=head->instruction_count+1;
        before=pc_stats(c);allocations=attempts;
        CHECK(effect_invocation_local_inputs(c,body,&input)==XR_XIR_BAD_STRUCTURE && !input &&
            attempts==allocations && pc_stats(c).allocated_bytes==before.allocated_bytes &&
            pc_stats(c).live_bytes==before.live_bytes);
        *head=saved;
        CHECK(effect_invocation_local_inputs(c,body,&input)==XR_XIR_OK && !input);
    }
    xr_xir_compile_artifact_free(checked);memset(functions,0xa5,sizeof(functions));
    memset(identities,0xa5,sizeof(identities));memset(&module,0xa5,sizeof(module));
    memset(&operation,0xa5,sizeof(operation));memset(&initializer,0xa5,sizeof(initializer));
    parameter=XR_XIR_BOOL;
    before=pc_stats(c);allocations=attempts;
    CHECK(effect_invocation_local_inputs(c,body,&input)==XR_XIR_OK && !input &&
        attempts==allocations && pc_stats(c).live_bytes==before.live_bytes &&
        pc_stats(c).work-before.work==13);
    effect_body_material_free(body);
}

/* Corrupt the actual owned type input, including a forged zero parameter
 * span and a backward-order violation. Each new read must rediscover it. */
static void pc_local_type_controls(const XrXirCompileContext *c,EffectBodyMaterial *body) {
    XrXirTypeNode *nodes=(XrXirTypeNode *)body->domain.types->nodes;
    CHECK(body->domain.types->count==2 && nodes[1].kind==XR_XIR_TYPE_CELL);
    XrXirTypeNode saved=nodes[1];
    XrCompileResourceStats before=pc_stats(c);size_t allocations=attempts;
    EffectInvocationLocalRead read={c,body,0};
    CHECK(effect_invocation_local_type(&read,(XrXirType)EE_CELL)==XR_XIR_OK);
    for (unsigned mode=0;mode<3;++mode) {
        nodes[1]=saved;
        if (!mode) nodes[1].element=(XrXirType)XR_XIR_TYPE_PARAMETER_BASE;
        if (mode==1) nodes[1].element=(XrXirType)EE_CELL;
        if (mode==2) nodes[1].parameter_span=1;
        read=(EffectInvocationLocalRead){c,body,0};
        CHECK(effect_invocation_local_type(&read,(XrXirType)EE_CELL)==XR_XIR_BAD_TYPE);
    }
    nodes[1]=saved;read=(EffectInvocationLocalRead){c,body,0};
    CHECK(effect_invocation_local_type(&read,(XrXirType)EE_CELL)==XR_XIR_OK);
    CHECK(attempts==allocations && pc_stats(c).allocated_bytes==before.allocated_bytes &&
        pc_stats(c).live_bytes==before.live_bytes);
}

#include "xir_current_input_callback_cases.h"
#include "xir_final_outcome_cases.h"
static void pc_local_equation_controls(void) {
    XrXirCompileContext c=pc_owner(pc_caps());uint64_t baseline=pc_stats(&c).live_bytes;
    EdgeFixture fixture;pc_empty_fixture(&fixture,0);
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;
    CHECK(xir_fixture_check(&c,&fixture.module,&checked,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_effects_analyze(checked,&effects)==XR_XIR_OK);pc_empty_golden(effects,0);
    EffectInvocationCertificate *certificate=effects->invocations;EffectInvocationLocalInput *inputs=NULL;
    CHECK(certificate && certificate->final && certificate->body->domain.types->count==2 &&
        xr_xir_type_is_cell(certificate->body->domain.types,(XrXirType)EE_CELL));
    CHECK(effect_invocation_local_inputs(&c,certificate->body,&inputs)==XR_XIR_OK && inputs);
    CHECK(inputs[0].facts.requires_root && !inputs[0].facts.unresolved && inputs[0].witnesses[0].cause==XR_XIR_ROOT_CAUSE_INITIALIZER && !inputs[1].facts.requires_root && !inputs[2].facts.requires_root);
    CHECK(effect_invocation_local_equations_match(&c,certificate->final,inputs)==XR_XIR_OK);
    pc_local_type_controls(&c,certificate->body);
    pc_local_final_mutations(&c,certificate,inputs);
    pc_local_source_fields(&c,certificate->body,xr_xir_compile_artifact_module(checked));
    XrXirCompileContext foreign={0};XrCompileResourceLimits caps=pc_caps();
    CHECK(xr_compile_resources_new(&caps,&foreign.resources)==XR_COMPILE_RESOURCE_OK);
    foreign.limits=c.limits;EffectInvocationLocalInput *occupied=inputs,*missing=NULL;
    size_t allocations=attempts;XrCompileResourceStats before=pc_stats(&c);
    CHECK(effect_invocation_local_inputs(&c,certificate->body,&occupied)==XR_XIR_BAD_STRUCTURE && occupied==inputs);
    CHECK(effect_invocation_local_inputs(&foreign,certificate->body,&missing)==XR_XIR_BAD_STRUCTURE && !missing);
    CHECK(effect_invocation_local_inputs(NULL,certificate->body,&missing)==XR_XIR_BAD_STRUCTURE && !missing &&
        attempts==allocations && pc_stats(&c).work==before.work);
    xr_compile_resources_release(foreign.resources);xr_compile_resources_free(inputs);inputs=NULL;
    xr_xir_compile_artifact_free(checked);checked=NULL;memset(&fixture,0xa5,sizeof(fixture));
    CHECK(effect_invocation_local_inputs(&c,certificate->body,&inputs)==XR_XIR_OK && inputs);
    CHECK(effect_invocation_local_equations_match(&c,certificate->final,inputs)==XR_XIR_OK);
    CHECK(effect_invocation_certificate_result_match(&c,certificate,(&certificate->final->summary))==XR_XIR_OK);
    CHECK(effect_invocation_public_gate(&c,certificate)==XR_XIR_OK);pc_empty_golden(effects,0);
    xr_compile_resources_free(inputs);xr_xir_compile_effects_free(effects);
    pc_local_formal_preflight(&c);pc_release(&c,baseline);
    pc_local_transfer_positive();pc_local_callback_boundary();pc_local_release_boundary();
    pc_current_input_callbacks();
    puts("empty transfer: genuine unused Cell pool, initializer/thrower, final counterfacts, raw ten fields and producer death");
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
#include "xir_final_metadata_cases.h"
#include "xir_final_control_proof_cases.h"
#include "xir_source_terminal_callback_cases.h"
#include "xir_error_callee_first_controls.h"
#include "xir_error_unit_cell_controls.h"
#include "xir_error_invocation_controls.h"
#include "xir_error_return_coverage_controls.h"
static void pc_semantics(void) {
    for (unsigned mode=0;mode<EI_CASES;++mode) {
        XrXirCompileContext c=pc_owner(pc_caps());uint64_t baseline=pc_stats(&c).live_bytes;
        CHECK(ei_whole(&c,mode)==XR_XIR_OK);pc_release(&c,baseline);
    }
    for (unsigned mode=0;mode<ER_CASES;++mode) {
        XrXirCompileContext c=pc_owner(pc_caps());uint64_t baseline=pc_stats(&c).live_bytes;
        CHECK(er_whole(&c,mode)==XR_XIR_OK);pc_release(&c,baseline);
    }
    pc_unit_cell_controls();cf_whole_cases();cf_occupied();
    pc_compact_bad_types();
    for (unsigned mode=0;mode<3;++mode) {
        XrXirCompileContext c=pc_owner(pc_caps());uint64_t baseline=pc_stats(&c).live_bytes;
        CHECK(pc_compact_whole(&c,mode)==XR_XIR_OK);pc_release(&c,baseline);
    }
    cc_bad_types();
    for (unsigned mode=0;mode<11;++mode) {
        XrXirCompileContext c=pc_owner(pc_caps());uint64_t baseline=pc_stats(&c).live_bytes;
        CHECK(cc_whole(&c,mode)==XR_XIR_OK);pc_release(&c,baseline);
    }
    pc_empty_cases();pc_local_equation_controls();
    pc_cases();pc_negative();pc_occupied();
}
static void pc_performance(void) {
    cf_whole_cases();
    static const unsigned modes[]={0,3,9,10};
    for (unsigned i=0;i<sizeof(modes)/sizeof(*modes);++i) cc_limits(modes[i]);
}
static void pc_resources(void) {
    for (unsigned mode=0;mode<EI_CASES;++mode) { ei_faults(mode);ei_limits(mode); }
    for (unsigned mode=0;mode<ER_CASES;++mode) { er_faults(mode);er_limits(mode); }
    cf_whole_faults();cf_whole_limits();
    for (unsigned mode=0;mode<2;++mode) { pc_compact_faults(mode);pc_compact_limits(mode); }
    static const unsigned modes[]={0,3,9,10};
    for (unsigned i=0;i<sizeof(modes)/sizeof(*modes);++i) { cc_faults(modes[i]);cc_limits(modes[i]); }
    pc_empty_faults();pc_empty_limits();
    pc_faults(PC_PHI_SELF);pc_faults(PC_LATE_JOIN);
    pc_limits(PC_PHI_SELF);pc_limits(PC_LATE_JOIN);
}
int main(int argc,char **argv) {
    if (argc==2 && !strcmp(argv[1],"--control-proof-literals")) { pc_control_proof_literals();return 0; }
    if (argc==2 && !strcmp(argv[1],"--source-terminal-callback")) { pc_source_terminal_literals();return 0; }
    if (argc==2 && !strcmp(argv[1],"--outcomes-literals")) {
        pc_outcome_late_literals();pc_outcome_owner_literals();pc_outcome_metadata_literals();return 0;
    }
    if (argc==2 && !strcmp(argv[1],"--outcomes-resources")) {
        pc_outcome_resources();return 0;
    }
    if (argc==2 && !strcmp(argv[1],"--outcomes-late-probe")) {
        pc_outcome_late_literals();return 0;
    }

    if (argc!=1) {
        if (argc!=2 || (strcmp(argv[1],"--semantics") && strcmp(argv[1],"--performance") &&
            strcmp(argv[1],"--resources") && strcmp(argv[1],"--current-input-callback") &&
            strcmp(argv[1],"--current-input-resources"))) {
            fputs("usage: test_xir_error_cfg_pending [--semantics|--performance|--resources|--current-input-callback|--current-input-resources]\n",stderr);
            return 2;
        }
        if (!strcmp(argv[1],"--current-input-resources")) pc_current_input_resources();
        else if (!strcmp(argv[1],"--current-input-callback")) pc_current_input_callbacks();
        else if (!strcmp(argv[1],"--semantics")) pc_semantics();
        else if (!strcmp(argv[1],"--performance")) pc_performance();
        else pc_resources();
        CHECK(!live && !live_bytes);puts("typed-error CFG pending selected feedback PASS");return 0;
    }

    pc_invocation_controls();
    pc_return_coverage_controls();
    pc_unit_cell_controls();
    pc_callee_first_controls();
    pc_compact_controls();pc_closed_carriers_controls();
    pc_empty_cases();pc_local_equation_controls();
    pc_empty_faults();pc_empty_limits();
    pc_cases();pc_negative();pc_occupied();
    pc_faults(PC_PHI_SELF);pc_faults(PC_LATE_JOIN);
    pc_limits(PC_PHI_SELF);pc_limits(PC_LATE_JOIN);
    CHECK(!live && !live_bytes);puts("typed-error CFG pending ownership PASS");return 0;
}
