/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_text_storage_cases.h - Stable owned text and atomic index growth
 */
#ifndef XIR_SOURCE_TEXT_STORAGE_CASES_H
#define XIR_SOURCE_TEXT_STORAGE_CASES_H
static void source_text_storage_cases(void) {
    SourceContext context = {0}; CHECK(allocation_private_context(&context,allocation_limits()));
    const char *saved[64]; char caller[32];
    for (unsigned i = 0; i < 64; ++i) {
        CHECK(snprintf(caller,sizeof(caller),"owned-word-%u",i) > 0);
        saved[i] = source_owned_text(&context,caller); CHECK(saved[i]);
        memset(caller,'x',sizeof(caller));
    }
    CHECK(context.owned_text_count == 64 && context.owned_text_capacity == 128);
    size_t attempts_before = source_fixture_compile_attempts;
    XrCompileResourceStats before, after;
    CHECK(xr_compile_resources_stats(context.compile.resources,&before) == XR_COMPILE_RESOURCE_OK);
    for (unsigned i = 0; i < 64; ++i) {
        CHECK(snprintf(caller,sizeof(caller),"owned-word-%u",i) > 0);
        CHECK(!strcmp(saved[i],caller) && source_owned_text(&context,caller) == saved[i]);
    }
    CHECK(xr_compile_resources_stats(context.compile.resources,&after) == XR_COMPILE_RESOURCE_OK);
    CHECK(source_fixture_compile_attempts == attempts_before && after.allocated_bytes == before.allocated_bytes &&
        after.live_bytes == before.live_bytes && after.work > before.work);
    uint32_t occupied = 0, slot;
    while (!context.owned_texts[occupied].text) ++occupied;
    CHECK(source_owned_text_slot(&context,context.owned_texts,context.owned_text_capacity,
        context.owned_texts[occupied].hash,"a different value with the same supplied hash",&slot));
    CHECK(!context.owned_texts[slot].text && context.owned_text_count == 64);

    source_owned_text_index_clear(&context);
    CHECK(!context.owned_texts && !context.owned_text_count && !context.owned_text_capacity);
    for (unsigned i = 0; i < 64; ++i) {
        CHECK(snprintf(caller,sizeof(caller),"owned-word-%u",i) > 0 && !strcmp(saved[i],caller));
        CHECK(!strcmp(source_owned_text(&context,caller),saved[i]));
    }

    SourceOwnedTextEntry *original = context.owned_texts;
    uint32_t capacity = context.owned_text_capacity;
    source_fixture_compile_fail_at = source_fixture_compile_attempts; source_fixture_compile_injected = false;
    CHECK(!source_owned_text_grow(&context) && context.diagnostic.status == XR_XIR_OUT_OF_MEMORY &&
        source_fixture_compile_injected && context.owned_texts == original &&
        context.owned_text_capacity == capacity && context.owned_text_count == 64);
    source_fixture_compile_fail_at = SIZE_MAX;
    for (unsigned i = 0; i < 64; ++i) {
        CHECK(snprintf(caller,sizeof(caller),"owned-word-%u",i) > 0 && !strcmp(saved[i],caller));
    }
    allocation_private_close(&context);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    puts("Source text: owned bytes survive caller mutation, index growth and retirement; hits allocate nothing, collisions stay distinct, failed growth preserves old storage; physical0");
}
#endif // XIR_SOURCE_TEXT_STORAGE_CASES_H
