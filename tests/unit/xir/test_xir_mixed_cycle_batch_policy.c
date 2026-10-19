/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_cycle_batch_policy.c - Source branching rings preserve authentic closure owners
 */
#define main cycle_original_source_main
#define p20_cell_cycle_fixture_program p20_cycle_batch_fixture_program
#define p20_cell_cycle_entries p20_cycle_batch_pair_entries
#include "test_xir_mixed_cell_cycles.c"
#undef p20_cell_cycle_entries
#undef p20_cell_cycle_fixture_program
#undef main
XR_DATA const uint32_t p20_cycle_batch_entries[3];
typedef struct CyclePolicyRun {
    CellCycleFixture fixture;
    XrXirInstance *instances[2];
    XrXirDomain *domains[2];
    XrXirValue functions[17],aliases[17];
    uintptr_t addresses[17][3];
    unsigned captures[17],count,shape,rejections;
} CyclePolicyRun;
static const XrXirValue *policy_cell(const XrXirValue *function,unsigned at) {
    CHECK(xr_xir_value_valid(function));const XrXirFunctionBinding *b=xr_xir_function_binding(function);
    CHECK(b && b->capture_count && b->capture_count<=2 && at<b->capture_count && b->captures && b->release==function_gate_drop);return &b->captures[at];
}
static void policy_make(CyclePolicyRun *run,unsigned index,unsigned owner,unsigned shape) {
    XrXirValue args[]={{XR_XIR_I64,0,shape},{XR_XIR_I64,0,0}};CellCycleWitness witness={0};
    CHECK(xr_xir_instance_start(run->instances[owner],p20_cycle_batch_entries[0],args,2)==XR_XIR_CALL_READY);
    XrXirInstanceResult event={0};unsigned polls=0;
    do {event=cycle_tick(&run->fixture,run->instances,owner,&witness,0,UINT32_MAX);CHECK(++polls<4096);}while(event.outcome.status!=XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_take_result(run->instances[owner],&run->functions[index])==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_value_copy(&run->functions[index],&run->aliases[index])==XR_XIR_VALUE_OK);
    const XrXirFunctionBinding *binding=xr_xir_function_binding(&run->functions[index]);CHECK(binding && binding->capture_count==shape+1);
    run->captures[index]=binding->capture_count;run->addresses[index][0]=(uintptr_t)run->functions[index].payload;
    for(unsigned c=0;c<binding->capture_count;++c){const XrXirValue *cell=policy_cell(&run->functions[index],c);CHECK(xr_xir_cell_in_domain(cell,run->domains[owner]));run->addresses[index][c+1]=(uintptr_t)cell->payload;for(unsigned k=0;k<c;++k)CHECK(run->addresses[index][k+1]!=run->addresses[index][c+1]);}
    for(unsigned i=0;i<index;++i){CHECK(run->addresses[i][0]!=run->addresses[index][0]);for(unsigned a=0;a<run->captures[i];++a)for(unsigned b=0;b<binding->capture_count;++b)CHECK(run->addresses[i][a+1]!=run->addresses[index][b+1]);}
    CHECK(witness.yields==1 && witness.root==(cycle_native(run->fixture.backend,true)?2u:1u) && witness.worker==(cycle_native(run->fixture.backend,false)?2u:1u));
}
/* Shared code entries can execute with different genuine captured Cells. */
static bool policy_frame_matches(const CallFrame *frame,const XrXirFunctionBinding *binding) {
    if(frame->entry->parameter_count!=binding->capture_count+1 || !frame->arguments)return false;
    for(unsigned c=0;c<binding->capture_count;++c)if(frame->arguments[c].type!=binding->captures[c].type || frame->arguments[c].payload!=binding->captures[c].payload)return false;
    return true;
}
static bool policy_capture_frame(CyclePolicyRun *run,const XrXirValue *function) {
    const XrXirFunctionBinding *expected=xr_xir_function_binding(function);XrXirCall *call=run->instances[0]->call;
    CHECK(call && !call->driving);const CallFrame *root=NULL;
    for(const CallFrame *f=call->top;f;f=f->parent){
        uint32_t index=(uint32_t)(f->entry-call->config.entries);if(!f->entered || index!=expected->entry)continue;
        bool known=policy_frame_matches(f,expected);
        for(unsigned i=0;i<run->count;++i){const XrXirFunctionBinding *binding=xr_xir_function_binding(&run->functions[i]);if(binding && binding->entry==index && policy_frame_matches(f,binding))known=true;}
        CHECK(known);for(unsigned c=0;c+1<f->entry->parameter_count;++c)CHECK(xr_xir_cell_in_domain(&f->arguments[c],run->domains[0]));root=f;
    }
    if(root)CHECK(policy_frame_matches(root,expected));return root!=NULL;
}
static void policy_invoke(CyclePolicyRun *run,const XrXirValue *function,int64_t command,int64_t expected) {
    const XrXirFunctionBinding *binding=xr_xir_function_binding(function);CellCycleWitness w={0};CHECK(binding && binding->capture_count<=2);
    XrXirValue argument={XR_XIR_I64,0,command};CHECK(xr_xir_instance_start_function(run->instances[0],function,&argument,1)==XR_XIR_CALL_READY);
    XrXirInstanceResult event={0};unsigned polls=0;
    do {event=cycle_tick(&run->fixture,run->instances,0,&w,0,UINT32_MAX);if(policy_capture_frame(run,function))w.captured_call=true;CHECK(++polls<4096);}while(event.outcome.status!=XR_XIR_CALL_RETURNED);
    XrXirValue result={0};CHECK(xr_xir_instance_take_result(run->instances[0],&result)==XR_XIR_CALL_RETURNED);
    CHECK(result.type==XR_XIR_I64 && result.payload==expected && !w.yields && (!binding->capture_count || (w.captured_call && w.worker==(cycle_native(run->fixture.backend,false)?2u:1u))));xr_xir_value_drop(&result);
}
static void policy_reject(CyclePolicyRun *run,unsigned left,unsigned right,unsigned owner) {
    const XrXirFunctionBinding *binding=xr_xir_function_binding(&run->functions[left]);
    for(unsigned c=0;c<binding->capture_count;++c){const XrXirValue *cell=policy_cell(&run->functions[left],c);XrXirValue old={0};CHECK(xr_xir_cell_read(cell,&old)==XR_XIR_VALUE_OK);size_t attempts=runtime_attempts;
        XrXirDomainBudgetStats before[2]={xr_xir_domain_budget_stats(run->domains[0]),xr_xir_domain_budget_stats(run->domains[1])};
        for(unsigned bad=0;bad<2;++bad){XrXirValueAdmission admission=instance_candidate_admission(run->instances[bad?1-owner:owner]);CHECK(admission.function==admit_function_binding && admission.context==run->instances[bad?1-owner:owner]);CHECK(xr_xir_cell_write(cell,&run->functions[right],&admission)==XR_XIR_VALUE_BAD_ARGUMENT);++run->rejections;XrXirValue now={0};CHECK(xr_xir_cell_read(cell,&now)==XR_XIR_VALUE_OK && !memcmp(&old,&now,sizeof(old)) && runtime_attempts==attempts);xr_xir_value_drop(&now);}
        xr_xir_value_drop(&old);for(unsigned i=0;i<2;++i){XrXirDomainBudgetStats after=xr_xir_domain_budget_stats(run->domains[i]);CHECK(after.requested_bytes==before[i].requested_bytes && after.requested_call_bytes==before[i].requested_call_bytes && after.work>=before[i].work);}
    }
}
static void policy_graph(unsigned backend,unsigned shape,unsigned count,unsigned broken,unsigned close_order,unsigned drop_order) {
    CHECK(count && count<=16 && shape<2);CyclePolicyRun run={0};run.shape=shape;run.count=count;XrXirProgram *program=cycle_program(&run.fixture,backend);XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    for(unsigned i=0;i<2;++i){CHECK(xr_xir_instance_new(program,&config,&run.instances[i])==XR_XIR_CALL_READY);run.domains[i]=run.instances[i]->domain;}
    CHECK(run.domains[0]!=run.domains[1]);xr_xir_compile_program_drop(program);CHECK(!run.fixture.releases);
    for(unsigned i=0;i<count;++i)policy_make(&run,i,0,shape);policy_make(&run,count,1,1-shape);
    XrXirValueAdmission admission=instance_candidate_admission(run.instances[0]);
    for(unsigned i=0;i<count;++i)for(unsigned c=0;c<run.captures[i];++c)CHECK(xr_xir_cell_write(policy_cell(&run.functions[i],c),&run.functions[(i+1+c)%count],&admission)==XR_XIR_VALUE_OK);
    for(unsigned i=0;i<count;++i)for(unsigned c=0;c<run.captures[i];++c){XrXirValue edge={0};CHECK(xr_xir_cell_read(policy_cell(&run.functions[i],c),&edge)==XR_XIR_VALUE_OK && edge.type==run.functions[(i+1+c)%count].type && edge.payload==run.functions[(i+1+c)%count].payload);xr_xir_value_drop(&edge);}
    policy_reject(&run,0,count,0);policy_reject(&run,count,0,1);CHECK(run.rejections==6);
    for(unsigned i=0;i<count;++i)policy_invoke(&run,&run.functions[i],1,42);
    if(shape)policy_invoke(&run,&run.functions[0],2,84);
    if(broken){policy_invoke(&run,&run.functions[0],0,17);for(unsigned c=0;c<run.captures[0];++c){XrXirValue base={0};CHECK(xr_xir_cell_read(policy_cell(&run.functions[0],c),&base)==XR_XIR_VALUE_OK);const XrXirFunctionBinding *b=xr_xir_function_binding(&base);CHECK(b && !b->capture_count);policy_invoke(&run,&base,1,shape?(c?401:301):201);xr_xir_value_drop(&base);}}
    for(unsigned i=0;i<2;++i){unsigned at=i?1-close_order:close_order;CHECK(xr_xir_instance_free(run.instances[at])==XR_XIR_CALL_READY);run.instances[at]=NULL;CHECK(!run.fixture.releases && run.domains[at]->closed);}
    size_t attempts=runtime_attempts;for(unsigned i=0;i<=count;++i)xr_xir_value_drop(&run.functions[i]);
    for(unsigned i=0;i<count;++i){unsigned at=drop_order?count-1-i:i;xr_xir_value_drop(&run.aliases[at]);if(!broken && i+1<count)for(unsigned j=0;j<count;++j)for(unsigned c=0;c<=run.captures[j];++c)CHECK(cycle_physical(run.addresses[j][c]));for(unsigned j=0;j<count;++j)if(run.aliases[j].type){CHECK(xr_xir_value_valid(&run.aliases[j]));for(unsigned c=0;c<run.captures[j];++c){XrXirValue value={0};CHECK(xr_xir_cell_read(policy_cell(&run.aliases[j],c),&value)==XR_XIR_VALUE_OK);xr_xir_value_drop(&value);}}}
    for(unsigned i=0;i<count;++i)for(unsigned c=0;c<=run.captures[i];++c)CHECK(!cycle_physical(run.addresses[i][c]));
    CHECK(!run.fixture.releases && xr_xir_value_valid(&run.aliases[count]));xr_xir_value_drop(&run.aliases[count]);
    CHECK(attempts==runtime_attempts && run.fixture.releases==1 && !run.fixture.vm && !run.fixture.entries && !runtime_live && !runtime_bytes && !effects_compile_live && !effects_compile_bytes);
    printf("Cycle batch policy backend=%u shape=%u ring=%u broken=%u close=%u drop=%u functions=%u mainCells=%u peerCells=%u foreignRefusals6 actualFrames allCapturedCells SourceBreak codeLease1 noAllocateDrop physical0/0\n",backend,shape,count,broken,close_order,drop_order,count+1,count*(shape+1),2-shape);
}
int main(int argc,char **argv) {
    CHECK(argc==2);(void)cycle_original_source_main;const char *names[]={"0","1","vm","native"};unsigned backend=0;while(backend<4 && strcmp(argv[1],names[backend]))++backend;CHECK(backend<4);
    const unsigned sizes[]={1,2,3,16};for(unsigned shape=0;shape<2;++shape)for(unsigned n=0;n<4;++n)for(unsigned broken=0;broken<2;++broken)for(unsigned close=0;close<2;++close)for(unsigned drop=0;drop<2;++drop)policy_graph(backend,shape,sizes[n],broken,close,drop);return 0;
}
