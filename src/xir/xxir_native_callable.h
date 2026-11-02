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
#include "xxir_buffer_storage_auth.h"
#include "xxir_crypto_auth.h"
#include "xxir_mem_native.h"
#include "xxir_task_aggregate_auth.h"
#include "xxir_math_native.h"
#include "xxir_sys_native.h"
#include "xxir_os_native.h"
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
    if(id==XR_NATIVE_CALLABLE_SYS_THREAD_YIELD)return xir_sys_thread_yield_signature(parameters,count,result,error);
    if(xir_os_native_id(id))return xir_os_native_signature(id,parameters,count,result,error);
    if(xir_math_native_id(id))return xir_math_native_signature(id,parameters,count,result,error);
    if(xir_task_aggregate_native(id))return xir_task_aggregate_signature(id,types,parameters,count,result,error);
    if (xir_mem_native_id(id)) return xir_mem_native_signature(id,types,parameters,count,result,error);
    if (id==XR_NATIVE_CALLABLE_CRYPTO_FILL_RANDOM || id==XR_NATIVE_CALLABLE_CRYPTO_TIMING_EQUAL) {
        bool fill=id==XR_NATIVE_CALLABLE_CRYPTO_FILL_RANDOM;uint32_t required=fill?1u:2u;
        if (!canonical || canonical->owner || canonical->error_e_type || canonical->ctfe ||
            !canonical->namespace_id || strcmp(canonical->namespace_id,"crypto") ||
            !canonical->source_path || strcmp(canonical->source_path,"stdlib/defs/core.def") ||
            !canonical->member || strcmp(canonical->member->name,fill?"__fillRandomBytes":"__timingSafeEqualBytes") ||
            strcmp(canonical->member->signature,fill?"(bytes: ref Array<u8>): ()":"(a: Array<u8>, b: Array<u8>): bool") ||
            canonical->member->is_method || canonical->member->is_static || canonical->member->is_public ||
            canonical->member->parameter_count!=required || !canonical->member->parameters ||
            canonical->member->allocation!=(fill?XR_NATIVE_ALLOCATION_UNKNOWN:XR_NATIVE_ALLOCATION_NO_HEAP) ||
            canonical->member->ownership!=XR_NATIVE_RESULT_OWNERSHIP_OWNED ||
            strcmp(canonical->member->failures,"none") ||
            canonical->member->result!=(fill?XR_NATIVE_TERM_UNIT:XR_NATIVE_TERM_BOOL) ||
            count!=required || !parameters || result!=(fill?XR_XIR_UNIT:XR_XIR_BOOL) || error!=XR_XIR_NEVER) return false;
        for(uint32_t p=0;p<count;++p) {
            XrXirType type=parameters[p];
            if(fill) { if(!xr_xir_type_is_cell(types,type))return false;type=xr_xir_cell_element(types,type); }
            if(!xr_xir_type_is_array(types,type) || xr_xir_array_element(types,type)!=XR_XIR_U8 ||
                canonical->member->parameters[p].type!=XR_NATIVE_TERM_ARRAY_U8 ||
                canonical->member->parameters[p].optional || canonical->member->parameters[p].variadic) return false;
        }
        return true;
    }
    if (id==XR_NATIVE_CALLABLE_UNICODE_HAS_PROPERTY) {
        return canonical && !canonical->owner && !canonical->error_e_type && canonical->ctfe &&
            canonical->namespace_id && !strcmp(canonical->namespace_id,"regex") &&
            canonical->member && !canonical->member->is_method && !canonical->member->is_static &&
            canonical->member->parameter_count==2 && canonical->member->parameters &&
            canonical->member->parameters[0].type==XR_NATIVE_TERM_I64 &&
            canonical->member->parameters[1].type==XR_NATIVE_TERM_I64 &&
            canonical->member->result==XR_NATIVE_TERM_BOOL &&
            canonical->member->allocation==XR_NATIVE_ALLOCATION_NO_HEAP &&
            count==2 && parameters && parameters[0]==XR_XIR_I64 && parameters[1]==XR_XIR_I64 &&
            result==XR_XIR_BOOL && error==XR_XIR_NEVER;
    }
    if (id==XR_NATIVE_CALLABLE_UNICODE_PROPERTY_ID) {
        return canonical && !canonical->owner && !canonical->error_e_type && canonical->ctfe &&
            canonical->namespace_id && !strcmp(canonical->namespace_id,"regex") &&
            canonical->member && !canonical->member->is_method && !canonical->member->is_static &&
            canonical->member->parameter_count==1 && canonical->member->parameters &&
            canonical->member->parameters[0].type==XR_NATIVE_TERM_STRING &&
            canonical->member->result==XR_NATIVE_TERM_I64 &&
            canonical->member->allocation==XR_NATIVE_ALLOCATION_NO_HEAP &&
            count==1 && parameters && parameters[0]==XR_XIR_STRING && result==XR_XIR_I64 && error==XR_XIR_NEVER;
    }
    const XrXirTypeNode *slice=count==1 && parameters ? xr_xir_type_node(types,parameters[0]) : NULL;
    return canonical && canonical->owner && canonical->error_e_type && canonical->owner->id==XR_NATIVE_DECLARATION_STRING &&
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
    if (!native || !native->error_e_type || !types) return XR_XIR_NEVER;
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
    if(id->native_callable==XR_NATIVE_CALLABLE_SYS_THREAD_YIELD &&
        !xir_sys_thread_yield_scope(module->declarations,index,module->function_count))return XR_XIR_BAD_TYPE;
    if(xir_os_native_id(id->native_callable) &&
        !xir_os_native_scope(module->declarations,index,module->function_count))return XR_XIR_BAD_TYPE;
    if(xir_math_native_id(id->native_callable) &&
        !xir_math_native_scope(module->declarations,index,module->function_count))return XR_XIR_BAD_TYPE;
    if (xir_mem_native_id(id->native_callable) && !xir_buffer_storage_scope(module->declarations,index))
        return XR_XIR_BAD_TYPE;
    if (id->native_callable==XR_NATIVE_CALLABLE_BUFFER_SLICE_MUT_BEGIN &&
        (function->parameter_count!=3 || !function->parameters ||
            !xir_mem_buffer_ref_scope(module,index,function->parameters[2]))) return XR_XIR_BAD_TYPE;
    if ((id->native_callable==XR_NATIVE_CALLABLE_CRYPTO_FILL_RANDOM || id->native_callable==XR_NATIVE_CALLABLE_CRYPTO_TIMING_EQUAL) &&
        !xir_crypto_scope(module->declarations,index)) return XR_XIR_BAD_TYPE;
    if (!xir_native_callable_identity(id) ||
        (module->generics && (module->generics[index].parameter_count || module->generics[index].argument_count)) || function->blocks ||
        function->block_count || function->instructions || function->instruction_count ||
        function->operands || function->operand_count || function->consume_parameter_count ||
        function->consume_parameters || index==module->declarations->entry_function)
        return XR_XIR_BAD_STRUCTURE;
    for (uint32_t m=0;m<module->declarations->module_count;++m)
        if (module->declarations->modules[m].initializer==index) return XR_XIR_BAD_STRUCTURE;
    return xir_native_callable_types(id->native_callable,module->types,function->parameters,
        function->parameter_count,function->result,function->error_type) ? XR_XIR_OK : XR_XIR_BAD_TYPE;
}
#endif
