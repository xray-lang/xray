/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_page_value.inc.c - Domain-owned mappings and failure-preserving close
 *
 * KEY CONCEPT:
 *   Only successful provider unmap retires an allocation and its exact charge.
 *   Pointer drops release metadata; the Domain's owner retains page liability.
 */
static XrXirValueStatus page_reserve(XrXirDomain *domain,size_t bytes) {
    domain_lock(domain);XrXirValueStatus status=XR_XIR_VALUE_OK;
    if (domain->closed || bytes>domain->limit-domain->stats.live_bytes ||
        domain->stats.allocations==UINT64_MAX || (domain->budget.bound &&
        (bytes>domain->budget.requested_limit-domain->budget.requested_bytes ||
         domain->budget.work==domain->budget.work_limit))) status=XR_XIR_VALUE_LIMIT;
    else {
        if (domain->budget.bound) {domain->budget.requested_bytes+=bytes;++domain->budget.work;}
        domain->stats.live_bytes+=bytes;++domain->stats.allocations;
        if (domain->stats.live_bytes>domain->stats.peak_bytes) domain->stats.peak_bytes=domain->stats.live_bytes;
    }
    domain_unlock(domain);return status;
}
static void page_refund(XrXirDomain *domain,size_t bytes) {
    domain_lock(domain);
    XR_CHECK(domain->stats.live_bytes>=bytes && domain->stats.allocations>domain->stats.frees,
        "page retirement retains its original accounting liability");
    domain->stats.live_bytes-=bytes;++domain->stats.frees;domain_unlock(domain);
}
static void page_retire(XrXirDomain *domain,XirPageRecord **slot) {
    XirPageRecord *page=*slot;*slot=page->next;
    page->origin->page=NULL;pointer_origin_close(&page->origin);
    page_refund(domain,page->charged_bytes);xr_xir_domain_deallocate(domain,page,sizeof(*page));
}
static XrXirValueStatus domain_pages_close(XrXirDomain *domain) {
    while (domain->pages) {
        XirPageRecord *page=domain->pages;
        if (!xr_mem_unmap(page->address,page->length)) return XR_XIR_VALUE_IO;
        page_retire(domain,&domain->pages);
    }
    return XR_XIR_VALUE_OK;
}
XR_FUNC XrXirValueStatus xr_xir_page_allocate(XrXirDomain *domain,const XrXirTypeArena *arena,
    XrXirType type,int64_t bytes,int64_t protection,XrXirValue *output) {
    if (!xr_xir_domain_current(domain) || xr_xir_domain_is_compile(domain) || domain->closed ||
        !unit_value(output) || !xr_xir_pointer_writable(xr_xir_compile_type_arena_types(arena),type) ||
        bytes<=0 || (uint64_t)bytes>SIZE_MAX || protection<0 || protection>7) return XR_XIR_VALUE_BAD_ARGUMENT;
    size_t page_size=xr_mem_page_size();
    if (!page_size || (size_t)bytes>SIZE_MAX-(page_size-1)) return XR_XIR_VALUE_OVERFLOW;
    size_t charged=((size_t)bytes+page_size-1)/page_size*page_size;
    xr_xir_value_graph_begin();XirPointerOrigin *origin=NULL;XrXirValue value={0};
    XrXirValueStatus status=pointer_origin_new(domain,&origin);
    XirPageRecord *record=status==XR_XIR_VALUE_OK?xr_xir_domain_allocate(domain,sizeof(*record),&status):NULL;
    if (record) {
        memset(record,0,sizeof(*record));XirPointer source={.origin=origin,.generation=origin->generation};
        status=pointer_new(domain,arena,type,&source,&value);
    }
    if (status==XR_XIR_VALUE_OK) status=page_reserve(domain,charged);
    if (status==XR_XIR_VALUE_OK) {
        void *address=xr_mem_map((size_t)bytes,(int)protection);
        if (!address) {page_refund(domain,charged);status=XR_XIR_VALUE_OOM;}
        else {
            *record=(XirPageRecord){domain->pages,origin,address,(size_t)bytes,charged};
            origin->page=record;domain->pages=record;
            ((XirPointer *)object_pointer(&value))->address=(uintptr_t)address;
            *output=value;xr_xir_value_graph_end();return XR_XIR_VALUE_OK;
        }
    }
    xr_xir_value_drop(&value);
    if (record) xr_xir_domain_deallocate(domain,record,sizeof(*record));
    pointer_origin_drop(origin);xr_xir_value_graph_end();return status;
}
static bool pointer_page(const XrXirValue *value,int64_t bytes,const XirPointer **pointer) {
    if (bytes<=0 || (uint64_t)bytes>SIZE_MAX || !pointer_argument(value,true,pointer) ||
        !pointer_live(*pointer) || !(*pointer)->origin || !(*pointer)->origin->page) return false;
    XirPageRecord *page=(*pointer)->origin->page;
    return (*pointer)->address==(uintptr_t)page->address && (size_t)bytes==page->length &&
        page->origin==(*pointer)->origin;
}
XR_FUNC XrXirValueStatus xr_xir_page_protect(const XrXirValue *value,int64_t bytes,int64_t protection,bool *output) {
    if (!output) return XR_XIR_VALUE_BAD_ARGUMENT;
    const XirPointer *pointer=NULL;
    if (protection<0 || protection>7 || !pointer_page(value,bytes,&pointer)) {*output=false;return XR_XIR_VALUE_OK;}
    XirPageRecord *page=pointer->origin->page;
    *output=xr_mem_protect(page->address,page->length,(int)protection);return XR_XIR_VALUE_OK;
}
XR_FUNC XrXirValueStatus xr_xir_page_free(const XrXirValue *value,int64_t bytes,bool *output) {
    if (!output) return XR_XIR_VALUE_BAD_ARGUMENT;
    const XirPointer *pointer=NULL;*output=false;
    if (!pointer_page(value,bytes,&pointer)) return XR_XIR_VALUE_OK;
    XrXirDomain *domain=pointer->object.domain;XirPageRecord **slot=&domain->pages;
    while (*slot && *slot!=pointer->origin->page) slot=&(*slot)->next;
    if (!*slot) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!xr_mem_unmap((*slot)->address,(*slot)->length)) return XR_XIR_VALUE_OK;
    page_retire(domain,slot);*output=true;return XR_XIR_VALUE_OK;
}
