/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_internal_storage.h - Generated table of internal native storage classes
 *
 * KEY CONCEPT:
 *   No storage name is special cased in the compiler. The generated table names
 *   every internal native class, the standard module that owns it and the
 *   carrier it projects, so a name resolves only inside its own module.
 */
#ifndef XXIR_INTERNAL_STORAGE_H
#define XXIR_INTERNAL_STORAGE_H
#include <string.h>
#include "xxir_types.h"
#include "../shared/xnative_declaration.h"

static inline const XrInternalStorageType *xir_internal_storage_lookup(const char *module, const char *name) {
    if (!module || !name) return NULL;
    uint32_t count = 0;
    const XrInternalStorageType *table = xr_internal_storage_table(&count);
    for (uint32_t i = 0; i < count; ++i)
        if (!strcmp(table[i].module, module) && !strcmp(table[i].name, name)) return &table[i];
    return NULL;
}
/* Any module: a name from the table that resolves nowhere names a storage class
 * whose visibility does not include the current module. */
static inline const XrInternalStorageType *xir_internal_storage_lookup_name(const char *name) {
    if (!name) return NULL;
    uint32_t count = 0;
    const XrInternalStorageType *table = xr_internal_storage_table(&count);
    for (uint32_t i = 0; i < count; ++i)
        if (!strcmp(table[i].name, name)) return &table[i];
    return NULL;
}
static inline bool xir_internal_storage_nominal_table(uint32_t id, const char **module, const char **name) {
    uint32_t count = 0;
    const XrInternalStorageType *table = xr_internal_storage_table(&count);
    for (uint32_t i = 0; i < count; ++i)
        if (table[i].carrier == XR_INTERNAL_STORAGE_NOMINAL && table[i].native_id == id) {
            if (module) *module = table[i].module;
            if (name) *name = table[i].name;
            return true;
        }
    return false;
}
#endif // XXIR_INTERNAL_STORAGE_H
