/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtc_xir_images.h - Owned leases for observed provider image files
 */
#ifndef XTC_XIR_IMAGES_H
#define XTC_XIR_IMAGES_H
#include "xtc_xir_target.h"
#include "../../os/os_proc.h"

enum { XR_XIR_IMAGE_EXE = 1, XR_XIR_IMAGE_DLL = 2 };
typedef struct XrXirImageFile {
    const char *path;
    uint32_t kind_mask;
    uint64_t length;
    uint8_t digest[32];
} XrXirImageFile;
typedef struct XrXirImageCollector XrXirImageCollector;

/* The observer borrows its owner throughout the existing process run. It
 * acquires leases before returning from each event, but never reads or closes
 * the event's borrowed file handle. Serialize mutation on the process thread.
 * Seal hashes leased files; it does not certify process or Target success. */
XR_FUNC XrXirTargetStatus xtc_xir_images_new(XrCompileResources *resources, XrXirImageCollector **output);
XR_FUNC XrProcImageObserver xtc_xir_images_observer(XrXirImageCollector *images);
XR_FUNC XrXirTargetStatus xtc_xir_images_seal(XrXirImageCollector *images);
XR_FUNC XrCompileResources *xtc_xir_images_resources(const XrXirImageCollector *images);
XR_FUNC XrXirTargetStatus xtc_xir_images_status(const XrXirImageCollector *images);
XR_FUNC bool xtc_xir_images_sealed(const XrXirImageCollector *images);
/* Only sealed owners expose their immutable file table. */
XR_FUNC uint32_t xtc_xir_images_count(const XrXirImageCollector *images);
XR_FUNC const XrXirImageFile *xtc_xir_images_file(const XrXirImageCollector *images, uint32_t index);
XR_FUNC void xtc_xir_images_free(XrXirImageCollector *images);
#endif // XTC_XIR_IMAGES_H
