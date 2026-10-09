/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_library_slots.inc.c - Owned private state relocation
 *
 * KEY CONCEPT:
 *   Slot ownership follows selected module views; root permission is independent.
 */
static bool source_library_slot_map(SourceContext *ctx,const XrXirModule *library,
    SourceLibraryMap *map,uint32_t first) {
    const XrXirDeclarations *d=library->declarations;
    map->slot_count=d->slot_count;map->next_slot=first;
    if (!d->slot_count) return true;
    map->slots=source_scratch(ctx,d->slot_count,sizeof(*map->slots),false);
    if (!map->slots) return false;
    for (uint32_t s=0; s<d->slot_count; ++s) {
        if (!source_work(ctx,NULL)) return false;
        uint32_t owner=d->slots[s].module;
        if (owner>=map->module_count)
            return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"library slot owner is invalid");
        map->slots[s]=UINT32_MAX;
        if (map->modules[owner]==UINT32_MAX) continue;
        if (map->next_slot>=ctx->slot_count)
            return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"library slot inventory is inconsistent");
        map->slots[s]=map->next_slot++;
    }
    return true;
}
static bool source_library_slot_instruction(SourceContext *ctx,const SourceLibraryMap *map,
    XrXirInstruction *op) {
    if (op->op!=XR_XIR_SLOT_GROUP_INIT) {
        if (op->immediate<0 || (uint64_t)op->immediate>=map->slot_count ||
            !map->slots || map->slots[(uint32_t)op->immediate]==UINT32_MAX)
            return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"library slot identity is invalid");
        op->immediate=map->slots[(uint32_t)op->immediate];return true;
    }
    uint64_t packed=(uint64_t)op->immediate;
    uint32_t first=(uint32_t)(packed>>32),count=(uint32_t)packed;
    if (!count || first>map->slot_count || count>map->slot_count-first ||
        !map->slots || map->slots[first]==UINT32_MAX)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"library slot group is invalid");
    uint32_t target=map->slots[first];
    for (uint32_t i=0; i<count; ++i) {
        if (!source_work(ctx,NULL)) return false;
        if (i>UINT32_MAX-target || map->slots[first+i]!=target+i)
            return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"library slot group is not contiguous");
    }
    op->immediate=(int64_t)(((uint64_t)target<<32)|count);return true;
}
static bool source_library_slots(SourceContext *ctx,const XrXirModule *library,
    const SourceLibraryMap *map) {
    const XrXirDeclarations *d=library->declarations;
    for (uint32_t s=0; s<map->slot_count; ++s) {
        if (!source_work(ctx,NULL)) return false;
        uint32_t target=map->slots[s];
        if (target==UINT32_MAX) continue;
        const XrXirSlot *original=&d->slots[s];
        if (target>=ctx->slot_count || original->module>=map->module_count ||
            map->modules[original->module]==UINT32_MAX || original->mutable>1 ||
            !library_catalog_state_type(library->types,original->type))
            return source_fail(ctx,NULL,XR_XIR_BAD_STAGE,"library slot relocation is not admitted");
        XrXirSlot owned=*original;
        if (!source_library_type(ctx,map->unit,original->type,&owned.type)) return false;
        owned.module=map->modules[original->module];
        if (!source_copy_bytes(ctx,NULL,&ctx->slots[target],&owned,sizeof(owned))) return false;
    }
    return true;
}
