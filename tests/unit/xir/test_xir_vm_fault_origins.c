/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_vm_fault_origins.c - Authenticated output failures and protocol rejection
 */
/* The immutable test fixture's canonical bodies and observation wrappers are
 * reused verbatim. Its original nine-graph main is compiled but never called. */
#define main immutable_pending_exit_main
#include "test_xir_vm_pending_exit.c"
#undef main

typedef struct RoleOutput {
    XrXirOutputStatus initial;
    int64_t values[16];
    uint32_t count,writes,false_outputs;
    bool failed;
} RoleOutput;
static XrXirOutputStatus role_output(void *context,const XrXirOutputGroup *group) {
    RoleOutput *out=context;
    CHECK(group->stream==XR_XIR_STDOUT && !group->line && group->count==1);
    const XrXirValue *value=&group->values[0];
    if(value->type==XR_XIR_STRING) {
        const char *bytes=NULL;size_t length=0;static const char expected[]={'a',0,'b'};
        CHECK(xr_xir_string_view(value,&bytes,&length) && length==3 && !memcmp(bytes,expected,3));
        ++out->writes;out->failed=true;return out->initial;
    }
    if(value->type==XR_XIR_BOOL) {CHECK(!value->payload);++out->false_outputs;return XR_XIR_OUTPUT_OK;}
    CHECK(value->type==XR_XIR_I64 && out->count<16);
    out->values[out->count++]=(int64_t)value->payload;
    if(!out->failed) {out->failed=true;return out->initial;}
    return XR_XIR_OUTPUT_OK;
}
enum RoleKind {ROLE_OUTPUT,ROLE_TASK,ROLE_WRITE};
static bool role_case(uint32_t kind,XrXirOutputStatus provider_status,
    XrXirCallStatus expected_status,bool execution_failure,unsigned order) {
    CHECK(!runtime_live && !runtime_bytes);observed_count=0;observed_call=NULL;
    LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
    uint32_t fixture=kind==ROLE_TASK?PENDING_AWAIT:kind==ROLE_WRITE?PENDING_WRITE_STREAM:PENDING_DIRECT;
    XrXirProgram *program=cleanup_program(&owner.context,fixture);
    XrXirInstance *instances[2]={NULL,NULL};RoleOutput outputs[2]={{.initial=provider_status},{.initial=provider_status}};
    for(unsigned i=0;i<2;++i) {
        XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,role_output,&outputs[i]};
        CHECK(xr_xir_instance_new(program,&config,&instances[i])==XR_XIR_CALL_READY);
    }
    xr_xir_compile_program_drop(program);bool valid=true;
    for(unsigned i=0;i<2;++i) {
        uint32_t frontier_begin=observed_count;
        XrXirValue argument={0};
        if(kind==ROLE_WRITE)CHECK(xr_xir_string_new(instances[i]->domain,"a\0b",3,&argument)==XR_XIR_VALUE_OK);
        CHECK(xr_xir_instance_start(instances[i],1,kind==ROLE_WRITE?&argument:NULL,kind==ROLE_WRITE?1u:0u)==XR_XIR_CALL_READY);
        xr_xir_value_drop(&argument);
        XrXirInstanceResult result=xr_xir_instance_poll_bounded(instances[i],UINT64_MAX);
        XrXirDomainBudgetStats first=xr_xir_domain_budget_stats(instances[i]->domain);
        bool oracle=result.epoch==1 && result.outcome.status==expected_status && observed_call;
        if(kind==ROLE_WRITE) {
            oracle=oracle && outputs[i].writes==1 && outputs[i].false_outputs==1 && outputs[i].count==1 && outputs[i].values[0]==6;
            XrXirValue result_value={0};XrXirCallStatus take=xr_xir_instance_take_result(instances[i],&result_value);
            oracle=oracle && take==XR_XIR_CALL_RETURNED && result_value.type==XR_XIR_I64 && result_value.payload==7;
            xr_xir_value_drop(&result_value);
        } else {
            oracle=oracle && !result.outcome.value.type && !result.outcome.value.reserved && !result.outcome.value.payload && xr_xir_panic_empty(&result.outcome.panic);
            uint32_t count=execution_failure?2u:1u;
            oracle=oracle && outputs[i].count==count && outputs[i].values[0]==7 && (!execution_failure || outputs[i].values[1]==6);
            XrXirValue empty={0},sentinel={XR_XIR_I64,0,91};
            XrXirCallStatus take_empty=xr_xir_instance_take_result(instances[i],&empty);
            XrXirCallStatus take_occupied=xr_xir_instance_take_result(instances[i],&sentinel);
            oracle=oracle && take_empty==(kind==ROLE_TASK?expected_status:XR_XIR_CALL_BAD_STATE) && take_occupied==(kind==ROLE_TASK?expected_status:XR_XIR_CALL_BAD_ARGUMENT);
            oracle=oracle && !empty.type && !empty.reserved && !empty.payload && sentinel.type==XR_XIR_I64 && !sentinel.reserved && sentinel.payload==91;
            XrXirInstanceResult repeated=xr_xir_instance_poll_bounded(instances[i],UINT64_MAX);
            oracle=oracle && repeated.epoch==1 && repeated.outcome.status==expected_status;
        }
        if(execution_failure || kind==ROLE_WRITE) {
            oracle=oracle && observed_count==frontier_begin+2 && observed_frontiers[frontier_begin]==3 && !observed_frontiers[frontier_begin+1] && !xr_xir_call_cleanup_incomplete(observed_call);
        } else {
            oracle=oracle && observed_count==frontier_begin+1 && observed_frontiers[frontier_begin]==3 && xr_xir_call_cleanup_incomplete(observed_call);
        }
        if(execution_failure) {
            XrXirCallStatus restarted=xr_xir_instance_start(instances[i],1,NULL,0);
            oracle=oracle && restarted==XR_XIR_CALL_READY;
            if(restarted==XR_XIR_CALL_READY) {
                result=xr_xir_instance_poll_bounded(instances[i],UINT64_MAX);
                XrXirValue owned={0};XrXirCallStatus taken=xr_xir_instance_take_result(instances[i],&owned);
                oracle=oracle && result.epoch==2 && result.outcome.status==XR_XIR_CALL_RETURNED && taken==XR_XIR_CALL_RETURNED && owned.type==XR_XIR_I64 && owned.payload==7;
                xr_xir_value_drop(&owned);
                oracle=oracle && outputs[i].count==4 && outputs[i].values[2]==7 && outputs[i].values[3]==6;
                XrXirDomainBudgetStats second=xr_xir_domain_budget_stats(instances[i]->domain);
                oracle=oracle && second.requested_call_bytes>first.requested_call_bytes && second.work>first.work;
            }
        } else if(kind==ROLE_OUTPUT) {
            XrXirCallStatus rejected=xr_xir_instance_start(instances[i],1,NULL,0);
            oracle=oracle && rejected==XR_XIR_CALL_BAD_STATE;
            XrXirInstanceResult cached=xr_xir_instance_poll_bounded(instances[i],UINT64_MAX);
            oracle=oracle && cached.epoch==1 && cached.outcome.status==expected_status && outputs[i].count==1;
        }
        fprintf(stderr,"origin kind%u raw%u expectedCall%u instance%u order%u values",kind,(unsigned)provider_status,(unsigned)expected_status,i,order);
        for(uint32_t n=0;n<outputs[i].count;++n)fprintf(stderr," %lld",(long long)outputs[i].values[n]);
        fprintf(stderr," initialCall%u finalCall%u epoch%llu frontierCount%u oracle=%s\n",(unsigned)expected_status,(unsigned)result.outcome.status,(unsigned long long)result.epoch,observed_count-frontier_begin,oracle?"PASS":"FAIL");
        valid=valid && oracle;
    }
    XrXirCallStatus free0=xr_xir_instance_free(instances[order]),free1=xr_xir_instance_free(instances[1-order]);
    valid=valid && free0==XR_XIR_CALL_READY && free1==XR_XIR_CALL_READY;
    CHECK(!runtime_live && !runtime_bytes);
    XrCompileResourceStats fees=library_compile_stats(&owner.context);CHECK(fees.live_bytes==owner.baseline.live_bytes);
    library_compile_owner_drop(&owner);CHECK(!source_program_compile_live && !source_program_compile_bytes);
    fprintf(stderr,"origin kind%u raw%u order%u physical0 allocated%llu peak%llu work%llu free%u/%u oracle=%s\n",kind,(unsigned)provider_status,order,(unsigned long long)fees.allocated_bytes,(unsigned long long)fees.peak_bytes,(unsigned long long)fees.work,(unsigned)free0,(unsigned)free1,valid?"PASS":"FAIL");
    return valid;
}


