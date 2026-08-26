/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtc_command.c - Provider-specific argv adapters for semantic native intent
 */

#include "xtc_command.h"

#include <stdio.h>
#include <string.h>

static bool command_error(char *err, size_t err_size, const char *message) {
    if (err && err_size)
        snprintf(err, err_size, "%s", message);
    return false;
}

static bool add(XrToolchainArgSink *sink, const char *arg, char *err, size_t err_size) {
    if (!sink || !sink->add)
        return command_error(err, err_size, "missing toolchain argument sink");
    return sink->add(sink->context, arg, err, err_size);
}

static bool joined(XrToolchainArgSink *sink, const char *prefix, const char *value, char *err,
                   size_t err_size) {
    if (!value || !value[0])
        return true;
    if (!sink || !sink->add_joined)
        return command_error(err, err_size, "missing toolchain joined-argument sink");
    return sink->add_joined(sink->context, prefix, value, err, err_size);
}

static bool emit_gnu_driver(const XrToolchainSelection *selection, const XrToolchainTarget *target,
                            XrToolchainArgSink *sink, char *err, size_t err_size) {
    if (!selection || !selection->program || !selection->program[0])
        return command_error(err, err_size, "missing verified provider executable");
    if (!add(sink, selection->program, err, err_size))
        return false;
    if (selection->provider == XR_TOOLCHAIN_PROVIDER_ZIG) {
        if (!add(sink, "cc", err, err_size))
            return false;
        /* Zig defaults to the Windows GNU ABI on Windows. Keep an explicitly
         * selected native MSVC ABI exact so it can consume the verified COFF
         * runtime archives shipped with the SDK. */
        bool needs_explicit_target =
            target && (!target->is_native || target->abi == XR_TOOLCHAIN_TARGET_ABI_MSVC);
        if (needs_explicit_target) {
            if (!target->zig_triple || !target->zig_triple[0])
                return command_error(err, err_size, "cross target has no Zig triple");
            if (!add(sink, "-target", err, err_size) ||
                !add(sink, target->zig_triple, err, err_size))
                return false;
            if (target->cpu && target->cpu[0] &&
                !joined(sink, "-mcpu=", target->cpu, err, err_size))
                return false;
        }
    } else if (selection->provider == XR_TOOLCHAIN_PROVIDER_APPLE_CLANG &&
               selection->system_sdk[0]) {
        if (!add(sink, "-isysroot", err, err_size) ||
            !add(sink, selection->system_sdk, err, err_size))
            return false;
    }
    return true;
}

static bool emit_msvc_driver(const XrToolchainSelection *selection, const XrToolchainTarget *target,
                             XrToolchainArgSink *sink, char *err, size_t err_size) {
    if (!selection || !selection->program || !selection->program[0])
        return command_error(err, err_size, "missing verified MSVC executable");
    if (target && target->abi != XR_TOOLCHAIN_TARGET_ABI_MSVC)
        return command_error(err, err_size, "MSVC provider requires a windows-msvc target ABI");
    return add(sink, selection->program, err, err_size);
}

static const char *gnu_optimization(XrOptimizationLevel level) {
    switch (level) {
        case XR_OPTIMIZATION_NONE:
            return "-O0";
        case XR_OPTIMIZATION_BASIC:
            return "-O1";
        case XR_OPTIMIZATION_SIZE:
            return "-Os";
        case XR_OPTIMIZATION_RELEASE:
            return "-O2";
        case XR_OPTIMIZATION_SPEED:
            return "-O3";
    }
    return NULL;
}

static const char *msvc_optimization(XrOptimizationLevel level) {
    switch (level) {
        case XR_OPTIMIZATION_NONE:
            return "/Od";
        case XR_OPTIMIZATION_BASIC:
            return "/O1";
        case XR_OPTIMIZATION_SIZE:
            return "/O1";
        case XR_OPTIMIZATION_RELEASE:
            return "/O2";
        case XR_OPTIMIZATION_SPEED:
            return "/O2";
    }
    return NULL;
}

