/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_slot_exports.inc.c - Budgeted immutable slot export names
 *
 * KEY CONCEPT:
 *   Names index the already verified owned literal pool. Visibility remains
 *   separate from type, initialization, Domain and root-execution permission.
 */
static XrXirStatus declaration_slot_exports(const XrXirCompileContext *work,
    const XrXirDeclarations *d) {
    size_t count=0;
    for (uint32_t s=0;s<d->slot_count;++s) {
        if (!xir_compile_work(work,1)) return XR_XIR_BUDGET;
        if (d->slots[s].exported) ++count;
    }
    if (!count) return XR_XIR_OK;
    if (count>SIZE_MAX/2) return XR_XIR_BUDGET;
    size_t capacity=1;
    while (capacity<count*2) {
        if (capacity>SIZE_MAX/2) return XR_XIR_BUDGET;
        capacity*=2;
    }
    XrXirStatus status=XR_XIR_OK;
    uint32_t *entries=xir_compile_calloc(work,capacity,sizeof(*entries),&status);
    if (!entries) return status;
    for (uint32_t s=0;s<d->slot_count && status==XR_XIR_OK;++s) {
        if (!xir_compile_work(work,1)) {status=XR_XIR_BUDGET;break;}
        const XrXirSlot *slot=&d->slots[s];
        if (!slot->exported) continue;
        const XrXirLiteral *name=&d->literals[slot->name];
        if (!name->length || !name->bytes) {status=XR_XIR_BAD_STRUCTURE;break;}
        if (!xir_compile_work(work,(uint64_t)name->length*2+1)) {status=XR_XIR_BUDGET;break;}
        if (memchr(name->bytes,0,name->length)) {status=XR_XIR_BAD_STRUCTURE;break;}
        uint64_t hash=UINT64_C(14695981039346656037)^slot->module;
        for (uint32_t i=0;i<name->length;++i)
            hash=(hash^(unsigned char)name->bytes[i])*UINT64_C(1099511628211);
        size_t at=(size_t)hash&(capacity-1);
        for (;;) {
            if (!xir_compile_work(work,1)) {status=XR_XIR_BUDGET;break;}
            if (!entries[at]) {entries[at]=s+1;break;}
            const XrXirSlot *previous=&d->slots[entries[at]-1];
            const XrXirLiteral *other=&d->literals[previous->name];
            if (previous->module==slot->module && other->length==name->length) {
                if (!xir_compile_work(work,name->length)) {status=XR_XIR_BUDGET;break;}
                if (!memcmp(other->bytes,name->bytes,name->length)) {status=XR_XIR_BAD_STRUCTURE;break;}
            }
            at=(at+1)&(capacity-1);
        }
    }
    xr_compile_resources_free(entries);return status;
}
