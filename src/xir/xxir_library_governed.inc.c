/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_library_governed.inc.c - Exact native owner module after common verification
 *
 * KEY CONCEPT: This lookup reuses authenticated records; it grants no native proof.
 */
#ifndef XXIR_LIBRARY_GOVERNED_INC
#define XXIR_LIBRARY_GOVERNED_INC
#include "xxir_nominal.h"
#include "../shared/xnative_declaration.h"
/* Callers must first run complete Checked verification with construction.
 * The common native owner authenticates shape, fingerprint and canonical name.
 * This bounded lookup only relates that owner to its actual module ordinal. */
static XrXirStatus library_governed_ordering_module(const XrXirCompileContext *context,
    const XrXirModule *module,uint32_t ordinal,bool *output) {
    const XrXirDeclarations *declarations=module->declarations;
    if (!declarations || ordinal>=declarations->module_count || !output) return XR_XIR_BAD_STRUCTURE;
    const XrXirSourceModule *view=&declarations->modules[ordinal];
    const XrXirNominalTable *table=module->types ? module->types->nominals : NULL;
    bool owned=false;
    for (uint32_t n=0;table && n<table->count;++n) {
        if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
        const XrXirNominalDeclaration *record=&table->declarations[n];
        if (record->native.native_id!=XR_NATIVE_DECLARATION_ORDERING) continue;
        if (!xir_compile_work(context,record->module.length)) return XR_XIR_BUDGET;
        if (record->module.length==view->name_length &&
            !memcmp(record->module.bytes,view->name,view->name_length)) owned=true;
    }
    *output=owned;return XR_XIR_OK;
}
#endif
