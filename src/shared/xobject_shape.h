/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xobject_shape.h - Runtime-neutral object shape descriptor ABI
 */

#ifndef XOBJECT_SHAPE_H
#define XOBJECT_SHAPE_H

#include "../base/xhash.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
    XR_OBJECT_DOMAIN_JSON = 0,
    XR_OBJECT_DOMAIN_STRUCT = 1,
};

enum {
    XR_OBJECT_SHAPE_STATIC = 0,
    XR_OBJECT_SHAPE_OWNED = 1,
};

enum {
    XR_OBJECT_SHAPE_FIELD_READONLY = 1u << 0,
    XR_OBJECT_SHAPE_FIELD_OPTIONAL = 1u << 1,
};

typedef struct XrtObjectShapeField {
    const char *name;
    uint64_t stable_type_key;
    uint32_t symbol_hash;
    uint16_t ordinal;
    uint8_t flags;
    uint8_t reserved;
} XrtObjectShapeField;

typedef struct XrtObjectShape {
    uint64_t stable_key;
    int64_t field_count;
    const XrtObjectShapeField *fields;
    uint8_t object_domain;
    uint8_t storage;
    uint16_t reserved16;
    uint32_t reserved32;
} XrtObjectShape;

/* The key is content-derived and portable across translation units. Pointer
 * identity is deliberately absent from the contract. Callers must still
 * compare the descriptor table after a key match to defend against collisions. */
static inline uint64_t xr_object_shape_key_fold(uint64_t key, const void *data, size_t size) {
    uint64_t part = xr_hash_bytes64(data, size);
    key ^= part + UINT64_C(0x9e3779b97f4a7c15) + (key << 6) + (key >> 2);
    return key ? key : 1;
}

static inline uint64_t xr_object_shape_key_fold_u64(uint64_t key, uint64_t value) {
    uint8_t bytes[8];
    for (uint8_t i = 0; i < 8; i++)
        bytes[i] = (uint8_t) (value >> (i * 8));
    return xr_object_shape_key_fold(key, bytes, sizeof(bytes));
}

static inline uint64_t xr_object_shape_key_begin(uint8_t domain, int64_t field_count) {
    uint64_t key = xr_object_shape_key_fold(XR_FNV64_OFFSET_BASIS, &domain, sizeof(domain));
    return xr_object_shape_key_fold_u64(key, (uint64_t) field_count);
}

static inline uint64_t xr_object_shape_key_add_field(uint64_t key, const char *name,
                                                     uint64_t stable_type_key, uint8_t flags) {
    const char *field_name = name ? name : "?";
    key = xr_object_shape_key_fold(key, field_name, strlen(field_name));
    key = xr_object_shape_key_fold_u64(key, stable_type_key);
    return xr_object_shape_key_fold(key, &flags, sizeof(flags));
}

static inline uint64_t xr_object_shape_stable_key(uint8_t domain,
                                                  const XrtObjectShapeField *fields,
                                                  int64_t field_count) {
    uint64_t key = xr_object_shape_key_begin(domain, field_count);
    for (int64_t i = 0; fields && i < field_count; i++)
        key = xr_object_shape_key_add_field(key, fields[i].name, fields[i].stable_type_key,
                                            fields[i].flags);
    return key;
}

#endif /* XOBJECT_SHAPE_H */
