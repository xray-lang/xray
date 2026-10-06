/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_vm19_fault_oracle.c - Finite runtime fault and source arbitration oracles
 */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include "xir/xxir_call.h"
enum OraclePhase { ORACLE_PREP,ORACLE_NEW,ORACLE_INPUT,ORACLE_START,ORACLE_POLL,ORACLE_RESTART,ORACLE_REPOLL,ORACLE_TAKE,ORACLE_FREE };
static unsigned oracle_phase,oracle_hit_phase,oracle_hits,oracle_reason,oracle_poll_step,oracle_hit_step;
static bool oracle_cancel_seen,oracle_cancel_accepted;
static unsigned oracle_cancel_status;
/* Test-host static observer storage, outside the finite product ledger. */
typedef struct OracleAllocationEvent {size_t ordinal,bytes;unsigned kind,phase,step;uint64_t epoch;} OracleAllocationEvent;
static OracleAllocationEvent oracle_allocations[512];
static unsigned oracle_allocation_count;static uint64_t oracle_epoch;
static void oracle_allocation_attempt(unsigned kind,size_t bytes,size_t ordinal) {
    if(oracle_phase<ORACLE_NEW)return;
    if(oracle_allocation_count==512)abort();
    oracle_allocations[oracle_allocation_count++]=(OracleAllocationEvent){ordinal,bytes,kind,oracle_phase,oracle_poll_step,oracle_epoch};
}
static unsigned oracle_budget_rejections,oracle_budget_first_phase;
static bool oracle_executing_poll;
static void oracle_budget_rejected(void) {
    if(oracle_phase>=ORACLE_NEW) {
        if(!oracle_budget_rejections)oracle_budget_first_phase=oracle_phase;
        ++oracle_budget_rejections;
        if(oracle_executing_poll && (oracle_phase==ORACLE_POLL || oracle_phase==ORACLE_REPOLL) &&
            (!oracle_reason || oracle_reason==XR_XIR_CALL_CANCELLED))oracle_reason=XR_XIR_CALL_LIMIT;
    }
}
static void oracle_malloc_failure(void) {
    ++oracle_hits;oracle_hit_phase=oracle_phase;oracle_hit_step=oracle_poll_step;
    if((oracle_phase==ORACLE_POLL || oracle_phase==ORACLE_REPOLL) && !oracle_reason)oracle_reason=6;
}
#include "vm19_fault/role_fixture_projection.h"

typedef struct OracleOutput {PendingOutput *pending;RoleOutput *role;uint32_t id;unsigned calls;} OracleOutput;
static XrXirOutputStatus oracle_output(void *context,const XrXirOutputGroup *group) {
    OracleOutput *out=context;++out->calls;
    XrXirOutputStatus status=out->id<9?pending_output(out->pending,group):role_output(out->role,group);
    /* A cancellation request is not an accepted host/resource failure. The
     * executing driver owns acceptance: an uncancelled worker or real cleanup
     * can accept a legal host fault while the root has only requested cancel.
     * Prior host faults and protocol observations remain sticky. */
    XrXirInstance *instance=out->pending->instance;
    XrXirCall *driver=instance && instance->executor && instance->executor->current ?
        instance->executor->current->call : instance ? instance->call : NULL;
    bool host_after_cancel=group->values[0].type!=XR_XIR_STRING &&
        oracle_reason==XR_XIR_CALL_CANCELLED && oracle_executing_poll &&
        (oracle_phase==ORACLE_POLL || oracle_phase==ORACLE_REPOLL) &&
        (status==XR_XIR_OUTPUT_ERROR || status==XR_XIR_OUTPUT_OOM || status==XR_XIR_OUTPUT_LIMIT) &&
        driver && driver->driving && (!driver->cancel_requested || cleanup_active(driver)) &&
        (driver->abort_reason==XR_XIR_CALL_READY || driver->abort_reason==XR_XIR_CALL_CANCELLED);
    if(group->values[0].type!=XR_XIR_STRING && status!=XR_XIR_OUTPUT_OK && (!oracle_reason || host_after_cancel)) {
        bool cancel_arbitrates=driver && driver->cancel_requested && !cleanup_active(driver) &&
            driver->abort_reason==XR_XIR_CALL_READY;
        oracle_reason=cancel_arbitrates?XR_XIR_CALL_CANCELLED:
            status==XR_XIR_OUTPUT_ERROR?XR_XIR_CALL_OUTPUT_ERROR:status==XR_XIR_OUTPUT_OOM?XR_XIR_CALL_OOM:
            status==XR_XIR_OUTPUT_LIMIT?XR_XIR_CALL_LIMIT:status==XR_XIR_OUTPUT_BAD_ABI?XR_XIR_CALL_BAD_ABI:XR_XIR_CALL_BAD_ARGUMENT;
    }
    if(host_after_cancel) {
        fprintf(stderr,"HOST_AFTER_CANCEL provider%u accepted%u phase%u step%u drivercancel%u cleanup%u prior%u epoch%llu ASSERT\n",
            (unsigned)status,oracle_reason,oracle_phase,oracle_poll_step,driver->cancel_requested,
            cleanup_active(driver),(unsigned)driver->abort_reason,(unsigned long long)oracle_epoch);
    }
    return status;
}
static void oracle_empty_result(XrXirInstance *instance,XrXirInstanceResult result) {
    if(result.outcome.status==XR_XIR_CALL_RETURNED)return;
    CHECK(!result.outcome.value.type && !result.outcome.value.reserved && !result.outcome.value.payload && xr_xir_panic_empty(&result.outcome.panic));
    XrXirValue empty={0},occupied={XR_XIR_I64,0,91};
    XrXirCallStatus a=xr_xir_instance_take_result(instance,&empty),b=xr_xir_instance_take_result(instance,&occupied);
    CHECK(!empty.type && !empty.reserved && !empty.payload && occupied.type==XR_XIR_I64 && !occupied.reserved && occupied.payload==91);
    CHECK(a==XR_XIR_CALL_BAD_STATE || a==result.outcome.status || a==XR_XIR_CALL_LIMIT);
    CHECK(b==XR_XIR_CALL_BAD_STATE || b==XR_XIR_CALL_BAD_ARGUMENT || b==result.outcome.status || b==XR_XIR_CALL_LIMIT);
}

