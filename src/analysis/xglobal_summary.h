/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xglobal_summary.h - Whole-program summary/evidence data model
 */

#ifndef XGLOBAL_SUMMARY_H
#define XGLOBAL_SUMMARY_H

#include "../base/xdefs.h"
#include <stdint.h>

typedef uint32_t XgModuleId;
typedef uint32_t XgDeclId;
typedef uint32_t XgFuncId;
typedef uint32_t XgTypeId;
typedef uint32_t XgClassId;
typedef uint32_t XgInterfaceId;
typedef uint32_t XgMethodId;
typedef uint32_t XgFieldId;
typedef uint32_t XgCallsiteId;
typedef uint32_t XgLinkId;

#define XG_LINK_DEP_NAME_MAX 512

enum {
    XG_NO_ID = 0,
};

typedef enum XgBuildProfile {
    XG_BUILD_CHECK = 0,
    XG_BUILD_DEV,
    XG_BUILD_NATIVE_RELEASE,
    XG_BUILD_FREESTANDING,
    XG_BUILD_DEBUG_TOOLING,
} XgBuildProfile;

typedef enum XgDeclKind {
    XG_DECL_FUNC = 1,
    XG_DECL_CLASS,
    XG_DECL_STRUCT,
    XG_DECL_UNION,
    XG_DECL_ENUM,
    XG_DECL_INTERFACE,
    XG_DECL_GLOBAL,
} XgDeclKind;

enum {
    XG_DECL_PUBLIC = 1u << 0,
    XG_DECL_EXPORT = 1u << 1,
    XG_DECL_NATIVE = 1u << 2,
    XG_DECL_EXTERN = 1u << 3,
    XG_DECL_C_EXPORT = 1u << 4,
    XG_DECL_DERIVE = 1u << 5,
    XG_DECL_FINAL = 1u << 6,
};

enum {
    XG_CLASS_EXPLICIT_FINAL = 1u << 0,
    XG_CLASS_HAS_SUBCLASS = 1u << 1,
    XG_CLASS_INFERRED_FINAL = 1u << 2,
    XG_CLASS_NATIVE = 1u << 3,
    XG_CLASS_RUNTIME_ONLY = 1u << 4,
};

enum {
    XG_METHOD_STATIC = 1u << 0,
    XG_METHOD_CONSTRUCTOR = 1u << 1,
    XG_METHOD_DIRECT_ONLY = 1u << 2,
    XG_METHOD_OVERRIDDEN = 1u << 3,
};

typedef enum XgCallsiteKind {
    XG_CALL_DIRECT_FUNC = 1,
    XG_CALL_METHOD,
    XG_CALL_INTERFACE,
    XG_CALL_CLOSURE,
    XG_CALL_NATIVE,
    XG_CALL_EXTERN,
} XgCallsiteKind;

typedef enum XgLinkDependencyKind {
    XG_LINK_DEP_EXTERN_DYLIB = 1,
    XG_LINK_DEP_STDLIB_SYMBOL,
} XgLinkDependencyKind;

enum {
    XG_CALL_MAY_THROW = 1u << 0,
    XG_CALL_MAY_SUSPEND = 1u << 1,
    XG_CALL_USES_DEFAULT_ARGS = 1u << 2,
};

enum {
    XG_CAP_COROUTINE = 1u << 0,
    XG_CAP_CHANNEL = 1u << 1,
    XG_CAP_EXCEPTION = 1u << 2,
    XG_CAP_NATIVE = 1u << 3,
    XG_CAP_EXTERN = 1u << 4,
    XG_CAP_OBJECTS = 1u << 5,
    XG_CAP_DEEP_COPY = 1u << 6,
    XG_CAP_INSTANCEOF = 1u << 7,
    XG_CAP_SYS_THREAD = 1u << 8,
    XG_CAP_SCOPE = 1u << 9,
    XG_CAP_TIMER = 1u << 10,
    XG_CAP_NETPOLL = 1u << 11,
    XG_CAP_TASK = 1u << 12,
    XG_CAP_ATOMIC = 1u << 13,
    XG_CAP_WORK_QUEUE = 1u << 14,
    XG_CAP_RESULT_GROUP = 1u << 15,
    XG_CAP_COUNTDOWN_LATCH = 1u << 16,
    XG_CAP_SEMAPHORE = 1u << 17,
    XG_CAP_EVENT_COUNT = 1u << 18,
    XG_CAP_GENERATOR = 1u << 19,
    XG_CAP_STACKTRACE = 1u << 20,
};

