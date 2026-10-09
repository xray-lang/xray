/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_root_parameter_capture_index.c - Ordered latent dependency convergence
 *
 * KEY CONCEPT:
 *   Reference dependencies wake their callers without executing captured bodies.
 */
#include "xir_construction_fixture.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
#include "xir_root_parameter_fixture.h"
#include "xir/xxir_effects.c"

typedef struct CaptureIndexFixture {
    XrXirTypeNode nodes[2];XrXirTypes types;XrXirType parameter;
    XrXirInstruction run[2],early_a[4],early_b[3],late[3],factory[3],reader[2],init[3];
    XrXirBlock blocks[7];XrXirFunction functions[7];XrXirFunctionIdentity identities[7];
    uint32_t operands[2];XrXirSlot slot;XrXirSourceModule source;XrXirDeclarations declarations;
    XrXirEffectParameter parameters[3];XrXirRootTerm terms[3];
    XrXirRootValueIdentity values[5];XrXirFunctionEffectContract contracts[7];
    XrXirProvenance evidence;XrXirModule module;
} CaptureIndexFixture;

static void capture_index_fixture(CaptureIndexFixture *f) {
    memset(f,0,sizeof(*f));f->parameter=(XrXirType)256;
    f->nodes[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_I64,.flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED};
    f->nodes[1]=f->nodes[0];f->nodes[1].flags=XR_XIR_CALLABLE_ROOT_REQUIRED;
    f->types=(XrXirTypes){.nodes=f->nodes,.count=2};
    f->run[0]=(XrXirInstruction){.op=XR_XIR_CALL,.type=XR_XIR_I64,.immediate=4};
    f->run[1]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={0,0}};
    f->early_a[0]=(XrXirInstruction){.op=XR_XIR_FUNCTION_REF,.type=(XrXirType)256,.args={0,1},.immediate=3};
    f->early_a[1]=f->early_a[0];f->early_a[1].args[0]=1;
    f->early_a[2]=(XrXirInstruction){.op=XR_XIR_CALL_INDIRECT,.type=XR_XIR_I64,.immediate=2};
    f->early_a[3]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={3,0}};
    f->early_b[0]=f->early_a[0];
    f->early_b[1]=(XrXirInstruction){.op=XR_XIR_CALL_INDIRECT,.type=XR_XIR_I64,.immediate=1};
    f->early_b[2]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={2,0}};
    f->late[0]=f->early_a[0];f->late[0].immediate=1;
    f->late[1]=(XrXirInstruction){.op=XR_XIR_CALL_INDIRECT,.type=XR_XIR_I64,.immediate=0};
    f->late[2]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={2,0}};
    f->factory[0]=(XrXirInstruction){.op=XR_XIR_FUNCTION_REF,.type=(XrXirType)257,.immediate=5};
    f->factory[1]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=37};
    f->factory[2]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={1,0}};
    f->reader[0]=(XrXirInstruction){.op=XR_XIR_SLOT_LOAD,.type=XR_XIR_I64};
    f->reader[1]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={0,0}};
    f->init[0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=9};
    f->init[1]=(XrXirInstruction){.op=XR_XIR_SLOT_INIT,.args={0,0}};
    f->init[2]=(XrXirInstruction){.op=XR_XIR_RETURN};
    const char *names[]={"run","earlyA","earlyB","late","factory","reader","init"};
    const uint32_t lengths[]={3,6,6,4,7,6,4},counts[]={2,4,3,3,3,2,3};
    XrXirInstruction *ops[]={f->run,f->early_a,f->early_b,f->late,f->factory,f->reader,f->init};
    for (uint32_t i=0;i<7;++i) {
        f->blocks[i]=(XrXirBlock){.count=counts[i]};
        f->functions[i]=(XrXirFunction){.name=names[i],.name_length=lengths[i],.result=i==6?XR_XIR_UNIT:XR_XIR_I64,
            .instructions=ops[i],.instruction_count=counts[i],.blocks=&f->blocks[i],.block_count=1};
        if (i>=1 && i<=3) {
            f->functions[i].parameters=&f->parameter;f->functions[i].parameter_count=1;
            f->functions[i].operands=f->operands;f->functions[i].operand_count=i==1?2u:1u;
            f->parameters[i-1]=(XrXirEffectParameter){XR_XIR_EFFECT_PARAMETER_VARIABLE,
                XR_XIR_EFFECT_USE_INVOKE|XR_XIR_EFFECT_USE_CONST_CAPTURE};
            f->terms[i-1]=(XrXirRootTerm){XR_XIR_ROOT_TERM_PARAMETER,0};
            f->contracts[i]=(XrXirFunctionEffectContract){.parameter_count=1,.parameters=&f->parameters[i-1],
                .formula={.term_count=1,.terms=&f->terms[i-1]}};
        }
    }
    f->values[0]=(XrXirRootValueIdentity){0,XR_XIR_EFFECT_VALUE_AUTHENTIC_REF,(XrXirType)256};
    f->values[1]=(XrXirRootValueIdentity){1,XR_XIR_EFFECT_VALUE_AUTHENTIC_REF,(XrXirType)256};
    f->values[2]=f->values[0];f->values[3]=f->values[0];
    f->values[4]=(XrXirRootValueIdentity){0,XR_XIR_EFFECT_VALUE_AUTHENTIC_REF,(XrXirType)257};
    f->contracts[1].values=f->values;f->contracts[1].value_count=2;
    for (uint32_t i=2;i<=4;++i) { f->contracts[i].values=&f->values[i];f->contracts[i].value_count=1; }
    f->contracts[5].formula.constant_mask=XR_XIR_CALLABLE_ROOT_REQUIRED;
    f->contracts[6].formula.constant_mask=XR_XIR_CALLABLE_ROOT_REQUIRED;
    f->slot=(XrXirSlot){0,XR_XIR_I64,1};f->source=(XrXirSourceModule){"capture",7,NULL,0,6};
    f->declarations=(XrXirDeclarations){.modules=&f->source,.module_count=1,.functions=f->identities,
        .slots=&f->slot,.slot_count=1,.entry_function=0};
    f->evidence=(XrXirProvenance){.kind=XR_XIR_EVIDENCE_TEMPLATE,.contracts=f->contracts,.contract_count=7};
    f->module=(XrXirModule){.stage=XR_XIR_BUILT,.functions=f->functions,.function_count=7,
        .types=&f->types,.declarations=&f->declarations,.provenance=&f->evidence,.linkage_kind=XR_XIR_PROGRAM};
}