static bool emit_gnu_simd(XrNativeSimdMode simd, XrToolchainArgSink *sink, char *err,
                          size_t err_size) {
    switch (simd) {
        case XR_NATIVE_SIMD_DEFAULT:
        case XR_NATIVE_SIMD_NEON:
            /* NEON is part of the supported AArch64 baseline. */
            return true;
        case XR_NATIVE_SIMD_SSE2:
            return add(sink, "-msse2", err, err_size);
        case XR_NATIVE_SIMD_AVX2:
            return add(sink, "-mavx2", err, err_size);
        case XR_NATIVE_SIMD_AVX512:
            return add(sink, "-mavx512f", err, err_size);
        case XR_NATIVE_SIMD_SVE:
            return add(sink, "-march=armv8-a+sve", err, err_size);
        case XR_NATIVE_SIMD_VSX:
            return add(sink, "-mvsx", err, err_size);
        case XR_NATIVE_SIMD_LSX:
            return add(sink, "-mlsx", err, err_size);
    }
    return command_error(err, err_size, "unsupported GNU native SIMD compile intent");
}

static bool emit_msvc_simd(XrNativeSimdMode simd, XrToolchainArgSink *sink, char *err,
                           size_t err_size) {
    switch (simd) {
        case XR_NATIVE_SIMD_DEFAULT:
        case XR_NATIVE_SIMD_SSE2:
            /* SSE2 is part of the supported x86_64 MSVC baseline. */
            return true;
        case XR_NATIVE_SIMD_AVX2:
            return add(sink, "/arch:AVX2", err, err_size);
        case XR_NATIVE_SIMD_AVX512:
            return add(sink, "/arch:AVX512", err, err_size);
        case XR_NATIVE_SIMD_NEON:
        case XR_NATIVE_SIMD_SVE:
        case XR_NATIVE_SIMD_VSX:
        case XR_NATIVE_SIMD_LSX:
            return command_error(err, err_size,
                                 "requested SIMD compile intent is unsupported by MSVC provider");
    }
    return command_error(err, err_size, "unsupported MSVC native SIMD compile intent");
}