enum {
    XG_METADATA_TYPENAME = 1u << 0,
    XG_METADATA_DERIVE = 1u << 1,
    XG_METADATA_DEBUG = 1u << 2,
    XG_METADATA_TOOLING = 1u << 3,
};

enum {
    XG_STATIC_DATA_COMPTIME_VALUE = 1u << 0,
    XG_STATIC_DATA_FIXED_LAYOUT = 1u << 1,
    XG_STATIC_DATA_RODATA = 1u << 2,
    XG_STATIC_DATA_FREESTANDING_SAFE = 1u << 3,
    XG_STATIC_DATA_RUNTIME_INIT = 1u << 4,
};

typedef struct XgBuildKey {
    uint64_t source_hash;
    uint64_t compiler_semver_hash;
    uint64_t profile_hash;
    uint64_t imported_summary_hash;
    XgModuleId module_id;
    uint32_t profile;
} XgBuildKey;

typedef struct XgDeclSummary {
    XgModuleId module_id;
    XgDeclId decl_id;
    uint8_t kind;
    uint32_t flags;
    uint32_t name_id;
    uint32_t type_key;
    uint32_t signature_key;
    uint32_t source_span_id;
} XgDeclSummary;

typedef struct XgClassSummary {
    XgModuleId module_id;
    XgDeclId decl_id;
    XgClassId class_id;
    XgClassId parent_class_id;
    uint32_t name_id;
    uint32_t flags;
    uint32_t field_start;
    uint32_t field_count;
    uint32_t method_start;
    uint32_t method_count;
    uint32_t interface_start;
    uint32_t interface_count;
    uint8_t decl_kind;
} XgClassSummary;

typedef struct XgMethodSummary {
    XgMethodId method_id;
    XgClassId owner_class_id;
    uint32_t name_id;
    uint32_t signature_key;
    XgMethodId override_of;
    uint32_t default_arg_contract_id;
    uint32_t flags;
} XgMethodSummary;

typedef struct XgInterfaceImplSummary {
    XgClassId implementor_class_id;
    XgInterfaceId interface_id;
    uint32_t name_id;
    uint32_t type_key;
    uint32_t source_span_id;
    uint32_t flags;
} XgInterfaceImplSummary;

typedef struct XgBodySummary {
    XgFuncId func_id;
    uint64_t body_hash;
    uint32_t effect_bits;
    uint32_t escape_bits;
    uint32_t capability_bits;
    uint32_t callsite_start;
    uint32_t callsite_count;
    uint32_t metadata_use_bits;
    uint32_t static_data_use_bits;
} XgBodySummary;

typedef struct XgCallsiteSummary {
    XgCallsiteId callsite_id;
    XgFuncId owner_func_id;
    uint32_t source_span_id;
    uint32_t body_ordinal;
    uint8_t kind;
    XgFuncId static_target_func_id;
    XgClassId receiver_static_class_id;
    XgInterfaceId receiver_static_interface_id;
    XgMethodId method_id;
    uint32_t method_name_id;
    uint32_t method_signature_key;
    uint32_t arg_type_key_start;
    uint16_t arg_count;
    uint32_t flags;
} XgCallsiteSummary;

typedef struct XgLinkDependencySummary {
    XgLinkId link_id;
    XgModuleId module_id;
    XgDeclId decl_id;
    uint32_t source_span_id;
    uint32_t name_id;
    uint8_t kind;
    uint32_t flags;
    char name[XG_LINK_DEP_NAME_MAX];
} XgLinkDependencySummary;

