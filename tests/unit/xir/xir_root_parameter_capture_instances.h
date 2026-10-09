#include "xir_construction_fixture.h"
/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_root_parameter_capture_instances.h - Literal latent capture instances
 *
 * KEY CONCEPT:
 *   Constructing a closure is separate from invoking it. Authentic actual
 *   captures retain precision without recovering a fixed Fn8 target's body.
 */
#ifndef XIR_ROOT_PARAMETER_CAPTURE_INSTANCES_H
#define XIR_ROOT_PARAMETER_CAPTURE_INSTANCES_H
typedef struct RootCaptureCase {
    uint32_t actual_flags, closure_flags;
    bool invoke, constant_root, reverse, fixed;
} RootCaptureCase;
static const RootCaptureCase rci_cases[]={
    {2,2,true,false,false,false},{4,4,true,false,false,false},
    {8,8,true,false,false,false},{12,12,true,false,false,false},
    {2,2,false,false,false,false},{4,4,false,false,false,false},
    {8,8,false,false,false,false},{12,12,false,false,false,false},
    {2,4,true,true,false,false},{4,4,true,true,false,false},
    {2,2,true,false,true,false},{4,4,true,false,true,false},
    {8,8,true,false,false,true}
};
typedef struct RootCaptureFixture {
    XrXirTypeNode nodes[4];XrXirTypes types;XrXirType parameter;
    XrXirInstruction entry[4], factory[6], bind[3], pure[2], init[3];
    XrXirBlock entry_block, factory_blocks[3], bind_block, pure_block, init_block;
    uint32_t entry_operand, factory_operand;
    XrXirFunction functions[5];XrXirFunctionIdentity identities[5];
    XrXirSourceModule source_module;XrXirDeclarations declarations;XrXirSlot slot;
    XrXirEffectParameter factory_parameter, bind_parameter;
    XrXirRootTerm factory_term, bind_term;
    XrXirRootValueIdentity entry_values[2], factory_values[2];
    XrXirEffectCallBinding entry_binding, factory_binding;
    XrXirFunctionEffectContract contracts[5];XrXirProvenance evidence;XrXirModule module;
} RootCaptureFixture;

