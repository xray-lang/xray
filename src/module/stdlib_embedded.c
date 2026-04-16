/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * stdlib_embedded.c - Stub for embedded stdlib bytecode API
 *
 * KEY CONCEPT:
 *   All built-in stdlib modules are now pure C. No embedded bytecode is
 *   shipped for built-in modules.
 *
 *   The API (xr_get_embedded_stdlib_bytecode / xr_get_embedded_stdlib)
 *   is retained so that third-party hybrid modules can provide their own
 *   embedded bytecode via a custom provider in the future.
 *
 *   The script extension loading mechanism in xmodule.c (file-system
 *   fallback + export override) is also preserved for third-party use.
 */

#include <stddef.h>
#include <stdint.h>

// Embedded bytecode lookup — always returns NULL (no built-in embedded scripts)
const uint8_t* xr_get_embedded_stdlib_bytecode(const char *module_name, size_t *out_size) {
    (void)module_name;
    (void)out_size;
    return NULL;
}

// Legacy source lookup — always returns NULL
const char* xr_get_embedded_stdlib(const char *module_name) {
    (void)module_name;
    return NULL;
}
