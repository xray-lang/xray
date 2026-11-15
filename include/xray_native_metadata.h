/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * Exact metadata admission for the installed synchronous native runtime. */
#ifndef XRAY_NATIVE_METADATA_H
#define XRAY_NATIVE_METADATA_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define XRT_NATIVE_METADATA_MAGIC UINT64_C(0x58524d3158524d31)
#define XRT_NATIVE_METADATA_SCHEMA UINT32_C(1)
enum { XRT_NATIVE_METADATA_CALLABLE=1, XRT_NATIVE_METADATA_BOXED=2, XRT_NATIVE_METADATA_MODULE=3 };
typedef struct XrtNativeMetadataHeader {
    uint64_t magic;
    uint32_t schema, kind, descriptor_size, type_size, parameter_size, entry_size;
    uint32_t prefix_size, reserved;
} XrtNativeMetadataHeader;
/* Each public descriptor starts with this exact prefix. The first eight bytes
 * are read independently before any other descriptor byte or row. Both halves
 * of the magic are odd; pre-cutover descriptors start with a naturally aligned
 * typed-array pointer (or NULL), so they cannot match on 32/64-bit targets in
 * either byte order. A readable matching magic commits the caller to readable
 * complete descriptor storage. This is format admission, not pointer authority.
 * There is one current schema; zero, older schemas and different row strides
 * are refused rather than inferred from metadata contents. */
static inline bool xrt_native_metadata_matches(const void *descriptor,
    const XrtNativeMetadataHeader *expected) {
    uint64_t magic=0;
    if(!descriptor)return false;
    memcpy(&magic,descriptor,sizeof(magic));
    if(magic!=XRT_NATIVE_METADATA_MAGIC)return false;
    XrtNativeMetadataHeader actual;
    memcpy(&actual,descriptor,sizeof(actual));
    return actual.schema==expected->schema&&actual.kind==expected->kind&&
        actual.descriptor_size==expected->descriptor_size&&actual.type_size==expected->type_size&&
        actual.parameter_size==expected->parameter_size&&actual.entry_size==expected->entry_size&&
        actual.prefix_size==sizeof(actual)&&actual.reserved==0;
}
#endif
