/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_invocation_native_terminal_cases.h - Complete current terminal replay
 */
#ifndef XIR_INVOCATION_NATIVE_TERMINAL_CASES_H
#define XIR_INVOCATION_NATIVE_TERMINAL_CASES_H
static size_t native_terminal_reads,native_terminal_refused=SIZE_MAX;
static size_t native_terminal_sites,native_terminal_roots;
static uint64_t native_terminal_work_units;
static XrXirStatus native_terminal_head(const EffectBodyRequest *request,uint32_t function,
    EffectBodyFunctionHead *output,EffectBodyFunctionDimensions *dimensions) {
    if(native_terminal_reads++==native_terminal_refused)return XR_XIR_IO;
    return effect_body_owned_head(request,function,output,dimensions);
}
static XrXirStatus native_terminal_instruction(const EffectBodyRequest *request,uint32_t function,
    uint32_t ordinal,XrXirInstruction *storage,const XrXirInstruction **output,EffectBodyInstructionFields *fields) {
    if(native_terminal_reads++==native_terminal_refused)return XR_XIR_IO;
    return effect_body_owned_instruction(request,function,ordinal,storage,output,fields);
}
static XrXirStatus native_terminal_type(const EffectBodyRequest *request,uint32_t function,
    uint32_t value,XrXirType *output) {
    if(native_terminal_reads++==native_terminal_refused)return XR_XIR_IO;
    return effect_body_owned_type(request,function,value,output);
}
/* The original cause allocation owns the complete input, both output copies
 * and parameter coverage. Its bytes and every other live fact stay intact. */
