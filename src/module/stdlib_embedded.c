/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * stdlib_embedded.c - Embedded stdlib script lookup
 *
 * KEY CONCEPT:
 *   Pure-Xray stdlib modules must be loadable without relying on cwd or an
 *   installed stdlib directory. Bytecode is the preferred artifact; embedded
 *   source remains as a bootstrap fallback while the stdlib bytecode generator
 *   is being wired into the build.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    const char *name;
    const char *source;
} XrEmbeddedStdlibSource;

typedef struct {
    const char *name;
    const uint8_t *bytecode;
    size_t size;
} XrEmbeddedStdlibBytecode;

#include <stdlib_embedded_generated.inc>

const uint8_t *xr_get_embedded_stdlib_bytecode(const char *module_name, size_t *out_size) {
    if (out_size)
        *out_size = 0;
    if (!module_name)
        return NULL;
    for (size_t i = 0; i < xr_embedded_stdlib_bytecode_count; i++) {
        const XrEmbeddedStdlibBytecode *entry = &xr_embedded_stdlib_bytecodes[i];
        if (entry->name && strcmp(entry->name, module_name) == 0) {
            if (out_size)
                *out_size = entry->size;
            return entry->bytecode;
        }
    }
    return NULL;
}

const char *xr_get_embedded_stdlib(const char *module_name) {
    if (!module_name)
        return NULL;
    for (size_t i = 0; i < xr_embedded_stdlib_source_count; i++) {
        const XrEmbeddedStdlibSource *entry = &xr_embedded_stdlib_sources[i];
        if (strcmp(entry->name, module_name) == 0)
            return entry->source;
    }
    return NULL;
}
