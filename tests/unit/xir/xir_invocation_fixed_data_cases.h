/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_invocation_fixed_data_cases.h - Actual barrier coordinates and independent replay
 */
#ifndef XIR_INVOCATION_FIXED_DATA_CASES_H
#define XIR_INVOCATION_FIXED_DATA_CASES_H

static XrXirStatus fixed_data_delegate(const EffectBodyRequest *request,uint32_t f,uint32_t i,
    XrXirInstruction *storage,const XrXirInstruction **output,EffectBodyInstructionFields *fields) {
    return effect_body_owned_instruction(request,f,i,storage,output,fields);
}
static XrXirStatus fixed_data_replay(const XrXirCompileContext *context,XrXirEffects *effects,bool owned) {
    EffectInvocationCertificate *certificate=effects->invocations;EffectInvocationRead read={0};
    XrXirStatus status=effect_invocation_read_open(context,effects,certificate->declared,certificate->final,&read);
    EffectBodyRequest body=read.body;if(!owned) body.instruction=fixed_data_delegate;
    if(status==XR_XIR_OK) status=effect_body_physical_match(context,read.final->physical,read.final->material);
    if(status==XR_XIR_OK) status=owned?effect_invocation_owned_proofs_read(&read,&body):
        effect_invocation_producers_verify_read(&read,&body);
    if(status==XR_XIR_OK && !owned && !read.producer_enabled)
        status=effect_invocation_data_verify_read(&read,&body);
    if(status==XR_XIR_OK && !owned) status=effect_invocation_coverage_verify_read(&read,&body);
    if(status==XR_XIR_OK && !owned) status=effect_invocation_iterators_read(&read,&body,NULL);
    if(status==XR_XIR_OK && !owned) status=effect_invocation_class_verify_read(&read,&body);
    if(status==XR_XIR_OK) status=effect_invocation_closed_read(context,certificate->final);
    effect_invocation_read_close(&read);return status;
}
/* The real Cell initializer fixes the same callable SSA passed to apply.
 * Removing that initializer keeps the authenticated producer and its original
 * advertisement, while the actual barrier fact is absent. */
