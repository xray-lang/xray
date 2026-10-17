#include "xir/xxir_program_packet_internal.h"
/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_channel_task_wait_cases.h - Native driver scheduling and exact owned release
 */
#ifndef XIR_CHANNEL_TASK_WAIT_CASES_H
#define XIR_CHANNEL_TASK_WAIT_CASES_H
#include "xir_native_metadata_fixture.h"
typedef struct ChannelTaskFixture {
    ResultOwner owner;
    ChannelCallWitness witness;
    XrXirValue channel;
    XrXirProgram *program;
    XrXirTypeArena *tasks;
    XrXirCallBudget budget;
    XrXirTaskExecutor *executor;
} ChannelTaskFixture;
static XrXirAction channel_task_init(XrXirCallView *view) {
    (void)view;return channel_call_action(XR_XIR_ACTION_RETURN,(XrXirValue){0});
}
static XrXirAction channel_task_main(XrXirCallView *view) {
    (void)view;return channel_call_action(XR_XIR_ACTION_RETURN,result_integer(0));
}
static XrXirAction channel_task_sender(XrXirCallView *view) {
    CHECK(view->argument_count==1);XrXirValue number=result_integer(42);XrXirChannelState state=XR_XIR_CHANNEL_RETRY;
    CHECK(xr_xir_channel_send_owned(&view->arguments[0],&number,xr_xir_call_admission(view),&state)==XR_XIR_VALUE_OK &&
        state==XR_XIR_CHANNEL_SENT && result_empty(&number));
    return channel_call_action(XR_XIR_ACTION_RETURN,result_integer(29));
}
static void channel_task_code_drop(void *owner) { ++((ChannelCallWitness *)owner)->code_releases; }
static void channel_task_new(ChannelTaskFixture *f) {
    result_owner_ready(&f->owner);
    CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_I64,1,&f->owner.admission,&f->channel)==XR_XIR_VALUE_OK);
    f->witness=(ChannelCallWitness){.channel=&f->channel,.sequence=channel_call_sequence(&f->owner,&f->channel)};
    const XrXirType parameter=(XrXirType)RESULT_CHANNEL_I64;
    XrXirCallEntry entries[]={
        {XR_XIR_CALL_ABI_VERSION,NULL,0,XR_XIR_UNIT,0,channel_task_init,NULL,NULL,0,0},
        {XR_XIR_CALL_ABI_VERSION,&parameter,1,XR_XIR_I64,sizeof(uint32_t),channel_call_entry,channel_call_release,&f->witness,0,0},
        {XR_XIR_CALL_ABI_VERSION,&parameter,1,XR_XIR_I64,0,channel_task_sender,NULL,NULL,0,0},
        {XR_XIR_CALL_ABI_VERSION,NULL,0,XR_XIR_I64,0,channel_task_main,NULL,NULL,0,0}};
    XrXirFunctionIdentity functions[4]={0};functions[1].exported=functions[2].exported=1;
    const XrXirSourceModule module={"channel-wait",12,NULL,0,0};
    XrXirDeclarations declarations={&module,1,functions,NULL,0,NULL,0,0,3,NULL};
    const XrXirTypeNode nodes[]={ {.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_CHANNEL,.element=XR_XIR_I64} };
    const XrXirTypes program_types={nodes,2,NULL,NULL};
    CHECK(xr_xir_compile_declarations_verify(&f->owner.context,&declarations,&program_types,4,XR_XIR_PROGRAM)==XR_XIR_OK);
    XrXirProgramSpec spec={XR_XIR_PROGRAM_ABI_VERSION,{XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},
        entries,4,&declarations,{&f->witness,channel_task_code_drop},
        &program_types,{0}};
    /* Trusted native entries use a real checked and lowered metadata proof.
     * This gate exercises the driver; it does not assert Source body effects. */
    XrXirArtifact *proof=NULL;XrXirStatus proof_status=native_metadata_fixture(&f->owner.context,&spec,&proof);
    if (proof_status!=XR_XIR_OK) fprintf(stderr,"Channel Task metadata status=%u\n",proof_status);
    CHECK(proof_status==XR_XIR_OK);
    spec.packet=xir_program_packet_borrow(proof);
    CHECK(xr_xir_compile_program_seal(&f->owner.context,&spec,&f->program)==XR_XIR_OK);
    xr_xir_compile_artifact_free(proof);
    XrXirTypeNode task={.kind=XR_XIR_TYPE_TASK,.element=XR_XIR_I64};XrXirTypes types={&task,1,NULL,NULL};
    CHECK(xr_xir_compile_type_arena_new(&f->owner.context,&types,&f->tasks)==XR_XIR_VALUE_OK);
    f->budget=(XrXirCallBudget){.byte_limit=4194304,.requested_limit=8388608,.resume_limit=1000,.work_domain=f->owner.domain};
    XrXirTaskExecutorConfig config={.program=f->program,.domain=f->owner.domain,.task_arena=f->tasks,
        .call_limit=4194304,.poll_limit=1000,.requested_value_limit=8388608,.requested_call_limit=8388608,
        .work_limit=128000000,.depth_limit=64,.budget=&f->budget,.admission=f->owner.admission};
    CHECK(xr_xir_task_executor_new(&config,&f->executor)==XR_XIR_CALL_READY);
}
static void channel_task_drop(ChannelTaskFixture *f) {
    CHECK(xr_xir_task_executor_free(f->executor,NULL)==XR_XIR_CALL_READY && !f->budget.live_bytes &&
        f->budget.allocations==f->budget.frees);
    xr_xir_compile_program_drop(f->program);CHECK(f->witness.code_releases==1);
    xr_xir_compile_type_arena_drop(f->tasks);xr_xir_value_drop(&f->channel);result_owner_free(&f->owner);
}
static XrXirValue channel_task_spawn(ChannelTaskFixture *f,uint32_t entry) {
    XrXirCallRequest request={entry,&f->channel,1,NULL,NULL};XrXirValue task={0};
    CHECK(xr_xir_task_executor_spawn(f->executor,(XrXirType)256,&request,&task)==XR_XIR_CALL_READY);return task;
}
static void channel_task_scheduled(void) {
    ChannelTaskFixture f={0};channel_task_new(&f);XrXirValue receiver=channel_task_spawn(&f,1);
    CHECK(xr_xir_task_executor_poll(f.executor,1)==XR_XIR_CALL_SUSPENDED && f.witness.callbacks==1);
    for (unsigned i=0;i<16;++i) CHECK(xr_xir_task_executor_poll(f.executor,1)==XR_XIR_CALL_SUSPENDED && f.witness.callbacks==1);
    XrXirValue sender=channel_task_spawn(&f,2);XrXirCallStatus state=XR_XIR_CALL_READY;
    for (unsigned i=0;i<32 && state==XR_XIR_CALL_READY;++i) state=xr_xir_task_executor_poll(f.executor,4);
    CHECK(state==XR_XIR_CALL_RETURNED && f.witness.callbacks==2 && f.witness.releases==1);
    XrXirCallResult outcome={0};CHECK(xr_xir_task_copy_outcome(&receiver,&outcome)==XR_XIR_CALL_RETURNED && outcome.value.payload==17);
    xr_xir_call_result_drop(&outcome);CHECK(xr_xir_task_copy_outcome(&sender,&outcome)==XR_XIR_CALL_RETURNED && outcome.value.payload==29);
    xr_xir_call_result_drop(&outcome);xr_xir_value_drop(&receiver);xr_xir_value_drop(&sender);channel_task_drop(&f);
}
static void channel_task_root(bool cancel) {
    ChannelTaskFixture f={0};channel_task_new(&f);XrXirCallAccounting accounting={0};XrXirCallConfig config={0};
    CHECK(xr_xir_call_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.entries=f.program->entries;config.entry_count=f.program->entry_count;config.admission=f.owner.admission;
    config.accounting=&accounting;config.byte_limit=4194304;config.poll_limit=1000;config.depth_limit=64;
    XrXirCallRequest request={1,&f.channel,1,NULL,NULL};XrXirCall *root=NULL;
    CHECK(xr_xir_call_new_budgeted(&config,&request,&f.budget,&root)==XR_XIR_CALL_READY);
    CHECK(xr_xir_task_executor_attach_root(f.executor,root,7)==XR_XIR_CALL_READY);
    XrXirCallResult pending=xr_xir_task_executor_poll_root(f.executor,1);CHECK(pending.status==XR_XIR_CALL_SUSPENDED && pending.wake);
    XrXirWaitRequest wait={0};CHECK(xr_xir_task_executor_root_wait(f.executor,7,pending.wake,&wait)==XR_XIR_CALL_READY &&
        wait.kind==XR_XIR_WAIT_CHANNEL && xr_xir_task_executor_resume_root(f.executor,7,pending.wake)==XR_XIR_CALL_BAD_STATE);
    if (cancel) CHECK(xr_xir_task_executor_cancel_root(f.executor)==XR_XIR_CALL_CANCEL_REQUESTED);
    else { XrXirValue number=result_integer(51);result_send(&f.owner,&f.channel,&number); }
    XrXirCallResult result={.status=XR_XIR_CALL_READY};
    for (unsigned i=0;i<32 && result.status==XR_XIR_CALL_READY;++i) result=xr_xir_task_executor_poll_root(f.executor,1);
    CHECK(result.status==(cancel ? XR_XIR_CALL_CANCELLED : XR_XIR_CALL_RETURNED));
    CHECK(f.witness.releases==1 && f.witness.callbacks==(cancel ? 1u : 2u));
    CHECK(xr_xir_task_executor_root_wait(f.executor,7,pending.wake,&wait)==XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_task_executor_free(f.executor,NULL)==XR_XIR_CALL_READY);f.executor=NULL;
    CHECK(xr_xir_call_free(root)==XR_XIR_CALL_READY && !accounting.live_bytes);channel_task_drop(&f);
}
static void channel_task_stop(void) {
    ChannelTaskFixture f={0};channel_task_new(&f);XrXirValue receiver=channel_task_spawn(&f,1);
    CHECK(xr_xir_task_executor_poll(f.executor,1)==XR_XIR_CALL_SUSPENDED);
    XirChannel *storage=(XirChannel *)object_pointer(&f.channel);atomic_store_explicit(&storage->locked,true,memory_order_relaxed);
    CHECK(xr_xir_task_executor_stop(f.executor)==XR_XIR_CALL_CANCEL_REQUESTED);
    XrXirCallStatus state=XR_XIR_CALL_READY;
    for (unsigned i=0;i<16 && state==XR_XIR_CALL_READY;++i) state=xr_xir_task_executor_poll(f.executor,1);
    CHECK(state==XR_XIR_CALL_RETURNED && f.witness.releases==1);
    XrXirCallResult outcome={0};CHECK(xr_xir_task_copy_outcome(&receiver,&outcome)==XR_XIR_CALL_CANCELLED);
    xr_xir_call_result_drop(&outcome);xr_xir_value_drop(&receiver);
    atomic_store_explicit(&storage->locked,false,memory_order_release);XrXirChannelState closed=XR_XIR_CHANNEL_RETRY;
    CHECK(xr_xir_channel_close(&f.channel,&f.owner.admission,&closed)==XR_XIR_VALUE_OK && !storage->watch_count);
    channel_task_drop(&f);
}
static void channel_task_wait_cases(void) {
    channel_task_scheduled();channel_task_root(false);channel_task_root(true);channel_task_stop();
}
#endif // XIR_CHANNEL_TASK_WAIT_CASES_H