static void rci_fixture(RootCaptureFixture *f, const RootCaptureCase *probe) {
    memset(f,0,sizeof(*f));
    static const uint32_t flags[]={8,2,4,12};uint32_t actual=0;
    for (uint32_t i=0;i<4;++i) {
        f->nodes[i]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_I64,.flags=flags[i]};
        if (flags[i]==probe->actual_flags) actual=256+i;
    }
    CHECK(actual);f->types=(XrXirTypes){.nodes=f->nodes,.count=4};f->parameter=(XrXirType)256;
    f->pure[0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=7};
    f->pure[1]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={0,0}};
    f->pure_block=(XrXirBlock){.count=2};
    f->functions[3]=(XrXirFunction){.name="pure",.name_length=4,.result=XR_XIR_I64,
        .instructions=f->pure,.instruction_count=2,.blocks=&f->pure_block,.block_count=1};
    f->entry[0]=(XrXirInstruction){.op=XR_XIR_FUNCTION_REF,.type=(XrXirType)(probe->fixed?257u:actual),.immediate=3};
    f->entry[1]=(XrXirInstruction){.op=probe->fixed?XR_XIR_FUNCTION_WEAKEN:XR_XIR_COPY,
        .type=(XrXirType)actual,.args={0,0}};
    f->entry[2]=(XrXirInstruction){.op=XR_XIR_CALL,.type=XR_XIR_I64,.args={0,1},.immediate=1};
    f->entry[3]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={2,0}};
    f->entry_block=(XrXirBlock){.count=4};f->entry_operand=1;
    f->functions[0]=(XrXirFunction){.name="run",.name_length=3,.result=XR_XIR_I64,
        .instructions=f->entry,.instruction_count=4,.blocks=&f->entry_block,.block_count=1,
        .operands=&f->entry_operand,.operand_count=1};
    f->entry_values[0]=(XrXirRootValueIdentity){0,XR_XIR_EFFECT_VALUE_AUTHENTIC_REF,f->entry[0].type};
    f->entry_values[1]=(XrXirRootValueIdentity){1,
        probe->fixed?XR_XIR_EFFECT_VALUE_FIXED:XR_XIR_EFFECT_VALUE_CALL_BIND,(XrXirType)256};
    f->entry_binding=(XrXirEffectCallBinding){XR_XIR_EFFECT_BINDING_DIRECT,2,0,1};
    f->contracts[0]=(XrXirFunctionEffectContract){.value_count=2,.values=f->entry_values,
        .binding_count=probe->fixed?0u:1u,.bindings=probe->fixed?NULL:&f->entry_binding,
        .formula={.constant_mask=probe->invoke?(probe->closure_flags&12u):0}};
    f->factory[0]=(XrXirInstruction){.op=XR_XIR_COPY,.type=(XrXirType)256,.args={0,0}};
    f->factory[1]=(XrXirInstruction){.op=XR_XIR_FUNCTION_REF,.type=(XrXirType)256,.args={0,1},.immediate=2};
    f->factory[2]=(XrXirInstruction){.op=probe->invoke?XR_XIR_CALL_INDIRECT:XR_XIR_CONST_INT,
        .type=XR_XIR_I64,.immediate=probe->invoke?2:7};
    f->factory[3]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={3,0}};
    f->factory_blocks[0]=(XrXirBlock){.count=4};f->factory_operand=1;
    f->factory_values[0]=(XrXirRootValueIdentity){0,XR_XIR_EFFECT_VALUE_CALL_BIND,(XrXirType)256};
    f->factory_values[1]=(XrXirRootValueIdentity){1,XR_XIR_EFFECT_VALUE_AUTHENTIC_REF,(XrXirType)256};
    if (probe->reverse) {
        f->factory[0]=(XrXirInstruction){.op=XR_XIR_JUMP,.targets={2,0}};
        f->factory[4]=(XrXirInstruction){.op=XR_XIR_COPY,.type=(XrXirType)256,.args={0,0}};
        f->factory[5]=(XrXirInstruction){.op=XR_XIR_JUMP,.targets={1,0}};
        f->factory_blocks[0]=(XrXirBlock){.count=1};
        f->factory_blocks[1]=(XrXirBlock){.first=1,.count=3};
        f->factory_blocks[2]=(XrXirBlock){.first=4,.count=2};f->factory_operand=5;
        f->factory_values[0]=(XrXirRootValueIdentity){1,XR_XIR_EFFECT_VALUE_AUTHENTIC_REF,(XrXirType)256};
        f->factory_values[1]=(XrXirRootValueIdentity){4,XR_XIR_EFFECT_VALUE_CALL_BIND,(XrXirType)256};
    }
    f->functions[1]=(XrXirFunction){.name="factory",.name_length=7,.parameters=&f->parameter,.parameter_count=1,
        .result=XR_XIR_I64,.instructions=f->factory,.instruction_count=probe->reverse?6u:4u,
        .blocks=f->factory_blocks,.block_count=probe->reverse?3u:1u,
        .operands=&f->factory_operand,.operand_count=1};
    f->factory_parameter=(XrXirEffectParameter){XR_XIR_EFFECT_PARAMETER_VARIABLE,
        XR_XIR_EFFECT_USE_COPY|XR_XIR_EFFECT_USE_CONST_CAPTURE|XR_XIR_EFFECT_USE_INVOKE};
    f->factory_term=(XrXirRootTerm){XR_XIR_ROOT_TERM_PARAMETER,0};
    f->factory_binding=(XrXirEffectCallBinding){XR_XIR_EFFECT_BINDING_CAPTURE,1,0,f->factory_operand};
    f->contracts[1]=(XrXirFunctionEffectContract){.parameter_count=1,.parameters=&f->factory_parameter,
        .value_count=2,.values=f->factory_values,.binding_count=1,.bindings=&f->factory_binding,
        .formula={.constant_mask=probe->invoke&&probe->constant_root?4u:0u,
            .term_count=probe->invoke?1u:0u,.terms=probe->invoke?&f->factory_term:NULL}};
    f->bind[0]=(XrXirInstruction){.op=XR_XIR_CALL_INDIRECT,.type=XR_XIR_I64,.immediate=0};
    f->bind[1]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={1,0}};
    f->bind_block=(XrXirBlock){.count=2};
    if (probe->constant_root) {
        f->bind[0]=(XrXirInstruction){.op=XR_XIR_SLOT_LOAD,.type=XR_XIR_I64};
        f->bind[1]=(XrXirInstruction){.op=XR_XIR_CALL_INDIRECT,.type=XR_XIR_I64,.immediate=0};
        f->bind[2]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={2,0}};f->bind_block.count=3;
    }
    f->functions[2]=(XrXirFunction){.name="bind",.name_length=4,.parameters=&f->parameter,.parameter_count=1,
        .result=XR_XIR_I64,.instructions=f->bind,.instruction_count=f->bind_block.count,
        .blocks=&f->bind_block,.block_count=1};
    f->bind_parameter=(XrXirEffectParameter){XR_XIR_EFFECT_PARAMETER_VARIABLE,XR_XIR_EFFECT_USE_INVOKE};
    f->bind_term=(XrXirRootTerm){XR_XIR_ROOT_TERM_PARAMETER,0};
    f->contracts[2]=(XrXirFunctionEffectContract){.parameter_count=1,.parameters=&f->bind_parameter,
        .formula={.constant_mask=probe->constant_root?4u:0u,.term_count=1,.terms=&f->bind_term}};
    f->init[0]=(XrXirInstruction){.op=XR_XIR_RETURN};f->init_block=(XrXirBlock){.count=1};
    if (probe->constant_root) {
        f->init[0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=9};
        f->init[1]=(XrXirInstruction){.op=XR_XIR_SLOT_INIT,.args={0,0}};
        f->init[2]=(XrXirInstruction){.op=XR_XIR_RETURN};f->init_block.count=3;
        f->slot=(XrXirSlot){0,XR_XIR_I64,1};f->declarations.slots=&f->slot;f->declarations.slot_count=1;
    }
    f->functions[4]=(XrXirFunction){.name="init",.name_length=4,.result=XR_XIR_UNIT,
        .instructions=f->init,.instruction_count=f->init_block.count,.blocks=&f->init_block,.block_count=1};
    f->source_module=(XrXirSourceModule){"capture",7,NULL,0,4};
    f->declarations.modules=&f->source_module;f->declarations.module_count=1;
    f->declarations.functions=f->identities;f->declarations.entry_function=0;
    f->contracts[4].formula.constant_mask=4;
    f->evidence=(XrXirProvenance){.kind=XR_XIR_EVIDENCE_TEMPLATE,.contracts=f->contracts,.contract_count=5};
    f->module=(XrXirModule){.stage=XR_XIR_BUILT,.functions=f->functions,.function_count=5,
        .declarations=&f->declarations,.types=&f->types,.linkage_kind=XR_XIR_PROGRAM,.provenance=&f->evidence};
}

