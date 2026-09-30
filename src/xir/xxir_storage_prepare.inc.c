/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_storage_prepare.inc.c - Unpublished owned packed storage transactions
 */
typedef struct StoragePrepared {
    StoragePack pack;
    unsigned char *bytes;
    uint64_t compact;
    uint32_t stride;
    bool owns;
} StoragePrepared;
static void storage_prepared_end(StoragePrepared *prepared) {
    if (prepared->owns) storage_pack_release(&prepared->pack, prepared->bytes, UINT64_MAX);
    if (prepared->bytes && prepared->stride > sizeof(prepared->compact)) {
        xr_xir_domain_deallocate(prepared->pack.admission->domain, prepared->bytes, prepared->stride);
        prepared->pack.admission->scratch_bytes += prepared->stride;
    }
    storage_pack_end(&prepared->pack);
}
static XrXirValueStatus storage_prepared_begin(StoragePrepared *prepared,
    const XrXirValue *value, uint32_t stride, XrXirValueAdmission *admission) {
    *prepared = (StoragePrepared){0}; prepared->stride = stride;
    XrXirValueStatus status = storage_pack_begin(admission, (XrXirType)value->type, &prepared->pack);
    if (status != XR_XIR_VALUE_OK) return status;
    if (stride <= sizeof(prepared->compact)) prepared->bytes = (unsigned char *)&prepared->compact;
    else {
        if (stride > admission->scratch_bytes) { storage_pack_end(&prepared->pack); return XR_XIR_VALUE_LIMIT; }
        prepared->bytes = xr_xir_domain_allocate(admission->domain, stride, &status);
        if (!prepared->bytes) { storage_pack_end(&prepared->pack); return status; }
        admission->scratch_bytes -= stride;
    }
    status = storage_pack_value(&prepared->pack, value, prepared->bytes);
    if (status != XR_XIR_VALUE_OK) { storage_prepared_end(prepared); return status; }
    prepared->owns = true; return XR_XIR_VALUE_OK;
}
