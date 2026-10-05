/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtc_xir_images.c - Transactional observation and sealing of image leases
 */
#include "xtc_xir_images.h"
#include "xtc_xir_sysroot_internal.h"
#include <string.h>

static XrOsProcStatus images_process_status(XrXirTargetStatus status) {
    switch (status) {
    case XR_XIR_TARGET_OK: return XR_PROC_OK;
    case XR_XIR_TARGET_BUDGET: return XR_PROC_BUDGET;
    case XR_XIR_TARGET_OUT_OF_MEMORY: return XR_PROC_OUT_OF_MEMORY;
    case XR_XIR_TARGET_UNRESOLVED: return XR_PROC_UNRESOLVED;
    case XR_XIR_TARGET_UNSUPPORTED: return XR_PROC_UNSUPPORTED;
    case XR_XIR_TARGET_INVALID: return XR_PROC_INVALID_ARGUMENT;
    default: return XR_PROC_IO;
    }
}
static XrOsProcStatus images_observe(void *context, const XrProcImageEvent *event) {
    XrXirImageCollector *images = context;
    if (!images || images->sealed) return XR_PROC_INVALID_ARGUMENT;
    if (images->storage.status != XR_XIR_TARGET_OK) return images_process_status(images->storage.status);
    if (!event || event->pid <= 0 || (event->kind != XR_PROC_IMAGE_EXECUTABLE && event->kind != XR_PROC_IMAGE_DLL))
        xtc_xir_target_fail(&images->storage, XR_XIR_TARGET_INVALID);
    else xtc_xir_sysroot_observe(images, event);
    return images_process_status(images->storage.status);
}
XR_FUNC XrXirTargetStatus xtc_xir_images_new(XrCompileResources *resources, XrXirImageCollector **output) {
    if (!resources || !output || *output) return XR_XIR_TARGET_INVALID;
    void *memory = NULL;
    XrCompileResourceStatus status = xr_compile_resources_calloc(resources, 1, sizeof(XrXirImageCollector), &memory);
    if (status != XR_COMPILE_RESOURCE_OK)
        return status == XR_COMPILE_RESOURCE_BUDGET ? XR_XIR_TARGET_BUDGET :
            status == XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_TARGET_OUT_OF_MEMORY : XR_XIR_TARGET_INVALID;
    XrXirImageCollector *images = memory;
    images->storage.resources = resources;
    *output = images; return XR_XIR_TARGET_OK;
}
#ifdef XR_OS_WINDOWS
XR_FUNC XrXirTargetStatus xtc_xir_images_new_with_proof(XrCompileResources *resources,
    XtcXirHashProofCache *cache,XrXirImageCollector **output) {
    if(!cache || !resources || !output || *output || xtc_xir_hash_proof_cache_resources(cache)!=resources)return XR_XIR_TARGET_INVALID;
    XrXirImageCollector *images=NULL;
    XrXirTargetStatus status=xtc_xir_images_new(resources,&images);
    if(status!=XR_XIR_TARGET_OK)return status;
    if(!xtc_xir_target_work(&images->storage,sizeof(images->proof_cache))) {
        status=images->storage.status;xtc_xir_images_free(images);return status;
    }
    images->proof_cache=cache;*output=images;return XR_XIR_TARGET_OK;
}
#endif
XR_FUNC XrProcImageObserver xtc_xir_images_observer(XrXirImageCollector *images) {
    return (XrProcImageObserver){images, images ? images_observe : NULL};
}
XR_FUNC XrXirTargetStatus xtc_xir_images_seal(XrXirImageCollector *images) {
    if (!images) return XR_XIR_TARGET_INVALID;
    if (images->sealed) return XR_XIR_TARGET_OK;
    if (images->storage.status != XR_XIR_TARGET_OK) return images->storage.status;
    if (!images->count) { xtc_xir_target_fail(&images->storage, XR_XIR_TARGET_INVALID); return images->storage.status; }
    XrXirImageFile *files = xtc_xir_target_allocate(&images->storage, images->count * sizeof(*files));
    uint32_t index = 0;
    if (files) for (XtcXirImage *image = images->images; image; image = image->next) {
        XrXirTargetFile hashed = {0};
        #ifdef XR_OS_WINDOWS
        bool hashed_ok=images->proof_cache?xtc_xir_sysroot_hash_with_proof(&images->storage,image->lease,&hashed,images->proof_cache):
            xtc_xir_sysroot_hash(&images->storage,image->lease,&hashed);
#else
        bool hashed_ok=xtc_xir_sysroot_hash(&images->storage,image->lease,&hashed);
#endif
        if (!hashed_ok ||
            !xtc_xir_target_work(&images->storage, sizeof(*files))) break;
        files[index] = (XrXirImageFile){image->path, image->kind_mask, hashed.length, {0}};
        memcpy(files[index].digest, hashed.digest, sizeof(hashed.digest));
        ++index;
    }
    if (images->storage.status == XR_XIR_TARGET_OK && xtc_xir_target_work(&images->storage, 1)) {
        images->files = files; images->sealed = true;
    }
    return images->storage.status;
}
XR_FUNC XrCompileResources *xtc_xir_images_resources(const XrXirImageCollector *images) {
    return images ? images->storage.resources : NULL;
}
XR_FUNC XrXirTargetStatus xtc_xir_images_status(const XrXirImageCollector *images) {
    return images ? images->storage.status : XR_XIR_TARGET_INVALID;
}
XR_FUNC bool xtc_xir_images_sealed(const XrXirImageCollector *images) { return images && images->sealed; }
XR_FUNC uint32_t xtc_xir_images_count(const XrXirImageCollector *images) { return images && images->sealed ? images->count : 0; }
XR_FUNC const XrXirImageFile *xtc_xir_images_file(const XrXirImageCollector *images, uint32_t index) {
    return images && images->sealed && index < images->count ? &images->files[index] : NULL;
}
XR_FUNC XrXirTargetStatus xtc_xir_images_read(XrXirImageCollector *images,
    uint32_t index, size_t limit, void **owned_bytes, size_t *length) {
    if (!images || !images->sealed || index >= images->count || !limit ||
        !owned_bytes || *owned_bytes || !length || *length) return XR_XIR_TARGET_INVALID;
    if (images->storage.status != XR_XIR_TARGET_OK) return images->storage.status;
    XtcXirImage *image = images->images;
    for (uint32_t at = 0; at <= index; ++at) {
        if (!xtc_xir_target_work(&images->storage, 1)) return images->storage.status;
        if (!image) { xtc_xir_target_fail(&images->storage, XR_XIR_TARGET_INVALID); return images->storage.status; }
        if (at != index) image = image->next;
    }
    return xtc_xir_sysroot_read(&images->storage, image->lease, images->files[index].length,
        limit, owned_bytes, length);
}
XR_FUNC void xtc_xir_images_free(XrXirImageCollector *images) {
    if (!images) return;
    xtc_xir_sysroot_close(&images->storage);
    while (images->storage.memory) {
        XtcXirMemory *memory = images->storage.memory;
        images->storage.memory = memory->next; xr_compile_resources_free(memory);
    }
    xr_compile_resources_free(images);
}