static void rci_shape(const XrXirArtifact *artifact, const RootCaptureCase *probe) {
    const XrXirModule *m=xr_xir_compile_artifact_module(artifact);
    CHECK(m && m->stage==XR_XIR_CHECKED && m->provenance && m->provenance->kind==XR_XIR_EVIDENCE_INSTANCE);
    const XrXirProvenance *p=m->provenance;const XrXirModule *source=xr_xir_compile_artifact_module(p->source);
    CHECK(source && source->function_count==5 && p->count==m->function_count);
    const XrXirInstruction *entry=&m->functions[0].instructions[2];
    CHECK(entry->op==XR_XIR_CALL && entry->immediate>=0 && (uint64_t)entry->immediate<m->function_count);
    uint32_t factory=(uint32_t)entry->immediate;
    CHECK(p->origins[factory].function==1 && p->origins[factory].effect_argument_count==1);
    const XrXirTypeNode *argument=xr_xir_callable_signature(m->types,m->functions[factory].parameters[0]);
    CHECK(argument && argument->flags==probe->actual_flags);
    const XrXirInstruction *closure=&m->functions[factory].instructions[1];
    CHECK(closure->op==XR_XIR_FUNCTION_REF && closure->immediate>=0 && (uint64_t)closure->immediate<m->function_count);
    const XrXirTypeNode *signature=xr_xir_callable_signature(m->types,closure->type);
    CHECK(signature && signature->flags==probe->closure_flags && !signature->parameter_count && signature->result==XR_XIR_I64);
    CHECK(p->origins[(uint32_t)closure->immediate].function==2);
    bool proof=false;
    for (uint32_t b=0;b<p->binding_count;++b) {
        const XrXirEffectBindingProof *binding=&p->bindings[b];
        if (binding->caller==factory && binding->instruction==1) {
            CHECK(binding->family==XR_XIR_EFFECT_BINDING_CAPTURE && binding->parameter==0 &&
                binding->callee==(uint32_t)closure->immediate && binding->actual_value==(probe->reverse?5u:1u));
            proof=true;
        }
    }
    CHECK(proof);
    XrXirEffects *effects=NULL;
    CHECK(xr_xir_compile_effects_analyze(artifact,&effects)==XR_XIR_OK && effects);
    const XrXirRootEffects *facts=xr_xir_effects_root(effects,factory);
    CHECK(facts && facts->requires_root==(probe->invoke && !!(probe->closure_flags&4)) &&
        facts->unresolved==(probe->invoke && !!(probe->closure_flags&8)));
    xr_xir_compile_effects_free(effects);
}

