/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_domain_admission_generation_cases.h - Prefix rejection before budget and type authority
 */
#ifndef XIR_DOMAIN_ADMISSION_GENERATION_CASES_H
#define XIR_DOMAIN_ADMISSION_GENERATION_CASES_H
static void prefix_domain_reject_admission(XrXirDomain *domain, XrXirTypeArena *arena,
    const XrXirValue *text) {
    PrefixLease lease = {true,0};
    const XrXirFunctionBinding binding = {&lease,prefix_binding_release,3,NULL,0};
    XrXirValueAdmission admission = {arena,domain,prefix_binding_admit,&lease,65536,65536};
    XrXirValue output = {0}, scalar = {XR_XIR_I64,0,42}; bool equal = true;
    CHECK(xr_xir_value_equal(&scalar,&scalar,XR_XIR_I64,&admission,&equal) == XR_XIR_VALUE_BAD_ARGUMENT && equal);
    CHECK(xr_xir_array_new((XrXirType)PREFIX_CLASS_T,&scalar,1,&admission,&output) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_nullable_new((XrXirType)PREFIX_CLASS_T,&scalar,&admission,&output) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_tuple_new((XrXirType)PREFIX_CLASS_T,&scalar,1,&admission,&output) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_struct_new((XrXirType)PREFIX_CLASS_T,&scalar,1,&admission,&output) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_enum_new((XrXirType)PREFIX_CLASS_T,0,&scalar,1,&admission,&output) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_atomic_new((XrXirType)PREFIX_CLASS_T,&scalar,&admission,&output) == XR_XIR_VALUE_BAD_ARGUMENT);
    XrXirAtomicProgress progress = {0};
    XrXirAtomicOutcome outcome = xr_xir_atomic_resume(&progress,&admission,NULL,&output);
    CHECK(outcome.status == XR_XIR_RUN_BAD_ARGUMENT && outcome.permission == XR_XIR_CALL_READY && !outcome.continuing);
    XrXirFaultDetail fault = {430,0,3,4}, saved = fault;
    XrXirCellLoan rejected_loan = {0};
    CHECK(xr_xir_cell_loan_prepare_path(&scalar,NULL,&admission,&fault,&rejected_loan) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(cell_loan_empty(&rejected_loan));
    CHECK(!memcmp(&fault,&saved,sizeof(fault)));
    XrXirClassObservation *occupied = (XrXirClassObservation *)(uintptr_t)1;
    CHECK(xr_xir_domain_class_observation_open(domain,1,&occupied) == XR_XIR_VALUE_BAD_ARGUMENT &&
        occupied == (XrXirClassObservation *)(uintptr_t)1);
    XrXirValue fields[] = {scalar,*text};
    output = scalar;
    CHECK(xr_xir_class_new((XrXirType)PREFIX_CLASS_T,fields,2,&admission,&output) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_function_new(domain,arena,(XrXirType)PREFIX_FN_T,&binding,&admission,&output) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(output.type == scalar.type && output.reserved == scalar.reserved && output.payload == scalar.payload);
    CHECK(admission.work == 65536 && admission.scratch_bytes == 65536 && !lease.releases);
}
static void prefix_domain_reject_execution(XrXirDomain *domain) {
    XrXirCallConfig config; CHECK(xr_xir_call_config_init(&config,sizeof(config)) == XR_XIR_CALL_READY);
    XrXirCallAccounting accounting = {0}; config.accounting = &accounting;
    config.admission.domain = domain;
    config.admission.arena = (XrXirTypeArena *)(uintptr_t)1;
    config.entries = (const XrXirCallEntry *)(uintptr_t)1; config.entry_count = 1;
    XrXirCall *call = NULL;
    CHECK(xr_xir_call_new(&config,0,NULL,0,&call) == XR_XIR_CALL_BAD_ARGUMENT && !call);
    XrXirCallAccounting empty_accounting = {0}; CHECK(!memcmp(&accounting,&empty_accounting,sizeof(accounting)));
    XrXirCallBudget budget = {.work_domain=domain,.byte_limit=65536,.requested_limit=65536};
    XrXirCallBudget saved = budget; XrXirCallStatus status = XR_XIR_CALL_READY;
    CHECK(!xr_xir_call_budget_allocate(&budget,1,&status) && status == XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(!memcmp(&budget,&saved,sizeof(budget)) && !budget.exhausted);
    const XrXirCallRequest request = {0,NULL,0,NULL,NULL};
    CHECK(xr_xir_call_new_budgeted(&config,&request,&budget,&call) == XR_XIR_CALL_BAD_ARGUMENT && !call);
    XrXirTaskExecutorConfig tasks = {.program=(XrXirProgram *)(uintptr_t)1,.domain=domain,.budget=&budget};
    XrXirTaskExecutor *executor = NULL;
    CHECK(xr_xir_task_executor_new(&tasks,&executor) == XR_XIR_CALL_BAD_ARGUMENT && !executor);
    CHECK(!memcmp(&budget,&saved,sizeof(budget)));
    XrXirClassObservation observed = {.domain=domain};
    uint64_t cursor=0; XrXirClassEvent event={0}; uint32_t count=0; bool overflow=false;
    CHECK(xr_xir_class_observation_retain(&observed) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_class_observation_drain(&observed,&cursor,&event,1,&count,&overflow) == XR_XIR_VALUE_BAD_ARGUMENT &&
        !cursor && !count && !overflow && !event.serial && !event.kind && !observed.references);
    call = (XrXirCall *)(uintptr_t)1;
    CHECK(xr_xir_call_new(&config,0,NULL,0,&call) == XR_XIR_CALL_BAD_ARGUMENT &&
        call == (XrXirCall *)(uintptr_t)1);
    CHECK(xr_xir_call_new_budgeted(&config,&request,&budget,&call) == XR_XIR_CALL_BAD_ARGUMENT &&
        call == (XrXirCall *)(uintptr_t)1);
    executor = (XrXirTaskExecutor *)(uintptr_t)1;
    CHECK(xr_xir_task_executor_new(&tasks,&executor) == XR_XIR_CALL_BAD_ARGUMENT &&
        executor == (XrXirTaskExecutor *)(uintptr_t)1);
}
static void prefix_domain_current_identity(XrXirDomain *domain) {
    const unsigned char identity[8] = {0x58,0x52,0x44,0x4d,0x1b,0,0,1};
    _Static_assert(offsetof(XrXirDomain,identity) == 0 && offsetof(XrXirDomain,references) == 8 &&
        offsetof(XrXirDomain,locked) == 12, "independently selected Domain prefix offsets");
    CHECK(xr_xir_domain_current(domain) && !memcmp(domain,identity,sizeof(identity)));
    CHECK(!xr_xir_domain_current(NULL) && !xr_xir_domain_retain(NULL) && !xr_xir_domain_work(NULL,0));
    XrXirDomain *occupied = domain;
    CHECK(xr_xir_domain_new(65536,&occupied) == XR_XIR_VALUE_BAD_ARGUMENT && occupied == domain);
    XrXirDomainStats saved = xr_xir_domain_stats(domain);
    CHECK(xr_xir_domain_retain(domain)); xr_xir_domain_drop(domain);
    XrXirDomainStats after = xr_xir_domain_stats(domain); CHECK(!memcmp(&saved,&after,sizeof(saved)));
}
static XrXirAction prefix_domain_scalar_entry(XrXirCallView *view) {
    CHECK(view && view->phase == XR_XIR_CALL_NORMAL);
    CHECK(xr_xir_call_admission(view) && !xr_xir_call_admission(view)->domain);
    return (XrXirAction){.kind=XR_XIR_ACTION_RETURN};
}
static void prefix_domain_current_scalar_call(void) {
    const XrXirCallEntry entry = {.abi_version=XR_XIR_CALL_ABI_VERSION,.result=XR_XIR_UNIT,
        .resume=prefix_domain_scalar_entry};
    XrXirCallConfig config; CHECK(xr_xir_call_config_init(&config,sizeof(config)) == XR_XIR_CALL_READY);
    XrXirCallAccounting accounting = {0}; config.accounting = &accounting;
    config.entries = &entry; config.entry_count = 1; XrXirCall *call = NULL;
    CHECK(!config.admission.domain && xr_xir_call_new(&config,0,NULL,0,&call) == XR_XIR_CALL_READY && call);
    XrXirCallResult result = xr_xir_call_poll_bounded(call,16);
    CHECK(result.status == XR_XIR_CALL_RETURNED && !result.value.type && !result.value.reserved && !result.value.payload);
    XrXirValue output = {0}; CHECK(xr_xir_call_take_result(call,&output) == XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_call_free(call) == XR_XIR_CALL_READY && !accounting.live_bytes && !accounting.depth &&
        accounting.allocations == accounting.frees);
}
#endif // XIR_DOMAIN_ADMISSION_GENERATION_CASES_H
