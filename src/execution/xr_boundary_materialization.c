/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_boundary_materialization.c - Exact public boundary layout derivation
 */

#include "xr_boundary_materialization.h"

#include "../base/xmalloc.h"
#include "../base/xsha256.h"
#include "../core/xr_core_spec_gen.h"
#include "../program/xr_validated_program_internal.h"

#include <limits.h>
#include <string.h>

typedef struct XrBoundaryTypeMetric {
    uint8_t state;
    uint8_t layout_kind;
    uint8_t cleanup_kind;
    uint8_t reserved8;
    uint32_t size;
    uint32_t alignment;
    uint32_t tag_offset;
    uint32_t tag_size;
    uint32_t payload_offset;
    uint32_t field_count;
    uint32_t variant_count;
    XrBoundaryTypeLayoutId id;
} XrBoundaryTypeMetric;

typedef struct XrBoundaryLayoutBuilder {
    const XrValidatedProgram *program;
    const XrTargetProfile *profile;
    const XrBoundaryAbi *abi;
    XrMaterializedBoundaryKind boundary_kind;
    XrBoundaryMaterializationBudget budget;
    XrBoundaryTypeMetric *metrics;
    uint64_t work;
    uint32_t type_visits;
    uint32_t type_depth;
    uint32_t fields;
    uint32_t variants;
    XrBoundaryMaterializationStatus status;
    XrBoundaryMaterializationDiagnostic diagnostic;
} XrBoundaryLayoutBuilder;

static void hash_u64(XrSHA256Context *context, uint64_t value) {
    uint8_t bytes[8];
    for (uint32_t index = 0; index < sizeof(bytes); ++index)
        bytes[index] = (uint8_t) (value >> (index * 8u));
    xr_sha256_update(context, bytes, sizeof(bytes));
}

static void hash_fingerprint(XrSHA256Context *context, XrFingerprint fingerprint) {
    xr_sha256_update(context, fingerprint.bytes, sizeof(fingerprint.bytes));
}

static bool fingerprint_is_zero(XrFingerprint fingerprint) {
    uint8_t combined = 0;
    for (uint32_t index = 0; index < sizeof(fingerprint.bytes); ++index)
        combined |= fingerprint.bytes[index];
    return combined == 0;
}

static bool boundary_kind_valid(XrMaterializedBoundaryKind kind) {
    return kind >= XR_MATERIALIZED_BOUNDARY_PUBLIC_CALL &&
           kind <= XR_MATERIALIZED_BOUNDARY_RELOADABLE;
}

static void clear_diagnostic(XrBoundaryMaterializationDiagnostic *diagnostic) {
    if (diagnostic)
        memset(diagnostic, 0, sizeof(*diagnostic));
}

static bool reject(XrBoundaryLayoutBuilder *builder, XrBoundaryMaterializationStatus status,
                   XrBoundaryMaterializationDiagnosticKind kind, uint16_t type_id, uint32_t variant,
                   uint32_t field) {
    if (builder->status == XR_BOUNDARY_MATERIALIZATION_OK) {
        builder->status = status;
        builder->diagnostic.kind = kind;
        builder->diagnostic.type_id = type_id;
        builder->diagnostic.variant_ordinal = variant;
        builder->diagnostic.field_ordinal = field;
    }
    return false;
}

static bool spend(XrBoundaryLayoutBuilder *builder, uint64_t amount, uint16_t type_id) {
    if (amount > builder->budget.max_work - builder->work)
        return reject(builder, XR_BOUNDARY_MATERIALIZATION_RESOURCE_LIMIT,
                      XR_BOUNDARY_MATERIALIZATION_DIAGNOSTIC_RESOURCE, type_id,
                      XR_BOUNDARY_VARIANT_NONE, UINT32_MAX);
    builder->work += amount;
    return true;
}

static bool checked_align(XrBoundaryLayoutBuilder *builder, uint32_t value, uint32_t alignment,
                          uint16_t type_id, uint32_t variant, uint32_t field, uint32_t *result) {
    if (!alignment || (alignment & (alignment - 1u)) != 0u || value > UINT32_MAX - (alignment - 1u))
        return reject(builder, XR_BOUNDARY_MATERIALIZATION_OVERFLOW,
                      XR_BOUNDARY_MATERIALIZATION_DIAGNOSTIC_OVERFLOW, type_id, variant, field);
    uint32_t aligned = (value + alignment - 1u) & ~(alignment - 1u);
    if (aligned > builder->budget.max_extent)
        return reject(builder, XR_BOUNDARY_MATERIALIZATION_RESOURCE_LIMIT,
                      XR_BOUNDARY_MATERIALIZATION_DIAGNOSTIC_RESOURCE, type_id, variant, field);
    *result = aligned;
    return true;
}

