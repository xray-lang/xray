/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_crypto_auth.h - Canonical standard-module authority for private byte leaves
 *
 * KEY CONCEPT:
 *   Native byte operations belong to their full received module identity.
 *   A callable number or a similarly spelled local declaration grants nothing.
 */
#ifndef XXIR_CRYPTO_AUTH_H
#define XXIR_CRYPTO_AUTH_H
#include "xxir_declarations.h"
#include <string.h>
static inline bool xir_crypto_scope(const XrXirDeclarations *declarations,uint32_t function) {
    static const char identity[]="stdlib-module-v1:module=6:crypto:path=16:crypto/crypto.xr";
    if (!declarations || !declarations->functions || !declarations->modules) return false;
    uint32_t owner=declarations->functions[function].module;
    if (owner>=declarations->module_count) return false;
    const XrXirSourceModule *module=declarations->modules+owner;
    return module->name && module->name_length==sizeof(identity)-1 && !module->lexical_scope &&
        !memcmp(module->name,identity,sizeof(identity)-1);
}
#endif // XXIR_CRYPTO_AUTH_H
