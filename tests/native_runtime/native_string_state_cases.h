/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * native_string_state_cases.h - Actual String slots preserve flags and failed mutations
 */
typedef struct StringStateProbe {
    XrtModuleState *state;
    uint32_t mode;
} StringStateProbe;
static XrXirValueStatus string_state_new(StringStateProbe *probe) {
    XrtModuleTypeSpec type={XRT_MODULE_STRING,0};
    XrtModuleSpec module={0};
    XrtModuleSlotSpec slot={.flags=probe->mode == 3 ? 0 : XRT_MODULE_SLOT_MUTABLE,
        .size=sizeof(void *),.alignment=_Alignof(void *)};
    XrtModuleLayoutSpec spec={.types=&type,.type_count=1,.modules=&module,.module_count=1,
        .slots=&slot,.slot_count=1,.payload_size=sizeof(void *),.payload_alignment=_Alignof(void *),.byte_limit=UINT64_MAX};
    XrtModuleLayout *layout=NULL;
    XrXirValueStatus status=xrt_module_layout_new(&spec,&layout);
    if (!status) status=xrt_module_state_new(layout,UINT64_MAX,&probe->state);
    xrt_module_layout_drop(&layout);
    return status;
}
static void string_state_setup(void *opaque) {
    StringStateProbe *probe=opaque;
    xrt_module_require(xrt_module_begin(probe->state,0),1);
    XrtModuleAccess published={0};
    xrt_module_require(xrt_module_cache(probe->state,0,&published),1);
    if (probe->mode == 3) { (void)xrt_string_ref_state(published,1); return; }
    const char seed_bytes[]="original String large enough for a new suffix";
    XrtString *seed=xrt_string_new(seed_bytes,sizeof(seed_bytes)-1,1);
    XrtModuleRef *initial=NULL;
    xrt_module_require(xrt_module_construct(probe->state,0,&seed,&initial),1);
    XrtModuleAccess constructed={0};
    xrt_module_require(xrt_module_reference_cache(initial,&constructed),1);
    if (probe->mode == 1) { (void)xrt_string_ref_read(xrt_string_ref_state(published,1),1); return; }
    if (probe->mode == 2) { constructed.kind=XRT_MODULE_I64; (void)xrt_string_ref_state(constructed,1); return; }
    xrt_string_ref_append(xrt_string_ref_state(constructed,1),xrt_string_literal("!",1,1),1);
    xrt_string_drop(&seed);
    xrt_module_require(xrt_module_publish(initial),1);
    xrt_module_require(xrt_module_ready(probe->state,0),1);
}
static void string_state_append(void *opaque) {
    StringStateProbe *probe=opaque;
    XrtModuleAccess access={0};
    xrt_module_require(xrt_module_cache(probe->state,0,&access),1);
    xrt_string_ref_append(xrt_string_ref_state(access,1),xrt_string_literal("\0Ω",3,1),1);
    if (probe->mode == 5) xrt_null_unwrap(1);
}
static void string_state_check(StringStateProbe *probe, bool appended) {
    XrtModuleAccess access={0};
    OK(xrt_module_cache(probe->state,0,&access));
    const void *payload=NULL;
    OK(xrt_module_borrow(access.reference,&payload));
    XrtString *actual=NULL;
    memcpy(&actual,payload,sizeof(actual));
    const char *expected=appended ? "original String large enough for a new suffix!\0Ω" :
        "original String large enough for a new suffix!";
    string_equals(actual,expected,appended ? sizeof("original String large enough for a new suffix!\0Ω")-1 :
        sizeof("original String large enough for a new suffix!")-1);
}
static void native_string_state_cases(XrtLocationOwner *locations) {
    for (uint32_t mode=0;mode<6;++mode) {
        StringStateProbe probe={.mode=mode};
        OK(string_state_new(&probe));
        XrtExecutionFailure failure={0};
        XrtExecutionStatus setup=xrt_run_embedded(locations,&failure,string_state_setup,&probe);
        if (mode>=1 && mode<=3) CHECK(setup == XRT_EXECUTION_FAILURE && failure.kind == XRT_FAILURE_VALUE);
        else {
            CHECK(setup == XRT_EXECUTION_COMPLETED);
            string_state_check(&probe,false);
            XrtExecutionStatus result=xrt_run_embedded(locations,&failure,string_state_append,&probe);
            CHECK(mode == 5 ? result == XRT_EXECUTION_FAILURE && failure.kind == XRT_FAILURE_NULL_UNWRAP :
                result == XRT_EXECUTION_COMPLETED);
            string_state_check(&probe,true);
        }
        xrt_module_state_drop(&probe.state);
    }
}
#if defined(XR_NATIVE_ARRAY_ALLOCATIONS)
static void native_string_state_sweep(XrtLocationOwner *locations) {
    StringStateProbe probe={0};
    OK(string_state_new(&probe));
    XrtExecutionFailure failure={0};
    CHECK(xrt_run_embedded(locations,&failure,string_state_setup,&probe) == XRT_EXECUTION_COMPLETED);
    native_array_calls=0;
    CHECK(xrt_run_embedded(locations,&failure,string_state_append,&probe) == XRT_EXECUTION_COMPLETED);
    size_t points=native_array_calls;
    xrt_module_state_drop(&probe.state);
    CHECK(points && !native_array_live && !native_array_bytes);
    for (size_t point=0;point<points;++point) {
        probe=(StringStateProbe){0};
        OK(string_state_new(&probe));
        CHECK(xrt_run_embedded(locations,&failure,string_state_setup,&probe) == XRT_EXECUTION_COMPLETED);
        native_array_calls=0;native_array_fail_at=point;native_array_injected=false;
        CHECK(xrt_run_embedded(locations,&failure,string_state_append,&probe) == XRT_EXECUTION_FAILURE);
        CHECK(native_array_injected && failure.kind == XRT_FAILURE_VALUE && failure.detail.value.status == XR_XIR_VALUE_OOM);
        native_array_fail_at=SIZE_MAX;
        string_state_check(&probe,false);
        xrt_module_state_drop(&probe.state);
        CHECK(!native_array_live && !native_array_bytes);
    }
    printf("String State append allocation failure points: %zu\n",points);
}
#endif
