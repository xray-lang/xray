/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_boundary_materialization.h - Profile-bound public value layouts
 *
 * KEY CONCEPT:
 *   A materialized boundary is derived from one validated logical program and
 *   one exact target profile. Executor-local slots, frames, and native
 *   registers never enter this contract.
 */

#ifndef XR_BOUNDARY_MATERIALIZATION_H
#define XR_BOUNDARY_MATERIALIZATION_H

#include "xr_execution.h"

#define XR_BOUNDARY_MATERIALIZATION_SCHEMA_VERSION UINT32_C(1)
#define XR_BOUNDARY_VARIANT_NONE UINT32_MAX

typedef XrFingerprint XrBoundaryTypeLayoutId;
typedef XrFingerprint XrBoundaryCallLayoutId;

typedef enum XrMaterializedBoundaryKind {
    XR_MATERIALIZED_BOUNDARY_INVALID = 0,
    XR_MATERIALIZED_BOUNDARY_PUBLIC_CALL,
    XR_MATERIALIZED_BOUNDARY_DYNAMIC_STORAGE,
    XR_MATERIALIZED_BOUNDARY_FFI,
    XR_MATERIALIZED_BOUNDARY_HYBRID,
    XR_MATERIALIZED_BOUNDARY_RELOADABLE,
} XrMaterializedBoundaryKind;

typedef enum XrBoundaryTypeLayoutKind {
    XR_BOUNDARY_TYPE_LAYOUT_INVALID = 0,
    XR_BOUNDARY_TYPE_LAYOUT_VOID,
    XR_BOUNDARY_TYPE_LAYOUT_SCALAR,
    XR_BOUNDARY_TYPE_LAYOUT_AGGREGATE,
    XR_BOUNDARY_TYPE_LAYOUT_VARIANT,
} XrBoundaryTypeLayoutKind;

typedef enum XrBoundaryRootKind {
    XR_BOUNDARY_ROOT_INVALID = 0,
    XR_BOUNDARY_ROOT_STRONG,
    XR_BOUNDARY_ROOT_WEAK,
} XrBoundaryRootKind;

typedef enum XrBoundaryCleanupKind {
    XR_BOUNDARY_CLEANUP_INVALID = 0,
    XR_BOUNDARY_CLEANUP_TRIVIAL,
    XR_BOUNDARY_CLEANUP_DROP,
} XrBoundaryCleanupKind;

typedef enum XrBoundaryCallDirection {
    XR_BOUNDARY_DIRECTION_INVALID = 0,
    XR_BOUNDARY_DIRECTION_IN,
    XR_BOUNDARY_DIRECTION_OUT,
    XR_BOUNDARY_DIRECTION_INOUT,
} XrBoundaryCallDirection;

typedef struct XrBoundaryRootLayout {
    uint32_t offset;
    uint16_t type_id;
    uint8_t kind;
    uint8_t reserved8;
} XrBoundaryRootLayout;

typedef struct XrBoundaryFieldLayout {
    uint16_t type_id;
    uint16_t reserved16;
    uint32_t variant_ordinal;
    uint32_t field_ordinal;
    uint32_t offset;
    uint32_t size;
    uint32_t alignment;
    XrBoundaryTypeLayoutId type_layout_id;
} XrBoundaryFieldLayout;

typedef struct XrBoundaryVariantLayout {
    uint32_t variant_ordinal;
    uint32_t field_begin;
    uint32_t field_count;
    uint32_t payload_size;
    uint32_t payload_alignment;
} XrBoundaryVariantLayout;

typedef struct XrBoundaryTypeLayout {
    uint32_t schema_version;
    uint16_t type_id;
    uint8_t boundary_kind;
    uint8_t layout_kind;
    uint8_t cleanup_kind;
    uint8_t reserved8[3];
    uint32_t size;
    uint32_t alignment;
    uint32_t tag_offset;
    uint32_t tag_size;
    uint32_t payload_offset;
    uint32_t field_count;
    uint32_t variant_count;
    uint32_t root_count;
    XrBoundaryFieldLayout *fields;
    XrBoundaryVariantLayout *variants;
    XrBoundaryRootLayout *roots;
    XrProgramId program_id;
    XrBoundaryAbiId boundary_abi_id;
    XrBoundaryTypeLayoutId id;
} XrBoundaryTypeLayout;

