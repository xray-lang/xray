/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 */
/* Fixed canonical inventory after complete Checked and construction verification.
 * Native descriptor proof remains the existing common nominal verifier. */
#ifndef XXIR_LIBRARY_PRELUDE_INC
#define XXIR_LIBRARY_PRELUDE_INC
#include "xxir_library_governed.inc.c"
#include "xxir_prelude_source_internal.h"
#include "xxir_interface.h"
#include "xxir_implementation.h"
static XrXirStatus library_prelude_literal(const XrXirCompileContext *context,
    XrXirLiteral actual,const char *expected,bool *equal) {
    size_t length=0;
    while (true) {
        if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
        if (!expected[length]) break;
        if (length==SIZE_MAX-1) return XR_XIR_BUDGET;
        ++length;
    }
    if (!xir_compile_work(context,length)) return XR_XIR_BUDGET;
    *equal=actual.length==length && !memcmp(actual.bytes,expected,length); return XR_XIR_OK;
}
static XrXirStatus library_prelude_module(const XrXirCompileContext *context,
    const XrXirModule *module,uint32_t ordinal,bool *output) {
    const XrXirDeclarations *d=module->declarations;
    if (!d || ordinal>=d->module_count || !output) return XR_XIR_BAD_STRUCTURE;
    const XrXirSourceModule *view=&d->modules[ordinal]; bool canonical=false;
    XrXirStatus status=library_prelude_literal(context,(XrXirLiteral){view->name,view->name_length},XIR_PRELUDE_CANONICAL,&canonical);
    if (status!=XR_XIR_OK) return status;
    if (!canonical) { *output=false; return XR_XIR_OK; }
    bool governed=false;
    status=library_governed_ordering_module(context,module,ordinal,&governed);
    if (status!=XR_XIR_OK) return status;
    if (!governed || view->dependency_count || !module->types || !module->types->nominals)
        return XR_XIR_BAD_STRUCTURE;
    const XrXirNominalTable *table=module->types->nominals; uint32_t owned=0;
    uint32_t seen[2]={0,0};
    if (xir_prelude_source.enum_count!=2) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t i=0;i<table->count;++i) {
        if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
        const XrXirNominalDeclaration *record=&table->declarations[i]; bool owner=false;
        status=library_prelude_literal(context,record->module,XIR_PRELUDE_CANONICAL,&owner);
        if (status!=XR_XIR_OK) return status;
        if (!owner) continue;
        ++owned; uint32_t match=UINT32_MAX;
        for (uint32_t e=0;e<xir_prelude_source.enum_count;++e) {
            bool equal=false; status=library_prelude_literal(context,record->name,xir_prelude_source.enums[e].name,&equal);
            if (status!=XR_XIR_OK) return status;
            if (equal) match=e;
        }
        if (match==UINT32_MAX || seen[match]++ || record->kind!=XR_XIR_NOMINAL_ENUM ||
            record->exported!=1 || record->parameter_count || record->field_count || record->flags)
            return XR_XIR_BAD_STRUCTURE;
        const XirPreludeEnum *expected=&xir_prelude_source.enums[match];
        if (record->native.native_id!=expected->native_id || record->variant_count!=expected->variant_count)
            return XR_XIR_BAD_STRUCTURE;
        if (!xir_compile_work(context,sizeof(record->native.source_fingerprint))) return XR_XIR_BUDGET;
        for (size_t byte=0;byte<sizeof(record->native.source_fingerprint);++byte) {
            uint8_t wanted=expected->native_id ? xir_prelude_source.input_sha256[byte] : 0;
            if (record->native.source_fingerprint[byte]!=wanted) return XR_XIR_BAD_STRUCTURE;
        }
        for (uint32_t v=0;v<expected->variant_count;++v) {
            const XrXirNominalVariant *variant=&record->variants[v]; bool equal=false;
            status=library_prelude_literal(context,variant->name,expected->variants[v],&equal);
            if (status!=XR_XIR_OK) return status;
            if (!equal || variant->field_begin || variant->field_count) return XR_XIR_BAD_STRUCTURE;
        }
    }
    if (owned!=xir_prelude_source.enum_count || !seen[0] || !seen[1]) return XR_XIR_BAD_STRUCTURE;
    const XrXirImplementationTable *implementations=d->implementations;
    for (uint32_t i=0;implementations && i<implementations->count;++i) {
        bool owner=false;
        status=library_prelude_literal(context,
            table->declarations[implementations->records[i].nominal_declaration].module,XIR_PRELUDE_CANONICAL,&owner);
        if (status!=XR_XIR_OK) return status;
        if (owner) return XR_XIR_BAD_STRUCTURE;
    }
    const XrXirInterfaceTable *interfaces=module->types->interfaces;
    for (uint32_t i=0;interfaces && i<interfaces->count;++i) {
        bool owner=false; status=library_prelude_literal(context,interfaces->declarations[i].module,XIR_PRELUDE_CANONICAL,&owner);
        if (status!=XR_XIR_OK) return status;
        if (owner) return XR_XIR_BAD_STRUCTURE;
    }
    for (uint32_t s=0;s<d->slot_count;++s) {
        if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
        if (d->slots[s].module==ordinal) return XR_XIR_BAD_STRUCTURE;
    }
    for (uint32_t f=0;f<module->function_count;++f) {
        if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
        if (d->functions[f].module==ordinal && f!=view->initializer) return XR_XIR_BAD_STRUCTURE;
    }
    const XrXirFunction *initializer=&module->functions[view->initializer];
    if (initializer->parameter_count || initializer->result!=XR_XIR_UNIT ||
        initializer->block_count!=1 || initializer->instruction_count!=1 || initializer->operand_count ||
        initializer->blocks[0].first || initializer->blocks[0].count!=1 ||
        initializer->instructions[0].op!=XR_XIR_RETURN || initializer->instructions[0].type!=XR_XIR_UNIT)
        return XR_XIR_BAD_STRUCTURE;
    *output=true; return XR_XIR_OK;
}
#endif
