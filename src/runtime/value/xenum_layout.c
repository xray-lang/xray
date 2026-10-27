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
#include "../../base/xio_policy.inc.h"
#include <stddef.h>
#include "../../shared/xr_hash_core.h"

#include <stdalign.h>
#include <string.h>

#define XR_ENUM_NOMINAL_HASH_SEED UINT64_C(1469598103934665603)
#define XR_ENUM_NOMINAL_HASH_PRIME UINT64_C(1099511628211)

typedef struct EnumLayoutOwner {
    XrOsIoPolicy policy;
    uint32_t count;
    uint16_t *metadata_counts;
    XrEnumVariantLayout *variants;
    XrEnumLayout layout;
} EnumLayoutOwner;

static EnumLayoutOwner *layout_owner(XrEnumLayout *layout) {
    return (EnumLayoutOwner *) ((char *) layout - offsetof(EnumLayoutOwner, layout));
}
static bool layout_shape(const XrEnumLayout *layout, const EnumLayoutOwner *owner) {
    return layout->variant_count == owner->count && layout->variants == owner->variants;
}
static bool enum_work(XrIoContext *io, uint64_t units) {
    return !io || ((io)->status == XR_OS_IO_OK);
}
static char *enum_copy_text(XrIoContext *io, const char *text) {
    size_t length;
    if (!io_length(io, text, &length))
        return NULL;
    char *copy = io_alloc(io, length + 1);
    if (copy && !io_copy(io, copy, text, length + 1)) {
        io_free(io, copy);
        copy = NULL;
    }
    return copy;
}
static void enum_free_payload(const XrOsIoPolicy *policy, XrEnumVariantLayout *variant,
                              uint16_t count) {
    if (variant->payload_names) {
        for (uint16_t i = 0; i < count; ++i)
            policy->free(policy->context, (void *) variant->payload_names[i]);
        policy->free(policy->context, (void *) variant->payload_names);
    }
    if (variant->payload_type_ids)
        policy->free(policy->context, variant->payload_type_ids);
    variant->payload_names = NULL;
    variant->payload_type_ids = NULL;
}

static uint64_t enum_nominal_hash_byte(uint64_t hash, uint8_t value) {
    return (hash ^ value) * XR_ENUM_NOMINAL_HASH_PRIME;
}

static uint64_t enum_nominal_hash_u32(uint64_t hash, uint32_t value) {
    hash = enum_nominal_hash_byte(hash, (uint8_t) (value >> 24));
    hash = enum_nominal_hash_byte(hash, (uint8_t) (value >> 16));
    hash = enum_nominal_hash_byte(hash, (uint8_t) (value >> 8));
    return enum_nominal_hash_byte(hash, (uint8_t) value);
}

static uint64_t enum_nominal_hash_string(XrIoContext *io, uint64_t hash, const char *value) {
    size_t length = 0;
    if (value && io) {
        if (!io_length(io, value, &length))
            return 0;
        if (length > UINT32_MAX) {
            (void) io_status(io, XR_OS_IO_OVERFLOW);
            return 0;
        }
    } else if (value)
        length = strlen(value);
    hash = enum_nominal_hash_u32(hash, (uint32_t) length);
    for (size_t i = 0; i < length; i++) {
        if (!enum_work(io, 1))
            return 0;
        hash = enum_nominal_hash_byte(hash, (uint8_t) value[i]);
    }
    return hash;
}

static uint32_t enum_nominal_id(XrIoContext *io, const XrEnumLayout *layout) {
    if (!layout || !layout->nominal_owner || !layout->nominal_owner[0] || !layout->name ||
        !layout->name[0] || !layout->variants || !layout->variant_count)
        return 0;
    uint64_t hash = XR_ENUM_NOMINAL_HASH_SEED;
    hash = enum_nominal_hash_string(io, hash, "xray.enum.nominal.v1");
    if (io && io->status != XR_OS_IO_OK)
        return 0;
    hash = enum_nominal_hash_string(io, hash, layout->nominal_owner);
    if (io && io->status != XR_OS_IO_OK)
        return 0;
    hash = enum_nominal_hash_string(io, hash, layout->name);
    if (io && io->status != XR_OS_IO_OK)
        return 0;
    hash = enum_nominal_hash_u32(hash, layout->variant_count);
    for (uint32_t i = 0; i < layout->variant_count; i++) {
        if (!enum_work(io, 1))
            return 0;
        hash = enum_nominal_hash_string(io, hash, layout->variants[i].name);
        if (io && io->status != XR_OS_IO_OK)
            return 0;
        hash = enum_nominal_hash_u32(hash, layout->variants[i].payload_count);
    }
    hash = xr_hash_core_mix_u64(hash);
    return ((uint32_t) hash) | UINT32_C(0x80000000);
}
XR_FUNC uint32_t xr_enum_layout_nominal_id(const XrEnumLayout *layout) {
    return enum_nominal_id(NULL, layout);
}