static XrXirStatus capture_index_order(const XrXirCompileContext *c, const XrXirModule *m, bool qualify) {
    EffectGraph graph={0};XrXirCompileContext context=*c;
    XrXirStatus status=effect_reference_graph(m,&graph,5,&context);
    if (status==XR_XIR_OK && qualify) {
        static const uint32_t callers[]={1,1,2};uint32_t edge=graph.reference_heads[3];
        for (uint32_t i=0;i<3;++i) {
            CHECK(edge!=UINT32_MAX && graph.references[edge].caller==callers[i]);edge=graph.references[edge].next;
        }
        CHECK(edge==UINT32_MAX);
        edge=graph.reference_heads[1];CHECK(edge!=UINT32_MAX && graph.references[edge].caller==3);
        CHECK(graph.references[edge].next==UINT32_MAX);
        edge=graph.reference_heads[5];CHECK(edge!=UINT32_MAX && graph.references[edge].caller==4);
        CHECK(graph.references[edge].next==UINT32_MAX);
        CHECK(graph.reference_heads[0]==UINT32_MAX && graph.reference_heads[2]==UINT32_MAX &&
            graph.reference_heads[4]==UINT32_MAX && graph.reference_heads[6]==UINT32_MAX);
    }
    effect_graph_free(&graph);return status;
}

static void capture_index_facts(const XrXirEffects *effects, bool contracts) {
    for (uint32_t f=0;f<7;++f) {
        const XrXirRootEffects *root=xr_xir_effects_root(effects,f);
        CHECK(root && root->requires_root==(f>=5) && root->unresolved==(f>=1 && f<=3));
        if (!contracts) continue;
        const XrXirFunctionEffectContract *contract=xir_effects_contract(effects,f);CHECK(contract);
        if (f>=1 && f<=3) {
            CHECK(contract->parameter_count==1 && contract->parameters[0].kind==XR_XIR_EFFECT_PARAMETER_VARIABLE);
            CHECK(contract->parameters[0].uses==(XR_XIR_EFFECT_USE_INVOKE|XR_XIR_EFFECT_USE_CONST_CAPTURE));
            CHECK(!contract->formula.constant_mask && contract->formula.term_count==1 &&
                contract->formula.terms[0].kind==XR_XIR_ROOT_TERM_PARAMETER && contract->formula.terms[0].index==0);
        } else CHECK(!contract->formula.term_count && contract->formula.constant_mask==(f>=5?4u:0u));
    }
}

