/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_domain_storage_prefix_cases.h - Reject an old owner's eight readable bytes
 */
#ifndef XIR_DOMAIN_STORAGE_PREFIX_CASES_H
#define XIR_DOMAIN_STORAGE_PREFIX_CASES_H
static void prefix_domain_reject_owner(XrXirDomain *domain, XrXirTypeArena *arena,
    const XrXirValue *text) {
    XrXirDomainStats empty_values = {0}, values = xr_xir_domain_stats(domain);
    XrXirDomainBudgetStats empty_budget = {0}, budget = xr_xir_domain_budget_stats(domain);
    CHECK(!memcmp(&values,&empty_values,sizeof(values)) && !memcmp(&budget,&empty_budget,sizeof(budget)));
    CHECK(!xr_xir_domain_retain(domain) && !xr_xir_domain_work(domain,0) && !xr_xir_domain_work(domain,1));
    XrXirDomainBudgetControls controls = {65536,65536,65536,65536,65536};
    CHECK(xr_xir_domain_budget_bind(domain,&controls) == XR_XIR_VALUE_BAD_ARGUMENT);
    XrXirValueStatus status = XR_XIR_VALUE_OK;
    CHECK(!xr_xir_domain_allocate(domain,1,&status) && status == XR_XIR_VALUE_BAD_ARGUMENT);
    status = XR_XIR_VALUE_OK;
    CHECK(!xr_xir_domain_metadata_allocate(domain,1,&status) && status == XR_XIR_VALUE_BAD_ARGUMENT);
    status = XR_XIR_VALUE_OK;
    CHECK(!xr_xir_domain_call_allocate(domain,1,&status) && status == XR_XIR_VALUE_BAD_ARGUMENT);
    XrXirClassObservation *observation = NULL;
    CHECK(xr_xir_domain_class_observation_open(domain,1,&observation) == XR_XIR_VALUE_BAD_ARGUMENT && !observation);
    PrefixLease lease = {true,0};
    const XrXirFunctionBinding binding = {&lease,prefix_binding_release,3,NULL,0};
    XrXirValueAdmission admission = {arena,domain,prefix_binding_admit,&lease,65536,65536};
    XrXirValue output = {0}, scalar = {XR_XIR_I64,0,42}, fields[] = {scalar,*text};
    CHECK(xr_xir_string_new(domain,"a",1,&output) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_cell_new(domain,arena,(XrXirType)PREFIX_CELL_T,&scalar,&admission,&output) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_function_new(domain,arena,(XrXirType)PREFIX_FN_T,&binding,&admission,&output) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_class_new((XrXirType)PREFIX_CLASS_T,fields,2,&admission,&output) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_value_admit(&scalar,XR_XIR_I64,&admission) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(!output.type && !output.reserved && !output.payload && !lease.releases);
    CHECK(admission.work == 65536 && admission.scratch_bytes == 65536);
}
static void prefix_domain_reject_value(XrXirDomain *old_domain, XrXirValueAdmission *admission,
    uint32_t kind, XrXirType type, void *unreadable) {
    enum { PREFIX_OBJECT_BYTES = offsetof(XirObject,kind)+sizeof(uint32_t) };
    _Alignas(XirObject) unsigned char storage[PREFIX_OBJECT_BYTES]; memset(storage,0,sizeof(storage));
    _Atomic(uint32_t) *references = (_Atomic(uint32_t) *)storage; atomic_init(references,1);
    XrXirTypeArena *arena = unreadable;
    memcpy(storage+offsetof(XirObject,domain),&old_domain,sizeof(old_domain));
    memcpy(storage+offsetof(XirObject,arena),&arena,sizeof(arena));
    memcpy(storage+offsetof(XirObject,type),&type,sizeof(type));
    memcpy(storage+offsetof(XirObject,kind),&kind,sizeof(kind));
    XrXirValue value = {(uint32_t)type,0,0}; void *pointer = storage;
    memcpy(&value.payload,&pointer,sizeof(pointer)); prefix_unreadable = unreadable;
    unsigned char before[PREFIX_OBJECT_BYTES]; memcpy(before,storage,sizeof(storage));
    prefix_reject_apis(&value,admission);
    CHECK(atomic_load(references) == 1 && !memcmp(before,storage,sizeof(storage)));
    prefix_unreadable = NULL;
}
static void prefix_domain_literal_case(uint32_t references, bool locked, unsigned char padding,
    unsigned char generation,XrXirTypeArena *arena, XrXirValueAdmission *admission, const XrXirValue *text) {
    SYSTEM_INFO info; GetSystemInfo(&info); size_t page = info.dwPageSize;
    CHECK(page >= 8 && page <= SIZE_MAX/2 && sizeof(atomic_bool) == 1 && sizeof(_Atomic(uint32_t)) == 4);
    unsigned char *memory = VirtualAlloc(NULL,page*2,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE); CHECK(memory);
    DWORD previous = 0; CHECK(VirtualProtect(memory+page,page,PAGE_NOACCESS,&previous));
    unsigned char *prefix = memory+page-8; memset(prefix,padding,8);
    if (padding == 2) { prefix[5]=0; prefix[6]=0; prefix[7]=1; }
    atomic_init((_Atomic(uint32_t) *)prefix,references);
    atomic_init((atomic_bool *)(prefix+4),locked);
    CHECK(prefix[4] == (locked ? 1u : 0u));
    if (generation) {prefix[4]=generation;prefix[5]=0;prefix[6]=0;prefix[7]=1;}
    unsigned char before[8]; memcpy(before,prefix,8);
    size_t attempts = prefix_runtime_allocations, live = cell_owner_live, bytes = cell_owner_bytes;
    size_t compiler = value_compile_calls; XrXirDomain *domain = (XrXirDomain *)prefix;
    prefix_domain_reject_owner(domain,arena,text);
    prefix_unreadable = memory+page;
    prefix_domain_reject_admission(domain,(XrXirTypeArena *)prefix_unreadable,text);
    prefix_unreadable = NULL;
    prefix_domain_reject_execution(domain);
    prefix_domain_reject_value(domain,admission,XIR_OBJECT_CLASS,(XrXirType)PREFIX_CLASS_T,memory+page);
    prefix_domain_reject_value(domain,admission,XIR_OBJECT_CELL,(XrXirType)PREFIX_CELL_T,memory+page);
    prefix_domain_reject_value(domain,admission,XIR_OBJECT_FUNCTION,(XrXirType)PREFIX_FN_T,memory+page);
    CHECK(prefix_runtime_allocations == attempts && cell_owner_live == live && cell_owner_bytes == bytes &&
        value_compile_calls == compiler && !prefix_old_arena_access);
    CHECK(atomic_load((_Atomic(uint32_t) *)prefix) == references && !memcmp(prefix,before,8));
    CHECK(VirtualFree(memory,0,MEM_RELEASE));
}
static void prefix_domain_cases(XrXirTypeArena *arena, XrXirDomain *domain,
    XrXirValueAdmission *admission) {
    XrXirValue text = {0}; CHECK(xr_xir_string_new(domain,"seed",4,&text) == XR_XIR_VALUE_OK);
    const uint32_t references[] = {1,UINT32_MAX-1,UINT32_MAX};
    for (unsigned r=0;r<3;++r) for (unsigned locked=0;locked<2;++locked)
        for (unsigned padding=0;padding<2;++padding)
            prefix_domain_literal_case(references[r],locked != 0,padding ? 0xffu : 0u,0,arena,admission,&text);
    for (unsigned locked=0;locked<2;++locked)
        prefix_domain_literal_case(UINT32_C(0x4d445258),locked != 0,2,0,arena,admission,&text);
    for (unsigned char generation=24;generation<=26;++generation)
        prefix_domain_literal_case(UINT32_C(0x4d445258),false,0,generation,arena,admission,&text);
    xr_xir_value_drop(&text);
}
#endif // XIR_DOMAIN_STORAGE_PREFIX_CASES_H
