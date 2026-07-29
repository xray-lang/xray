/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtc_command.h - Provider-neutral native command intent and provider adapters
 */

#ifndef XTC_COMMAND_H
#define XTC_COMMAND_H

#include "xtc_model.h"

#include <stdbool.h>
#include <stddef.h>

typedef enum XrOptimizationLevel {
    XR_OPTIMIZATION_NONE = 0,
    XR_OPTIMIZATION_BASIC,
    XR_OPTIMIZATION_SIZE,
    XR_OPTIMIZATION_RELEASE,
    XR_OPTIMIZATION_SPEED,
} XrOptimizationLevel;

typedef enum XrFpContractMode {
    XR_FP_CONTRACT_DEFAULT = 0,
    XR_FP_CONTRACT_OFF,
    XR_FP_CONTRACT_FAST,
} XrFpContractMode;

typedef enum XrDebugInfoMode {
    XR_DEBUG_INFO_NONE = 0,
    XR_DEBUG_INFO_FULL,
} XrDebugInfoMode;

typedef enum XrVisibilityMode {
    XR_VISIBILITY_DEFAULT = 0,
    XR_VISIBILITY_HIDDEN,
} XrVisibilityMode;

typedef enum XrCpuTune {
    XR_CPU_TUNE_PORTABLE = 0,
    XR_CPU_TUNE_NATIVE,
} XrCpuTune;

typedef enum XrNativeSimdMode {
    XR_NATIVE_SIMD_DEFAULT = 0,
    XR_NATIVE_SIMD_SSE2,
    XR_NATIVE_SIMD_AVX2,
    XR_NATIVE_SIMD_AVX512,
    XR_NATIVE_SIMD_NEON,
    XR_NATIVE_SIMD_SVE,
    XR_NATIVE_SIMD_VSX,
    XR_NATIVE_SIMD_LSX,
} XrNativeSimdMode;

typedef enum XrWarningPolicy {
    XR_WARNING_POLICY_DEFAULT = 0,
    XR_WARNING_POLICY_SUPPRESS,
    XR_WARNING_POLICY_STRICT,
} XrWarningPolicy;

typedef struct XrNativeCompileSpec {
    XrOptimizationLevel optimization;
    XrFpContractMode fp_contract;
    XrDebugInfoMode debug_info;
    bool frame_pointer;
    bool lto;
    bool pic;
    bool function_sections;
    bool data_sections;
    XrVisibilityMode visibility;
    XrCpuTune cpu_tune;
    XrNativeSimdMode simd;
    XrWarningPolicy warnings;
    bool freestanding;
    bool disable_stack_protector;
    bool disable_unwind_tables;
    bool disable_machine_outliner;
    bool disable_vectorization;
    bool disable_slp_vectorization;
    const char *cpu;
    const char *language_standard;
} XrNativeCompileSpec;

typedef struct XrNativeLinkSpec {
    bool shared;
    bool relocatable;
    bool strip;
    bool dead_strip;
    bool lto;
    bool no_standard_libraries;
    const char *entry;
    const char *linker_script;
} XrNativeLinkSpec;

typedef struct XrToolchainArgSink {
    void *context;
    bool (*add)(void *context, const char *arg, char *err, size_t err_size);
    bool (*add_joined)(void *context, const char *prefix, const char *value, char *err,
                       size_t err_size);
} XrToolchainArgSink;

typedef struct XrToolchainProviderVTable {
    XrToolchainProviderId id;
    bool (*emit_driver)(const XrToolchainSelection *selection, const XrToolchainTarget *target,
                        XrToolchainArgSink *sink, char *err, size_t err_size);
    bool (*emit_compile)(const XrToolchainSelection *selection, const XrNativeCompileSpec *spec,
                         XrToolchainArgSink *sink, char *err, size_t err_size);
    bool (*emit_link)(const XrToolchainSelection *selection, const XrToolchainTarget *target,
                      const XrNativeLinkSpec *spec, XrToolchainArgSink *sink, char *err,
                      size_t err_size);
} XrToolchainProviderVTable;

XR_FUNC const XrToolchainProviderVTable *xtc_command_provider(XrToolchainProviderId provider);
XR_FUNC bool xtc_command_emit_driver(const XrToolchainSelection *selection,
                                     const XrToolchainTarget *target, XrToolchainArgSink *sink,
                                     char *err, size_t err_size);
XR_FUNC bool xtc_command_emit_compile(const XrToolchainSelection *selection,
                                      const XrNativeCompileSpec *spec, XrToolchainArgSink *sink,
                                      char *err, size_t err_size);
XR_FUNC bool xtc_command_emit_link(const XrToolchainSelection *selection,
                                   const XrToolchainTarget *target, const XrNativeLinkSpec *spec,
                                   XrToolchainArgSink *sink, char *err, size_t err_size);

/* Provider-dialect primitives for paths and logical names. */
XR_FUNC bool xtc_command_emit_compile_io(XrToolchainProviderId provider, const char *source,
                                         const char *object, XrToolchainArgSink *sink, char *err,
                                         size_t err_size);
XR_FUNC bool xtc_command_emit_assembly_io(XrToolchainProviderId provider, const char *source,
                                          const char *assembly, const char *object,
                                          XrToolchainArgSink *sink, char *err, size_t err_size);
XR_FUNC bool xtc_command_emit_link_output(XrToolchainProviderId provider, const char *output,
                                          XrToolchainArgSink *sink, char *err, size_t err_size);
XR_FUNC bool xtc_command_emit_include(XrToolchainProviderId provider, const char *path,
                                      XrToolchainArgSink *sink, char *err, size_t err_size);
XR_FUNC bool xtc_command_emit_define(XrToolchainProviderId provider, const char *value,
                                     XrToolchainArgSink *sink, char *err, size_t err_size);
XR_FUNC bool xtc_command_emit_system_library(XrToolchainProviderId provider, const char *name,
                                             XrToolchainArgSink *sink, char *err, size_t err_size);
XR_FUNC bool xtc_command_emit_sysroot(XrToolchainProviderId provider, const char *path,
                                      XrToolchainArgSink *sink, char *err, size_t err_size);
XR_FUNC bool xtc_command_emit_library_search(XrToolchainProviderId provider, const char *path,
                                             XrToolchainArgSink *sink, char *err, size_t err_size);

#endif /* XTC_COMMAND_H */
