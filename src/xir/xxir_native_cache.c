/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_native_cache.c - Owned authenticated static Checked/native pairs
 *
 * KEY CONCEPT: A generated registry authenticates code; callers supply no code.
 */
#include "xxir_native_cache_internal.h"
#include "xxir_native_cache_registry_internal.h"
#include "xxir_native_cache_projection.inc.c"
extern const XrXirProgramSpec xir_output_cache_program;
struct XirNativeCache {
    XrXirCompileContext context;
    uint32_t references;
    XrXirArtifact *checked;
    XirNativeCacheRegistry manifest;
    XrXirCallEntry entries[2];
    XrXirType parameters[2];
    char *variant;
};
static XrXirStatus cache_component(const XrXirCompileContext *context, const XirNativeCacheComponent *component) {
    if (!component->bytes || !component->length) return XR_XIR_BAD_STRUCTURE;
    XrSHA256Context hash; uint8_t digest[32]; xr_sha256_init(&hash);
    if (!cache_hash_bytes(context,&hash,component->bytes,component->length) ||
        !xir_compile_work(context,33)) return XR_XIR_BUDGET;
    xr_sha256_final(&hash,digest);
    return memcmp(digest,component->digest,32) ? XR_XIR_BAD_STRUCTURE : XR_XIR_OK;
}
static XrXirStatus cache_binding_id(const XrXirCompileContext *context,
    const XirNativeCacheRegistry *record, uint8_t output[32]) {
    XrSHA256Context hash; xr_sha256_init(&hash);
    const XirNativeCacheComponent *components[] = {&record->checked,&record->native_object,&record->generated_c};
    for (unsigned i = 0; i < 3; ++i)
        if (!cache_hash_word(context,&hash,components[i]->length) ||
            !cache_hash_bytes(context,&hash,components[i]->digest,32)) return XR_XIR_BUDGET;
    if (!cache_hash_word(context,&hash,record->schema) ||
        !cache_hash_word(context,&hash,record->architecture) ||
        !cache_hash_word(context,&hash,record->value_abi) ||
        !cache_hash_word(context,&hash,record->call_abi) ||
        !cache_hash_word(context,&hash,record->program_abi) ||
        !cache_hash_word(context,&hash,record->entry_count) ||
        !cache_hash_bytes(context,&hash,record->semantic_id,32) ||
        !cache_hash_bytes(context,&hash,record->runtime_layout,32) ||
        !cache_hash_bytes(context,&hash,record->sdk_identity,32)) return XR_XIR_BUDGET;
    size_t length = 0;
    do { if (!xir_compile_work(context,1)) return XR_XIR_BUDGET; } while (record->variant[length++]);
    if (!cache_hash_frame(context,&hash,record->variant,length-1)) return XR_XIR_BUDGET;
    for (unsigned i = 0; i < 2; ++i)
        if (!cache_hash_word(context,&hash,record->leaf_indices[i]) ||
            !cache_hash_bytes(context,&hash,record->leaf_digests[i],32)) return XR_XIR_BUDGET;
    if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
    xr_sha256_final(&hash,output); return XR_XIR_OK;
}
static XrXirStatus cache_registry_verify(const XrXirCompileContext *context,
    const XirNativeCacheRegistry *record) {
    if (!record || record->schema != 1 || !record->variant || !record->variant[0] ||
        record->architecture != XR_XIR_ARCH_X86_64 || record->value_abi != XR_XIR_VALUE_ABI_VERSION ||
        record->call_abi != XR_XIR_CALL_ABI_VERSION || record->program_abi != XR_XIR_PROGRAM_ABI_VERSION ||
        record->entries != xir_output_cache_program.entries ||
        record->entry_count != xir_output_cache_program.entry_count) return XR_XIR_BAD_STRUCTURE;
    uint8_t digest[32]; XrXirStatus status = cache_binding_id(context,record,digest);
    if (status != XR_XIR_OK) return status;
    if (!xir_compile_work(context,32)) return XR_XIR_BUDGET;
    if (memcmp(digest,record->binding_digest,32)) return XR_XIR_BAD_STRUCTURE;
    const XirNativeCacheComponent *components[] = {&record->checked,&record->native_object,&record->generated_c};
    for (unsigned i = 0; i < 3; ++i) {
        status = cache_component(context,components[i]); if (status != XR_XIR_OK) return status;
    }
    status = cache_semantic_id(context,record->checked.bytes,record->checked.length,digest);
    if (status != XR_XIR_OK) return status;
    if (!xir_compile_work(context,32)) return XR_XIR_BUDGET;
    if (memcmp(digest,record->semantic_id,32)) return XR_XIR_BAD_STRUCTURE;
    status = cache_abi_id(context,digest); if (status != XR_XIR_OK) return status;
    if (!xir_compile_work(context,32)) return XR_XIR_BUDGET;
    if (memcmp(digest,record->runtime_layout,32)) return XR_XIR_BAD_LAYOUT;
    for (unsigned i = 0; i < 2; ++i) {
        if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
        if (record->leaf_indices[i] >= record->entry_count || record->leaf_indices[0] == record->leaf_indices[1])
            return XR_XIR_BAD_STRUCTURE;
        const XrXirCallEntry *entry = &record->entries[record->leaf_indices[i]];
        if (entry->abi_version != XR_XIR_CALL_ABI_VERSION || entry->parameter_count != 1 ||
            !entry->parameters || entry->parameters[0] != XR_XIR_STRING || entry->result != XR_XIR_BOOL ||
            !entry->resume || !entry->release || entry->environment || entry->flags || entry->cleanup_owner)
            return XR_XIR_BAD_TYPE;
    }
    return XR_XIR_OK;
}
XR_FUNC XrXirStatus xir_native_cache_open(const XrXirCompileContext *context, XirNativeCache **output) {
    if (!xir_compile_context_valid(context) || !output || *output) return XR_XIR_BAD_STRUCTURE;
    const XirNativeCacheRegistry *record = &xir_native_cache_registry;
    XrXirStatus status = cache_registry_verify(context,record); if (status != XR_XIR_OK) return status;
    XirNativeCache *cache = xir_compile_calloc(context,1,sizeof(*cache),&status);
    if (!cache) return status;
    cache->context = *context; cache->references = 1;
    if (!xir_compile_work(context,sizeof(cache->manifest))) { status = XR_XIR_BUDGET; goto fail; }
    cache->manifest = *record;
    size_t length = 0;
    do { if (!xir_compile_work(context,1)) { status = XR_XIR_BUDGET; goto fail; } } while (record->variant[length++]);
    cache->variant = xir_compile_copy(context,record->variant,length,&status);
    if (!cache->variant) goto fail;
    cache->manifest.variant = cache->variant;
    status = xr_xir_compile_checked_read(context,record->checked.bytes,record->checked.length,&cache->checked,NULL);
    if (status != XR_XIR_OK) goto fail;
    const XrXirModule *module = xr_xir_compile_artifact_module(cache->checked);
    static const char identity[] = "stdlib-module-v1:module=2:io:path=12:io/output.xr";
    if (!xir_compile_work(context,sizeof(identity)-1)) { status = XR_XIR_BUDGET; goto fail; }
    if (!module || module->linkage_kind != XR_XIR_LIBRARY || !module->declarations ||
        module->declarations->module_count != 1 || module->declarations->modules[0].dependency_count ||
        module->declarations->modules[0].name_length != sizeof(identity)-1 ||
        memcmp(module->declarations->modules[0].name,identity,sizeof(identity)-1)) { status = XR_XIR_BAD_STRUCTURE; goto fail; }
    XrXirCheckedPacket canonical = {0}; status = xr_xir_compile_checked_write(cache->checked,&canonical,NULL);
    if (status == XR_XIR_OK) {
        if (!xir_compile_work(context,canonical.length)) status = XR_XIR_BUDGET;
        else if (canonical.length != record->checked.length || memcmp(canonical.bytes,record->checked.bytes,canonical.length)) status = XR_XIR_BAD_STRUCTURE;
    }
    xr_xir_compile_checked_packet_free(&canonical); if (status != XR_XIR_OK) goto fail;
    for (unsigned i = 0; i < 2; ++i) {
        if (!xir_compile_work(context,sizeof(XrXirCallEntry)+sizeof(XrXirType))) { status = XR_XIR_BUDGET; goto fail; }
        cache->entries[i] = record->entries[record->leaf_indices[i]];
        cache->parameters[i] = cache->entries[i].parameters[0]; cache->entries[i].parameters = &cache->parameters[i];
    }
    *output = cache; return XR_XIR_OK;
fail:
    xir_native_cache_drop(cache); return status;
}
XR_FUNC XrXirStatus xir_native_cache_library_catalog_new(const XrXirCompileContext *context,
    const char *physical_root, XrXirLibraryCatalog **output) {
    if (!xir_compile_context_valid(context) || !physical_root || !output || *output)
        return XR_XIR_BAD_STRUCTURE;
    const XirNativeCacheComponent *checked = &xir_native_cache_registry.checked;
    XrXirLibraryInput input = {checked->bytes,checked->length,{0}, (XrXirLibraryModuleInput[]){{{XR_MODULE_IDENTITY_STDLIB,"io",physical_root},"io/output.xr"}},1};
    if (!xir_compile_work(context,sizeof(input.sha256))) return XR_XIR_BUDGET;
    memcpy(input.sha256,checked->digest,sizeof(input.sha256));
    /* The ordinary reader verifies the complete Checked body and canonical owner.
     * The AOT consumer has no dependency on a native cache variant's applicability. */
    return xr_xir_compile_library_catalog_new_v2(context,&input,1,output);
}
XR_FUNC XrXirStatus xir_native_cache_retain(XirNativeCache *cache) {
    if (!cache || !cache->references) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(&cache->context,1)) return XR_XIR_BUDGET;
    if (cache->references == UINT32_MAX) return XR_XIR_BUDGET;
    ++cache->references; return XR_XIR_OK;
}
XR_FUNC void xir_native_cache_drop(XirNativeCache *cache) {
    if (!cache || !cache->references || --cache->references) return;
    xr_xir_compile_artifact_free(cache->checked); xr_compile_resources_free(cache->variant); xr_compile_resources_free(cache);
}
XR_FUNC const XrXirCompileContext *xir_native_cache_context(const XirNativeCache *cache) {
    return cache && cache->references ? &cache->context : NULL;
}
XR_FUNC XrXirStatus xir_native_cache_lookup(XirNativeCache *cache, const XrXirArtifact *lowered,
    uint32_t function, XirNativeCacheHit *output) {
    if (!cache || !cache->references || !lowered || !output) return XR_XIR_BAD_STRUCTURE;
    const XrXirCompileContext *context = xr_xir_compile_artifact_context(lowered);
    const XrXirModule *module = xr_xir_compile_artifact_module(lowered);
    const XrXirTarget *target = xr_xir_compile_artifact_target(lowered);
    if (!context || context->resources != cache->context.resources || !module || module->stage != XR_XIR_LOWERED ||
        !target || function >= module->function_count) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
    if (target->architecture != cache->manifest.architecture || target->abi_version != cache->manifest.value_abi)
        return XR_XIR_BAD_LAYOUT;
    /* Whole verification is the private VM worker's precondition, never a persistent flag. */
    for (uint32_t stream = 1; stream <= 2; ++stream) {
        uint8_t digest[32]; XrXirStatus status = cache_leaf_id(lowered,function,stream,digest);
        if (status == XR_XIR_UNRESOLVED) continue;
        if (status != XR_XIR_OK) return status;
        if (!xir_compile_work(context,32)) return XR_XIR_BUDGET;
        if (memcmp(digest,cache->manifest.leaf_digests[stream-1],32)) return XR_XIR_BAD_STRUCTURE;
        if (!xir_compile_work(context,sizeof(XirNativeCacheHit))) return XR_XIR_BUDGET;
        XirNativeCacheHit hit = {XIR_NATIVE_CACHE_MATCH,cache->entries[stream-1]}; *output = hit; return XR_XIR_OK;
    }
    if (!xir_compile_work(context,sizeof(XirNativeCacheHit))) return XR_XIR_BUDGET;
    *output = (XirNativeCacheHit){0}; return XR_XIR_OK;
}
