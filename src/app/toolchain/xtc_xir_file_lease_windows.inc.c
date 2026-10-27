/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtc_xir_file_lease_windows.inc.c - Role-neutral leases using sysroot ownership
 */
static XrXirTargetStatus file_lease_open(XrCompileResources *resources, const char *path,
    bool directory,XtcXirHashProofCache *cache,XtcXirFileLease **output) {
    if (!resources || !path || !output || *output || (cache && cache->resources!=resources)) return XR_XIR_TARGET_INVALID;
    void *memory = NULL;
    XrCompileResourceStatus allocated = xr_compile_resources_calloc(resources, 1, sizeof(XtcXirFileLease), &memory);
    if (allocated != XR_COMPILE_RESOURCE_OK)
        return allocated==XR_COMPILE_RESOURCE_MEMORY_LIMIT ? XR_XIR_TARGET_MEMORY_LIMIT :
        allocated==XR_COMPILE_RESOURCE_OVERFLOW ? XR_XIR_TARGET_OVERFLOW :
            allocated == XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_TARGET_OUT_OF_MEMORY : XR_XIR_TARGET_INVALID;
    XtcXirFileLease *lease = memory;
    lease->storage.resources = resources; lease->directory = directory;
    XrXirTargetSnapshot *storage = &lease->storage;
    const char *canonical = NULL;
    bool owned_scratch=cache!=NULL;
    if(owned_scratch) {
        void *scratch=NULL;
        XrCompileResourceStatus made=xr_compile_resources_alloc(resources,XTC_XIR_TARGET_PATH_LIMIT*sizeof(wchar_t),&scratch);
        if(made!=XR_COMPILE_RESOURCE_OK)xtc_xir_target_fail(storage,made==XR_COMPILE_RESOURCE_MEMORY_LIMIT ? XR_XIR_TARGET_MEMORY_LIMIT :
        made==XR_COMPILE_RESOURCE_OVERFLOW ? XR_XIR_TARGET_OVERFLOW :made==XR_COMPILE_RESOURCE_OUT_OF_MEMORY?XR_XIR_TARGET_OUT_OF_MEMORY:XR_XIR_TARGET_INVALID);
        storage->scratch=scratch;
    } else storage->scratch=xtc_xir_target_allocate(storage,XTC_XIR_TARGET_PATH_LIMIT*sizeof(wchar_t));
    if (storage->scratch && sysroot_open_path(storage, path, directory, &canonical, &lease->lock)) {
        if (directory) {
            if (sysroot_id(storage, lease->lock->handle, &lease->directory_facts.volume, lease->directory_facts.file_id) &&
                storage->status == XR_XIR_TARGET_OK) {
                lease->directory_facts.path = canonical; lease->directory_facts.native_path = lease->lock->path;
            }
        } else {
            XrXirTargetFile file = {0};
            if (sysroot_hash_with_proof(storage, lease->lock, &file,cache) && storage->status == XR_XIR_TARGET_OK) {
                lease->file.path = canonical; lease->file.length = file.length;
                memcpy(lease->file.digest, file.digest, sizeof(lease->file.digest));
            }
        }
    }
    if(owned_scratch) { xr_compile_resources_free(storage->scratch);storage->scratch=NULL; }
    XrXirTargetStatus status = storage->status;
    if (status != XR_XIR_TARGET_OK) { xtc_xir_file_lease_free(lease); return status; }
    *output = lease; return XR_XIR_TARGET_OK;
}
XR_FUNC XrXirTargetStatus xtc_xir_file_lease_open(XrCompileResources *resources,
    const char *path, XtcXirFileLease **output) {
    return file_lease_open(resources,path,false,NULL,output);
}
XR_FUNC XrXirTargetStatus xtc_xir_file_lease_open_with_proof(XrCompileResources *resources,
    const char *path,XtcXirHashProofCache *cache,XtcXirFileLease **output) {
    if(!cache)return XR_XIR_TARGET_INVALID;
    return file_lease_open(resources,path,false,cache,output);
}
XR_FUNC XrXirTargetStatus xtc_xir_file_lease_directory_open(XrCompileResources *resources,
    const char *path, XtcXirFileLease **output) {
    return file_lease_open(resources,path,true,NULL,output);
}
XR_FUNC XrXirTargetStatus xtc_xir_sysroot_read(XrXirTargetSnapshot *storage,
    XtcXirLock *lock, uint64_t file_length, size_t limit, void **owned_bytes, size_t *length) {
    if (!storage || !lock || lock->directory || !limit || !owned_bytes || *owned_bytes || !length || *length)
        return XR_XIR_TARGET_INVALID;
    if (storage->status != XR_XIR_TARGET_OK) return storage->status;
    if (file_length > SIZE_MAX) {
        xtc_xir_target_fail(storage, XR_XIR_TARGET_OVERFLOW); return storage->status;
    }
    if (file_length > limit) {
        xtc_xir_target_fail(storage, XR_XIR_TARGET_BUDGET); return storage->status;
    }
    size_t bytes = (size_t)file_length;
    void *data = NULL;
    XrCompileResourceStatus allocated = xr_compile_resources_alloc(storage->resources, bytes ? bytes : 1, &data);
    if (allocated != XR_COMPILE_RESOURCE_OK) {
        xtc_xir_target_fail(storage, allocated==XR_COMPILE_RESOURCE_MEMORY_LIMIT ? XR_XIR_TARGET_MEMORY_LIMIT :
        allocated==XR_COMPILE_RESOURCE_OVERFLOW ? XR_XIR_TARGET_OVERFLOW :
            allocated == XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_TARGET_OUT_OF_MEMORY : XR_XIR_TARGET_INVALID);
        return storage->status;
    }
    LARGE_INTEGER start = {0};
    if (storage->status == XR_XIR_TARGET_OK && !SetFilePointerEx(lock->handle, start, NULL, FILE_BEGIN))
        sysroot_error(storage, GetLastError());
    for (size_t at = 0; at < bytes && storage->status == XR_XIR_TARGET_OK;) {
        DWORD amount = (DWORD)(bytes - at > 65536 ? 65536 : bytes - at), actual = 0;
        if (storage->status != XR_XIR_TARGET_OK) break;
        if (!ReadFile(lock->handle, (uint8_t *)data + at, amount, &actual, NULL)) {
            sysroot_error(storage, GetLastError()); break;
        }
        if (actual != amount) { xtc_xir_target_fail(storage, XR_XIR_TARGET_IO); break; }
        at += actual;
    }
    if (!bytes && storage->status == XR_XIR_TARGET_OK) *(uint8_t *)data = 0;
    if (storage->status != XR_XIR_TARGET_OK) { xr_compile_resources_free(data); return storage->status; }
    *owned_bytes = data; *length = bytes; return XR_XIR_TARGET_OK;
}
