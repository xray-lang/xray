/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_stdlib.c - Generated standard library symbols for LSP
 */

#include "xlsp_stdlib.h"
#include <string.h>

#include "xlsp_stdlib_generated.inc"

const XlspModuleInfo *xlsp_stdlib_get_modules(int *count) {
    if (count)
        *count = g_xlsp_stdlib_module_count;
    return g_xlsp_stdlib_modules;
}

const XlspModuleInfo *xlsp_stdlib_find_module(const char *name) {
    if (!name)
        return NULL;

    for (int i = 0; i < g_xlsp_stdlib_module_count; i++) {
        if (strcmp(g_xlsp_stdlib_modules[i].name, name) == 0)
            return &g_xlsp_stdlib_modules[i];
    }
    return NULL;
}

const XlspSymbolInfo *xlsp_stdlib_find_symbol(const XlspModuleInfo *module, const char *name) {
    if (!module || !name)
        return NULL;

    for (int i = 0; i < module->symbol_count; i++) {
        if (strcmp(module->symbols[i].name, name) == 0)
            return &module->symbols[i];
    }
    return NULL;
}
