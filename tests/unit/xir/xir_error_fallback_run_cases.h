/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_error_fallback_run_cases.h - Ordered fallback edges and independent scalar replay
 */
#ifndef XIR_ERROR_FALLBACK_RUN_CASES_H
#define XIR_ERROR_FALLBACK_RUN_CASES_H

static XrXirStatus fallback_original(ErrorFlow *flow,ErrorInvocationContexts *contexts,
    const EffectInvocationCertificate *certificate,uint32_t n,uint32_t instruction) {
    ErrorInvocationCall *call=&contexts->calls[contexts->nodes[n].calls+instruction];
    if (call->handled) return XR_XIR_OK;
    XrXirInstruction storage;const XrXirInstruction *op=NULL;
    XrXirStatus read=error_operation(flow,contexts->nodes[n].body,instruction,&storage,&op);
    if (read!=XR_XIR_OK) return read;
    uint32_t target=UINT32_MAX;
    if (op->op==XR_XIR_CALL || op->op==XR_XIR_INVOKE || op->op==XR_XIR_GO)
        target=op->immediate<0 || (uint64_t)op->immediate>=flow->effects->count?UINT32_MAX:(uint32_t)op->immediate;
    else if (op->op==XR_XIR_CALL_DEFAULT || op->op==XR_XIR_INVOKE_DEFAULT) {
        const uint32_t *identity=xr_xir_default_identity(op);const XrXirDefaultBinding *binding=NULL;
        XrXirStatus status=xir_metadata_default_lookup(flow->remaining,&flow->body.domain,identity[0],identity[1],&binding);
        if (status!=XR_XIR_OK) return status;
        if (!binding || binding->function>=flow->effects->count) return XR_XIR_BAD_STRUCTURE;
        target=binding->function;
    } else return XR_XIR_OK;
    if (target>=flow->effects->count || contexts->link_count>=contexts->link_capacity)
        return XR_XIR_BAD_STRUCTURE;
    target=certificate->final->roots[target];
    if (target>=contexts->count) return XR_XIR_BAD_STRUCTURE;
    uint32_t link=contexts->link_count++;
    if (!xir_compile_work(flow->remaining,5)) return XR_XIR_BUDGET;
    contexts->links[link]=(ErrorInvocationLink){n,target,UINT32_MAX,call->head,contexts->consumers[target]};
    call->head=link;contexts->consumers[target]=link;return XR_XIR_OK;
}


