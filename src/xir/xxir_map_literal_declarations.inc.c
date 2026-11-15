/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_map_literal_declarations.inc.c - Receiving-owned literal provenance
 */
static XrXirStatus declaration_map_literals_verify(const XrXirCompileContext *context,const XrXirDeclarations *d) {
    if((d->map_literal_count!=0)!=(d->map_literals!=NULL))return XR_XIR_BAD_STRUCTURE;
    if(d->map_literal_count>context->limits.instructions)return XR_XIR_BUDGET;
    uint64_t remaining=context->limits.parameters;
    for(uint32_t n=0;n<d->map_literal_count;++n) {
        XrXirMapLiteral literal=d->map_literals[n];
        if(literal.module>=d->module_count || (literal.file.length!=0)!=(literal.file.bytes!=NULL) ||
            (literal.file.length && (memchr(literal.file.bytes,0,literal.file.length) ||
            xr_utf8_core_scan_strict((const uint8_t *)literal.file.bytes,literal.file.length).error!=XR_UTF8_OK)) ||
            literal.count>65536 || (literal.count!=0)!=(literal.items!=NULL))return XR_XIR_BAD_STRUCTURE;
        if(literal.count>remaining)return XR_XIR_BUDGET;
        remaining-=literal.count;
        for(uint32_t c=0;c<literal.count;++c)
            if(literal.items[c].kind>1 || !literal.items[c].line || !literal.items[c].column)return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}
static void declaration_map_literals_free(const XrXirDeclarations *d) {
    if(d->map_literals)for(uint32_t n=0;n<d->map_literal_count;++n) {
        xr_compile_resources_free((void *)d->map_literals[n].file.bytes);
        xr_compile_resources_free((void *)d->map_literals[n].items);
    }
    xr_compile_resources_free((void *)d->map_literals);
}
