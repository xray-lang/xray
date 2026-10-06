/* Real Source tasks preserve their own terminal fault when copying succeeds. */
#include "xir/xxir_source.h"
#include "xir/xxir_task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_effect_execution_owner.h"
static unsigned oc_phase, oc_instance, oc_hits, oc_denials, late_stage;
static size_t cross_last_ordinal,cross_last_bytes;
static unsigned cross_expected_instance=UINT32_MAX;
static bool oc_cancel_accepted;
static void oracle_allocation_attempt(unsigned kind,size_t bytes,size_t ordinal) {
    cross_last_ordinal=ordinal;cross_last_bytes=bytes;
    printf("CONTROL_ALLOC {\"kind\":%u,\"bytes\":%zu,\"ordinal\":%zu,\"phase\":%u,\"instance\":%u,\"after_cancel\":%u}\n",
        kind,bytes,ordinal,oc_phase,oc_instance,oc_cancel_accepted);
    if(oc_cancel_accepted&&oc_phase==3)printf("LATE_STAGE_ALLOC {\"ordinal\":%zu,\"stage\":%u,\"instance\":%u}\n",ordinal,late_stage,oc_instance);
}
static void oracle_malloc_failure(void) {
    CHECK(oc_phase==3&&oc_cancel_accepted&&oc_instance==cross_expected_instance);
    ++oc_hits;
    printf("CROSS_HIT {\"instance\":%u,\"ordinal\":%zu,\"bytes\":%zu,\"phase\":%u,\"after_cancel\":1}\n",
        oc_instance,cross_last_ordinal,cross_last_bytes,oc_phase);
}
static void oracle_budget_rejected(void) { ++oc_denials; }
/* Only the canonical Instance body is renamed; VM calls the public delegate. */
#define xr_xir_task_go oc_canonical_go
#define xr_xir_string_new late_canonical_string_new
#define xr_xir_enum_new late_canonical_enum_new
#define xr_xir_value_admit late_canonical_value_admit
#include "vm19_fault/runtime_observer_projection.h"
#undef xr_xir_task_go
#undef xr_xir_string_new
#undef xr_xir_enum_new
#undef xr_xir_value_admit
/* Each observer delegates once without modifying input, ownership or status. */
XR_FUNC XrXirValueStatus xr_xir_string_new(XrXirDomain *domain,const char *bytes,size_t length,XrXirValue *output) {
    unsigned prior=late_stage;late_stage=1;
    XrXirValueStatus status=late_canonical_string_new(domain,bytes,length,output);late_stage=prior;
    return status;
}
XR_FUNC XrXirValueStatus xr_xir_enum_new(XrXirType type,uint32_t variant,const XrXirValue *fields,
    uint32_t count,XrXirValueAdmission *admission,XrXirValue *output) {
    unsigned prior=late_stage;late_stage=2;
    XrXirValueStatus status=late_canonical_enum_new(type,variant,fields,count,admission,output);late_stage=prior;
    return status;
}
XR_FUNC XrXirValueStatus xr_xir_value_admit(const XrXirValue *value,XrXirType type,XrXirValueAdmission *admission) {
    unsigned prior=late_stage;late_stage=3;
    XrXirValueStatus status=late_canonical_value_admit(value,type,admission);late_stage=prior;
    return status;
}