static bool checked_add(XrBoundaryLayoutBuilder *builder, uint32_t left, uint32_t right,
                        uint16_t type_id, uint32_t variant, uint32_t field, uint32_t *result) {
    if (left > UINT32_MAX - right)
        return reject(builder, XR_BOUNDARY_MATERIALIZATION_OVERFLOW,
                      XR_BOUNDARY_MATERIALIZATION_DIAGNOSTIC_OVERFLOW, type_id, variant, field);
    uint32_t sum = left + right;
    if (sum > builder->budget.max_extent)
        return reject(builder, XR_BOUNDARY_MATERIALIZATION_RESOURCE_LIMIT,
                      XR_BOUNDARY_MATERIALIZATION_DIAGNOSTIC_RESOURCE, type_id, variant, field);
    *result = sum;
    return true;
}

static const XrBoundaryValueAbi *scalar_abi(const XrBoundaryAbi *abi, uint16_t type_id) {
    if (!abi)
        return NULL;
    for (uint32_t index = 0; index < abi->value_count; ++index) {
        if (abi->values[index].type_id == type_id)
            return &abi->values[index];
    }
    return NULL;
}

static void begin_type_hash(const XrBoundaryLayoutBuilder *builder, uint16_t type_id,
                            uint8_t layout_kind, XrSHA256Context *context) {
    static const uint8_t domain[] = "xray-boundary-type-layout-v1\0";
    xr_sha256_init(context);
    xr_sha256_update(context, domain, sizeof(domain) - 1u);
    XrProgramId program_id = xr_validated_program_id(builder->program);
    xr_sha256_update(context, program_id.bytes, sizeof(program_id.bytes));
    hash_fingerprint(context, builder->abi->id);
    hash_u64(context, builder->boundary_kind);
    hash_u64(context, type_id);
    hash_u64(context, layout_kind);
}

static bool type_metric(XrBoundaryLayoutBuilder *builder, uint16_t type_id,
                        XrBoundaryTypeMetric *metric_out);

static bool scalar_metric(XrBoundaryLayoutBuilder *builder, uint16_t type_id,
                          XrBoundaryTypeMetric *metric) {
    const XrBoundaryValueAbi *scalar = scalar_abi(builder->abi, type_id);
    if (!scalar)
        return reject(builder, XR_BOUNDARY_MATERIALIZATION_UNSUPPORTED,
                      XR_BOUNDARY_MATERIALIZATION_DIAGNOSTIC_TYPE, type_id,
                      XR_BOUNDARY_VARIANT_NONE, UINT32_MAX);
    memset(metric, 0, sizeof(*metric));
    metric->state = 2u;
    metric->layout_kind = type_id == XR_CORE_TYPE_VOID ? XR_BOUNDARY_TYPE_LAYOUT_VOID
                                                       : XR_BOUNDARY_TYPE_LAYOUT_SCALAR;
    metric->cleanup_kind = XR_BOUNDARY_CLEANUP_TRIVIAL;
    metric->size = scalar->size;
    metric->alignment = type_id == XR_CORE_TYPE_VOID ? 1u : scalar->alignment;
    XrSHA256Context context;
    begin_type_hash(builder, type_id, metric->layout_kind, &context);
    hash_u64(&context, scalar->representation);
    hash_u64(&context, scalar->ownership);
    hash_u64(&context, metric->size);
    hash_u64(&context, metric->alignment);
    xr_sha256_final(&context, metric->id.bytes);
    return true;
}

static bool note_type_members(XrBoundaryLayoutBuilder *builder, const XrValidatedType *type) {
    uint64_t fields = type->field_count;
    for (uint32_t variant = 0; variant < type->variant_count; ++variant)
        fields += type->variants[variant].payload_count;
    if (fields > builder->budget.max_fields - builder->fields ||
        type->variant_count > builder->budget.max_variants - builder->variants)
        return reject(builder, XR_BOUNDARY_MATERIALIZATION_RESOURCE_LIMIT,
                      XR_BOUNDARY_MATERIALIZATION_DIAGNOSTIC_RESOURCE, type->type_id,
                      XR_BOUNDARY_VARIANT_NONE, UINT32_MAX);
    builder->fields += (uint32_t) fields;
    builder->variants += type->variant_count;
    return spend(builder, fields + type->variant_count + 1u, type->type_id);
}