static XrXirStatus native_terminal_execute(EffectInvocationFlow *flow) {
    EffectInvocationNode *node=&flow->owner->final.nodes[flow->node],before=*node;
    uint64_t dependencies=((uint64_t)flow->basis.parameters+63)/64,contexts=((uint64_t)node->instruction_count+63)/64;
    size_t bytes=(size_t)node->cause_count*2*sizeof(EffectInvocationCause)+
        (size_t)node->parameter_count*flow->basis.row_words*sizeof(uint64_t)+
        (size_t)(2*dependencies+contexts)*2*sizeof(uint64_t)+(size_t)node->parameter_count*sizeof(uint64_t);
    unsigned char payload[4096];EffectInvocationClassFact classes[32];EffectInvocationProducer producers[32];
    uint64_t rows[128],flags[4];size_t words=(size_t)flow->row_count*flow->basis.fn_words,flag_words=((size_t)flow->values+63)/64;
    CHECK(bytes<=sizeof(payload) && node->class_count<=32 && node->producer_count<=32 && words<=128 && flag_words<=2);
    memcpy(payload,node->causes,bytes);
    if(node->class_count)memcpy(classes,node->classes,(size_t)node->class_count*sizeof(*classes));
    if(node->producer_count)memcpy(producers,node->producers,(size_t)node->producer_count*sizeof(*producers));
    if(words)memcpy(rows,flow->rows,words*sizeof(uint64_t));
    if(flag_words)memcpy(flags,flow->fixed,2*flag_words*sizeof(uint64_t));
    RootParameterMark physical=rp_mark();uint64_t live=rp_stats(flow->owner->work).live_bytes;
    XrXirStatus status=effect_invocation_class_terminals_match(flow);
    CHECK(!memcmp(node,&before,sizeof(before)) && !memcmp(payload,node->causes,bytes));
    CHECK(!node->class_count || !memcmp(classes,node->classes,(size_t)node->class_count*sizeof(*classes)));
    CHECK(!node->producer_count || !memcmp(producers,node->producers,(size_t)node->producer_count*sizeof(*producers)));
    CHECK(!words || !memcmp(rows,flow->rows,words*sizeof(uint64_t)));
    CHECK(!flag_words || !memcmp(flags,flow->fixed,2*flag_words*sizeof(uint64_t)));
    CHECK(rp_stats(flow->owner->work).live_bytes==live);rp_balanced(physical);return status;
}
static void native_terminal_delegates(EffectInvocationFlow *flow) {
    EffectBodyRequest original=flow->body;CHECK(native_terminal_execute(flow)==XR_XIR_OK);
    uint32_t expected_instruction=flow->instruction;
    for(unsigned reader=0;reader<3;++reader){flow->body=original;
        if(!reader)flow->body.head=native_terminal_head;
        else if(reader==1)flow->body.instruction=native_terminal_instruction;
        else flow->body.type=native_terminal_type;
        native_terminal_reads=0;native_terminal_refused=SIZE_MAX;
        CHECK(native_terminal_execute(flow)==XR_XIR_OK && flow->instruction==expected_instruction);
        size_t sites=native_terminal_reads;native_terminal_sites+=sites;
        if(reader<2)CHECK(sites);
        for(size_t at=0;at<sites;++at){native_terminal_reads=0;native_terminal_refused=at;
            CHECK(native_terminal_execute(flow)==XR_XIR_IO && native_terminal_reads==at+1);
            native_terminal_reads=0;
            CHECK(native_terminal_execute(flow)==XR_XIR_IO && native_terminal_reads==at+1);}
    }
    native_terminal_refused=SIZE_MAX;flow->body=original;
}
static void native_terminal_faults(EffectInvocationFlow *flow) {
    EffectInvocationWorkspace *owner=flow->owner;const XrXirCompileContext *work=owner->work;
    EffectInvocationNode *node=&owner->final.nodes[flow->node];
    EffectBodyMaterial *material=owner->final.material;EffectBodyFunction *head=&material->functions[node->root],saved=*head;
    CHECK(node->root==5 && saved.parameter_count==1 && saved.instruction_count==2);
    for(unsigned field=0;field<7;++field){*head=saved;
        if(!field){head->name=NULL;head->name_length=1;}else if(field==1)head->parameters=NULL;
        else if(field==2){head->blocks=NULL;head->block_count=1;}
        else if(field==3){head->operands=NULL;head->operand_count=1;}
        else if(field==4)head->runs=NULL;else if(field==5)head->run_count=0;else head->run_count=head->run_capacity+1;
        CHECK(native_terminal_execute(flow)==XR_XIR_BAD_STRUCTURE);*head=saved;
    }
    EffectBodyRequest original=flow->body;XrXirDeclarations declarations={0};XrXirGeneric generics={0};XrXirDefaultTable defaults={0};
    for(unsigned field=0;field<5;++field){
        if(!field)flow->body.domain.types=&effect_empty_types;else if(field==1)flow->body.domain.declarations=&declarations;
        else if(field==2)flow->body.domain.generics=&generics;else if(field==3)flow->body.domain.defaults=&defaults;
        else ++flow->body.domain.function_count;
        CHECK(native_terminal_execute(flow)==XR_XIR_BAD_STRUCTURE);flow->body=original;
    }
    owner->final.material=NULL;CHECK(native_terminal_execute(flow)==XR_XIR_BAD_STRUCTURE);owner->final.material=material;
    const EffectPhysicalFunction *physical=flow->physical;flow->physical=NULL;
    CHECK(native_terminal_execute(flow)==XR_XIR_BAD_STRUCTURE);flow->physical=physical;
    ++flow->values;CHECK(native_terminal_execute(flow)==XR_XIR_BAD_STRUCTURE);--flow->values;
    uint32_t root=node->root;node->root=effect_body_count(material);
    CHECK(native_terminal_execute(flow)==XR_XIR_BAD_STRUCTURE);node->root=root;
    const XrXirInstruction **references=flow->instruction_refs;XrXirInstruction *instructions=flow->instructions;
    flow->instruction_refs=NULL;flow->instructions=NULL;
    CHECK(native_terminal_execute(flow)==XR_XIR_BAD_STRUCTURE);flow->instruction_refs=references;flow->instructions=instructions;
    const XrXirInstruction *native_rows[128];CHECK(flow->function->instruction_count<=128);
    for(uint32_t i=0;i<flow->function->instruction_count;++i)native_rows[i]=references?references[i]:&instructions[i];
    native_rows[0]=NULL;flow->instruction_refs=native_rows;
    CHECK(native_terminal_execute(flow)==XR_XIR_BAD_STRUCTURE);flow->instruction_refs=references;
    XrXirType *parameter=(XrXirType *)head->parameters;XrXirType held=*parameter;*parameter=XR_XIR_I64;
    CHECK(native_terminal_execute(flow)==XR_XIR_BAD_STRUCTURE);*parameter=held;
    XrXirCompileContext foreign=rp_owner(rp_caps());XrCompileResourceStats before=rp_stats(&foreign);
    flow->body.read.owner=&foreign;CHECK(native_terminal_execute(flow)==XR_XIR_BAD_STRUCTURE);flow->body=original;
    CHECK(rp_stats(&foreign).work==before.work && rp_stats(&foreign).allocated_bytes==before.allocated_bytes &&
        rp_stats(&foreign).live_bytes==before.live_bytes);rp_owner_free(&foreign,before.live_bytes);
    CHECK(native_terminal_execute(flow)==XR_XIR_OK && work==owner->work);
}
static void native_terminal_resource_cuts(EffectInvocationFlow *flow) {
    const XrXirCompileContext *work=flow->owner->work;uint64_t entry=rp_stats(work).work;
    size_t attempts=rp_attempts;CHECK(native_terminal_execute(flow)==XR_XIR_OK && rp_attempts==attempts+1);
    uint64_t exact=rp_stats(work).work-entry,original_limit=work->resources->limits.work;native_terminal_work_units+=exact;
    rp_fail_at=rp_attempts;rp_injected=false;CHECK(native_terminal_execute(flow)==XR_XIR_OUT_OF_MEMORY && rp_injected);
    rp_fail_at=SIZE_MAX;rp_injected=false;CHECK(native_terminal_execute(flow)==XR_XIR_OK);
    for(uint64_t cut=0;cut<=exact;++cut){entry=rp_stats(work).work;CHECK(exact<=original_limit-entry);
        work->resources->limits.work=entry+cut;
        CHECK(native_terminal_execute(flow)==(cut==exact?XR_XIR_OK:XR_XIR_BUDGET));
        if(cut<exact)CHECK(native_terminal_execute(flow)==XR_XIR_BUDGET);
        work->resources->limits.work=original_limit;
    }
}
/* Complete checking and effects derivation precede producer death. Every
 * controlled terminal is then replayed from the retained genuine owner. */