static XrXirStatus rci_pipeline(const XrXirCompileContext *c,
    const RootCaptureCase *probe, XrXirArtifact **output, bool qualify) {
    RootCaptureFixture f;rci_fixture(&f,probe);
    XrXirArtifact *checked=NULL,*read=NULL,*instance=NULL,*reloaded=NULL,*lowered=NULL;
    XrXirCheckedPacket first={0},second={0};XrXirDiagnostic d={0};
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    XrXirStatus status=xir_fixture_check(c, &f.module, &checked, &d);
    if (status==XR_XIR_OK) status=xr_xir_compile_checked_write(checked,&first,&d);
    if (status==XR_XIR_OK) status=xr_xir_compile_checked_read(c,first.bytes,first.length,&read,&d);
    if (status==XR_XIR_OK) status=xr_xir_compile_specialize(read,&instance,&d);
    if (status==XR_XIR_OK) status=xr_xir_compile_checked_write(instance,&second,&d);
    if (status==XR_XIR_OK) status=xr_xir_compile_checked_read(c,second.bytes,second.length,&reloaded,&d);
    if (status==XR_XIR_OK && qualify) rci_shape(reloaded,probe);
    if (status==XR_XIR_OK) status=xr_xir_compile_lower(reloaded,&target,&lowered,&d);
    if (status==XR_XIR_OK) status=xr_xir_compile_artifact_verify(lowered,&d);
    if (status==XR_XIR_OK) { *output=reloaded;reloaded=NULL; }
    xr_xir_compile_artifact_free(lowered);xr_xir_compile_artifact_free(reloaded);
    xr_xir_compile_artifact_free(instance);xr_xir_compile_artifact_free(read);xr_xir_compile_artifact_free(checked);
    xr_xir_compile_checked_packet_free(&first);xr_xir_compile_checked_packet_free(&second);
    return status;
}

