/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_class_access_resource_types.h - Independent Source access resource oracles
 */
#ifndef XIR_CLASS_ACCESS_RESOURCE_TYPES_H
#define XIR_CLASS_ACCESS_RESOURCE_TYPES_H
typedef struct AccessSourceCase {
    const char *name;
    int64_t result;
    bool rejected, alias;
} AccessSourceCase;
static const AccessSourceCase access_source_cases[] = {
    {"class_access_worker_return_formal.xr",41,false,false},
    {"class_access_worker_array_cow.xr",4240,false,false},
    {"class_access_worker_carrier_unknown.xr",0,true,false},
    {"class_access_same_owner.xr",0,false,true},
    {"class_access_worker_module_alias.xr",0,true,false}};
enum { ACCESS_CREATE, ACCESS_START, ACCESS_POLL, ACCESS_TRANSFER, ACCESS_CONTROL,
    ACCESS_CLOSE, ACCESS_DROP, ACCESS_RETRY, ACCESS_STICKY };
enum { ACCESS_RUN, ACCESS_CANCEL, ACCESS_STOP, ACCESS_CLOSE_ACTIVE, ACCESS_MODES };
typedef struct AccessRuntimeCost {
    size_t sites, hits, prepare_begin, prepare_end;
    XrXirCallStatus failure;
    XrXirDomainBudgetStats budgets[2], prepare_before, prepare_failed, prepare_after;
    XrXirDomainStats values[2];
    bool complete, recovered, controlled;
    uint32_t results[2], getters[2], loans, rebinds[2];
} AccessRuntimeCost;
typedef struct AccessRuntimeRun {
    const AccessSourceCase *fixture;
    const CellSourceEntries *entries;
    XrXirInstance *instances[2];
    XrXirDomain *domains[2];
    XrXirValue results[2][2], counters[2][2], projection;
    const void *projection_owner;
    XrXirCallStatus observed[2];
    size_t injected[2];
    unsigned mode;
    bool retry;
    bool failed[2];
    AccessRuntimeCost cost;
} AccessRuntimeRun;
typedef struct AccessCompileSite { size_t ordinal, bytes; } AccessCompileSite;
typedef struct AccessCompileCost {
    XrXirStatus status, first;
    XrCompileResourceStats initial, stats;
    size_t sites, hits;
    bool complete, same_owner, bootstrap, fits;
} AccessCompileCost;
static bool access_runtime_retry;
static XrCompileResourceLimits access_compile_limits(void) {
    return (XrCompileResourceLimits){UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(128000000)};
}
static bool access_resource_status(XrXirCallStatus status) {
    return status==XR_XIR_CALL_OOM || status==XR_XIR_CALL_LIMIT;
}
static bool access_value_empty(const XrXirValue *value) {
    return value && !value->type && !value->reserved && !value->payload;
}
static void access_number(const XrXirValue *value,int64_t expected) {
    CHECK(xr_xir_value_valid(value) && value->type==XR_XIR_I64 && !value->reserved && value->payload==expected);
}
static void access_physical_zero(void) {
    instance_compile_zero();
    if (runtime_live || runtime_bytes || runtime_owned) runtime_report_residuals(stderr);
    CHECK(!runtime_live && !runtime_bytes && !runtime_owned);
}
#endif // XIR_CLASS_ACCESS_RESOURCE_TYPES_H
