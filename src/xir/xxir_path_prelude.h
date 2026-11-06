/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_path_prelude.h - Declaration-only visibility for the source-owned Path class
 *
 * KEY CONCEPT:
 *   The builtin route identifies one ordinary declaration without importing its values.
 */
#ifndef XXIR_PATH_PRELUDE_H
#define XXIR_PATH_PRELUDE_H
#include "xxir_nominal.h"
#include "xxir_prelude_source_internal.h"
#include <stdio.h>
#include <string.h>
static inline const char *xir_path_prelude_module(void) {
    return xir_prelude_declaration_module("Path");
}
static inline bool xir_path_prelude_coordinates(char logical[64],char canonical[160]) {
    const char *module=xir_path_prelude_module();if(!module)return false;
    int size=snprintf(logical,64,"%s/%s.xr",module,module);
    if(size<0||size>=64)return false;
    int count=snprintf(canonical,160,"stdlib-module-v1:module=%zu:%s:path=%d:%s",strlen(module),module,size,logical);
    return count>0&&count<160;
}
static inline bool xir_path_prelude_literal(XrXirLiteral actual,const char *text) {
    size_t length=strlen(text);
    return actual.bytes&&actual.length==length&&!memcmp(actual.bytes,text,length);
}
/* A complete module supplies the actual declaration ordinal. This observation
 * is deliberately insufficient for construction, field or call authority. */
static inline bool xir_path_prelude_declaration(const XrXirNominalTable *table,uint32_t ordinal) {
    if(!table||ordinal>=table->count)return false;
    XrXirNominalIdentity identity;
    if(table->declarations) {
        const XrXirNominalDeclaration *d=table->declarations+ordinal;
        identity=(XrXirNominalIdentity){d->module,d->name,d->exported,d->parameter_count,NULL,
            d->field_count,d->kind,d->variants,d->variant_count,d->flags,d->native,d->parent};
        if((d->field_count&&!d->fields)||(d->parameter_count&&!d->constraints))return false;
    } else {
        if(!table->identities)return false;
        identity=table->identities[ordinal];
        if(identity.field_count&&!identity.fields)return false;
    }
    if(identity.kind!=XR_XIR_NOMINAL_CLASS||identity.exported!=1||identity.arity||
        identity.native.native_id||identity.variant_count||!xir_path_prelude_literal(identity.name,"Path"))return false;
    for(unsigned b=0;b<sizeof(identity.native.source_fingerprint);++b)
        if(identity.native.source_fingerprint[b])return false;
    char logical[64],canonical[160];if(!xir_path_prelude_coordinates(logical,canonical)||
        !xir_path_prelude_literal(identity.module,canonical))return false;
    uint32_t declared=0;
    for(uint32_t n=0;n<table->count;++n) {
        XrXirLiteral module=table->declarations?table->declarations[n].module:table->identities[n].module;
        XrXirLiteral name=table->declarations?table->declarations[n].name:table->identities[n].name;
        declared+=xir_path_prelude_literal(module,canonical)&&xir_path_prelude_literal(name,"Path");
    }
    return declared==1;
}
static inline bool xir_path_prelude_type(const XrXirDeclarations *scope,
    const XrXirNominalTable *table,uint32_t ordinal) {
    if(!scope||!scope->modules||!xir_path_prelude_declaration(table,ordinal))return false;
    char logical[64],canonical[160];if(!xir_path_prelude_coordinates(logical,canonical))return false;
    uint32_t owners=0;
    for(uint32_t m=0;m<scope->module_count;++m) {
        const XrXirSourceModule *module=scope->modules+m;
        owners+=module->name&&module->name_length==strlen(canonical)&&
            !memcmp(module->name,canonical,module->name_length);
    }
    return owners==1;
}
#endif // XXIR_PATH_PRELUDE_H
