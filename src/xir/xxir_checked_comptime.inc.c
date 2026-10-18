/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_checked_comptime.inc.c - Explicit current site and substitution wire facts
 *
 * KEY CONCEPT:
 *   Every owned site and ordered argument is an explicit recursive wire fact.
 */
static void checked_comptime(CheckedCursor *c,XrXirConstruction *owner) {
    uint32_t count=checked_u32(c,c->reading?0:owner->comptime.site_count);
    if (c->status!=XR_XIR_OK) return;
    XrXirComptimeSite *sites=checked_array(c,owner->comptime.sites,count,sizeof(*sites),12);
    if (c->reading) { owner->comptime.sites=sites;owner->comptime.site_count=sites?count:0; }
    for (uint32_t s=0;s<count && c->status==XR_XIR_OK;++s) {
        XrXirComptimeSite row=sites[s];row.caller=checked_u32(c,row.caller);
        row.instruction=checked_u32(c,row.instruction);row.helper=checked_u32(c,row.helper);
        if (c->reading) sites[s]=row;
    }
    count=checked_u32(c,c->reading?0:owner->comptime.binding_count);
    if (c->status!=XR_XIR_OK) return;
    XrXirComptimeBinding *bindings=checked_array(c,owner->comptime.bindings,count,sizeof(*bindings),12);
    if (c->reading) { owner->comptime.bindings=bindings;owner->comptime.binding_count=bindings?count:0; }
    for (uint32_t b=0;b<count && c->status==XR_XIR_OK;++b) {
        XrXirComptimeBinding row=bindings[b];row.site=checked_u32(c,row.site);
        uint32_t arguments=checked_u32(c,row.argument_count);
        XrXirType *types=checked_array(c,row.arguments,arguments,sizeof(*types),4);
        if (c->reading) {
            row.arguments=types;row.argument_count=types?arguments:0;bindings[b]=row;
        }
        for (uint32_t a=0;a<arguments && c->status==XR_XIR_OK;++a) {
            XrXirType type=(XrXirType)checked_u32(c,(uint32_t)types[a]);
            if (c->reading) types[a]=type;
        }
        row.provider=checked_u32(c,row.provider);
        if (c->reading) bindings[b].provider=row.provider;
    }
}
