/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_native_callable.h - Canonical bodies with ordinary throwing signatures
 */
#ifndef XXIR_NATIVE_CALLABLE_H
#define XXIR_NATIVE_CALLABLE_H
#include "xxir_types.h"
#include <string.h>
#include "xxir_nominal.h"
#include "../shared/xnative_declaration.h"

static inline bool xir_native_error_type(const XrXirTypes *types,XrXirType type,uint32_t id) {
    const XrXirTypeNode *node=xr_xir_type_node(types,type);
    if (!node || node->kind!=XR_XIR_TYPE_NOMINAL || node->nominal.argument_count ||
        !types->nominals || node->nominal.declaration>=types->nominals->count) return false;
    const XrXirNominalTable *table=types->nominals;
    const XrXirNominalNativeRecord *native=table->declarations ?
        &table->declarations[node->nominal.declaration].native :
        table->identities ? &table->identities[node->nominal.declaration].native : NULL;
    const XrNativeTypeDeclaration *canonical=xr_native_declaration_by_id(id);
    return native && canonical && native->native_id==id &&
        !memcmp(native->source_fingerprint,canonical->source_fingerprint.bytes,32);
}
static inline bool xir_native_callable_types(uint32_t id,const XrXirTypes *types,
    const XrXirType *parameters,uint32_t count,XrXirType result,XrXirType error) {
    const XrNativeCallableDeclaration *canonical=xr_native_callable_by_id(id);
    const XrXirTypeNode *slice=count==1 && parameters ? xr_xir_type_node(types,parameters[0]) : NULL;
    return canonical && canonical->owner->id==XR_NATIVE_DECLARATION_STRING &&
        canonical->member->operation==XR_NATIVE_OPERATION_STRING_FROM_UTF8 &&
        canonical->member->is_static && canonical->member->is_method &&
        canonical->member->parameter_count==1 && canonical->member->parameters[0].type==XR_NATIVE_TERM_SLICE_U8 &&
        canonical->member->result==XR_NATIVE_TERM_STRING &&
        canonical->member->ownership==XR_NATIVE_RESULT_OWNERSHIP_OWNED &&
        slice && slice->kind==XR_XIR_TYPE_SLICE && slice->element==XR_XIR_U8 &&
        result==XR_XIR_STRING && xir_native_error_type(types,error,canonical->error_e_type->id);
}
static inline XrXirType xir_native_callable_error(const XrXirTypes *types,uint32_t id) {
    const XrNativeCallableDeclaration *native=xr_native_callable_by_id(id);
    if (!native || !types) return XR_XIR_NEVER;
    for (uint32_t i=0;i<types->count;++i) {
        XrXirType type=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+i);
        if (xir_native_error_type(types,type,native->error_e_type->id)) return type;
    }
    return XR_XIR_NEVER;
}
static inline bool xir_native_callable_identity(const XrXirFunctionIdentity *id) {
    return id && id->native_callable && !id->exported && !id->nominal_owner && !id->member_access &&
        !id->cleanup_owner && !id->method_kind && !id->test_role && !id->test_timeout_seconds &&
        id->promises==XR_XIR_FUNCTION_NO_SUSPEND;
}
static inline XrXirStatus xir_native_callable_verify(const XrXirModule *module,uint32_t index) {
    if (!module || !module->declarations || !module->declarations->functions ||
        index>=module->function_count || !module->functions) return XR_XIR_BAD_STRUCTURE;
    const XrXirFunctionIdentity *id=&module->declarations->functions[index];
    if (!id->native_callable) return XR_XIR_OK;
    const XrXirFunction *function=&module->functions[index];
    if (!xir_native_callable_identity(id) ||
        (module->generics && (module->generics[index].parameter_count || module->generics[index].argument_count)) || function->blocks ||
        function->block_count || function->instructions || function->instruction_count ||
        function->operands || function->operand_count || index==module->declarations->entry_function)
        return XR_XIR_BAD_STRUCTURE;
    for (uint32_t m=0;m<module->declarations->module_count;++m)
        if (module->declarations->modules[m].initializer==index) return XR_XIR_BAD_STRUCTURE;
    return xir_native_callable_types(id->native_callable,module->types,function->parameters,
        function->parameter_count,function->result,function->error_type) ? XR_XIR_OK : XR_XIR_BAD_TYPE;
}
#endif
