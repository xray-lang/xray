/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtc_xir_native_admission_posix.inc.c - POSIX executable and bundle facts
 *
 * KEY CONCEPT:
 *   The admitted output must be a 64-bit executable for this host's CPU in
 *   its native object format. Those header facts enter the target profile the
 *   way PE facts do on Windows; imports and the dynamic loader are not checked.
 */
#include "xtc_xir_native_image.h"
typedef XtcXirNativeImage AdmissionImage;
static const char admission_triple[] = XR_XIR_SDK_POSIX_TRIPLE;
#ifdef XR_OS_MACOS
#define ADMISSION_TARGET_WORDS 4u
#define ADMISSION_GROUP_WORDS 0u
#else
#define ADMISSION_TARGET_WORDS 0u
#define ADMISSION_GROUP_WORDS 2u
#endif

static bool admission_image(NativeAdmission *a, const void *bytes, size_t length, AdmissionImage *image) {
#if defined(XR_OS_MACOS) && defined(XR_ARCH_X86_64)
    const uint32_t machine = XTC_XIR_MACHO_CPU_X86_64;
#elif defined(XR_OS_MACOS)
    const uint32_t machine = XTC_XIR_MACHO_CPU_ARM64;
#elif defined(XR_ARCH_X86_64)
    const uint32_t machine = XTC_XIR_ELF_MACHINE_X86_64;
#else
    const uint32_t machine = XTC_XIR_ELF_MACHINE_AARCH64;
#endif
    XtcXirNativeImageStatus status = xtc_xir_native_image_parse(bytes, length,
        (XtcXirNativeImageFormat)XR_XIR_SDK_POSIX_OBJECT_FORMAT, machine, image);
    return status == XTC_XIR_NATIVE_IMAGE_OK || admission_fail(a, status == XTC_XIR_NATIVE_IMAGE_UNSUPPORTED ?
        XTC_XIR_ADMISSION_UNSUPPORTED : XTC_XIR_ADMISSION_INVALID, XTC_XIR_ADMISSION_PE, status);
}
static void admission_image_hash(XrSHA256Context *hash, const AdmissionImage *image) {
    admission_integer(hash, image->format, 4); admission_integer(hash, image->machine, 4);
    admission_integer(hash, image->type, 4); admission_integer(hash, image->flags, 4);
    admission_integer(hash, image->entry, 8); admission_integer(hash, image->commands, 8);
}
static bool admission_runtime_supported(const NativeAdmission *a) {
    return a->runtime.architecture == XR_XIR_SDK_POSIX_ARCHITECTURE &&
        a->runtime.object_format == XR_XIR_SDK_POSIX_OBJECT_FORMAT && a->runtime.crt == XR_XIR_SDK_POSIX_CRT;
}
static bool admission_command_shape(unsigned stage, const XrProcessView *view) {
    const size_t base = 1 + ADMISSION_TARGET_WORDS;
    return view->env_count == 2 && (stage < 2 ? view->argc == base + 19 + stage :
        view->argc == base + 11 + ADMISSION_GROUP_WORDS);
}
/* Both owners publish canonical paths, compared byte-for-byte. */
static bool admission_archives(NativeAdmission *a) {
    static const char *const names[5] = {"lib/libxray_xir_runtime_host.a", "lib/libxray_xir_scalar.a",
        "lib/libxray_xir_admission.a", "lib/libxray_xir_declarations.a", "lib/libxray_compile_resources.a"};
    const char *paths[5] = {0}; bool seen[5] = {0}; bool okay = true;
    for (unsigned i = 0; okay && i < 5; ++i) {
        XrXirRuntimeSdkStatus status = xr_xir_runtime_sdk_file(a->sdk, names[i], &paths[i]);
        if (status != XR_XIR_SDK_OK) okay = admission_fail(a, status == XR_XIR_SDK_UNRESOLVED ?
            XTC_XIR_ADMISSION_UNRESOLVED : XTC_XIR_ADMISSION_INVALID, XTC_XIR_ADMISSION_SDK, status);
    }
    for (uint32_t i = 0; okay && i < a->facts.file_count; ++i) {
        XrXirInvocationFile row;
        okay = admission_row(a, i, &row);
        if (!okay || row.kind != XR_XIR_INVOCATION_SDK_ARCHIVE) continue;
        bool found = false;
        for (unsigned j = 0; !found && j < 5; ++j) {
            if (strcmp(row.path, paths[j])) continue;
            if (seen[j]) okay = admission_fail(a, XTC_XIR_ADMISSION_MISMATCH, XTC_XIR_ADMISSION_SDK, 0);
            seen[j] = found = true;
        }
        if (okay && !found) okay = admission_fail(a, XTC_XIR_ADMISSION_MISMATCH, XTC_XIR_ADMISSION_SDK, 0);
    }
    for (unsigned i = 0; okay && i < 5; ++i)
        if (!seen[i]) okay = admission_fail(a, XTC_XIR_ADMISSION_MISMATCH, XTC_XIR_ADMISSION_SDK, 0);
    return okay;
}