static unsigned measured_steps;
static XrXirInstanceResult measured_poll(XrXirInstance *instance,uint32_t id,
    PendingOutput *pending,unsigned cancel_prefix) {
    XrXirInstanceResult result={0};bool cancelled=false;unsigned loops=0;
    for(;;) {
        if(!oracle_cancel_seen && cancel_prefix!=UINT32_MAX && measured_steps==cancel_prefix) {
            XrXirCallStatus status=xr_xir_instance_cancel_current(instance);
            CHECK(status==XR_XIR_CALL_CANCEL_REQUESTED || status==XR_XIR_CALL_READY || status==XR_XIR_CALL_BAD_STATE);
            if(status==XR_XIR_CALL_CANCEL_REQUESTED && !oracle_reason)oracle_reason=XR_XIR_CALL_CANCELLED;
            oracle_cancel_seen=true;oracle_cancel_status=(unsigned)status;oracle_cancel_accepted=status==XR_XIR_CALL_CANCEL_REQUESTED;
            cancelled=true;
        }
        oracle_poll_step=measured_steps;oracle_executing_poll=true;
        result=xr_xir_instance_poll_bounded(instance,1);oracle_executing_poll=false;++measured_steps;
        CHECK(++loops<4096);
        if(id==PENDING_CANCEL_AFTER && pending->count && !cancelled) {
            CHECK(xr_xir_instance_stop(instance)==XR_XIR_CALL_OUTPUT_ERROR);cancelled=true;
        }
        if(result.outcome.status==XR_XIR_CALL_SUSPENDED) {
            XrXirWaitRequest request={0};
            CHECK(xr_xir_instance_wait_request(instance,result.epoch,result.outcome.wake,&request)==XR_XIR_CALL_READY);
            CHECK(request.kind==XR_XIR_WAIT_TIMER_MS && request.after_ms==1);
            CHECK(xr_xir_instance_resume(instance,result.epoch,result.outcome.wake)==XR_XIR_CALL_READY);
            continue;
        }
        if(result.outcome.status!=XR_XIR_CALL_READY) {
            if(!oracle_cancel_seen && cancel_prefix!=UINT32_MAX && measured_steps==cancel_prefix) {
                XrXirDomainBudgetStats before=xr_xir_domain_budget_stats(instance->domain);
                XrXirCallStatus status=xr_xir_instance_cancel_current(instance);
                CHECK(status==XR_XIR_CALL_BAD_STATE || status==XR_XIR_CALL_READY);
                oracle_cancel_seen=true;oracle_cancel_status=(unsigned)status;
                XrXirInstanceResult cached=xr_xir_instance_poll_bounded(instance,1);
                CHECK(cached.epoch==result.epoch && cached.outcome.status==result.outcome.status);
                XrXirDomainBudgetStats after=xr_xir_domain_budget_stats(instance->domain);
                CHECK(before.requested_bytes==after.requested_bytes && before.requested_call_bytes==after.requested_call_bytes && before.work==after.work);
            }
            break;
        }
    }
    return result;
}
static void measure_operation(uint32_t id,size_t fail_at,unsigned axis,uint64_t cap,unsigned cancel_prefix) {
    CHECK(id<19 && !runtime_live && !runtime_bytes);
    oracle_phase=ORACLE_PREP;oracle_hits=0;oracle_hit_phase=0;oracle_hit_step=0;oracle_reason=0;
    oracle_cancel_seen=false;oracle_cancel_accepted=false;oracle_cancel_status=UINT32_MAX;
    oracle_budget_rejections=0;oracle_budget_first_phase=0;oracle_allocation_count=0;oracle_epoch=0;oracle_executing_poll=false;
    runtime_attempts=0;source_program_compile_attempts=0;
    LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
    uint32_t kind=id<9?ROLE_OUTPUT:id<14?ROLE_OUTPUT:id<17?ROLE_TASK:ROLE_WRITE;
    uint32_t mode=id<9?id:kind==ROLE_TASK?PENDING_AWAIT:kind==ROLE_WRITE?PENDING_WRITE_STREAM:PENDING_DIRECT;
    XrXirProgram *program=id==18?role_wait_program(&owner.context):cleanup_program(&owner.context,mode);
    XrCompileResourceStats prep=library_compile_stats(&owner.context);
    size_t prep_attempts=runtime_attempts,prep_live=runtime_live,prep_bytes=runtime_bytes;
    size_t prep_compiler_sites=source_program_compile_attempts;
    PendingOutput pending={.mode=mode};RoleOutput role={.initial=XR_XIR_OUTPUT_ERROR};
    static const XrXirOutputStatus statuses[]={XR_XIR_OUTPUT_ERROR,XR_XIR_OUTPUT_OOM,XR_XIR_OUTPUT_LIMIT,XR_XIR_OUTPUT_BAD_ABI,(XrXirOutputStatus)99};
    if(id>=9 && id<14)role.initial=statuses[id-9];
    if(id>=14 && id<17)role.initial=statuses[id-14];
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    OracleOutput callback={&pending,&role,id,0};
    config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,oracle_output,&callback};
    if(axis==0)config.requested_value_limit=cap;
    if(axis==1)config.requested_call_limit=cap;
    if(axis==2)config.work_limit=cap;
    runtime_attempts=0;runtime_fail_at=fail_at;observed_count=0;observed_call=NULL;measured_steps=0;
    XrXirInstance *instance=NULL;XrXirDomain *lease=NULL;XrXirValue argument={0};
    oracle_phase=ORACLE_NEW;
    XrXirCallStatus created=xr_xir_instance_new(program,&config,&instance),started=XR_XIR_CALL_BAD_STATE,restarted=XR_XIR_CALL_BAD_STATE;
    XrXirInstanceResult first={0},last={0};bool normal=fail_at==SIZE_MAX && axis==UINT32_MAX && cancel_prefix==UINT32_MAX;
    if(created==XR_XIR_CALL_READY) {
        lease=instance->domain;CHECK(xr_xir_domain_retain(lease));pending.instance=instance;
        oracle_phase=ORACLE_INPUT;XrXirValueStatus input=XR_XIR_VALUE_OK;
        if(mode==PENDING_WRITE_STREAM && id!=18) {
            input=xr_xir_string_new(lease,"a\0b",3,&argument);
            if(input==XR_XIR_VALUE_LIMIT) {
                CHECK(oracle_phase==ORACLE_INPUT && !oracle_executing_poll && !oracle_reason);
                fprintf(stderr,"INPUT_REJECT valueStatus%u phase%u pollActive%u epochMarker%u\n",
                    (unsigned)input,oracle_phase,oracle_executing_poll,oracle_reason);
                oracle_budget_rejected();
                CHECK(!oracle_reason);
            }
        }
        if(input==XR_XIR_VALUE_OK) {
            oracle_phase=ORACLE_START;uint64_t start_epoch=instance->epoch;XrXirCall *start_call=instance->call;
            started=xr_xir_instance_start(instance,1,argument.type?&argument:NULL,argument.type?1u:0u);
            if(started!=XR_XIR_CALL_READY)CHECK(instance->epoch==start_epoch && instance->call==start_call);
            xr_xir_value_drop(&argument);
            if(started==XR_XIR_CALL_READY) {
                oracle_epoch=instance->epoch;oracle_phase=ORACLE_POLL;first=measured_poll(instance,id,&pending,cancel_prefix);last=first;
                oracle_empty_result(instance,first);
                if(oracle_reason)CHECK(first.outcome.status==(XrXirCallStatus)oracle_reason);
                else if(first.outcome.status!=XR_XIR_CALL_RETURNED)CHECK(first.outcome.status==XR_XIR_CALL_LIMIT || first.outcome.status==XR_XIR_CALL_CANCELLED);
                bool retry=id==PENDING_RETRY || (id>=9 && id<12) || (id>=14 && id<17) || id==18;
                if(retry) {
                    oracle_phase=ORACLE_RESTART;uint64_t epoch=instance->epoch;XrXirCall *old=instance->call;
                    XrXirCallStatus next=xr_xir_instance_start(instance,1,NULL,0);restarted=next;
                    if(next==XR_XIR_CALL_READY) {oracle_reason=0;oracle_epoch=instance->epoch;oracle_phase=ORACLE_REPOLL;last=measured_poll(instance,id,&pending,cancel_prefix);oracle_empty_result(instance,last);if(oracle_reason)CHECK(last.outcome.status==(XrXirCallStatus)oracle_reason);else CHECK(last.outcome.status==XR_XIR_CALL_RETURNED || last.outcome.status==XR_XIR_CALL_LIMIT);}
                    else if(!normal) {CHECK(instance->epoch==epoch && instance->call==old);CHECK(next==XR_XIR_CALL_OOM || next==XR_XIR_CALL_LIMIT || next==XR_XIR_CALL_BAD_STATE || next==first.outcome.status);}
                    else if(normal)CHECK(false);
                }
                if(last.outcome.status==XR_XIR_CALL_RETURNED) {
                    XrXirValue value={0};XrXirCallStatus taken=xr_xir_instance_take_result(instance,&value);
                    CHECK(taken==XR_XIR_CALL_RETURNED && value.type==XR_XIR_I64 && value.payload==7);
                    xr_xir_value_drop(&value);
                }
            }
        } else {CHECK(!normal && !argument.type && !argument.reserved && !argument.payload);CHECK(input==XR_XIR_VALUE_OOM || input==XR_XIR_VALUE_LIMIT);}
    }
    else {CHECK(!instance);CHECK(created==XR_XIR_CALL_OOM || created==XR_XIR_CALL_LIMIT);}
    if(normal) {
        CHECK(created==XR_XIR_CALL_READY && started==XR_XIR_CALL_READY);
        XrXirCallStatus expected=id==PENDING_CANCEL_BEFORE?XR_XIR_CALL_CANCELLED:id==PENDING_WRITE_STREAM||id==17?XR_XIR_CALL_RETURNED:XR_XIR_CALL_OUTPUT_ERROR;
        if(id>=9 && id<14)expected=(XrXirCallStatus[]){XR_XIR_CALL_OUTPUT_ERROR,XR_XIR_CALL_OOM,XR_XIR_CALL_LIMIT,XR_XIR_CALL_BAD_ABI,XR_XIR_CALL_BAD_ARGUMENT}[id-9];
        if(id>=14 && id<17)expected=(XrXirCallStatus[]){XR_XIR_CALL_OUTPUT_ERROR,XR_XIR_CALL_OOM,XR_XIR_CALL_LIMIT}[id-14];
        CHECK(first.outcome.status==expected);
        CHECK(id!=PENDING_RETRY || last.outcome.status==XR_XIR_CALL_RETURNED);
    }
    XrXirDomainBudgetStats fees={0};XrXirDomainStats values={0};XrXirCallStatus freed=XR_XIR_CALL_READY;
    bool incomplete=instance && instance->call && xr_xir_call_cleanup_incomplete(instance->call);
    if(incomplete && observed_count)CHECK(observed_frontiers[observed_count-1]!=0);
    if(instance && started==XR_XIR_CALL_READY && !incomplete && observed_count)CHECK(observed_frontiers[observed_count-1]==0);
    oracle_phase=ORACLE_FREE;if(instance)freed=xr_xir_instance_free(instance);
    if(lease) {fees=xr_xir_domain_budget_stats(lease);values=xr_xir_domain_stats(lease);xr_xir_domain_drop(lease);}
    size_t sites=runtime_attempts;runtime_fail_at=SIZE_MAX;
    CHECK(runtime_live==prep_live && runtime_bytes==prep_bytes);
    xr_xir_compile_program_drop(program);
    CHECK(!runtime_live && !runtime_bytes);
    CHECK(library_compile_stats(&owner.context).live_bytes==owner.baseline.live_bytes);
    library_compile_owner_drop(&owner);CHECK(!source_program_compile_live && !source_program_compile_bytes);
    if(fail_at!=SIZE_MAX)CHECK(oracle_hits==1 && sites>fail_at);
    if(fail_at!=SIZE_MAX && oracle_hit_phase==ORACLE_NEW)CHECK(created==XR_XIR_CALL_OOM);
    if(fail_at!=SIZE_MAX && oracle_hit_phase==ORACLE_START)CHECK(started==XR_XIR_CALL_OOM);
    if(fail_at!=SIZE_MAX && oracle_hit_phase==ORACLE_RESTART)CHECK(restarted==XR_XIR_CALL_OOM);
    if(cancel_prefix!=UINT32_MAX)CHECK(oracle_cancel_seen);
    CHECK(oracle_allocation_count==sites);
    for(unsigned i=0;i<oracle_allocation_count;++i) {
        const OracleAllocationEvent *event=&oracle_allocations[i];
        fprintf(stderr,"ALLOC ordinal%llu bytes%llu kind%u phase%u step%u epoch%llu\n",
            (unsigned long long)event->ordinal,(unsigned long long)event->bytes,event->kind,event->phase,event->step,(unsigned long long)event->epoch);
    }
    fprintf(stderr,"ORACLE id%u hit%u phase%u step%u reason%u restart%u outputcalls%u ASSERT/physical0\n",id,oracle_hits,oracle_hit_phase,oracle_hit_step,oracle_reason,restarted,callback.calls);
    fprintf(stderr,"BUDGET rejected%u firstPhase%u ASSERT\n",oracle_budget_rejections,oracle_budget_first_phase);
    fprintf(stderr,"CONTROL cancelSeen%u cancelAccepted%u cancelStatus%u incomplete%u frontierCount%u ASSERT\n",oracle_cancel_seen,oracle_cancel_accepted,oracle_cancel_status,incomplete,observed_count);
    printf("{\"id\":%u,\"fail_at\":%llu,\"axis\":%u,\"cap\":%llu,\"cancel_prefix\":%u,\"runtime_malloc_sites\":%llu,\"poll_prefix_steps\":%u,\"created\":%u,\"started\":%u,\"first_status\":%u,\"final_status\":%u,\"free_status\":%u,\"requested_value\":%llu,\"requested_call\":%llu,\"work\":%llu,\"metadata_peak\":%llu,\"call_peak\":%llu,\"value_peak\":%llu,\"prep_compiler_allocated\":%llu,\"prep_compiler_peak\":%llu,\"prep_compiler_work\":%llu,\"prep_compiler_sites\":%llu,\"prep_runtime_attempts_cumulative\":%llu,\"prep_runtime_live\":%llu,\"prep_runtime_bytes\":%llu,\"physical_zero\":true}\n",
        id,(unsigned long long)fail_at,axis,(unsigned long long)cap,cancel_prefix,(unsigned long long)sites,measured_steps,created,started,first.outcome.status,last.outcome.status,freed,
        (unsigned long long)fees.requested_bytes,(unsigned long long)fees.requested_call_bytes,(unsigned long long)fees.work,(unsigned long long)fees.metadata_peak,(unsigned long long)fees.call_peak,(unsigned long long)values.peak_bytes,
        (unsigned long long)prep.allocated_bytes,(unsigned long long)prep.peak_bytes,(unsigned long long)prep.work,(unsigned long long)prep_compiler_sites,(unsigned long long)prep_attempts,(unsigned long long)prep_live,(unsigned long long)prep_bytes);
    fflush(stdout);
}
int main(int argc,char **argv) {
    if(argc==1)for(uint32_t id=0;id<19;++id)measure_operation(id,SIZE_MAX,UINT32_MAX,0,UINT32_MAX);
    else {
        CHECK(argc==6);uint32_t id=(uint32_t)strtoul(argv[1],NULL,10);
        size_t fail=(size_t)strtoull(argv[2],NULL,10);unsigned axis=(unsigned)strtoul(argv[3],NULL,10);
        uint64_t cap=strtoull(argv[4],NULL,10);unsigned cancel=(unsigned)strtoul(argv[5],NULL,10);
        measure_operation(id,fail,axis,cap,cancel);
    }
    library_compile_observer_free();return 0;
}
