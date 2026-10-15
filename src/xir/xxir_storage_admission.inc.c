/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_storage_admission.inc.c - Received identity views of actual owner storage
 *
 * KEY CONCEPT:
 *   A compiler-authenticated type identity permits reading storage metadata from
 *   its actual owner arena. Domain, callable and loan authority are unchanged.
 */
static XrXirValueStatus storage_admission_begin(const XrXirValue *value,
    XrXirValueAdmission *admission,XrXirValueAdmission *local) {
    if (!admission || !admission_domain_current(admission) || !local || !xr_xir_value_valid(value)) return XR_XIR_VALUE_BAD_ARGUMENT;
    *local=*admission;
    if (!owned_carrier_type((XrXirType)value->type)) return XR_XIR_VALUE_OK;
    const XirObject *owner=object_pointer(value);
    if (!owner->arena || admission->arena==owner->arena) return XR_XIR_VALUE_OK;
    uint64_t work=xir_type_arena_lookup_work(admission->arena);
    if (work>admission->work || !xr_xir_domain_work(admission->domain,work)) return XR_XIR_VALUE_LIMIT;
    admission->work-=work;local->work=admission->work;
    XrXirType expected;
    if (!xir_type_arena_local_type(admission->arena,owner->arena,(XrXirType)value->type,&expected) ||
        !xr_xir_value_argument(value,admission->arena,expected)) return XR_XIR_VALUE_BAD_ARGUMENT;
    local->arena=owner->arena;
    return XR_XIR_VALUE_OK;
}
static XrXirValueStatus storage_admission_end(XrXirValueAdmission *admission,
    const XrXirValueAdmission *local,XrXirValueStatus status) {
    admission->work=local->work;admission->scratch_bytes=local->scratch_bytes;
    return status;
}
static XrXirValueStatus storage_place_admission(const XrXirValuePlace *place,
    XrXirValueAdmission *admission,XrXirValueAdmission *local,XrXirValuePlace *storage) {
    if (!place || !place->payload || !admission_domain_current(admission) || !storage ||
        !xr_xir_type_is_array(xr_xir_compile_type_arena_types(admission->arena),place->type))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    int64_t payload;memcpy(&payload,place->payload,sizeof(payload));
    XrXirValue value={0};
    if (!xr_xir_value_from_payload(admission->arena,place->type,payload,&value)) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValueStatus status=storage_admission_begin(&value,admission,local);
    if (status==XR_XIR_VALUE_OK) *storage=(XrXirValuePlace){(XrXirType)value.type,place->payload};
    return status;
}
