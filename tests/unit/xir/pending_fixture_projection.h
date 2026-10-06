/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_vm_pending_exit.c - Legal execution faults must execute registered cleanup
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_task.h"
#include "xir/xxir_call_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_library_compile_owner.h"

/* Only the canonical Call definition is renamed. VM references resolve to
 * the observing wrapper, which delegates the original view and frontier once. */
#define xr_xir_call_cleanup_frontier cleanup_canonical_frontier
#include "runtime_observer_projection.h"
#undef xr_xir_call_cleanup_frontier
#include "xir/xxir_task.c"

static uint32_t observed_frontiers[32], observed_count;
static XrXirCall *observed_call;
XR_FUNC XrXirCallStatus xr_xir_call_cleanup_frontier(XrXirCallView *view, uint32_t frontier) {
    if (view) {
        XrXirCallView copied = *view;
        CHECK(cleanup_canonical_frontier(&copied, 17) == XR_XIR_CALL_BAD_STATE);
    }
    XrXirCallStatus status = cleanup_canonical_frontier(view, frontier);
    if (status == XR_XIR_CALL_READY) {
        CHECK(observed_count < 32);
        observed_frontiers[observed_count++] = frontier;
        observed_call = view->activation;
    }
    return status;
}


enum PendingMode { PENDING_DIRECT,PENDING_CALLER,PENDING_AWAIT,PENDING_LIFO,
    PENDING_CLEANUP_FAIL,PENDING_CANCEL_AFTER,PENDING_CANCEL_BEFORE,PENDING_RETRY,PENDING_WRITE_STREAM };
