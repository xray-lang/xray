/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xsource_value_api.inc.c - Owned data and host scalars on the original ledger
 */
#include "../xir/xxir_types.h"
#include "../xir/xxir_nominal.h"
#include "../xir/xxir_type_arena.h"
_Static_assert(sizeof(float)==sizeof(uint32_t),"the public float carrier is binary32");
_Static_assert(sizeof(double)==sizeof(uint64_t),"the public double carrier is binary64");
static XrSourceStatus api_value_status(XrXirValueStatus status) {
    return status==XR_XIR_VALUE_OK ? XR_SOURCE_OK : status==XR_XIR_VALUE_OOM ? XR_SOURCE_OUT_OF_MEMORY :
        status==XR_XIR_VALUE_LIMIT || status==XR_XIR_VALUE_REFCOUNT_LIMIT ? XR_SOURCE_LIMIT :
        status==XR_XIR_VALUE_UNSUPPORTED ? XR_SOURCE_UNSUPPORTED :
        status==XR_XIR_VALUE_BAD_UTF8 ? XR_SOURCE_INVALID : XR_SOURCE_BAD_ARGUMENT;
}
static XrSourceStatus api_value_constructed(XrSourceCompiler *compiler,XrSourceStatus status) {
    if(status==XR_SOURCE_BUDGET || status==XR_SOURCE_OUT_OF_MEMORY)
        (void)xr_compile_state_fail(xr_source_compiler_state(compiler),status==XR_SOURCE_BUDGET ?
            XR_COMPILE_RESOURCE_BUDGET : XR_COMPILE_RESOURCE_OUT_OF_MEMORY);
    return status;
}
static bool api_scalar_type(XrSourceValueKind kind,XrXirType *type) {
    switch(kind) {
    case XR_SOURCE_VALUE_UNIT:*type=XR_XIR_UNIT;break;
    case XR_SOURCE_VALUE_BOOL:*type=XR_XIR_BOOL;break;
    case XR_SOURCE_VALUE_I8:*type=XR_XIR_I8;break;
    case XR_SOURCE_VALUE_I16:*type=XR_XIR_I16;break;
    case XR_SOURCE_VALUE_I32:*type=XR_XIR_I32;break;
    case XR_SOURCE_VALUE_I64:*type=XR_XIR_I64;break;
    case XR_SOURCE_VALUE_U8:*type=XR_XIR_U8;break;
    case XR_SOURCE_VALUE_U16:*type=XR_XIR_U16;break;
    case XR_SOURCE_VALUE_U32:*type=XR_XIR_U32;break;
    case XR_SOURCE_VALUE_U64:*type=XR_XIR_U64;break;
    case XR_SOURCE_VALUE_F32:*type=XR_XIR_F32;break;
    case XR_SOURCE_VALUE_F64:*type=XR_XIR_F64;break;
    case XR_SOURCE_VALUE_RUNE:*type=XR_XIR_RUNE;break;
    default:return false;
    }
    return true;
}
static XrSourceStatus api_value_wrap(XrCompileResources *resources,const XrXirValue *value,XrSourceValue **output) {
    if(!resources || !value || !output || *output)return XR_SOURCE_BAD_ARGUMENT;
    XrSourceValue *owner=NULL;
    XrSourceStatus status=api_resource(xr_compile_resources_calloc(resources,1,sizeof(*owner),(void **)&owner));
    if(status!=XR_SOURCE_OK)return status;
    owner->resources=resources;
    status=api_value_status(xr_xir_value_copy(value,&owner->value));
    if(status!=XR_SOURCE_OK){xr_compile_resources_free(owner);return status;}
    *output=owner;return XR_SOURCE_OK;
}
XRAY_API XrSourceStatus xray_source_value_new_scalar(XrSourceCompiler *compiler,const XrSourceScalar *scalar,
    XrSourceValue **output) {
    if(!compiler || !scalar || !output || *output)return XR_SOURCE_BAD_ARGUMENT;
    XrXirType type=XR_XIR_UNIT;
    if(!api_scalar_type(scalar->kind,&type))return XR_SOURCE_BAD_ARGUMENT;
    XrXirValue value={(uint32_t)type,0,0};
    if(scalar->kind>=XR_SOURCE_VALUE_I8 && scalar->kind<=XR_SOURCE_VALUE_I64)value.payload=scalar->value.signed_value;
    else if(scalar->kind>=XR_SOURCE_VALUE_U8 && scalar->kind<=XR_SOURCE_VALUE_U64)
        memcpy(&value.payload,&scalar->value.unsigned_value,sizeof(value.payload));
    else if(type==XR_XIR_BOOL)value.payload=scalar->value.boolean;
    else if(type==XR_XIR_RUNE)value.payload=scalar->value.rune;
    else if(type==XR_XIR_F32) {
        uint32_t bits;memcpy(&bits,&scalar->value.f32,sizeof(bits));value.payload=bits;
    } else if(type==XR_XIR_F64)memcpy(&value.payload,&scalar->value.f64,sizeof(value.payload));
    if(!xr_xir_value_valid(&value))return XR_SOURCE_INVALID;
    XrSourceStatus status=api_resource(xr_compile_state_status(xr_source_compiler_state(compiler)));
    if(status==XR_SOURCE_OK)status=api_value_wrap(xr_source_compiler_resources(compiler),&value,output);
    return api_value_constructed(compiler,status);
}
XRAY_API XrSourceStatus xray_source_value_new_string(XrSourceCompiler *compiler,const char *bytes,size_t length,
    XrSourceValue **output) {
    if(!compiler || !output || *output || (length && !bytes))return XR_SOURCE_BAD_ARGUMENT;
    if(length>PTRDIFF_MAX)return XR_SOURCE_LIMIT;
    XrSourceStatus status=api_resource(xr_compile_state_status(xr_source_compiler_state(compiler)));
    if(status!=XR_SOURCE_OK)return status;
    XrCompileResources *resources=xr_source_compiler_resources(compiler);XrXirDomain *domain=NULL;
    XrXirValueStatus made=xr_xir_compile_domain_new(resources,UINT64_MAX,&domain);
    if(made!=XR_XIR_VALUE_OK)return api_value_constructed(compiler,
        made==XR_XIR_VALUE_LIMIT ? XR_SOURCE_BUDGET : api_value_status(made));
    XrXirValue value={0};made=xr_xir_string_new(domain,bytes,length,&value);
    xr_xir_domain_drop(domain);
    status=made==XR_XIR_VALUE_LIMIT ? XR_SOURCE_BUDGET : api_value_status(made);
    if(status==XR_SOURCE_OK)status=api_value_wrap(resources,&value,output);
    xr_xir_value_drop(&value);return api_value_constructed(compiler,status);
}
XRAY_API XrSourceStatus xray_source_result_value(const XrSourceResult *result,XrSourceValue **output) {
    if(!result || !output || *output)return XR_SOURCE_BAD_ARGUMENT;
    if(result->value.status!=XR_XIR_CALL_RETURNED && result->value.status!=XR_XIR_CALL_THROWN)return XR_SOURCE_BAD_STATE;
    return api_value_wrap(result->resources,&result->value.value,output);
}
XRAY_API XrSourceStatus xray_source_value_copy(const XrSourceValue *value,XrSourceValue **output) {
    if(!value || !output || *output)return XR_SOURCE_BAD_ARGUMENT;
    return api_value_wrap(value->resources,&value->value,output);
}
XRAY_API void xray_source_value_free(XrSourceValue *value) {
    if(!value)return;xr_xir_value_drop(&value->value);xr_compile_resources_free(value);
}
XRAY_API XrSourceStatus xray_source_value_kind(const XrSourceValue *value,XrSourceValueKind *output) {
    if(!value || !output)return XR_SOURCE_BAD_ARGUMENT;
    if(!xr_xir_value_valid(&value->value))return XR_SOURCE_INVALID;
    XrSourceValueKind kind=XR_SOURCE_VALUE_UNIT;XrXirType type=(XrXirType)value->value.type;
    for(int scalar=XR_SOURCE_VALUE_UNIT;scalar<=XR_SOURCE_VALUE_RUNE;++scalar) {
        XrXirType expected=XR_XIR_UNIT;
        if(api_scalar_type((XrSourceValueKind)scalar,&expected) && type==expected){*output=(XrSourceValueKind)scalar;return XR_SOURCE_OK;}
    }
    if(type==XR_XIR_STRING)kind=XR_SOURCE_VALUE_STRING;
    else if(type==XR_XIR_ERROR)kind=XR_SOURCE_VALUE_ERROR;
    else if(type==XR_XIR_PANIC_INFO)kind=XR_SOURCE_VALUE_PANIC_INFO;
    else {
        const XrXirTypes *types=xr_xir_compile_type_arena_types(xr_xir_value_arena(&value->value));
        const XrXirTypeNode *node=xr_xir_type_node(types,type);
        if(!node)return XR_SOURCE_UNSUPPORTED;
        switch(node->kind) {
        case XR_XIR_TYPE_ARRAY:kind=XR_SOURCE_VALUE_ARRAY;break;
        case XR_XIR_TYPE_TUPLE:kind=XR_SOURCE_VALUE_TUPLE;break;
        case XR_XIR_TYPE_NULLABLE:kind=XR_SOURCE_VALUE_NULLABLE;break;
        case XR_XIR_TYPE_ATOMIC:kind=XR_SOURCE_VALUE_ATOMIC;break;
        case XR_XIR_TYPE_TASK:kind=XR_SOURCE_VALUE_TASK;break;
        case XR_XIR_TYPE_CALLABLE:kind=XR_SOURCE_VALUE_CALLABLE;break;
        case XR_XIR_TYPE_NOMINAL: {
            uint32_t nominal=types->nominals->identities[node->nominal.declaration].kind;
            kind=nominal==XR_XIR_NOMINAL_CLASS ? XR_SOURCE_VALUE_CLASS :
                nominal==XR_XIR_NOMINAL_ENUM ? XR_SOURCE_VALUE_ENUM : XR_SOURCE_VALUE_STRUCT;break;
        }
        default:return XR_SOURCE_UNSUPPORTED;
        }
    }
    *output=kind;return XR_SOURCE_OK;
}
XRAY_API XrSourceStatus xray_source_value_scalar(const XrSourceValue *value,XrSourceScalar *output) {
    if(!value || !output)return XR_SOURCE_BAD_ARGUMENT;
    XrSourceValueKind kind;XrSourceStatus status=xray_source_value_kind(value,&kind);
    if(status!=XR_SOURCE_OK)return status;
    XrXirType type;if(!api_scalar_type(kind,&type))return XR_SOURCE_INVALID;
    XrSourceScalar result={.kind=kind};
    if(kind>=XR_SOURCE_VALUE_I8 && kind<=XR_SOURCE_VALUE_I64)result.value.signed_value=value->value.payload;
    else if(kind>=XR_SOURCE_VALUE_U8 && kind<=XR_SOURCE_VALUE_U64)
        memcpy(&result.value.unsigned_value,&value->value.payload,sizeof(result.value.unsigned_value));
    else if(type==XR_XIR_BOOL)result.value.boolean=(int)value->value.payload;
    else if(type==XR_XIR_RUNE)result.value.rune=(uint32_t)value->value.payload;
    else if(type==XR_XIR_F32) {
        uint32_t bits=(uint32_t)value->value.payload;memcpy(&result.value.f32,&bits,sizeof(bits));
    } else if(type==XR_XIR_F64)memcpy(&result.value.f64,&value->value.payload,sizeof(result.value.f64));
    *output=result;return XR_SOURCE_OK;
}
XRAY_API XrSourceStatus xray_source_value_bytes(const XrSourceValue *value,const char **bytes,size_t *length) {
    if(!value || !bytes || !length)return XR_SOURCE_BAD_ARGUMENT;
    const char *view=NULL;size_t size=0;
    if(!xr_xir_string_view(&value->value,&view,&size))return XR_SOURCE_INVALID;
    *bytes=view;*length=size;return XR_SOURCE_OK;
}
XRAY_API XrSourceStatus xray_source_value_format(const XrSourceValue *value,XrSourceWrite write,void *context) {
    if(!value)return XR_SOURCE_BAD_ARGUMENT;
    if(!xr_xir_value_valid(&value->value))return XR_SOURCE_INVALID;
    const XrXirValue *source=&value->value;XrXirValue borrowed={0};
    if(source->type==XR_XIR_ERROR) {
        if(!xr_xir_error_borrow(source,&borrowed))return XR_SOURCE_INVALID;source=&borrowed;
    }
    ApiFormat output={write?write:api_stdio,context,XR_SOURCE_STDOUT,XR_SOURCE_WRITE_OK};
    XrXirFormatContext format={0};
    if(xr_value_format_value(xr_xir_value_format_reader(&format),(XrValueFormatNode){source,0},
        (XrValueFormatSink){&output,api_format_write},0))return XR_SOURCE_OK;
    return output.status==XR_SOURCE_WRITE_OOM ? XR_SOURCE_OUT_OF_MEMORY :
        output.status==XR_SOURCE_WRITE_LIMIT ? XR_SOURCE_LIMIT :
        output.status==XR_SOURCE_WRITE_IO ? XR_SOURCE_IO : XR_SOURCE_UNSUPPORTED;
}