static bool emit_gnu_compile(const XrToolchainSelection *selection, const XrNativeCompileSpec *spec,
                             XrToolchainArgSink *sink, char *err, size_t err_size) {
    if (!spec)
        return command_error(err, err_size, "missing native compile specification");
    /* Xray's hosted Linux AOT runtime uses the POSIX/GNU interfaces exposed by
     * glibc and musl under their feature-test macros (clock_gettime,
     * pthread_rwlock_t, getaddrinfo, MAP_ANONYMOUS, and friends). C11 mode
     * otherwise hides those declarations, so make the hosted Linux contract
     * explicit for every GNU-style provider instead of relying on shell flags. */
    if (selection && selection->target.os == XR_TOOLCHAIN_TARGET_OS_LINUX &&
        !add(sink, "-D_GNU_SOURCE", err, err_size))
        return false;
    if (selection && selection->provider == XR_TOOLCHAIN_PROVIDER_ZIG &&
        selection->target.abi == XR_TOOLCHAIN_TARGET_ABI_MSVC &&
        !add(sink, "-D_CRT_SECURE_NO_WARNINGS", err, err_size))
        return false;
    if (!add(sink, gnu_optimization(spec->optimization), err, err_size))
        return false;
    if (spec->fp_contract == XR_FP_CONTRACT_OFF && !add(sink, "-ffp-contract=off", err, err_size))
        return false;
    if (spec->fp_contract == XR_FP_CONTRACT_FAST && !add(sink, "-ffp-contract=fast", err, err_size))
        return false;
    if (spec->debug_info == XR_DEBUG_INFO_FULL && !add(sink, "-g", err, err_size))
        return false;
    if (spec->frame_pointer && !add(sink, "-fno-omit-frame-pointer", err, err_size))
        return false;
    if (spec->lto && !add(sink, "-flto", err, err_size))
        return false;
    if (spec->pic && !add(sink, "-fPIC", err, err_size))
        return false;
    if (spec->function_sections && !add(sink, "-ffunction-sections", err, err_size))
        return false;
    if (spec->data_sections && !add(sink, "-fdata-sections", err, err_size))
        return false;
    if (spec->visibility == XR_VISIBILITY_HIDDEN &&
        !add(sink, "-fvisibility=hidden", err, err_size))
        return false;
    if (spec->cpu_tune == XR_CPU_TUNE_NATIVE && !add(sink, "-march=native", err, err_size))
        return false;
    if (spec->cpu && spec->cpu[0]) {
        bool uses_mcpu =
            selection && (selection->target.arch == XR_TOOLCHAIN_TARGET_ARCH_ARM ||
                          selection->target.arch == XR_TOOLCHAIN_TARGET_ARCH_AARCH64 ||
                          selection->target.arch == XR_TOOLCHAIN_TARGET_ARCH_POWERPC64 ||
                          selection->target.arch == XR_TOOLCHAIN_TARGET_ARCH_THUMB);
        if (!joined(sink, uses_mcpu ? "-mcpu=" : "-march=", spec->cpu, err, err_size))
            return false;
    }
    /* Explicit SIMD intent follows the CPU baseline so a narrower --cpu does
     * not silently disable the requested ISA. */
    if (!emit_gnu_simd(spec->simd, sink, err, err_size))
        return false;
    bool gcc = selection && selection->provider == XR_TOOLCHAIN_PROVIDER_GCC;
    if (spec->disable_vectorization &&
        !add(sink, gcc ? "-fno-tree-vectorize" : "-fno-vectorize", err, err_size))
        return false;
    if (spec->disable_slp_vectorization &&
        !add(sink, gcc ? "-fno-tree-slp-vectorize" : "-fno-slp-vectorize", err, err_size))
        return false;
    if (spec->warnings == XR_WARNING_POLICY_SUPPRESS && !add(sink, "-w", err, err_size))
        return false;
    if (spec->warnings == XR_WARNING_POLICY_STRICT &&
        (!add(sink, "-Wall", err, err_size) || !add(sink, "-Wextra", err, err_size) ||
         !add(sink, "-Werror", err, err_size)))
        return false;
    if (spec->freestanding && !add(sink, "-ffreestanding", err, err_size))
        return false;
    if (spec->disable_stack_protector && !add(sink, "-fno-stack-protector", err, err_size))
        return false;
    if (spec->disable_unwind_tables &&
        (!add(sink, "-fno-unwind-tables", err, err_size) ||
         !add(sink, "-fno-asynchronous-unwind-tables", err, err_size)))
        return false;
    if (spec->disable_machine_outliner && !add(sink, "-mno-outline", err, err_size))
        return false;
    return !spec->language_standard ||
           joined(sink, "-std=", spec->language_standard, err, err_size);
}

static bool emit_msvc_compile(const XrToolchainSelection *selection,
                              const XrNativeCompileSpec *spec, XrToolchainArgSink *sink, char *err,
                              size_t err_size) {
    (void) selection;
    if (!spec)
        return command_error(err, err_size, "missing native compile specification");
    if (spec->pic || spec->visibility == XR_VISIBILITY_HIDDEN ||
        spec->cpu_tune == XR_CPU_TUNE_NATIVE || spec->freestanding ||
        spec->disable_stack_protector || spec->disable_unwind_tables ||
        spec->disable_machine_outliner || spec->disable_vectorization ||
        spec->disable_slp_vectorization || (spec->cpu && spec->cpu[0]))
        return command_error(err, err_size,
                             "requested native compile intent is unsupported by MSVC provider");
    if (!add(sink, "/nologo", err, err_size) || !add(sink, "/utf-8", err, err_size) ||
        !add(sink, msvc_optimization(spec->optimization), err, err_size))
        return false;
    if (spec->fp_contract == XR_FP_CONTRACT_OFF && !add(sink, "/fp:strict", err, err_size))
        return false;
    if (spec->fp_contract == XR_FP_CONTRACT_FAST && !add(sink, "/fp:fast", err, err_size))
        return false;
    if (spec->debug_info == XR_DEBUG_INFO_FULL && !add(sink, "/Zi", err, err_size))
        return false;
    if (spec->frame_pointer && !add(sink, "/Oy-", err, err_size))
        return false;
    if (spec->lto && !add(sink, "/GL", err, err_size))
        return false;
    if (spec->function_sections && !add(sink, "/Gy", err, err_size))
        return false;
    if (spec->data_sections && !add(sink, "/Gw", err, err_size))
        return false;
    if (!emit_msvc_simd(spec->simd, sink, err, err_size))
        return false;
    if (spec->warnings == XR_WARNING_POLICY_SUPPRESS && !add(sink, "/w", err, err_size))
        return false;
    if (spec->warnings == XR_WARNING_POLICY_STRICT &&
        (!add(sink, "/W4", err, err_size) || !add(sink, "/WX", err, err_size)))
        return false;
    if (spec->language_standard) {
        if (strcmp(spec->language_standard, "c11") != 0 &&
            strcmp(spec->language_standard, "c17") != 0)
            return command_error(err, err_size, "MSVC provider supports only c11 or c17 here");
        if (!joined(sink, "/std:", spec->language_standard, err, err_size) ||
            !add(sink, "/experimental:c11atomics", err, err_size))
            return false;
    }
    return true;
}