static void rci_literals(void) {
    for (size_t i=0;i<sizeof(rci_cases)/sizeof(rci_cases[0]);++i) {
        RootParameterMark physical=rp_mark();XrXirCompileContext c=rp_owner(rp_caps());
        uint64_t live=rp_stats(&c).live_bytes;XrXirArtifact *result=NULL;
        XrXirStatus status=rci_pipeline(&c,&rci_cases[i],&result,true);
        if (status!=XR_XIR_OK) fprintf(stderr,"capture case%zu status%u\n",i,(unsigned)status);
        CHECK(status==XR_XIR_OK && result);xr_xir_compile_artifact_free(result);
        rp_owner_free(&c,live);rp_balanced(physical);
    }
}

static void rci_resources(void) {
    const RootCaptureCase *probe=&rci_cases[10];
    RootParameterMark physical=rp_mark();XrXirCompileContext normal=rp_owner(rp_caps());
    uint64_t live=rp_stats(&normal).live_bytes;XrXirArtifact *result=NULL;rp_attempts=0;
    CHECK(rci_pipeline(&normal,probe,&result,false)==XR_XIR_OK && result);
    size_t sites=rp_attempts;CHECK(sites);XrCompileResourceStats required=rp_stats(&normal);
    xr_xir_compile_artifact_free(result);rp_owner_free(&normal,live);rp_balanced(physical);
    for (size_t fault=0;fault<sites;++fault) {
        XrXirCompileContext c=rp_owner(rp_caps());live=rp_stats(&c).live_bytes;RootParameterMark mark=rp_mark();
        result=NULL;rp_attempts=0;rp_fail_at=fault;rp_injected=false;
        CHECK(rci_pipeline(&c,probe,&result,false)==XR_XIR_OUT_OF_MEMORY && !result && rp_injected);
        rp_balanced(mark);XrCompileResourceStats failed=rp_stats(&c);rp_fail_at=SIZE_MAX;
        CHECK(rci_pipeline(&c,probe,&result,false)==XR_XIR_OK && result);
        CHECK(rp_stats(&c).work>=failed.work && rp_stats(&c).allocated_bytes>=failed.allocated_bytes);
        xr_xir_compile_artifact_free(result);rp_balanced(mark);rp_owner_free(&c,live);rp_balanced(physical);
    }
    for (unsigned axis=0;axis<3;++axis) for (unsigned shortfall=0;shortfall<2;++shortfall) {
        XrCompileResourceLimits caps=rp_caps();
        if (axis==0) caps.allocated_bytes=required.allocated_bytes-shortfall;
        if (axis==1) caps.live_bytes=required.peak_bytes-shortfall;
        if (axis==2) caps.work=required.work-shortfall;
        XrXirCompileContext c=rp_owner(caps);live=rp_stats(&c).live_bytes;result=NULL;
        CHECK(rci_pipeline(&c,probe,&result,false)==(shortfall?XR_XIR_BUDGET:XR_XIR_OK));
        CHECK(shortfall?!result:result!=NULL);xr_xir_compile_artifact_free(result);
        rp_owner_free(&c,live);rp_balanced(physical);
    }
    printf("capture Check/write/read/specialize/Lowered actual OOM sites=%zu\n",sites);
}

/* The input remains immutable. Complete public rechecking independently
 * derives the latent bound and authenticates the actual capture operand. */
