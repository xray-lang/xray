/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_generator_cell_borrow_faults.h - Complete allocating failure suffixes and retries
 */
#ifndef XIR_GENERATOR_CELL_BORROW_FAULTS_H
#define XIR_GENERATOR_CELL_BORROW_FAULTS_H
typedef struct BorrowFaultCost { size_t sites;XrXirCallStatus status; } BorrowFaultCost;
static uint32_t borrow_history_nodes,borrow_history_suffixes;
static BorrowFaultCost borrow_fault_operation(XrXirProgram *program,bool negative,const size_t *history,uint32_t count) {
    size_t blocks=runtime_live,bytes=runtime_bytes;CHECK(count<=128 && !borrow_iterator.type && !borrow_cell.type);
    if(count)memcpy(borrow_failure_ordinals,history,count*sizeof(*history));
    borrow_failure_count=count;borrow_failure_next=0;runtime_attempts=source_native_event_count=0;
    runtime_fail_at=SIZE_MAX;source_native_recording=true;source_native_phase=0;source_native_poll=UINT32_MAX;
    borrow_negative=negative;borrow_demands=borrow_producers=borrow_loans=0;borrow_program_owner=program;
    XrXirInstanceConfig config;XrXirInstance *instance=NULL;
    CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirCallStatus status=xr_xir_instance_new(program,&config,&instance);
    if(status==XR_XIR_CALL_READY) {
        borrow_observed_instance=instance;source_native_phase=1;
        status=xr_xir_instance_start(instance,borrow_ids[negative?1:0],NULL,0);
        if(status==XR_XIR_CALL_READY) {
            source_native_phase=2;XrXirInstanceResult result=xr_xir_instance_poll_bounded(instance,1000000);
            status=result.outcome.status;CHECK(!result.outcome.wake);
            if(status==XR_XIR_CALL_RETURNED) {
                source_native_phase=3;XrXirValue value={0};CHECK(!negative);
                CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
                CHECK(value.type==XR_XIR_I64 && value.payload==17);xr_xir_value_drop(&value);
            } else CHECK(!result.outcome.value.type);
        }
    }
    if(instance) {
        source_native_phase=4;XrXirCallStatus freed=xr_xir_instance_free(instance);borrow_observed_instance=NULL;
        if(freed!=XR_XIR_CALL_READY && (status==XR_XIR_CALL_READY || status==XR_XIR_CALL_RETURNED))status=freed;
        else CHECK(freed==XR_XIR_CALL_READY || freed==status);
    }
    source_native_phase=5;borrow_program_owner=NULL;
    if(borrow_iterator.type){borrow_retired();xr_xir_value_drop(&borrow_iterator);}
    if(borrow_cell.type) {
        CHECK(xr_xir_cell_unborrowed(&borrow_cell));XrXirValue value={0};
        CHECK(xr_xir_cell_read(&borrow_cell,&value)==XR_XIR_VALUE_OK && value.type==XR_XIR_I64 && value.payload==17);
        xr_xir_value_drop(&value);xr_xir_value_drop(&borrow_cell);
    }
    source_native_recording=false;runtime_fail_at=SIZE_MAX;
    CHECK(runtime_live==blocks && runtime_bytes==bytes && runtime_attempts==source_native_event_count);
    CHECK(borrow_failure_next==count);borrow_failure_count=borrow_failure_next=0;
    XrXirCallStatus expected=negative?XR_XIR_CALL_BAD_ARGUMENT:XR_XIR_CALL_RETURNED;
    if(!count)CHECK(status==expected && borrow_demands==1 && (negative?!borrow_producers && borrow_loans==1:borrow_producers && !borrow_loans));
    else CHECK(status==XR_XIR_CALL_OOM);
    return (BorrowFaultCost){runtime_attempts,status};
}
static bool borrow_same_allocation(const SourceNativeAllocationEvent *a,const SourceNativeAllocationEvent *b) {
    return a->ordinal==b->ordinal && a->bytes==b->bytes && a->line==b->line && a->kind==b->kind &&
        a->phase==b->phase && a->instance==b->instance && a->poll==b->poll && a->role==b->role &&
        a->entry==b->entry && !strcmp(a->file,b->file);
}
static void borrow_fault_node(XrXirProgram *program,bool negative,size_t *history,uint32_t depth,
    const SourceNativeAllocationEvent *parent,uint32_t parent_id) {
    CHECK(depth<=128 && borrow_history_nodes<65000);uint32_t node=borrow_history_nodes++;
    BorrowFaultCost cost=borrow_fault_operation(program,negative,history,depth);
    SourceNativeAllocationEvent *trace=xr_malloc(cost.sites*sizeof(*trace));CHECK(trace);
    memcpy(trace,source_native_events,cost.sites*sizeof(*trace));
    if(depth)for(size_t i=0;i<=history[depth-1];++i)CHECK(borrow_same_allocation(&trace[i],&parent[i]));
    printf("BORROW_FAILURE_NODE node=%u parent=%u borrowed=%u depth=%u sites=%zu status=%u history=",node,parent_id,negative?1u:0u,depth,cost.sites,cost.status);
    for(uint32_t i=0;i<depth;++i)printf("%s%zu",i?",":"",history[i]);puts("");
    for(size_t i=0;i<cost.sites;++i) {
        const SourceNativeAllocationEvent *e=&trace[i];
        printf("BORROW_ALLOC node=%u ordinal=%zu bytes=%zu file=%s line=%u kind=%u phase=%u role=%u entry=%u hit=%u\n",
            node,e->ordinal,e->bytes,e->file,e->line,e->kind,e->phase,e->role,e->entry,e->injected?1u:0u);
    }
    if(depth) {
        BorrowFaultCost retry=borrow_fault_operation(program,negative,NULL,0);
        CHECK(retry.sites && retry.status==(negative?XR_XIR_CALL_BAD_ARGUMENT:XR_XIR_CALL_RETURNED));
        printf("BORROW_RETRY node=%u same_program=1 status=%u sites=%zu physical_baseline=1\n",node,retry.status,retry.sites);
    }
    size_t begin=depth?history[depth-1]+1:0;
    for(size_t ordinal=begin;ordinal<cost.sites;++ordinal) {
        CHECK(depth<128);history[depth]=ordinal;if(depth)++borrow_history_suffixes;
        borrow_fault_node(program,negative,history,depth+1,trace,node);
    }
    xr_free(trace);
}
static void borrow_faults(void) {
    for(uint32_t negative=0;negative<2;++negative) {
        BorrowCodeOwner owner={0};XrXirProgram *program=borrow_program_new(&owner);size_t history[128]={0};
        borrow_history_nodes=borrow_history_suffixes=0;
        borrow_fault_node(program,negative!=0,history,0,NULL,UINT32_MAX);
        xr_xir_compile_program_drop(program);CHECK(owner.releases==1);source_fixture_owner_free(&owner.compiler);
        CHECK(!runtime_live && !runtime_bytes);
        printf("BORROW_FAILURE_CENSUS borrowed=%u nodes=%u suffixes=%u same_program_retries=%u physical=0/0 PASS\n",
            negative,borrow_history_nodes,borrow_history_suffixes,borrow_history_nodes-1);
    }
}
#endif // XIR_GENERATOR_CELL_BORROW_FAULTS_H