static bool aggregate_metric(XrBoundaryLayoutBuilder *builder, const XrValidatedType *type,
                             XrBoundaryTypeMetric *metric) {
    uint32_t offset = 0;
    uint32_t alignment = 1;
    XrSHA256Context context;
    begin_type_hash(builder, type->type_id, XR_BOUNDARY_TYPE_LAYOUT_AGGREGATE, &context);
    hash_u64(&context, type->field_count);
    for (uint32_t field = 0; field < type->field_count; ++field) {
        XrBoundaryTypeMetric child;
        if (!type_metric(builder, type->field_types[field], &child) ||
            !checked_align(builder, offset, child.alignment, type->type_id,
                           XR_BOUNDARY_VARIANT_NONE, field, &offset))
            return false;
        hash_u64(&context, field);
        hash_u64(&context, type->field_types[field]);
        hash_u64(&context, offset);
        hash_u64(&context, child.size);
        hash_u64(&context, child.alignment);
        hash_fingerprint(&context, child.id);
        if (!checked_add(builder, offset, child.size, type->type_id, XR_BOUNDARY_VARIANT_NONE,
                         field, &offset))
            return false;
        if (child.alignment > alignment)
            alignment = child.alignment;
    }
    if (!checked_align(builder, offset, alignment, type->type_id, XR_BOUNDARY_VARIANT_NONE,
                       UINT32_MAX, &metric->size))
        return false;
    metric->alignment = alignment;
    metric->field_count = type->field_count;
    hash_u64(&context, metric->size);
    hash_u64(&context, metric->alignment);
    xr_sha256_final(&context, metric->id.bytes);
    return true;
}

static bool variant_payload_shape(XrBoundaryLayoutBuilder *builder, const XrValidatedType *type,
                                  uint32_t variant, uint32_t *size_out, uint32_t *alignment_out) {
    const XrValidatedVariant *row = &type->variants[variant];
    uint32_t offset = 0;
    uint32_t alignment = 1;
    for (uint32_t field = 0; field < row->payload_count; ++field) {
        XrBoundaryTypeMetric child;
        if (!type_metric(builder, row->payload_types[field], &child) ||
            !checked_align(builder, offset, child.alignment, type->type_id, variant, field,
                           &offset) ||
            !checked_add(builder, offset, child.size, type->type_id, variant, field, &offset))
            return false;
        if (child.alignment > alignment)
            alignment = child.alignment;
    }
    if (!checked_align(builder, offset, alignment, type->type_id, variant, UINT32_MAX, size_out))
        return false;
    *alignment_out = alignment;
    return true;
}

static bool hash_variant_fields(XrBoundaryLayoutBuilder *builder, const XrValidatedType *type,
                                uint32_t payload_offset, XrSHA256Context *context) {
    for (uint32_t variant = 0; variant < type->variant_count; ++variant) {
        const XrValidatedVariant *row = &type->variants[variant];
        uint32_t offset = 0;
        uint32_t payload_size = 0;
        uint32_t payload_alignment = 1;
        if (!variant_payload_shape(builder, type, variant, &payload_size, &payload_alignment))
            return false;
        hash_u64(context, variant);
        hash_u64(context, row->payload_count);
        hash_u64(context, payload_size);
        hash_u64(context, payload_alignment);
        for (uint32_t field = 0; field < row->payload_count; ++field) {
            XrBoundaryTypeMetric child;
            uint32_t absolute_offset = 0;
            if (!type_metric(builder, row->payload_types[field], &child) ||
                !checked_align(builder, offset, child.alignment, type->type_id, variant, field,
                               &offset) ||
                !checked_add(builder, payload_offset, offset, type->type_id, variant, field,
                             &absolute_offset))
                return false;
            hash_u64(context, field);
            hash_u64(context, row->payload_types[field]);
            hash_u64(context, absolute_offset);
            hash_u64(context, child.size);
            hash_u64(context, child.alignment);
            hash_fingerprint(context, child.id);
            if (!checked_add(builder, offset, child.size, type->type_id, variant, field, &offset))
                return false;
        }
    }
    return true;
}

static bool variant_metric(XrBoundaryLayoutBuilder *builder, const XrValidatedType *type,
                           XrBoundaryTypeMetric *metric) {
    XrBoundaryTypeMetric tag;
    if (!type_metric(builder, builder->abi->variant_tag_type, &tag))
        return false;
    uint32_t payload_size = 0;
    uint32_t payload_alignment = 1;
    uint32_t field_count = 0;
    for (uint32_t variant = 0; variant < type->variant_count; ++variant) {
        uint32_t case_size = 0;
        uint32_t case_alignment = 1;
        if (!variant_payload_shape(builder, type, variant, &case_size, &case_alignment))
            return false;
        if (case_size > payload_size)
            payload_size = case_size;
        if (case_alignment > payload_alignment)
            payload_alignment = case_alignment;
        field_count += type->variants[variant].payload_count;
    }
    uint32_t total_alignment =
        tag.alignment > payload_alignment ? tag.alignment : payload_alignment;
    uint32_t tag_end = 0;
    if (!checked_add(builder, 0u, tag.size, type->type_id, XR_BOUNDARY_VARIANT_NONE, UINT32_MAX,
                     &tag_end) ||
        !checked_align(builder, tag_end, payload_alignment, type->type_id, XR_BOUNDARY_VARIANT_NONE,
                       UINT32_MAX, &metric->payload_offset))
        return false;
    uint32_t end = 0;
    if (!checked_add(builder, metric->payload_offset, payload_size, type->type_id,
                     XR_BOUNDARY_VARIANT_NONE, UINT32_MAX, &end) ||
        !checked_align(builder, end, total_alignment, type->type_id, XR_BOUNDARY_VARIANT_NONE,
                       UINT32_MAX, &metric->size))
        return false;
    metric->alignment = total_alignment;
    metric->tag_size = tag.size;
    metric->field_count = field_count;
    metric->variant_count = type->variant_count;
    XrSHA256Context context;
    begin_type_hash(builder, type->type_id, XR_BOUNDARY_TYPE_LAYOUT_VARIANT, &context);
    hash_u64(&context, builder->abi->variant_tag_type);
    hash_u64(&context, metric->tag_offset);
    hash_u64(&context, metric->tag_size);
    hash_u64(&context, metric->payload_offset);
    hash_u64(&context, type->variant_count);
    if (!hash_variant_fields(builder, type, metric->payload_offset, &context))
        return false;
    hash_u64(&context, metric->size);
    hash_u64(&context, metric->alignment);
    xr_sha256_final(&context, metric->id.bytes);
    return true;
}

