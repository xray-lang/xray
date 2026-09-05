/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_method_symbol.h - Stable builtin method identities
 */

#ifndef XI_METHOD_SYMBOL_H
#define XI_METHOD_SYMBOL_H

/* Selector text is diagnostic metadata. Compiler authorities compare these
 * generated IDs, whose values are the runtime symbol-table contract. */
typedef enum XiMethodSymbolId {
    XI_METHOD_SYMBOL_INVALID = 0,
#define XI_METHOD_SYM(aot_name, id, rt_name, display_name) XI_METHOD_SYMBOL_##aot_name = id,
#include "xi_method_sym.def"
#undef XI_METHOD_SYM
} XiMethodSymbolId;

static inline const char *xi_method_symbol_display_name(XiMethodSymbolId symbol) {
    switch (symbol) {
#define XI_METHOD_SYM(aot_name, id, rt_name, display_name)                                         \
    case XI_METHOD_SYMBOL_##aot_name:                                                              \
        return display_name;
#include "xi_method_sym.def"
#undef XI_METHOD_SYM
        case XI_METHOD_SYMBOL_INVALID:
            return NULL;
    }
    return NULL;
}

#endif /* XI_METHOD_SYMBOL_H */
