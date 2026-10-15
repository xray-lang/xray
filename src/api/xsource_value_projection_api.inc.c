/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xsource_value_projection_api.inc.c - Owned public projections of immutable data
 */
#include "../xir/xxir_tuple.h"
#include "../xir/xxir_nullable.h"
#include "../xir/xxir_panic.h"
static XrSourceStatus api_value_require(const XrSourceValue *value,XrSourceValueKind expected) {
    if(!value)return XR_SOURCE_BAD_ARGUMENT;
    XrSourceValueKind kind;XrSourceStatus status=xray_source_value_kind(value,&kind);
    return status==XR_SOURCE_OK && kind!=expected ? XR_SOURCE_INVALID : status;
}
static XrSourceStatus api_value_take(XrCompileResources *resources,XrXirValue *value,XrSourceValue **output) {
    XrSourceValue *owner=NULL;
    XrSourceStatus status=api_resource(xr_compile_resources_calloc(resources,1,sizeof(*owner),(void **)&owner));
    if(status!=XR_SOURCE_OK)return status;
    /* The kernel already returned ownership; publication must not repeat its
     * retention or the original Domain's copy-work charge. */
    owner->resources=resources;owner->value=*value;*value=(XrXirValue){0};*output=owner;return XR_SOURCE_OK;
}
XRAY_API XrSourceStatus xray_source_value_tuple_count(const XrSourceValue *value,size_t *output) {
    if(!value || !output)return XR_SOURCE_BAD_ARGUMENT;
    XrSourceStatus status=api_value_require(value,XR_SOURCE_VALUE_TUPLE);if(status!=XR_SOURCE_OK)return status;
    const XrXirTypeNode *node=xr_xir_tuple_signature(
        xr_xir_compile_type_arena_types(xr_xir_value_arena(&value->value)),(XrXirType)value->value.type);
    if(!node)return XR_SOURCE_INVALID;
    *output=node->parameter_count;return XR_SOURCE_OK;
}
XRAY_API XrSourceStatus xray_source_value_tuple_get(const XrSourceValue *value,size_t field,XrSourceValue **output) {
    if(!value || !output || *output)return XR_SOURCE_BAD_ARGUMENT;
    XrSourceStatus status=api_value_require(value,XR_SOURCE_VALUE_TUPLE);if(status!=XR_SOURCE_OK)return status;
    if(field>UINT32_MAX)return XR_SOURCE_LIMIT;
    XrXirValue projected={0};status=api_value_status(xr_xir_tuple_get(&value->value,(uint32_t)field,&projected));
    if(status==XR_SOURCE_OK)status=api_value_take(value->resources,&projected,output);
    xr_xir_value_drop(&projected);return status;
}
XRAY_API XrSourceStatus xray_source_value_nullable(const XrSourceValue *value,int *some,XrSourceValue **output) {
    if(!value || !some || !output || *output)return XR_SOURCE_BAD_ARGUMENT;
    XrSourceStatus status=api_value_require(value,XR_SOURCE_VALUE_NULLABLE);if(status!=XR_SOURCE_OK)return status;
    bool present=false;const XrXirValue *payload=NULL;
    if(!xr_xir_nullable_view(&value->value,&present,&payload))return XR_SOURCE_INVALID;
    if(present)status=api_value_wrap(value->resources,payload,output);
    if(status==XR_SOURCE_OK)*some=present?1:0;return status;
}
XRAY_API XrSourceStatus xray_source_value_panic_code(const XrSourceValue *value,int64_t *output) {
    if(!value || !output)return XR_SOURCE_BAD_ARGUMENT;
    XrSourceStatus status=api_value_require(value,XR_SOURCE_VALUE_PANIC_INFO);if(status!=XR_SOURCE_OK)return status;
    XrXirFaultDetail detail={0};if(!xr_xir_panic_info_detail(&value->value,&detail))return XR_SOURCE_INVALID;
    *output=detail.code;return XR_SOURCE_OK;
}
XRAY_API XrSourceStatus xray_source_value_panic_message(const XrSourceValue *value,XrSourceValue **output) {
    if(!value || !output || *output)return XR_SOURCE_BAD_ARGUMENT;
    XrSourceStatus status=api_value_require(value,XR_SOURCE_VALUE_PANIC_INFO);if(status!=XR_SOURCE_OK)return status;
    XrXirValue projected={0};status=api_value_status(xr_xir_panic_info_message(&value->value,&projected));
    if(status==XR_SOURCE_OK)status=api_value_take(value->resources,&projected,output);
    xr_xir_value_drop(&projected);return status;
}
