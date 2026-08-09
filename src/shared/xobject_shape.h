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
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
    XR_OBJECT_DOMAIN_STRUCT = 0,
};

enum {
    XR_OBJECT_SHAPE_STATIC = 0,
    XR_OBJECT_SHAPE_OWNED = 1,
};

enum {
    XR_OBJECT_SHAPE_FIELD_READONLY = 1u << 0,
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

static inline uint64_t xr_object_shape_stable_name_key(const char *name) {
    const char *field_name = name ? name : "?";
    uint64_t key = xr_hash_bytes64(field_name, strlen(field_name));
    return key ? key : 1;
}

static inline uint32_t xr_object_shape_symbol_hash(const char *name) {
    const char *field_name = name ? name : "?";
    uint32_t hash = xr_hash_bytes(field_name, strlen(field_name));
    return hash ? hash : 1;
}

static inline int xr_object_shape_name_compare(const char *left, const char *right) {
    uint64_t left_key = xr_object_shape_stable_name_key(left);
    uint64_t right_key = xr_object_shape_stable_name_key(right);
    if (left_key < right_key)
        return -1;
    if (left_key > right_key)
        return 1;

    uint32_t left_hash = xr_object_shape_symbol_hash(left);
    uint32_t right_hash = xr_object_shape_symbol_hash(right);
    if (left_hash < right_hash)
        return -1;
    if (left_hash > right_hash)
        return 1;
    return 0;
}

/* Canonical field order is part of the VM/AOT object-shape contract. Keep the
 * implementation here so every producer, including native stdlib objects,
 * derives identical ordinals without depending on compiler-only analysis. */
static inline void xr_object_shape_sort_names(const char **names, int64_t count) {
    if (!names || count <= 1)
        return;
    for (int64_t i = 1; i < count; i++) {
        const char *current = names[i];
        int64_t j = i;
        while (j > 0 && xr_object_shape_name_compare(names[j - 1], current) > 0) {
            names[j] = names[j - 1];
            j--;
        }
        names[j] = current;
    }
}

static inline int64_t xr_object_shape_canonical_ordinal(const char *const *names, int64_t count,
                                                        const char *name) {
    if (!names || count < 0 || !name)
        return -1;
    int64_t ordinal = 0;
    bool found = false;
    for (int64_t i = 0; i < count; i++) {
        if (names[i] && strcmp(names[i], name) == 0)
            found = true;
        if (xr_object_shape_name_compare(names[i], name) < 0)
            ordinal++;
    }
    return found ? ordinal : -1;
}

static inline uint64_t xr_object_shape_key_add_field_key(uint64_t key, uint64_t stable_name_key,
                                                         uint64_t stable_type_key, uint8_t flags) {
    key = xr_object_shape_key_fold_u64(key, stable_name_key);
    key = xr_object_shape_key_fold_u64(key, stable_type_key);
    return xr_object_shape_key_fold(key, &flags, sizeof(flags));
}

static inline uint64_t xr_object_shape_key_add_field(uint64_t key, const char *name,
                                                     uint64_t stable_type_key, uint8_t flags) {
    return xr_object_shape_key_add_field_key(key, xr_object_shape_stable_name_key(name),
                                             stable_type_key, flags);
}

static inline uint64_t xr_object_shape_stable_key(uint8_t domain, const XrtObjectShapeField *fields,
                                                  int64_t field_count) {
    uint64_t key = xr_object_shape_key_begin(domain, field_count);
    for (int64_t i = 0; fields && i < field_count; i++)
        key = xr_object_shape_key_add_field(key, fields[i].name, fields[i].stable_type_key,
                                            fields[i].flags);
    return key;
}

#endif /* XOBJECT_SHAPE_H */