static XrXirStatus native_terminal_run(const XrXirCompileContext *context,bool controls) {
    InvocationClassCellFixture fixture;invocation_class_cell_fixture(&fixture,0);
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;EffectInvocationWorkspace replay={0};
    XrXirStatus status=xir_fixture_check(context,&fixture.source.module,&checked,NULL);
    if(status==XR_XIR_OK)status=xr_xir_compile_artifact_verify(checked,NULL);
    if(status==XR_XIR_OK)status=xr_xir_compile_effects_analyze(checked,&effects);
    xr_xir_compile_artifact_free(checked);memset(&fixture,0xCE,sizeof(fixture));
    if(status==XR_XIR_OK){EffectInvocationCertificate *certificate=effects->invocations;CHECK(certificate && certificate->final);
        status=effect_invocation_read_open(context,effects,certificate->declared,certificate->final,&replay);
        EffectBodyRequest body=effect_body_owned_request(context,certificate->body);
        for(uint32_t root=3;status==XR_XIR_OK && root<=5;root+=2){
            EffectInvocationFlow flow={.owner=&replay,.node=replay.final.roots[root],.instruction=UINT32_MAX,.precision=true};
            status=effect_invocation_origins_read(&flow,&body);
            if(status==XR_XIR_OK)status=native_terminal_execute(&flow);
            if(status==XR_XIR_OK && controls){CHECK(flow.types_ready);++native_terminal_roots;
                native_terminal_delegates(&flow);if(root==5)native_terminal_faults(&flow);native_terminal_resource_cuts(&flow);}
            xr_compile_resources_free(flow.memory);
        }
        if(status==XR_XIR_OK)status=effect_invocation_class_verify_read(&replay,&body);
    }
    effect_invocation_read_close(&replay);xr_xir_compile_effects_free(effects);return status;
}
static void invocation_native_terminal_controls(void) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());uint64_t baseline=rp_stats(&context).live_bytes;
    CHECK(native_terminal_run(&context,false)==XR_XIR_OK);XrCompileResourceStats measured=rp_stats(&context);
    rp_owner_free(&context,baseline);rp_balanced(physical);
    context=rp_owner(rp_caps());baseline=rp_stats(&context).live_bytes;CHECK(native_terminal_run(&context,true)==XR_XIR_OK);
    rp_owner_free(&context,baseline);rp_balanced(physical);
    for(unsigned axis=0;axis<3;++axis)for(unsigned minus=0;minus<2;++minus){XrCompileResourceLimits limits=rp_caps();
        if(!axis)limits.allocated_bytes=measured.allocated_bytes-minus;
        else if(axis==1)limits.live_bytes=measured.peak_bytes-minus;else limits.work=measured.work-minus;
        context=rp_owner(limits);baseline=rp_stats(&context).live_bytes;
        CHECK(native_terminal_run(&context,false)==(minus?XR_XIR_BUDGET:XR_XIR_OK));rp_owner_free(&context,baseline);rp_balanced(physical);
    }
    CHECK(native_terminal_roots==2 && native_terminal_sites && native_terminal_work_units);
    fprintf(stderr,"Native terminals: genuine Class/Cell certificate after Checked/producer death, two actual roots and%zu head/instruction/type callback sites with every IO/retry, complete node/input/output/cause/coverage/Class/producer/row/flag payload unchanged,7 full head presence and5 domain plus material/view/foreign-ledger/current Cell type refusals, original sole storage OOM and every work unit across%llu total units with same finite-owner retry; independent Class verification, whole genuine-fixture six axes%llu/%llu/%llu exact/minusone PASS\n",
        native_terminal_sites,(unsigned long long)native_terminal_work_units,(unsigned long long)measured.allocated_bytes,
        (unsigned long long)measured.peak_bytes,(unsigned long long)measured.work);
}
#endif // XIR_INVOCATION_NATIVE_TERMINAL_CASES_H
