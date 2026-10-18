/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_enum_storage.c - Owned enum records and staged layout publication
 */
#include "xanalyzer_enum_storage.h"
#include "../parser/xast_nodes_common.h"
#include "../../base/xio_policy.inc.h"
#include <stddef.h>
#include <string.h>

typedef struct EnumVariantStorage {
    uint16_t name_count, location_count, type_count;
} EnumVariantStorage;
typedef struct EnumInfoOwner {
    XrOsIoPolicy policy;
    uint32_t count;
    XaEnumVariantInfo *variants;
    EnumVariantStorage *storage;
    XaEnumInfo info;
} EnumInfoOwner;
static EnumInfoOwner *enum_owner(XaEnumInfo *info) {
    return (EnumInfoOwner *) ((char *) info - offsetof(EnumInfoOwner, info));
}
static const XrOsIoPolicy *variant_policy(XaEnumVariantInfo *variant) {
    if (!variant || !variant->owner)
        return NULL;
    EnumInfoOwner *owner = enum_owner(variant->owner);
    if (variant->owner->variants != owner->variants ||
        variant->owner->variant_count != owner->count)
        return NULL;
    uintptr_t first = (uintptr_t) owner->variants, address = (uintptr_t) variant;
    if (address < first || (address - first) % sizeof(*variant) ||
        (address - first) / sizeof(*variant) >= owner->count)
        return NULL;
    return &owner->policy;
}
static EnumVariantStorage *variant_storage(XaEnumVariantInfo *variant) {
    EnumInfoOwner *owner = enum_owner(variant->owner);
    return &owner->storage[variant - owner->variants];
}
static char *copy_text(XrIoContext *io, const char *text) {
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
static void free_names(const XrOsIoPolicy *policy, const char **names, uint16_t count) {
    if (!names)
        return;
    for (uint16_t i = 0; i < count; ++i)
        if (names[i])
            policy->free(policy->context, (void *) names[i]);
    policy->free(policy->context, (void *) names);
}
XR_FUNC XrCompileResourceStatus xa_enum_capture_status(XrCompileState *state, XrOsIoStatus status) {
    return xr_compile_state_fail(
        state, status == XR_OS_IO_OK              ? XR_COMPILE_RESOURCE_OK
               : status == XR_OS_IO_OUT_OF_MEMORY ? XR_COMPILE_RESOURCE_OUT_OF_MEMORY
               : status == XR_OS_IO_BUDGET        ? XR_COMPILE_RESOURCE_BUDGET
                                                  : XR_COMPILE_RESOURCE_BAD_ARGUMENT);
}
XR_FUNC void xa_enum_info_free(XaEnumInfo *info) {
    if (!info)
        return;
    EnumInfoOwner *owner = enum_owner(info);
    XrOsIoPolicy policy = owner->policy;
    if (owner->variants)
        for (uint32_t i = 0; i < owner->count; ++i) {
            XaEnumVariantInfo *variant = &owner->variants[i];
            free_names(&policy, variant->payload_names,
                       owner->storage ? owner->storage[i].name_count : 0);
            if (variant->payload_locations) {
                for (uint16_t j = 0; j < owner->storage[i].location_count; ++j)
                    if (variant->payload_locations[j].file)
                        policy.free(policy.context, (void *) variant->payload_locations[j].file);
                policy.free(policy.context, variant->payload_locations);
            }
            if (variant->payload_types)
                policy.free(policy.context, variant->payload_types);
            if (variant->name)
                policy.free(policy.context, (void *) variant->name);
        }
    if (owner->variants)
        policy.free(policy.context, owner->variants);
    if (owner->storage)
        policy.free(policy.context, owner->storage);
    xr_enum_layout_free(info->layout);
    if (info->nominal_owner)
        policy.free(policy.context, (void *) info->nominal_owner);
    if (info->name)
        policy.free(policy.context, (void *) info->name);
    policy.free(policy.context, owner);
}
XR_FUNC XrOsIoStatus xa_enum_info_new(const XrOsIoPolicy *policy, const char *nominal_owner,
                                      const char *name, uint32_t count, XaEnumInfo **output) {
    if (!io_policy_valid(policy) || !nominal_owner || !nominal_owner[0] || !name || !count ||
        !output || *output)
        return XR_OS_IO_BAD_ARGUMENT;
    if ((size_t) count > SIZE_MAX / sizeof(XaEnumVariantInfo))
        return XR_OS_IO_BUDGET;
    XrIoContext io = {policy, XR_OS_IO_OK};
    EnumInfoOwner *owner = io_alloc(&io, sizeof(*owner));
    if (!owner)
        return io.status;
    if (!io_clear(&io, owner, sizeof(*owner))) {
        io_free(&io, owner);
        return io.status;
    }
    owner->policy = *policy;
    owner->count = count;
    XaEnumInfo *info = &owner->info;
    info->variant_count = count;
    info->variants = io_alloc(&io, (size_t) count * sizeof(*info->variants));
    if (info->variants &&
        !io_clear(&io, info->variants, (size_t) count * sizeof(*info->variants))) {
        io_free(&io, info->variants);
        info->variants = NULL;
    }
    owner->variants = info->variants;
    owner->storage = io_alloc(&io, (size_t) count * sizeof(*owner->storage));
    if (owner->storage &&
        !io_clear(&io, owner->storage, (size_t) count * sizeof(*owner->storage))) {
        io_free(&io, owner->storage);
        owner->storage = NULL;
    }
    if (io.status == XR_OS_IO_OK)
        info->nominal_owner = copy_text(&io, nominal_owner);
    if (io.status == XR_OS_IO_OK)
        info->name = copy_text(&io, name);
    for (uint32_t i = 0; i < count && io.status == XR_OS_IO_OK; ++i) {
        if (!io_work(&io, 1))
            break;
        info->variants[i].owner = info;
        info->variants[i].symbol = -1;
        info->variants[i].tag = i;
    }
    if (io.status == XR_OS_IO_OK)
        (void) io_work(&io, 1);
    if (io.status != XR_OS_IO_OK) {
        xa_enum_info_free(info);
        return io.status;
    }
    *output = info;
    return XR_OS_IO_OK;
}
XR_FUNC XrOsIoStatus xa_enum_variant_set_name(XaEnumVariantInfo *variant, const char *name) {
    const XrOsIoPolicy *policy = variant_policy(variant);
    if (!policy || !name)
        return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {policy, XR_OS_IO_OK};
    char *copy = copy_text(&io, name);
    if (io.status == XR_OS_IO_OK)
        (void) io_work(&io, 1);
    if (io.status != XR_OS_IO_OK) {
        io_free(&io, copy);
        return io.status;
    }
    io_free(&io, (void *) variant->name);
    variant->name = copy;
    return XR_OS_IO_OK;
}
XR_FUNC XrOsIoStatus xa_enum_variant_set_payload_names(XaEnumVariantInfo *variant,
                                                       const char *const *names, uint16_t count) {
    const XrOsIoPolicy *policy = variant_policy(variant);
    if (!policy || variant->payload_names || variant->payload_count != count || (count && !names))
        return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {policy, XR_OS_IO_OK};
    const char **copy = NULL;
    if (count) {
        copy = io_alloc(&io, (size_t) count * sizeof(*copy));
        if (copy && !io_clear(&io, (void *) copy, (size_t) count * sizeof(*copy))) {
            io_free(&io, (void *) copy);
            copy = NULL;
        }
    }
    for (uint16_t i = 0; i < count && io.status == XR_OS_IO_OK; ++i) {
        size_t length;
        if (!io_work(&io, 1) || !io_length(&io, names[i], &length))
            break;
        if (!length) {
            (void) io_status(&io, XR_OS_IO_BAD_ARGUMENT);
            break;
        }
        for (uint16_t prior = 0; prior < i && io.status == XR_OS_IO_OK; ++prior) {
            size_t n = 0;
            for (;;) {
                if (!io_work(&io, 2))
                    break;
                char a = names[i][n], b = names[prior][n];
                if (a != b)
                    break;
                if (!a) {
                    (void) io_status(&io, XR_OS_IO_BAD_ARGUMENT);
                    break;
                }
                ++n;
            }
        }
        if (io.status == XR_OS_IO_OK)
            copy[i] = copy_text(&io, names[i]);
    }
    if (io.status == XR_OS_IO_OK)
        (void) io_work(&io, 1);
    if (io.status != XR_OS_IO_OK) {
        free_names(policy, copy, count);
        return io.status;
    }
    variant->payload_names = copy;
    variant_storage(variant)->name_count = count;
    return XR_OS_IO_OK;
}
XR_FUNC XrOsIoStatus xa_enum_variant_set_payload_locations(XaEnumVariantInfo *variant,
                                                           const char *file,
                                                           const XrNameSpan *spans,
                                                           uint16_t count) {
    const XrOsIoPolicy *policy = variant_policy(variant);
    if (!policy || variant->payload_locations || variant->payload_count != count ||
        (count && (!file || !file[0] || !spans || !variant->payload_names)))
        return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {policy, XR_OS_IO_OK};
    XrLocation *locations = NULL;
    if (count) {
        locations = io_alloc(&io, (size_t) count * sizeof(*locations));
        if (locations && !io_clear(&io, locations, (size_t) count * sizeof(*locations))) {
            io_free(&io, locations);
            locations = NULL;
        }
    }
    for (uint16_t i = 0; i < count && io.status == XR_OS_IO_OK; ++i) {
        if (!io_work(&io, 1))
            break;
        XrNameSpan span = spans[i];
        size_t length;
        if (span.line <= 0 || span.column <= 0 || !variant->payload_names[i]) {
            (void) io_status(&io, XR_OS_IO_BAD_ARGUMENT);
            break;
        }
        if (!io_length(&io, variant->payload_names[i], &length))
            break;
        if (length > UINT32_MAX - (uint32_t) span.column) {
            (void) io_status(&io, XR_OS_IO_BAD_ARGUMENT);
            break;
        }
        locations[i].file = copy_text(&io, file);
        if (io.status != XR_OS_IO_OK)
            break;
        locations[i].line = locations[i].end_line = (uint32_t) span.line;
        locations[i].column = (uint32_t) span.column;
        locations[i].end_column = (uint32_t) span.column + (uint32_t) length;
    }
    if (io.status == XR_OS_IO_OK)
        (void) io_work(&io, 1);
    if (io.status != XR_OS_IO_OK) {
        if (locations)
            for (uint16_t i = 0; i < count; ++i)
                io_free(&io, (void *) locations[i].file);
        io_free(&io, locations);
        return io.status;
    }
    variant->payload_locations = locations;
    variant_storage(variant)->location_count = count;
    return XR_OS_IO_OK;
}
XR_FUNC XrOsIoStatus xa_enum_variant_set_payload_types(XaEnumVariantInfo *variant,
                                                       XrType *const *types, uint16_t count) {
    const XrOsIoPolicy *policy = variant_policy(variant);
    if (!policy || variant->payload_types || variant->payload_count != count || (count && !types))
        return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {policy, XR_OS_IO_OK};
    XrType **copy = NULL;
    if (count) {
        copy = io_alloc(&io, (size_t) count * sizeof(*copy));
        if (copy)
            (void) io_copy(&io, copy, types, (size_t) count * sizeof(*copy));
    }
    if (io.status == XR_OS_IO_OK)
        (void) io_work(&io, 1);
    if (io.status != XR_OS_IO_OK) {
        io_free(&io, copy);
        return io.status;
    }
    variant->payload_types = copy;
    variant_storage(variant)->type_count = count;
    return XR_OS_IO_OK;
}
XR_FUNC XrOsIoStatus xa_enum_info_finalize_layout(XaEnumInfo *info) {
    if (!info || !info->variants || !info->variant_count)
        return XR_OS_IO_BAD_ARGUMENT;
    EnumInfoOwner *owner = enum_owner(info);
    if (info->variants != owner->variants || info->variant_count != owner->count)
        return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {&owner->policy, XR_OS_IO_OK};
    if ((size_t) info->variant_count > SIZE_MAX / sizeof(const char *))
        return XR_OS_IO_BUDGET;
    const char **names = io_alloc(&io, (size_t) info->variant_count * sizeof(*names));
    int *counts = io_alloc(&io, (size_t) info->variant_count * sizeof(*counts));
    XrEnumLayout *layout = NULL;
    for (uint32_t i = 0; i < info->variant_count && io.status == XR_OS_IO_OK; ++i) {
        if (!io_work(&io, 1))
            break;
        XaEnumVariantInfo *variant = &info->variants[i];
        if (variant->owner != info || !variant->name ||
            (variant->payload_count && (!variant->payload_names || !variant->payload_types))) {
            (void) io_status(&io, XR_OS_IO_BAD_ARGUMENT);
            break;
        }
        EnumVariantStorage *storage = &owner->storage[i];
        if ((variant->payload_names && storage->name_count != variant->payload_count) ||
            (variant->payload_types && storage->type_count != variant->payload_count) ||
            (variant->payload_locations && storage->location_count != variant->payload_count)) {
            (void) io_status(&io, XR_OS_IO_BAD_ARGUMENT);
            break;
        }
        names[i] = variant->name;
        counts[i] = variant->payload_count;
    }
    if (io.status == XR_OS_IO_OK)
        (void) io_status(&io, xr_enum_layout_new(io.policy, info->nominal_owner, info->name, names,
                                                 info->variant_count, &layout));
    if (io.status == XR_OS_IO_OK)
        (void) io_status(&io,
                         xr_enum_layout_set_payload_counts(layout, counts, info->variant_count));
    for (uint32_t i = 0; i < info->variant_count && io.status == XR_OS_IO_OK; ++i) {
        XaEnumVariantInfo *variant = &info->variants[i];
        if (!variant->payload_count)
            continue;
        uint8_t *ids = io_alloc(&io, variant->payload_count);
        for (uint16_t j = 0; j < variant->payload_count && io.status == XR_OS_IO_OK; ++j) {
            if (!io_work(&io, 1))
                break;
            ids[j] = xr_type_to_tid(variant->payload_types[j]);
        }
        if (io.status == XR_OS_IO_OK)
            (void) io_status(
                &io, xr_enum_layout_set_variant_payload_metadata(layout, i, variant->payload_names,
                                                                 ids, variant->payload_count));
        io_free(&io, ids);
    }
    if (io.status == XR_OS_IO_OK)
        (void) io_work(&io, 1);
    if (io.status == XR_OS_IO_OK) {
        XrEnumLayout *old = info->layout;
        info->layout = layout;
        info->is_payload_enum = !layout->is_zero_payload;
        xr_enum_layout_free(old);
    } else
        xr_enum_layout_free(layout);
    io_free(&io, (void *) names);
    io_free(&io, counts);
    return io.status;
}
XR_FUNC XrOsIoStatus xa_enum_info_clone(const XrOsIoPolicy *policy, const XaEnumInfo *source,
                                        XaEnumInfo **output) {
    if (!io_policy_valid(policy) || !output || *output)
        return XR_OS_IO_BAD_ARGUMENT;
    if (!source)
        return XR_OS_IO_OK;
    EnumInfoOwner *original = enum_owner((XaEnumInfo *) source);
    if (source->variants != original->variants || source->variant_count != original->count)
        return XR_OS_IO_BAD_ARGUMENT;
    XaEnumInfo *copy = NULL;
    XrOsIoStatus status =
        xa_enum_info_new(policy, source->nominal_owner, source->name, source->variant_count, &copy);
    XrIoContext io = {policy, status};
    for (uint32_t i = 0; i < source->variant_count && io.status == XR_OS_IO_OK; ++i) {
        if (!io_work(&io, 1))
            break;
        const XaEnumVariantInfo *src = &source->variants[i];
        XaEnumVariantInfo *dst = &copy->variants[i];
        EnumVariantStorage *storage = &original->storage[i];
        if (src->owner != source ||
            (src->payload_names && storage->name_count != src->payload_count) ||
            (src->payload_types && storage->type_count != src->payload_count) ||
            (src->payload_locations && storage->location_count != src->payload_count)) {
            (void) io_status(&io, XR_OS_IO_BAD_ARGUMENT);
            break;
        }
        dst->symbol = src->symbol;
        dst->tag = src->tag;
        dst->payload_count = src->payload_count;
        (void) io_status(&io, xa_enum_variant_set_name(dst, src->name));
        if (io.status == XR_OS_IO_OK)
            (void) io_status(&io, xa_enum_variant_set_payload_names(dst, src->payload_names,
                                                                    src->payload_count));
        if (io.status == XR_OS_IO_OK)
            (void) io_status(&io, xa_enum_variant_set_payload_types(dst, src->payload_types,
                                                                    src->payload_count));
        if (io.status == XR_OS_IO_OK && src->payload_locations) {
            XrLocation *locations = io_alloc(&io, (size_t) src->payload_count * sizeof(*locations));
            if (locations &&
                !io_clear(&io, locations, (size_t) src->payload_count * sizeof(*locations))) {
                io_free(&io, locations);
                locations = NULL;
            }
            for (uint16_t j = 0; j < src->payload_count && io.status == XR_OS_IO_OK; ++j) {
                if (!io_copy(&io, &locations[j], &src->payload_locations[j], sizeof(*locations)))
                    break;
                locations[j].file = NULL;
                locations[j].file = copy_text(&io, src->payload_locations[j].file);
            }
            if (io.status == XR_OS_IO_OK) {
                dst->payload_locations = locations;
                variant_storage(dst)->location_count = src->payload_count;
            } else {
                if (locations)
                    for (uint16_t j = 0; j < src->payload_count; ++j)
                        io_free(&io, (void *) locations[j].file);
                io_free(&io, locations);
            }
        }
    }
    if (io.status == XR_OS_IO_OK)
        (void) io_status(&io, xa_enum_info_finalize_layout(copy));
    if (io.status == XR_OS_IO_OK)
        (void) io_work(&io, 1);
    if (io.status != XR_OS_IO_OK) {
        xa_enum_info_free(copy);
        return io.status;
    }
    if (source->layout)
        copy->layout->layout_id = source->layout->layout_id;
    *output = copy;
    return XR_OS_IO_OK;
}
