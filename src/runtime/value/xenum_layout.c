/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xenum_layout.c - Compile-time enum tagged aggregate layout descriptor
 */

#include "xenum_layout.h"
#include "xvalue.h"
#include "../../base/xmalloc.h"
#include "../../shared/xr_hash_core.h"

#include <stdalign.h>
#include <string.h>

#define XR_ENUM_NOMINAL_HASH_SEED UINT64_C(1469598103934665603)
#define XR_ENUM_NOMINAL_HASH_PRIME UINT64_C(1099511628211)

static uint64_t enum_nominal_hash_byte(uint64_t hash, uint8_t value) {
    return (hash ^ value) * XR_ENUM_NOMINAL_HASH_PRIME;
}

static uint64_t enum_nominal_hash_u32(uint64_t hash, uint32_t value) {
    hash = enum_nominal_hash_byte(hash, (uint8_t) (value >> 24));
    hash = enum_nominal_hash_byte(hash, (uint8_t) (value >> 16));
    hash = enum_nominal_hash_byte(hash, (uint8_t) (value >> 8));
    return enum_nominal_hash_byte(hash, (uint8_t) value);
}

static uint64_t enum_nominal_hash_string(uint64_t hash, const char *value) {
    size_t length = value ? strlen(value) : 0;
    hash = enum_nominal_hash_u32(hash, (uint32_t) length);
    for (size_t i = 0; i < length; i++)
        hash = enum_nominal_hash_byte(hash, (uint8_t) value[i]);
    return hash;
}

uint32_t xr_enum_layout_nominal_id(const XrEnumLayout *layout) {
    if (!layout || !layout->nominal_owner || !layout->nominal_owner[0] || !layout->name ||
        !layout->name[0] || !layout->variants || layout->variant_count == 0)
        return 0;

    uint64_t hash = XR_ENUM_NOMINAL_HASH_SEED;
    hash = enum_nominal_hash_string(hash, "xray.enum.nominal.v1");
    hash = enum_nominal_hash_string(hash, layout->nominal_owner);
    hash = enum_nominal_hash_string(hash, layout->name);
    hash = enum_nominal_hash_u32(hash, layout->variant_count);
    for (uint32_t i = 0; i < layout->variant_count; i++) {
        hash = enum_nominal_hash_string(hash, layout->variants[i].name);
        hash = enum_nominal_hash_u32(hash, layout->variants[i].payload_count);
    }
    hash = xr_hash_core_mix_u64(hash);
    return ((uint32_t) hash) | UINT32_C(0x80000000);
}

static void enum_layout_refresh_nominal_id(XrEnumLayout *layout) {
    if (layout)
        layout->layout_id = xr_enum_layout_nominal_id(layout);
}

static uint32_t enum_align_up(uint32_t value, uint32_t align) {
    if (align <= 1)
        return value;
    return (value + align - 1u) & ~(align - 1u);
}

static uint8_t enum_tag_size(uint32_t variant_count) {
    if (variant_count <= 256u)
        return 1;
    if (variant_count <= 65536u)
        return 2;
    return 4;
}

static void enum_layout_recompute(XrEnumLayout *layout) {
    if (!layout)
        return;

    uint16_t max_payload = 0;
    for (uint32_t i = 0; i < layout->variant_count; i++) {
        if (layout->variants[i].payload_count > max_payload)
            max_payload = layout->variants[i].payload_count;
    }

    layout->tag_size = enum_tag_size(layout->variant_count);
    layout->is_zero_payload = max_payload == 0;

    uint32_t payload_slot_size = (uint32_t) sizeof(XrValue);
    uint32_t payload_slot_align = (uint32_t) alignof(XrValue);
    uint32_t tag_align = layout->tag_size;
    uint32_t enum_align = tag_align;
    if (!layout->is_zero_payload && payload_slot_align > enum_align)
        enum_align = payload_slot_align;

    layout->align = (uint16_t) enum_align;
    layout->contains_refs = !layout->is_zero_payload;
    layout->payload_offset =
        layout->is_zero_payload ? 0 : enum_align_up(layout->tag_size, enum_align);
    layout->payload_size = (uint32_t) max_payload * payload_slot_size;
    layout->size = layout->is_zero_payload
                       ? enum_align_up(layout->tag_size, enum_align)
                       : enum_align_up(layout->payload_offset + layout->payload_size, enum_align);

    for (uint32_t i = 0; i < layout->variant_count; i++) {
        XrEnumVariantLayout *variant = &layout->variants[i];
        variant->payload_size = (uint32_t) variant->payload_count * payload_slot_size;
        variant->payload_align = variant->payload_count > 0 ? (uint16_t) payload_slot_align : 1;
        variant->contains_refs = variant->payload_count > 0;
    }
}

