/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_library_reader_cases.h - Owned decoding and held-memory rejection
 *
 * KEY CONCEPT:
 *   Failed decoding preserves owned artifacts and publishes no partial output.
 */
#ifndef XIR_LIBRARY_READER_CASES_H
#define XIR_LIBRARY_READER_CASES_H
static XrXirStatus library_reader_operation(const XrXirCompileContext *context,void *opaque) {
    LibraryPacketFixture *fixture=opaque;XrXirArtifact *artifact=NULL;
    XrXirStatus status=xr_xir_compile_checked_read(context,fixture->bytes,fixture->length,&artifact,NULL);
    CHECK(status==XR_XIR_OK?artifact!=NULL:artifact==NULL);xr_xir_compile_artifact_free(artifact);return status;
}
static void reader_owned_cases(const uint8_t *bytes,size_t length) {
    CHECK(length>=80);
    LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
    XrXirArtifact *first=NULL,*second=NULL;
    XrCompileResourceStats initial=library_compile_stats(&owner.context);
    CHECK(xr_xir_compile_checked_read(&owner.context,bytes,length,&first,NULL)==XR_XIR_OK);
    XrCompileResourceStats after=library_compile_stats(&owner.context);
    CHECK(after.held_bytes>initial.held_bytes);
    CHECK(xr_xir_compile_checked_read(&owner.context,bytes,length,&second,NULL)==XR_XIR_OK);
    XrCompileResourceStats both=library_compile_stats(&owner.context);
    CHECK(both.held_bytes>after.held_bytes && both.peak_bytes>after.peak_bytes);
    xr_xir_compile_artifact_free(first);xr_xir_compile_artifact_free(second);
    CHECK(library_compile_stats(&owner.context).held_bytes==initial.held_bytes);
    library_compile_owner_drop(&owner);
    XrCompileResourceLimits limited={XR_COMPILE_MEMORY_FINITE,both.peak_bytes-1,true};
    CHECK(library_compile_owner_new(&owner,&limited)==XR_XIR_OK);first=NULL;second=NULL;
    CHECK(xr_xir_compile_checked_read(&owner.context,bytes,length,&first,NULL)==XR_XIR_OK);
    after=library_compile_stats(&owner.context);
    const XrXirModule *module=xr_xir_compile_artifact_module(first);CHECK(module);
    CHECK(xr_xir_compile_checked_read(&owner.context,bytes,length,&second,NULL)==XR_XIR_MEMORY_LIMIT&&!second);
    XrCompileResourceStats failed=library_compile_stats(&owner.context);
    CHECK(failed.held_bytes==after.held_bytes && xr_xir_compile_artifact_module(first)==module);
    xr_xir_compile_artifact_free(first);library_compile_owner_drop(&owner);
    uint8_t *bad=xr_malloc(length);CHECK(bad);memcpy(bad,bytes,length);memset(bad+76,255,4);
    XrSHA256Context hash;xr_sha256_init(&hash);xr_sha256_update(&hash,bad,32);xr_sha256_update(&hash,bad+64,length-64);xr_sha256_final(&hash,bad+32);
    CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);initial=library_compile_stats(&owner.context);first=NULL;
    CHECK(xr_xir_compile_checked_read(&owner.context,bad,length,&first,NULL)==XR_XIR_BAD_STRUCTURE&&!first);
    failed=library_compile_stats(&owner.context);CHECK(failed.held_bytes==initial.held_bytes);
    library_compile_owner_drop(&owner);xr_free(bad);
    LibraryPacketFixture fixture={bytes,length,NULL};library_compile_operation_cases("Checked owned reader",library_reader_operation,&fixture);
}

#endif
