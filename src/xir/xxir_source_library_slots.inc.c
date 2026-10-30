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
static bool source_slot_fact(SourceContext *ctx,uint32_t slot,const char *name,
    uint32_t length,uint32_t exported) {
    if (slot>=ctx->slot_count || slot>=ctx->slot_capacity || !ctx->slot_facts ||
        !ctx->slot_fact_present || ctx->slot_fact_present[slot] || !name || !length || exported>1)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"source slot name has no unique actual owner");
    char *owned=source_alloc(ctx,length,1);
    if (!owned || !source_copy_bytes(ctx,NULL,owned,name,length)) return false;
    ctx->slot_facts[slot]=(XrXirSlotFact){slot,owned,length,exported};
    ctx->slot_fact_present[slot]=true;return true;
}
static bool source_library_slot_map(SourceContext *ctx,const XrXirModule *library,
    SourceLibraryMap *map,uint32_t first) {
    const XrXirDeclarations *d=library->declarations;
    map->slot_count=d->slot_count;map->next_slot=first;
    if (!d->slot_count) return true;
    map->slots=source_scratch(ctx,d->slot_count,sizeof(*map->slots),false);
    if (!map->slots) return false;
    for (uint32_t s=0; s<d->slot_count; ++s) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
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
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        if (i>UINT32_MAX-target || map->slots[first+i]!=target+i)
            return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"library slot group is not contiguous");
    }
    op->immediate=(int64_t)(((uint64_t)target<<32)|count);return true;
}
static bool source_library_public_slot(SourceContext *ctx,const XrXirModule *library,
    const SourceLibraryMap *map,const XrXirSlotFacts *facts,uint32_t slot,uint32_t target) {
    const XrXirSlot *original=&library->declarations->slots[slot];
    const XrXirSlotFact *fact=&facts->rows[slot];
    if (library->linkage_kind!=XR_XIR_LIBRARY || original->mutable ||
        (original->type!=XR_XIR_I64 && original->type!=XR_XIR_F64) || fact->exported!=1) return true;
    for (uint32_t s=0;s<map->slot_count;++s) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        if (s==slot || map->slots[s]==UINT32_MAX ||
            library->declarations->slots[s].module!=original->module) continue;
        const XrXirSlotFact *peer=&facts->rows[s];
        if (peer->name_length==fact->name_length &&
            source_span_same(ctx,NULL,peer->name,fact->name,fact->name_length))
            return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"library public slot name conflicts with actual module state");
        if (ctx->diagnostic.status!=XR_XIR_OK) return false;
    }
    char *name=source_library_owned_name(ctx,fact->name,fact->name_length);bool visible=false;
    if (!name || !source_library_source_name(ctx,name,fact->name_length,&visible)) return false;
    if (!visible) return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"library public slot has no Source declaration spelling");
    uint32_t owner=ctx->slots[target].module;ctx->module=owner;
    SourceName *symbol=add_name(ctx,&ctx->names[owner],name,NULL);
    if (!symbol) return false;
    symbol->kind=SOURCE_SLOT;symbol->index=target;symbol->module=owner;
    symbol->type=ctx->slots[target].type;symbol->checked_library=true;
    if (!source_query_declare(ctx,symbol,XR_XIR_SOURCE_BINDING,0,(XrXirSourceRange){owner,0,0,0,0})) return false;
    XrHirBodyBinding binding={0};if(!source_hir_binding_register(ctx,symbol,&binding))return false;
    source_query_binding_type(ctx,symbol);
    ((XrXirSourceDeclaration *)ctx->query.declarations)[symbol->declaration-1].exported=true;
    return true;
}
static bool source_library_slots(SourceContext *ctx,const XrXirModule *library,
    const SourceLibraryMap *map) {
    const XrXirDeclarations *d=library->declarations;
    const XrXirSlotFacts *facts=map->unit?xr_xir_compile_construction_slots(map->unit->construction):NULL;
    if (!facts || facts->count!=d->slot_count || (facts->count && !facts->rows) ||
        map->slot_count!=d->slot_count || map->unit->library!=library)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"library complete slot facts are missing");
    for (uint32_t s=0; s<map->slot_count; ++s) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        uint32_t target=map->slots[s];
        if (target==UINT32_MAX) continue;
        const XrXirSlot *original=&d->slots[s];
        if (target>=ctx->slot_count || original->module>=map->module_count ||
            map->modules[original->module]==UINT32_MAX || original->mutable>1 ||
            !library_catalog_state_slot(library->types,d,facts,s))
            return source_fail(ctx,NULL,XR_XIR_BAD_STAGE,"library slot relocation is not admitted");
        XrXirSlot owned=*original;
        if (!source_library_type(ctx,map->unit,original->type,&owned.type)) return false;
        owned.module=map->modules[original->module];
        if (!source_copy_bytes(ctx,NULL,&ctx->slots[target],&owned,sizeof(owned))) return false;
        const XrXirSlotFact *fact=&facts->rows[s];
        if (fact->slot!=s)
            return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"library slot fact ordinal is inconsistent");
        if (!source_slot_fact(ctx,target,fact->name,fact->name_length,fact->exported) ||
            !source_library_public_slot(ctx,library,map,facts,s,target)) return false;
    }
    return true;
}
