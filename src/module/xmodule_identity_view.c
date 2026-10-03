/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmodule_identity_view.c - Shared identity grammar and explicitly metered traversal
 */
#include "xmodule_identity_internal.h"
#include <ctype.h>
#include <stdint.h>

XR_FUNC bool xr_module_identity_work(XrModuleIdentityWork *work, uint64_t count) {
    if (work->status != XR_MODULE_OK) return false;
    if (!work->charge(work->context,count)) { work->status = XR_MODULE_BUDGET; return false; }
    return true;
}
XR_FUNC bool xr_module_identity_length(XrModuleIdentityWork *work, const char *text, size_t *output) {
    if (!text) return false;
    size_t length = 0;
    for (;;) {
        if (!xr_module_identity_work(work,1)) return false;
        if (!text[length]) { *output = length; return true; }
        if (length == SIZE_MAX-1) { work->status = XR_MODULE_BUDGET; return false; }
        ++length;
    }
}
XR_FUNC bool xr_module_identity_absolute(XrModuleIdentityWork *work, const char *path) {
    if (!xr_module_identity_work(work,1) || !path || !path[0]) return false;
    if ((path[0] == '/' && path[1] == '/') || (path[0] == '\\' && path[1] == '\\'))
        return path[2] && path[2] != '?' && path[2] != '.';
    if (path[0] == '/') return true;
    return isalpha((unsigned char)path[0]) && path[1] == ':' && (path[2] == '/' || path[2] == '\\');
}
static bool namespace_segment(XrModuleIdentityWork *work, const char *text, size_t length, bool version) {
    if (!xr_module_identity_work(work,1) || !text || !length ||
        (length == 1 && text[0] == '.') || (length == 2 && text[0] == '.' && text[1] == '.')) return false;
    for (size_t i = 0; i < length; ++i) {
        if (!xr_module_identity_work(work,1)) return false;
        unsigned char ch = (unsigned char)text[i];
        if (!isalnum(ch) && ch != '_' && ch != '-' && ch != '.' && !(version && ch == '+')) return false;
    }
    return true;
}
static const char *find_char(XrModuleIdentityWork *work, const char *text, size_t length, char needle) {
    for (size_t i = 0; i < length; ++i) {
        if (!xr_module_identity_work(work,1)) return NULL;
        if (text[i] == needle) return text+i;
    }
    return NULL;
}
static bool namespace_value(XrModuleIdentityWork *work, XrModuleIdentityKind kind,
    const char *value, size_t length) {
    if (!xr_module_identity_work(work,1)) return false;
    if (kind == XR_MODULE_IDENTITY_SCRIPT) return length == 0;
    if (!value || !length) return false;
    if (kind == XR_MODULE_IDENTITY_PROJECT || kind == XR_MODULE_IDENTITY_STDLIB || kind == XR_MODULE_IDENTITY_MEMORY)
        return namespace_segment(work,value,length,false);
    if (kind != XR_MODULE_IDENTITY_PACKAGE) return false;
    const char *slash = find_char(work,value,length,'/');
    size_t slash_offset = slash ? (size_t)(slash-value) : 0;
    const char *at = slash ? find_char(work,slash+1,length-slash_offset-1,'@') : NULL;
    size_t at_offset = at ? (size_t)(at-value) : 0;
    return slash && at && !find_char(work,slash+1,(size_t)(at-slash-1),'/') &&
        !find_char(work,at+1,length-at_offset-1,'@') &&
        namespace_segment(work,value,slash_offset,false) &&
        namespace_segment(work,slash+1,(size_t)(at-slash-1),false) &&
        namespace_segment(work,at+1,length-at_offset-1,true);
}
XR_FUNC bool xr_module_identity_authority_walk(XrModuleIdentityWork *work, const XrModuleIdentityAuthority *authority) {
    if (!xr_module_identity_work(work,1) || !authority) return false;
    const char *value = authority->namespace_id;
    size_t length = 0;
    if (value && !xr_module_identity_length(work,value,&length)) return false;
    if (!namespace_value(work,authority->kind,value,length)) return false;
    if (authority->kind == XR_MODULE_IDENTITY_MEMORY) return !authority->physical_root || !authority->physical_root[0];
    if (authority->kind == XR_MODULE_IDENTITY_STDLIB && authority->physical_root && authority->physical_root[0])
        return xr_module_identity_absolute(work,authority->physical_root);
    return true;
}
XR_FUNC bool xr_module_identity_logical_walk(XrModuleIdentityWork *work, const char *text, size_t length) {
    if (!xr_module_identity_work(work,1) || !text || !length || text[0] == '/') return false;
    size_t start = 0;
    for (size_t i = 0; i <= length; ++i) {
        if (!xr_module_identity_work(work,1)) return false;
        if (i < length && text[i] == '\\') return false;
        if (i < length && text[i] != '/') continue;
        size_t count = i-start;
        if (!count || (count == 1 && text[start] == '.') || (count == 2 && text[start] == '.' && text[start+1] == '.')) return false;
        if (i == length) return true;
        start = i+1;
    }
    return true;
}