static bool dynamic_metric(XrBoundaryLayoutBuilder *builder, uint16_t type_id,
                           XrBoundaryTypeMetric *metric_out) {
    uint32_t index = (uint32_t) type_id - XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE;
    if (index >= builder->program->type_count)
        return reject(builder, XR_BOUNDARY_MATERIALIZATION_UNSUPPORTED,
                      XR_BOUNDARY_MATERIALIZATION_DIAGNOSTIC_TYPE, type_id,
                      XR_BOUNDARY_VARIANT_NONE, UINT32_MAX);
    XrBoundaryTypeMetric *metric = &builder->metrics[index];
    if (metric->state == 2u) {
        *metric_out = *metric;
        return true;
    }
    if (metric->state == 1u || builder->type_visits == builder->budget.max_type_visits)
        return reject(builder, XR_BOUNDARY_MATERIALIZATION_RESOURCE_LIMIT,
                      XR_BOUNDARY_MATERIALIZATION_DIAGNOSTIC_RESOURCE, type_id,
                      XR_BOUNDARY_VARIANT_NONE, UINT32_MAX);
    metric->state = 1u;
    ++builder->type_visits;
    const XrValidatedType *type = &builder->program->types[index];
    if (type->kind != XR_CORE_IR_TYPE_AGGREGATE && type->kind != XR_CORE_IR_TYPE_VARIANT)
        return reject(builder, XR_BOUNDARY_MATERIALIZATION_UNSUPPORTED,
                      XR_BOUNDARY_MATERIALIZATION_DIAGNOSTIC_TYPE, type_id,
                      XR_BOUNDARY_VARIANT_NONE, UINT32_MAX);
    if (!note_type_members(builder, type))
        return false;
    metric->layout_kind = type->kind == XR_CORE_IR_TYPE_AGGREGATE
                              ? XR_BOUNDARY_TYPE_LAYOUT_AGGREGATE
                              : XR_BOUNDARY_TYPE_LAYOUT_VARIANT;
    metric->cleanup_kind = XR_BOUNDARY_CLEANUP_TRIVIAL;
    bool valid = type->kind == XR_CORE_IR_TYPE_AGGREGATE ? aggregate_metric(builder, type, metric)
                                                         : variant_metric(builder, type, metric);
    if (!valid)
        return false;
    metric->state = 2u;
    *metric_out = *metric;
    return true;
}

static bool type_metric(XrBoundaryLayoutBuilder *builder, uint16_t type_id,
                        XrBoundaryTypeMetric *metric_out) {
    if (!spend(builder, 1u, type_id))
        return false;
    if (builder->type_depth == builder->budget.max_type_depth)
        return reject(builder, XR_BOUNDARY_MATERIALIZATION_RESOURCE_LIMIT,
                      XR_BOUNDARY_MATERIALIZATION_DIAGNOSTIC_RESOURCE, type_id,
                      XR_BOUNDARY_VARIANT_NONE, UINT32_MAX);
    ++builder->type_depth;
    bool valid = type_id <= XR_CORE_TYPE_ERROR ? scalar_metric(builder, type_id, metric_out)
                 : type_id < XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE
                     ? reject(builder, XR_BOUNDARY_MATERIALIZATION_UNSUPPORTED,
                              XR_BOUNDARY_MATERIALIZATION_DIAGNOSTIC_TYPE, type_id,
                              XR_BOUNDARY_VARIANT_NONE, UINT32_MAX)
                     : dynamic_metric(builder, type_id, metric_out);
    --builder->type_depth;
    return valid;
}

