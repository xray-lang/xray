/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_cgen_method_symbols.inc.c - AOT builtin method symbol resolution
 */

static int cg_method_sym(const char *name) {
    if (!name)
        return -1;
    static const struct {
        const char *name;
        int sym;
    } map[] = {
#define XI_METHOD_SYM(aot_name, id, rt_name, display_name) {display_name, XRT_SYM_##aot_name},
#include "../ir/xi_method_sym.def"
#undef XI_METHOD_SYM
        {"size", XRT_SYM_SIZE},
        {"add", XRT_SYM_SET},
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (strcmp(name, map[i].name) == 0)
            return map[i].sym;
    }
    return -1;
}