static uint32_t fallback_reads,fallback_refuse=UINT32_MAX;
static XrXirStatus fallback_delegate(const EffectBodyRequest *request,uint32_t f,uint32_t i,
    XrXirInstruction *storage,const XrXirInstruction **output,EffectBodyInstructionFields *fields) {
    ++fallback_reads;
    if(f==3 && i==fallback_refuse)return XR_XIR_BAD_STRUCTURE;
    return effect_body_owned_instruction(request,f,i,storage,output,fields);
}
static ErrorInvocationContexts *fallback_storage(ErrorFlow *flow,uint32_t pattern) {
    ErrorInvocationContexts *contexts=NULL;
    CHECK(error_invocation_storage(flow,flow->effects->invocations,&contexts)==XR_XIR_OK && contexts);
    const EffectBodyMaterial *body=flow->effects->invocations->body;
    for(uint32_t n=0;n<contexts->count;++n)
        for(uint32_t i=0;i<body->functions[contexts->nodes[n].body].instruction_count;++i)
            contexts->calls[contexts->nodes[n].calls+i].handled=pattern==1 || (pattern==2 && !(i&1));
    return contexts;
}
static XrXirStatus fallback_replay(ErrorFlow *flow,ErrorInvocationContexts *contexts,bool scalar) {
    const EffectInvocationCertificate *certificate=flow->effects->invocations;
    for(uint32_t n=0;n<contexts->count;++n) {
        EffectBodyFunctionDimensions head={0};
        XrXirStatus status=effect_body_request_dimensions(&flow->body,contexts->nodes[n].body,&head);
        if(status!=XR_XIR_OK)return status;
        if(scalar) {
            for(uint32_t i=0;i<head.instruction_count;++i) {
                status=fallback_original(flow,contexts,certificate,n,i);
                if(status!=XR_XIR_OK)return status;
            }
        } else status=error_invocation_fallback_body(flow,contexts,certificate,n,head.instruction_count);
        if(status!=XR_XIR_OK)return status;
    }
    return XR_XIR_OK;
}
static void fallback_equal(ErrorFlow *flow,uint32_t pattern) {
    ErrorInvocationContexts *old=fallback_storage(flow,pattern),*got=fallback_storage(flow,pattern);
    XrCompileResourceStats before=rp_stats(flow->remaining);size_t attempts=rp_attempts;
    fallback_reads=0;CHECK(fallback_replay(flow,old,true)==XR_XIR_OK);uint32_t visits=fallback_reads;
    fallback_reads=0;CHECK(fallback_replay(flow,got,false)==XR_XIR_OK && fallback_reads==visits);
    XrCompileResourceStats after=rp_stats(flow->remaining);
    CHECK(attempts==rp_attempts && before.allocated_bytes==after.allocated_bytes && before.live_bytes==after.live_bytes);
    CHECK(old->count==got->count && old->link_count==got->link_count && old->summaries==got->summaries);
    for(uint32_t n=0;n<old->count;++n) {
        CHECK(old->nodes[n].body==got->nodes[n].body && old->nodes[n].summary==got->nodes[n].summary &&
            old->nodes[n].calls==got->nodes[n].calls && old->consumers[n]==got->consumers[n]);
        uint32_t count=flow->effects->invocations->body->functions[old->nodes[n].body].instruction_count;
        for(uint32_t i=0;i<count;++i) {
            ErrorInvocationCall a=old->calls[old->nodes[n].calls+i],b=got->calls[got->nodes[n].calls+i];
            CHECK(a.head==b.head && a.handled==b.handled && a.unknown==b.unknown && a.frontier==b.frontier);
        }
    }
    for(uint32_t e=0;e<old->link_count;++e) {
        ErrorInvocationLink a=old->links[e],b=got->links[e];
        CHECK(a.caller==b.caller && a.target==b.target && a.producer==b.producer && a.next==b.next && a.consumer_next==b.consumer_next);
    }
    EffectBodyRequest owned=effect_body_owned_request(flow->remaining,flow->effects->invocations->body);
    uint32_t expected=0;
    for(uint32_t n=0;n<got->count;++n) {
        uint32_t f=got->nodes[n].body;
        const EffectBodyFunction *function=&flow->effects->invocations->body->functions[f];
        for(uint32_t i=0;i<function->instruction_count;++i) {
            XrXirInstruction op;
            CHECK(effect_body_instruction_get(&owned.read,function,i,&op)==XR_XIR_OK);
            if(f!=0 || pattern==1 || (pattern==2 && !(i&1)))continue;
            if(op.op!=XR_XIR_CALL && op.op!=XR_XIR_CALL_DEFAULT)continue;
            uint32_t target=op.op==XR_XIR_CALL?1:4;
            CHECK(expected<got->link_count && got->links[expected].caller==n &&
                got->nodes[got->links[expected].target].body==target && got->links[expected].producer==UINT32_MAX &&
                got->links[expected].next==UINT32_MAX);
            ++expected;
        }
    }
    CHECK(got->link_count==expected && (pattern==1 || expected));
    error_invocation_contexts_free(old);error_invocation_contexts_free(got);
}
static void fallback_reject(ErrorFlow *flow,ErrorInvocationContexts *contexts,uint32_t n) {
    uint32_t count=((const EffectBodyMaterial *)flow->body.owner)->functions[3].instruction_count;
    uint32_t links=contexts->link_count;
    CHECK(error_invocation_fallback_body(flow,contexts,flow->effects->invocations,n,count)==XR_XIR_BAD_STRUCTURE);
    CHECK(contexts->link_count==links);
}
static void fallback_selected_rejections(ErrorFlow *flow) {
    EffectBodyMaterial *body=flow->effects->invocations->body;
    EffectInvocationFinal *final=(EffectInvocationFinal *)flow->effects->invocations->final;
    ErrorInvocationContexts *contexts=fallback_storage(flow,0);uint32_t n=UINT32_MAX;
    for(uint32_t j=0;j<contexts->count;++j)if(contexts->nodes[j].body==0){n=j;break;}
    CHECK(n!=UINT32_MAX);const EffectBodyRun *ref=NULL;
    CHECK(effect_body_record_ref(&flow->body.read,&body->functions[0],3,&ref)==XR_XIR_OK);
    EffectBodyRun *run=(EffectBodyRun *)ref,saved=*run;
    XrXirInstruction selected;CHECK(effect_body_instruction_ref_value(&flow->body.read,ref,3,&selected)==XR_XIR_OK);
    if(selected.op==XR_XIR_CALL_DEFAULT)run->base.targets[0]+=32;
    else run->base.immediate-=64;
    CHECK(error_invocation_fallback_body(flow,contexts,flow->effects->invocations,n,body->functions[0].instruction_count)==XR_XIR_BAD_STRUCTURE);
    CHECK(!contexts->link_count);*run=saved;
    uint32_t root=final->roots[1];final->roots[1]=UINT32_MAX;
    CHECK(error_invocation_fallback_body(flow,contexts,flow->effects->invocations,n,body->functions[0].instruction_count)==XR_XIR_BAD_STRUCTURE);
    final->roots[1]=root;error_invocation_contexts_free(contexts);
}
static void fallback_failures(ErrorFlow *flow,uint32_t runs) {
    EffectBodyMaterial *body=flow->effects->invocations->body;
    ErrorInvocationContexts *contexts=fallback_storage(flow,0);uint32_t n=UINT32_MAX;
    for(uint32_t j=0;j<contexts->count;++j)if(contexts->nodes[j].body==3){n=j;break;}
    CHECK(n!=UINT32_MAX);EffectBodyFunction *function=&body->functions[3];
    for(uint32_t j=0;j<runs;++j) {
        EffectBodyRun saved=function->runs[j];
        function->runs[j].count=0;fallback_reject(flow,contexts,n);function->runs[j]=saved;
        ++function->runs[j].first;fallback_reject(flow,contexts,n);function->runs[j]=saved;
        function->runs[j].mask|=1024;fallback_reject(flow,contexts,n);function->runs[j]=saved;
        function->runs[j].base.op=XR_XIR_OP_COUNT;fallback_reject(flow,contexts,n);function->runs[j]=saved;
    }
    if(runs>1) {
        EffectBodyRun saved=function->runs[0];
        for(uint32_t w=0;w<EFFECT_BODY_WORDS;++w) {
            function->runs[0].mask|=UINT32_C(1)<<w;function->runs[0].delta[w]=0;
            fallback_reject(flow,contexts,n);function->runs[0]=saved;
        }
    }
    ++function->instruction_count;fallback_reject(flow,contexts,n);--function->instruction_count;
    EffectBodyFunction saved=*function;*function=(EffectBodyFunction){.result=XR_XIR_UNIT};
    CHECK(error_invocation_fallback_body(flow,contexts,flow->effects->invocations,n,0)==XR_XIR_OK);
    function->run_capacity=1;fallback_reject(flow,contexts,n);*function=saved;
    ErrorFlow foreign=*flow;foreign.body.domain.function_count++;fallback_reject(&foreign,contexts,n);
    foreign=*flow;foreign.body.domain.types=NULL;fallback_reject(&foreign,contexts,n);
    foreign=*flow;foreign.body.domain.defaults=NULL;
    if(flow->body.domain.defaults)fallback_reject(&foreign,contexts,n);
    XrXirCompileContext other=rp_owner(rp_caps());foreign=*flow;foreign.remaining=&other;
    XrCompileResourceStats initial=rp_stats(&other);fallback_reject(&foreign,contexts,n);
    XrCompileResourceStats after=rp_stats(&other);CHECK(!memcmp(&initial,&after,sizeof(initial)));rp_owner_free(&other,initial.live_bytes);
    ErrorFlow delegated=*flow;delegated.body.instruction=fallback_delegate;fallback_refuse=0;
    CHECK(fallback_replay(&delegated,contexts,false)==XR_XIR_BAD_STRUCTURE);
    fallback_refuse=UINT32_MAX;error_invocation_contexts_free(contexts);
}
static void fallback_owned(uint32_t runs,uint32_t mode,uint32_t less) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());uint64_t baseline=rp_stats(&context).live_bytes;
    InvocationDeferredFixture fixture;invocation_deferred_fixture(&fixture,0);
    XrXirInstruction constants[9]={
        {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=1},{.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=2},
        {.op=XR_XIR_CONST_BOOL,.type=XR_XIR_BOOL,.immediate=1},{.op=XR_XIR_CONST_BOOL,.type=XR_XIR_BOOL},
        {.op=XR_XIR_CONST_FLOAT,.type=XR_XIR_F64},{.op=XR_XIR_CONST_FLOAT,.type=XR_XIR_F64,.immediate=1},
        {.op=XR_XIR_CONST_RUNE,.type=XR_XIR_RUNE,.immediate=97},{.op=XR_XIR_CONST_RUNE,.type=XR_XIR_RUNE,.immediate=98},
        {.op=XR_XIR_RETURN}};
    uint32_t count=2*runs-1;constants[count-1]=(XrXirInstruction){.op=XR_XIR_RETURN};
    fixture.functions[3].instructions=constants;fixture.functions[3].instruction_count=fixture.init_block.count=count;
    XrXirInstruction entry[6]={0},helper[2]={{.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=7},{.op=XR_XIR_RETURN}};
    XrXirInstruction owner={.op=XR_XIR_RETURN};XrXirBlock entry_block={.count=6},two={.count=2},one={.count=1};
    uint32_t actuals[4]={2,1,2,1};XrXirType parameter=XR_XIR_I64;
    XrXirFunction functions[6]={0};XrXirFunctionIdentity identities[6]={0};
    XrXirDefaultBinding binding={XR_XIR_DEFAULT_PARAMETER,5,0,4};XrXirDefaultTable table={&binding,1};
    if(mode) {
        memcpy(entry,fixture.entry,3*sizeof(*entry));entry[3]=entry[4]=fixture.entry[3];
        if(mode==1)entry[4].args[0]=2;
        else entry[3]=(XrXirInstruction){.op=XR_XIR_CALL_DEFAULT,.type=XR_XIR_I64,.targets={5,0}};
        entry[5]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={4,0}};
        fixture.functions[0].instructions=entry;fixture.functions[0].instruction_count=6;fixture.functions[0].blocks=&entry_block;
        fixture.functions[0].operands=actuals;fixture.functions[0].operand_count=mode==1?4:2;
    }
    if(mode==2) {
        memcpy(functions,fixture.functions,sizeof(fixture.functions));
        functions[4]=(XrXirFunction){.name="default",.name_length=7,.result=XR_XIR_I64,.blocks=&two,.block_count=1,.instructions=helper,.instruction_count=2};
        functions[5]=(XrXirFunction){.name="owner",.name_length=5,.parameters=&parameter,.parameter_count=1,.result=XR_XIR_I64,.blocks=&one,.block_count=1,.instructions=&owner,.instruction_count=1};
        fixture.module.functions=functions;fixture.module.function_count=6;fixture.declarations.functions=identities;fixture.module.defaults=&table;
    }
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xir_fixture_check(&context,&fixture.module,&checked,&diagnostic);
    if(status!=XR_XIR_OK)fprintf(stderr,"fallback fixture runs%u mode%u status%u f%u i%u reason%u\n",runs,mode,status,diagnostic.function,diagnostic.instruction,diagnostic.reason);
    CHECK(status==XR_XIR_OK && xr_xir_compile_effects_analyze(checked,&effects)==XR_XIR_OK);
    ErrorFlow raw={.body=effect_body_raw_request(&context,&fixture.module),.remaining=&context,.effects=effects};
    for(uint32_t pattern=0;pattern<3;++pattern)fallback_equal(&raw,pattern);
    xr_xir_compile_artifact_free(checked);memset(&fixture,0xCE,sizeof(fixture));memset(constants,0xCE,sizeof(constants));memset(entry,0xCE,sizeof(entry));
    memset(helper,0xCE,sizeof(helper));memset(&owner,0xCE,sizeof(owner));memset(actuals,0xCE,sizeof(actuals));memset(functions,0xCE,sizeof(functions));
    memset(identities,0xCE,sizeof(identities));memset(&binding,0xCE,sizeof(binding));memset(&table,0xCE,sizeof(table));
    ErrorFlow flow={.body=effect_body_owned_request(&context,effects->invocations->body),.remaining=&context,.effects=effects};
    CHECK(effects->invocations->body->functions[3].run_count==runs);
    for(uint32_t pattern=0;pattern<3;++pattern) {
        fallback_equal(&flow,pattern);ErrorFlow delegated=flow;delegated.body.instruction=fallback_delegate;fallback_equal(&delegated,pattern);
    }
    fallback_selected_rejections(&flow);fallback_failures(&flow,runs);
    ErrorInvocationContexts *trial=fallback_storage(&flow,0);XrCompileResourceStats before=rp_stats(&context);
    CHECK(fallback_replay(&flow,trial,false)==XR_XIR_OK);XrCompileResourceStats after=rp_stats(&context);
    uint64_t required=after.work-before.work;error_invocation_contexts_free(trial);trial=fallback_storage(&flow,0);
    before=rp_stats(&context);uint64_t remaining=required-less,limit=rp_caps().work;
    CHECK(required && before.work+remaining<limit && xr_compile_resources_work(context.resources,limit-before.work-remaining)==XR_COMPILE_RESOURCE_OK);
    CHECK(fallback_replay(&flow,trial,false)==(less?XR_XIR_BUDGET:XR_XIR_OK));error_invocation_contexts_free(trial);
    xr_xir_compile_effects_free(effects);rp_owner_free(&context,baseline);rp_balanced(physical);CHECK(!rp_live && !rp_live_bytes);
}
static void error_fallback_run_cases(void) {
    for(uint32_t runs=1;runs<=5;++runs)for(uint32_t mode=0;mode<3;++mode)for(uint32_t less=0;less<2;++less)fallback_owned(runs,mode,less);
}
#endif // XIR_ERROR_FALLBACK_RUN_CASES_H
