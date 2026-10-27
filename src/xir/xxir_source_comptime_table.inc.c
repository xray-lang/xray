/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_comptime_table.inc.c - Canonical receiving across module owners
 *
 * KEY CONCEPT:
 *   Relocated coordinates determine site ordinals. Existing complete ordered
 *   keys and providers retain meaning when local and imported tables merge.
 */
static bool source_comptime_table_sites(SourceContext *ctx,const XrXirComptimeTable *tables,
    uint32_t count,XrXirComptimeSite *sites,uint32_t total) {
    uint32_t next=0;
    for (uint32_t t=0;t<count;++t) for (uint32_t s=0;s<tables[t].site_count;++s) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        XrXirComptimeSite row=tables[t].sites[s];uint32_t at=next;
        if (next==total) return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"comptime site inventory grew");
        while (at) {
            if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
            const XrXirComptimeSite *prior=&sites[at-1];
            if (prior->caller==row.caller && prior->instruction==row.instruction)
                return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"comptime site coordinate is duplicated");
            if (prior->caller<row.caller || (prior->caller==row.caller && prior->instruction<row.instruction)) break;
            sites[at]=*prior;--at;
        }
        sites[at]=row;++next;
    }
    return next==total || source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"comptime site inventory is incomplete");
}
static bool source_comptime_table_rows(SourceContext *ctx,const XrXirComptimeTable *tables,
    uint32_t count,const XrXirComptimeSite *sites,uint32_t site_count,XrXirComptimeBinding *rows,uint32_t total) {
    uint32_t next=0;
    for (uint32_t t=0;t<count;++t) for (uint32_t b=0;b<tables[t].binding_count;++b) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        XrXirComptimeBinding row=tables[t].bindings[b];
        if (row.site>=tables[t].site_count || next==total)
            return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"comptime binding has no original site");
        const XrXirComptimeSite *original=&tables[t].sites[row.site];row.site=UINT32_MAX;
        for (uint32_t s=0;s<site_count;++s) {
            if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
            if (sites[s].caller==original->caller && sites[s].instruction==original->instruction && sites[s].helper==original->helper) row.site=s;
        }
        if (row.site==UINT32_MAX) return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"comptime binding lost its relocated site");
        uint32_t at=next;
        while (at) {
            if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
            int order=xir_comptime_key_order(&rows[at-1],&row);
            if (!order) return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"comptime ordered key is duplicated");
            if (order<0) break;
            if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
            rows[at]=rows[at-1];--at;
        }
        rows[at]=row;++next;
    }
    return next==total || source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"comptime binding inventory is incomplete");
}
static bool source_comptime_table_merge(SourceContext *ctx,const XrXirComptimeTable *tables,
    uint32_t count,XrXirComptimeTable *output) {
    uint32_t site_count=0,binding_count=0;
    for (uint32_t t=0;t<count;++t) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        if (site_count>ctx->compile.limits.instructions || tables[t].site_count>ctx->compile.limits.instructions-site_count ||
            binding_count>ctx->compile.limits.instructions || tables[t].binding_count>ctx->compile.limits.instructions-binding_count)
            return source_fail(ctx,NULL,XR_XIR_BUDGET,"comptime merged inventory exhausted");
        site_count+=tables[t].site_count;binding_count+=tables[t].binding_count;
    }
    XrXirComptimeSite *sites=site_count?source_alloc(ctx,site_count,sizeof(*sites)):NULL;
    XrXirComptimeBinding *rows=binding_count?source_alloc(ctx,binding_count,sizeof(*rows)):NULL;
    if ((site_count && !sites) || (binding_count && !rows) ||
        !source_comptime_table_sites(ctx,tables,count,sites,site_count) ||
        !source_comptime_table_rows(ctx,tables,count,sites,site_count,rows,binding_count)) return false;
    *output=(XrXirComptimeTable){sites,site_count,rows,binding_count};return true;
}
static bool source_library_comptime_table(SourceContext *ctx,XrXirComptimeTable *output) {
    uint32_t count=0;
    for (SourceLibraryUnit *unit=ctx->library_units;unit;unit=unit->next) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        if (count==ctx->compile.limits.functions) return source_fail(ctx,NULL,XR_XIR_BUDGET,"library comptime owner inventory exhausted");
        ++count;
    }
    XrXirComptimeTable *tables=count?source_alloc(ctx,count,sizeof(*tables)):NULL;
    if (count && !tables) return false;
    uint32_t next=0;
    for (SourceLibraryUnit *unit=ctx->library_units;unit;unit=unit->next) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        tables[next++]=unit->comptime;
    }
    return source_comptime_table_merge(ctx,tables,count,output);
}
