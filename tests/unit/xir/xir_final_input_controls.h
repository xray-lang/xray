/* Current Control/ROOT fixtures share the parent's real compiler ledger and
 * physical allocation tracker. No execution policy is attached to these reads. */
#ifndef XIR_FINAL_INPUT_CONTROLS_H
#define XIR_FINAL_INPUT_CONTROLS_H
#include "xir_error_cfg_pending_fixture.h"
#include "xir_effect_input_fixture.h"
#include "xir_final_closed_carrier_fixture.h"

static size_t pc_final_physical_size(const void *memory) {
    for (size_t i=0;i<rp_live;++i)
        if (rp_blocks[i].pointer==memory) return rp_blocks[i].bytes;
    CHECK(false);return 0;
}
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
    CHECK(facts->functions[2].throws==XR_XIR_EFFECT_MAY);
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
    CHECK(!(xr_xir_effects_read(effects,0).status==XR_XIR_OK) && !(xr_xir_effects_read(effects,0).status==XR_XIR_OK && xr_xir_effects_read(effects,0).suspend_witness.cause!=0) &&
        xr_xir_effects_go_safe(effects,0)==XR_XIR_BAD_STRUCTURE &&
        xr_xir_effects_task_creation(effects,0)==XR_XIR_EFFECT_UNKNOWN);
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
        XrXirCompileContext c=rp_owner(rp_caps());c.limits=xr_xir_compile_default_limits();uint64_t baseline=rp_stats(&c).held_bytes;
        CHECK(pc_empty_whole(&c,mode)==XR_XIR_OK);rp_owner_free(&c,baseline);CHECK(!rp_live && !rp_live_bytes);
    }
    XrXirCompileContext c=rp_owner(rp_caps());c.limits=xr_xir_compile_default_limits();uint64_t baseline=rp_stats(&c).held_bytes;
    EdgeFixture f;pc_empty_fixture(&f,5);
    f.cleanup[2]=(XrXirInstruction){.op=XR_XIR_THROW,.args={1}};
    XrXirArtifact *checked=NULL;
    CHECK(xir_fixture_check(&c, &f.module, &checked, NULL)==XR_XIR_BAD_TYPE && !checked);
    rp_owner_free(&c,baseline);CHECK(!rp_live && !rp_live_bytes);
    puts("empty whole literals: wide/reverse/PHI/filter/Error return/cleanup/panic/suspend and badcleanup");
}

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
    XrXirCompileContext c=rp_owner(rp_caps());c.limits=xr_xir_compile_default_limits();uint64_t baseline=rp_stats(&c).held_bytes;
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
    xr_xir_compile_effects_free(effects);xr_xir_compile_artifact_free(checked);rp_owner_free(&c,baseline);CHECK(!rp_live && !rp_live_bytes);
}

typedef struct PcLocalCallback { EffectFinalFunctions *receiving;uint32_t calls; } PcLocalCallback;
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
    XrXirCompileContext c=rp_owner(rp_caps());c.limits=xr_xir_compile_default_limits();uint64_t baseline=rp_stats(&c).held_bytes;
    EdgeFixture fixture;pc_empty_fixture(&fixture,0);
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;
    CHECK(xir_fixture_check(&c,&fixture.module,&checked,NULL)==XR_XIR_OK);
    CHECK(effect_infer_construct(&c,(XirEffectInput){xr_xir_compile_artifact_module(checked),xr_xir_compile_artifact_construction(checked),NULL},NULL,&effects)==XR_XIR_OK);pc_empty_constructed(effects,0);
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
    xr_xir_compile_effects_free(effects);xr_xir_compile_artifact_free(checked);rp_owner_free(&c,baseline);CHECK(!rp_live && !rp_live_bytes);
}

static EffectInvocationCertificate *pc_release_target;
static unsigned pc_release_poison_calls;
static void pc_local_release_poison(void *memory) {
    size_t bytes=pc_final_physical_size(memory);
    size_t expected=(size_t)(&pc_release_target->final->summary)->count*sizeof(EffectInvocationLocalInput);
    if (bytes!=sizeof(CompileAllocation)+expected) return;
    const EffectInvocationLocalInput *inputs=(const void *)((CompileAllocation *)memory+1);
    CHECK(inputs[0].facts.requires_root &&
        inputs[0].witnesses[0].cause==XR_XIR_ROOT_CAUSE_INITIALIZER);
    rp_release_observer=NULL;
    ++pc_release_poison_calls;
    pc_erase_initializer_root(pc_release_target,(&pc_release_target->final->summary));
}