static bool builder_init(XrBoundaryLayoutBuilder *builder, const XrInstance *instance,
                         XrMaterializedBoundaryKind boundary_kind,
                         const XrBoundaryMaterializationBudget *budget) {
    memset(builder, 0, sizeof(*builder));
    builder->budget = budget ? *budget : xr_boundary_materialization_default_budget();
    builder->boundary_kind = boundary_kind;
    builder->program = xr_execution_instance_program(instance);
    builder->profile = xr_execution_instance_profile(instance);
    builder->abi = xr_target_profile_boundary_abi(builder->profile);
    if (!builder->program || !builder->profile || !boundary_kind_valid(boundary_kind) ||
        !builder->budget.max_work || !builder->budget.max_type_visits ||
        !builder->budget.max_type_depth || !builder->budget.max_fields ||
        !builder->budget.max_variants || !builder->budget.max_extent ||
        builder->budget.max_type_depth > builder->budget.max_type_visits) {
        builder->status = XR_BOUNDARY_MATERIALIZATION_INVALID_INPUT;
        builder->diagnostic.kind = XR_BOUNDARY_MATERIALIZATION_DIAGNOSTIC_INPUT;
        return false;
    }
    if (!xr_target_profile_verify(builder->profile, NULL, 0) || !builder->abi ||
        builder->abi->schema_version != XR_BOUNDARY_ABI_SCHEMA_VERSION ||
        builder->abi->call_convention != XR_BOUNDARY_CALL_FRAME_V1 ||
        builder->abi->aggregate_layout_model !=
            XR_BOUNDARY_AGGREGATE_LAYOUT_DECLARATION_ORDER_NATURAL ||
        builder->abi->variant_layout_model != XR_BOUNDARY_VARIANT_LAYOUT_U32_TAG_NATURAL_PAYLOAD ||
        builder->abi->root_model != XR_BOUNDARY_ROOT_MODEL_EXPLICIT_OFFSETS ||
        builder->abi->cleanup_model != XR_BOUNDARY_CLEANUP_MODEL_EXPLICIT_ACTIONS ||
        builder->abi->variant_tag_type != XR_CORE_TYPE_U32) {
        builder->status = XR_BOUNDARY_MATERIALIZATION_INVALID_INPUT;
        builder->diagnostic.kind = XR_BOUNDARY_MATERIALIZATION_DIAGNOSTIC_PROFILE;
        return false;
    }
    if (builder->program->type_count != 0u) {
        builder->metrics = xr_calloc(builder->program->type_count, sizeof(*builder->metrics));
        if (!builder->metrics) {
            builder->status = XR_BOUNDARY_MATERIALIZATION_OUT_OF_MEMORY;
            builder->diagnostic.kind = XR_BOUNDARY_MATERIALIZATION_DIAGNOSTIC_MEMORY;
            return false;
        }
    }
    return true;
}

static void builder_destroy(XrBoundaryLayoutBuilder *builder) {
    xr_free(builder->metrics);
}

static bool allocate_layout_rows(XrBoundaryLayoutBuilder *builder, XrBoundaryTypeLayout *layout) {
#if SIZE_MAX < UINT64_MAX
    if ((uint64_t) layout->field_count > SIZE_MAX / sizeof(*layout->fields) ||
        (uint64_t) layout->variant_count > SIZE_MAX / sizeof(*layout->variants))
        return reject(builder, XR_BOUNDARY_MATERIALIZATION_OVERFLOW,
                      XR_BOUNDARY_MATERIALIZATION_DIAGNOSTIC_OVERFLOW, layout->type_id,
                      XR_BOUNDARY_VARIANT_NONE, UINT32_MAX);
#endif
    if (layout->field_count != 0u) {
        layout->fields = xr_calloc(layout->field_count, sizeof(*layout->fields));
        if (!layout->fields)
            return reject(builder, XR_BOUNDARY_MATERIALIZATION_OUT_OF_MEMORY,
                          XR_BOUNDARY_MATERIALIZATION_DIAGNOSTIC_MEMORY, layout->type_id,
                          XR_BOUNDARY_VARIANT_NONE, UINT32_MAX);
    }
    if (layout->variant_count != 0u) {
        layout->variants = xr_calloc(layout->variant_count, sizeof(*layout->variants));
        if (!layout->variants)
            return reject(builder, XR_BOUNDARY_MATERIALIZATION_OUT_OF_MEMORY,
                          XR_BOUNDARY_MATERIALIZATION_DIAGNOSTIC_MEMORY, layout->type_id,
                          XR_BOUNDARY_VARIANT_NONE, UINT32_MAX);
    }
    return true;
}

static bool fill_aggregate_rows(XrBoundaryLayoutBuilder *builder, const XrValidatedType *type,
                                XrBoundaryTypeLayout *layout) {
    uint32_t offset = 0;
    for (uint32_t field = 0; field < type->field_count; ++field) {
        XrBoundaryTypeMetric child;
        if (!type_metric(builder, type->field_types[field], &child) ||
            !checked_align(builder, offset, child.alignment, type->type_id,
                           XR_BOUNDARY_VARIANT_NONE, field, &offset))
            return false;
        layout->fields[field] = (XrBoundaryFieldLayout) {
            .type_id = type->field_types[field],
            .variant_ordinal = XR_BOUNDARY_VARIANT_NONE,
            .field_ordinal = field,
            .offset = offset,
            .size = child.size,
            .alignment = child.alignment,
            .type_layout_id = child.id,
        };
        if (!checked_add(builder, offset, child.size, type->type_id, XR_BOUNDARY_VARIANT_NONE,
                         field, &offset))
            return false;
    }
    return true;
}

