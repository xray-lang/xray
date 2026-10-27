/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtc_xir_hash_proof.inc.c - Operation-owned content proof and real file leases
 */
typedef struct XtcXirHashProof {
    struct XtcXirHashProof *next;
    HANDLE handle;
    uint64_t volume, length;
    uint8_t file_id[16], digest[32];
} XtcXirHashProof;
struct XtcXirHashProofCache {
    XrCompileResources *resources;
    XtcXirHashProof *proofs;
    uint32_t count, limit;
};
XR_FUNC XrXirTargetStatus xtc_xir_hash_proof_cache_new(XrCompileResources *resources,
    uint32_t limit, XtcXirHashProofCache **output) {
    if(!resources || !limit || limit>XTC_XIR_TARGET_FILE_LIMIT || !output || *output) return XR_XIR_TARGET_INVALID;
    XtcXirHashProofCache *cache=NULL;
    XrCompileResourceStatus status=xr_compile_resources_calloc(resources,1,sizeof(*cache),(void **)&cache);
    if(status!=XR_COMPILE_RESOURCE_OK) return status==XR_COMPILE_RESOURCE_MEMORY_LIMIT ? XR_XIR_TARGET_MEMORY_LIMIT :
        status==XR_COMPILE_RESOURCE_OVERFLOW ? XR_XIR_TARGET_OVERFLOW :
        status==XR_COMPILE_RESOURCE_OUT_OF_MEMORY?XR_XIR_TARGET_OUT_OF_MEMORY:XR_XIR_TARGET_INVALID;
    cache->resources=resources;cache->limit=limit;*output=cache;return XR_XIR_TARGET_OK;
}
XR_FUNC XrCompileResources *xtc_xir_hash_proof_cache_resources(const XtcXirHashProofCache *cache) {
    return cache?cache->resources:NULL;
}
XR_FUNC void xtc_xir_hash_proof_cache_free(XtcXirHashProofCache *cache) {
    if(!cache)return;
    while(cache->proofs) {
        XtcXirHashProof *proof=cache->proofs;cache->proofs=proof->next;
        if(proof->handle)XR_CHECK(CloseHandle(proof->handle),"Owned content proof handle could not be closed");
        xr_compile_resources_free(proof);
    }
    xr_compile_resources_free(cache);
}
static bool hash_proof_match(XrXirTargetSnapshot *storage,const XtcXirHashProof *proof,
    uint64_t volume,const uint8_t id[16],uint64_t length,bool *matches) {
    *matches=false;
    if(storage->status != XR_XIR_TARGET_OK)return false;
    if(proof->volume!=volume)return true;
    if(storage->status != XR_XIR_TARGET_OK)return false;
    if(memcmp(proof->file_id,id,16))return true;
    if(storage->status != XR_XIR_TARGET_OK)return false;
    *matches=proof->length==length;return true;
}
static bool hash_proof_publish(XrXirTargetSnapshot *storage,XtcXirHashProofCache *cache,
    XtcXirLock *lock,const XrXirTargetFile *file) {
    if(storage->status != XR_XIR_TARGET_OK)return false;
    if(cache->count==cache->limit)return xtc_xir_target_fail(storage,XR_XIR_TARGET_BUDGET);
    XtcXirHashProof *proof=NULL;
    XrCompileResourceStatus status=xr_compile_resources_calloc(storage->resources,1,sizeof(*proof),(void **)&proof);
    if(status!=XR_COMPILE_RESOURCE_OK)return xtc_xir_target_fail(storage,status==XR_COMPILE_RESOURCE_MEMORY_LIMIT ? XR_XIR_TARGET_MEMORY_LIMIT :
        status==XR_COMPILE_RESOURCE_OVERFLOW ? XR_XIR_TARGET_OVERFLOW :status==XR_COMPILE_RESOURCE_OUT_OF_MEMORY?XR_XIR_TARGET_OUT_OF_MEMORY:XR_XIR_TARGET_INVALID);
    if(storage->status != XR_XIR_TARGET_OK) { xr_compile_resources_free(proof);return false; }
    HANDLE duplicate=NULL;
    if(!DuplicateHandle(GetCurrentProcess(),lock->handle,GetCurrentProcess(),&duplicate,0,FALSE,DUPLICATE_SAME_ACCESS)) {
        sysroot_error(storage,GetLastError());xr_compile_resources_free(proof);return false;
    }
    if(storage->status != XR_XIR_TARGET_OK) { XR_CHECK(CloseHandle(duplicate),"Unpublished content proof handle could not be closed");xr_compile_resources_free(proof);return false; }
    proof->handle=duplicate;proof->next=cache->proofs;proof->volume=lock->volume;proof->length=file->length;
    memcpy(proof->file_id,lock->file_id,sizeof(proof->file_id));memcpy(proof->digest,file->digest,sizeof(proof->digest));
    cache->proofs=proof;++cache->count;return true;
}
static bool hash_proof_unmap(XrXirTargetSnapshot *storage,const uint8_t *view) {
    if(UnmapViewOfFile(view))return true;
    sysroot_error(storage,GetLastError());
    /* Retry only as cleanup; an exclusive owned view must not survive return. */
    XR_CHECK(UnmapViewOfFile(view),"Owned content proof view could not be released");
    return false;
}
static bool hash_proof_mapped(XrXirTargetSnapshot *storage,XtcXirLock *lock,
    uint64_t length,XrXirTargetFile *file) {
    XrSHA256Context hash;
    if(storage->status != XR_XIR_TARGET_OK)return false;
    xr_sha256_init(&hash);
    file->length=length;
    HANDLE mapping=NULL;
    if(length) {
        SYSTEM_INFO info;
        if(storage->status != XR_XIR_TARGET_OK)return false;
        GetSystemInfo(&info);
        if(!info.dwAllocationGranularity || 65536u%info.dwAllocationGranularity)
            return xtc_xir_target_fail(storage,XR_XIR_TARGET_UNSUPPORTED);
        if(storage->status != XR_XIR_TARGET_OK)return false;
        mapping=CreateFileMappingW(lock->handle,NULL,PAGE_READONLY,0,0,NULL);
        if(!mapping)return sysroot_error(storage,GetLastError());
    }
    for(uint64_t at=0;at<length;) {
        size_t amount=(size_t)(length-at>65536?65536:length-at);
        if(storage->status != XR_XIR_TARGET_OK)break;
        const uint8_t *view=MapViewOfFile(mapping,FILE_MAP_READ,(DWORD)(at>>32),(DWORD)at,amount);
        if(!view) { sysroot_error(storage,GetLastError());break; }
        bool okay=storage->status == XR_XIR_TARGET_OK;
        if(okay) {
            __try { xr_sha256_update(&hash,view,amount); }
            __except(GetExceptionCode()==EXCEPTION_IN_PAGE_ERROR?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {
                xtc_xir_target_fail(storage,XR_XIR_TARGET_IO);okay=false;
            }
        }
        if(okay)okay=storage->status == XR_XIR_TARGET_OK;
        if(!hash_proof_unmap(storage,view))okay=false;
        if(!okay)break;
        at+=amount;
    }
    if(mapping) {

        XR_CHECK(CloseHandle(mapping),"Owned content proof mapping could not be closed");
    }
    if(storage->status != XR_XIR_TARGET_OK)return false;
    xr_sha256_final(&hash,file->digest);return true;
}
static bool sysroot_hash_with_proof(XrXirTargetSnapshot *storage,XtcXirLock *lock,
    XrXirTargetFile *file,XtcXirHashProofCache *cache) {
    if(!cache)return sysroot_hash(storage,lock,file);
    if(storage->status != XR_XIR_TARGET_OK)return false;
    if(cache->resources!=storage->resources)return xtc_xir_target_fail(storage,XR_XIR_TARGET_INVALID);
    uint64_t volume=0;uint8_t id[16];
    if(!sysroot_id(storage,lock->handle,&volume,id))return false;
    LARGE_INTEGER size={0};
    if(storage->status != XR_XIR_TARGET_OK)return false;
    if(!GetFileSizeEx(lock->handle,&size))return sysroot_error(storage,GetLastError());
    if(size.QuadPart<0)return xtc_xir_target_fail(storage,XR_XIR_TARGET_INVALID);
    for(XtcXirHashProof *p=cache->proofs;p;) {
        bool matches=false;
        if(!hash_proof_match(storage,p,volume,id,(uint64_t)size.QuadPart,&matches))return false;
        if(matches) {
            if(storage->status != XR_XIR_TARGET_OK)return false;
            file->length=p->length;memcpy(file->digest,p->digest,sizeof(file->digest));return true;
        }
        if(storage->status != XR_XIR_TARGET_OK)return false;
        p=p->next;
    }
    if(!hash_proof_mapped(storage,lock,(uint64_t)size.QuadPart,file))return false;
    return hash_proof_publish(storage,cache,lock,file);
}
XR_FUNC bool xtc_xir_sysroot_hash_with_proof(XrXirTargetSnapshot *storage,XtcXirLock *lock,
    XrXirTargetFile *file,XtcXirHashProofCache *cache) {
    return sysroot_hash_with_proof(storage,lock,file,cache);
}
