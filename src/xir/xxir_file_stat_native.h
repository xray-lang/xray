/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_file_stat_native.h - Exact immutable filesystem result identity
 *
 * KEY CONCEPT:
 *   The private native class is a result carrier, never a source constructor.
 */
#ifndef XXIR_FILE_STAT_NATIVE_H
#define XXIR_FILE_STAT_NATIVE_H
#include "xxir_nominal.h"
#include "xxir_types.h"
#include "../shared/xnative_declaration.h"
#include <string.h>

static inline bool xir_file_stat_name(XrXirLiteral name,const char *expected) {
    size_t length=strlen(expected);
    return name.bytes && name.length==length && !memcmp(name.bytes,expected,length);
}
static inline bool xir_file_stat_header(const XrXirNominalIdentity *identity) {
    const XrNativeTypeDeclaration *native=xr_native_declaration_by_id(XR_NATIVE_DECLARATION_FILE_STAT);
    if(!identity || !xr_native_declaration_validate(native) ||
        native->kind!=XR_NATIVE_DECLARATION_IDENTITY || native->parameter_count || native->member_count!=10 ||
        identity->native.native_id!=native->id || identity->kind!=XR_XIR_NOMINAL_CLASS || identity->exported ||
        identity->arity || identity->parent || identity->flags!=XR_XIR_NOMINAL_FINAL ||
        identity->field_count!=10 || identity->variant_count || identity->variants ||
        !xir_file_stat_name(identity->name,native->name) ||
        !xir_file_stat_name(identity->module,"stdlib-module-v1:module=2:io:path=8:io/io.xr") ||
        memcmp(identity->native.source_fingerprint,native->source_fingerprint.bytes,32))return false;
    if(identity->fields)for(uint32_t f=0;f<10;++f)
        if(identity->fields[f].flags || !xir_file_stat_name(identity->fields[f].name,native->members[f].name))return false;
    return true;
}
static inline bool xir_file_stat_field_type(const XrXirTypes *types,uint32_t field,XrXirType type) {
    if(field<5)return type==XR_XIR_I64;
    if(field>=7)return field<10 && type==XR_XIR_BOOL;
    const XrXirTypeNode *node=xr_xir_type_node(types,type);
    return node && node->kind==XR_XIR_TYPE_NULLABLE && !node->parameter_span && node->element==XR_XIR_I64;
}
static inline bool xir_file_stat_type(const XrXirTypes *types,XrXirType type) {
    const XrXirTypeNode *node=xr_xir_type_node(types,type);
    if(!node || node->kind!=XR_XIR_TYPE_NOMINAL || node->parameter_span || node->nominal.argument_count ||
        !types->nominals || node->nominal.declaration>=types->nominals->count ||
        node->nominal.field_count!=10 || !node->nominal.fields)return false;
    const XrXirNominalTable *table=types->nominals;uint32_t d=node->nominal.declaration;
    XrXirNominalIdentity identity;
    if(table->declarations) {
        const XrXirNominalDeclaration *decl=table->declarations+d;
        identity=(XrXirNominalIdentity){decl->module,decl->name,decl->exported,decl->parameter_count,NULL,
            decl->field_count,decl->kind,decl->variants,decl->variant_count,decl->flags,decl->native,decl->parent};
        if(decl->constraints || !decl->fields)return false;
    } else if(table->identities)identity=table->identities[d];else return false;
    if(!xir_file_stat_header(&identity))return false;
    const XrNativeTypeDeclaration *native=xr_native_declaration_by_id(XR_NATIVE_DECLARATION_FILE_STAT);
    for(uint32_t f=0;f<10;++f) {
        if(!xir_file_stat_field_type(types,f,node->nominal.fields[f]))return false;
        if(table->declarations) {
            const XrXirNominalField *field=table->declarations[d].fields+f;
            if(field->flags || field->type!=node->nominal.fields[f] ||
                !xir_file_stat_name(field->name,native->members[f].name))return false;
        }
    }
    return true;
}
static inline bool xir_file_stat_nullable(const XrXirTypes *types,XrXirType type) {
    const XrXirTypeNode *node=xr_xir_type_node(types,type);
    return node && node->kind==XR_XIR_TYPE_NULLABLE && !node->parameter_span && xir_file_stat_type(types,node->element);
}
#endif // XXIR_FILE_STAT_NATIVE_H