static bool fill_variant_rows(XrBoundaryLayoutBuilder *builder, const XrValidatedType *type,
                              XrBoundaryTypeLayout *layout) {
    uint32_t flat_field = 0;
    for (uint32_t variant = 0; variant < type->variant_count; ++variant) {
        const XrValidatedVariant *row = &type->variants[variant];
        uint32_t payload_size = 0;
        uint32_t payload_alignment = 1;
        if (!variant_payload_shape(builder, type, variant, &payload_size, &payload_alignment))
            return false;
        layout->variants[variant] = (XrBoundaryVariantLayout) {
            .variant_ordinal = variant,
            .field_begin = flat_field,
            .field_count = row->payload_count,
            .payload_size = payload_size,
            .payload_alignment = payload_alignment,
        };
        uint32_t offset = 0;
        for (uint32_t field = 0; field < row->payload_count; ++field) {
            XrBoundaryTypeMetric child;
            uint32_t absolute_offset = 0;
            if (!type_metric(builder, row->payload_types[field], &child) ||
                !checked_align(builder, offset, child.alignment, type->type_id, variant, field,
                               &offset) ||
                !checked_add(builder, layout->payload_offset, offset, type->type_id, variant, field,
                             &absolute_offset))
                return false;
            layout->fields[flat_field++] = (XrBoundaryFieldLayout) {
                .type_id = row->payload_types[field],
                .variant_ordinal = variant,
                .field_ordinal = field,
                .offset = absolute_offset,
                .size = child.size,
                .alignment = child.alignment,
                .type_layout_id = child.id,
            };
            if (!checked_add(builder, offset, child.size, type->type_id, variant, field, &offset))
                return false;
        }
    }
    return true;
}

static bool fill_type_layout(XrBoundaryLayoutBuilder *builder, uint16_t type_id,
                             const XrBoundaryTypeMetric *metric, XrBoundaryTypeLayout *layout) {
    layout->schema_version = XR_BOUNDARY_MATERIALIZATION_SCHEMA_VERSION;
    layout->type_id = type_id;
    layout->boundary_kind = (uint8_t) builder->boundary_kind;
    layout->layout_kind = metric->layout_kind;
    layout->cleanup_kind = metric->cleanup_kind;
    layout->size = metric->size;
    layout->alignment = metric->alignment;
    layout->tag_offset = metric->tag_offset;
    layout->tag_size = metric->tag_size;
    layout->payload_offset = metric->payload_offset;
    layout->field_count = metric->field_count;
    layout->variant_count = metric->variant_count;
    layout->program_id = xr_validated_program_id(builder->program);
    layout->boundary_abi_id = builder->abi->id;
    layout->id = metric->id;
    if (!allocate_layout_rows(builder, layout))
        return false;
    if (type_id < XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE)
        return true;
    const XrValidatedType *type = xr_validated_program_type(builder->program, type_id);
    return type &&
           (type->kind == XR_CORE_IR_TYPE_AGGREGATE ? fill_aggregate_rows(builder, type, layout)
                                                    : fill_variant_rows(builder, type, layout));
}

XrBoundaryMaterializationBudget xr_boundary_materialization_default_budget(void) {
    return (XrBoundaryMaterializationBudget) {
        .max_work = UINT64_C(4194304),
        .max_type_visits = UINT32_C(65536),
        .max_type_depth = UINT32_C(256),
        .max_fields = UINT32_C(1048576),
        .max_variants = UINT32_C(65536),
        .max_extent = UINT32_C(67108864),
    };
}