static XrXirProgram *cleanup_program(const XrXirCompileContext *context,uint32_t mode) {
    XrXirInstruction done={.op=XR_XIR_RETURN};
    XrXirInstruction ops[10]={
        {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=7},
        {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=6},
        {.op=XR_XIR_CLEANUP_REGISTER,.args={0,1},.targets={1},.immediate=2},
        {.op=XR_XIR_OUTPUT,.args={0},.immediate=1},
        {.op=XR_XIR_RETURN,.args={0}}};
    XrXirBlock blocks[4]={{0,3,0,0},{3,2,0,3}};
    uint32_t op_count=5,block_count=2,operands[]={1,2};
    XrXirInstruction helper[]={{.op=XR_XIR_OUTPUT,.args={0},.immediate=1},{.op=XR_XIR_RETURN}};
    XrXirInstruction worker[5]={
        {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=7},
        {.op=XR_XIR_OUTPUT,.args={0},.immediate=1},
        {.op=XR_XIR_RETURN,.args={0}}};
    XrXirBlock worker_blocks[2]={{0,3,0,0}};
    uint32_t worker_op_count=3,worker_block_count=1,worker_operand=mode==PENDING_CALLER?1u:0u;
    if(mode==PENDING_CALLER) {
        ops[3]=(XrXirInstruction){.op=XR_XIR_CALL,.type=XR_XIR_I64,.immediate=3};
        ops[4].args[0]=3;
        worker[1]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=9};
        worker[2]=(XrXirInstruction){.op=XR_XIR_CLEANUP_REGISTER,.args={0,1},.targets={1},.immediate=4};
        worker[3]=(XrXirInstruction){.op=XR_XIR_OUTPUT,.args={0},.immediate=1};
        worker[4]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={0}};
        worker_blocks[0]=(XrXirBlock){0,3,0,0};worker_blocks[1]=(XrXirBlock){3,2,0,3};
        worker_op_count=5;worker_block_count=2;
    } else if(mode==PENDING_LIFO) {
        ops[2]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=8};
        ops[3]=(XrXirInstruction){.op=XR_XIR_CLEANUP_REGISTER,.args={0,1},.targets={1},.immediate=2};
        ops[4]=(XrXirInstruction){.op=XR_XIR_CLEANUP_REGISTER,.args={1,1},.targets={2},.immediate=2};
        ops[5]=(XrXirInstruction){.op=XR_XIR_OUTPUT,.args={0},.immediate=1};
        ops[6]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={0}};
        blocks[0]=(XrXirBlock){0,4,0,0};blocks[1]=(XrXirBlock){4,1,0,4};blocks[2]=(XrXirBlock){5,2,0,5};
        op_count=7;block_count=3;
    } else if(mode==PENDING_AWAIT) {
        ops[3]=(XrXirInstruction){.op=XR_XIR_GO,.type=256,.immediate=3};
        ops[4]=(XrXirInstruction){.op=XR_XIR_TASK_AWAIT,.args={3},.targets={2,3}};
        ops[5]=(XrXirInstruction){.op=XR_XIR_INVOKE_RESULT,.type=XR_XIR_I64,.immediate=4};
        ops[6]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={5}};
        ops[7]=(XrXirInstruction){.op=XR_XIR_INVOKE_ERROR,.type=XR_XIR_ERROR,.immediate=4};
        ops[8]=(XrXirInstruction){.op=XR_XIR_THROW,.args={7}};
        blocks[1]=(XrXirBlock){3,2,0,3};blocks[2]=(XrXirBlock){5,2,0,3};blocks[3]=(XrXirBlock){7,2,0,3};
        op_count=9;block_count=4;
    }
    if(mode==PENDING_WRITE_STREAM) {
        operands[0]=2;
        ops[3]=(XrXirInstruction){.op=XR_XIR_WRITE_STREAM,.type=XR_XIR_BOOL,.args={0},.immediate=1};
        ops[4]=(XrXirInstruction){.op=XR_XIR_OUTPUT,.args={4},.immediate=1};
        ops[5]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={1}};
        blocks[1]=(XrXirBlock){3,3,0,3};op_count=6;
    }
    XrXirType string_parameter=XR_XIR_STRING;
    XrXirType parameter=XR_XIR_I64;
    XrXirBlock init_block={.count=1},helper_block={.count=2};
    XrXirInstruction script[]={{.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=7},{.op=XR_XIR_RETURN,.args={0}}};
    XrXirBlock script_block={.count=2};
    XrXirFunction functions[]={
        {.name="init",.name_length=4,.result=XR_XIR_UNIT,.blocks=&init_block,.block_count=1,.instructions=&done,.instruction_count=1},
        {.name="main",.name_length=4,.parameters=mode==PENDING_WRITE_STREAM?&string_parameter:NULL,.parameter_count=mode==PENDING_WRITE_STREAM?1u:0u,.result=XR_XIR_I64,.blocks=blocks,.block_count=block_count,.instructions=ops,.instruction_count=op_count,.operands=operands,.operand_count=mode==PENDING_LIFO?2u:1u},
        {.name="cleanup",.name_length=7,.parameters=&parameter,.parameter_count=1,.result=XR_XIR_UNIT,.blocks=&helper_block,.block_count=1,.instructions=helper,.instruction_count=2},
        {.name="worker",.name_length=6,.result=XR_XIR_I64,.blocks=worker_blocks,.block_count=worker_block_count,.instructions=worker,.instruction_count=worker_op_count,.operands=mode==PENDING_CALLER?&worker_operand:NULL,.operand_count=mode==PENDING_CALLER?1u:0u},
        {.name="child_cleanup",.name_length=13,.parameters=&parameter,.parameter_count=1,.result=XR_XIR_UNIT,.blocks=&helper_block,.block_count=1,.instructions=helper,.instruction_count=2},
        {.name="script",.name_length=6,.result=XR_XIR_I64,.blocks=&script_block,.block_count=1,.instructions=script,.instruction_count=2}};
    XrXirFunctionIdentity identities[6]={{0}};identities[1].exported=1;identities[2].cleanup_owner=2;identities[4].cleanup_owner=4;identities[5].exported=1;
    XrXirTypeNode task_node={.kind=XR_XIR_TYPE_TASK,.element=XR_XIR_I64};XrXirTypes types={.nodes=&task_node,.count=1};
    XrXirSourceModule source={.name="root",.name_length=4};
    XrXirDeclarations declarations={.modules=&source,.module_count=1,.functions=identities,.root_module=0,.entry_function=mode==PENDING_WRITE_STREAM?5u:1u};
    XrXirModule built={.stage=XR_XIR_BUILT,.functions=functions,.function_count=mode==PENDING_WRITE_STREAM?6u:5u,.declarations=&declarations,.linkage_kind=XR_XIR_PROGRAM};
    if(mode==PENDING_AWAIT)built.types=&types;
    XrXirArtifact *checked=NULL,*read=NULL,*special=NULL,*lowered=NULL;XrXirCheckedPacket packet={0};XrXirProgram *program=NULL;
    XrXirDiagnostic diagnostic={0};XrXirStatus status=xr_xir_compile_check(context,&built,&checked,&diagnostic);
    if(status!=XR_XIR_OK)fprintf(stderr,"cleanup mode%u check%u f%u b%u i%u reason%u\n",mode,status,diagnostic.function,diagnostic.block,diagnostic.instruction,diagnostic.reason);
    CHECK(status==XR_XIR_OK);
    memset(ops,0xa5,sizeof(ops));memset(blocks,0xa5,sizeof(blocks));memset(helper,0xa5,sizeof(helper));memset(worker,0xa5,sizeof(worker));
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



