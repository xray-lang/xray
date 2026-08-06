/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_json_type.h - Runtime-neutral Json.decode field type contract
 */

#ifndef XR_JSON_TYPE_H
#define XR_JSON_TYPE_H

#include <stdbool.h>
#include <stdint.h>

typedef enum XrJsonValueKind {
    XR_JSON_VALUE_ANY = 0,
    XR_JSON_VALUE_NULL = 1,
    XR_JSON_VALUE_BOOL = 2,
    XR_JSON_VALUE_INT = 3,
    XR_JSON_VALUE_FLOAT = 4,
    XR_JSON_VALUE_STRING = 5,
    XR_JSON_VALUE_JSON = 6,
    XR_JSON_VALUE_STRUCT_OBJECT = 7,
    XR_JSON_VALUE_ARRAY = 8,
    XR_JSON_VALUE_MAP = 9,
    XR_JSON_VALUE_ENUM = 10,
    XR_JSON_VALUE_CLASS_INSTANCE = 11,
} XrJsonValueKind;

enum {
    XR_JSON_VALUE_KIND_MASK = 0x7fu,
    XR_JSON_VALUE_NULLABLE = 0x80u,
};

static inline uint8_t xr_json_value_kind_base(uint8_t encoded) {
    return encoded & XR_JSON_VALUE_KIND_MASK;
}

static inline bool xr_json_value_kind_is_nullable(uint8_t encoded) {
    return (encoded & XR_JSON_VALUE_NULLABLE) != 0;
}

/* VM/bytecode recursive construction contract. Object/class/enum nodes use
 * target_descriptor; Array/Map nodes use child plus storage_type. */
typedef struct XrJsonDecodeSchema {
    uint8_t value_kind;
    uint8_t storage_type;
    uint16_t reserved;
    const void *target_descriptor;
    const struct XrJsonDecodeSchema *child;
} XrJsonDecodeSchema;

typedef struct XrJsonDecodeFieldSpec {
    const char *name;
    uint8_t value_kind;
    const struct XrJsonDecodeFieldSpec *nested_fields;
    uint16_t nested_field_count;
    const void *target_shape;
} XrJsonDecodeFieldSpec;

/* Runtime-neutral descriptor for the Json string representation of a
 * payload-free enum. Generated code owns every referenced string/table for
 * process lifetime; VM bytecode rebuilds the equivalent immutable enum type. */
typedef struct XrJsonEnumDecodeSpec {
    uint32_t layout_id;
    uint16_t member_count;
    uint16_t reserved;
    const char *enum_name;
    const char *const *member_names;
    /* Backend-owned immutable descriptor used when a decoded ordinal crosses
     * a tagged boundary. AOT points at XrAotEnumScalarLayout; the VM rebuilds
     * the equivalent nominal enum identity from bytecode metadata. */
    const void *tagged_layout;
} XrJsonEnumDecodeSpec;

typedef enum XrJsonNominalDecodeTargetKind {
    XR_JSON_NOMINAL_TARGET_CLASS = 1,
    XR_JSON_NOMINAL_TARGET_VALUE_STRUCT = 2,
} XrJsonNominalDecodeTargetKind;

/* AOT-side construction contract for a @derive(Json) nominal target. type_id
 * is the registered destructor/storage-promotion identity for both reference
 * classes and heap-materialized value structs. target_kind selects whether a
 * parsed value boxes as XR_TINSTANCE or XR_TAG_AGG_REF; value structs may also
 * decode directly into caller-provided native storage. The VM carries the same
 * field schemas on XrClass and resolves the class name stored in the root
 * schema. Generated C owns these immutable tables for process lifetime. */
typedef struct XrJsonClassDecodeFieldSpec {
    const char *name;
    uint32_t offset;
    uint8_t native_type;
    uint8_t reserved[3];
    XrJsonDecodeFieldSpec value;
} XrJsonClassDecodeFieldSpec;

typedef struct XrJsonClassDecodeSpec {
    uint16_t type_id;
    uint16_t field_count;
    uint32_t instance_size;
    uint8_t target_kind;
    uint8_t reserved[3];
    const XrJsonClassDecodeFieldSpec *fields;
} XrJsonClassDecodeSpec;

#endif
