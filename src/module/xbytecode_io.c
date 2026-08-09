/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xbytecode_io.c - Bytecode serialization/deserialization implementation
 *
 * KEY CONCEPT:
 *   Serializes XrProto to portable bytecode format (.xrc) and loads it back.
 *   Handles symbol table remapping for cross-compilation compatibility.
 */

#include "xbytecode_io.h"
#include "xmodule.h"
#include "../../stdlib/stdlib_cache.h"
#include "../base/xmalloc.h"
#include "../base/xfileio.h"
#include "../base/xlog.h"
#include "xray_vm.h"
#include "../runtime/xisolate_api.h"
#include "../runtime/xexec_state.h"
#include "../runtime/value/xchunk.h"
#include "../runtime/value/xslot_type.h"
#include "../runtime/value/xffi_sig.h"
#include "../runtime/value/xtype.h"
#include "../runtime/value/xstruct_layout.h"
#include "../runtime/object/xstring.h"
#include "../runtime/object/xbigint.h"
#include "../runtime/value/xvalue.h"
#include "../runtime/class/xclass.h"
#include "../runtime/class/xclass_system.h"
#include "../runtime/class/xclass_descriptor.h"
#include "../runtime/class/xenum.h"
#include "../runtime/class/xinstance.h"
#include "../base/xdynarray.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../runtime/symbol/xsymbol_table.h"

/* ========== Writer Helper ========== */

typedef struct BcWriteLayoutEntry {
    const XrAggregateLayout *layout;
    uint64_t key;
} BcWriteLayoutEntry;

typedef struct BcReadLayoutEntry {
    XrAggregateLayout *layout;
    uint64_t key;
    uint64_t *nested_keys;
    uint16_t *expected_offsets;
    uint16_t *expected_sizes;
    uint8_t state;
    bool transferred;
} BcReadLayoutEntry;

typedef struct {
    uint8_t *buf;
    size_t size;
    size_t capacity;
    XrVMRuntime *X;
    const char *stdlib_module;
    int flags;
    XrBcError error;
    BcWriteLayoutEntry *layouts;
    uint32_t layout_count;
    uint32_t layout_capacity;
} BcWriter;

static void bc_writer_init(BcWriter *w, XrVMRuntime *X, const char *stdlib_module, int flags) {
    XR_DCHECK(w != NULL, "bc_writer_init: NULL writer");
    w->buf = NULL;
    w->size = 0;
    w->capacity = 0;
    w->X = X;
    w->stdlib_module = stdlib_module;
    w->flags = flags;
    w->error = XR_BC_OK;
    w->layouts = NULL;
    w->layout_count = 0;
    w->layout_capacity = 0;
}

static bool bc_enum_shape_matches(const XrEnumType *actual, const XrEnumType *canonical) {
    if (!actual || !canonical || actual->member_count != canonical->member_count)
        return false;
    for (uint32_t i = 0; i < actual->member_count; i++) {
        const char *actual_name = xr_enum_type_member_name(actual, i);
        const char *canonical_name = xr_enum_type_member_name(canonical, i);
        if (!actual_name || !canonical_name || strcmp(actual_name, canonical_name) != 0 ||
            xr_enum_type_payload_count(actual, i) != xr_enum_type_payload_count(canonical, i))
            return false;
    }
    return true;
}

static bool bc_writer_ensure(BcWriter *w, size_t need) {
    if (w->size <= w->capacity && need <= w->capacity - w->size)
        return true;

    if (need > SIZE_MAX - w->size) {
        w->error = XR_BC_ERR_ALLOC;
        return false;
    }
    size_t required = w->size + need;

    size_t new_cap = w->capacity ? w->capacity * 2 : 4096;
    if (new_cap < w->capacity)
        new_cap = required;
    while (new_cap < required) {
        if (new_cap > SIZE_MAX / 2) {
            new_cap = required;
            break;
        }
        new_cap *= 2;
    }

    uint8_t *new_buf = xr_realloc(w->buf, new_cap);
    if (!new_buf) {
        w->error = XR_BC_ERR_ALLOC;
        return false;
    }

    w->buf = new_buf;
    w->capacity = new_cap;
    XR_DCHECK(w->size + need <= w->capacity, "bc_writer_ensure: capacity still insufficient");
    return true;
}

static bool bc_put_u8(BcWriter *w, uint8_t v) {
    if (!bc_writer_ensure(w, 1))
        return false;
    w->buf[w->size++] = v;
    return true;
}

static bool bc_put_u16(BcWriter *w, uint16_t v) {
    if (!bc_writer_ensure(w, 2))
        return false;
    w->buf[w->size++] = v & 0xFF;
    w->buf[w->size++] = (v >> 8) & 0xFF;
    return true;
}

static bool bc_put_u32(BcWriter *w, uint32_t v) {
    if (!bc_writer_ensure(w, 4))
        return false;
    w->buf[w->size++] = v & 0xFF;
    w->buf[w->size++] = (v >> 8) & 0xFF;
    w->buf[w->size++] = (v >> 16) & 0xFF;
    w->buf[w->size++] = (v >> 24) & 0xFF;
    return true;
}

static bool bc_put_u64(BcWriter *w, uint64_t v) {
    if (!bc_writer_ensure(w, 8))
        return false;
    for (int i = 0; i < 8; i++) {
        w->buf[w->size++] = (v >> (i * 8)) & 0xFF;
    }
    return true;
}

static bool bc_put_i64(BcWriter *w, int64_t v) {
    return bc_put_u64(w, (uint64_t) v);
}

static bool bc_put_f64(BcWriter *w, double v) {
    union {
        double d;
        uint64_t u;
    } u;
    u.d = v;
    return bc_put_u64(w, u.u);
}

static bool bc_put_bytes(BcWriter *w, const void *data, size_t len) {
    if (!bc_writer_ensure(w, len))
        return false;
    memcpy(w->buf + w->size, data, len);
    w->size += len;
    return true;
}

static bool bc_put_string_data(BcWriter *w, const char *str, uint32_t len) {
    if (len > 0 && !str)
        return false;
    if (!bc_put_u32(w, len))
        return false;
    if (len > 0 && !bc_put_bytes(w, str, len))
        return false;
    return true;
}

static bool bc_put_string(BcWriter *w, const char *str) {
    size_t len = str ? strlen(str) : 0;
    if (len > UINT32_MAX) {
        w->error = XR_BC_ERR_METADATA;
        return false;
    }
    return bc_put_string_data(w, str ? str : "", (uint32_t) len);
}

/* ========== Reader Helper ========== */

typedef struct {
    const uint8_t *buf;
    size_t size;
    size_t pos;
    XrVMRuntime *X;
    XrBcError error;
    BcReadLayoutEntry *layouts;
    uint32_t layout_count;
} BcReader;

static void bc_reader_init(BcReader *r, XrVMRuntime *X, const uint8_t *buf, size_t size) {
    XR_DCHECK(r != NULL, "bc_reader_init: NULL reader");
    XR_DCHECK(buf != NULL, "bc_reader_init: NULL buf");
    XR_DCHECK(size > 0, "bc_reader_init: zero size");
    r->buf = buf;
    r->size = size;
    r->pos = 0;
    r->X = X;
    r->error = XR_BC_OK;
    r->layouts = NULL;
    r->layout_count = 0;
}

static bool bc_has_bytes(BcReader *r, size_t n) {
    return r->pos <= r->size && n <= r->size - r->pos;
}

static uint8_t bc_get_u8(BcReader *r) {
    if (!bc_has_bytes(r, 1)) {
        r->error = XR_BC_ERR_TRUNCATED;
        return 0;
    }
    return r->buf[r->pos++];
}

static uint16_t bc_get_u16(BcReader *r) {
    if (!bc_has_bytes(r, 2)) {
        r->error = XR_BC_ERR_TRUNCATED;
        return 0;
    }
    uint16_t v = r->buf[r->pos] | (r->buf[r->pos + 1] << 8);
    r->pos += 2;
    return v;
}

static uint32_t bc_get_u32(BcReader *r) {
    if (!bc_has_bytes(r, 4)) {
        r->error = XR_BC_ERR_TRUNCATED;
        return 0;
    }
    uint32_t v = (uint32_t) r->buf[r->pos] | ((uint32_t) r->buf[r->pos + 1] << 8) |
                 ((uint32_t) r->buf[r->pos + 2] << 16) | ((uint32_t) r->buf[r->pos + 3] << 24);
    r->pos += 4;
    return v;
}

static uint64_t bc_get_u64(BcReader *r) {
    if (!bc_has_bytes(r, 8)) {
        r->error = XR_BC_ERR_TRUNCATED;
        return 0;
    }
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v |= ((uint64_t) r->buf[r->pos++]) << (i * 8);
    }
    return v;
}

static int64_t bc_get_i64(BcReader *r) {
    return (int64_t) bc_get_u64(r);
}

static double bc_get_f64(BcReader *r) {
    union {
        double d;
        uint64_t u;
    } u;
    u.u = bc_get_u64(r);
    return u.d;
}

static char *bc_get_string_data(BcReader *r, uint32_t *out_len) {
    uint32_t len = bc_get_u32(r);
    if (r->error != XR_BC_OK)
        return NULL;
    if (!bc_has_bytes(r, len)) {
        r->error = XR_BC_ERR_TRUNCATED;
        return NULL;
    }

    if ((size_t) len > SIZE_MAX - 1u) {
        r->error = XR_BC_ERR_CORRUPT;
        return NULL;
    }
    char *str = xr_malloc((size_t) len + 1u);
    if (!str) {
        r->error = XR_BC_ERR_ALLOC;
        return NULL;
    }

    memcpy(str, r->buf + r->pos, len);
    str[len] = '\0';
    r->pos += len;
    if (out_len)
        *out_len = len;
    return str;
}

static char *bc_get_string(BcReader *r) {
    return bc_get_string_data(r, NULL);
}

static bool bc_put_optional_string(BcWriter *w, const char *str);
static char *bc_read_optional_string(BcReader *r);

/* ========== Canonical Aggregate Layout Table ========== */

#define BC_LAYOUT_FORMAT_VERSION 2u
#define BC_MAX_LAYOUTS 4096u
#define BC_MAX_LAYOUT_DEPTH 16u

static bool bc_writer_add_layout(BcWriter *w, const XrAggregateLayout *layout, uint32_t depth) {
    if (!w || !layout || depth > BC_MAX_LAYOUT_DEPTH || layout->field_count > XR_MAX_AGG_FIELDS) {
        if (w)
            w->error = XR_BC_ERR_METADATA;
        return false;
    }
    const XrTargetDataLayout *target = xr_target_data_layout_host();
    if (!target || layout->target_abi_hash != target->stable_hash) {
        w->error = XR_BC_ERR_TARGET_ABI;
        return false;
    }
    uint64_t key = xr_aggregate_layout_stable_key(layout);
    if (key == 0) {
        w->error = XR_BC_ERR_METADATA;
        return false;
    }
    for (uint32_t i = 0; i < w->layout_count; i++) {
        if (w->layouts[i].key != key)
            continue;
        if (!xr_aggregate_layout_semantically_equal(w->layouts[i].layout, layout)) {
            w->error = XR_BC_ERR_METADATA;
            return false;
        }
        return true;
    }
    for (uint16_t i = 0; i < layout->field_count; i++) {
        const XrAggregateFieldLayout *field = &layout->fields[i];
        if (field->native_type == XR_NATIVE_NESTED_AGGREGATE &&
            !bc_writer_add_layout(w, field->sub_layout, depth + 1))
            return false;
    }
    XrAggregateLayout computed = *layout;
    if (!xr_aggregate_layout_compute(&computed, target) ||
        computed.total_size != layout->total_size || computed.alignment != layout->alignment) {
        w->error = XR_BC_ERR_METADATA;
        return false;
    }
    for (uint16_t i = 0; i < layout->field_count; i++) {
        if (computed.fields[i].offset != layout->fields[i].offset ||
            computed.fields[i].size != layout->fields[i].size) {
            w->error = XR_BC_ERR_METADATA;
            return false;
        }
    }
    if (w->layout_count == BC_MAX_LAYOUTS) {
        w->error = XR_BC_ERR_METADATA;
        return false;
    }
    if (w->layout_count == w->layout_capacity) {
        uint32_t next = w->layout_capacity ? w->layout_capacity * 2u : 16u;
        if (next > BC_MAX_LAYOUTS)
            next = BC_MAX_LAYOUTS;
        BcWriteLayoutEntry *entries =
            (BcWriteLayoutEntry *) xr_realloc(w->layouts, (size_t) next * sizeof(*entries));
        if (!entries) {
            w->error = XR_BC_ERR_ALLOC;
            return false;
        }
        w->layouts = entries;
        w->layout_capacity = next;
    }
    w->layouts[w->layout_count++] = (BcWriteLayoutEntry) {.layout = layout, .key = key};
    return true;
}

