/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_package.h - Canonical project-level native package plan
 *
 * Native code is a build input, never source-language syntax.  This plan is
 * parsed once from xray.toml and is shared by the analyzer, VM FFI lowering,
 * AOT linking, provenance reporting, and the `xray explain native` command.
 */

#ifndef XNATIVE_PACKAGE_H
#define XNATIVE_PACKAGE_H

#include "../base/xdefs.h"
#include "../base/xtoml.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef enum XrNativeAuditMode {
    XR_NATIVE_AUDIT_NONE = 0,
    XR_NATIVE_AUDIT_EXPLORATORY,
    XR_NATIVE_AUDIT_SHIPPING,
} XrNativeAuditMode;

typedef enum XrNativeVmPolicy {
    XR_NATIVE_VM_UNSPECIFIED = 0,
    XR_NATIVE_VM_VERIFIED_DYNAMIC,
    XR_NATIVE_VM_UNSUPPORTED,
} XrNativeVmPolicy;

typedef enum XrNativeUnitKind {
    XR_NATIVE_UNIT_C = 1,
    XR_NATIVE_UNIT_ASM,
    XR_NATIVE_UNIT_OBJECT,
    XR_NATIVE_UNIT_STATIC_LIBRARY,
    XR_NATIVE_UNIT_DYNAMIC_LIBRARY,
    XR_NATIVE_UNIT_PLATFORM,
} XrNativeUnitKind;

typedef enum XrNativeSymbolKind {
    XR_NATIVE_SYMBOL_FUNCTION = 1,
    XR_NATIVE_SYMBOL_ADDRESS,
} XrNativeSymbolKind;

typedef enum XrNativeParamAccess {
    XR_NATIVE_ACCESS_NONE = 0,
    XR_NATIVE_ACCESS_READ,
    XR_NATIVE_ACCESS_WRITE,
    XR_NATIVE_ACCESS_READWRITE,
} XrNativeParamAccess;

typedef enum XrNativeEscape {
    XR_NATIVE_ESCAPE_UNSPECIFIED = 0,
    XR_NATIVE_ESCAPE_NOESCAPE,
    XR_NATIVE_ESCAPE_BORROW,
    XR_NATIVE_ESCAPE_RETAIN,
    XR_NATIVE_ESCAPE_CONSUME,
} XrNativeEscape;

typedef enum XrNativeOwnership {
    XR_NATIVE_OWNERSHIP_UNSPECIFIED = 0,
    XR_NATIVE_OWNERSHIP_VALUE,
    XR_NATIVE_OWNERSHIP_BORROWED,
    XR_NATIVE_OWNERSHIP_OWNED,
    XR_NATIVE_OWNERSHIP_NONE,
} XrNativeOwnership;

typedef enum XrNativeOutputState {
    XR_NATIVE_OUTPUT_NONE = 0,
    XR_NATIVE_OUTPUT_COMPLETE,
    XR_NATIVE_OUTPUT_PARTIAL,
} XrNativeOutputState;

typedef struct XrNativeParamContract {
    uint32_t index;
    XrNativeParamAccess access;
    bool nullable;
    int32_t length_from;
    XrNativeEscape escape;
    XrNativeOwnership ownership;
    XrNativeOutputState output;
    bool descriptor_rebind;
    bool may_relocate;
    bool may_shorten;
    bool invalidates_views;
} XrNativeParamContract;

typedef struct XrNativeReturnContract {
    XrNativeOwnership ownership;
    bool nullable;
    char *validity;
    char *drop_function;
} XrNativeReturnContract;

typedef enum XrNativeCallbackThread {
    XR_NATIVE_CALLBACK_CALLER_THREAD = 1,
    XR_NATIVE_CALLBACK_FOREIGN_THREAD,
} XrNativeCallbackThread;

typedef enum XrNativeCallbackLifetime {
    XR_NATIVE_CALLBACK_CALL_ONLY = 1,
    XR_NATIVE_CALLBACK_RETAINED,
} XrNativeCallbackLifetime;

typedef enum XrNativeRuntimeAttach {
    XR_NATIVE_RUNTIME_ATTACH_NOT_REQUIRED = 1,
    XR_NATIVE_RUNTIME_ATTACH_DETACH,
} XrNativeRuntimeAttach;