static void fixed_data_case(uint32_t store,uint32_t less) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&context).live_bytes;
    InvocationDeferredFixture fixture;invocation_deferred_fixture(&fixture,0);
    XrXirTypeNode nodes[3];memcpy(nodes,fixture.nodes,sizeof(fixture.nodes));
    nodes[2]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CELL,.element=(XrXirType)257};
    fixture.types.nodes=nodes;fixture.types.count=3;
    XrXirInstruction entry[6];memcpy(entry,fixture.entry,3*sizeof(*entry));
    entry[3]=(XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=(XrXirType)258,.args={2,0}};
    entry[4]=fixture.entry[3];entry[5]=fixture.entry[4];entry[5].args[0]=4;
    if(store) {fixture.functions[0].instructions=entry;fixture.functions[0].instruction_count=6;fixture.entry_block.count=6;}
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;
    CHECK(xir_fixture_check(&context,&fixture.module,&checked,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_effects_analyze(checked,&effects)==XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);memset(&fixture,0xCE,sizeof(fixture));memset(entry,0xCE,sizeof(entry));
    memset(nodes,0xCE,sizeof(nodes));
    EffectInvocationFinal *final=effects->invocations->final;uint32_t n=final->roots[0];
    CHECK(n<final->count && final->nodes[n].body==0);
    EffectInvocationNode *node=&final->nodes[n];EffectInvocationBasis basis={0};uint32_t columns=0;
    EffectInvocationRead read={.work=&context,.final=final,
        .body=effect_body_owned_request(&context,final->material)};
    CHECK(effect_invocation_basis(&read,node->root,&basis)==XR_XIR_OK &&
        effect_invocation_data_columns(&read,&basis,&columns)==XR_XIR_OK);
    uint32_t ordinal=UINT32_MAX,count=node->producer_count;
    for(uint32_t p=0;p<count;++p) if(node->producers[p].value==2 &&
        node->producers[p].site==UINT32_MAX && node->producers[p].atom==columns-1) {
        CHECK(ordinal==UINT32_MAX);ordinal=p;
    }
    CHECK((ordinal!=UINT32_MAX)==!!store);
    EffectInvocationColumn untouched={73,91},position=untouched;
    CHECK(effect_invocation_data_offset(&read,&basis,columns-1,&position)==XR_XIR_BAD_STRUCTURE &&
        position.word==untouched.word && position.bit==untouched.bit);
    /* Own an explicit spare test slot. The complete original array is restored
     * before retirement; no element reference survives a possible growth. */
    EffectInvocationProducer *original=node->producers;uint32_t capacity=node->producer_capacity;
    XrXirStatus allocation=XR_XIR_OK;
    EffectInvocationProducer *records=xir_compile_alloc(&context,(size_t)(count+1)*sizeof(*records),&allocation);
    CHECK(records && allocation==XR_XIR_OK);memcpy(records,original,(size_t)count*sizeof(*records));
    node->producers=records;node->producer_capacity=count+1;
    EffectInvocationProducer literal={2,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,0,columns-1};
    for(uint32_t route=0;route<2;++route) {
        CHECK(fixed_data_replay(&context,effects,route!=0)==XR_XIR_OK);
        uint32_t attacks=store?7:1;
        for(uint32_t attack=0;attack<attacks;++attack) {
            memcpy(records,original,(size_t)count*sizeof(*records));node->producer_count=count;
            if(!store) {records[count]=literal;++node->producer_count;}
            else if(!attack) {records[ordinal]=records[count-1];--node->producer_count;}
            else if(attack==1) {records[count]=records[ordinal];++node->producer_count;}
            else if(attack==2) records[ordinal].atom=columns;
            else if(attack==3) records[ordinal].site=0;
            else if(attack==4) records[ordinal].distance=1;
            else if(attack==5) records[ordinal].next_value=0;
            else records[ordinal].value=node->parameter_count+node->instruction_count;
            XrXirStatus rejected=fixed_data_replay(&context,effects,route!=0);
            if(rejected!=XR_XIR_BAD_STRUCTURE) fprintf(stderr,"FIXED_DATA store%u route%u attack%u status%u work%llu\n",
                store,route,attack,(unsigned)rejected,(unsigned long long)rp_stats(&context).work);
            CHECK(rejected==XR_XIR_BAD_STRUCTURE);
            memcpy(records,original,(size_t)count*sizeof(*records));node->producer_count=count;
            CHECK(fixed_data_replay(&context,effects,route!=0)==XR_XIR_OK);
        }
    }
    node->producers=original;node->producer_capacity=capacity;node->producer_count=count;
    xr_compile_resources_free(records);
    uint64_t work=rp_stats(&context).work;
    CHECK(fixed_data_replay(&context,effects,true)==XR_XIR_OK);
    uint64_t required=rp_stats(&context).work-work;work=rp_stats(&context).work;
    CHECK(required && work+required-less<rp_caps().work &&
        xr_compile_resources_work(context.resources,rp_caps().work-work-required+less)==XR_COMPILE_RESOURCE_OK);
    CHECK(fixed_data_replay(&context,effects,true)==(less?XR_XIR_BUDGET:XR_XIR_OK));
    xr_xir_compile_effects_free(effects);rp_owner_free(&context,baseline);rp_balanced(physical);
    CHECK(!rp_live && !rp_live_bytes);
}
static void fixed_data_cases(void) {
    for(uint32_t store=0;store<2;++store) for(uint32_t less=0;less<2;++less) fixed_data_case(store,less);
    puts("FIXED_DATA genuine Checked store/absence,producer-dead,owned/delegated missing/inflated/duplicate/cause attacks,restores,exact/minus1,physical0 PASS");
}
#endif // XIR_INVOCATION_FIXED_DATA_CASES_H