typedef struct PendingOutput {
    int64_t values[16];uint32_t count,mode,writes,false_results;XrXirInstance *instance;
} PendingOutput;
static XrXirOutputStatus pending_output(void *context,const XrXirOutputGroup *group) {
    PendingOutput *output=context;
    CHECK(group->stream==XR_XIR_STDOUT && !group->line && group->count==1);
    if(output->mode==PENDING_WRITE_STREAM && group->values[0].type==XR_XIR_STRING) {
        const char *bytes=NULL;size_t length=0;static const char expected[]={'a',0,'b'};
        CHECK(xr_xir_string_view(&group->values[0],&bytes,&length) && length==3 && !memcmp(bytes,expected,3));
        ++output->writes;return XR_XIR_OUTPUT_ERROR;
    }
    if(output->mode==PENDING_WRITE_STREAM && group->values[0].type==XR_XIR_BOOL) {
        CHECK(!group->values[0].payload);++output->false_results;return XR_XIR_OUTPUT_OK;
    }
    CHECK(group->values[0].type==XR_XIR_I64 && output->count<16);
    int64_t value=(int64_t)group->values[0].payload;
    output->values[output->count++]=value;
    if(output->count==1 && output->mode!=PENDING_WRITE_STREAM) {
        if(output->mode==PENDING_CANCEL_BEFORE)CHECK(xr_xir_instance_stop(output->instance)==XR_XIR_CALL_READY);
        return XR_XIR_OUTPUT_ERROR;
    }
    if(output->mode==PENDING_CLEANUP_FAIL && value==6)return XR_XIR_OUTPUT_LIMIT;
    return XR_XIR_OUTPUT_OK;
}
static bool pending_case(uint32_t mode,unsigned order) {
    CHECK(!runtime_live && !runtime_bytes);observed_count=0;observed_call=NULL;
    LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
    XrXirProgram *program=cleanup_program(&owner.context,mode);
    XrXirInstance *instances[2]={NULL,NULL};PendingOutput outputs[2]={{.mode=mode},{.mode=mode}};
    for(unsigned i=0;i<2;++i) {
        XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,pending_output,&outputs[i]};
        CHECK(xr_xir_instance_new(program,&config,&instances[i])==XR_XIR_CALL_READY);
        outputs[i].instance=instances[i];
    }
    xr_xir_compile_program_drop(program);bool valid=true;
    for(unsigned i=0;i<2;++i) {
        XrXirValue argument={0};
        if(mode==PENDING_WRITE_STREAM)CHECK(xr_xir_string_new(instances[i]->domain,"a\0b",3,&argument)==XR_XIR_VALUE_OK);
        CHECK(xr_xir_instance_start(instances[i],1,mode==PENDING_WRITE_STREAM?&argument:NULL,mode==PENDING_WRITE_STREAM?1u:0u)==XR_XIR_CALL_READY);
        xr_xir_value_drop(&argument);
        XrXirDomainBudgetStats before=xr_xir_domain_budget_stats(instances[i]->domain);
        XrXirInstanceResult result;
        if(mode==PENDING_CANCEL_AFTER) {
            uint32_t steps=0;
            do {result=xr_xir_instance_poll_bounded(instances[i],1);CHECK(++steps<1024);} while(!outputs[i].count);
            XrXirCallStatus stopped=xr_xir_instance_stop(instances[i]);
            fprintf(stderr,"cancel after accepted OUTPUT status%u previouspoll%u (old direct-abort may already be terminal)\n",stopped,result.outcome.status);
        }
        result=xr_xir_instance_poll_bounded(instances[i],UINT64_MAX);
        XrXirDomainBudgetStats after=xr_xir_domain_budget_stats(instances[i]->domain);
        CHECK(after.requested_call_bytes>=before.requested_call_bytes && after.requested_bytes>=before.requested_bytes && after.work>=before.work);
        static const int64_t direct[]={7,6},caller[]={7,9,6},lifo[]={7,8,6},write[]={6};
        const int64_t *expected=mode==PENDING_CALLER?caller:mode==PENDING_LIFO?lifo:mode==PENDING_WRITE_STREAM?write:direct;
        uint32_t count=mode==PENDING_CALLER||mode==PENDING_LIFO?3:mode==PENDING_WRITE_STREAM?1:2;
        XrXirCallStatus expected_status=mode==PENDING_CANCEL_BEFORE?XR_XIR_CALL_CANCELLED:mode==PENDING_WRITE_STREAM?XR_XIR_CALL_RETURNED:XR_XIR_CALL_OUTPUT_ERROR;
        bool oracle=result.epoch==1 && result.outcome.status==expected_status && outputs[i].count==count && !memcmp(outputs[i].values,expected,count*sizeof(*expected));
        if(mode==PENDING_WRITE_STREAM)oracle=result.epoch==1 && result.outcome.status==XR_XIR_CALL_RETURNED && outputs[i].writes==1 && outputs[i].false_results==1 && outputs[i].count==1 && outputs[i].values[0]==6;
        if(result.outcome.status!=XR_XIR_CALL_RETURNED) {
            CHECK(!result.outcome.value.type && !result.outcome.value.reserved && !result.outcome.value.payload);
            CHECK(xr_xir_panic_empty(&result.outcome.panic));
            XrXirValue empty={0},sentinel={XR_XIR_I64,0,91};
            XrXirCallStatus empty_status=xr_xir_instance_take_result(instances[i],&empty);
            XrXirCallStatus occupied_status=xr_xir_instance_take_result(instances[i],&sentinel);
            CHECK(empty_status==(mode==PENDING_AWAIT?XR_XIR_CALL_OUTPUT_ERROR:XR_XIR_CALL_BAD_STATE));
            CHECK(occupied_status==(mode==PENDING_AWAIT?XR_XIR_CALL_OUTPUT_ERROR:XR_XIR_CALL_BAD_ARGUMENT));
            CHECK(!empty.type && !empty.reserved && !empty.payload);
            CHECK(sentinel.type==XR_XIR_I64 && !sentinel.reserved && sentinel.payload==91);
            XrXirInstanceResult cached=xr_xir_instance_poll_bounded(instances[i],UINT64_MAX);
            CHECK(cached.epoch==result.epoch && cached.outcome.status==result.outcome.status);
        }
        fprintf(stderr,"pending mode%u instance%u order%u status%u epoch%llu calls%u values",mode,i,order,result.outcome.status,(unsigned long long)result.epoch,outputs[i].count);
        for(uint32_t n=0;n<outputs[i].count;++n)fprintf(stderr," %lld",(long long)outputs[i].values[n]);
        fprintf(stderr," expectedstatus%u count%u; requestedCall %llu->%llu work %llu->%llu oracle=%s\n",expected_status,count,(unsigned long long)before.requested_call_bytes,(unsigned long long)after.requested_call_bytes,(unsigned long long)before.work,(unsigned long long)after.work,oracle?"PASS":"FAIL");
        if(mode==PENDING_RETRY) {
            XrXirCallStatus restarted=xr_xir_instance_start(instances[i],1,NULL,0);
            if(restarted==XR_XIR_CALL_READY) {
                result=xr_xir_instance_poll_bounded(instances[i],UINT64_MAX);
                XrXirDomainBudgetStats retry=xr_xir_domain_budget_stats(instances[i]->domain);
                oracle=oracle && result.epoch==2 && result.outcome.status==XR_XIR_CALL_RETURNED && outputs[i].count==4 && outputs[i].values[2]==7 && outputs[i].values[3]==6 && retry.requested_call_bytes>after.requested_call_bytes && retry.work>after.work;
                XrXirValue owned={0};CHECK(xr_xir_instance_take_result(instances[i],&owned)==XR_XIR_CALL_RETURNED && owned.type==XR_XIR_I64 && owned.payload==7);xr_xir_value_drop(&owned);
            } else oracle=false;
            fprintf(stderr,"pending retry mode%u instance%u start%u oracle=%s\n",mode,i,restarted,oracle?"PASS":"FAIL");
        }
        if(mode==PENDING_CLEANUP_FAIL) {
            CHECK(xr_xir_instance_start(instances[i],1,NULL,0)==XR_XIR_CALL_BAD_STATE);
            CHECK(observed_call && xr_xir_call_cleanup_incomplete(observed_call));
        }
        if(mode==PENDING_WRITE_STREAM) {
            oracle=result.epoch==1 && result.outcome.status==XR_XIR_CALL_RETURNED && outputs[i].writes==1 && outputs[i].false_results==1 && outputs[i].count==1 && outputs[i].values[0]==6;
            XrXirValue owned={0};CHECK(xr_xir_instance_take_result(instances[i],&owned)==XR_XIR_CALL_RETURNED && owned.type==XR_XIR_I64 && owned.payload==7);xr_xir_value_drop(&owned);
            fprintf(stderr,"WRITE_STREAM actual ERROR false continued cleanup6 writes%u false%u oracle=%s\n",outputs[i].writes,outputs[i].false_results,oracle?"PASS":"FAIL");
        }
        if(oracle && mode!=PENDING_CLEANUP_FAIL)oracle=observed_call && !xr_xir_call_cleanup_incomplete(observed_call);
        valid=valid && oracle;
    }
    XrXirCallStatus expected_free=mode==PENDING_AWAIT?XR_XIR_CALL_OUTPUT_ERROR:XR_XIR_CALL_READY;
    CHECK(xr_xir_instance_free(instances[order])==expected_free);
    CHECK(xr_xir_instance_free(instances[1-order])==expected_free);
    CHECK(!runtime_live && !runtime_bytes);
    XrCompileResourceStats stats=library_compile_stats(&owner.context);CHECK(stats.live_bytes==owner.baseline.live_bytes);
    library_compile_owner_drop(&owner);CHECK(!source_program_compile_live && !source_program_compile_bytes);
    fprintf(stderr,"pending mode%u order%u runtime/compiler physical0 allocated%llu peak%llu work%llu\n",mode,order,(unsigned long long)stats.allocated_bytes,(unsigned long long)stats.peak_bytes,(unsigned long long)stats.work);
    return valid;
}
int main(void) {
    bool valid=true;
    for(uint32_t mode=0;mode<9;++mode)for(unsigned order=0;order<2;++order) {
        bool actual=pending_case(mode,order);valid=valid && actual;
    }
    library_compile_observer_free();CHECK(valid);return 0;
}