typedef struct XrNativeCallbackContract {
    uint32_t index;
    int32_t context_index;
    XrNativeEscape escape;
    XrNativeCallbackThread thread;
    XrNativeCallbackLifetime lifetime;
    XrNativeRuntimeAttach runtime_attach;
    bool reentrant;
} XrNativeCallbackContract;

typedef struct XrNativeSymbolContract {
    XrNativeParamContract *params;
    uint32_t param_count;
    XrNativeReturnContract result;
    char **effects;
    uint32_t effect_count;
    XrNativeCallbackContract *callbacks;
    uint32_t callback_count;
    char *failure;
    char *allocation;
    char *blocking;
    char *suspend;
    char *io;
    char *sync;
    char *panic;
    char *error;
    bool complete;
} XrNativeSymbolContract;

typedef struct XrNativeUnit {
    char *name;
    XrNativeUnitKind kind;
    char **sources;         /* canonical paths inside package root */
    char **source_relpaths; /* stable manifest spellings */
    char **source_hashes;   /* audited lower-case SHA-256 */
    uint32_t source_count;
    char **include_dirs; /* canonical paths inside package root */
    uint32_t include_dir_count;
    char **defines; /* object-like NAME or NAME=VALUE only */
    uint32_t define_count;
    char **system_links; /* sealed platform identities */
    uint32_t system_link_count;
    char *language_standard;
    char *optimization;
    char *visibility;
    char *warning_policy;
    char *cpu_feature;
    char *output; /* canonical declared artifact identity; build output is target/cache scoped */
    char *purpose;
    uint64_t fingerprint;
} XrNativeUnit;

typedef struct XrNativeSymbol {
    char *xray_name;
    char *native_name;
    XrNativeSymbolKind kind;
    char *calling_convention;
    char *unit_name;
    const XrNativeUnit *unit; /* resolved, non-owning */
    XrNativeSymbolContract contract;
} XrNativeSymbol;

typedef struct XrNativeLayoutAssertion {
    char *xray_type;
    char *c_type;
    char *header;
    bool assert_size;
    bool assert_align;
    bool assert_fields;
    /* Some compiled module declares an aggregate with this name.  Distinguishes
     * "declared but has no fixed layout" (an error) from "not part of this
     * program" (vacuous for this build). */
    bool declared;
    bool resolved;
    uint32_t expected_size;
    uint32_t expected_align;
    char **field_names;
    uint32_t *field_offsets;
    uint32_t field_count;
} XrNativeLayoutAssertion;

typedef struct XrNativeCapability {
    char *type_name;
    char *request;
    char *attestation;
    char *scope;
    bool verified;
} XrNativeCapability;

typedef struct XrNativeTargetPlan {
    char *triple;
    char *profile;
    char *visibility;
    char *cpu_feature;
    char **system_links;
    uint32_t system_link_count;
    XrNativeVmPolicy vm_policy;
} XrNativeTargetPlan;

typedef struct XrCExportPlan {
    char *xray_name;
    char *symbol;
    char *visibility;
    char *abi;
    bool header;
} XrCExportPlan;

typedef struct XrLinkSymbolPlan {
    char *xray_name;
    char *section;
    bool used;
    bool weak;
} XrLinkSymbolPlan;

typedef enum XrFreestandingEntryKind {
    XR_FREESTANDING_ENTRY_START = 1,
    XR_FREESTANDING_ENTRY_INTERRUPT,
    XR_FREESTANDING_ENTRY_NAKED_STUB,
} XrFreestandingEntryKind;

typedef struct XrFreestandingEntryPlan {
    char *xray_name;
    char *symbol;
    XrFreestandingEntryKind kind;
    char *abi;
    char *section;
    char *stub;
} XrFreestandingEntryPlan;

