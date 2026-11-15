/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * Full panic text has an independent owner after activation teardown. */
typedef struct NativeMessagePanicCase {
    const char *bytes;
    size_t length;
    XrXirDomain *pin;
    uint64_t baseline;
} NativeMessagePanicCase;
static void native_message_panic_root(void *opaque) {
    NativeMessagePanicCase *context=opaque;XrtValueRoot *root=xrt_value_root_current(0);
    context->pin=xrt_value_root_domain(root,0);CHECK(xr_xir_domain_retain(context->pin));
    context->baseline=xr_xir_domain_stats(context->pin).live_bytes;
    XrXirValueStatus status=xr_xir_string_new(context->pin,context->bytes,context->length,&root->pending);
    if(status!=XR_XIR_VALUE_OK)xrt_value_failure(status,0);
    xrt_message_panic_take(&root->pending,434,0);
}
static void native_message_panic_owners(void) {
    char bytes[4096],expected[4096];
    for(size_t n=0;n<sizeof(bytes);++n)bytes[n]=n%17?'x':'\0';
    memcpy(expected,bytes,sizeof(bytes));
    XrtLocationOwner *locations=NULL;CHECK(xrt_locations_create(NULL,0,&locations)==XRT_LOCATION_OK);
    XrtValueRoot root={0};XrtExecutionFailure failure={0};
    NativeMessagePanicCase context={.bytes=bytes,.length=sizeof(bytes)};
#if defined(XR_NATIVE_STRING_ALLOCATIONS)
    size_t before=native_string_calls;
#endif
    CHECK(xrt_run_embedded_owned(locations,&failure,native_message_panic_root,&context,&root)==XRT_EXECUTION_FAILURE);
    CHECK(xrt_value_root_empty(&root) && failure.kind==XRT_FAILURE_MESSAGE_PANIC &&
        failure.detail.message.code==434 && failure.detail.message.source_id==0);
    memset(bytes,0xa5,sizeof(bytes));const char *text=NULL;size_t length=0;
    CHECK(xr_xir_string_view(&failure.detail.message.message,&text,&length) && length==sizeof(expected) && !memcmp(text,expected,length));
#if defined(XR_NATIVE_STRING_ALLOCATIONS)
    size_t sites=native_string_calls-before;
#endif
    xrt_execution_failure_drop(&failure);
    CHECK(xr_xir_domain_stats(context.pin).live_bytes==context.baseline);
    xr_xir_domain_drop(context.pin);context.pin=NULL;
#if defined(XR_NATIVE_STRING_ALLOCATIONS)
    CHECK(!native_string_live && !native_string_bytes && sites>0);
    memcpy(bytes,expected,sizeof(bytes));
    for(size_t point=0;point<sites;++point) {
        native_string_fail_at=native_string_calls+point;native_string_injected=false;
        CHECK(xrt_run_embedded_owned(locations,&failure,native_message_panic_root,&context,&root)==XRT_EXECUTION_FAILURE);
        native_string_fail_at=SIZE_MAX;
        if(failure.kind!=XRT_FAILURE_VALUE || failure.detail.value.status!=XR_XIR_VALUE_OOM || !native_string_injected)
            fprintf(stderr,"message fault point=%zu/%zu kind=%u status=%u injected=%u empty=%u\n",point,sites,
                (unsigned)failure.kind,(unsigned)failure.detail.value.status,(unsigned)native_string_injected,(unsigned)xrt_value_root_empty(&root));
        CHECK(failure.kind==XRT_FAILURE_VALUE && failure.detail.value.status==XR_XIR_VALUE_OOM &&
            xrt_value_root_empty(&root) && native_string_injected);
        xrt_execution_failure_drop(&failure);
        if(context.pin) {
            CHECK(xr_xir_domain_stats(context.pin).live_bytes==context.baseline);
            xr_xir_domain_drop(context.pin);context.pin=NULL;
        }
        CHECK(!native_string_live && !native_string_bytes);
    }
#endif
    xrt_locations_release(locations);
}
