/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtc_xir_sysroot_internal.h - Private resource and handle ownership
 */
#ifndef XTC_XIR_SYSROOT_INTERNAL_H
#define XTC_XIR_SYSROOT_INTERNAL_H
#include "xtc_xir_target.h"
#include "xtc_xir_images.h"
#include <wchar.h>
#define XTC_XIR_TARGET_PATH_LIMIT 32768u
#define XTC_XIR_TARGET_FILE_LIMIT 4096u
#define XTC_XIR_TARGET_COMMAND_LIMIT 64u
#define XTC_XIR_TARGET_ARGUMENT_LIMIT 4096u
#define XTC_XIR_TARGET_TEXT_LIMIT (1024u * 1024u)
typedef struct XtcXirHashProofCache XtcXirHashProofCache;

typedef struct XtcXirMemory {
    struct XtcXirMemory *next;
    uint64_t alignment;
} XtcXirMemory;
typedef struct XtcXirLock {
    struct XtcXirLock *next;
    void *handle;
    wchar_t *path;
    size_t length;
    uint64_t volume;
    uint8_t file_id[16];
    bool directory;
} XtcXirLock;
struct XrXirTargetSnapshot {
    XrCompileResources *resources;
    XrXirTargetStatus status;
    XrXirTargetFacts facts;
    XrXirTargetFile *files;
    XrXirTargetCommandFacts *commands;
    XtcXirMemory *memory;
    XtcXirLock *locks;
    wchar_t *scratch;
    size_t scratch_length;
};
typedef struct XtcXirImage {
    struct XtcXirImage *next;
    XtcXirLock *lease;
    const char *path;
    uint32_t kind_mask;
} XtcXirImage;
struct XrXirImageCollector {
    /* Both owners use the same private allocation, scratch and lease arena.
     * Its Target facts are never exposed as a constructed Target snapshot. */
    XrXirTargetSnapshot storage;
    XtcXirImage *images, *last;
    XrXirImageFile *files;
    uint32_t count;
    bool sealed;
    XtcXirHashProofCache *proof_cache;
};
XR_FUNC bool xtc_xir_target_fail(XrXirTargetSnapshot *snapshot, XrXirTargetStatus status);
XR_FUNC bool xtc_xir_target_work(XrXirTargetSnapshot *snapshot, uint64_t work);
XR_FUNC void *xtc_xir_target_allocate(XrXirTargetSnapshot *snapshot, size_t bytes);
XR_FUNC char *xtc_xir_target_text(XrXirTargetSnapshot *snapshot, const char *text);
XR_FUNC bool xtc_xir_target_length(XrXirTargetSnapshot *snapshot, const char *text, size_t *length);
XR_FUNC bool xtc_xir_target_compare(XrXirTargetSnapshot *snapshot, const char *a, const char *b, int *order);
XR_FUNC bool xtc_xir_sysroot_capture(XrXirTargetSnapshot *snapshot, const XrXirTargetSnapshotRequest *request);
XR_FUNC void xtc_xir_sysroot_close(XrXirTargetSnapshot *snapshot);
XR_FUNC bool xtc_xir_sysroot_observe(XrXirImageCollector *images, const XrProcImageEvent *event);
XR_FUNC bool xtc_xir_sysroot_hash(XrXirTargetSnapshot *snapshot, XtcXirLock *lock, XrXirTargetFile *file);
XR_FUNC XrXirTargetStatus xtc_xir_sysroot_read(XrXirTargetSnapshot *storage,
    XtcXirLock *lock, uint64_t file_length, size_t limit, void **owned_bytes, size_t *length);
#ifdef XR_OS_WINDOWS
XR_FUNC XrXirTargetStatus xtc_xir_hash_proof_cache_new(XrCompileResources *resources,
    uint32_t limit,XtcXirHashProofCache **output);
XR_FUNC void xtc_xir_hash_proof_cache_free(XtcXirHashProofCache *cache);
XR_FUNC XrCompileResources *xtc_xir_hash_proof_cache_resources(const XtcXirHashProofCache *cache);
XR_FUNC bool xtc_xir_sysroot_hash_with_proof(XrXirTargetSnapshot *storage,XtcXirLock *lock,
    XrXirTargetFile *file,XtcXirHashProofCache *cache);
XR_FUNC XrXirTargetStatus xtc_xir_images_new_with_proof(XrCompileResources *resources,
    XtcXirHashProofCache *cache,XrXirImageCollector **output);
struct XtcXirFileLease;
XR_FUNC XrXirTargetStatus xtc_xir_file_lease_open_with_proof(XrCompileResources *resources,
    const char *path,XtcXirHashProofCache *cache,struct XtcXirFileLease **output);
#endif
#endif // XTC_XIR_SYSROOT_INTERNAL_H