static int bc_layout_entry_compare(const void *left, const void *right) {
    const BcWriteLayoutEntry *a = (const BcWriteLayoutEntry *) left;
    const BcWriteLayoutEntry *b = (const BcWriteLayoutEntry *) right;
    return a->key < b->key ? -1 : a->key > b->key ? 1 : 0;
}

static uint64_t bc_writer_layout_key(BcWriter *w, const XrAggregateLayout *layout) {
    if (!layout)
        return 0;
    uint64_t key = xr_aggregate_layout_stable_key(layout);
    for (uint32_t i = 0; i < w->layout_count; i++) {
        if (w->layouts[i].key == key &&
            xr_aggregate_layout_semantically_equal(w->layouts[i].layout, layout))
            return key;
    }
    w->error = XR_BC_ERR_METADATA;
    return 0;
}

static bool bc_write_layout_table(BcWriter *w) {
    if (!bc_put_u32(w, w->layout_count))
        return false;
    for (uint32_t li = 0; li < w->layout_count; li++) {
        const BcWriteLayoutEntry *entry = &w->layouts[li];
        const XrAggregateLayout *layout = entry->layout;
        if (!bc_put_u32(w, BC_LAYOUT_FORMAT_VERSION) || !bc_put_u64(w, entry->key) ||
            !bc_put_u64(w, layout->target_abi_hash) || !bc_put_u8(w, layout->kind) ||
            !bc_put_u32(w, layout->total_size) || !bc_put_u32(w, layout->alignment) ||
            !bc_put_u32(w, layout->explicit_align) || !bc_put_u16(w, layout->field_count))
            return false;
        for (uint16_t fi = 0; fi < layout->field_count; fi++) {
            const XrAggregateFieldLayout *field = &layout->fields[fi];
            const char *name = layout->field_names ? layout->field_names[fi] : NULL;
            uint64_t nested_key = 0;
            if (field->native_type == XR_NATIVE_NESTED_AGGREGATE) {
                nested_key = bc_writer_layout_key(w, field->sub_layout);
                if (nested_key == 0)
                    return false;
            }
            if (!bc_put_optional_string(w, name) || !bc_put_u32(w, field->offset) ||
                !bc_put_u32(w, field->size) || !bc_put_u8(w, field->native_type) ||
                !bc_put_u32(w, field->elem_count) || !bc_put_u8(w, field->elem_native_type) ||
                !bc_put_u64(w, nested_key) || !bc_put_u8(w, field->is_flexible ? 1 : 0))
                return false;
        }
    }
    return true;
}

static int bc_reader_layout_index(const BcReader *r, uint64_t key) {
    uint32_t lo = 0;
    uint32_t hi = r ? r->layout_count : 0;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        uint64_t candidate = r->layouts[mid].key;
        if (candidate < key)
            lo = mid + 1u;
        else
            hi = mid;
    }
    return r && lo < r->layout_count && r->layouts[lo].key == key ? (int) lo : -1;
}

static XrAggregateLayout *bc_reader_layout(BcReader *r, uint64_t key) {
    if (key == 0)
        return NULL;
    int index = bc_reader_layout_index(r, key);
    if (index < 0) {
        r->error = XR_BC_ERR_CORRUPT;
        return NULL;
    }
    return r->layouts[index].layout;
}

static void bc_reader_layout_table_dispose(BcReader *r) {
    if (!r || !r->layouts)
        return;
    for (uint32_t i = 0; i < r->layout_count; i++) {
        BcReadLayoutEntry *entry = &r->layouts[i];
        if (!entry->transferred)
            xr_aggregate_layout_free_owned(entry->layout);
        xr_free(entry->nested_keys);
        xr_free(entry->expected_offsets);
        xr_free(entry->expected_sizes);
    }
    xr_free(r->layouts);
    r->layouts = NULL;
    r->layout_count = 0;
}

static bool bc_reader_validate_layout(BcReader *r, uint32_t index, uint32_t depth) {
    if (!r || index >= r->layout_count || depth > BC_MAX_LAYOUT_DEPTH) {
        if (r)
            r->error = XR_BC_ERR_CORRUPT;
        return false;
    }
    BcReadLayoutEntry *entry = &r->layouts[index];
    if (entry->state == 2)
        return true;
    if (entry->state == 1) {
        r->error = XR_BC_ERR_CORRUPT;
        return false;
    }
    entry->state = 1;
    XrAggregateLayout *layout = entry->layout;
    uint16_t expected_total = layout->total_size;
    uint32_t expected_align = layout->alignment;
    for (uint16_t i = 0; i < layout->field_count; i++) {
        XrAggregateFieldLayout *field = &layout->fields[i];
        uint64_t nested_key = entry->nested_keys[i];
        if (field->native_type == XR_NATIVE_NESTED_AGGREGATE) {
            int child = bc_reader_layout_index(r, nested_key);
            if (nested_key == 0 || child < 0) {
                r->error = XR_BC_ERR_CORRUPT;
                return false;
            }
            if (!bc_reader_validate_layout(r, (uint32_t) child, depth + 1))
                return false;
            field->sub_layout = r->layouts[child].layout;
        } else if (nested_key != 0) {
            r->error = XR_BC_ERR_CORRUPT;
            return false;
        }
    }
    const XrTargetDataLayout *target = xr_target_data_layout_host();
    if (!xr_aggregate_layout_compute(layout, target) || layout->total_size != expected_total ||
        layout->alignment != expected_align) {
        r->error = XR_BC_ERR_CORRUPT;
        return false;
    }
    for (uint16_t i = 0; i < layout->field_count; i++) {
        if (layout->fields[i].offset != entry->expected_offsets[i] ||
            layout->fields[i].size != entry->expected_sizes[i]) {
            r->error = XR_BC_ERR_CORRUPT;
            return false;
        }
    }
    if (xr_aggregate_layout_stable_key(layout) != entry->key) {
        r->error = XR_BC_ERR_CORRUPT;
        return false;
    }
    entry->state = 2;
    return true;
}

static bool bc_reader_intern_layout(BcReader *r, uint32_t index) {
    BcReadLayoutEntry *entry = &r->layouts[index];
    if (entry->transferred)
        return true;
    for (uint16_t i = 0; i < entry->layout->field_count; i++) {
        if (entry->layout->fields[i].native_type != XR_NATIVE_NESTED_AGGREGATE)
            continue;
        int child = bc_reader_layout_index(r, entry->nested_keys[i]);
        if (child < 0 || !bc_reader_intern_layout(r, (uint32_t) child))
            return false;
        entry->layout->fields[i].sub_layout = r->layouts[child].layout;
    }
    XrAggregateLayout *canonical =
        xr_vm_struct_layout_intern_owned(xr_isolate_get_vm_state(r->X), entry->layout);
    if (!canonical) {
        r->error = XR_BC_ERR_ALLOC;
        return false;
    }
    entry->layout = canonical;
    entry->transferred = true;
    return true;
}

static bool bc_read_layout_table(BcReader *r) {
    uint32_t count = bc_get_u32(r);
    if (r->error != XR_BC_OK)
        return false;
    if (count > BC_MAX_LAYOUTS) {
        r->error = XR_BC_ERR_CORRUPT;
        return false;
    }
    if (count == 0)
        return true;
    r->layouts = (BcReadLayoutEntry *) xr_calloc(count, sizeof(*r->layouts));
    if (!r->layouts) {
        r->error = XR_BC_ERR_ALLOC;
        return false;
    }
    r->layout_count = count;
    const XrTargetDataLayout *target = xr_target_data_layout_host();
    if (!target) {
        r->error = XR_BC_ERR_TARGET_ABI;
        return false;
    }
    uint64_t previous_key = 0;
    for (uint32_t li = 0; li < count; li++) {
        BcReadLayoutEntry *entry = &r->layouts[li];
        uint32_t format = bc_get_u32(r);
        entry->key = bc_get_u64(r);
        uint64_t target_hash = bc_get_u64(r);
        uint8_t kind = bc_get_u8(r);
        uint32_t total_size = bc_get_u32(r);
        uint32_t alignment = bc_get_u32(r);
        uint32_t explicit_align = bc_get_u32(r);
        uint16_t field_count = bc_get_u16(r);
        if (r->error != XR_BC_OK)
            return false;
        if (format != BC_LAYOUT_FORMAT_VERSION || entry->key == 0 ||
            (li > 0 && entry->key <= previous_key) || kind > XR_AGG_LAYOUT_UNION ||
            total_size > UINT16_MAX || alignment == 0 || alignment > UINT16_MAX ||
            field_count > XR_MAX_AGG_FIELDS) {
            r->error = XR_BC_ERR_CORRUPT;
            return false;
        }
        if (target_hash != target->stable_hash) {
            r->error = XR_BC_ERR_TARGET_ABI;
            return false;
        }
        previous_key = entry->key;
        XrAggregateLayout *layout = (XrAggregateLayout *) xr_calloc(1, sizeof(*layout));
        if (!layout) {
            r->error = XR_BC_ERR_ALLOC;
            return false;
        }
        entry->layout = layout;
        layout->target_abi_hash = target_hash;
        layout->kind = kind;
        layout->total_size = (uint16_t) total_size;
        layout->alignment = alignment;
        layout->explicit_align = explicit_align;
        layout->field_count = field_count;
        if (field_count > 0) {
            layout->field_names = (const char **) xr_calloc(field_count, sizeof(char *));
            entry->nested_keys = (uint64_t *) xr_calloc(field_count, sizeof(uint64_t));
            entry->expected_offsets = (uint16_t *) xr_calloc(field_count, sizeof(uint16_t));
            entry->expected_sizes = (uint16_t *) xr_calloc(field_count, sizeof(uint16_t));
            if (!layout->field_names || !entry->nested_keys || !entry->expected_offsets ||
                !entry->expected_sizes) {
                r->error = XR_BC_ERR_ALLOC;
                return false;
            }
        }
        for (uint16_t fi = 0; fi < field_count; fi++) {
            char *name = bc_read_optional_string(r);
            uint32_t offset = bc_get_u32(r);
            uint32_t size = bc_get_u32(r);
            uint8_t native_type = bc_get_u8(r);
            uint32_t elem_count = bc_get_u32(r);
            uint8_t elem_type = bc_get_u8(r);
            uint64_t nested_key = bc_get_u64(r);
            uint8_t flexible = bc_get_u8(r);
            if (r->error != XR_BC_OK) {
                xr_free(name);
                return false;
            }
            if (offset > UINT16_MAX || size > UINT16_MAX || elem_count > UINT16_MAX ||
                native_type > XR_NATIVE_POINTER || elem_type > XR_NATIVE_POINTER || flexible > 1) {
                xr_free(name);
                r->error = XR_BC_ERR_CORRUPT;
                return false;
            }
            layout->field_names[fi] = name;
            layout->fields[fi].offset = (uint16_t) offset;
            layout->fields[fi].size = (uint16_t) size;
            layout->fields[fi].native_type = native_type;
            layout->fields[fi].elem_count = (uint16_t) elem_count;
            layout->fields[fi].elem_native_type = elem_type;
            layout->fields[fi].is_flexible = flexible != 0;
            entry->nested_keys[fi] = nested_key;
            entry->expected_offsets[fi] = (uint16_t) offset;
            entry->expected_sizes[fi] = (uint16_t) size;
        }
    }
    for (uint32_t i = 0; i < count; i++) {
        if (!bc_reader_validate_layout(r, i, 0))
            return false;
    }
    for (uint32_t i = 0; i < count; i++) {
        if (!bc_reader_intern_layout(r, i))
            return false;
    }
    return true;
}