static void pc_local_release_boundary(void) {
    XrXirCompileContext c=rp_owner(rp_caps());c.limits=xr_xir_compile_default_limits();uint64_t baseline=rp_stats(&c).held_bytes;
    EdgeFixture fixture;pc_empty_fixture(&fixture,0);
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;
    CHECK(xir_fixture_check(&c,&fixture.module,&checked,NULL)==XR_XIR_OK);
    CHECK(effect_infer_construct(&c,(XirEffectInput){xr_xir_compile_artifact_module(checked),xr_xir_compile_artifact_construction(checked),NULL},NULL,&effects)==XR_XIR_OK);pc_empty_constructed(effects,0);
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
    pc_release_target=certificate;pc_release_poison_calls=0;rp_release_observer=pc_local_release_poison;
    XrXirStatus status=effect_invocation_publication_commit(&c,effects,&certificate);
    rp_release_observer=NULL;pc_release_target=NULL;
    printf("allocator release ROOT pollution: callbacks%u status%u publishedRoot%u\n",
        pc_release_poison_calls,(unsigned)status,(unsigned)effect_roots_read(effects)->root[0].requires_root);
    CHECK(pc_release_poison_calls<=1 && effect_roots_read(effects)->root[0].requires_root);
    if (pc_release_poison_calls)
        CHECK(status==XR_XIR_BAD_STRUCTURE && certificate && effects->invocations==old);
    else
        CHECK(status==XR_XIR_OK && !certificate && effects->invocations!=old);
    pc_empty_constructed(effects,0);effect_invocation_certificate_free(certificate);effect_graph_free(&graph);
    xr_xir_compile_effects_free(effects);xr_xir_compile_artifact_free(checked);rp_owner_free(&c,baseline);CHECK(!rp_live && !rp_live_bytes);
}

static void pc_local_formal_preflight(const XrXirCompileContext *c) {
    XrXirType parameter=XR_XIR_I64;
    XrXirInstruction operation={.op=XR_XIR_RETURN,.args={0}};
    XrXirBlock block={.count=1};
    XrXirFunction function={.name="formal",.name_length=6,.parameters=&parameter,
        .parameter_count=1,.result=XR_XIR_I64,.error_type=XR_XIR_NEVER,.blocks=&block,.block_count=1,
        .instructions=&operation,.instruction_count=1};
    XrXirInstruction initializer={.op=XR_XIR_RETURN};
    XrXirFunction functions[2]={function,{.name="init",.name_length=4,
        .result=XR_XIR_UNIT,.error_type=XR_XIR_NEVER,.blocks=&block,.block_count=1,
        .instructions=&initializer,.instruction_count=1}};
    XrXirFunctionIdentity identities[2]={{.exported=1},{0}};
    XrXirSourceModule module={.name="formal",.name_length=6,.initializer=1};
    XrXirDeclarations declarations={.modules=&module,.module_count=1,
        .functions=identities,.root_module=UINT32_MAX,.entry_function=UINT32_MAX};
    XrXirModule source={.stage=XR_XIR_BUILT,.functions=functions,.declarations=&declarations,
        .function_count=2,.linkage_kind=XR_XIR_LIBRARY};
    XrXirArtifact *checked=NULL;EffectBodyMaterial *body=NULL;
    CHECK(xir_fixture_check(c,&source,&checked,NULL)==XR_XIR_OK && checked);
    EffectInputFacts *facts=NULL;
    CHECK(effect_input_facts_new(c,(XirEffectInput){xr_xir_compile_artifact_module(checked),
        xr_xir_compile_artifact_construction(checked),NULL},&facts)==XR_XIR_OK);
    CHECK(effect_body_material_create(c,xr_xir_compile_artifact_module(checked),facts,&body)==XR_XIR_OK && body);
    effect_input_facts_free(facts);
    EffectInvocationLocalInput *input=NULL;
    XrCompileResourceStats before=rp_stats(c);size_t allocations=rp_attempts;
    size_t saved_failure=rp_fail_at;bool saved_injected=rp_injected;
    rp_fail_at=rp_attempts;rp_injected=false;
    CHECK(effect_invocation_local_inputs(c,body,&input)==XR_XIR_OK && !input && !rp_injected);
    rp_fail_at=saved_failure;rp_injected=saved_injected;
    XrCompileResourceStats after=rp_stats(c);
    CHECK(rp_attempts==allocations && after.allocation_count==before.allocation_count &&
        after.allocated_bytes==before.allocated_bytes && after.held_bytes==before.held_bytes);
    EffectBodyFunction *head=&body->functions[0],saved=*head;
    for (unsigned mode=0;mode<4;++mode) {
        if (!mode) head->parameters=NULL;
        if (mode==1) head->blocks=NULL;
        if (mode==2) head->runs=NULL;
        if (mode==3) head->run_capacity=head->instruction_count+1;
        before=rp_stats(c);allocations=rp_attempts;
        CHECK(effect_invocation_local_inputs(c,body,&input)==XR_XIR_BAD_STRUCTURE && !input &&
            rp_attempts==allocations && rp_stats(c).allocated_bytes==before.allocated_bytes &&
            rp_stats(c).held_bytes==before.held_bytes);
        *head=saved;
        CHECK(effect_invocation_local_inputs(c,body,&input)==XR_XIR_OK && !input);
    }
    xr_xir_compile_artifact_free(checked);memset(functions,0xa5,sizeof(functions));
    memset(identities,0xa5,sizeof(identities));memset(&module,0xa5,sizeof(module));
    memset(&operation,0xa5,sizeof(operation));memset(&initializer,0xa5,sizeof(initializer));
    parameter=XR_XIR_BOOL;
    before=rp_stats(c);allocations=rp_attempts;
    CHECK(effect_invocation_local_inputs(c,body,&input)==XR_XIR_OK && !input &&
        rp_attempts==allocations && rp_stats(c).held_bytes==before.held_bytes &&
        conditional_memory_unchanged(c,before));
    effect_body_material_free(body);
}

