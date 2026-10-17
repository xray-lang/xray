#include "xir/xxir_program_packet_internal.h"
/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_channel_rendezvous_task_cases.h - Real executor ownership of native payload waits
 */
#ifndef XIR_CHANNEL_RENDEZVOUS_TASK_CASES_H
#define XIR_CHANNEL_RENDEZVOUS_TASK_CASES_H
typedef struct RendezvousTask {
    ResultOwner owner;
    RendezvousWitness sent,received;
    XrXirValue channel;
    XrXirProgram *program;
    XrXirTypeArena *tasks;
    XrXirCallBudget budget;
    XrXirTaskExecutor *executor;
    unsigned code_releases;
} RendezvousTask;
static void rendezvous_task_code_drop(void *context) { ++((RendezvousTask *)context)->code_releases; }
static void rendezvous_task_new_mode(RendezvousTask *f,bool receive_first) {
    result_owner_ready(&f->owner);
    CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_I64,0,&f->owner.admission,&f->channel)==XR_XIR_VALUE_OK);
    const XrXirType parameter=(XrXirType)RESULT_CHANNEL_I64;
    XrXirCallEntry entries[]={
        {XR_XIR_CALL_ABI_VERSION,NULL,0,XR_XIR_UNIT,0,channel_task_init,NULL,NULL,0,0},
        {XR_XIR_CALL_ABI_VERSION,&parameter,1,XR_XIR_I64,receive_first ? sizeof(RecvWaitFrame) : sizeof(RendezvousFrame),receive_first ? recv_wait_sender_entry : rendezvous_sender_entry,receive_first ? recv_wait_call_release : rendezvous_call_release,&f->sent,0,0},
        {XR_XIR_CALL_ABI_VERSION,&parameter,1,XR_XIR_I64,receive_first ? sizeof(RecvWaitFrame) : sizeof(RendezvousFrame),receive_first ? recv_wait_entry : rendezvous_receiver_entry,receive_first ? recv_wait_call_release : rendezvous_call_release,&f->received,0,0},
        {XR_XIR_CALL_ABI_VERSION,NULL,0,XR_XIR_I64,0,channel_task_main,NULL,NULL,0,0}};
    XrXirFunctionIdentity functions[4]={0};functions[1].exported=functions[2].exported=1;
    const XrXirSourceModule module={"rendezvous",10,NULL,0,0};
    XrXirDeclarations declarations={&module,1,functions,NULL,0,NULL,0,0,3,NULL};
    const XrXirTypeNode nodes[]={ {.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64}, {.kind=XR_XIR_TYPE_CHANNEL,.element=XR_XIR_I64} };
    const XrXirTypes types={nodes,2,NULL,NULL};
    XrXirProgramSpec spec={XR_XIR_PROGRAM_ABI_VERSION,{XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},entries,4,
        &declarations,{f,rendezvous_task_code_drop},&types,{0}};
    XrXirArtifact *proof=NULL;CHECK(native_metadata_fixture(&f->owner.context,&spec,&proof)==XR_XIR_OK);
    spec.packet=xir_program_packet_borrow(proof);
    CHECK(xr_xir_compile_program_seal(&f->owner.context,&spec,&f->program)==XR_XIR_OK);xr_xir_compile_artifact_free(proof);
    XrXirTypeNode task={.kind=XR_XIR_TYPE_TASK,.element=XR_XIR_I64};XrXirTypes task_types={&task,1,NULL,NULL};
    CHECK(xr_xir_compile_type_arena_new(&f->owner.context,&task_types,&f->tasks)==XR_XIR_VALUE_OK);
    f->budget=(XrXirCallBudget){.byte_limit=4194304,.requested_limit=8388608,.resume_limit=1000,.work_domain=f->owner.domain};
    XrXirTaskExecutorConfig config={.program=f->program,.domain=f->owner.domain,.task_arena=f->tasks,
        .call_limit=4194304,.poll_limit=1000,.requested_value_limit=8388608,.requested_call_limit=8388608,
        .work_limit=128000000,.depth_limit=64,.budget=&f->budget,.admission=f->owner.admission};
    CHECK(xr_xir_task_executor_new(&config,&f->executor)==XR_XIR_CALL_READY);
}
static void rendezvous_task_new(RendezvousTask *f) { rendezvous_task_new_mode(f,false); }
static XrXirValue rendezvous_task_spawn(RendezvousTask *f,unsigned entry) {
    XrXirCallRequest request={entry,&f->channel,1,NULL,NULL};XrXirValue task={0};
    CHECK(xr_xir_task_executor_spawn(f->executor,(XrXirType)256,&request,&task)==XR_XIR_CALL_READY);return task;
}
static void rendezvous_task_drop(RendezvousTask *f) {
    CHECK(xr_xir_task_executor_free(f->executor,NULL)==XR_XIR_CALL_READY && !f->budget.live_bytes && f->budget.allocations==f->budget.frees);
    xr_xir_compile_program_drop(f->program);CHECK(f->code_releases==1);xr_xir_compile_type_arena_drop(f->tasks);
    xr_xir_value_drop(&f->channel);result_owner_free(&f->owner);
}
static void rendezvous_task_pair(void) {
    RendezvousTask f={0};rendezvous_task_new(&f);XrXirValue sent=rendezvous_task_spawn(&f,1);
    CHECK(xr_xir_task_executor_poll(f.executor,1)==XR_XIR_CALL_SUSPENDED && f.sent.callbacks==1);
    for (unsigned i=0;i<16;++i) CHECK(xr_xir_task_executor_poll(f.executor,1)==XR_XIR_CALL_SUSPENDED && f.sent.callbacks==1);
    XrXirValue received=rendezvous_task_spawn(&f,2);XrXirCallStatus status=XR_XIR_CALL_READY;
    for (unsigned i=0;i<32 && status==XR_XIR_CALL_READY;++i) status=xr_xir_task_executor_poll(f.executor,4);
    CHECK(status==XR_XIR_CALL_RETURNED && f.sent.releases==1 && f.received.releases==1);
    XrXirCallResult result={0};CHECK(xr_xir_task_copy_outcome(&sent,&result)==XR_XIR_CALL_RETURNED && result.value.payload==XR_XIR_CHANNEL_SEND_WAIT_SENT);
    xr_xir_call_result_drop(&result);CHECK(xr_xir_task_copy_outcome(&received,&result)==XR_XIR_CALL_RETURNED && result.value.payload==541);
    xr_xir_call_result_drop(&result);xr_xir_value_drop(&sent);xr_xir_value_drop(&received);rendezvous_task_drop(&f);
}
static void rendezvous_task_stop(void) {
    RendezvousTask f={0};rendezvous_task_new(&f);XrXirValue sent=rendezvous_task_spawn(&f,1);
    CHECK(xr_xir_task_executor_poll(f.executor,1)==XR_XIR_CALL_SUSPENDED);
    XirChannel *storage=(XirChannel *)object_pointer(&f.channel);atomic_store_explicit(&storage->locked,true,memory_order_relaxed);
    CHECK(xr_xir_task_executor_stop(f.executor)==XR_XIR_CALL_CANCEL_REQUESTED);XrXirCallStatus status=XR_XIR_CALL_READY;
    for (unsigned i=0;i<16 && status==XR_XIR_CALL_READY;++i) status=xr_xir_task_executor_poll(f.executor,1);
    CHECK(status==XR_XIR_CALL_RETURNED && f.sent.releases==1 && f.sent.reason==XR_XIR_CALL_CANCELLED);
    XrXirCallResult result={0};CHECK(xr_xir_task_copy_outcome(&sent,&result)==XR_XIR_CALL_CANCELLED);
    xr_xir_call_result_drop(&result);xr_xir_value_drop(&sent);atomic_store_explicit(&storage->locked,false,memory_order_release);
    XrXirChannelState state=XR_XIR_CHANNEL_RETRY;CHECK(xr_xir_channel_close(&f.channel,&f.owner.admission,&state)==XR_XIR_VALUE_OK && !storage->send_count);
    rendezvous_task_drop(&f);
}
static void rendezvous_task_cases(void) {
    rendezvous_task_pair();rendezvous_task_stop();
    puts("Rendezvous actual native Task executor: owned sender wait, real receiver541, typed terminal ack, held-lock stop and one Program code lease release physical0");
}
#endif // XIR_CHANNEL_RENDEZVOUS_TASK_CASES_H