static bool prefix_matches(XrModuleIdentityWork *work, const char *text, const char *prefix, size_t *matched) {
    size_t i = 0;
    for (;;) {
        if (!xr_module_identity_work(work,1)) return false;
        if (!prefix[i]) { if (matched) *matched = i; return true; }
        if (text[i] != prefix[i]) return false;
        ++i;
    }
}
static bool framed_component(XrModuleIdentityWork *work, const char **cursor, const char *prefix,
    const char **value, size_t *size) {
    size_t prefix_length;
    const char *text = *cursor;
    if (!prefix_matches(work,text,prefix,&prefix_length)) return false;
    text += prefix_length;
    if (!xr_module_identity_work(work,1) || !isdigit((unsigned char)text[0]) ||
        (text[0] == '0' && isdigit((unsigned char)text[1]))) return false;
    size_t length = 0;
    for (;;) {
        if (!xr_module_identity_work(work,1)) return false;
        if (!isdigit((unsigned char)*text)) break;
        unsigned digit = (unsigned)(*text-'0');
        if (length > (SIZE_MAX-digit)/10) return false;
        length = length*10+digit; ++text;
    }
    if (*text++ != ':') return false;
    for (size_t i = 0; i < length; ++i) {
        if (!xr_module_identity_work(work,1) || !text[i]) return false;
    }
    *value = text; *size = length; *cursor = text+length; return true;
}
typedef struct IdentityView {
    XrModuleIdentityKind kind;
    const char *namespace_id;
    size_t namespace_length;
} IdentityView;
static bool exact_text(XrModuleIdentityWork *work, const char *text, size_t length, const char *expected) {
    size_t count;
    return xr_module_identity_length(work,expected,&count) && count == length && prefix_matches(work,text,expected,NULL);
}
static bool identity_walk(XrModuleIdentityWork *work, const char *identity, IdentityView *view) {
    if (!identity || !xr_module_identity_work(work,1) || !identity[0]) return false;
    const char *cursor = identity, *first = NULL, *second = NULL, *third = NULL;
    size_t a = 0, b = 0, c = 0;
    XrModuleIdentityKind kind;
    if (prefix_matches(work,identity,"memory-module-v1:",NULL)) {
        if (!framed_component(work,&cursor,"memory-module-v1:id=",&first,&a) || *cursor ||
            !namespace_value(work,XR_MODULE_IDENTITY_MEMORY,first,a)) return false;
        kind = XR_MODULE_IDENTITY_MEMORY; second = first; b = a;
    } else if (prefix_matches(work,identity,"stdlib-module-v1:",NULL)) {
        if (!framed_component(work,&cursor,"stdlib-module-v1:module=",&first,&a) ||
            !framed_component(work,&cursor,":path=",&second,&b) || *cursor ||
            !namespace_value(work,XR_MODULE_IDENTITY_STDLIB,first,a) ||
            !xr_module_identity_logical_walk(work,second,b)) return false;
        kind = XR_MODULE_IDENTITY_STDLIB; second = first; b = a;
    } else {
        if (!framed_component(work,&cursor,"module-id-v1:kind=",&first,&a) ||
            !framed_component(work,&cursor,":namespace=",&second,&b) ||
            !framed_component(work,&cursor,":path=",&third,&c) || *cursor) return false;
        if (exact_text(work,first,a,"project")) kind = XR_MODULE_IDENTITY_PROJECT;
        else if (exact_text(work,first,a,"script")) kind = XR_MODULE_IDENTITY_SCRIPT;
        else if (exact_text(work,first,a,"package")) kind = XR_MODULE_IDENTITY_PACKAGE;
        else return false;
        if (!namespace_value(work,kind,second,b) || !xr_module_identity_logical_walk(work,third,c)) return false;
    }
    if (work->status != XR_MODULE_OK) return false;
    *view = (IdentityView){kind,second,b}; return true;
}
static bool unbounded_query(void *context, uint64_t count) { (void)context; (void)count; return true; }
XR_FUNC bool xr_module_identity_authority_valid(const XrModuleIdentityAuthority *authority) {
    XrModuleIdentityWork work = {NULL,unbounded_query,XR_MODULE_OK};
    return xr_module_identity_authority_walk(&work,authority);
}
XR_FUNC bool xr_module_identity_valid(const char *identity, XrModuleIdentityKind *kind_out) {
    if (kind_out) *kind_out = 0;
    XrModuleIdentityWork work = {NULL,unbounded_query,XR_MODULE_OK}; IdentityView view;
    if (!identity_walk(&work,identity,&view)) return false;
    if (kind_out) *kind_out = view.kind;
    return true;
}
XR_FUNC bool xr_module_identity_stdlib_namespace(const char *identity, const char **namespace_out, size_t *length_out) {
    if (namespace_out) *namespace_out = NULL;
    if (length_out) *length_out = 0;
    if (!namespace_out || !length_out) return false;
    XrModuleIdentityWork work = {NULL,unbounded_query,XR_MODULE_OK}; IdentityView view;
    if (!identity_walk(&work,identity,&view) || view.kind != XR_MODULE_IDENTITY_STDLIB) return false;
    *namespace_out = view.namespace_id; *length_out = view.namespace_length; return true;
}