static XrXirProgram *role_wait_program(const XrXirCompileContext *context) {
    XrXirInstruction init={.op=XR_XIR_RETURN};
    XrXirInstruction ops[]={
        {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=7},
        {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=6},
        {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=1},
        {.op=XR_XIR_CLEANUP_REGISTER,.args={0,1},.targets={1},.immediate=2},
        {.op=XR_XIR_TIMER_AFTER_MS,.args={2}},
        {.op=XR_XIR_OUTPUT,.args={0},.immediate=1},
        {.op=XR_XIR_RETURN,.args={0}}};
    XrXirInstruction helper[]={{.op=XR_XIR_OUTPUT,.args={0},.immediate=1},{.op=XR_XIR_RETURN}};
    XrXirBlock blocks[]={{0,4,0,0},{4,3,0,4}},init_block={.count=1},helper_block={.count=2};
    XrXirType parameter=XR_XIR_I64;uint32_t operand=1;
    XrXirFunction functions[]={
        {.name="init",.name_length=4,.result=XR_XIR_UNIT,.blocks=&init_block,.block_count=1,.instructions=&init,.instruction_count=1},
        {.name="main",.name_length=4,.result=XR_XIR_I64,.blocks=blocks,.block_count=2,.instructions=ops,.instruction_count=7,.operands=&operand,.operand_count=1},
        {.name="cleanup",.name_length=7,.parameters=&parameter,.parameter_count=1,.result=XR_XIR_UNIT,.blocks=&helper_block,.block_count=1,.instructions=helper,.instruction_count=2}};
    XrXirFunctionIdentity identities[3]={{0}};identities[1].exported=1;identities[2].cleanup_owner=2;
    XrXirSourceModule source={.name="root",.name_length=4};
    XrXirDeclarations declarations={.modules=&source,.module_count=1,.functions=identities,.root_module=0,.entry_function=1};
    XrXirModule built={.stage=XR_XIR_BUILT,.functions=functions,.function_count=3,.declarations=&declarations,.linkage_kind=XR_XIR_PROGRAM};
    XrXirArtifact *checked=NULL,*read=NULL,*special=NULL,*lowered=NULL;XrXirCheckedPacket packet={0};XrXirProgram *program=NULL;
    XrXirDiagnostic diagnostic={0};XrXirStatus status=xr_xir_compile_check(context,&built,&checked,&diagnostic);
    if(status!=XR_XIR_OK)fprintf(stderr,"wait fixture check%u f%u b%u i%u reason%u\n",status,diagnostic.function,diagnostic.block,diagnostic.instruction,diagnostic.reason);
    CHECK(status==XR_XIR_OK);
    memset(ops,0xa5,sizeof(ops));memset(helper,0xa5,sizeof(helper));memset(blocks,0xa5,sizeof(blocks));
    CHECK(xr_xir_compile_checked_write(checked,&packet,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(checked);
    CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&read,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(read,&special,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(read);
    CHECK(xr_xir_compile_artifact_verify(special,NULL)==XR_XIR_OK);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(special,&target,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(special);
    CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK && program && !lowered);
    return program;
}
static bool role_reject_wait(XrXirInstance *instance,uint64_t epoch,uint64_t wake) {
    XrXirWaitRequest output;memset(&output,0xa5,sizeof(output));XrXirWaitRequest before=output;
    XrXirDomainBudgetStats fee=xr_xir_domain_budget_stats(instance->domain);size_t live=runtime_live,bytes=runtime_bytes;
    bool valid=xr_xir_instance_wait_request(instance,epoch,wake,&output)==XR_XIR_CALL_BAD_STATE && !memcmp(&output,&before,sizeof(output));
    valid=valid && xr_xir_instance_resume(instance,epoch,wake)==XR_XIR_CALL_BAD_STATE;
    XrXirDomainBudgetStats after=xr_xir_domain_budget_stats(instance->domain);
    return valid && fee.requested_bytes==after.requested_bytes && fee.requested_call_bytes==after.requested_call_bytes && fee.work==after.work && live==runtime_live && bytes==runtime_bytes;
}
static bool role_wait_case(unsigned order) {
    CHECK(!runtime_live && !runtime_bytes);observed_count=0;observed_call=NULL;
    LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
    XrXirProgram *program=role_wait_program(&owner.context);XrXirInstance *instances[2]={NULL,NULL};
    RoleOutput outputs[2]={{.initial=XR_XIR_OUTPUT_ERROR},{.initial=XR_XIR_OUTPUT_ERROR}};
    for(unsigned i=0;i<2;++i) {
        XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,role_output,&outputs[i]};
        CHECK(xr_xir_instance_new(program,&config,&instances[i])==XR_XIR_CALL_READY);
    }
    xr_xir_compile_program_drop(program);bool valid=true;
    for(unsigned i=0;i<2;++i) {
        CHECK(xr_xir_instance_start(instances[i],1,NULL,0)==XR_XIR_CALL_READY);
        XrXirInstanceResult first=xr_xir_instance_poll_bounded(instances[i],UINT64_MAX);
        bool oracle=first.epoch==1 && first.outcome.status==XR_XIR_CALL_SUSPENDED && first.outcome.wake && !outputs[i].count;
        XrXirWaitRequest request={0};XrXirCallStatus queried=xr_xir_instance_wait_request(instances[i],first.epoch,first.outcome.wake,&request);
        oracle=oracle && queried==XR_XIR_CALL_READY && request.kind==XR_XIR_WAIT_TIMER_MS && !request.reserved && request.after_ms==1 && !request.subject && !request.generation && !request.ticket;
        oracle=oracle && role_reject_wait(instances[i],first.epoch+1,first.outcome.wake) && role_reject_wait(instances[i],first.epoch,first.outcome.wake+1);
        XrXirInstanceResult preserved=xr_xir_instance_poll_bounded(instances[i],UINT64_MAX);
        oracle=oracle && preserved.epoch==first.epoch && preserved.outcome.status==first.outcome.status && preserved.outcome.wake==first.outcome.wake && !outputs[i].count;
        XrXirCallStatus resumed=xr_xir_instance_resume(instances[i],first.epoch,first.outcome.wake);
        oracle=oracle && resumed==XR_XIR_CALL_READY && role_reject_wait(instances[i],first.epoch,first.outcome.wake);
        XrXirInstanceResult failed=xr_xir_instance_poll_bounded(instances[i],UINT64_MAX);
        oracle=oracle && failed.epoch==1 && failed.outcome.status==XR_XIR_CALL_OUTPUT_ERROR && outputs[i].count==2 && outputs[i].values[0]==7 && outputs[i].values[1]==6 && observed_call && !xr_xir_call_cleanup_incomplete(observed_call);
        XrXirDomainBudgetStats first_fee=xr_xir_domain_budget_stats(instances[i]->domain);
        XrXirCallStatus start=xr_xir_instance_start(instances[i],1,NULL,0);oracle=oracle && start==XR_XIR_CALL_READY;
        XrXirInstanceResult second=xr_xir_instance_poll_bounded(instances[i],UINT64_MAX);
        oracle=oracle && second.epoch==2 && second.outcome.status==XR_XIR_CALL_SUSPENDED && second.outcome.wake;
        oracle=oracle && role_reject_wait(instances[i],first.epoch,first.outcome.wake);
        oracle=oracle && xr_xir_instance_resume(instances[i],second.epoch,second.outcome.wake)==XR_XIR_CALL_READY;
        XrXirInstanceResult returned=xr_xir_instance_poll_bounded(instances[i],UINT64_MAX);XrXirValue owned={0};
        XrXirCallStatus take=xr_xir_instance_take_result(instances[i],&owned);
        oracle=oracle && returned.epoch==2 && returned.outcome.status==XR_XIR_CALL_RETURNED && take==XR_XIR_CALL_RETURNED && owned.type==XR_XIR_I64 && owned.payload==7 && outputs[i].count==4 && outputs[i].values[2]==7 && outputs[i].values[3]==6;
        xr_xir_value_drop(&owned);XrXirDomainBudgetStats second_fee=xr_xir_domain_budget_stats(instances[i]->domain);
        oracle=oracle && second_fee.requested_call_bytes>first_fee.requested_call_bytes && second_fee.work>first_fee.work;
        fprintf(stderr,"public TIMER instance%u order%u oldepoch%llu oldwake%llu newepoch%llu newwake%llu originalfault13 cleanup6 retry7 exact-stale-preservation oracle=%s\n",i,order,(unsigned long long)first.epoch,(unsigned long long)first.outcome.wake,(unsigned long long)second.epoch,(unsigned long long)second.outcome.wake,oracle?"PASS":"FAIL");
        valid=valid && oracle;
    }
    XrXirCallStatus free0=xr_xir_instance_free(instances[order]),free1=xr_xir_instance_free(instances[1-order]);valid=valid && free0==XR_XIR_CALL_READY && free1==XR_XIR_CALL_READY;
    CHECK(!runtime_live && !runtime_bytes);XrCompileResourceStats fees=library_compile_stats(&owner.context);CHECK(fees.live_bytes==owner.baseline.live_bytes);
    library_compile_owner_drop(&owner);CHECK(!source_program_compile_live && !source_program_compile_bytes);
    fprintf(stderr,"public TIMER order%u physical0 allocated%llu peak%llu work%llu oracle=%s\n",order,(unsigned long long)fees.allocated_bytes,(unsigned long long)fees.peak_bytes,(unsigned long long)fees.work,valid?"PASS":"FAIL");
    return valid;
}
int main(void) {
    bool valid=true;
    const XrXirOutputStatus provider[]={XR_XIR_OUTPUT_ERROR,XR_XIR_OUTPUT_OOM,XR_XIR_OUTPUT_LIMIT,XR_XIR_OUTPUT_BAD_ABI,(XrXirOutputStatus)99};
    const XrXirCallStatus expected[]={XR_XIR_CALL_OUTPUT_ERROR,XR_XIR_CALL_OOM,XR_XIR_CALL_LIMIT,XR_XIR_CALL_BAD_ABI,XR_XIR_CALL_BAD_ARGUMENT};
    for(unsigned order=0;order<2;++order) {
        for(unsigned i=0;i<5;++i) {bool result=role_case(ROLE_OUTPUT,provider[i],expected[i],i<3,order);valid=valid && result;}
        for(unsigned i=0;i<3;++i) {bool result=role_case(ROLE_TASK,provider[i],expected[i],true,order);valid=valid && result;}
        bool result=role_case(ROLE_WRITE,XR_XIR_OUTPUT_ERROR,XR_XIR_CALL_RETURNED,false,order);valid=valid && result;
    }
    for(unsigned order=0;order<2;++order) {bool actual=role_wait_case(order);valid=valid && actual;}
    library_compile_observer_free();CHECK(valid);return 0;
}
