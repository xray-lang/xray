/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdap_hit.c - Strict finite hit-count predicates on the original work ledger
 */
#include "xdap_hit.h"
static bool hit_space(char c) { return c==' ' || c=='\t'; }
XR_FUNC XrXirStatus xdap_hit_parse(XrCompileResources *resources,const char *bytes,size_t length,XrXirVmDebugRule *output) {
    if (!resources || (!bytes && length) || !output || length>64) return XR_XIR_BAD_STRUCTURE;
    if (xr_compile_resources_work(resources,length+1)!=XR_COMPILE_RESOURCE_OK) return XR_XIR_BUDGET;
    XrXirVmDebugRule rule=*output;rule.hit_kind=XR_XIR_VM_DEBUG_HIT_ALWAYS;rule.hit_value=0;
    size_t at=0;while (at<length && hit_space(bytes[at])) ++at;
    if (at==length) { *output=rule;return XR_XIR_OK; }
    rule.hit_kind=XR_XIR_VM_DEBUG_HIT_GREATER_EQUAL;
    char op=bytes[at];
    if (op=='>' || op=='<' || op=='=' || op=='%') {
        ++at;bool equal=at<length && bytes[at]=='=';
        if (equal) ++at;
        if (op=='%' && equal) return XR_XIR_BAD_STRUCTURE;
        rule.hit_kind=op=='>' ? (equal ? XR_XIR_VM_DEBUG_HIT_GREATER_EQUAL : XR_XIR_VM_DEBUG_HIT_GREATER) :
            op=='<' ? (equal ? XR_XIR_VM_DEBUG_HIT_LESS_EQUAL : XR_XIR_VM_DEBUG_HIT_LESS) :
            op=='=' ? XR_XIR_VM_DEBUG_HIT_EQUAL : XR_XIR_VM_DEBUG_HIT_MULTIPLE;
    }
    while (at<length && hit_space(bytes[at])) ++at;
    size_t digits=at;
    while (at<length && bytes[at]>='0' && bytes[at]<='9') {
        uint64_t digit=(uint32_t)(bytes[at++]-'0');
        if (rule.hit_value>(UINT64_MAX-digit)/10) return XR_XIR_BAD_STRUCTURE;
        rule.hit_value=rule.hit_value*10+digit;
    }
    if (at==digits) return XR_XIR_BAD_STRUCTURE;
    while (at<length && hit_space(bytes[at])) ++at;
    if (at!=length || (rule.hit_kind==XR_XIR_VM_DEBUG_HIT_MULTIPLE && !rule.hit_value)) return XR_XIR_BAD_STRUCTURE;
    *output=rule;return XR_XIR_OK;
}