static void rci_forged(void) {
    RootParameterMark physical=rp_mark();XrXirCompileContext c=rp_owner(rp_caps());
    uint64_t live=rp_stats(&c).live_bytes;XrXirArtifact *instance=NULL;
    CHECK(rci_pipeline(&c,&rci_cases[0],&instance,false)==XR_XIR_OK && instance);
    const XrXirModule *m=xr_xir_compile_artifact_module(instance);
    CHECK(m->function_count<=32 && m->types->count<64);
    uint32_t factory=(uint32_t)m->functions[0].instructions[2].immediate;
    CHECK(factory<m->function_count && m->functions[factory].instruction_count<=128);
    for (unsigned bad=0;bad<3;++bad) {
        RootParameterMark mark=rp_mark();
        XrXirModule input=*m;XrXirTypes types=*m->types;
        XrXirFunction functions[32];XrXirInstruction instructions[128];XrXirTypeNode nodes[64];
        memcpy(functions,m->functions,m->function_count*sizeof(*functions));
        memcpy(instructions,m->functions[factory].instructions,m->functions[factory].instruction_count*sizeof(*instructions));
        memcpy(nodes,m->types->nodes,m->types->count*sizeof(*nodes));
        const XrXirTypeNode *original=xr_xir_callable_signature(m->types,instructions[1].type);
        CHECK(original && original->flags==2);
        CHECK(original->parameter_count==0 && !original->parameters &&
            original->element==XR_XIR_UNIT && original->nominal.argument_count==0);
        XrXirTypeNode forged=*original;
        if (bad==0) forged.flags=4;
        if (bad==1) forged.flags=3;
        if (bad==2) forged.result=XR_XIR_BOOL;
        uint32_t replacement=types.count;
        for (uint32_t t=0;t<types.count;++t) {
            const XrXirTypeNode *node=&nodes[t];
            if (node->kind==forged.kind && node->element==forged.element &&
                node->parameter_count==0 && node->result==forged.result &&
                node->flags==forged.flags && node->parameter_span==forged.parameter_span)
                replacement=t;
        }
        CHECK(bad!=0 || replacement<m->types->count);
        if (replacement==types.count) nodes[types.count++]=forged;
        instructions[1].type=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+replacement);
        types.nodes=nodes;functions[factory].instructions=instructions;
        input.functions=functions;input.types=&types;
        XrXirArtifact *checked=NULL;XrXirDiagnostic d={0};
        CHECK(xir_fixture_recheck(&c, &input, &checked, &d)==XR_XIR_BAD_TYPE && !checked);
        CHECK(instructions[1].type==(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+replacement));
        rp_balanced(mark);
    }
    XrXirProvenance *proof=NULL;
    CHECK(xr_xir_compile_provenance_copy(&c,m->provenance,&proof)==XR_XIR_OK && proof);
    bool forged=false;
    for (uint32_t b=0;b<proof->binding_count;++b) {
        XrXirEffectBindingProof *binding=&proof->bindings[b];
        if (binding->caller==factory && binding->instruction==1 && binding->family==XR_XIR_EFFECT_BINDING_CAPTURE) {
            CHECK(binding->actual_value==1);binding->actual_value=0;forged=true;
        }
    }
    CHECK(forged);XrXirModule input=*m;input.provenance=proof;
    XrXirArtifact *checked=NULL;XrXirDiagnostic d={0};RootParameterMark mark=rp_mark();
    CHECK(xir_fixture_recheck(&c, &input, &checked, &d)==XR_XIR_BAD_STRUCTURE && !checked);
    rp_balanced(mark);xr_xir_compile_provenance_free(proof);
    xr_xir_compile_artifact_free(instance);rp_owner_free(&c,live);rp_balanced(physical);
}