static XrXirValueStatus oc_result_copy(const XrXirCallResult *source,XrXirCallResult *output);
#define xr_xir_call_result_copy oc_result_copy
#define main oc_original_unit_main
#include "test_xir_task_unit.c"
#undef main
#undef xr_xir_call_result_copy
static XrXirValue *oc_early;
static unsigned oc_go_calls,oc_copy_calls;
static XrXirValueStatus oc_copy_status;
static XrXirCallStatus oc_cached_status;
static void oc_hex(const void *data,size_t size) {
    const unsigned char *p=data;
    for(size_t i=0;i<size;++i)printf("%02x",(unsigned)p[i]);
}
XR_FUNC XrXirCallStatus xr_xir_task_go(XrXirCallView *view,XrXirType type,uint32_t entry,
    const XrXirValue *arguments,uint32_t count,XrXirValue *output) {
    ++oc_go_calls;
    XrXirCallStatus status=oc_canonical_go(view,type,entry,arguments,count,output);
    if(status==XR_XIR_CALL_READY) {
        CHECK(oc_early&&xr_xir_value_valid(output));
        const XrXirTypeNode *node=xr_xir_type_node(xr_xir_compile_type_arena_types(xr_xir_value_arena(output)),type);
        CHECK(node&&node->kind==XR_XIR_TYPE_TASK&&node->element==XR_XIR_UNIT);
        XrXirValue original=*output;
        XrXirDomain *domain=((XirTask *)(uintptr_t)output->payload)->object.domain;
        XrXirDomainBudgetStats before=xr_xir_domain_budget_stats(domain);
        CHECK(xr_xir_value_copy(output,oc_early)==XR_XIR_VALUE_OK);
        CHECK(!memcmp(&original,output,sizeof(original))&&oc_early->payload==output->payload);
        XrXirDomainBudgetStats after=xr_xir_domain_budget_stats(domain);
        CHECK(after.work>before.work&&after.requested_limit==before.requested_limit&&after.work_limit==before.work_limit);
        printf("CONTROL_GO {\"instance\":%u,\"calls\":%u,\"status\":%u,\"work\":[%llu,%llu],\"alias\":1}\n",
            oc_instance,oc_go_calls,(unsigned)status,(unsigned long long)before.work,(unsigned long long)after.work);
    }
    return status;
}
static XrXirValueStatus oc_result_copy(const XrXirCallResult *source,XrXirCallResult *output) {
    ++oc_copy_calls;oc_cached_status=source->status;
    oc_copy_status=xr_xir_call_result_copy(source,output);
    return oc_copy_status;
}
typedef struct OutcomeProvider {unsigned mode,calls;} OutcomeProvider;
static XrXirOutputStatus oc_output(void *context,const XrXirOutputGroup *group) {
    OutcomeProvider *p=context;
    CHECK(group&&group->stream==XR_XIR_STDOUT&&group->count==1&&group->line);
    CHECK(group->values[0].type==XR_XIR_I64&&group->values[0].payload==41);
    CHECK(!p->calls++);
    XrXirOutputStatus status=p->mode==1?XR_XIR_OUTPUT_OOM:p->mode==2?XR_XIR_OUTPUT_LIMIT:XR_XIR_OUTPUT_OK;
    printf("CONTROL_OUTPUT {\"instance\":%u,\"status\":%u,\"value\":41}\n",oc_instance,(unsigned)status);
    return status;
}
static void oc_copies(const XrXirValue *task,XrXirCallStatus expected,bool error) {
    const XirTask *storage=(const XirTask *)(uintptr_t)task->payload;
    unsigned char cached[sizeof(storage->outcome)];memcpy(cached,&storage->outcome,sizeof(cached));
    XrXirValue alias={0};CHECK(xr_xir_value_copy(task,&alias)==XR_XIR_VALUE_OK&&alias.payload==task->payload);
    XrXirCallResult occupied={.status=XR_XIR_CALL_RETURNED,.value={XR_XIR_I64,0,99}};
    unsigned char bytes[sizeof(occupied)];memcpy(bytes,&occupied,sizeof(bytes));
    CHECK(xr_xir_task_copy_outcome(task,&occupied)==XR_XIR_CALL_BAD_ARGUMENT&&!memcmp(bytes,&occupied,sizeof(bytes)));
    xr_xir_call_result_drop(&occupied);
    for(unsigned i=0;i<2;++i) {
        XrXirCallResult out={0};memcpy(bytes,&out,sizeof(bytes));oc_copy_calls=0;
        XrXirDomainBudgetStats before=xr_xir_domain_budget_stats(storage->object.domain);
        XrXirCallStatus status=xr_xir_task_copy_outcome(i?&alias:task,&out);
        XrXirDomainBudgetStats after=xr_xir_domain_budget_stats(storage->object.domain);
        CHECK(!memcmp(cached,&storage->outcome,sizeof(cached)));
        CHECK(after.requested_bytes>=before.requested_bytes&&after.requested_call_bytes>=before.requested_call_bytes&&after.work>before.work);
        CHECK(after.requested_limit==before.requested_limit&&after.requested_call_limit==before.requested_call_limit&&after.work_limit==before.work_limit);
        CHECK(oc_copy_calls==1&&oc_copy_status==XR_XIR_VALUE_OK&&oc_cached_status==expected);
        CHECK(status==expected&&out.status==expected&&xr_xir_call_result_valid(&out));
        if(expected==XR_XIR_CALL_CANCELLED||expected==XR_XIR_CALL_OOM||expected==XR_XIR_CALL_LIMIT) {
            CHECK(!out.wake&&xr_xir_panic_empty(&out.panic));unit_empty(&out.value);
        } else unit_sticky_value(&out,error);
        printf("CONTROL_COPY {\"instance\":%u,\"status\":%u,\"cached\":%u,\"value_status\":%u,\"delegations\":%u,\"size\":%zu,\"before\":\"",
            oc_instance,(unsigned)status,(unsigned)oc_cached_status,(unsigned)oc_copy_status,oc_copy_calls,sizeof(out));
        oc_hex(bytes,sizeof(bytes));printf("\",\"after\":\"");oc_hex(&out,sizeof(out));puts("\"}");
        xr_xir_call_result_drop(&out);
    }
    xr_xir_value_drop(&alias);
}