static void pc_local_type_controls(const XrXirCompileContext *c,EffectBodyMaterial *body) {
    XrXirTypeNode *nodes=(XrXirTypeNode *)body->domain.types->nodes;
    CHECK(body->domain.types->count==2 && nodes[1].kind==XR_XIR_TYPE_CELL);
    XrXirTypeNode saved=nodes[1];
    XrCompileResourceStats before=rp_stats(c);size_t allocations=rp_attempts;
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
    CHECK(rp_attempts==allocations && rp_stats(c).allocated_bytes==before.allocated_bytes &&
        rp_stats(c).held_bytes==before.held_bytes);
}

#include "xir_current_input_callback_cases.h"
#include "xir_final_outcome_cases.h"
static void pc_local_equation_controls(void) {
    XrXirCompileContext c=rp_owner(rp_caps());c.limits=xr_xir_compile_default_limits();uint64_t baseline=rp_stats(&c).held_bytes;
    EdgeFixture fixture;pc_empty_fixture(&fixture,0);
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;
    CHECK(xir_fixture_check(&c,&fixture.module,&checked,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_effects_analyze(checked,&effects)==XR_XIR_OK);pc_empty_golden(effects,0);
    EffectInvocationCertificate *certificate=effects->invocations;EffectInvocationLocalInput *inputs=NULL;
    CHECK(certificate && certificate->final && certificate->body->domain.types->count==2 &&
        xr_xir_type_is_cell(certificate->body->domain.types,(XrXirType)EE_CELL));
    CHECK(effect_invocation_local_inputs(&c,certificate->body,&inputs)==XR_XIR_OK && inputs);
    CHECK(inputs[0].facts.requires_root && !inputs[0].facts.unresolved &&
        inputs[0].witnesses[0].cause==XR_XIR_ROOT_CAUSE_INITIALIZER &&
        !inputs[1].facts.requires_root && !inputs[2].facts.requires_root);
    CHECK(effect_invocation_local_equations_match(&c,certificate->final,inputs)==XR_XIR_OK);
    pc_local_type_controls(&c,certificate->body);
    pc_local_final_mutations(&c,certificate,inputs);
    pc_local_source_fields(&c,certificate->body,xr_xir_compile_artifact_module(checked));
    XrXirCompileContext foreign={0};XrCompileResourceLimits caps=rp_caps();
    CHECK(xr_compile_resources_new(&caps,&foreign.resources)==XR_COMPILE_RESOURCE_OK);
    foreign.limits=c.limits;EffectInvocationLocalInput *occupied=inputs,*missing=NULL;
    size_t allocations=rp_attempts;XrCompileResourceStats before=rp_stats(&c);
    CHECK(effect_invocation_local_inputs(&c,certificate->body,&occupied)==XR_XIR_BAD_STRUCTURE && occupied==inputs);
    CHECK(effect_invocation_local_inputs(&foreign,certificate->body,&missing)==XR_XIR_BAD_STRUCTURE && !missing);
    CHECK(effect_invocation_local_inputs(NULL,certificate->body,&missing)==XR_XIR_BAD_STRUCTURE && !missing &&
        rp_attempts==allocations && conditional_memory_unchanged(&c,before));
    xr_compile_resources_release(foreign.resources);xr_compile_resources_free(inputs);inputs=NULL;
    xr_xir_compile_artifact_free(checked);checked=NULL;memset(&fixture,0xa5,sizeof(fixture));
    CHECK(effect_invocation_local_inputs(&c,certificate->body,&inputs)==XR_XIR_OK && inputs);
    CHECK(effect_invocation_local_equations_match(&c,certificate->final,inputs)==XR_XIR_OK);
    CHECK(effect_invocation_certificate_result_match(&c,certificate,(&certificate->final->summary))==XR_XIR_OK);
    CHECK(effect_invocation_public_gate(&c,certificate)==XR_XIR_OK);pc_empty_golden(effects,0);
    xr_compile_resources_free(inputs);xr_xir_compile_effects_free(effects);
    pc_local_formal_preflight(&c);rp_owner_free(&c,baseline);CHECK(!rp_live && !rp_live_bytes);
    pc_local_transfer_positive();pc_local_callback_boundary();pc_local_release_boundary();
    pc_current_input_callbacks();
    puts("empty transfer: genuine unused Cell pool, initializer/thrower, final counterfacts, raw ten fields and producer death");
}

#include "xir_final_metadata_cases.h"
#include "xir_source_terminal_callback_cases.h"
#endif