static void rci_query_rejections(void) {
    RootParameterMark physical=rp_mark();XrXirCompileContext c=rp_owner(rp_caps());
    uint64_t live=rp_stats(&c).live_bytes;XrXirArtifact *instance=NULL;
    CHECK(rci_pipeline(&c,&rci_cases[0],&instance,false)==XR_XIR_OK && instance);
    const XrXirModule *m=xr_xir_compile_artifact_module(instance);const XrXirProvenance *p=m->provenance;
    const XrXirModule *source=xr_xir_compile_artifact_module(p->source);
    uint32_t factory=(uint32_t)m->functions[0].instructions[2].immediate;
    uint32_t target=(uint32_t)m->functions[factory].instructions[1].immediate;
    XrXirEffects *effects=NULL;
    CHECK(xr_xir_compile_effects_analyze(p->source,&effects)==XR_XIR_OK && effects);
    XirEffectCaptureRequest request={source,m->types,&m->functions[factory],effects,
        &p->origins[factory],&p->origins[target],1,NULL,0};
    XirEffectCaptureBound result={UINT32_MAX,true};
    CHECK(xir_effect_capture_bound(&c,&request,&result)==XR_XIR_OK && result.refined && result.flags==2);
    for (unsigned bad=0;bad<2;++bad) {
        XrXirOrigin wrong=*request.target;XirEffectCaptureRequest input=request;input.target=&wrong;
        if (bad==0) wrong.function=3;
        if (bad==1) wrong.effect_argument_count=2;
        result=(XirEffectCaptureBound){UINT32_MAX,true};RootParameterMark mark=rp_mark();
        CHECK(xir_effect_capture_bound(&c,&input,&result)==XR_XIR_BAD_STRUCTURE);
        CHECK(result.flags==UINT32_MAX && result.refined);rp_balanced(mark);
    }
    XrXirCompileContext foreign=rp_owner(rp_caps());uint64_t foreign_live=rp_stats(&foreign).live_bytes;
    RootParameterMark mark=rp_mark();XrCompileResourceStats before=rp_stats(&foreign);
    result=(XirEffectCaptureBound){UINT32_MAX,true};size_t attempts=rp_attempts;
    CHECK(xir_effect_capture_bound(&foreign,&request,&result)==XR_XIR_BAD_STRUCTURE);
    XrCompileResourceStats after=rp_stats(&foreign);
    CHECK(result.flags==UINT32_MAX && result.refined && rp_attempts==attempts &&
        before.work==after.work && before.allocated_bytes==after.allocated_bytes &&
        before.live_bytes==after.live_bytes);
    rp_balanced(mark);rp_owner_free(&foreign,foreign_live);
    XrXirModule wrong_source=*source;--wrong_source.function_count;
    XirEffectCaptureRequest input=request;input.source=&wrong_source;
    before=rp_stats(&c);mark=rp_mark();attempts=rp_attempts;
    result=(XirEffectCaptureBound){UINT32_MAX,true};
    CHECK(xir_effect_capture_bound(&c,&input,&result)==XR_XIR_BAD_STRUCTURE);
    after=rp_stats(&c);
    CHECK(result.flags==UINT32_MAX && result.refined && before.work==after.work &&
        before.allocated_bytes==after.allocated_bytes && rp_attempts==attempts);rp_balanced(mark);
    before=rp_stats(&c);XrCompileResourceLimits caps=rp_caps();
    CHECK(caps.work>before.work+11);
    CHECK(xr_compile_resources_work(c.resources,caps.work-before.work-11)==XR_COMPILE_RESOURCE_OK);
    before=rp_stats(&c);mark=rp_mark();attempts=rp_attempts;
    for (unsigned retry=0;retry<2;++retry) {
        result=(XirEffectCaptureBound){UINT32_MAX,true};
        CHECK(xir_effect_capture_bound(&c,&request,&result)==XR_XIR_BUDGET);
        after=rp_stats(&c);
        CHECK(result.flags==UINT32_MAX && result.refined && before.work==after.work &&
            before.allocated_bytes==after.allocated_bytes && rp_attempts==attempts);
    }
    rp_balanced(mark);xr_xir_compile_effects_free(effects);xr_xir_compile_artifact_free(instance);
    rp_owner_free(&c,live);rp_balanced(physical);
}

static void rci_cases_run(void) { rci_literals();rci_forged();rci_query_rejections();rci_resources(); }
#endif // XIR_ROOT_PARAMETER_CAPTURE_INSTANCES_H