typedef struct XrNativePackagePlan {
    char *root;
    char *name;
    char *version;
    char *license;
    char *source;
    XrNativeAuditMode audit_mode;
    XrNativeVmPolicy vm_policy;
    XrNativeUnit *units;
    uint32_t unit_count;
    XrNativeSymbol *symbols;
    uint32_t symbol_count;
    XrNativeLayoutAssertion *layouts;
    uint32_t layout_count;
    XrNativeCapability *capabilities;
    uint32_t capability_count;
    XrNativeTargetPlan *targets;
    uint32_t target_count;
    XrCExportPlan *exports;
    uint32_t export_count;
    XrLinkSymbolPlan *link_symbols;
    uint32_t link_symbol_count;
    XrFreestandingEntryPlan *entries;
    uint32_t entry_count;
    uint64_t fingerprint;
    bool valid;
    char *error;
} XrNativePackagePlan;

/* Owners retain the exact allocation policy in private aligned storage. */
typedef enum XrManifestStatus {
    XR_MANIFEST_OK, XR_MANIFEST_NOT_FOUND, XR_MANIFEST_INVALID,
    XR_MANIFEST_BAD_ARGUMENT, XR_MANIFEST_LIMIT, XR_MANIFEST_BUDGET,
    XR_MANIFEST_OUT_OF_MEMORY, XR_MANIFEST_IO, XR_MANIFEST_UNSUPPORTED
} XrManifestStatus;
typedef struct XrManifestDiagnostic { char message[256]; } XrManifestDiagnostic;

/* Missing native/export/link/freestanding tables return NOT_FOUND. No failed
 * parse publishes a partial plan. The DOM must use the same exact policy.
 * Constructors require an initially NULL output. Queries return OK with NULL
 * for missing or ambiguous short names; failures preserve their output. */
XR_FUNC XrManifestStatus xr_native_package_plan_parse_owned(const XrOsIoPolicy *policy,
    XrTomlValue *document, const char *absolute_root, XrNativePackagePlan **output,
    XrManifestDiagnostic *diagnostic);
XR_FUNC void xr_native_package_plan_free_owned(XrNativePackagePlan *plan);
XR_FUNC bool xr_native_package_plan_uses_policy(const XrNativePackagePlan *plan,
    const XrOsIoPolicy *policy);
XR_FUNC XrManifestStatus xr_native_package_find_unit_owned(const XrNativePackagePlan *,
    const char *name, const XrNativeUnit **output);
XR_FUNC XrManifestStatus xr_native_package_find_symbol_owned(const XrNativePackagePlan *,
    const char *name, const XrNativeSymbol **output);
XR_FUNC XrManifestStatus xr_native_package_find_export_owned(const XrNativePackagePlan *,
    const char *name, const XrCExportPlan **output);
XR_FUNC XrManifestStatus xr_native_package_find_link_symbol_owned(const XrNativePackagePlan *,
    const char *name, const XrLinkSymbolPlan **output);
XR_FUNC XrManifestStatus xr_native_package_find_entry_owned(const XrNativePackagePlan *,
    const char *name, const XrFreestandingEntryPlan **output);
/* Failure preserves the entire previous plan. Cleanup never charges work. */
XR_FUNC XrManifestStatus xr_native_package_configure_c_exports_owned(XrNativePackagePlan *,
    const char *public_prefix, const char *exclude_csv, XrManifestDiagnostic *diagnostic);
XR_FUNC const char *xr_native_symbol_library(const XrNativeSymbol *symbol);
struct XrAggregateLayout;
XR_FUNC XrManifestStatus xr_native_package_resolve_layout_owned(XrNativePackagePlan *,
    const char *xray_type, const struct XrAggregateLayout *layout, bool *matched);
XR_FUNC XrManifestStatus xr_native_package_note_layout_subject_owned(XrNativePackagePlan *,
    const char *xray_type);
XR_FUNC XrManifestStatus xr_native_package_validate_symbol_arity_owned(const XrNativePackagePlan *,
    const char *xray_name, uint32_t arity, XrManifestDiagnostic *diagnostic);
XR_FUNC XrManifestStatus xr_native_package_explain_owned(const XrNativePackagePlan *, FILE *out);

XR_FUNC const char *xr_native_audit_mode_name(XrNativeAuditMode mode);
XR_FUNC const char *xr_native_unit_kind_name(XrNativeUnitKind kind);
XR_FUNC const char *xr_native_param_access_name(XrNativeParamAccess access);

#endif /* XNATIVE_PACKAGE_H */