typedef struct XgGlobalEvidence {
    XgBuildKey key;

    XgDeclSummary *decls;
    XgClassSummary *classes;
    XgMethodSummary *methods;
    XgInterfaceImplSummary *interface_impls;
    XgBodySummary *bodies;
    XgCallsiteSummary *callsites;
    XgLinkDependencySummary *link_deps;

    uint32_t ndecls;
    uint32_t nclasses;
    uint32_t nmethods;
    uint32_t ninterface_impls;
    uint32_t nbodies;
    uint32_t ncallsites;
    uint32_t nlink_deps;

    uint32_t decl_cap;
    uint32_t class_cap;
    uint32_t method_cap;
    uint32_t interface_impl_cap;
    uint32_t body_cap;
    uint32_t callsite_cap;
    uint32_t link_dep_cap;
} XgGlobalEvidence;

XR_FUNC uint32_t xg_name_id(const char *name);
XR_FUNC const char *xg_build_profile_name(uint32_t profile);
XR_FUNC const char *xg_decl_kind_name(uint8_t kind);
XR_FUNC const char *xg_callsite_kind_name(uint8_t kind);
XR_FUNC const char *xg_link_dependency_kind_name(uint8_t kind);
XR_FUNC const char *xg_capability_name(uint32_t capability);
XR_FUNC const uint32_t *xg_capability_catalog(uint32_t *out_count);
XR_FUNC const char *xg_metadata_name(uint32_t metadata);
XR_FUNC const uint32_t *xg_metadata_catalog(uint32_t *out_count);
XR_FUNC const char *xg_static_data_name(uint32_t static_data);
XR_FUNC const uint32_t *xg_static_data_catalog(uint32_t *out_count);

XR_FUNC void xg_global_evidence_init(XgGlobalEvidence *evidence, XgBuildKey key);
XR_FUNC void xg_global_evidence_free(XgGlobalEvidence *evidence);

XR_FUNC bool xg_global_evidence_reserve_decls(XgGlobalEvidence *evidence, uint32_t capacity);
XR_FUNC bool xg_global_evidence_reserve_classes(XgGlobalEvidence *evidence, uint32_t capacity);
XR_FUNC bool xg_global_evidence_reserve_methods(XgGlobalEvidence *evidence, uint32_t capacity);
XR_FUNC bool xg_global_evidence_reserve_interface_impls(XgGlobalEvidence *evidence,
                                                        uint32_t capacity);
XR_FUNC bool xg_global_evidence_reserve_bodies(XgGlobalEvidence *evidence, uint32_t capacity);
XR_FUNC bool xg_global_evidence_reserve_callsites(XgGlobalEvidence *evidence, uint32_t capacity);
XR_FUNC bool xg_global_evidence_reserve_link_deps(XgGlobalEvidence *evidence, uint32_t capacity);

XR_FUNC XgDeclSummary *xg_global_evidence_add_decl(XgGlobalEvidence *evidence,
                                                   const XgDeclSummary *summary);
XR_FUNC XgClassSummary *xg_global_evidence_add_class(XgGlobalEvidence *evidence,
                                                     const XgClassSummary *summary);
XR_FUNC XgMethodSummary *xg_global_evidence_add_method(XgGlobalEvidence *evidence,
                                                       const XgMethodSummary *summary);
XR_FUNC XgInterfaceImplSummary *
xg_global_evidence_add_interface_impl(XgGlobalEvidence *evidence,
                                      const XgInterfaceImplSummary *summary);
XR_FUNC XgBodySummary *xg_global_evidence_add_body(XgGlobalEvidence *evidence,
                                                   const XgBodySummary *summary);
XR_FUNC XgCallsiteSummary *xg_global_evidence_add_callsite(XgGlobalEvidence *evidence,
                                                           const XgCallsiteSummary *summary);
XR_FUNC XgLinkDependencySummary *
xg_global_evidence_add_link_dependency(XgGlobalEvidence *evidence,
                                       const XgLinkDependencySummary *summary);
XR_FUNC const XgCallsiteSummary *xg_global_evidence_find_callsite(const XgGlobalEvidence *evidence,
                                                                  XgCallsiteId callsite_id);

XR_FUNC uint64_t xg_global_evidence_hash(const XgGlobalEvidence *evidence);
XR_FUNC char *xg_global_evidence_dump(const XgGlobalEvidence *evidence);

#endif  // XGLOBAL_SUMMARY_H