static XrXirStatus capture_index_pipeline(const XrXirCompileContext *c, XrXirArtifact **output, bool qualify) {
    CaptureIndexFixture f;capture_index_fixture(&f);XrXirDiagnostic d={0};
    XrXirArtifact *checked=NULL,*read=NULL,*instance=NULL,*lowered=NULL;XrXirEffects *effects=NULL;
    XrXirCheckedPacket packet={0};const char *phase="check";
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    XrXirStatus status=xir_fixture_check(c, &f.module, &checked, &d);memset(&f,0x95,sizeof(f));
    if (status==XR_XIR_OK) { phase="write";status=xr_xir_compile_checked_write(checked,&packet,&d); }
    xr_xir_compile_artifact_free(checked);
    if (status==XR_XIR_OK) { phase="read";status=xr_xir_compile_checked_read(c,packet.bytes,packet.length,&read,&d); }
    xr_xir_compile_checked_packet_free(&packet);
    if (status==XR_XIR_OK) { phase="index-order";status=capture_index_order(c,&read->module,qualify); }
    if (status==XR_XIR_OK) { phase="effects";status=xr_xir_compile_effects_analyze(read,&effects); }
    if (status==XR_XIR_OK && qualify) capture_index_facts(effects,true);
    xr_xir_compile_effects_free(effects);effects=NULL;
    if (status==XR_XIR_OK) { phase="specialize";status=xr_xir_compile_specialize(read,&instance,&d); }
    xr_xir_compile_artifact_free(read);
    if (status==XR_XIR_OK) { phase="lower";status=xr_xir_compile_lower(instance,&target,&lowered,&d); }
    xr_xir_compile_artifact_free(instance);
    if (status==XR_XIR_OK) { phase="verify-lowered";status=xr_xir_compile_artifact_verify(lowered,&d); }
    if (status==XR_XIR_OK) { phase="lowered-effects";status=xr_xir_compile_effects_analyze(lowered,&effects); }
    if (status==XR_XIR_OK && qualify) capture_index_facts(effects,false);
    xr_xir_compile_effects_free(effects);
    if (status==XR_XIR_OK) { *output=lowered;lowered=NULL; }
    xr_xir_compile_artifact_free(lowered);
    if (qualify && status!=XR_XIR_OK) fprintf(stderr,"capture index phase%s status%u f%u b%u i%u\n",
        phase,status,d.function,d.block,d.instruction);
    return status;
}

static void capture_index_resources(void) {
    RootParameterMark physical=rp_mark();XrXirCompileContext normal=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&normal).live_bytes;XrXirArtifact *artifact=NULL;rp_attempts=0;
    CHECK(capture_index_pipeline(&normal,&artifact,false)==XR_XIR_OK && artifact);
    size_t sites=rp_attempts;CHECK(sites);XrCompileResourceStats required=rp_stats(&normal);
    xr_xir_compile_artifact_free(artifact);rp_owner_free(&normal,baseline);rp_balanced(physical);
    for (size_t fault=0;fault<sites;++fault) {
        XrXirCompileContext c=rp_owner(rp_caps());baseline=rp_stats(&c).live_bytes;
        RootParameterMark mark=rp_mark();rp_attempts=0;rp_fail_at=fault;rp_injected=false;artifact=NULL;
        CHECK(capture_index_pipeline(&c,&artifact,false)==XR_XIR_OUT_OF_MEMORY && rp_injected && !artifact);
        rp_balanced(mark);XrCompileResourceStats failed=rp_stats(&c);rp_fail_at=SIZE_MAX;
        CHECK(capture_index_pipeline(&c,&artifact,false)==XR_XIR_OK && artifact);
        CHECK(rp_stats(&c).work>=failed.work && rp_stats(&c).allocated_bytes>=failed.allocated_bytes);
        xr_xir_compile_artifact_free(artifact);rp_balanced(mark);
        rp_owner_free(&c,baseline);rp_balanced(physical);
    }
    for (unsigned axis=0;axis<3;++axis) for (unsigned shortfall=0;shortfall<2;++shortfall) {
        XrCompileResourceLimits caps=rp_caps();
        if (axis==0) caps.allocated_bytes=required.allocated_bytes-shortfall;
        if (axis==1) caps.live_bytes=required.peak_bytes-shortfall;
        if (axis==2) caps.work=required.work-shortfall;
        XrXirCompileContext c=rp_owner(caps);baseline=rp_stats(&c).live_bytes;artifact=NULL;
        CHECK(capture_index_pipeline(&c,&artifact,false)==(shortfall?XR_XIR_BUDGET:XR_XIR_OK));
        CHECK(shortfall?!artifact:artifact!=NULL);xr_xir_compile_artifact_free(artifact);
        rp_owner_free(&c,baseline);rp_balanced(physical);
    }
    printf("capture index complete pipeline OOM sites=%zu allocated=%llu peak=%llu work=%llu\n",sites,
        (unsigned long long)required.allocated_bytes,(unsigned long long)required.peak_bytes,(unsigned long long)required.work);
}

int main(void) {
    RootParameterMark physical=rp_mark();XrXirCompileContext c=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&c).live_bytes;XrXirArtifact *artifact=NULL;
    CHECK(capture_index_pipeline(&c,&artifact,true)==XR_XIR_OK && artifact);
    xr_xir_compile_artifact_free(artifact);rp_owner_free(&c,baseline);rp_balanced(physical);
    capture_index_resources();CHECK(!rp_live && !rp_live_bytes);return 0;
}
