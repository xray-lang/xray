/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_generic_method_old_cache.h - Native cache proof version rejection
 *
 * KEY CONCEPT:
 *   Runtime metadata admission still requires an exact current semantic proof.
 */
#ifndef XIR_GENERIC_METHOD_OLD_CACHE_H
#define XIR_GENERIC_METHOD_OLD_CACHE_H
#include "base/xsha256.h"
#include "base/xmalloc.h"
#include <stddef.h>
#include <string.h>
static void generic_method_old_cache(const XrXirProgramSpec *native) {
    CHECK(native && native->proof.bytes && native->proof.length >= 64);
    CHECK(!native->code.owner && !native->code.release);
    XrXirProgram *program = NULL;
    CHECK(xr_xir_program_seal(native,(XrXirProgramBudget){33554432,64000000},&program) == XR_XIR_OK);
    xr_xir_program_drop(program);
    uint8_t *bytes = xr_malloc(native->proof.length); CHECK(bytes);
    for (uint32_t mode = 0; mode < 2; ++mode) {
        memcpy(bytes,native->proof.bytes,native->proof.length);
        size_t offset = mode ? 12 : 8;
        uint32_t old = mode ? 45 : 17;
        for (uint32_t b = 0; b < 4; ++b) bytes[offset+b] = (uint8_t)(old >> (b*8));
        XrSHA256Context sha; xr_sha256_init(&sha);
        xr_sha256_update(&sha,bytes,32);
        xr_sha256_update(&sha,bytes+64,native->proof.length-64);
        xr_sha256_final(&sha,bytes+32);
        uint8_t identity[32]; xr_sha256(bytes,native->proof.length,identity);
        XrXirProgramSpec stale = *native; stale.proof.bytes = bytes; stale.proof.identity = identity;
        program = (XrXirProgram *)(uintptr_t)1;
        CHECK(xr_xir_program_seal(&stale,(XrXirProgramBudget){33554432,64000000},&program) == XR_XIR_BAD_STRUCTURE);
        CHECK(!program);
    }
    xr_free(bytes);
}
_Static_assert(XR_XIR_PROGRAM_ABI_VERSION == 24,"runtime program metadata ABI");
_Static_assert(sizeof(void *) != 8 || sizeof(XrXirNominalIdentity) == 72,"runtime nominal identity stride");
_Static_assert(sizeof(void *) != 8 || offsetof(XrXirNominalIdentity,flags) == 68,"runtime nominal final fact offset");
_Static_assert(sizeof(XrXirFunctionIdentity) == 28,"runtime identity stride remains unchanged");
_Static_assert(sizeof(void *) != 8 || sizeof(XrXirDeclarations) == 72,"runtime declaration stride remains unchanged");
_Static_assert(sizeof(void *) != 8 || offsetof(XrXirDeclarations,implementations) == 64,"runtime implementation pointer offset");
#endif // XIR_GENERIC_METHOD_OLD_CACHE_H