/* ========== Value Serialization ========== */

// Value type tags
#define BC_VAL_NULL 0
#define BC_VAL_BOOL 1
#define BC_VAL_INT 2
#define BC_VAL_FLOAT 3
#define BC_VAL_STRING 4
#define BC_VAL_DYNAMIC_SHAPE 5
#define BC_VAL_CLASS_DESCRIPTOR 6
#define BC_VAL_ENUM_TYPE 7
#define BC_VAL_RUNE 8
#define BC_VAL_BIGINT 9

#define BC_SHAPE_JSON 1
#define BC_SHAPE_STRUCT_OBJECT 2
#define BC_SHAPE_JSON_DECODE_ROOT 3

static bool bc_write_dynamic_shape(BcWriter *w, XrValue val);
static bool bc_write_value(BcWriter *w, XrValue val, bool as_dynamic_shape,
                           bool as_class_descriptor);
static XrValue bc_read_value(BcReader *r);
static bool bc_read_json_decode_schema(BcReader *r, XrJsonDecodeSchema *out, int depth);

static bool bc_write_json_decode_schema(BcWriter *w, const XrJsonDecodeSchema *schema, int depth) {
    if (!w || !schema || depth > 32 ||
        xr_json_value_kind_base(schema->value_kind) > XR_JSON_VALUE_CLASS_INSTANCE)
        return false;
    if (!bc_put_u8(w, schema->value_kind) || !bc_put_u8(w, schema->storage_type))
        return false;
    switch ((XrJsonValueKind) xr_json_value_kind_base(schema->value_kind)) {
        case XR_JSON_VALUE_STRUCT_OBJECT:
            return schema->target_descriptor &&
                   bc_write_dynamic_shape(
                       w, XR_FROM_INT((int64_t) (intptr_t) schema->target_descriptor));
        case XR_JSON_VALUE_ARRAY:
        case XR_JSON_VALUE_MAP:
            return schema->child && bc_write_json_decode_schema(w, schema->child, depth + 1);
        case XR_JSON_VALUE_ENUM:
            return schema->target_descriptor &&
                   bc_write_value(w, XR_FROM_PTR(schema->target_descriptor), false, false);
        case XR_JSON_VALUE_CLASS_INSTANCE:
            return schema->target_descriptor &&
                   bc_write_value(w, XR_FROM_PTR(schema->target_descriptor), false, false);
        default:
            return schema->child == NULL;
    }
}

static bool bc_write_dynamic_shape(BcWriter *w, XrValue val) {
    if (!XR_IS_INT(val))
        return false;

    XrClass *cls = (XrClass *) (intptr_t) XR_TO_INT(val);
    if (!cls || !(cls->flags & XR_CLASS_DYNAMIC_LAYOUT))
        return false;

    uint8_t kind = 0;
    if ((cls->flags & XR_CLASS_JSON_DECODE_ROOT) != 0) {
        kind = BC_SHAPE_JSON_DECODE_ROOT;
    } else if (cls->builtin_kind == XR_BK_JSON) {
        kind = BC_SHAPE_JSON;
    } else if (cls->builtin_kind == XR_BK_STRUCT_OBJECT) {
        kind = BC_SHAPE_STRUCT_OBJECT;
    } else {
        return false;
    }

    if (!bc_put_u8(w, BC_VAL_DYNAMIC_SHAPE))
        return false;
    if (!bc_put_u8(w, kind))
        return false;
    if (!bc_put_u8(w, (cls->flags & XR_CLASS_DYNAMIC_SEALED) ? 1 : 0))
        return false;
    if (!bc_put_u32(w, cls->field_count))
        return false;
    for (uint16_t i = 0; i < cls->field_count; i++) {
        const char *name = (cls->fields && cls->fields[i].name) ? cls->fields[i].name : "";
        if (!bc_put_string(w, name))
            return false;
        uint64_t stable_type_key = cls->fields ? cls->fields[i].stable_type_key : 0;
        uint8_t shape_flags = cls->fields ? cls->fields[i].shape_flags : 0;
        if (!bc_put_u64(w, stable_type_key) || !bc_put_u8(w, shape_flags))
            return false;
        XrJsonDecodeSchema schema =
            cls->fields ? cls->fields[i].json_decode_schema : (XrJsonDecodeSchema) {0};
        if (cls->fields)
            schema.value_kind = cls->fields[i].json_value_kind;
        if (xr_json_value_kind_base(schema.value_kind) == XR_JSON_VALUE_STRUCT_OBJECT &&
            !schema.target_descriptor && cls->fields)
            schema.target_descriptor = cls->fields[i].json_struct_object_class;
        if (!bc_write_json_decode_schema(w, &schema, 0))
            return false;
    }
    return true;
}

static bool bc_put_optional_string(BcWriter *w, const char *str) {
    if (!bc_put_u8(w, str ? 1 : 0))
        return false;
    return !str || bc_put_string(w, str);
}

static bool bc_write_field_descriptor(BcWriter *w, const XrFieldDescriptorEntry *field) {
    if (!field || !field->name)
        return false;
    if (!bc_put_string(w, field->name))
        return false;
    if (!bc_put_optional_string(w, field->type_name))
        return false;
    if (!bc_write_value(w, field->default_value, false, false))
        return false;
    return bc_put_u16(w, field->flags) &&
           bc_write_json_decode_schema(w, &field->json_decode_schema, 0);
}

static bool bc_write_method_descriptor(BcWriter *w, const XrMethodDescriptorEntry *method) {
    if (!method || !method->name)
        return false;
    if (!bc_put_string(w, method->name))
        return false;
    if (!bc_put_u32(w, method->closure_index))
        return false;
    if (!bc_put_optional_string(w, method->return_type_name))
        return false;
    if (!bc_put_u8(w, method->param_count))
        return false;
    for (uint8_t i = 0; i < method->param_count; i++) {
        const char *param_type = method->param_type_names ? method->param_type_names[i] : NULL;
        if (!bc_put_optional_string(w, param_type))
            return false;
    }
    if (!bc_put_u16(w, method->flags))
        return false;
    if (!bc_put_u8(w, method->op_type))
        return false;
    return bc_put_u8(w, method->is_operator ? 1 : 0);
}

static bool bc_write_class_descriptor(BcWriter *w, XrValue val) {
    if (!XR_IS_PTR(val) || !XR_TO_PTR(val))
        return false;

    const XrClassDescriptor *desc = (const XrClassDescriptor *) XR_TO_PTR(val);

    if (!bc_put_u8(w, BC_VAL_CLASS_DESCRIPTOR))
        return false;
    if (!desc->class_name || !bc_put_string(w, desc->class_name))
        return false;
    if (!bc_put_optional_string(w, desc->super_name))
        return false;
    if (!bc_put_optional_string(w, desc->generic_origin_name))
        return false;
    if (!bc_put_optional_string(w, desc->display_name))
        return false;
    if (desc->mono_type_arg_count < 0 || desc->mono_type_arg_count > UINT16_MAX)
        return false;
    if (!bc_put_u32(w, (uint32_t) desc->mono_type_arg_count))
        return false;
    for (int i = 0; i < desc->mono_type_arg_count; i++) {
        const char *name = desc->mono_type_arg_names ? desc->mono_type_arg_names[i] : NULL;
        if (!bc_put_optional_string(w, name))
            return false;
    }
    if (!bc_put_u32(w, (uint32_t) desc->super_global_index))
        return false;
    if (!bc_put_u32(w, desc->flags))
        return false;
    if (!bc_put_u8(w, desc->is_monomorphized ? 1 : 0))
        return false;

    if (!bc_put_u32(w, desc->instance_field_count))
        return false;
    for (uint32_t i = 0; i < desc->instance_field_count; i++) {
        if (!bc_write_field_descriptor(w, &desc->instance_fields[i]))
            return false;
    }

    if (!bc_put_u32(w, desc->static_field_count))
        return false;
    for (uint32_t i = 0; i < desc->static_field_count; i++) {
        if (!bc_write_field_descriptor(w, &desc->static_fields[i]))
            return false;
    }

    if (!bc_put_u32(w, desc->instance_method_count))
        return false;
    for (uint32_t i = 0; i < desc->instance_method_count; i++) {
        if (!bc_write_method_descriptor(w, &desc->instance_methods[i]))
            return false;
    }

    if (!bc_put_u32(w, desc->static_method_count))
        return false;
    for (uint32_t i = 0; i < desc->static_method_count; i++) {
        if (!bc_write_method_descriptor(w, &desc->static_methods[i]))
            return false;
    }

    if (!bc_put_u8(w, desc->interface_count))
        return false;
    for (uint8_t i = 0; i < desc->interface_count; i++) {
        const char *name = desc->interfaces ? desc->interfaces[i].interface_name : NULL;
        if (!bc_put_optional_string(w, name))
            return false;
    }

    if (!bc_put_u32(w, (uint32_t) desc->clinit_proto_index))
        return false;
    if (!bc_put_u32(w, desc->descriptor_version))
        return false;
    if (!bc_put_u32(w, desc->checksum))
        return false;
    uint64_t layout_key = bc_writer_layout_key(w, desc->struct_layout);
    if (desc->struct_layout && layout_key == 0)
        return false;
    return bc_put_u64(w, layout_key);
}

static bool bc_write_enum_type(BcWriter *w, XrValue val) {
    if (!XR_IS_ENUM_TYPE(val))
        return false;

    XrEnumType *enum_type = XR_TO_ENUM_TYPE(val);
    const char *nominal_owner =
        enum_type && enum_type->layout ? enum_type->layout->nominal_owner : NULL;
    if (!enum_type || !nominal_owner || !nominal_owner[0] || !enum_type->name ||
        enum_type->member_count > UINT16_MAX)
        return false;

    if (!bc_put_u8(w, BC_VAL_ENUM_TYPE))
        return false;
    if (w->stdlib_module && strcmp(nominal_owner, w->stdlib_module) == 0) {
        XrEnumType *canonical = xr_stdlib_enum_type_get(w->X, w->stdlib_module, enum_type->name);
        if (canonical && !bc_enum_shape_matches(enum_type, canonical)) {
            w->error = XR_BC_ERR_METADATA;
            return false;
        }
    }
    if (!bc_put_string(w, nominal_owner))
        return false;
    if (!bc_put_string(w, enum_type->name))
        return false;
    if (!bc_put_u32(w, enum_type->member_count))
        return false;
    if (!bc_put_u32(w, enum_type->derive_flags))
        return false;

    for (uint32_t i = 0; i < enum_type->member_count; i++) {
        const char *name = xr_enum_type_member_name(enum_type, i);
        if (!name || !bc_put_string(w, name))
            return false;
        int payload_count = xr_enum_type_payload_count(enum_type, i);
        if (payload_count < 0 || payload_count > UINT16_MAX)
            return false;
        if (!bc_put_u16(w, (uint16_t) payload_count))
            return false;
        const XrEnumVariantLayout *variant =
            enum_type->layout ? xr_enum_layout_variant(enum_type->layout, i) : NULL;
        for (int p = 0; p < payload_count; p++) {
            const char *payload_name =
                variant && variant->payload_names && variant->payload_names[p]
                    ? variant->payload_names[p]
                    : "";
            uint8_t payload_type_id =
                variant && variant->payload_type_ids ? variant->payload_type_ids[p] : XR_TID_NULL;
            if (!bc_put_string(w, payload_name) || !bc_put_u8(w, payload_type_id))
                return false;
        }
    }
    return true;
}

