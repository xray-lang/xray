/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtc_xir_native_image.c - Bounded ELF64 and Mach-O executable facts
 *
 * KEY CONCEPT:
 *   Admission needs the same facts PE parsing gives on Windows: the machine,
 *   that the image is a program rather than a library, and that its entry
 *   point lies in executable file-backed bytes. Nothing else is interpreted.
 */
#include "xtc_xir_native_image.h"

static uint16_t image_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t image_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t image_u64(const uint8_t *p) { return (uint64_t)image_u32(p) | ((uint64_t)image_u32(p + 4) << 32); }

static XtcXirNativeImageStatus image_elf(const uint8_t *bytes, size_t length, uint32_t machine,
    XtcXirNativeImage *output) {
    if (length < 64 || bytes[0] != 0x7f || bytes[1] != 'E' || bytes[2] != 'L' || bytes[3] != 'F')
        return XTC_XIR_NATIVE_IMAGE_INVALID;
    /* ELFCLASS64, little-endian, current version, System V or GNU ABI. */
    if (bytes[4] != 2 || bytes[5] != 1 || bytes[6] != 1 || (bytes[7] != 0 && bytes[7] != 3))
        return XTC_XIR_NATIVE_IMAGE_UNSUPPORTED;
    uint16_t type = image_u16(bytes + 16), count = image_u16(bytes + 56);
    uint64_t entry = image_u64(bytes + 24), table = image_u64(bytes + 32);
    if (image_u32(bytes + 20) != 1 || image_u16(bytes + 54) != 56) return XTC_XIR_NATIVE_IMAGE_INVALID;
    if (image_u16(bytes + 18) != machine || (type != 2 && type != 3) || !entry || !count)
        return XTC_XIR_NATIVE_IMAGE_UNSUPPORTED;
    if (table > length || (uint64_t)count * 56u > length - table) return XTC_XIR_NATIVE_IMAGE_INVALID;
    bool interpreter = false, executable = false;
    for (uint16_t i = 0; i < count; ++i) {
        const uint8_t *header = bytes + table + (size_t)i * 56u;
        uint32_t kind = image_u32(header), flags = image_u32(header + 4);
        uint64_t offset = image_u64(header + 8), address = image_u64(header + 16);
        uint64_t file_size = image_u64(header + 32), memory_size = image_u64(header + 40);
        if (kind == 3) interpreter = true;
        if (kind != 1) continue;
        if (offset > length || file_size > length - offset || file_size > memory_size)
            return XTC_XIR_NATIVE_IMAGE_INVALID;
        if ((flags & 1u) && entry >= address && entry - address < file_size) executable = true;
    }
    if (!executable || (type == 3 && !interpreter)) return XTC_XIR_NATIVE_IMAGE_UNSUPPORTED;
    *output = (XtcXirNativeImage){XTC_XIR_NATIVE_IMAGE_ELF64, machine, type, image_u32(bytes + 48), entry, count};
    return XTC_XIR_NATIVE_IMAGE_OK;
}
/* LC_MAIN gives the entry as a file offset; the first pass finds it, the
 * second requires an executable segment whose file bytes contain it. */
static XtcXirNativeImageStatus image_macho(const uint8_t *bytes, size_t length, uint32_t machine,
    XtcXirNativeImage *output) {
    if (length < 32 || image_u32(bytes) != 0xfeedfacfu) return XTC_XIR_NATIVE_IMAGE_INVALID;
    uint32_t count = image_u32(bytes + 16), size = image_u32(bytes + 20), flags = image_u32(bytes + 24);
    if (image_u32(bytes + 4) != machine || image_u32(bytes + 12) != 2) return XTC_XIR_NATIVE_IMAGE_UNSUPPORTED;
    if (size > length - 32 || count > size / 8) return XTC_XIR_NATIVE_IMAGE_INVALID;
    uint64_t entry = 0, end = 32u + (uint64_t)size;
    uint32_t mains = 0;
    for (uint32_t pass = 0; pass < 2; ++pass) {
        bool executable = false;
        uint64_t at = 32;
        for (uint32_t i = 0; i < count; ++i) {
            if (end - at < 8) return XTC_XIR_NATIVE_IMAGE_INVALID;
            uint32_t command = image_u32(bytes + at), used = image_u32(bytes + at + 4);
            if (used < 8 || used % 8 || used > end - at) return XTC_XIR_NATIVE_IMAGE_INVALID;
            if (!pass && command == 0x80000028u) {
                if (used < 24) return XTC_XIR_NATIVE_IMAGE_INVALID;
                entry = image_u64(bytes + at + 8); ++mains;
            }
            if (pass && command == 0x19u) {
                if (used < 72) return XTC_XIR_NATIVE_IMAGE_INVALID;
                uint64_t offset = image_u64(bytes + at + 40), file_size = image_u64(bytes + at + 48);
                if (offset > length || file_size > length - offset) return XTC_XIR_NATIVE_IMAGE_INVALID;
                if ((image_u32(bytes + at + 60) & 4u) && entry >= offset && entry - offset < file_size) executable = true;
            }
            at += used;
        }
        if (!pass && mains != 1) return XTC_XIR_NATIVE_IMAGE_UNSUPPORTED;
        if (pass && !executable) return XTC_XIR_NATIVE_IMAGE_UNSUPPORTED;
    }
    *output = (XtcXirNativeImage){XTC_XIR_NATIVE_IMAGE_MACHO64, machine, 2, flags, entry, count};
    return XTC_XIR_NATIVE_IMAGE_OK;
}
XR_FUNC XtcXirNativeImageStatus xtc_xir_native_image_parse(const uint8_t *bytes, size_t length,
    XtcXirNativeImageFormat format, uint32_t machine, XtcXirNativeImage *output) {
    if (!bytes || !output || (format != XTC_XIR_NATIVE_IMAGE_ELF64 && format != XTC_XIR_NATIVE_IMAGE_MACHO64))
        return XTC_XIR_NATIVE_IMAGE_BAD_ARGUMENT;
    XtcXirNativeImage image;
    XtcXirNativeImageStatus status = format == XTC_XIR_NATIVE_IMAGE_ELF64 ?
        image_elf(bytes, length, machine, &image) : image_macho(bytes, length, machine, &image);
    if (status == XTC_XIR_NATIVE_IMAGE_OK) *output = image;
    return status;
}
