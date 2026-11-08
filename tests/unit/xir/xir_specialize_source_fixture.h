/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_specialize_source_fixture.h - Source text and declaration lookup for key tests
 */
#ifndef XIR_SPECIALIZE_SOURCE_FIXTURE_H
#define XIR_SPECIALIZE_SOURCE_FIXTURE_H

static void spec_index_text_append(char *text, size_t capacity, size_t *length, const char *piece) {
    size_t count = strlen(piece);
    CHECK(*length < capacity && count < capacity - *length);
    memcpy(text + *length, piece, count + 1);
    *length += count;
}

static uint32_t spec_index_source_function(const XrXirModule *module, const char *name) {
    size_t length = strlen(name);
    uint32_t output = UINT32_MAX;
    for (uint32_t f = 0; f < module->function_count; ++f)
        if (module->functions[f].name_length == length && !memcmp(module->functions[f].name, name, length)) {
            CHECK(output == UINT32_MAX);
            output = f;
        }
    CHECK(output != UINT32_MAX);
    return output;
}
#endif // XIR_SPECIALIZE_SOURCE_FIXTURE_H
