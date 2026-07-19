/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xenum_layout.h - Compile-time enum tagged aggregate layout descriptor
 */

#ifndef XENUM_LAYOUT_H
#define XENUM_LAYOUT_H

#include <stdbool.h>
#include <stdint.h>
#include "../../base/xdefs.h"

typedef struct XrEnumVariantLayout {
    const char *name; /* Interned/stable; not owned by the layout. */
    int symbol;
    uint32_t tag;
    uint16_t payload_count;
    uint32_t payload_size;
    uint16_t payload_align;
    bool contains_refs;
    const char **payload_names; /* owned strings, one per payload slot */
    uint8_t *payload_type_ids;  /* stable XrTypeId values, one per payload slot */
} XrEnumVariantLayout;

typedef struct XrEnumLayout {
    uint32_t layout_id;
    const char *name; /* Interned/stable; not owned by the layout. */
    uint32_t variant_count;
    uint8_t tag_size;
    uint16_t align;
    uint32_t size;
    uint32_t payload_offset;
    uint32_t payload_size;
    bool contains_refs;
    bool is_zero_payload;
    XrEnumVariantLayout *variants;
} XrEnumLayout;

XR_FUNC XrEnumLayout *xr_enum_layout_new(const char *name, const char *const *variant_names,
                                         uint32_t variant_count);
XR_FUNC bool xr_enum_layout_set_payload_counts(XrEnumLayout *layout, const int *payload_counts,
                                               uint32_t count);
XR_FUNC bool xr_enum_layout_set_variant_payload_metadata(XrEnumLayout *layout, uint32_t tag,
                                                         const char *const *payload_names,
                                                         const uint8_t *payload_type_ids,
                                                         uint16_t payload_count);
XR_FUNC void xr_enum_layout_set_variant_symbol(XrEnumLayout *layout, uint32_t tag, int symbol);
XR_FUNC const XrEnumVariantLayout *xr_enum_layout_variant(const XrEnumLayout *layout, uint32_t tag);
XR_FUNC int xr_enum_layout_payload_count(const XrEnumLayout *layout, uint32_t tag);
XR_FUNC uint16_t xr_enum_layout_max_payload(const XrEnumLayout *layout);
XR_FUNC void xr_enum_layout_free(XrEnumLayout *layout);

#endif  // XENUM_LAYOUT_H
