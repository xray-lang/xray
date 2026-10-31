/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_slot_access_internal.h - Complete public slot read qualification
 *
 * KEY CONCEPT:
 *   Immutable export facts and a real direct dependency qualify value reads.
 *   A public read never grants initialization, write, place or loan authority.
 */
#ifndef XXIR_SLOT_ACCESS_INTERNAL_H
#define XXIR_SLOT_ACCESS_INTERNAL_H
#include "xxir_construction.h"

static inline bool xir_slot_public(const XrXirDeclarations *d,const XrXirSlotFacts *facts,uint32_t slot) {
    if(!d || !d->slots || slot>=d->slot_count || !facts || facts->count!=d->slot_count || !facts->rows)
        return false;
    const XrXirSlotFact *fact=&facts->rows[slot];
    return !d->slots[slot].mutable && fact->slot==slot && fact->name && fact->name_length && fact->exported==1;
}
static inline bool xir_slot_public_read(const XrXirDeclarations *d,const XrXirSlotFacts *facts,
    uint32_t from,uint32_t slot) {
    return d && d->modules && from<d->module_count && slot<d->slot_count &&
        d->slots && d->slots[slot].module<d->module_count && xir_slot_public(d,facts,slot) &&
        xr_xir_module_imports(d,from,d->slots[slot].module);
}
#endif // XXIR_SLOT_ACCESS_INTERNAL_H
