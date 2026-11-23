/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Licensed under the MIT License
 *
 * xlir_error_metadata.inc.c - Owned diagnostic metadata for a nominal error sum
 */
#include "shared/xr_value_format_core.h"

typedef struct LirErrorMetadata {
    uint8_t *output;
    const uint8_t *expected;
    size_t count, capacity;
} LirErrorMetadata;

static bool lir_error_bytes(LirErrorMetadata *metadata,const void *data,size_t length) {
    if (length > metadata->capacity - metadata->count) return false;
    if (metadata->expected && memcmp(metadata->expected + metadata->count,data,length)) return false;
    if (metadata->output) memcpy(metadata->output + metadata->count,data,length);
    metadata->count+=length;
    return true;
}

static bool lir_error_u32(LirErrorMetadata *metadata,uint32_t value) {
    uint8_t raw[4] = {(uint8_t)value,(uint8_t)(value>>8),(uint8_t)(value>>16),(uint8_t)(value>>24)};
    return lir_error_bytes(metadata,raw,sizeof(raw));
}

static bool lir_error_name(LirErrorMetadata *metadata,const char *text,uint32_t length) {
    return lir_error_u32(metadata,length) && lir_error_bytes(metadata,text,length);
}

static bool lir_error_metadata(const XrLirModule *m,XrLirType type,LirErrorMetadata *metadata) {
    XrLirRecordIdentity key={0};
    XrLirTypeLayout layout={0};
    const XrLirRecordField *fields=NULL;
    const XrLirSumVariant *variants=NULL;
    uint32_t field_count=0,variant_count=0;
    if (!xr_lir_error_sum_identity(m,type,&key) || xr_lir_type_layout(m,type,&layout)!=XR_LIR_OK ||
        !xr_lir_sum_variants(m,type,&fields,&field_count,&variants,&variant_count)) return false;
    uint32_t tag=xr_lir_sum_tag_size(variant_count);
    if (!lir_error_u32(metadata,XR_ERROR_METADATA_VERSION) || !lir_error_u32(metadata,type) ||
        !lir_error_u32(metadata,(uint32_t)layout.size) || !lir_error_u32(metadata,tag) ||
        !lir_error_u32(metadata,variant_count) || !lir_error_u32(metadata,key.argument_count) ||
        !lir_error_name(metadata,key.module,key.module_length) || !lir_error_name(metadata,key.name,key.name_length))
        return false;
    for (uint32_t a=0;a<key.argument_count;++a)
        if (!lir_error_u32(metadata,key.arguments[a].type) || !lir_error_u32(metadata,key.arguments[a].array_element))
            return false;
    uint32_t payload=(tag+layout.alignment-1u)&~(layout.alignment-1u);
    for (uint32_t v=0;v<variant_count;++v) {
        XrLirSumVariant variant=variants[v];
        if (!lir_error_name(metadata,variant.name,variant.name_length) || !lir_error_u32(metadata,variant.field_count))
            return false;
        uint32_t offset=payload;
        for (uint32_t f=0;f<variant.field_count;++f) {
            XrLirRecordField field=fields[variant.field_begin+f];
            XrLirTypeLayout child;
            if (xr_lir_type_layout(m,field.type,&child)!=XR_LIR_OK) return false;
            offset=(offset+child.alignment-1u)&~(child.alignment-1u);
            if (!lir_error_u32(metadata,lir_pod_scalar(field.type)) || !lir_error_u32(metadata,offset) ||
                !lir_error_name(metadata,field.name,field.name_length)) return false;
            offset+=(uint32_t)child.size;
        }
    }
    return true;
}

XrLirStatus xr_lir_error_literal(XrLirModule *m,XrLirType type,XrLirLiteral *output) {
    LirErrorMetadata measure={.capacity=SIZE_MAX};
    if (!lir_error_metadata(m,type,&measure)) return XR_LIR_INVALID_ARGUMENT;
    uint8_t *bytes=lir_allocate(m,measure.count,1);
    if (!bytes) return m->failure;
    LirErrorMetadata write={.output=bytes,.capacity=measure.count};
    bool good=lir_error_metadata(m,type,&write) && write.count==measure.count;
    XrLirStatus status=good ? xr_lir_add_literal(m,bytes,write.count,output) : XR_LIR_VERIFY_ERROR;
    xr_free(bytes);
    return status;
}

bool xr_lir_error_literal_matches(const XrLirModule *m,XrLirType type,uint32_t literal) {
    if (!literal || literal>m->literal_count) return false;
    const XrLirLiteralData *data=&m->literals[literal-1];
    LirErrorMetadata read={.expected=data->bytes,.capacity=data->length};
    return lir_error_metadata(m,type,&read) && read.count==data->length;
}
