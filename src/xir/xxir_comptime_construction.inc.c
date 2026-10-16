/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_comptime_construction.inc.c - Owned canonical compile-time call keys
 *
 * KEY CONCEPT:
 *   Receiving owners copy complete keys and retain no borrowed producer vector.
 */
#include "xxir_type_match_internal.h"
static int comptime_key_order(const XrXirComptimeBinding *a,const XrXirComptimeBinding *b) {
    if (a->site!=b->site) return a->site<b->site?-1:1;
    uint32_t count=a->argument_count<b->argument_count?a->argument_count:b->argument_count;
    for (uint32_t i=0;i<count;++i)
        if (a->arguments[i]!=b->arguments[i]) return a->arguments[i]<b->arguments[i]?-1:1;
    return a->argument_count==b->argument_count?0:a->argument_count<b->argument_count?-1:1;
}
static XrXirStatus comptime_storage_shape(const XrXirCompileContext *context,const XrXirComptimeTable *table) {
    if (!table) return XR_XIR_OK;
    if (!!table->sites!=!!table->site_count || !!table->bindings!=!!table->binding_count) return XR_XIR_BAD_STRUCTURE;
    if (!table->site_count && !table->binding_count) return XR_XIR_OK;
    uint64_t bytes=(uint64_t)table->site_count*sizeof(*table->sites)+(uint64_t)table->binding_count*sizeof(*table->bindings);
    if (bytes>SIZE_MAX) return XR_XIR_BUDGET;
    XrXirStatus status=xir_compile_resource_status(xr_compile_resources_admit(context->resources,bytes,bytes));
    if (status!=XR_XIR_OK) return status;
    for (uint32_t s=0;s<table->site_count;++s) {
        if (!xir_compile_work(context,sizeof(*table->sites)+4)) return XR_XIR_BUDGET;
        const XrXirComptimeSite *site=&table->sites[s];
        if (site->caller==site->helper || (s && (table->sites[s-1].caller>site->caller ||
            (table->sites[s-1].caller==site->caller && table->sites[s-1].instruction>=site->instruction)))) return XR_XIR_BAD_STRUCTURE;
    }
    for (uint32_t b=0;b<table->binding_count;++b) {
        if (!xir_compile_work(context,sizeof(*table->bindings)+4)) return XR_XIR_BUDGET;
        const XrXirComptimeBinding *row=&table->bindings[b];
        if (row->site>=table->site_count || !row->argument_count || !row->arguments) return XR_XIR_BAD_STRUCTURE;
        bytes=(uint64_t)row->argument_count*sizeof(*row->arguments);
        if (bytes>SIZE_MAX) return XR_XIR_BUDGET;
        status=xir_compile_resource_status(xr_compile_resources_admit(context->resources,bytes,bytes));
        if (status!=XR_XIR_OK) return status;
        if (!xir_compile_work(context,bytes*2+4)) return XR_XIR_BUDGET;
        if (b && comptime_key_order(&table->bindings[b-1],row)>=0) return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}
static XrXirStatus comptime_storage_copy(const XrXirCompileContext *context,const XrXirComptimeTable *source,
    XrXirComptimeTable *output) {
    if (!source) return XR_XIR_OK;
    XrXirStatus status=XR_XIR_OK;
    if (source->site_count) {
        output->sites=xir_compile_copy(context,source->sites,(size_t)source->site_count*sizeof(*source->sites),&status);
        if (!output->sites) return status;output->site_count=source->site_count;
    }
    if (source->binding_count) {
        output->bindings=xir_compile_calloc(context,source->binding_count,sizeof(*source->bindings),&status);
        if (!output->bindings) return status;output->binding_count=source->binding_count;
        for (uint32_t b=0;b<source->binding_count;++b) {
            if (!xir_compile_work(context,sizeof(*source->bindings))) return XR_XIR_BUDGET;
            XrXirComptimeBinding *to=&((XrXirComptimeBinding *)output->bindings)[b];
            *to=source->bindings[b];to->arguments=NULL;
            to->arguments=xir_compile_copy(context,source->bindings[b].arguments,(size_t)to->argument_count*sizeof(*to->arguments),&status);
            if (!to->arguments) return status;
        }
    }
    return status;
}
XR_FUNC const XrXirComptimeTable *xr_xir_compile_construction_comptime(const XrXirConstruction *owner) {
    return owner?&owner->comptime:NULL;
}
XR_FUNC const XrXirComptimeSite *xir_comptime_site(const XrXirConstruction *owner,uint32_t caller,uint32_t instruction) {
    uint32_t low=0,high=owner?owner->comptime.site_count:0;
    while (low<high) {
        uint32_t middle=low+(high-low)/2;const XrXirComptimeSite *row=&owner->comptime.sites[middle];
        if (row->caller<caller || (row->caller==caller && row->instruction<instruction)) low=middle+1;else high=middle;
    }
    if (!owner || low==owner->comptime.site_count) return NULL;
    const XrXirComptimeSite *row=&owner->comptime.sites[low];
    return row->caller==caller && row->instruction==instruction?row:NULL;
}
XR_FUNC XrXirStatus xir_comptime_select(const XrXirCompileContext *context,const XrXirConstruction *owner,
    const XirComptimeSelection *input,const XrXirComptimeBinding **output) {
    if (!input || !input->source || !owner || !output || *output || (input->argument_count && !input->arguments)) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
    if (!owner->comptime.site_count) return XR_XIR_OK;
    if (!xir_compile_work(context,256)) return XR_XIR_BUDGET;
    const XrXirComptimeSite *site=xir_comptime_site(owner,input->caller,input->instruction);
    if (!site) return XR_XIR_OK;
    uint32_t ordinal=(uint32_t)(site-owner->comptime.sites);
    const XrXirComptimeBinding *found=NULL;
    for (uint32_t b=0;b<owner->comptime.binding_count;++b) {
        if (!xir_compile_work(context,sizeof(XrXirComptimeBinding))) return XR_XIR_BUDGET;
        const XrXirComptimeBinding *row=&owner->comptime.bindings[b];
        if (row->site!=ordinal || row->argument_count!=input->argument_count) continue;
        bool same=true;
        for (uint32_t a=0;a<row->argument_count;++a) {
            XrXirStatus status=xr_xir_compile_type_substitution_matches_between(context,input->source->types,input->actual_types,
                NULL,0,row->arguments[a],input->arguments[a]);
            if (status==XR_XIR_BAD_TYPE) { same=false;break; }
            if (status!=XR_XIR_OK) return status;
        }
        if (!same) continue;
        if (found) return XR_XIR_BAD_STRUCTURE;
        found=row;
    }
    if (!found) return XR_XIR_BAD_TYPE;
    *output=found;return XR_XIR_OK;
}
