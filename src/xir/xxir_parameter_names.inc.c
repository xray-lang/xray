/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_parameter_names.inc.c - Bounded, receiving-owned declaration namespaces
 */
static XrXirStatus declaration_parameter_names_verify(const XrXirCompileContext *context,
    const XrXirDeclarations *d, uint32_t functions, const XrXirTypes *types,
    bool check_types, XrCompileRequestControl *control) {
    if ((d->parameter_name_count!=0)!=(d->parameter_names!=NULL)) return XR_XIR_BAD_STRUCTURE;
    if (d->parameter_name_count>context->limits.instructions) return XR_XIR_BUDGET;
    uint64_t bytes=context->limits.frame_bytes, work=context->limits.instructions;
    uint32_t remaining=context->limits.parameters;
    for (uint32_t n=0;n<d->parameter_name_count;++n) {
        XrXirStatus status=xir_receive_poll(control);
        if (status!=XR_XIR_OK) return status;
        const XrXirParameterNames *row=&d->parameter_names[n];
        if (row->kind>XR_XIR_PARAMETER_REQUIREMENT || (row->count!=0)!=(row->names!=NULL))
            return XR_XIR_BAD_STRUCTURE;
        if (n) {
            const XrXirParameterNames *prev=&d->parameter_names[n-1];
            if (row->kind<prev->kind || (row->kind==prev->kind &&
                (row->owner<prev->owner || (row->owner==prev->owner && row->member<=prev->member))))
                return XR_XIR_BAD_STRUCTURE;
        }
        if (row->kind==XR_XIR_PARAMETER_FUNCTION) {
            if (row->owner>=functions || row->member || !d->functions) return XR_XIR_BAD_STRUCTURE;
            const XrXirFunctionIdentity *id=&d->functions[row->owner];
            uint64_t offset=(uint64_t)id->capture_count+(id->method_kind==XR_XIR_READ_METHOD?1u:0u);
            if (offset!=row->parameter_offset) return XR_XIR_BAD_STRUCTURE;
        } else if (check_types) {
            if (!types || !types->interfaces || row->owner>=types->interfaces->count)
                return XR_XIR_BAD_STRUCTURE;
            const XrXirInterfaceDeclaration *owner=&types->interfaces->declarations[row->owner];
            if (row->member>=owner->method_count || row->parameter_offset) return XR_XIR_BAD_STRUCTURE;
            const XrXirTypeNode *signature=xr_xir_callable_signature(types,owner->methods[row->member].signature);
            if (!signature || signature->parameter_count!=row->count) return XR_XIR_BAD_STRUCTURE;
        }
        if (row->count>remaining || row->count>UINT32_MAX/2) return XR_XIR_BUDGET;
        remaining-=row->count;
        uint32_t capacity=1;
        while (capacity<row->count*2u) {
            if (capacity>UINT32_MAX/2) return XR_XIR_BUDGET;
            capacity*=2;
        }
        uint32_t *buckets=row->count?xir_compile_calloc(context,capacity,sizeof(*buckets),&status):NULL;
        if (row->count && !buckets) return status;
        for (uint32_t p=0;p<row->count && status==XR_XIR_OK;++p) {
            XrXirLiteral name=row->names[p];
            if ((name.length!=0)!=(name.bytes!=NULL)) { status=XR_XIR_BAD_STRUCTURE;break; }
            if (name.length>bytes) { status=XR_XIR_BUDGET;break; }
            bytes-=name.length;
            status=xir_constant_key_utf8((XrXirConstantKey){.kind=XR_XIR_KEY_STRING,.string=name},control);
            if (status!=XR_XIR_OK || !name.length) continue;
            uint64_t hash=UINT64_C(14695981039346656037);
            for (uint32_t b=0;b<name.length;++b) {
                if (!(b%4096)) { status=xir_receive_poll(control);if(status!=XR_XIR_OK)break; }
                if (!name.bytes[b]) { status=XR_XIR_BAD_STRUCTURE;break; }
                hash=(hash^(uint8_t)name.bytes[b])*UINT64_C(1099511628211);
            }
            if (status!=XR_XIR_OK) break;
            uint32_t slot=(uint32_t)hash&(capacity-1);
            while (buckets[slot]) {
                if (!work) { status=XR_XIR_BUDGET;break; }
                --work;
                XrXirLiteral prior=row->names[buckets[slot]-1];
                bool same=prior.length==name.length;
                for (uint32_t b=0;same && b<name.length;) {
                    status=xir_receive_poll(control);if(status!=XR_XIR_OK)break;
                    uint32_t chunk=name.length-b<4096?name.length-b:4096;
                    if (chunk>bytes) { status=XR_XIR_BUDGET;break; }
                    bytes-=chunk;same=memcmp(prior.bytes+b,name.bytes+b,chunk)==0;b+=chunk;
                }
                if (status!=XR_XIR_OK) break;
                if (same) { status=XR_XIR_BAD_STRUCTURE;break; }
                slot=(slot+1)&(capacity-1);
            }
            if (status==XR_XIR_OK) buckets[slot]=p+1;
        }
        xr_compile_resources_free(buckets);
        if (status!=XR_XIR_OK) return status;
    }
    return xir_receive_poll(control);
}
static void declaration_parameter_names_free(const XrXirDeclarations *d) {
    if (d->parameter_names) for (uint32_t n=0;n<d->parameter_name_count;++n) {
        const XrXirParameterNames *row=&d->parameter_names[n];
        if (row->names) for (uint32_t p=0;p<row->count;++p)
            xr_compile_resources_free((void *)row->names[p].bytes);
        xr_compile_resources_free((void *)row->names);
    }
    xr_compile_resources_free((void *)d->parameter_names);
}
