/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_ctfe_identity.inc.c - Receiving multiplicities on real mutable handles
 *
 * KEY CONCEPT:
 *   Retained keys pin identities, never root snapshots or interior addresses.
 *   Numeric multiplicities bound receiving work and grant no access authority.
 */
typedef struct CtfeIdentityEntry { XrXirValue value;uint64_t count; } CtfeIdentityEntry;
typedef struct CtfeIdentitySet {
    CtfeIdentityEntry *entries;
    size_t count,capacity;
    uint64_t *frame_bytes;
    bool retained;
} CtfeIdentitySet;
static XrXirCtfeStatus ctfe_identity_find(ScalarRun *run,const CtfeIdentitySet *set,
    const XrXirValue *key,size_t *position) {
    if (set->count>UINT64_MAX/sizeof(CtfeIdentityEntry) ||
        !scalar_compile_work(run,(uint64_t)set->count*sizeof(CtfeIdentityEntry)+8)) return XR_XIR_CTFE_BUDGET;
    /* Carrier equality names an already admitted identity. No payload address
     * is dereferenced, and no field, member or loan permission is inferred. */
    for (size_t i=0;i<set->count;++i) if (set->entries[i].value.type==key->type && set->entries[i].value.payload==key->payload) {
        *position=i;return XR_XIR_CTFE_OK;
    }
    *position=set->count;return XR_XIR_CTFE_OK;
}
static XrXirCtfeStatus ctfe_identity_grow(ScalarRun *run,CtfeOwnedState *state,CtfeIdentitySet *set) {
    if (!state->loan_bytes || set->capacity>SIZE_MAX/2/sizeof(CtfeIdentityEntry)) return XR_XIR_CTFE_FRAME_LIMIT;
    size_t next=set->capacity?set->capacity*2:1,bytes=next*sizeof(CtfeIdentityEntry),old=set->capacity*sizeof(CtfeIdentityEntry);
    if ((uint64_t)bytes>ctfe_frame_spare(state)) return XR_XIR_CTFE_FRAME_LIMIT;
    if (!scalar_compile_work(run,(uint64_t)set->count*sizeof(CtfeIdentityEntry)+(uint64_t)next*sizeof(CtfeIdentityEntry)*4+32))
        return XR_XIR_CTFE_BUDGET;
    void *memory=NULL;XrXirCtfeStatus status=ctfe_compile_status(xir_compile_resource_status(
        xr_compile_resources_calloc(run->compile_resources,1,bytes,&memory)));
    if (status!=XR_XIR_CTFE_OK) return status;
    if (set->count) memcpy(memory,set->entries,set->count*sizeof(CtfeIdentityEntry));
    xr_compile_resources_free(set->entries);set->entries=memory;set->capacity=next;set->frame_bytes=state->loan_bytes;
    XR_CHECK(*set->frame_bytes>=old,"identity storage preserves its original live frame reserve");
    *set->frame_bytes=*set->frame_bytes-old+bytes;return XR_XIR_CTFE_OK;
}
static XrXirCtfeStatus ctfe_identity_add(ScalarRun *run,CtfeOwnedState *state,CtfeIdentitySet *set,
    const XrXirValue *value,uint64_t count) {
    size_t position=0;XrXirCtfeStatus status=ctfe_identity_find(run,set,value,&position);if (status!=XR_XIR_CTFE_OK) return status;
    if (position<set->count) {
        if (count>UINT64_MAX-set->entries[position].count) return XR_XIR_CTFE_BUDGET;
        set->entries[position].count+=count;return XR_XIR_CTFE_OK;
    }
    if (set->count==set->capacity) { status=ctfe_identity_grow(run,state,set);if (status!=XR_XIR_CTFE_OK) return status; }
    if (!scalar_compile_work(run,sizeof(CtfeIdentityEntry)+state->value_header_work)) return XR_XIR_CTFE_BUDGET;
    if (set->retained) status=ctfe_value_status(xr_xir_value_copy(value,&set->entries[position].value));
    else set->entries[position].value=*value;
    if (status==XR_XIR_CTFE_OK) { set->entries[position].count=count;++set->count; }
    return status;
}
static void ctfe_identity_drop(CtfeIdentitySet *set) {
    if (set->retained) for (size_t i=0;i<set->count;++i) xr_xir_value_drop(&set->entries[i].value);
    size_t bytes=set->capacity*sizeof(CtfeIdentityEntry);
    if (bytes) {
        XR_CHECK(set->frame_bytes && *set->frame_bytes>=bytes,"identity terminal returns only its own live frame bytes");
        *set->frame_bytes-=bytes;
    }
    xr_compile_resources_free(set->entries);*set=(CtfeIdentitySet){0};
}
static XrXirCtfeStatus ctfe_identity_visit(ScalarRun *run,CtfeOwnedState *state,
    const XrXirValue *value,void *context) {
    return ctfe_identity_add(run,state,context,value,1);
}