XrBoundaryMaterializationStatus xr_execution_materialize_boundary_type(
    const XrInstance *instance, XrMaterializedBoundaryKind boundary_kind, uint16_t type_id,
    const XrBoundaryMaterializationBudget *budget, XrBoundaryTypeLayout **layout_out,
    XrBoundaryMaterializationDiagnostic *diagnostic_out) {
    if (layout_out)
        *layout_out = NULL;
    clear_diagnostic(diagnostic_out);
    if (!instance || !layout_out) {
        if (diagnostic_out)
            diagnostic_out->kind = XR_BOUNDARY_MATERIALIZATION_DIAGNOSTIC_INPUT;
        return XR_BOUNDARY_MATERIALIZATION_INVALID_INPUT;
    }
    XrBoundaryLayoutBuilder builder;
    if (!builder_init(&builder, instance, boundary_kind, budget)) {
        if (diagnostic_out)
            *diagnostic_out = builder.diagnostic;
        builder_destroy(&builder);
        return builder.status;
    }
    XrBoundaryTypeMetric metric;
    XrBoundaryTypeLayout *layout = NULL;
    if (type_metric(&builder, type_id, &metric)) {
        layout = xr_calloc(1u, sizeof(*layout));
        if (!layout)
            reject(&builder, XR_BOUNDARY_MATERIALIZATION_OUT_OF_MEMORY,
                   XR_BOUNDARY_MATERIALIZATION_DIAGNOSTIC_MEMORY, type_id, XR_BOUNDARY_VARIANT_NONE,
                   UINT32_MAX);
        else if (!fill_type_layout(&builder, type_id, &metric, layout)) {
            xr_boundary_type_layout_free(layout);
            layout = NULL;
        }
    }
    XrBoundaryMaterializationStatus status = builder.status;
    if (status == XR_BOUNDARY_MATERIALIZATION_OK && layout)
        *layout_out = layout;
    else if (status == XR_BOUNDARY_MATERIALIZATION_OK)
        status = XR_BOUNDARY_MATERIALIZATION_UNSUPPORTED;
    if (diagnostic_out)
        *diagnostic_out = builder.diagnostic;
    builder_destroy(&builder);
    return status;
}

void xr_boundary_type_layout_free(XrBoundaryTypeLayout *layout) {
    if (!layout)
        return;
    xr_free(layout->roots);
    xr_free(layout->variants);
    xr_free(layout->fields);
    xr_free(layout);
}

static void fill_call_slot(XrBoundaryCallSlotLayout *slot, uint16_t type_id,
                           XrBoundaryCallDirection direction, uint32_t offset,
                           const XrBoundaryTypeMetric *metric) {
    *slot = (XrBoundaryCallSlotLayout) {
        .type_id = type_id,
        .direction = (uint8_t) direction,
        .ownership =
            type_id == XR_CORE_TYPE_VOID ? XR_BOUNDARY_OWNERSHIP_NONE : XR_BOUNDARY_OWNERSHIP_COPY,
        .offset = offset,
        .size = metric->size,
        .alignment = metric->alignment,
        .type_layout_id = metric->id,
    };
}

static bool fill_call_arguments(XrBoundaryLayoutBuilder *builder,
                                const XrValidatedFunction *function, XrBoundaryCallLayout *layout) {
    uint32_t offset = 0;
    uint32_t alignment = 1;
    for (uint32_t parameter = 0; parameter < function->parameter_count; ++parameter) {
        XrBoundaryTypeMetric metric;
        uint16_t type_id = function->parameter_types[parameter];
        if (!type_metric(builder, type_id, &metric) ||
            !checked_align(builder, offset, metric.alignment, type_id, XR_BOUNDARY_VARIANT_NONE,
                           parameter, &offset))
            return false;
        fill_call_slot(&layout->arguments[parameter], type_id, XR_BOUNDARY_DIRECTION_IN, offset,
                       &metric);
        if (!checked_add(builder, offset, metric.size, type_id, XR_BOUNDARY_VARIANT_NONE, parameter,
                         &offset))
            return false;
        if (metric.alignment > alignment)
            alignment = metric.alignment;
    }
    if (!checked_align(builder, offset, alignment, XR_CORE_TYPE_VOID, XR_BOUNDARY_VARIANT_NONE,
                       UINT32_MAX, &layout->argument_size))
        return false;
    layout->argument_alignment = alignment;
    return true;
}

static void finish_call_id(const XrBoundaryLayoutBuilder *builder, XrBoundaryCallLayout *layout) {
    static const uint8_t domain[] = "xray-boundary-call-layout-v1\0";
    XrSHA256Context context;
    xr_sha256_init(&context);
    xr_sha256_update(&context, domain, sizeof(domain) - 1u);
    hash_fingerprint(&context, layout->execution_id);
    hash_fingerprint(&context, builder->abi->id);
    hash_u64(&context, layout->boundary_kind);
    hash_u64(&context, layout->function_id);
    hash_u64(&context, layout->call_convention);
    hash_u64(&context, layout->error_model);
    hash_u64(&context, layout->argument_count);
    hash_u64(&context, layout->argument_size);
    hash_u64(&context, layout->argument_alignment);
    for (uint32_t index = 0; index < layout->argument_count; ++index) {
        const XrBoundaryCallSlotLayout *slot = &layout->arguments[index];
        hash_u64(&context, slot->type_id);
        hash_u64(&context, slot->direction);
        hash_u64(&context, slot->ownership);
        hash_u64(&context, slot->offset);
        hash_u64(&context, slot->size);
        hash_u64(&context, slot->alignment);
        hash_fingerprint(&context, slot->type_layout_id);
    }
    const XrBoundaryCallSlotLayout *result = &layout->result;
    hash_u64(&context, result->type_id);
    hash_u64(&context, result->direction);
    hash_u64(&context, result->ownership);
    hash_u64(&context, result->size);
    hash_u64(&context, result->alignment);
    hash_fingerprint(&context, result->type_layout_id);
    xr_sha256_final(&context, layout->id.bytes);
}

