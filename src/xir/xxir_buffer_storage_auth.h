/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_buffer_storage_auth.h - Exact private byte-storage module reception
 *
 * KEY CONCEPT:
 *   A builtin type code or callable number alone does not establish a scope.
 *   The received function must belong to the actual canonical module record.
 */
#ifndef XXIR_BUFFER_STORAGE_AUTH_H
#define XXIR_BUFFER_STORAGE_AUTH_H
#include "xxir_declarations.h"
#include <string.h>
static inline bool xir_buffer_storage_scope(const XrXirDeclarations *declarations,uint32_t function) {
    static const char identity[]="stdlib-module-v1:module=3:mem:path=10:mem/mem.xr";
    if (!declarations || !declarations->functions || !declarations->modules) return false;
    uint32_t owner=declarations->functions[function].module;
    if (owner>=declarations->module_count) return false;
    const XrXirSourceModule *module=&declarations->modules[owner];
    return module->name && module->name_length==sizeof(identity)-1 &&
        !memcmp(module->name,identity,sizeof(identity)-1) && !module->lexical_scope;
}
#endif // XXIR_BUFFER_STORAGE_AUTH_H