static bool emit_gnu_link(const XrToolchainSelection *selection, const XrToolchainTarget *target,
                          const XrNativeLinkSpec *spec, XrToolchainArgSink *sink, char *err,
                          size_t err_size) {
    (void) selection;
    if (!spec)
        return command_error(err, err_size, "missing native link specification");
    if (spec->relocatable && !add(sink, "-r", err, err_size))
        return false;
    if (spec->shared && !spec->relocatable &&
        !add(sink,
             target && target->os == XR_TOOLCHAIN_TARGET_OS_DARWIN ? "-dynamiclib" : "-shared", err,
             err_size))
        return false;
    if (spec->resolve_from_host && target && target->os == XR_TOOLCHAIN_TARGET_OS_DARWIN &&
        !add(sink, "-Wl,-undefined,dynamic_lookup", err, err_size))
        return false;
    if (spec->lto && !add(sink, "-flto", err, err_size))
        return false;
    if (spec->dead_strip &&
        !add(sink,
             target && target->os == XR_TOOLCHAIN_TARGET_OS_DARWIN ? "-Wl,-dead_strip"
                                                                   : "-Wl,--gc-sections",
             err, err_size))
        return false;
    if (spec->strip && (!add(sink, "-Wl,-S", err, err_size) || !add(sink, "-Wl,-x", err, err_size)))
        return false;
    if (spec->entry && !joined(sink, "-Wl,-e,", spec->entry, err, err_size))
        return false;
    if (spec->linker_script && !joined(sink, "-Wl,-T,", spec->linker_script, err, err_size))
        return false;
    if (spec->no_standard_libraries && !add(sink, "-nostdlib", err, err_size))
        return false;
    return true;
}

static bool emit_msvc_link(const XrToolchainSelection *selection, const XrToolchainTarget *target,
                           const XrNativeLinkSpec *spec, XrToolchainArgSink *sink, char *err,
                           size_t err_size) {
    (void) selection;
    (void) target;
    if (!spec)
        return command_error(err, err_size, "missing native link specification");
    if (spec->relocatable || spec->resolve_from_host || spec->strip || spec->linker_script ||
        spec->no_standard_libraries)
        return command_error(err, err_size,
                             "requested native link intent is unsupported by MSVC provider");
    if (spec->shared && !add(sink, "/LD", err, err_size))
        return false;
    /* cl.exe forwards arguments following /link to link.exe. Keep compiler
     * switches (notably /LD) before the delimiter. */
    if (!add(sink, "/link", err, err_size))
        return false;
    if (spec->lto && !add(sink, "/LTCG", err, err_size))
        return false;
    if (spec->dead_strip && !add(sink, "/OPT:REF", err, err_size))
        return false;
    if (spec->entry && !joined(sink, "/ENTRY:", spec->entry, err, err_size))
        return false;
    return true;
}

static const XrToolchainProviderVTable gnu_vtables[] = {
    {XR_TOOLCHAIN_PROVIDER_APPLE_CLANG, emit_gnu_driver, emit_gnu_compile, emit_gnu_link},
    {XR_TOOLCHAIN_PROVIDER_GCC, emit_gnu_driver, emit_gnu_compile, emit_gnu_link},
    {XR_TOOLCHAIN_PROVIDER_LLVM_CLANG, emit_gnu_driver, emit_gnu_compile, emit_gnu_link},
    {XR_TOOLCHAIN_PROVIDER_ZIG, emit_gnu_driver, emit_gnu_compile, emit_gnu_link},
};