static bool bc_write_value(BcWriter *w, XrValue val, bool as_dynamic_shape,
                           bool as_class_descriptor) {
    if (as_class_descriptor)
        return bc_write_class_descriptor(w, val);
    if (as_dynamic_shape)
        return bc_write_dynamic_shape(w, val);
    if (XR_IS_ENUM_TYPE(val))
        return bc_write_enum_type(w, val);

    if (XR_IS_NULL(val)) {
        return bc_put_u8(w, BC_VAL_NULL);
    } else if (XR_IS_BOOL(val)) {
        if (!bc_put_u8(w, BC_VAL_BOOL))
            return false;
        return bc_put_u8(w, XR_TO_BOOL(val) ? 1 : 0);
    } else if (XR_IS_INT(val)) {
        if (!bc_put_u8(w, BC_VAL_INT))
            return false;
        return bc_put_i64(w, XR_TO_INT(val));
    } else if (XR_IS_RUNE(val)) {
        if (!bc_put_u8(w, BC_VAL_RUNE))
            return false;
        return bc_put_u32(w, XR_TO_RUNE(val));
    } else if (XR_IS_FLOAT(val)) {
        if (!bc_put_u8(w, BC_VAL_FLOAT))
            return false;
        return bc_put_f64(w, XR_TO_FLOAT(val));
    } else if (XR_IS_STRING(val)) {
        if (!bc_put_u8(w, BC_VAL_STRING))
            return false;
        XrString *s = XR_TO_STRING(val);
        return bc_put_string_data(w, s->data, s->length);
    } else if (XR_IS_BIGINT(val)) {
        /* BigInt literal constants serialize as their canonical decimal string.
         * The reader rebuilds an identical fixed-heap XrBigInt, so embedded
         * bytecode and .xrc artifacts preserve the value the emitter placed in
         * the constant pool instead of degrading it to null. */
        if (!bc_put_u8(w, BC_VAL_BIGINT))
            return false;
        char *digits = xr_bigint_to_string((XrBigInt *) XR_TO_PTR(val));
        if (!digits) {
            w->error = XR_BC_ERR_ALLOC;
            return false;
        }
        bool ok = bc_put_string(w, digits);
        xr_free(digits);
        return ok;
    }
    // Other types not supported yet
    return bc_put_u8(w, BC_VAL_NULL);
}

static char *bc_read_string_or_empty(BcReader *r) {
    return bc_get_string(r);
}

static char *bc_read_optional_string(BcReader *r) {
    uint8_t present = bc_get_u8(r);
    if (r->error != XR_BC_OK || present == 0)
        return NULL;
    if (present != 1) {
        r->error = XR_BC_ERR_CORRUPT;
        return NULL;
    }
    return bc_get_string(r);
}

static bool bc_read_field_descriptor(BcReader *r, XrFieldDescriptorEntry *field) {
    if (!field)
        return false;
    field->name = bc_read_string_or_empty(r);
    field->type_name = bc_read_optional_string(r);
    field->default_value = bc_read_value(r);
    field->flags = bc_get_u16(r);
    return r->error == XR_BC_OK && bc_read_json_decode_schema(r, &field->json_decode_schema, 0);
}

static bool bc_read_method_descriptor(BcReader *r, XrMethodDescriptorEntry *method) {
    if (!method)
        return false;
    method->name = bc_read_string_or_empty(r);
    method->closure_index = bc_get_u32(r);
    method->return_type_name = bc_read_optional_string(r);
    method->param_count = bc_get_u8(r);
    if (r->error != XR_BC_OK)
        return false;
    if (method->param_count > 0) {
        const char **params = xr_calloc(method->param_count, sizeof(char *));
        if (!params) {
            r->error = XR_BC_ERR_ALLOC;
            return false;
        }
        for (uint8_t i = 0; i < method->param_count; i++) {
            params[i] = bc_read_optional_string(r);
            if (r->error != XR_BC_OK)
                return false;
        }
        method->param_type_names = params;
    }
    method->flags = bc_get_u16(r);
    method->op_type = bc_get_u8(r);
    method->is_operator = bc_get_u8(r) != 0;
    return r->error == XR_BC_OK;
}

static XrValue bc_read_class_descriptor(BcReader *r) {
    XrClassDescriptor *desc = xr_calloc(1, sizeof(XrClassDescriptor));
    if (!desc) {
        r->error = XR_BC_ERR_ALLOC;
        return xr_null();
    }

    desc->class_name = bc_read_string_or_empty(r);
    desc->super_name = bc_read_optional_string(r);
    desc->generic_origin_name = bc_read_optional_string(r);
    desc->display_name = bc_read_optional_string(r);
    uint32_t mono_count = bc_get_u32(r);
    if (r->error != XR_BC_OK)
        return xr_null();
    if (mono_count > UINT16_MAX) {
        r->error = XR_BC_ERR_CORRUPT;
        return xr_null();
    }
    desc->mono_type_arg_count = (int) mono_count;
    if (mono_count > 0) {
        const char **names = xr_calloc(mono_count, sizeof(char *));
        if (!names) {
            r->error = XR_BC_ERR_ALLOC;
            return xr_null();
        }
        for (uint32_t i = 0; i < mono_count; i++) {
            names[i] = bc_read_optional_string(r);
            if (r->error != XR_BC_OK)
                return xr_null();
        }
        desc->mono_type_arg_names = names;
    }
    desc->super_global_index = (int32_t) bc_get_u32(r);
    desc->flags = bc_get_u32(r);
    desc->is_monomorphized = bc_get_u8(r) != 0;

    desc->instance_field_count = bc_get_u32(r);
    if (desc->instance_field_count > 0) {
        desc->instance_fields =
            xr_calloc(desc->instance_field_count, sizeof(XrFieldDescriptorEntry));
        if (!desc->instance_fields) {
            r->error = XR_BC_ERR_ALLOC;
            return xr_null();
        }
        for (uint32_t i = 0; i < desc->instance_field_count; i++) {
            if (!bc_read_field_descriptor(r, &desc->instance_fields[i]))
                return xr_null();
        }
    }

    desc->static_field_count = bc_get_u32(r);
    if (desc->static_field_count > 0) {
        desc->static_fields = xr_calloc(desc->static_field_count, sizeof(XrFieldDescriptorEntry));
        if (!desc->static_fields) {
            r->error = XR_BC_ERR_ALLOC;
            return xr_null();
        }
        for (uint32_t i = 0; i < desc->static_field_count; i++) {
            if (!bc_read_field_descriptor(r, &desc->static_fields[i]))
                return xr_null();
        }
    }

    desc->instance_method_count = bc_get_u32(r);
    if (desc->instance_method_count > 0) {
        desc->instance_methods =
            xr_calloc(desc->instance_method_count, sizeof(XrMethodDescriptorEntry));
        if (!desc->instance_methods) {
            r->error = XR_BC_ERR_ALLOC;
            return xr_null();
        }
        for (uint32_t i = 0; i < desc->instance_method_count; i++) {
            if (!bc_read_method_descriptor(r, &desc->instance_methods[i]))
                return xr_null();
        }
    }

    desc->static_method_count = bc_get_u32(r);
    if (desc->static_method_count > 0) {
        desc->static_methods =
            xr_calloc(desc->static_method_count, sizeof(XrMethodDescriptorEntry));
        if (!desc->static_methods) {
            r->error = XR_BC_ERR_ALLOC;
            return xr_null();
        }
        for (uint32_t i = 0; i < desc->static_method_count; i++) {
            if (!bc_read_method_descriptor(r, &desc->static_methods[i]))
                return xr_null();
        }
    }

    desc->interface_count = bc_get_u8(r);
    if (desc->interface_count > 0) {
        desc->interfaces = xr_calloc(desc->interface_count, sizeof(XrInterfaceDescriptorEntry));
        if (!desc->interfaces) {
            r->error = XR_BC_ERR_ALLOC;
            return xr_null();
        }
        for (uint8_t i = 0; i < desc->interface_count; i++) {
            desc->interfaces[i].interface_name = bc_read_optional_string(r);
            desc->interfaces[i].interface_ptr = NULL;
            if (r->error != XR_BC_OK)
                return xr_null();
        }
    }

    desc->clinit_proto_index = (int32_t) bc_get_u32(r);
    desc->descriptor_version = bc_get_u32(r);
    desc->checksum = bc_get_u32(r);
    uint64_t layout_key = bc_get_u64(r);
    if (r->error != XR_BC_OK)
        return xr_null();
    if (layout_key != 0) {
        desc->struct_layout = bc_reader_layout(r, layout_key);
        if (!desc->struct_layout)
            return xr_null();
    }

    XrValue val = {0};
    val.tag = XR_TAG_PTR;
    val.ptr = desc;
    val.heap_type = 0;
    return val;
}

