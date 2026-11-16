/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_map_literal.inc.c - Immutable source provenance for physical constructors
 */
static bool lir_map_literal_valid(const XrLirMapLiteral *literal) {
    if(!literal || (literal->path_length!=0)!=(literal->path!=NULL) ||
        (literal->path_length && !lir_record_text_valid(literal->path,literal->path_length)) ||
        (literal->count!=0)!=(literal->items!=NULL))return false;
    size_t count=literal->count;
    if(count>SIZE_MAX/sizeof(*literal->items))return false;
    for(uint32_t n=0;n<literal->count;++n)
        if(literal->items[n].kind>1 || !literal->items[n].line || !literal->items[n].column)return false;
    return true;
}
XR_FUNC bool xr_lir_internal_map_literals_valid(const XrLirModule *m) {
    if(m->map_literal_count>m->map_literal_capacity || (m->map_literal_capacity && !m->map_literals))return false;
    for(uint32_t n=0;n<m->map_literal_count;++n)if(!lir_map_literal_valid(&m->map_literals[n]))return false;
    return true;
}
XR_FUNC XrLirStatus xr_lir_map_literal_add(XrLirModule *m,const XrLirMapLiteral *input,uint32_t *out) {
    XrLirStatus status=lir_writable(m);if(status!=XR_LIR_OK)return status;
    if(!out || !lir_map_literal_valid(input))return lir_fail(m,XR_LIR_INVALID_ARGUMENT);
    if(m->map_literal_count==UINT32_MAX)return lir_fail(m,XR_LIR_OUT_OF_MEMORY);
    XrLirMapLiteral from=*input,literal=from;
    XrLirMapLiteral *grown=lir_reserve(m,m->map_literals,m->map_literal_count,&m->map_literal_capacity,sizeof(*grown));
    if(!grown)return m->failure;
    m->map_literals=grown;
    literal.path=from.path_length?lir_record_text_copy(m,from.path,from.path_length):NULL;
    literal.items=NULL;
    if((!literal.path_length || literal.path) && literal.count)literal.items=lir_copy(m,from.items,literal.count,sizeof(*literal.items));
    if((literal.path_length && !literal.path) || (literal.count && !literal.items)) {
        xr_free((void *)literal.path);xr_free((void *)literal.items);return m->failure;
    }
    m->map_literals[m->map_literal_count++]=literal;*out=m->map_literal_count;return XR_LIR_OK;
}
XR_FUNC uint32_t xr_lir_map_literal_count(const XrLirModule *m) {
    return xr_lir_module_is_readable(m)?m->map_literal_count:0;
}
XR_FUNC XrLirStatus xr_lir_map_literal_view(const XrLirModule *m,uint32_t ordinal,XrLirMapLiteral *out) {
    if(!out || !xr_lir_module_is_readable(m) || !ordinal || ordinal>m->map_literal_count)return XR_LIR_INVALID_ARGUMENT;
    *out=m->map_literals[ordinal-1];return XR_LIR_OK;
}
XR_FUNC XrLirStatus xr_lir_map_literals_copy(XrLirModule *target,const XrLirModule *source) {
    XrLirStatus status=lir_writable(target);if(status!=XR_LIR_OK)return status;
    if(!xr_lir_module_is_readable(source) || target->map_literal_count)return lir_fail(target,XR_LIR_INVALID_ARGUMENT);
    for(uint32_t n=0;n<source->map_literal_count;++n) {
        XrLirMapLiteral literal=source->map_literals[n];uint32_t ordinal=0;
        status=xr_lir_map_literal_add(target,&literal,&ordinal);if(status!=XR_LIR_OK)return status;
        XR_DCHECK(ordinal==n+1,"receiving literal directories preserve ordinals");
    }
    return XR_LIR_OK;
}