static const XrToolchainProviderVTable msvc_vtable = {XR_TOOLCHAIN_PROVIDER_MSVC, emit_msvc_driver,
                                                      emit_msvc_compile, emit_msvc_link};

XR_FUNC const XrToolchainProviderVTable *xtc_command_provider(XrToolchainProviderId provider) {
    if (provider == XR_TOOLCHAIN_PROVIDER_MSVC)
        return &msvc_vtable;
    for (size_t i = 0; i < sizeof(gnu_vtables) / sizeof(gnu_vtables[0]); i++) {
        if (gnu_vtables[i].id == provider)
            return &gnu_vtables[i];
    }
    return NULL;
}

XR_FUNC bool xtc_command_emit_driver(const XrToolchainSelection *selection,
                                     const XrToolchainTarget *target, XrToolchainArgSink *sink,
                                     char *err, size_t err_size) {
    const XrToolchainProviderVTable *provider =
        selection ? xtc_command_provider(selection->provider) : NULL;
    if (!provider)
        return command_error(err, err_size, "unsupported toolchain provider command dialect");
    return provider->emit_driver(selection, target, sink, err, err_size);
}

XR_FUNC bool xtc_command_emit_compile(const XrToolchainSelection *selection,
                                      const XrNativeCompileSpec *spec, XrToolchainArgSink *sink,
                                      char *err, size_t err_size) {
    const XrToolchainProviderVTable *provider =
        selection ? xtc_command_provider(selection->provider) : NULL;
    if (!provider)
        return command_error(err, err_size, "unsupported toolchain provider command dialect");
    return provider->emit_compile(selection, spec, sink, err, err_size);
}

XR_FUNC bool xtc_command_emit_link(const XrToolchainSelection *selection,
                                   const XrToolchainTarget *target, const XrNativeLinkSpec *spec,
                                   XrToolchainArgSink *sink, char *err, size_t err_size) {
    const XrToolchainProviderVTable *provider =
        selection ? xtc_command_provider(selection->provider) : NULL;
    if (!provider)
        return command_error(err, err_size, "unsupported toolchain provider command dialect");
    return provider->emit_link(selection, target, spec, sink, err, err_size);
}

XR_FUNC bool xtc_command_emit_compile_io(XrToolchainProviderId provider, const char *source,
                                         const char *object, XrToolchainArgSink *sink, char *err,
                                         size_t err_size) {
    if (provider == XR_TOOLCHAIN_PROVIDER_MSVC)
        return add(sink, "/c", err, err_size) && add(sink, source, err, err_size) &&
               joined(sink, "/Fo", object, err, err_size);
    return add(sink, "-c", err, err_size) && add(sink, source, err, err_size) &&
           add(sink, "-o", err, err_size) && add(sink, object, err, err_size);
}

XR_FUNC bool xtc_command_emit_assembly_io(XrToolchainProviderId provider, const char *source,
                                          const char *assembly, const char *object,
                                          XrToolchainArgSink *sink, char *err, size_t err_size) {
    if (!source || !source[0] || !assembly || !assembly[0])
        return command_error(err, err_size, "missing native assembly input or output");
    if (provider == XR_TOOLCHAIN_PROVIDER_MSVC) {
        if (!object || !object[0])
            return command_error(err, err_size, "MSVC assembly emission requires an object path");
        return add(sink, "/c", err, err_size) && add(sink, "/FAs", err, err_size) &&
               joined(sink, "/Fa", assembly, err, err_size) &&
               joined(sink, "/Fo", object, err, err_size) && add(sink, source, err, err_size);
    }
    return add(sink, "-S", err, err_size) && add(sink, source, err, err_size) &&
           add(sink, "-o", err, err_size) && add(sink, assembly, err, err_size);
}

XR_FUNC bool xtc_command_emit_link_output(XrToolchainProviderId provider, const char *output,
                                          XrToolchainArgSink *sink, char *err, size_t err_size) {
    if (provider == XR_TOOLCHAIN_PROVIDER_MSVC)
        return joined(sink, "/Fe", output, err, err_size);
    return add(sink, "-o", err, err_size) && add(sink, output, err, err_size);
}