XrBoundaryMaterializationStatus xr_execution_materialize_boundary_call(
    const XrInstance *instance, XrMaterializedBoundaryKind boundary_kind, uint32_t function_id,
    const XrBoundaryMaterializationBudget *budget, XrBoundaryCallLayout **layout_out,
    XrBoundaryMaterializationDiagnostic *diagnostic_out) {
    if (layout_out)
        *layout_out = NULL;
    clear_diagnostic(diagnostic_out);
    if (!instance || !layout_out) {
        if (diagnostic_out)
            diagnostic_out->kind = XR_BOUNDARY_MATERIALIZATION_DIAGNOSTIC_INPUT;
        return XR_BOUNDARY_MATERIALIZATION_INVALID_INPUT;
    }
    XrBoundaryLayoutBuilder builder;
    if (!builder_init(&builder, instance, boundary_kind, budget)) {
        if (diagnostic_out)
            *diagnostic_out = builder.diagnostic;
        builder_destroy(&builder);
        return builder.status;
    }
    XrBoundaryCallLayout *layout = NULL;
    if (function_id >= builder.program->function_count) {
        reject(&builder, XR_BOUNDARY_MATERIALIZATION_UNSUPPORTED,
               XR_BOUNDARY_MATERIALIZATION_DIAGNOSTIC_FUNCTION, 0, XR_BOUNDARY_VARIANT_NONE,
               UINT32_MAX);
        builder.diagnostic.function_id = function_id;
    } else {
        const XrValidatedFunction *function = &builder.program->functions[function_id];
        layout = xr_calloc(1u, sizeof(*layout));
        if (!layout)
            reject(&builder, XR_BOUNDARY_MATERIALIZATION_OUT_OF_MEMORY,
                   XR_BOUNDARY_MATERIALIZATION_DIAGNOSTIC_MEMORY, 0, XR_BOUNDARY_VARIANT_NONE,
                   UINT32_MAX);
        else if (function->parameter_count != 0u) {
            layout->arguments = xr_calloc(function->parameter_count, sizeof(*layout->arguments));
            if (!layout->arguments)
                reject(&builder, XR_BOUNDARY_MATERIALIZATION_OUT_OF_MEMORY,
                       XR_BOUNDARY_MATERIALIZATION_DIAGNOSTIC_MEMORY, 0, XR_BOUNDARY_VARIANT_NONE,
                       UINT32_MAX);
        }
        if (layout && builder.status == XR_BOUNDARY_MATERIALIZATION_OK) {
            layout->schema_version = XR_BOUNDARY_MATERIALIZATION_SCHEMA_VERSION;
            layout->function_id = function_id;
            layout->boundary_kind = (uint8_t) boundary_kind;
            layout->call_convention = builder.abi->call_convention;
            layout->error_model = builder.abi->error_model;
            layout->argument_count = function->parameter_count;
            layout->execution_id = xr_execution_instance_id(instance);
            XrBoundaryTypeMetric result;
            if (fill_call_arguments(&builder, function, layout) &&
                type_metric(&builder, function->result_type_id, &result)) {
                fill_call_slot(&layout->result, function->result_type_id, XR_BOUNDARY_DIRECTION_OUT,
                               0u, &result);
                finish_call_id(&builder, layout);
            }
        }
    }
    XrBoundaryMaterializationStatus status = builder.status;
    if (status == XR_BOUNDARY_MATERIALIZATION_OK && layout && !fingerprint_is_zero(layout->id))
        *layout_out = layout;
    else {
        xr_boundary_call_layout_free(layout);
        if (status == XR_BOUNDARY_MATERIALIZATION_OK)
            status = XR_BOUNDARY_MATERIALIZATION_UNSUPPORTED;
    }
    if (diagnostic_out)
        *diagnostic_out = builder.diagnostic;
    builder_destroy(&builder);
    return status;
}

void xr_boundary_call_layout_free(XrBoundaryCallLayout *layout) {
    if (!layout)
        return;
    xr_free(layout->arguments);
    xr_free(layout);
}

const char *xr_boundary_materialization_status_name(XrBoundaryMaterializationStatus status) {
    switch (status) {
        case XR_BOUNDARY_MATERIALIZATION_OK:
            return "ok";
        case XR_BOUNDARY_MATERIALIZATION_INVALID_INPUT:
            return "invalid-input";
        case XR_BOUNDARY_MATERIALIZATION_UNSUPPORTED:
            return "unsupported";
        case XR_BOUNDARY_MATERIALIZATION_RESOURCE_LIMIT:
            return "resource-limit";
        case XR_BOUNDARY_MATERIALIZATION_OVERFLOW:
            return "overflow";
        case XR_BOUNDARY_MATERIALIZATION_OUT_OF_MEMORY:
            return "out-of-memory";
    }
    return "unknown";
}
