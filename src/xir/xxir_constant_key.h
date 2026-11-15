/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * Shared validation for syntax-derived constants in HIR and persisted slots. */
#ifndef XXIR_CONSTANT_KEY_H
#define XXIR_CONSTANT_KEY_H
#include "xxir_declarations.h"
#include "xxir_nominal.h"
#include "xxir_types.h"
#include "../shared/xr_utf8_core.h"
#include "xxir_receive_control.h"
static inline bool xir_constant_key_header(XrXirConstantKey key) {
    if(key.kind==XR_XIR_KEY_STRING)return !key.bits && (!key.string.length || key.string.bytes);
    if(key.string.bytes || key.string.length)return false;
    if(key.kind==XR_XIR_KEY_DYNAMIC)return !key.bits;
    return key.kind==XR_XIR_KEY_SCALAR || key.kind==XR_XIR_KEY_ENUM;
}
/* UTF-8 belongs to the complete shape check before this scalar type query. */
static inline bool xir_constant_key_type(const XrXirTypes *types,XrXirType type,XrXirConstantKey key) {
    if(!xir_constant_key_header(key))return false;
    if(key.kind==XR_XIR_KEY_DYNAMIC)return true;
    if(key.kind==XR_XIR_KEY_STRING)return type==XR_XIR_STRING;
    if(key.kind==XR_XIR_KEY_ENUM) {
        const XrXirTypeNode *node=xr_xir_type_node(types,type);
        if(!node || node->kind!=XR_XIR_TYPE_NOMINAL || !types->nominals || node->nominal.declaration>=types->nominals->count)return false;
        const XrXirNominalTable *table=types->nominals;uint32_t kind,count;const XrXirNominalVariant *variants;
        if(table->declarations) {
            XrXirNominalDeclaration d=table->declarations[node->nominal.declaration];kind=d.kind;count=d.variant_count;variants=d.variants;
        } else if(table->identities) {
            XrXirNominalIdentity d=table->identities[node->nominal.declaration];kind=d.kind;count=d.variant_count;variants=d.variants;
        } else return false;
        return kind==XR_XIR_NOMINAL_ENUM && key.bits<count && variants && !variants[key.bits].field_count;
    }
    if(xr_xir_type_is_integer(type))return xr_xir_integer_payload_valid(type,(int64_t)key.bits);
    if(type==XR_XIR_BOOL)return key.bits<=1;
    if(type==XR_XIR_RUNE)return key.bits<=0x10ffff && !(key.bits>=0xd800 && key.bits<=0xdfff);
    if(type==XR_XIR_F32)return key.bits<=UINT32_MAX;
    return type==XR_XIR_F64;
}
typedef struct XirConstantKeyRead {XrCompileRequestControl *control;XrXirStatus status;uint32_t bytes;} XirConstantKeyRead;
static inline int xir_constant_key_read(void *opaque,const uint8_t *address,uint8_t *output) {
    XirConstantKeyRead *read=opaque;
    if(read->bytes==4096) {
        read->status=xir_receive_poll(read->control);if(read->status!=XR_XIR_OK)return 0;read->bytes=0;
    }
    *output=*address;++read->bytes;return 1;
}
static inline XrXirStatus xir_constant_key_utf8(XrXirConstantKey key,XrCompileRequestControl *control) {
    XirConstantKeyRead read={control,xir_receive_poll(control),0};if(read.status!=XR_XIR_OK)return read.status;
    if(!xir_constant_key_header(key))return XR_XIR_BAD_STRUCTURE;
    if(key.kind==XR_XIR_KEY_STRING) {
        XrUtf8ScanResult scan;
        if(!xr_utf8_core_scan_strict_read((const uint8_t *)key.string.bytes,key.string.length,xir_constant_key_read,&read,&scan))return read.status;
        if(scan.error!=XR_UTF8_OK)return XR_XIR_BAD_STRUCTURE;
    }
    return xir_receive_poll(control);
}
static inline XrXirStatus xir_constant_key_check(const XrXirTypes *types,XrXirType type,XrXirConstantKey key,XrCompileRequestControl *control) {
    XrXirStatus status=xir_constant_key_utf8(key,control);if(status!=XR_XIR_OK)return status;
    return xir_constant_key_type(types,type,key)?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
}
#endif