static XrValue bc_read_enum_type(BcReader *r) {
    char *nominal_owner = bc_read_string_or_empty(r);
    char *enum_name = bc_read_string_or_empty(r);
    uint32_t member_count = bc_get_u32(r);
    uint32_t derive_flags = bc_get_u32(r);
    if (r->error != XR_BC_OK) {
        xr_free(nominal_owner);
        xr_free(enum_name);
        return xr_null();
    }
    if (!nominal_owner || nominal_owner[0] == '\0' || !enum_name || enum_name[0] == '\0' ||
        member_count == 0 || member_count > UINT16_MAX) {
        xr_free(nominal_owner);
        xr_free(enum_name);
        r->error = XR_BC_ERR_CORRUPT;
        return xr_null();
    }

    char **member_names = xr_calloc(member_count, sizeof(char *));
    int *payload_counts = xr_calloc(member_count, sizeof(int));
    char ***payload_names = xr_calloc(member_count, sizeof(char **));
    uint8_t **payload_type_ids = xr_calloc(member_count, sizeof(uint8_t *));
    if (!member_names || !payload_counts || !payload_names || !payload_type_ids) {
        xr_free(nominal_owner);
        xr_free(enum_name);
        xr_free(member_names);
        xr_free(payload_counts);
        xr_free(payload_names);
        xr_free(payload_type_ids);
        r->error = XR_BC_ERR_ALLOC;
        return xr_null();
    }

    bool has_payloads = false;
    for (uint32_t i = 0; i < member_count; i++) {
        member_names[i] = bc_read_string_or_empty(r);
        uint16_t payload_count = bc_get_u16(r);
        if (r->error != XR_BC_OK)
            break;
        if (!member_names[i] || member_names[i][0] == '\0') {
            r->error = XR_BC_ERR_CORRUPT;
            break;
        }
        payload_counts[i] = (int) payload_count;
        if (payload_count > 0) {
            has_payloads = true;
            payload_names[i] = xr_calloc(payload_count, sizeof(char *));
            payload_type_ids[i] = xr_calloc(payload_count, sizeof(uint8_t));
            if (!payload_names[i] || !payload_type_ids[i]) {
                r->error = XR_BC_ERR_ALLOC;
                break;
            }
            for (uint16_t p = 0; p < payload_count; p++) {
                payload_names[i][p] = bc_read_string_or_empty(r);
                payload_type_ids[i][p] = bc_get_u8(r);
                if (r->error != XR_BC_OK)
                    break;
                if (!payload_names[i][p] || payload_type_ids[i][p] >= XR_TID_COUNT) {
                    r->error = XR_BC_ERR_CORRUPT;
                    break;
                }
            }
            if (r->error != XR_BC_OK)
                break;
        }
    }

    XrEnumType *enum_type = NULL;
    if (r->error == XR_BC_OK) {
        XrEnumType *canonical = xr_stdlib_enum_type_get(r->X, nominal_owner, enum_name);
        if (canonical) {
            bool shape_matches = canonical->member_count == member_count;
            for (uint32_t i = 0; shape_matches && i < member_count; i++) {
                const char *canonical_name = xr_enum_type_member_name(canonical, i);
                shape_matches = canonical_name && strcmp(canonical_name, member_names[i]) == 0 &&
                                xr_enum_type_payload_count(canonical, i) == payload_counts[i];
            }
            if (!shape_matches) {
                r->error = XR_BC_ERR_CORRUPT;
            } else {
                canonical->derive_flags = derive_flags;
                enum_type = canonical;
            }
        } else if (r->error == XR_BC_OK) {
            enum_type =
                xr_enum_type_new(r->X, nominal_owner, enum_name, member_names, (int) member_count);
            if (!enum_type) {
                r->error = XR_BC_ERR_ALLOC;
            }
        }
        if (enum_type && enum_type != canonical) {
            enum_type->derive_flags = derive_flags;
            if (has_payloads &&
                !xr_enum_type_set_adt_payloads(enum_type, payload_counts, (int) member_count)) {
                r->error = XR_BC_ERR_CORRUPT;
                enum_type = NULL;
            }
            if (enum_type) {
                for (uint32_t i = 0; i < member_count; i++) {
                    if (payload_counts[i] > 0 &&
                        !xr_enum_layout_set_variant_payload_metadata(
                            enum_type->layout, i, (const char *const *) payload_names[i],
                            payload_type_ids[i], (uint16_t) payload_counts[i])) {
                        r->error = XR_BC_ERR_CORRUPT;
                        enum_type = NULL;
                        break;
                    }
                }
            }
        }
    }

    for (uint32_t i = 0; i < member_count; i++) {
        xr_free(member_names[i]);
        for (int p = 0; p < payload_counts[i]; p++)
            xr_free(payload_names[i] ? payload_names[i][p] : NULL);
        xr_free(payload_names[i]);
        xr_free(payload_type_ids[i]);
    }
    xr_free(member_names);
    xr_free(payload_counts);
    xr_free(payload_names);
    xr_free(payload_type_ids);
    xr_free(nominal_owner);
    xr_free(enum_name);

    return (r->error == XR_BC_OK && enum_type) ? XR_FROM_PTR(enum_type) : xr_null();
}

static void bc_dispose_json_decode_schema(XrJsonDecodeSchema *schema) {
    if (!schema)
        return;
    if (schema->child) {
        XrJsonDecodeSchema *child = (XrJsonDecodeSchema *) schema->child;
        bc_dispose_json_decode_schema(child);
        xr_free(child);
    }
    memset(schema, 0, sizeof(*schema));
}

