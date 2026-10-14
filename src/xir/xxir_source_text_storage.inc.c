/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_text_storage.inc.c - Immutable text owned by one Source context
 *
 * KEY CONCEPT:
 *   Equal bytes share stable owned storage. Text identity grants no declaration
 *   or execution authority; lookup still uses the original scope and bindings.
 */
static bool source_owned_text_slot(SourceContext *ctx, SourceOwnedTextEntry *table,
    uint32_t capacity, uint64_t hash, const char *text, uint32_t *output) {
    uint32_t slot = (uint32_t)hash & (capacity - 1);
    for (uint32_t probe = 0; probe < capacity; ++probe) {
        if (!source_work(ctx,NULL)) return false;
        if (!table[slot].text || (table[slot].hash == hash &&
            source_text_same(ctx,NULL,table[slot].text,text))) {
            *output = slot; return ctx->diagnostic.status == XR_XIR_OK;
        }
        if (ctx->diagnostic.status != XR_XIR_OK) return false;
        slot = (slot + 1) & (capacity - 1);
    }
    return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"source text index is full");
}
static bool source_owned_text_grow(SourceContext *ctx) {
    uint32_t previous = ctx->owned_text_capacity;
    if (previous > UINT32_MAX / 2)
        return source_fail(ctx,NULL,XR_XIR_BUDGET,"source text index capacity exhausted");
    uint32_t capacity = previous ? previous * 2 : 16;
    SourceOwnedTextEntry *table = source_alloc(ctx,capacity,sizeof(*table));
    if (!table) return false;
    for (uint32_t i = 0; i < previous; ++i) {
        if (!source_work(ctx,NULL)) return false;
        const SourceOwnedTextEntry *entry = &ctx->owned_texts[i];
        if (!entry->text) continue;
        uint32_t slot;
        if (!source_owned_text_slot(ctx,table,capacity,entry->hash,entry->text,&slot) ||
            !source_copy_bytes(ctx,NULL,&table[slot],entry,sizeof(*entry))) return false;
    }
    /* Only this index reads its rows; callers retain the separate text blocks. */
    if (ctx->owned_texts) source_release_private(ctx->owned_texts);
    ctx->owned_texts = table; ctx->owned_text_capacity = capacity;
    return true;
}
static void source_owned_text_index_clear(SourceContext *ctx) {
    if (ctx->owned_texts) source_release_private(ctx->owned_texts);
    ctx->owned_texts = NULL; ctx->owned_text_count = 0; ctx->owned_text_capacity = 0;
}
static const char *source_owned_text(SourceContext *ctx, const char *text) {
    size_t length;
    if (!source_text_length(ctx,NULL,text,&length) ||
        !source_work_units(ctx,NULL,length + 1)) return NULL;
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < length; ++i) {
        hash ^= (unsigned char)text[i]; hash *= UINT64_C(1099511628211);
    }
    uint32_t slot = 0;
    if (ctx->owned_text_capacity) {
        if (!source_owned_text_slot(ctx,ctx->owned_texts,ctx->owned_text_capacity,hash,text,&slot)) return NULL;
        if (ctx->owned_texts[slot].text) return ctx->owned_texts[slot].text;
    }
    if (!ctx->owned_text_capacity ||
        ctx->owned_text_count >= ctx->owned_text_capacity - ctx->owned_text_capacity / 4) {
        if (!source_owned_text_grow(ctx) ||
            !source_owned_text_slot(ctx,ctx->owned_texts,ctx->owned_text_capacity,hash,text,&slot)) return NULL;
    }
    char *copy = source_alloc(ctx,length + 1,1);
    if (!copy || !source_copy_bytes(ctx,NULL,copy,text,length + 1)) return NULL;
    ctx->owned_texts[slot] = (SourceOwnedTextEntry){copy,hash}; ++ctx->owned_text_count;
    return copy;
}