typedef struct XrBoundaryCallSlotLayout {
    uint16_t type_id;
    uint8_t direction;
    uint8_t ownership;
    uint32_t offset;
    uint32_t size;
    uint32_t alignment;
    XrBoundaryTypeLayoutId type_layout_id;
} XrBoundaryCallSlotLayout;

typedef struct XrBoundaryCallLayout {
    uint32_t schema_version;
    uint32_t function_id;
    uint8_t boundary_kind;
    uint8_t call_convention;
    uint8_t error_model;
    uint8_t reserved8;
    uint32_t argument_count;
    uint32_t argument_size;
    uint32_t argument_alignment;
    XrBoundaryCallSlotLayout *arguments;
    XrBoundaryCallSlotLayout result;
    XrExecutionId execution_id;
    XrBoundaryCallLayoutId id;
} XrBoundaryCallLayout;

typedef struct XrBoundaryMaterializationBudget {
    uint64_t max_work;
    uint32_t max_type_visits;
    uint32_t max_type_depth;
    uint32_t max_fields;
    uint32_t max_variants;
    uint32_t max_extent;
} XrBoundaryMaterializationBudget;

typedef enum XrBoundaryMaterializationDiagnosticKind {
    XR_BOUNDARY_MATERIALIZATION_DIAGNOSTIC_NONE = 0,
    XR_BOUNDARY_MATERIALIZATION_DIAGNOSTIC_INPUT,
    XR_BOUNDARY_MATERIALIZATION_DIAGNOSTIC_TYPE,
    XR_BOUNDARY_MATERIALIZATION_DIAGNOSTIC_FUNCTION,
    XR_BOUNDARY_MATERIALIZATION_DIAGNOSTIC_PROFILE,
    XR_BOUNDARY_MATERIALIZATION_DIAGNOSTIC_RESOURCE,
    XR_BOUNDARY_MATERIALIZATION_DIAGNOSTIC_OVERFLOW,
    XR_BOUNDARY_MATERIALIZATION_DIAGNOSTIC_MEMORY,
} XrBoundaryMaterializationDiagnosticKind;

typedef struct XrBoundaryMaterializationDiagnostic {
    XrBoundaryMaterializationDiagnosticKind kind;
    uint16_t type_id;
    uint16_t reserved16;
    uint32_t function_id;
    uint32_t variant_ordinal;
    uint32_t field_ordinal;
} XrBoundaryMaterializationDiagnostic;

typedef enum XrBoundaryMaterializationStatus {
    XR_BOUNDARY_MATERIALIZATION_OK = 0,
    XR_BOUNDARY_MATERIALIZATION_INVALID_INPUT,
    XR_BOUNDARY_MATERIALIZATION_UNSUPPORTED,
    XR_BOUNDARY_MATERIALIZATION_RESOURCE_LIMIT,
    XR_BOUNDARY_MATERIALIZATION_OVERFLOW,
    XR_BOUNDARY_MATERIALIZATION_OUT_OF_MEMORY,
} XrBoundaryMaterializationStatus;

XR_FUNC XrBoundaryMaterializationBudget xr_boundary_materialization_default_budget(void);
XR_FUNC XrBoundaryMaterializationStatus xr_execution_materialize_boundary_type(
    const XrInstance *instance, XrMaterializedBoundaryKind boundary_kind, uint16_t type_id,
    const XrBoundaryMaterializationBudget *budget, XrBoundaryTypeLayout **layout_out,
    XrBoundaryMaterializationDiagnostic *diagnostic_out);
XR_FUNC void xr_boundary_type_layout_free(XrBoundaryTypeLayout *layout);
XR_FUNC XrBoundaryMaterializationStatus xr_execution_materialize_boundary_call(
    const XrInstance *instance, XrMaterializedBoundaryKind boundary_kind, uint32_t function_id,
    const XrBoundaryMaterializationBudget *budget, XrBoundaryCallLayout **layout_out,
    XrBoundaryMaterializationDiagnostic *diagnostic_out);
XR_FUNC void xr_boundary_call_layout_free(XrBoundaryCallLayout *layout);
XR_FUNC const char *xr_boundary_materialization_status_name(XrBoundaryMaterializationStatus status);

#endif  // XR_BOUNDARY_MATERIALIZATION_H