XrEnumLayout *xr_enum_layout_new(const char *nominal_owner, const char *name,
                                 const char *const *variant_names, uint32_t variant_count) {
    if (!nominal_owner || !nominal_owner[0] || !name || !variant_names || variant_count == 0)
        return NULL;

    XrEnumLayout *layout = (XrEnumLayout *) xr_calloc(1, sizeof(*layout));
    if (!layout)
        return NULL;

    layout->variants =
        (XrEnumVariantLayout *) xr_calloc((size_t) variant_count, sizeof(*layout->variants));
    if (!layout->variants) {
        xr_free(layout);
        return NULL;
    }

    layout->nominal_owner = xr_strdup(nominal_owner);
    if (!layout->nominal_owner) {
        xr_free(layout->variants);
        xr_free(layout);
        return NULL;
    }
    layout->name = name;
    layout->variant_count = variant_count;
    for (uint32_t i = 0; i < variant_count; i++) {
        layout->variants[i].name = variant_names[i];
        layout->variants[i].symbol = -1;
        layout->variants[i].tag = i;
    }

    enum_layout_recompute(layout);
    enum_layout_refresh_nominal_id(layout);
    return layout;
}

bool xr_enum_layout_set_payload_counts(XrEnumLayout *layout, const int *payload_counts,
                                       uint32_t count) {
    if (!layout || !payload_counts || count != layout->variant_count)
        return false;
    for (uint32_t i = 0; i < count; i++) {
        if (payload_counts[i] < 0)
            return false;
    }
    for (uint32_t i = 0; i < count; i++)
        layout->variants[i].payload_count = (uint16_t) payload_counts[i];
    enum_layout_recompute(layout);
    enum_layout_refresh_nominal_id(layout);
    return true;
}

bool xr_enum_layout_set_variant_payload_metadata(XrEnumLayout *layout, uint32_t tag,
                                                 const char *const *payload_names,
                                                 const uint8_t *payload_type_ids,
                                                 uint16_t payload_count) {
    if (!layout || tag >= layout->variant_count)
        return false;
    XrEnumVariantLayout *variant = &layout->variants[tag];
    if (variant->payload_count != payload_count)
        return false;
    if (variant->payload_names) {
        for (uint16_t i = 0; i < variant->payload_count; i++)
            xr_free((void *) variant->payload_names[i]);
        xr_free(variant->payload_names);
        variant->payload_names = NULL;
    }
    if (variant->payload_type_ids) {
        xr_free(variant->payload_type_ids);
        variant->payload_type_ids = NULL;
    }
    if (payload_count == 0)
        return true;

    variant->payload_names =
        (const char **) xr_calloc((size_t) payload_count, sizeof(*variant->payload_names));
    variant->payload_type_ids =
        (uint8_t *) xr_calloc((size_t) payload_count, sizeof(*variant->payload_type_ids));
    if (!variant->payload_names || !variant->payload_type_ids)
        return false;
    for (uint16_t i = 0; i < payload_count; i++) {
        const char *name = payload_names && payload_names[i] ? payload_names[i] : "";
        variant->payload_names[i] = xr_strdup(name);
        variant->payload_type_ids[i] = payload_type_ids ? payload_type_ids[i] : 0;
        if (!variant->payload_names[i])
            return false;
    }
    return true;
}

void xr_enum_layout_set_variant_symbol(XrEnumLayout *layout, uint32_t tag, int symbol) {
    if (!layout || tag >= layout->variant_count)
        return;
    layout->variants[tag].symbol = symbol;
}

const XrEnumVariantLayout *xr_enum_layout_variant(const XrEnumLayout *layout, uint32_t tag) {
    if (!layout || tag >= layout->variant_count)
        return NULL;
    return &layout->variants[tag];
}

int xr_enum_layout_payload_count(const XrEnumLayout *layout, uint32_t tag) {
    const XrEnumVariantLayout *variant = xr_enum_layout_variant(layout, tag);
    return variant ? (int) variant->payload_count : 0;
}

uint16_t xr_enum_layout_max_payload(const XrEnumLayout *layout) {
    if (!layout)
        return 0;
    uint16_t max_payload = 0;
    for (uint32_t i = 0; i < layout->variant_count; i++) {
        if (layout->variants[i].payload_count > max_payload)
            max_payload = layout->variants[i].payload_count;
    }
    return max_payload;
}

void xr_enum_layout_free(XrEnumLayout *layout) {
    if (!layout)
        return;
    if (layout->variants) {
        for (uint32_t i = 0; i < layout->variant_count; i++) {
            XrEnumVariantLayout *variant = &layout->variants[i];
            if (variant->payload_names) {
                for (uint16_t p = 0; p < variant->payload_count; p++)
                    xr_free((void *) variant->payload_names[p]);
                xr_free(variant->payload_names);
            }
            if (variant->payload_type_ids)
                xr_free(variant->payload_type_ids);
        }
        xr_free(layout->variants);
    }
    xr_free((void *) layout->nominal_owner);
    xr_free(layout);
}