static bool enum_names_exact(XrIoContext *io, const char *const *names, uint16_t count) {
    if (count && !names)
        return false;
    for (uint16_t i = 0; i < count; ++i) {
        if (!enum_work(io, 1) || !names[i])
            return false;
        if (!enum_work(io, 1) || !names[i][0])
            return false;
        for (uint16_t prior = 0; prior < i; ++prior) {
            size_t n = 0;
            for (;;) {
                if (!enum_work(io, 2))
                    return false;
                char a = names[i][n], b = names[prior][n];
                if (a != b)
                    break;
                if (!a)
                    return false;
                ++n;
            }
        }
    }
    return true;
}

XR_FUNC bool xr_enum_payload_names_are_exact(const char *const *names, uint16_t count) {
    return enum_names_exact(NULL, names, count);
}

XR_FUNC bool xr_enum_variant_payload_metadata_is_exact(const XrEnumVariantLayout *variant) {
    if (!variant)
        return false;
    if (variant->payload_count == 0)
        return variant->payload_names == NULL && variant->payload_type_ids == NULL;
    if (!xr_enum_payload_names_are_exact(variant->payload_names, variant->payload_count) ||
        !variant->payload_type_ids)
        return false;
    for (uint16_t i = 0; i < variant->payload_count; i++) {
        if (variant->payload_type_ids[i] >= XR_TID_COUNT)
            return false;
    }
    return true;
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

static bool enum_layout_recompute(XrIoContext *io, XrEnumLayout *layout) {
    uint16_t max_payload = 0;
    for (uint32_t i = 0; i < layout->variant_count; i++) {
        if (!((io)->status == XR_OS_IO_OK))
            return false;
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
        if (!((io)->status == XR_OS_IO_OK))
            return false;
        XrEnumVariantLayout *variant = &layout->variants[i];
        variant->payload_size = (uint32_t) variant->payload_count * payload_slot_size;
        variant->payload_align = variant->payload_count > 0 ? (uint16_t) payload_slot_align : 1;
        variant->contains_refs = variant->payload_count > 0;
    }
    return true;
}

XR_FUNC XrOsIoStatus xr_enum_layout_new(const XrOsIoPolicy *policy, const char *nominal_owner,
                                        const char *name, const char *const *variant_names,
                                        uint32_t count, XrEnumLayout **output) {
    if (!io_policy_valid(policy) || !output || *output || !nominal_owner || !name ||
        !variant_names || !count)
        return XR_OS_IO_BAD_ARGUMENT;
    if ((size_t) count > SIZE_MAX / sizeof(XrEnumVariantLayout))
        return XR_OS_IO_OVERFLOW;
    XrIoContext io = {policy, XR_OS_IO_OK};
    EnumLayoutOwner *owner = io_alloc(&io, sizeof(*owner));
    if (!owner)
        return io.status;
    if (!io_clear(&io, owner, sizeof(*owner))) {
        io_free(&io, owner);
        return io.status;
    }
    owner->policy = *policy;
    owner->count = count;
    XrEnumLayout *layout = &owner->layout;
    owner->metadata_counts = io_alloc(&io, (size_t) count * sizeof(uint16_t));
    if (owner->metadata_counts &&
        !io_clear(&io, owner->metadata_counts, (size_t) count * sizeof(uint16_t))) {
        io_free(&io, owner->metadata_counts);
        owner->metadata_counts = NULL;
    }
    owner->variants = io_alloc(&io, (size_t) count * sizeof(*owner->variants));
    if (owner->variants &&
        !io_clear(&io, owner->variants, (size_t) count * sizeof(*owner->variants))) {
        io_free(&io, owner->variants);
        owner->variants = NULL;
    }
    layout->variants = owner->variants;
    layout->variant_count = count;
    if (io.status == XR_OS_IO_OK)
        layout->nominal_owner = enum_copy_text(&io, nominal_owner);
    if (io.status == XR_OS_IO_OK)
        layout->name = enum_copy_text(&io, name);
    for (uint32_t i = 0; i < count && io.status == XR_OS_IO_OK; ++i) {
        if (!((&io)->status == XR_OS_IO_OK))
            break;
        layout->variants[i].name = enum_copy_text(&io, variant_names[i]);
        if (io.status != XR_OS_IO_OK)
            break;
        layout->variants[i].symbol = -1;
        layout->variants[i].tag = i;
    }
    if (io.status == XR_OS_IO_OK && enum_layout_recompute(&io, layout))
        layout->layout_id = enum_nominal_id(&io, layout);
    if (io.status == XR_OS_IO_OK)
        (void) ((&io)->status == XR_OS_IO_OK);
    if (io.status != XR_OS_IO_OK) {
        xr_enum_layout_free(layout);
        return io.status;
    }
    *output = layout;
    return XR_OS_IO_OK;
}

XR_FUNC XrOsIoStatus xr_enum_layout_set_payload_counts(XrEnumLayout *layout, const int *counts,
                                                       uint32_t count) {
    if (!layout || !counts)
        return XR_OS_IO_BAD_ARGUMENT;
    EnumLayoutOwner *owner = layout_owner(layout);
    if (!layout_shape(layout, owner) || count != owner->count)
        return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {&owner->policy, XR_OS_IO_OK};
    for (uint32_t i = 0; i < count; ++i) {
        if (!((&io)->status == XR_OS_IO_OK))
            return io.status;
        if (counts[i] < 0 || counts[i] > UINT16_MAX)
            return XR_OS_IO_BAD_ARGUMENT;
    }
    size_t bytes = (size_t) count * sizeof(*owner->variants);
    XrEnumVariantLayout *variants = io_alloc(&io, bytes);
    if (!variants)
        return io.status;
    (void) io_copy(&io, variants, owner->variants, bytes);
    for (uint32_t i = 0; i < count && io.status == XR_OS_IO_OK; ++i) {
        if (!((&io)->status == XR_OS_IO_OK))
            break;
        if (variants[i].payload_count != (uint16_t) counts[i]) {
            variants[i].payload_names = NULL;
            variants[i].payload_type_ids = NULL;
        }
        variants[i].payload_count = (uint16_t) counts[i];
    }
    XrEnumLayout staged = *layout;
    staged.variants = variants;
    if (io.status == XR_OS_IO_OK && enum_layout_recompute(&io, &staged))
        staged.layout_id = enum_nominal_id(&io, &staged);
    if (io.status == XR_OS_IO_OK)
        (void) ((&io)->status == XR_OS_IO_OK);
    if (io.status == XR_OS_IO_OK) {
        for (uint32_t i = 0; i < count; ++i)
            if (variants[i].payload_count != owner->variants[i].payload_count) {
                enum_free_payload(&owner->policy, &owner->variants[i], owner->metadata_counts[i]);
                owner->metadata_counts[i] = 0;
            }
        memcpy(owner->variants, variants, bytes);
        staged.variants = owner->variants;
        *layout = staged;
    }
    io_free(&io, variants);
    return io.status;
}

XR_FUNC XrOsIoStatus xr_enum_layout_set_variant_payload_metadata(XrEnumLayout *layout, uint32_t tag,
                                                                 const char *const *names,
                                                                 const uint8_t *type_ids,
                                                                 uint16_t count) {
    if (!layout)
        return XR_OS_IO_BAD_ARGUMENT;
    EnumLayoutOwner *owner = layout_owner(layout);
    if (!layout_shape(layout, owner) || tag >= owner->count ||
        layout->variants[tag].payload_count != count || (count && !type_ids))
        return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {&owner->policy, XR_OS_IO_OK};
    if (!enum_names_exact(&io, names, count)) {
        (void) io_status(&io, XR_OS_IO_BAD_ARGUMENT);
        return io.status;
    }
    for (uint16_t i = 0; i < count; ++i) {
        if (!((&io)->status == XR_OS_IO_OK))
            return io.status;
        if (type_ids[i] >= XR_TID_COUNT)
            return XR_OS_IO_BAD_ARGUMENT;
    }
    XrEnumVariantLayout staged = {0};
    if (count) {
        staged.payload_names = io_alloc(&io, (size_t) count * sizeof(*staged.payload_names));
        if (staged.payload_names && !io_clear(&io, (void *) staged.payload_names,
                                              (size_t) count * sizeof(*staged.payload_names))) {
            io_free(&io, (void *) staged.payload_names);
            staged.payload_names = NULL;
        }
        staged.payload_type_ids = io_alloc(&io, count);
        if (staged.payload_type_ids)
            (void) io_copy(&io, staged.payload_type_ids, type_ids, count);
        for (uint16_t i = 0; i < count && io.status == XR_OS_IO_OK; ++i)
            staged.payload_names[i] = enum_copy_text(&io, names[i]);
    }
    if (io.status == XR_OS_IO_OK)
        (void) ((&io)->status == XR_OS_IO_OK);
    if (io.status != XR_OS_IO_OK) {
        enum_free_payload(&owner->policy, &staged, count);
        return io.status;
    }
    XrEnumVariantLayout *variant = &owner->variants[tag];
    enum_free_payload(&owner->policy, variant, owner->metadata_counts[tag]);
    variant->payload_names = staged.payload_names;
    variant->payload_type_ids = staged.payload_type_ids;
    owner->metadata_counts[tag] = count;
    return XR_OS_IO_OK;
}

XR_FUNC XrOsIoStatus xr_enum_layout_set_variant_symbol(XrEnumLayout *layout, uint32_t tag,
                                                       int symbol) {
    if (!layout)
        return XR_OS_IO_BAD_ARGUMENT;
    EnumLayoutOwner *owner = layout_owner(layout);
    if (!layout_shape(layout, owner) || tag >= owner->count)
        return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {&owner->policy, XR_OS_IO_OK};
    if ((&io)->status == XR_OS_IO_OK)
        layout->variants[tag].symbol = symbol;
    return io.status;
}

XR_FUNC const XrEnumVariantLayout *xr_enum_layout_variant(const XrEnumLayout *layout,
                                                          uint32_t tag) {
    if (!layout || tag >= layout->variant_count)
        return NULL;
    return &layout->variants[tag];
}

XR_FUNC int xr_enum_layout_payload_count(const XrEnumLayout *layout, uint32_t tag) {
    const XrEnumVariantLayout *variant = xr_enum_layout_variant(layout, tag);
    return variant ? (int) variant->payload_count : 0;
}

XR_FUNC uint16_t xr_enum_layout_max_payload(const XrEnumLayout *layout) {
    if (!layout)
        return 0;
    uint16_t max_payload = 0;
    for (uint32_t i = 0; i < layout->variant_count; i++) {
        if (layout->variants[i].payload_count > max_payload)
            max_payload = layout->variants[i].payload_count;
    }
    return max_payload;
}

XR_FUNC void xr_enum_layout_free(XrEnumLayout *layout) {
    if (!layout)
        return;
    EnumLayoutOwner *owner = layout_owner(layout);
    XrOsIoPolicy policy = owner->policy;
    if (owner->variants)
        for (uint32_t i = 0; i < owner->count; ++i) {
            XrEnumVariantLayout *variant = &owner->variants[i];
            enum_free_payload(&policy, variant,
                              owner->metadata_counts ? owner->metadata_counts[i] : 0);
            if (variant->name)
                policy.free(policy.context, (void *) variant->name);
        }
    if (owner->variants)
        policy.free(policy.context, owner->variants);
    if (owner->metadata_counts)
        policy.free(policy.context, owner->metadata_counts);
    if (layout->name)
        policy.free(policy.context, (void *) layout->name);
    if (layout->nominal_owner)
        policy.free(policy.context, (void *) layout->nominal_owner);
    policy.free(policy.context, owner);
}
