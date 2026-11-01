/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtc_xir_native_image.h - Bounded ELF64 and Mach-O executable facts
 */
#ifndef XTC_XIR_NATIVE_IMAGE_H
#define XTC_XIR_NATIVE_IMAGE_H
#include "../../base/xdefs.h"
#include <stddef.h>
#include <stdint.h>

typedef enum XtcXirNativeImageStatus {
    XTC_XIR_NATIVE_IMAGE_OK, XTC_XIR_NATIVE_IMAGE_BAD_ARGUMENT,
    XTC_XIR_NATIVE_IMAGE_INVALID, XTC_XIR_NATIVE_IMAGE_UNSUPPORTED
} XtcXirNativeImageStatus;
/* Format words match the runtime SDK object_format facts. */
typedef enum XtcXirNativeImageFormat {
    XTC_XIR_NATIVE_IMAGE_ELF64 = 2, XTC_XIR_NATIVE_IMAGE_MACHO64 = 3
} XtcXirNativeImageFormat;
#define XTC_XIR_ELF_MACHINE_X86_64 62u
#define XTC_XIR_ELF_MACHINE_AARCH64 183u
#define XTC_XIR_MACHO_CPU_X86_64 0x01000007u
#define XTC_XIR_MACHO_CPU_ARM64 0x0100000cu
typedef struct XtcXirNativeImage {
    uint32_t format, machine, type, flags;
    uint64_t entry, commands;
} XtcXirNativeImage;

/* Decode one little-endian 64-bit executable for the requested format and
 * machine (ELF e_machine or Mach-O cputype). ELF must be ET_EXEC, or ET_DYN
 * with PT_INTERP, with its entry in an executable PT_LOAD's file bytes.
 * Mach-O must be MH_EXECUTE with exactly one LC_MAIN whose entry offset is in
 * an executable LC_SEGMENT_64's file bytes. Every header, program header and
 * load command is bounds-checked before it is read. Imports, relocations and
 * the dynamic loader are not validated. No allocation or I/O occurs; failures
 * leave the output unchanged. UNSUPPORTED means well-formed but not admitted. */
XR_FUNC XtcXirNativeImageStatus xtc_xir_native_image_parse(const uint8_t *bytes, size_t length,
    XtcXirNativeImageFormat format, uint32_t machine, XtcXirNativeImage *output);
#endif // XTC_XIR_NATIVE_IMAGE_H
