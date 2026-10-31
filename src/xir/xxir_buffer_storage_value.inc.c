/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_buffer_storage_value.inc.c - Failure-atomic byte-storage allocation
 *
 * KEY CONCEPT:
 *   Metadata and aligned payload have separate exact domain charges. Publication
 *   happens only after both allocations and initialization have completed.
 */
static XrXirValueStatus buffer_storage_new(XrXirDomain *domain,int64_t bytes,
    int64_t alignment,bool zeroed,XrXirValue *output) {
    if (!xr_xir_domain_current(domain) || xr_xir_domain_is_compile(domain) || !unit_value(output) ||
        bytes<0 || (uint64_t)bytes>SIZE_MAX || alignment<0 || (uint64_t)alignment>SIZE_MAX ||
        (alignment && (alignment<(int64_t)sizeof(void *) || (alignment&(alignment-1)))))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!xr_xir_domain_work(domain,zeroed ? (uint64_t)bytes+1u : 1u)) return XR_XIR_VALUE_LIMIT;
    if (!xr_xir_domain_retain(domain)) return XR_XIR_VALUE_REFCOUNT_LIMIT;
    XrXirValueStatus status=XR_XIR_VALUE_OK;
    XirBufferStorage *storage=xr_xir_domain_allocate(domain,sizeof(*storage),&status);
    if (!storage) {xr_xir_domain_drop(domain);return status;}
    memset(storage,0,sizeof(*storage));
    if (bytes) {
        storage->data=domain_payload_allocate(domain,(size_t)bytes,(size_t)alignment,zeroed,&status);
        if (!storage->data) {
            xr_xir_domain_deallocate(domain,storage,sizeof(*storage));xr_xir_domain_drop(domain);return status;
        }
        if (zeroed && alignment) memset(storage->data,0,(size_t)bytes);
    }
    storage->length=(size_t)bytes;storage->alignment=(size_t)alignment;
    atomic_init(&storage->object.references,1);
    storage->object.domain=domain;storage->object.type=XR_XIR_BUFFER_STORAGE;
    storage->object.kind=XIR_OBJECT_BUFFER_STORAGE;graph_object_initialize(&storage->object);
    xr_xir_value_object_publish(&storage->object);
    XrXirValue result={XR_XIR_BUFFER_STORAGE,0,0};memcpy(&result.payload,&storage,sizeof(storage));
    *output=result;return XR_XIR_VALUE_OK;
}
XR_FUNC XrXirValueStatus xr_xir_buffer_storage_new(XrXirDomain *domain,int64_t bytes,
    int64_t alignment,bool zeroed,XrXirValue *output) {
    xr_xir_value_graph_begin();
    XrXirValueStatus status=buffer_storage_new(domain,bytes,alignment,zeroed,output);
    xr_xir_value_graph_end();return status;
}
XR_FUNC XrXirValueStatus xr_xir_buffer_storage_length(const XrXirValue *storage,int64_t *output) {
    if (!output || !xr_xir_value_argument(storage,NULL,XR_XIR_BUFFER_STORAGE)) return XR_XIR_VALUE_BAD_ARGUMENT;
    const XirBufferStorage *body=(const XirBufferStorage *)object_pointer(storage);
    if (body->length>INT64_MAX) return XR_XIR_VALUE_BAD_ARGUMENT;
    *output=(int64_t)body->length;return XR_XIR_VALUE_OK;
}
XR_FUNC bool xr_xir_buffer_storage_observe(const XrXirValue *storage,
    const void **bytes,size_t *length,size_t *alignment) {
    if (!bytes || !length || !alignment || !xr_xir_value_argument(storage,NULL,XR_XIR_BUFFER_STORAGE)) return false;
    const XirBufferStorage *body=(const XirBufferStorage *)object_pointer(storage);
    *bytes=body->data;*length=body->length;*alignment=body->alignment;return true;
}