XR_FUNC bool xtc_command_emit_intermediate_object_output(XrToolchainProviderId provider,
                                                         const char *object,
                                                         XrToolchainArgSink *sink, char *err,
                                                         size_t err_size) {
    if (!object || !object[0])
        return command_error(err, err_size, "missing intermediate object output path");
    /* GNU-style drivers keep a combined compile-and-link object private.  MSVC
     * writes it into the process working directory unless /Fo owns the path. */
    if (provider == XR_TOOLCHAIN_PROVIDER_MSVC)
        return joined(sink, "/Fo", object, err, err_size);
    return true;
}

XR_FUNC bool xtc_command_emit_include(XrToolchainProviderId provider, const char *path,
                                      XrToolchainArgSink *sink, char *err, size_t err_size) {
    return joined(sink, provider == XR_TOOLCHAIN_PROVIDER_MSVC ? "/I" : "-I", path, err, err_size);
}

XR_FUNC bool xtc_command_emit_define(XrToolchainProviderId provider, const char *value,
                                     XrToolchainArgSink *sink, char *err, size_t err_size) {
    return joined(sink, provider == XR_TOOLCHAIN_PROVIDER_MSVC ? "/D" : "-D", value, err, err_size);
}

XR_FUNC bool xtc_command_emit_system_library(XrToolchainProviderId provider,
                                             const XrToolchainTarget *target, const char *name,
                                             XrToolchainArgSink *sink, char *err, size_t err_size) {
    if (provider == XR_TOOLCHAIN_PROVIDER_MSVC) {
        static const struct {
            const char *logical;
            const char *library;
        } mappings[] = {{"ws2_32", "ws2_32.lib"},
                        {"bcrypt", "bcrypt.lib"},
                        {"api-ms-win-core-synch-l1-2-0", "synchronization.lib"}};
        size_t len = name ? strlen(name) : 0;
        if (len >= 4 && strcmp(name + len - 4, ".lib") == 0)
            return add(sink, name, err, err_size);
        for (size_t i = 0; i < sizeof(mappings) / sizeof(mappings[0]); i++)
            if (name && strcmp(name, mappings[i].logical) == 0)
                return add(sink, mappings[i].library, err, err_size);
        return command_error(err, err_size,
                             "MSVC system-library logical name has no explicit .lib mapping");
    }
    /* GNU-style drivers targeting the MSVC ABI consume the installed Windows
     * SDK import libraries, where this API-set is exposed by
     * synchronization.lib. Keep the mapping target-specific so GNU ABI
     * providers retain their canonical API-set spelling. */
    bool uses_msvc_sdk =
        target && target->abi == XR_TOOLCHAIN_TARGET_ABI_MSVC &&
        (provider == XR_TOOLCHAIN_PROVIDER_LLVM_CLANG || provider == XR_TOOLCHAIN_PROVIDER_ZIG);
    if (uses_msvc_sdk && name &&
        strcmp(name, "api-ms-win-core-synch-l1-2-0") == 0)
        return joined(sink, "-l", "synchronization", err, err_size);
    return joined(sink, "-l", name, err, err_size);
}

XR_FUNC bool xtc_command_emit_sysroot(XrToolchainProviderId provider, const char *path,
                                      XrToolchainArgSink *sink, char *err, size_t err_size) {
    if (!path || !path[0])
        return true;
    if (provider == XR_TOOLCHAIN_PROVIDER_MSVC)
        return command_error(err, err_size,
                             "MSVC sysroot is expressed by its verified SDK environment");
    return joined(sink, "--sysroot=", path, err, err_size);
}

XR_FUNC bool xtc_command_emit_library_search(XrToolchainProviderId provider, const char *path,
                                             XrToolchainArgSink *sink, char *err, size_t err_size) {
    if (!path || !path[0])
        return true;
    if (provider == XR_TOOLCHAIN_PROVIDER_MSVC)
        return joined(sink, "/LIBPATH:", path, err, err_size);
    return joined(sink, "-L", path, err, err_size);
}
