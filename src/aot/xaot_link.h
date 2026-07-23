/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xaot_link.h - AOT link manifest
 */

#ifndef XAOT_LINK_H
#define XAOT_LINK_H

#include "../base/xdefs.h"
#include "xaot_class_layout.h"

typedef enum XaotSimdMode {
    XAOT_SIMD_AUTO = 0,
    XAOT_SIMD_SCALAR,
    XAOT_SIMD_NATIVE,
    XAOT_SIMD_NEON,
    XAOT_SIMD_SSE2,
    XAOT_SIMD_AVX2,
} XaotSimdMode;

enum {
    XAOT_SIMD_FEATURE_NEON = 1u << 0,
    XAOT_SIMD_FEATURE_SSE2 = 1u << 1,
    XAOT_SIMD_FEATURE_AVX2 = 1u << 2,
};

typedef struct XaotTarget {
    char *name;
    char *arch;
    char *os;
    char *abi;
    char *object_format;
    char *triple;
    char *cpu;
    uint16_t pointer_bits;
    char *endian;
    XrTargetDataLayout data_layout;
    XaotSimdMode simd_mode;
    uint32_t simd_features;
} XaotTarget;

typedef enum XaotLinkEntryKind {
    XAOT_LINK_GENERATED_C_FILE,
    XAOT_LINK_RUNTIME_CAP,
    XAOT_LINK_RUNTIME_OBJECT,
    XAOT_LINK_STDLIB_OBJECT,
    XAOT_LINK_STDLIB_SYMBOL,
    XAOT_LINK_NATIVE_INPUT,
    XAOT_LINK_SYSTEM_LIB,
    XAOT_LINK_DEFINE,
    XAOT_LINK_CC_FLAG,
    XAOT_LINK_LD_FLAG
} XaotLinkEntryKind;

/* Provider-neutral build intent.  These fields are facts about the requested
 * artifact; provider adapters are solely responsible for translating them to
 * GCC, Clang, Zig or MSVC argv. */
typedef enum XaotOptimizationLevel {
    XAOT_OPTIMIZATION_NONE = 0,
    XAOT_OPTIMIZATION_BASIC,
    XAOT_OPTIMIZATION_SIZE,
    XAOT_OPTIMIZATION_RELEASE,
    XAOT_OPTIMIZATION_SPEED,
} XaotOptimizationLevel;

typedef struct XaotCompileRequirements {
    XaotOptimizationLevel optimization;
    bool fp_contract_off;
    bool debug_info;
    bool frame_pointer;
    bool lto;
    bool pic;
    bool function_sections;
    bool data_sections;
    bool freestanding;
    bool stack_protector;
    bool unwind_tables;
    bool suppress_warnings;
    bool disable_machine_outliner;
    char cpu[128];
} XaotCompileRequirements;

typedef struct XaotLinkRequirements {
    bool shared;
    bool relocatable;
    bool strip;
    bool dead_strip;
    bool lto;
    bool standard_libraries;
    char entry[128];
    char linker_script[512];
} XaotLinkRequirements;

typedef struct XaotLinkManifest {
    XaotTarget target;

    XaotCompileRequirements compile;
    XaotLinkRequirements link;

    char **generated_c_files;
    uint32_t n_generated_c_files;

    char **runtime_caps;
    uint32_t n_runtime_caps;

    char **runtime_objects;
    uint32_t n_runtime_objects;

    char **stdlib_objects;
    uint32_t n_stdlib_objects;

    char **stdlib_symbols;
    uint32_t n_stdlib_symbols;

    /* Provider-neutral object/archive/shared-library paths supplied by the
     * program or a native package. They are argv inputs, never raw flags. */
    char **native_inputs;
    uint32_t n_native_inputs;

    char **system_libs;
    uint32_t n_system_libs;

    char **defines;
    uint32_t n_defines;

    char **cc_flags;
    uint32_t n_cc_flags;

    char **ld_flags;
    uint32_t n_ld_flags;

    /* Escape hatch only. Non-empty raw flags require an exact provider name;
     * they are never inferred or translated for another provider. */
    char raw_flag_provider[32];
} XaotLinkManifest;

XR_FUNC bool xaot_target_init(XaotTarget *target, const char *name);
XR_FUNC bool xaot_target_init_ex(XaotTarget *target, const char *name, const char *arch,
                                 const char *os, const char *abi, const char *object_format,
                                 const char *triple, uint16_t pointer_bits, const char *endian);
XR_FUNC void xaot_target_free(XaotTarget *target);
XR_FUNC bool xaot_simd_mode_parse(const char *text, XaotSimdMode *out);
XR_FUNC const char *xaot_simd_mode_name(XaotSimdMode mode);
XR_FUNC bool xaot_target_configure_simd(XaotTarget *target, XaotSimdMode mode, const char *cpu,
                                        char *err, size_t err_size);

XR_FUNC bool xaot_link_manifest_init(XaotLinkManifest *manifest, const XaotTarget *target);
XR_FUNC void xaot_link_manifest_free(XaotLinkManifest *manifest);
XR_FUNC bool xaot_link_manifest_set_target(XaotLinkManifest *manifest, const XaotTarget *target);
XR_FUNC bool xaot_link_manifest_add(XaotLinkManifest *manifest, XaotLinkEntryKind kind,
                                    const char *value);
XR_FUNC bool xaot_link_manifest_add_unique(XaotLinkManifest *manifest, XaotLinkEntryKind kind,
                                           const char *value);
XR_FUNC bool xaot_link_manifest_contains(const XaotLinkManifest *manifest, XaotLinkEntryKind kind,
                                         const char *value);
XR_FUNC bool xaot_link_manifest_needs_runtime(const XaotLinkManifest *manifest);

/* Returns an xr_malloc-owned JSON string. Caller releases it with xr_free(). */
XR_FUNC char *xaot_link_manifest_dump_json(const XaotLinkManifest *manifest);

#endif /* XAOT_LINK_H */