static bool bc_read_json_decode_schema(BcReader *r, XrJsonDecodeSchema *out, int depth) {
    if (!r || !out || depth > 32) {
        if (r)
            r->error = XR_BC_ERR_CORRUPT;
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->value_kind = bc_get_u8(r);
    out->storage_type = bc_get_u8(r);
    uint8_t base = xr_json_value_kind_base(out->value_kind);
    if (r->error != XR_BC_OK || base > XR_JSON_VALUE_CLASS_INSTANCE ||
        out->storage_type >= XR_ELEM_COUNT) {
        if (r->error == XR_BC_OK)
            r->error = XR_BC_ERR_CORRUPT;
        return false;
    }
    if (base == XR_JSON_VALUE_STRUCT_OBJECT) {
        XrValue nested = bc_read_value(r);
        if (r->error != XR_BC_OK || !XR_IS_INT(nested) || XR_TO_INT(nested) == 0) {
            if (r->error == XR_BC_OK)
                r->error = XR_BC_ERR_CORRUPT;
            return false;
        }
        out->target_descriptor = (const void *) (intptr_t) XR_TO_INT(nested);
    } else if (base == XR_JSON_VALUE_ENUM) {
        XrValue enum_type = bc_read_value(r);
        if (r->error != XR_BC_OK || !XR_IS_ENUM_TYPE(enum_type)) {
            if (r->error == XR_BC_OK)
                r->error = XR_BC_ERR_CORRUPT;
            return false;
        }
        out->target_descriptor = XR_TO_PTR(enum_type);
    } else if (base == XR_JSON_VALUE_CLASS_INSTANCE) {
        XrValue class_name = bc_read_value(r);
        if (r->error != XR_BC_OK || !XR_IS_STRING(class_name)) {
            if (r->error == XR_BC_OK)
                r->error = XR_BC_ERR_CORRUPT;
            return false;
        }
        out->target_descriptor = XR_TO_PTR(class_name);
    } else if (base == XR_JSON_VALUE_ARRAY || base == XR_JSON_VALUE_MAP) {
        XrJsonDecodeSchema *child = (XrJsonDecodeSchema *) xr_calloc(1, sizeof(*child));
        if (!child) {
            r->error = XR_BC_ERR_ALLOC;
            return false;
        }
        if (!bc_read_json_decode_schema(r, child, depth + 1)) {
            bc_dispose_json_decode_schema(child);
            xr_free(child);
            return false;
        }
        out->child = child;
    }
    return true;
}

static XrValue bc_read_dynamic_shape(BcReader *r) {
    uint8_t kind = bc_get_u8(r);
    uint8_t sealed_raw = bc_get_u8(r);
    uint32_t count = bc_get_u32(r);
    if (r->error != XR_BC_OK)
        return xr_null();
    if (count > UINT16_MAX || (kind != BC_SHAPE_JSON && kind != BC_SHAPE_STRUCT_OBJECT &&
                               kind != BC_SHAPE_JSON_DECODE_ROOT)) {
        r->error = XR_BC_ERR_CORRUPT;
        return xr_null();
    }

    char **names = NULL;
    uint8_t *json_value_kinds = NULL;
    XrClass **json_struct_object_classes = NULL;
    XrJsonDecodeSchema *json_decode_schemas = NULL;
    uint64_t *stable_type_keys = NULL;
    uint8_t *shape_field_flags = NULL;
    if (count > 0) {
        names = xr_malloc(sizeof(char *) * (size_t) count);
        json_value_kinds = xr_malloc((size_t) count);
        json_struct_object_classes = xr_calloc((size_t) count, sizeof(XrClass *));
        json_decode_schemas = xr_calloc((size_t) count, sizeof(XrJsonDecodeSchema));
        stable_type_keys = xr_malloc((size_t) count * sizeof(uint64_t));
        shape_field_flags = xr_malloc((size_t) count);
        if (!names || !json_value_kinds || !json_struct_object_classes || !json_decode_schemas ||
            !stable_type_keys || !shape_field_flags) {
            r->error = XR_BC_ERR_ALLOC;
            goto fail;
        }
        memset(names, 0, sizeof(char *) * (size_t) count);
    }

    for (uint32_t i = 0; i < count; i++) {
        names[i] = bc_get_string(r);
        if (r->error != XR_BC_OK)
            goto fail;
        if (!names[i]) {
            names[i] = xr_malloc(1);
            if (!names[i]) {
                r->error = XR_BC_ERR_ALLOC;
                goto fail;
            }
            names[i][0] = '\0';
        }
        stable_type_keys[i] = bc_get_u64(r);
        shape_field_flags[i] = bc_get_u8(r);
        if (r->error != XR_BC_OK ||
            (shape_field_flags[i] &
             ~(XR_OBJECT_SHAPE_FIELD_READONLY | XR_OBJECT_SHAPE_FIELD_OPTIONAL)) != 0) {
            if (r->error == XR_BC_OK)
                r->error = XR_BC_ERR_CORRUPT;
            goto fail;
        }
        if (!bc_read_json_decode_schema(r, &json_decode_schemas[i], 0))
            goto fail;
        json_value_kinds[i] = json_decode_schemas[i].value_kind;
        if (xr_json_value_kind_base(json_value_kinds[i]) == XR_JSON_VALUE_STRUCT_OBJECT)
            json_struct_object_classes[i] = (XrClass *) json_decode_schemas[i].target_descriptor;
    }

    XrClass *cls = NULL;
    bool sealed = sealed_raw != 0;
    if (kind == BC_SHAPE_JSON) {
        cls = xr_class_build_json_chain(r->X, (const char *const *) names, (int) count,
                                        stable_type_keys, shape_field_flags, sealed);
    } else {
        if (!sealed) {
            r->error = XR_BC_ERR_CORRUPT;
            cls = NULL;
        } else {
            cls = xr_class_build_struct_object_chain(
                r->X, (const char *const *) names, json_value_kinds, (int) count,
                json_struct_object_classes, json_decode_schemas, stable_type_keys,
                shape_field_flags);
            if (cls && kind == BC_SHAPE_JSON_DECODE_ROOT) {
                if (count != 1) {
                    r->error = XR_BC_ERR_CORRUPT;
                    cls = NULL;
                } else {
                    cls->flags |= XR_CLASS_JSON_DECODE_ROOT;
                }
            }
        }
    }

    for (uint32_t i = 0; i < count; i++)
        xr_free(names[i]);
    xr_free(names);
    xr_free(json_value_kinds);
    xr_free(json_struct_object_classes);
    for (uint32_t i = 0; i < count; i++)
        bc_dispose_json_decode_schema(&json_decode_schemas[i]);
    xr_free(json_decode_schemas);
    xr_free(stable_type_keys);
    xr_free(shape_field_flags);

    if (!cls) {
        r->error = XR_BC_ERR_ALLOC;
        return xr_null();
    }
    return xr_int((int64_t) (intptr_t) cls);

fail:
    if (names) {
        for (uint32_t i = 0; i < count; i++)
            xr_free(names[i]);
    }
    if (json_decode_schemas) {
        for (uint32_t i = 0; i < count; i++)
            bc_dispose_json_decode_schema(&json_decode_schemas[i]);
    }
    xr_free(names);
    xr_free(json_value_kinds);
    xr_free(json_struct_object_classes);
    xr_free(json_decode_schemas);
    xr_free(stable_type_keys);
    xr_free(shape_field_flags);
    return xr_null();
}

static XrValue bc_read_value(BcReader *r) {
    uint8_t type = bc_get_u8(r);
    if (r->error != XR_BC_OK)
        return xr_null();

    switch (type) {
        case BC_VAL_NULL:
            return xr_null();
        case BC_VAL_BOOL:
            return xr_bool(bc_get_u8(r) != 0);
        case BC_VAL_INT:
            return xr_int(bc_get_i64(r));
        case BC_VAL_RUNE:
            return xr_rune(bc_get_u32(r));
        case BC_VAL_FLOAT:
            return xr_float(bc_get_f64(r));
        case BC_VAL_STRING: {
            uint32_t len = 0;
            char *str = bc_get_string_data(r, &len);
            if (r->error != XR_BC_OK)
                return xr_null();
            /* Bytecode literals are module-lifetime constants.  Re-enter the
             * permanent compiler pool so embedded-NUL/raw byte payloads remain
             * valid and pool sweeps cannot invalidate proto constants. */
            XrString *s = xr_string_intern_permanent(r->X, str, len);
            xr_free(str);
            return s ? xr_string_value(s) : xr_null();
        }
        case BC_VAL_BIGINT: {
            char *digits = bc_get_string(r);
            if (r->error != XR_BC_OK)
                return xr_null();
            /* Rebuild on the compiler fixed heap so the constant lives for the
             * module lifetime, matching how the emitter materializes BigInt
             * literals.  The class pointer is reattached below because the
             * fixed-heap constructor intentionally leaves it null. */
            if (!digits || !*digits) {
                /* An empty payload cannot come from the writer, which always
                 * emits at least one digit; the stream is corrupt. */
                xr_free(digits);
                r->error = XR_BC_ERR_CORRUPT;
                return xr_null();
            }
            XrBigInt *bi =
                xr_bigint_from_string_on_fixed_heap(xr_isolate_get_fixed_heap(r->X), digits);
            xr_free(digits);
            if (!bi) {
                r->error = XR_BC_ERR_ALLOC;
                return xr_null();
            }
            XrayCoreClasses *core = xr_isolate_get_core_classes(r->X);
            if (core)
                bi->klass = core->bigintClass;
            return XR_FROM_PTR(bi);
        }
        case BC_VAL_DYNAMIC_SHAPE:
            return bc_read_dynamic_shape(r);
        case BC_VAL_CLASS_DESCRIPTOR:
            return bc_read_class_descriptor(r);
        case BC_VAL_ENUM_TYPE:
            return bc_read_enum_type(r);
        default:
            r->error = XR_BC_ERR_CORRUPT;
            return xr_null();
    }
}

/* ========== Symbol Table Serialization ========== */

// Collect global symbol IDs from per-function symbol table, returns max_symbol_id + 1
static int collect_symbols_from_proto(XrProto *proto, int max_symbol) {
    if (!proto)
        return max_symbol;

    // Scan per-function symbol table (no need to scan instructions)
    for (int i = 0; i < proto->symbol_count; i++) {
        int32_t sym = proto->symbols[i];
        if (sym >= max_symbol) {
            max_symbol = sym + 1;
        }
    }

    // Recursively process nested Protos
    uint32_t sub_count = (uint32_t) PROTO_PROTO_COUNT(proto);
    for (uint32_t i = 0; i < sub_count; i++) {
        XrProto *sub = PROTO_PROTO(proto, i);
        max_symbol = collect_symbols_from_proto(sub, max_symbol);
    }

    return max_symbol;
}

// Remap global symbol IDs in per-function symbol table
static void remap_symbols_in_proto(XrProto *proto, int *id_map, int map_size) {
    if (!proto)
        return;

    // Remap per-function symbol table entries (instructions are untouched)
    for (int i = 0; i < proto->symbol_count; i++) {
        int32_t old_id = proto->symbols[i];
        if (old_id >= 0 && old_id < map_size && id_map[old_id] >= 0) {
            proto->symbols[i] = id_map[old_id];
        }
    }

    // Recursively process nested Protos
    uint32_t sub_count = (uint32_t) PROTO_PROTO_COUNT(proto);
    for (uint32_t i = 0; i < sub_count; i++) {
        XrProto *sub = PROTO_PROTO(proto, i);
        remap_symbols_in_proto(sub, id_map, map_size);
    }
}

/* ========== Shared Variable Index Remapping ========== */

// Collect max shared index used in Proto, returns max_shared_index + 1
static int collect_shared_from_proto(XrProto *proto, int max_shared) {
    if (!proto)
        return max_shared;

    uint32_t code_count = (uint32_t) PROTO_CODE_COUNT(proto);
    for (uint32_t i = 0; i < code_count; i++) {
        XrInstruction inst = PROTO_CODE(proto, i);
        OpCode op = GET_OPCODE(inst);

        // Check opcodes that use shared index
        if (op == OP_GETSHARED || op == OP_SETSHARED) {
            int shared_idx = GETARG_Bx(inst);
            if (shared_idx >= max_shared) {
                max_shared = shared_idx + 1;
            }
        }
    }

    // Recursively process nested Protos
    uint32_t sub_count = (uint32_t) PROTO_PROTO_COUNT(proto);
    for (uint32_t i = 0; i < sub_count; i++) {
        XrProto *sub = PROTO_PROTO(proto, i);
        max_shared = collect_shared_from_proto(sub, max_shared);
    }

    return max_shared;
}

/* ========== Proto Serialization ========== */

static bool bc_write_proto(BcWriter *w, XrProto *proto);
static XrProto *bc_read_proto_depth(BcReader *r, int depth);

#define BC_MAX_NESTING_DEPTH 64

static bool *bc_collect_dynamic_shape_constants(XrProto *proto, uint32_t const_count) {
    if (const_count == 0)
        return NULL;

    bool *shape_consts = xr_calloc(const_count, sizeof(bool));
    if (!shape_consts)
        return NULL;

    uint32_t code_count = (uint32_t) PROTO_CODE_COUNT(proto);
    for (uint32_t i = 0; i < code_count; i++) {
        XrInstruction inst = PROTO_CODE(proto, i);
        OpCode op = GET_OPCODE(inst);
        int kidx = -1;
        if (op == OP_NEWJSON) {
            kidx = GETARG_B(inst);
        } else if (op == OP_JSON_DECODE || op == OP_JSON_PARSE_TYPED) {
            kidx = GETARG_C(inst);
        }
        if (kidx >= 0 && (uint32_t) kidx < const_count) {
            shape_consts[kidx] = true;
        }
    }
    return shape_consts;
}

static bool *bc_collect_class_descriptor_constants(XrProto *proto, uint32_t const_count) {
    if (const_count == 0)
        return NULL;

    bool *class_consts = xr_calloc(const_count, sizeof(bool));
    if (!class_consts)
        return NULL;

    uint32_t code_count = (uint32_t) PROTO_CODE_COUNT(proto);
    for (uint32_t i = 0; i < code_count; i++) {
        XrInstruction inst = PROTO_CODE(proto, i);
        if (GET_OPCODE(inst) != OP_CLASS_CREATE_FROM_DESCRIPTOR)
            continue;
        int kidx = GETARG_Bx(inst);
        if (kidx >= 0 && (uint32_t) kidx < const_count)
            class_consts[kidx] = true;
    }
    return class_consts;
}

static bool bc_collect_layouts_from_proto(BcWriter *w, XrProto *proto, uint32_t depth) {
    if (!w || !proto || depth > BC_MAX_NESTING_DEPTH) {
        if (w)
            w->error = XR_BC_ERR_METADATA;
        return false;
    }
    uint32_t const_count = (uint32_t) PROTO_CONST_COUNT(proto);
    bool *class_consts = bc_collect_class_descriptor_constants(proto, const_count);
    if (const_count > 0 && !class_consts) {
        w->error = XR_BC_ERR_ALLOC;
        return false;
    }
    for (uint32_t i = 0; i < const_count; i++) {
        if (!class_consts[i])
            continue;
        XrValue value = PROTO_CONSTANT(proto, i);
        if (!XR_IS_PTR(value) || !XR_TO_PTR(value)) {
            xr_free(class_consts);
            w->error = XR_BC_ERR_METADATA;
            return false;
        }
        const XrClassDescriptor *desc = (const XrClassDescriptor *) XR_TO_PTR(value);
        if (desc->struct_layout && !bc_writer_add_layout(w, desc->struct_layout, 0)) {
            xr_free(class_consts);
            return false;
        }
    }
    xr_free(class_consts);
    for (uint32_t i = 0; i < (uint32_t) PROTO_PROTO_COUNT(proto); i++) {
        if (!bc_collect_layouts_from_proto(w, PROTO_PROTO(proto, i), depth + 1))
            return false;
    }
    return true;
}

static bool bc_write_proto(BcWriter *w, XrProto *proto) {
    if (!proto)
        return false;

    // 1. Function name
    const char *name = proto->name ? proto->name->data : "";
    if (!bc_put_string(w, name))
        return false;

    // 2. Source file (optional)
    if (w->flags & XR_BC_STRIP_SOURCE) {
        if (!bc_put_string(w, ""))
            return false;
    } else {
        if (!bc_put_string(w, proto->source_file))
            return false;
    }

    // 3. Function attributes
    if (!bc_put_u32(w, proto->numparams))
        return false;
    if (!bc_put_u32(w, proto->maxstacksize))
        return false;
    if (!bc_put_u32(w, proto->num_globals))
        return false;
    if (!bc_put_u32(w, proto->struct_area_size))
        return false;
    if (!bc_put_u8(w, proto->is_vararg ? 1 : 0))
        return false;
    if (!bc_put_u8(w, proto->is_coro_safe ? 1 : 0))
        return false;

    // 3a. Canonical reachable-runtime entry contract. Child protos carry an
    // empty plan; the module root carries the verified plan consumed by VM.
    if (!bc_put_u32(w, proto->entry_plan.entry_func_id) ||
        !bc_put_u32(w, proto->entry_plan.reachable_body_count) ||
        !bc_put_u32(w, proto->entry_plan.reachable_effect_bits) ||
        !bc_put_u32(w, proto->entry_plan.required_capability_bits) ||
        !bc_put_u32(w, proto->entry_plan.provided_capability_bits) ||
        !bc_put_u32(w, proto->entry_plan.runtime_component_bits) ||
        !bc_put_u32(w, proto->entry_plan.provider_hook_bits) ||
        !bc_put_u32(w, proto->entry_plan.evidence) ||
        !bc_put_u8(w, proto->entry_plan.root_representation) ||
        !bc_put_u8(w, proto->entry_plan.scheduler_mode) ||
        !bc_put_u8(w, proto->entry_plan.unproven_reason))
        return false;

    // 3b. FFI extern signature (self-contained; the Xi IR is not serialized,
    // so the embedded-bytecode VM resolves the C symbol from here). The flag
    // doubles as "a signature follows" so a malformed extern proto without a
    // sig stays byte-aligned.
    {
        XrFFISig *sig = (proto->is_extern && proto->ffi_sig) ? proto->ffi_sig : NULL;
        if (!bc_put_u8(w, sig ? 1 : 0))
            return false;
        if (sig) {
            if (!bc_put_string(w, sig->symbol))
                return false;
            if (!bc_put_string(w, sig->dylib))
                return false;
            if (!bc_put_u8(w, sig->nparams))
                return false;
            for (uint8_t i = 0; i < sig->nparams; i++) {
                if (!bc_put_u8(w, sig->params[i]))
                    return false;
                XrFFICallbackSig *cb = sig->param_cbacks ? sig->param_cbacks[i] : NULL;
                if (!bc_put_u8(w, cb ? 1 : 0))
                    return false;
                if (cb) {
                    if (!bc_put_u8(w, cb->nparams))
                        return false;
                    for (uint8_t ci = 0; ci < cb->nparams; ci++) {
                        if (!bc_put_u8(w, cb->params[ci]))
                            return false;
                    }
                    if (!bc_put_u8(w, cb->ret))
                        return false;
                }
            }
            if (!bc_put_u8(w, sig->ret))
                return false;
        }
    }

    // 4. Bytecode
    uint32_t code_count = (uint32_t) PROTO_CODE_COUNT(proto);
    if (!bc_put_u32(w, code_count))
        return false;
    for (uint32_t i = 0; i < code_count; i++) {
        XrInstruction inst = PROTO_CODE(proto, i);
        if (!bc_put_u64(w, inst))
            return false;
    }

    // 5. Constants
    uint32_t const_count = (uint32_t) PROTO_CONST_COUNT(proto);
    if (!bc_put_u32(w, const_count))
        return false;
    bool *shape_consts = bc_collect_dynamic_shape_constants(proto, const_count);
    bool *class_consts = bc_collect_class_descriptor_constants(proto, const_count);
    if (const_count > 0 && (!shape_consts || !class_consts)) {
        xr_free(shape_consts);
        xr_free(class_consts);
        return false;
    }
    for (uint32_t i = 0; i < const_count; i++) {
        XrValue val = PROTO_CONSTANT(proto, i);
        if (!bc_write_value(w, val, shape_consts[i], class_consts[i])) {
            xr_free(shape_consts);
            xr_free(class_consts);
            return false;
        }
    }
    xr_free(shape_consts);
    xr_free(class_consts);

    // 6. Line info (optional)
    if (w->flags & XR_BC_STRIP_DEBUG) {
        if (!bc_put_u32(w, 0))
            return false;
    } else {
        uint32_t line_count = (uint32_t) PROTO_LINE_COUNT(proto);
        if (!bc_put_u32(w, line_count))
            return false;
        for (uint32_t i = 0; i < line_count; i++) {
            if (!bc_put_u32(w, PROTO_LINE(proto, i)))
                return false;
        }
    }

    // 7. Upvalue info
    uint32_t upval_count = (uint32_t) PROTO_UPVAL_COUNT(proto);
    if (!bc_put_u32(w, upval_count))
        return false;
    for (uint32_t i = 0; i < upval_count; i++) {
        UpvalInfo info = PROTO_UPVALUE(proto, i);
        if (!bc_put_u16(w, info.index))
            return false;
        if (!bc_put_u8(w, info.source))
            return false;
        if (!bc_put_u8(w, info.storage_mode))
            return false;
        if (!bc_put_u8(w, info.is_const))
            return false;
        if (!bc_put_u8(w, info.slot_type))
            return false;
        if (!bc_put_u8(w, info.capture_action))
            return false;
    }

    // 8. Nested Protos
    uint32_t sub_count = (uint32_t) PROTO_PROTO_COUNT(proto);
    if (!bc_put_u32(w, sub_count))
        return false;
    for (uint32_t i = 0; i < sub_count; i++) {
        XrProto *sub = PROTO_PROTO(proto, i);
        if (!bc_write_proto(w, sub))
            return false;
    }

    // 9. Per-function symbol table
    if (!bc_put_u32(w, (uint32_t) proto->symbol_count))
        return false;
    for (int i = 0; i < proto->symbol_count; i++) {
        if (!bc_put_u32(w, (uint32_t) proto->symbols[i]))
            return false;
    }

    return true;
}

static XrProto *bc_read_proto_depth(BcReader *r, int depth) {
    if (depth > BC_MAX_NESTING_DEPTH) {
        r->error = XR_BC_ERR_CORRUPT;
        return NULL;
    }
    // Allocate Proto through the canonical constructor so runtime metadata
    // such as proto_id stays unique for inline-cache indexing.
    XrProto *proto = xr_vm_proto_new();
    if (!proto) {
        r->error = XR_BC_ERR_ALLOC;
        return NULL;
    }

    // 1. Function name
    char *name = bc_get_string(r);
    if (r->error != XR_BC_OK)
        goto fail;
    if (name && name[0]) {
        proto->name = xr_string_intern(r->X, name, strlen(name), 0);
        xr_free(name);
    }

    // 2. Source file
    char *source = bc_get_string(r);
    if (r->error != XR_BC_OK)
        goto fail;
    if (source && source[0]) {
        proto->source_file = source;
    } else {
        xr_free(source);
    }

    // 3. Function attributes
    proto->numparams = bc_get_u32(r);
    proto->maxstacksize = bc_get_u32(r);
    proto->num_globals = bc_get_u32(r);
    uint32_t struct_area_size = bc_get_u32(r);
    proto->struct_area_size = struct_area_size;
    proto->is_vararg = bc_get_u8(r) != 0;
    proto->is_coro_safe = bc_get_u8(r) != 0;
    if (r->error != XR_BC_OK)
        goto fail;

    // 3a. Canonical reachable-runtime entry contract
    proto->entry_plan.entry_func_id = bc_get_u32(r);
    proto->entry_plan.reachable_body_count = bc_get_u32(r);
    proto->entry_plan.reachable_effect_bits = bc_get_u32(r);
    proto->entry_plan.required_capability_bits = bc_get_u32(r);
    proto->entry_plan.provided_capability_bits = bc_get_u32(r);
    proto->entry_plan.runtime_component_bits = bc_get_u32(r);
    proto->entry_plan.provider_hook_bits = bc_get_u32(r);
    proto->entry_plan.evidence = bc_get_u32(r);
    proto->entry_plan.root_representation = bc_get_u8(r);
    proto->entry_plan.scheduler_mode = bc_get_u8(r);
    proto->entry_plan.unproven_reason = bc_get_u8(r);
    if (r->error != XR_BC_OK)
        goto fail;

    // 3b. FFI extern signature
    {
        uint8_t has_ffi = bc_get_u8(r);
        if (r->error != XR_BC_OK)
            goto fail;
        if (has_ffi) {
            char *sym = bc_get_string(r);
            char *dylib = bc_get_string(r);
            uint8_t np = bc_get_u8(r);
            if (r->error != XR_BC_OK) {
                xr_free(sym);
                xr_free(dylib);
                goto fail;
            }
            XrFFISig *sig = xr_ffi_sig_new(sym ? sym : "", dylib, np);
            xr_free(sym);
            xr_free(dylib);
            if (!sig) {
                r->error = XR_BC_ERR_ALLOC;
                goto fail;
            }
            for (uint8_t i = 0; i < np; i++) {
                sig->params[i] = bc_get_u8(r);
                uint8_t has_cb = bc_get_u8(r);
                if (r->error != XR_BC_OK) {
                    xr_ffi_sig_free(sig);
                    goto fail;
                }
                if (has_cb) {
                    uint8_t cb_np = bc_get_u8(r);
                    uint8_t cb_stack[16];
                    uint8_t *cb_params = cb_np <= 16 ? cb_stack : xr_malloc(cb_np);
                    if (cb_np > 0 && !cb_params) {
                        xr_ffi_sig_free(sig);
                        r->error = XR_BC_ERR_ALLOC;
                        goto fail;
                    }
                    for (uint8_t ci = 0; ci < cb_np; ci++)
                        cb_params[ci] = bc_get_u8(r);
                    uint8_t cb_ret = bc_get_u8(r);
                    if (r->error != XR_BC_OK) {
                        if (cb_params != cb_stack)
                            xr_free(cb_params);
                        xr_ffi_sig_free(sig);
                        goto fail;
                    }
                    bool ok = xr_ffi_sig_set_param_callback_codes(sig, i, cb_params, cb_np, cb_ret);
                    if (cb_params != cb_stack)
                        xr_free(cb_params);
                    if (!ok) {
                        xr_ffi_sig_free(sig);
                        r->error = XR_BC_ERR_ALLOC;
                        goto fail;
                    }
                }
            }
            sig->ret = bc_get_u8(r);
            if (r->error != XR_BC_OK) {
                xr_ffi_sig_free(sig);
                goto fail;
            }
            proto->ffi_sig = sig;
            proto->is_extern = true;
        }
    }

    // 4. Bytecode
    uint32_t code_count = bc_get_u32(r);
    if (r->error != XR_BC_OK)
        goto fail;
    for (uint32_t i = 0; i < code_count; i++) {
        XrInstruction inst = bc_get_u64(r);
        if (r->error != XR_BC_OK)
            goto fail;
        DYNARRAY_ADD(&proto->code, inst, XrInstruction);
    }

    // 5. Constants
    uint32_t const_count = bc_get_u32(r);
    if (r->error != XR_BC_OK)
        goto fail;
    for (uint32_t i = 0; i < const_count; i++) {
        XrValue val = bc_read_value(r);
        if (r->error != XR_BC_OK)
            goto fail;
        DYNARRAY_ADD(&proto->constants, val, XrValue);
    }

    // 6. Line info
    uint32_t line_count = bc_get_u32(r);
    if (r->error != XR_BC_OK)
        goto fail;
    for (uint32_t i = 0; i < line_count; i++) {
        int line = (int) bc_get_u32(r);
        if (r->error != XR_BC_OK)
            goto fail;
        DYNARRAY_ADD(&proto->lineinfo, line, int);
    }

    // 7. Upvalue info
    uint32_t upval_count = bc_get_u32(r);
    if (r->error != XR_BC_OK)
        goto fail;
    for (uint32_t i = 0; i < upval_count; i++) {
        UpvalInfo info = {0};
        info.index = bc_get_u16(r);
        info.source = bc_get_u8(r);
        info.storage_mode = bc_get_u8(r);
        info.is_const = bc_get_u8(r);
        info.slot_type = bc_get_u8(r);
        info.capture_action = bc_get_u8(r);
        if (r->error != XR_BC_OK)
            goto fail;
        DYNARRAY_ADD(&proto->upvalues, info, UpvalInfo);
    }

    // 8. Nested Protos
    uint32_t sub_count = bc_get_u32(r);
    if (r->error != XR_BC_OK)
        goto fail;
    for (uint32_t i = 0; i < sub_count; i++) {
        XrProto *sub = bc_read_proto_depth(r, depth + 1);
        if (!sub)
            goto fail;
        xr_vm_proto_add_proto(proto, sub);
    }

    // 9. Per-function symbol table
    uint32_t sym_count = bc_get_u32(r);
    if (r->error != XR_BC_OK)
        goto fail;
    if (sym_count > 0) {
        proto->symbols = xr_malloc(sym_count * sizeof(int32_t));
        if (!proto->symbols) {
            r->error = XR_BC_ERR_ALLOC;
            goto fail;
        }
        proto->symbol_count = (int) sym_count;
        proto->symbol_capacity = (int) sym_count;
        for (uint32_t i = 0; i < sym_count; i++) {
            proto->symbols[i] = (int32_t) bc_get_u32(r);
            if (r->error != XR_BC_OK)
                goto fail;
        }
    }

    return proto;

fail:
    xr_vm_proto_free(proto);
    return NULL;
}

/* ========== Public API ========== */

const char *xr_bytecode_error_string(XrBcError error) {
    switch (error) {
        case XR_BC_OK:
            return "ok";
        case XR_BC_ERR_MAGIC:
            return "invalid bytecode magic";
        case XR_BC_ERR_VERSION:
            return "unsupported bytecode version";
        case XR_BC_ERR_TRUNCATED:
            return "truncated bytecode payload";
        case XR_BC_ERR_CORRUPT:
            return "corrupt bytecode metadata";
        case XR_BC_ERR_ALLOC:
            return "out of memory";
        case XR_BC_ERR_METADATA:
            return "unsupported or inconsistent bytecode metadata";
        case XR_BC_ERR_TARGET_ABI:
            return "bytecode aggregate layout target ABI mismatch";
        default:
            return "unknown bytecode error";
    }
}

static uint8_t *bytecode_write_impl(XrVMRuntime *X, const char *stdlib_module, XrProto *proto,
                                    int flags, size_t *out_size, XrBcError *error) {
    if (error)
        *error = XR_BC_OK;
    if (!X || !proto || !out_size) {
        if (error)
            *error = XR_BC_ERR_METADATA;
        return NULL;
    }
    *out_size = 0;
    if (!xr_vm_entry_plan_validate(proto) && !xr_vm_entry_plan_derive(proto)) {
        if (error)
            *error = XR_BC_ERR_METADATA;
        return NULL;
    }

    BcWriter w;
    bc_writer_init(&w, X, stdlib_module, flags);

    if (!bc_collect_layouts_from_proto(&w, proto, 0))
        goto fail;
    if (w.layout_count > 1)
        qsort(w.layouts, w.layout_count, sizeof(*w.layouts), bc_layout_entry_compare);

    // Collect symbols (symbol ID starts from 1, returns max ID + 1)
    int max_symbol_id = collect_symbols_from_proto(proto, 0);

    int used_shared_count = collect_shared_from_proto(proto, 0);
    int shared_count =
        proto->shared_count > used_shared_count ? proto->shared_count : used_shared_count;

    // Write header
    if (!bc_put_u32(&w, XR_BC_MAGIC))
        goto fail;
    if (!bc_put_u16(&w, XR_BC_VERSION))
        goto fail;
    if (!bc_put_u16(&w, (uint16_t) flags))
        goto fail;
    if (!bc_put_u32(&w, 1))
        goto fail;  // proto count
    if (!bc_put_u32(&w, (uint32_t) max_symbol_id))
        goto fail;  // max symbol id
    if (!bc_put_u32(&w, (uint32_t) shared_count))
        goto fail;  // shared count
    if (!bc_write_layout_table(&w))
        goto fail;

    // Write symbol table (symbol ID starts from 1)
    for (int i = 1; i <= max_symbol_id; i++) {
        const char *name = xr_symbol_get_name_by_id(X, i);
        if (!bc_put_string(&w, name ? name : ""))
            goto fail;
    }

    // Write Proto
    if (!bc_write_proto(&w, proto))
        goto fail;

    *out_size = w.size;
    xr_free(w.layouts);
    if (error)
        *error = XR_BC_OK;
    return w.buf;

fail:
    xr_free(w.buf);
    xr_free(w.layouts);
    if (error)
        *error = w.error == XR_BC_OK ? XR_BC_ERR_METADATA : w.error;
    return NULL;
}

uint8_t *xr_bytecode_write(XrVMRuntime *X, XrProto *proto, int flags, size_t *out_size,
                           XrBcError *error) {
    return bytecode_write_impl(X, NULL, proto, flags, out_size, error);
}

uint8_t *xr_bytecode_write_stdlib(XrVMRuntime *X, const char *canonical_module, XrProto *proto,
                                  int flags, size_t *out_size, XrBcError *error) {
    if (!canonical_module || !canonical_module[0]) {
        if (error)
            *error = XR_BC_ERR_METADATA;
        if (out_size)
            *out_size = 0;
        return NULL;
    }
    return bytecode_write_impl(X, canonical_module, proto, flags, out_size, error);
}

XrProto *xr_bytecode_read(XrVMRuntime *X, const uint8_t *data, size_t size, XrBcError *error) {
    if (!X || !data || size == 0) {
        if (error)
            *error = XR_BC_ERR_TRUNCATED;
        return NULL;
    }

    BcReader r;
    bc_reader_init(&r, X, data, size);

    // Read header
    uint32_t magic = bc_get_u32(&r);
    if (r.error != XR_BC_OK) {
        if (error)
            *error = r.error;
        return NULL;
    }
    if (magic != XR_BC_MAGIC) {
        if (error)
            *error = XR_BC_ERR_MAGIC;
        return NULL;
    }

    uint16_t version = bc_get_u16(&r);
    if (r.error != XR_BC_OK) {
        if (error)
            *error = r.error;
        return NULL;
    }
    if (version != XR_BC_VERSION) {
        if (error)
            *error = XR_BC_ERR_VERSION;
        return NULL;
    }

    bc_get_u16(&r);                           // flags
    uint32_t proto_count = bc_get_u32(&r);    // proto count
    uint32_t max_symbol_id = bc_get_u32(&r);  // max symbol id
    uint32_t shared_count = bc_get_u32(&r);   // shared count

    if (r.error != XR_BC_OK) {
        if (error)
            *error = r.error;
        return NULL;
    }

    if (proto_count != 1 || shared_count > (uint32_t) INT_MAX ||
        max_symbol_id >= (uint32_t) INT_MAX) {
        if (error)
            *error = XR_BC_ERR_CORRUPT;
        return NULL;
    }

    if (!bc_read_layout_table(&r)) {
        if (error)
            *error = r.error;
        bc_reader_layout_table_dispose(&r);
        return NULL;
    }

    // Read symbol table and build mapping (symbol ID starts from 1)
    int *id_map = NULL;
    int map_size = (int) max_symbol_id + 1;
    if (max_symbol_id > 0) {
        id_map = xr_malloc(map_size * sizeof(int));
        if (!id_map) {
            if (error)
                *error = XR_BC_ERR_ALLOC;
            bc_reader_layout_table_dispose(&r);
            return NULL;
        }
        memset(id_map, -1, map_size * sizeof(int));

        for (uint32_t i = 1; i <= max_symbol_id; i++) {
            char *name = bc_get_string(&r);
            if (r.error != XR_BC_OK) {
                xr_free(id_map);
                if (error)
                    *error = r.error;
                bc_reader_layout_table_dispose(&r);
                return NULL;
            }
            if (name && name[0]) {
                id_map[i] = xr_symbol_register_in_table(xr_isolate_get_symbol_table(X), name);
                xr_free(name);
            } else {
                id_map[i] = -1;
                xr_free(name);
            }
        }
    }

    // Read Proto
    XrProto *proto = bc_read_proto_depth(&r, 0);
    if (!proto && r.error == XR_BC_OK)
        r.error = XR_BC_ERR_CORRUPT;

    // Remap symbol IDs
    if (proto && id_map) {
        remap_symbols_in_proto(proto, id_map, map_size);
    }
    if (proto)
        proto->shared_count = (int) shared_count;
    if (proto && !xr_vm_entry_plan_validate(proto)) {
        xr_vm_proto_free(proto);
        proto = NULL;
        r.error = XR_BC_ERR_CORRUPT;
    }
    if (proto && r.pos != r.size) {
        xr_vm_proto_free(proto);
        proto = NULL;
        r.error = XR_BC_ERR_CORRUPT;
    }

    xr_free(id_map);
    bc_reader_layout_table_dispose(&r);
    if (error)
        *error = r.error;
    return proto;
}

int xr_eval_bytecode(XrVMRuntime *X, const uint8_t *data, size_t size) {
    XR_DCHECK(X != NULL, "eval_bytecode: NULL isolate");
    XR_DCHECK(data != NULL, "eval_bytecode: NULL data");
    XrBcError error;
    XrProto *proto = xr_bytecode_read(X, data, size, &error);
    if (!proto) {
        xr_log_warning("bytecode", "failed to load: %s", xr_bytecode_error_string(error));
        return -1;
    }

    // Use xr_execute which properly initializes coroutine and runtime
    int result = xr_execute(X, proto);
    xr_vm_proto_free(proto);
    return result;
}

/* ========== AOT Bytecode Load (decomposed API) ========== */

XrProto *xr_bytecode_load(XrVMRuntime *X, const uint8_t *data, size_t size) {
    XR_DCHECK(X != NULL, "bytecode_load: NULL isolate");
    XR_DCHECK(data != NULL, "bytecode_load: NULL data");
    XrBcError error;
    XrProto *proto = xr_bytecode_read(X, data, size, &error);
    if (!proto) {
        xr_log_warning("bytecode", "failed to load: %s", xr_bytecode_error_string(error));
        return NULL;
    }
    return proto;
}

/* ========== AOT Registration Helpers ========== */

const char *xr_proto_name(XrProto *p) {
    if (!p || !p->name)
        return NULL;
    return XR_STRING_CHARS(p->name);
}

XrProto **xr_proto_children(XrProto *p, int *count) {
    if (!p) {
        *count = 0;
        return NULL;
    }
    *count = PROTO_PROTO_COUNT(p);
    if (*count == 0)
        return NULL;
    return (XrProto **) p->protos.data;
}

void xr_proto_set_param_types(XrProto *p, const uint8_t *ptypes, int nparams, uint8_t return_type) {
    if (!p)
        return;
    p->return_type_info = xr_slot_type_to_type(NULL, return_type);
    if (nparams > 0 && ptypes && !p->param_types) {
        p->param_types = (struct XrType **) xr_calloc(nparams, sizeof(struct XrType *));
        if (p->param_types) {
            p->param_types_count = nparams;
            for (int i = 0; i < nparams; i++) {
                if (ptypes[i] == XR_SLOT_I64)
                    p->param_types[i] = xr_type_new_int(NULL);
                else if (ptypes[i] == XR_SLOT_F64)
                    p->param_types[i] = xr_type_new_float(NULL);
                else if (ptypes[i] == XR_SLOT_BOOL)
                    p->param_types[i] = xr_type_new_bool(NULL);
            }
        }
    }
}

int xr_run_bytecode_file(XrVMRuntime *X, const char *bytecode_file) {
    XR_DCHECK(X != NULL, "run_bytecode_file: NULL isolate");
    XR_DCHECK(bytecode_file != NULL, "run_bytecode_file: NULL bytecode_file");

    /* xr_file_read_all checks ftell, allocates with xr_malloc, and reports
     * the number of bytes that fread actually delivered, closing the door
     * on the previous unchecked-ftell + unchecked-fread pattern. */
    size_t size = 0;
    char *data = xr_file_read_all(bytecode_file, "rb", &size);
    if (!data) {
        xr_log_warning("bytecode", "cannot open or read: %s", bytecode_file);
        return -1;
    }

    int result = xr_eval_bytecode(X, (uint8_t *) data, size);
    xr_free(data);
    return result;
}

/* ========== Output Format ========== */

XrOutputFormat xr_detect_output_format(const char *filename, XrOutputFormat explicit_fmt) {
    if (explicit_fmt != XR_OUTPUT_AUTO)
        return explicit_fmt;
    if (!filename)
        return XR_OUTPUT_BYTECODE;

    const char *ext = strrchr(filename, '.');
    if (!ext)
        return XR_OUTPUT_BYTECODE;

    if (strcmp(ext, ".c") == 0)
        return XR_OUTPUT_C_SOURCE;
    if (strcmp(ext, ".h") == 0)
        return XR_OUTPUT_C_HEADER;
    if (strcmp(ext, ".xrc") == 0)
        return XR_OUTPUT_BYTECODE;

    return XR_OUTPUT_BYTECODE;
}

bool xr_output_c_source(XrVMRuntime *X, XrProto *proto, const char *output_file,
                        const char *var_name, int flags) {
    // Serialize
    size_t bc_size;
    uint8_t *bc = xr_bytecode_write(X, proto, flags, &bc_size, NULL);
    if (!bc)
        return false;

    // Write C file
    FILE *f = fopen(output_file, "w");
    if (!f) {
        xr_free(bc);
        return false;
    }

    fprintf(f, "/* Auto-generated by xray compile */\n\n");
    fprintf(f, "#include <stdint.h>\n\n");
    fprintf(f, "const uint32_t %s_size = %zu;\n\n", var_name, bc_size);
    fprintf(f, "const uint8_t %s[%zu] = {\n", var_name, bc_size);

    for (size_t i = 0; i < bc_size; i++) {
        if (i % 12 == 0)
            fprintf(f, "    ");
        fprintf(f, "0x%02x", bc[i]);
        if (i < bc_size - 1)
            fprintf(f, ",");
        if ((i + 1) % 12 == 0 || i == bc_size - 1)
            fprintf(f, "\n");
        else
            fprintf(f, " ");
    }

    fprintf(f, "};\n");

    fclose(f);
    xr_free(bc);
    return true;
}
