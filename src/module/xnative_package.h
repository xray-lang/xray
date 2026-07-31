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

/* Parse the already-built TOML DOM.  A present but invalid [native] section
 * returns an owned plan with valid=false and a stable diagnostic in error. */
XR_FUNC XrNativePackagePlan *xr_native_package_plan_parse(XrTomlValue *toml_root,
                                                          const char *project_root);
XR_FUNC void xr_native_package_plan_free(XrNativePackagePlan *plan);

XR_FUNC const XrNativeUnit *xr_native_package_find_unit(const XrNativePackagePlan *plan,
                                                        const char *name);
/* Lookup accepts either an exact manifest name (module.symbol) or a unique
 * final component (symbol).  Ambiguous short names fail closed. */
XR_FUNC const XrNativeSymbol *xr_native_package_find_symbol(const XrNativePackagePlan *plan,
                                                            const char *xray_name);
XR_FUNC const XrCExportPlan *xr_native_package_find_export(const XrNativePackagePlan *plan,
                                                           const char *xray_name);
XR_FUNC const XrLinkSymbolPlan *xr_native_package_find_link_symbol(const XrNativePackagePlan *plan,
                                                                   const char *xray_name);
XR_FUNC const XrFreestandingEntryPlan *xr_native_package_find_entry(const XrNativePackagePlan *plan,
                                                                    const char *xray_name);
/* Apply build-local C ABI shaping after a manifest has been validated. Prefixing
 * affects public exports only; excluded symbols are removed from the export
 * roots so ordinary AOT reachability can discard their implementation too. */
XR_FUNC bool xr_native_package_configure_c_exports(XrNativePackagePlan *plan,
                                                   const char *public_prefix,
                                                   const char *exclude_csv, char *error,
                                                   size_t error_size);
XR_FUNC const char *xr_native_symbol_library(const XrNativeSymbol *symbol);
struct XrAggregateLayout;
XR_FUNC bool xr_native_package_resolve_layout(XrNativePackagePlan *plan, const char *xray_type,
                                              const struct XrAggregateLayout *layout);
/* Record that the program declares an aggregate named `xray_type`, whether or
 * not it turned out to have a fixed layout. */
XR_FUNC void xr_native_package_note_layout_subject(XrNativePackagePlan *plan,
                                                   const char *xray_type);
XR_FUNC bool xr_native_package_validate_symbol_arity(const XrNativePackagePlan *plan,
                                                     const char *xray_name, uint32_t arity,
                                                     char *errbuf, size_t errbuf_len);
XR_FUNC void xr_native_package_explain(const XrNativePackagePlan *plan, FILE *out);

XR_FUNC const char *xr_native_audit_mode_name(XrNativeAuditMode mode);
XR_FUNC const char *xr_native_unit_kind_name(XrNativeUnitKind kind);
XR_FUNC const char *xr_native_param_access_name(XrNativeParamAccess access);

#endif /* XNATIVE_PACKAGE_H */
