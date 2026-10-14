/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_task_array_error.c - Owned Sendable array errors across task completion
 */
#define main unit_reference_main
#include "test_xir_task_unit.c"
#undef main
#include "task_array_error_generated.h"
static const char *array_error_names[]={"array","nested","strings"};
static XrXirProgram *array_error_program(unsigned mode,bool native,uint32_t *entry) {
    if(!native) {UnitOracle oracle={array_error_names[mode],"handle",true,false,true,0};return unit_program(&oracle,entry);}
    XrXirProgram *program=NULL;
    const XrXirCompileContext *context=effects_source_owner(UINT64_C(67108864),UINT64_C(128000000));
    CHECK(xr_xir_compile_program_seal(context,array_error_specs[mode],&program)==XR_XIR_OK);
    *entry=array_error_entries[mode];return program;
}
static void array_error_payload(const XrXirValue *task,const XrXirCallResult *outcome,unsigned mode) {
    CHECK(outcome->status==XR_XIR_CALL_THROWN&&!outcome->wake&&xr_xir_panic_empty(&outcome->panic));
    XrXirValue concrete=outcome->value;if(concrete.type==XR_XIR_ERROR)CHECK(xr_xir_error_borrow(&concrete,&concrete));
    XrXirEnumBorrow error={0};CHECK(xr_xir_enum_borrow(&concrete,&error)==XR_XIR_VALUE_OK);
    CHECK(error.name.length==7&&!memcmp(error.name.bytes,"Problem",7)&&error.member.length==6&&
        !memcmp(error.member.bytes,"Failed",6)&&error.field_count==1);
    const XirTask *storage=(const XirTask *)(uintptr_t)task->payload;
    XrXirValueAdmission admission={storage->object.arena,storage->object.domain,NULL,NULL,1000000,16777216};
    int64_t length=0;CHECK(xr_xir_array_len(&error.fields[0],&admission,&length)==XR_XIR_VALUE_OK&&length==2);
    for(unsigned i=0;i<2;++i) {
        XrXirValue value={0};XrXirFaultDetail fault={0};
        CHECK(xr_xir_array_get(&error.fields[0],i,&admission,&value,&fault)==XR_XIR_VALUE_OK);
        if(mode==2) {const char *bytes=NULL;size_t size=0;const char *want=i?"array":"owned";
            CHECK(xr_xir_string_view(&value,&bytes,&size)&&size==5&&!memcmp(bytes,want,5));}
        else if(mode==1) {
            CHECK(xr_xir_array_len(&value,&admission,&length)==XR_XIR_VALUE_OK&&length==2);
            for(unsigned j=0;j<2;++j) {XrXirValue leaf={0};
                CHECK(xr_xir_array_get(&value,j,&admission,&leaf,&fault)==XR_XIR_VALUE_OK);
                CHECK(leaf.type==XR_XIR_I64&&leaf.payload==(int64_t)(10*(2*i+j+1)));xr_xir_value_drop(&leaf);}
        } else CHECK(value.type==XR_XIR_I64&&value.payload==(int64_t)(10*(i+1)));
        xr_xir_value_drop(&value);
    }
}
static bool array_error_execute(XrXirProgram *program,uint32_t entry,unsigned mode,bool drop_program) {
    XrXirInstance *instances[2]={0};XrXirValue tasks[2]={{0}};bool completed=true;
    for(unsigned i=0;i<2;++i) {
        XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        XrXirCallStatus status=xr_xir_instance_new(program,&config,&instances[i]);
        if(status!=XR_XIR_CALL_READY){CHECK(status==XR_XIR_CALL_OOM||status==XR_XIR_CALL_LIMIT);completed=false;continue;}
    }
    if(instances[0]&&instances[1])CHECK(instances[0]->domain!=instances[1]->domain);
    /* Instances alone retain executable authority before any Task is spawned. */
    if(drop_program)xr_xir_compile_program_drop(program);
    for(unsigned i=0;i<2;++i)if(instances[i]) {
        XrXirCallStatus status=xr_xir_instance_start(instances[i],entry,NULL,0);
        if(status!=XR_XIR_CALL_READY){CHECK(status==XR_XIR_CALL_OOM||status==XR_XIR_CALL_LIMIT);completed=false;continue;}
        XrXirInstanceResult result={0};unsigned polls=0;
        do {result=xr_xir_instance_poll_bounded(instances[i],1);CHECK(++polls<4096);}while(result.outcome.status==XR_XIR_CALL_READY);
        if(result.outcome.status==XR_XIR_CALL_RETURNED) {
            CHECK(xr_xir_instance_take_result(instances[i],&tasks[i])==XR_XIR_CALL_RETURNED);
        } else {CHECK(result.outcome.status==XR_XIR_CALL_OOM||result.outcome.status==XR_XIR_CALL_LIMIT);completed=false;}
    }
    if(tasks[0].payload&&tasks[1].payload)CHECK(tasks[0].payload!=tasks[1].payload);
    for(unsigned i=0;i<2;++i)if(instances[i]) {
        XrXirCallStatus status=xr_xir_instance_free(instances[i]);
        CHECK(status==XR_XIR_CALL_READY||status==XR_XIR_CALL_OOM||status==XR_XIR_CALL_LIMIT);
    }
    for(unsigned i=0;i<2;++i)if(tasks[i].payload) {
        const XirTask *storage=(const XirTask *)(uintptr_t)tasks[i].payload;
        CHECK(storage->state==XIR_TASK_TERMINAL&&!storage->call&&!storage->executor);
        XrXirCallResult first={0},second={0};
        CHECK(xr_xir_task_copy_outcome(&tasks[i],&first)==XR_XIR_CALL_THROWN);
        CHECK(xr_xir_task_copy_outcome(&tasks[i],&second)==XR_XIR_CALL_THROWN);
        array_error_payload(&tasks[i],&first,mode);xr_xir_call_result_drop(&first);
        array_error_payload(&tasks[i],&second,mode);xr_xir_call_result_drop(&second);xr_xir_value_drop(&tasks[i]);
    }
    CHECK(!runtime_live&&!runtime_bytes);return completed;
}
static void array_error_type_controls(void) {
    XrXirTypeNode nodes[5]={{.kind=XR_XIR_TYPE_ARRAY,.element=257},
        {.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_CELL,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_CALLABLE}, {.kind=XR_XIR_TYPE_TASK,.element=XR_XIR_I64}};
    XrXirTypes types={nodes,5,NULL,NULL};
    for(unsigned minus=0;minus<2;++minus) {
        XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(1048576,&domain)==XR_XIR_VALUE_OK);
        XrXirDomainBudgetControls controls={1048576,1048576,2-minus,1048576,1048576};
        CHECK(xr_xir_domain_budget_bind(domain,&controls)==XR_XIR_VALUE_OK);
        XrXirTaskExecutor executor={0};executor.config.domain=domain;
        CHECK(task_core_error_field_admit(&executor,&types,256)==(minus?XR_XIR_CALL_LIMIT:XR_XIR_CALL_READY));
        XrXirDomainBudgetStats spent=xr_xir_domain_budget_stats(domain);
        CHECK(spent.work==2-minus&&spent.work_limit==2-minus);
        CHECK(task_core_error_field_admit(&executor,&types,256)==XR_XIR_CALL_LIMIT);
        CHECK(xr_xir_domain_budget_stats(domain).work==spent.work);
        xr_xir_domain_drop(domain);
    }
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(1048576,&domain)==XR_XIR_VALUE_OK);
    XrXirDomainBudgetControls controls={1048576,1048576,32,1048576,1048576};
    CHECK(xr_xir_domain_budget_bind(domain,&controls)==XR_XIR_VALUE_OK);
    XrXirTaskExecutor executor={0};executor.config.domain=domain;
    const XrXirType rejected[]={XR_XIR_ERROR,XR_XIR_PANIC_INFO,258,259,999};
    for(unsigned i=0;i<sizeof(rejected)/sizeof(rejected[0]);++i)
        CHECK(task_core_error_field_admit(&executor,&types,rejected[i])==XR_XIR_CALL_BAD_STATE);
    nodes[0].element=258;CHECK(task_core_error_field_admit(&executor,&types,256)==XR_XIR_CALL_BAD_STATE);
    nodes[0].element=260;CHECK(task_core_error_field_admit(&executor,&types,256)==XR_XIR_CALL_UNSUPPORTED);
    nodes[0].parameter_span=1;CHECK(task_core_error_field_admit(&executor,&types,256)==XR_XIR_CALL_BAD_STATE);
    nodes[0].parameter_span=0;nodes[0].element=256;
    uint64_t before=xr_xir_domain_budget_stats(domain).work;
    CHECK(task_core_error_field_admit(&executor,&types,256)==XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_domain_budget_stats(domain).work==before+types.count);
    xr_xir_domain_drop(domain);CHECK(!runtime_live&&!runtime_bytes);
}
static uint64_t array_error_work_operation(XrXirProgram *program,uint32_t entry,uint64_t limit,
    XrXirCallStatus expected,bool boundary) {
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.work_limit=limit;XrXirInstance *instance=NULL;
    CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
    XrXirInstanceResult result={0};unsigned polls=0;
    do {result=xr_xir_instance_poll_bounded(instance,1);CHECK(++polls<4096);}while(result.outcome.status==XR_XIR_CALL_READY);
    CHECK(result.outcome.status==XR_XIR_CALL_RETURNED);XrXirValue handle={0};
    CHECK(xr_xir_instance_take_result(instance,&handle)==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    const XirTask *task=(const XirTask *)(uintptr_t)handle.payload;
    CHECK(task->state==XIR_TASK_TERMINAL&&!task->executor&&!task->call&&task->outcome.status==XR_XIR_CALL_THROWN);
    XrXirCallResult outcome={0};CHECK(xr_xir_task_copy_outcome(&handle,&outcome)==expected);
    if(expected==XR_XIR_CALL_LIMIT)CHECK(xr_xir_call_result_empty(&outcome));
    else CHECK(outcome.status==XR_XIR_CALL_THROWN&&xr_xir_call_result_valid(&outcome));
    XrXirDomainBudgetStats spent=xr_xir_domain_budget_stats(task->object.domain);
    CHECK(spent.bound&&spent.work<=limit&&spent.work_limit==limit);
    xr_xir_call_result_drop(&outcome);
    if(boundary) {
        CHECK(spent.work==limit);
        CHECK(xr_xir_task_copy_outcome(&handle,&outcome)==XR_XIR_CALL_LIMIT&&xr_xir_call_result_empty(&outcome));
        XrXirDomainBudgetStats retry=xr_xir_domain_budget_stats(task->object.domain);
        CHECK(retry.work==spent.work&&retry.requested_bytes==spent.requested_bytes&&
            retry.requested_call_bytes==spent.requested_call_bytes&&task->outcome.status==XR_XIR_CALL_THROWN);
    }
    xr_xir_value_drop(&handle);CHECK(!runtime_live&&!runtime_bytes);return spent.work;
}
static void array_error_work_axes(XrXirProgram *program,uint32_t entry) {
    uint64_t work=array_error_work_operation(program,entry,128000000,XR_XIR_CALL_THROWN,false);CHECK(work>1);
    for(unsigned minus=0;minus<2;++minus)
        CHECK(array_error_work_operation(program,entry,work-minus,minus?XR_XIR_CALL_LIMIT:XR_XIR_CALL_THROWN,true)==work-minus);
    printf("TASK_ARRAY_ERROR_WORK exact=%llu minus1 sameDomain stickyTHROWN exhaustedRetry physical=0/0\n",(unsigned long long)work);
}
int main(int argc,char **argv) {
    CHECK(argc==4);unsigned mode=0;while(mode<3&&strcmp(argv[1],array_error_names[mode]))++mode;CHECK(mode<3);
    bool native=!strcmp(argv[2],"native"),fi=!strcmp(argv[3],"fi");
    CHECK(native||!strcmp(argv[2],"vm"));CHECK(fi||!strcmp(argv[3],"normal"));
    uint32_t entry=UINT32_MAX;XrXirProgram *program=array_error_program(mode,native,&entry);
    runtime_attempts=0;runtime_fail_at=SIZE_MAX;CHECK(array_error_execute(program,entry,mode,!fi));size_t sites=runtime_attempts;
    if(fi)for(size_t site=0;site<sites;++site) {
        runtime_attempts=0;runtime_fail_at=site;
        CHECK(!array_error_execute(program,entry,mode,false)&&runtime_attempts>site);
    }
    runtime_fail_at=SIZE_MAX;
    if(!fi)program=array_error_program(mode,native,&entry);
    array_error_work_axes(program,entry);xr_xir_compile_program_drop(program);effects_source_owners_free();
    CHECK(!effects_compile_live&&!effects_compile_bytes);array_error_type_controls();
    printf("TASK_ARRAY_ERROR %s %s %s sites=%zu twoInstances stickyCopies=2 escaped physical=0/0\n",argv[1],argv[2],argv[3],sites);return 0;
}