static void census_snapshot(XrXirInstance *instance,const XrXirValue *task,unsigned poll) {
    const XirTask *storage=task->payload?(const XirTask *)(uintptr_t)task->payload:NULL;
    printf("CENSUS_POLL {\"instance\":%u,\"poll\":%u,\"root_state\":%u,\"instance_state\":%u,\"go\":%u,\"task_state\":%u,\"cached\":%u,\"next_malloc\":%zu}\n",
        oc_instance,poll,(unsigned)xr_xir_call_state(instance->call),(unsigned)instance->state,oc_go_calls,
        storage?(unsigned)storage->state:0,storage?(unsigned)storage->outcome.status:0,runtime_attempts);
}
int main(int argc,char **argv) {
    CHECK(setvbuf(stdout,NULL,_IONBF,0)==0);
    unsigned prefix=argc>1?(unsigned)strtoul(argv[1],NULL,10):UINT32_MAX;
    size_t fail_at=argc>2?(size_t)_strtoui64(argv[2],NULL,10):SIZE_MAX;
    cross_expected_instance=argc>3?(unsigned)strtoul(argv[3],NULL,10):UINT32_MAX;
    CHECK(argc==1||argc==2||argc==4);
    if(fail_at!=SIZE_MAX)CHECK(prefix>=5&&prefix<=11&&cross_expected_instance<2);
    _Static_assert(XR_XIR_CHECKED_CONTRACT==68, "The cross observer binds Checked contract 68");
    uint32_t entry=UINT32_MAX;oc_phase=0;
    XrXirProgram *program=unit_program(&unit_oracles[7],&entry);
    XrXirInstance *instances[2]={0};XrXirDomain *leases[2]={0};XrXirValue early[2]={{0}};
    OutcomeProvider providers[2]={{0,0},{0,0}};
    runtime_attempts=0;runtime_fail_at=fail_at;oc_hits=oc_denials=0;oc_phase=1;
    for(unsigned i=0;i<2;++i) {
        oc_instance=i;XrXirInstanceConfig config;
        CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,oc_output,&providers[i]};
        CHECK(xr_xir_instance_new(program,&config,&instances[i])==XR_XIR_CALL_READY);
        leases[i]=instances[i]->domain;CHECK(xr_xir_domain_retain(leases[i]));
    }
    xr_xir_compile_program_drop(program);
    for(unsigned i=0;i<2;++i) {
        oc_instance=i;oc_cancel_accepted=false;oc_early=&early[i];oc_go_calls=0;oc_phase=2;
        CHECK(xr_xir_instance_start(instances[i],entry,NULL,0)==XR_XIR_CALL_READY);
        uint64_t epoch=instances[i]->epoch;XrXirInstanceResult result={0};unsigned polls=0;
        XrXirCallStatus cancel=XR_XIR_CALL_READY;size_t cancel_at=SIZE_MAX;oc_phase=3;
        census_snapshot(instances[i],&early[i],0);
        for(;;) {
            if(polls==prefix) {
                cancel=xr_xir_instance_cancel_current(instances[i]);oc_cancel_accepted=cancel==XR_XIR_CALL_CANCEL_REQUESTED;
                cancel_at=runtime_attempts;
                printf("CENSUS_CANCEL {\"instance\":%u,\"prefix\":%u,\"status\":%u,\"next_malloc\":%zu}\n",i,prefix,(unsigned)cancel,cancel_at);
            }
            result=xr_xir_instance_poll_bounded(instances[i],1);CHECK(++polls<4096);
            census_snapshot(instances[i],&early[i],polls);
            if(result.outcome.status!=XR_XIR_CALL_READY)break;
        }
        if(polls==prefix) {
            cancel=xr_xir_instance_cancel_current(instances[i]);
            CHECK(cancel==XR_XIR_CALL_BAD_STATE);
            printf("CENSUS_TERMINAL_CANCEL {\"instance\":%u,\"prefix\":%u,\"status\":%u}\n",i,prefix,(unsigned)cancel);
        }
        XrXirCallStatus first=instances[i]->executor->first_failure;
        XrXirCallStatus expected_first=i==cross_expected_instance?XR_XIR_CALL_OOM:XR_XIR_CALL_READY;
        CHECK(instances[i]->epoch==epoch&&first==expected_first);
        CHECK(!oc_denials&&!providers[i].calls);
        CHECK(oc_hits==(cross_expected_instance<=i?1u:0u));
        if(i==cross_expected_instance)CHECK(result.outcome.status==XR_XIR_CALL_OOM);
        else if(oc_cancel_accepted)CHECK(result.outcome.status==XR_XIR_CALL_CANCELLED);
        else CHECK(result.outcome.status==XR_XIR_CALL_RETURNED);
        XrXirValue root={0};XrXirCallStatus taken=xr_xir_instance_take_result(instances[i],&root);
        if(oc_cancel_accepted){CHECK(taken!=XR_XIR_CALL_RETURNED);unit_empty(&root);}
        else{CHECK(taken==XR_XIR_CALL_RETURNED&&root.payload==early[i].payload);xr_xir_value_drop(&root);}
        size_t end=runtime_attempts;oc_phase=4;
        XrXirCallStatus released=xr_xir_instance_free(instances[i]);CHECK(released==expected_first);instances[i]=NULL;
        XrXirCallStatus cached=i==cross_expected_instance?XR_XIR_CALL_OOM:XR_XIR_CALL_THROWN;
        if(early[i].payload){
            oc_phase=5;oc_copies(&early[i],cached,cached==XR_XIR_CALL_THROWN);
            const XirTask *task=(const XirTask *)(uintptr_t)early[i].payload;
            CHECK(task->state==XIR_TASK_TERMINAL&&!task->call&&!task->executor&&task->outcome.status==cached);
        }
        else CHECK(i!=cross_expected_instance);
        XrXirDomainBudgetStats fees=xr_xir_domain_budget_stats(leases[i]);
        CHECK(fees.bound&&fees.requested_limit==UINT64_C(67108864)&&fees.requested_call_limit==UINT64_C(67108864)&&fees.work_limit==UINT64_C(128000000));
        printf("CROSS_RESULT {\"instance\":%u,\"prefix\":%u,\"root\":%u,\"task\":%u,\"first\":%u,\"free\":%u,\"epoch\":%llu,\"limits\":[%llu,%llu,%llu]}\n",
            i,prefix,(unsigned)result.outcome.status,early[i].payload?(unsigned)cached:0,(unsigned)first,(unsigned)released,(unsigned long long)epoch,
            (unsigned long long)fees.requested_limit,(unsigned long long)fees.requested_call_limit,(unsigned long long)fees.work_limit);
        printf("CENSUS_RESULT {\"instance\":%u,\"prefix\":%u,\"cancel\":%u,\"polls\":%u,\"root\":%u,\"go\":%u,\"late_malloc\":%zu,\"fees\":[%llu,%llu,%llu]}\n",
            i,prefix,(unsigned)cancel,polls,(unsigned)result.outcome.status,oc_go_calls,
            oc_cancel_accepted?end-cancel_at:0,(unsigned long long)fees.requested_bytes,(unsigned long long)fees.requested_call_bytes,(unsigned long long)fees.work);
        oc_phase=6;xr_xir_value_drop(&early[i]);oc_early=NULL;
        CHECK(xr_xir_domain_stats(leases[i]).live_bytes==sizeof(*leases[i]));xr_xir_domain_drop(leases[i]);leases[i]=NULL;
    }
    CHECK(oc_hits==(fail_at==SIZE_MAX?0u:1u));
    CHECK(!runtime_live&&!runtime_bytes);effects_source_owners_free();CHECK(!effects_compile_live&&!effects_compile_bytes);
    printf("CENSUS_FINAL {\"physical\":[0,0],\"malloc_sites\":%zu,\"hits\":%u,\"semantic\":%u}\n",runtime_attempts,oc_hits,XR_XIR_CHECKED_CONTRACT);
    return 0;
}
