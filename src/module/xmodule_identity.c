/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmodule_identity.c - Relocatable source-module identity authority
 */

#include "xmodule_identity_internal.h"
#include "../base/xio_policy.h"
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
static bool identity_charge(void *owner, uint64_t count) {
    return xr_compile_resources_work(owner,count) == XR_COMPILE_RESOURCE_OK;
}
static XrModuleStatus identity_failure(const XrModuleIdentityWork *work) {
    return work->status == XR_MODULE_OK ? XR_MODULE_INVALID : work->status;
}
static size_t identity_text_length(XrModuleIdentityWork *work, const char *text) {
    size_t length = 0;
    if (!xr_module_identity_length(work,text,&length) && work->status == XR_MODULE_OK) work->status = XR_MODULE_INVALID;
    return length;
}
static void *identity_alloc(XrCompileResources *owner, XrModuleIdentityWork *work, size_t size) {
    if (work->status != XR_MODULE_OK) return NULL;
    void *result = NULL;
    XrCompileResourceStatus status = xr_compile_resources_alloc(owner,size,&result);
    if (status != XR_COMPILE_RESOURCE_OK) work->status = status == XR_COMPILE_RESOURCE_OUT_OF_MEMORY ?
        XR_MODULE_OUT_OF_MEMORY : status == XR_COMPILE_RESOURCE_BUDGET ? XR_MODULE_BUDGET : XR_MODULE_INVALID;
    return result;
}
static char *identity_copy(XrCompileResources *owner, XrModuleIdentityWork *work, const char *text) {
    size_t length = identity_text_length(work,text);
    if (length == SIZE_MAX) { work->status = XR_MODULE_BUDGET; return NULL; }
    char *copy = identity_alloc(owner,work,length+1);
    if (!copy) return NULL;
    if (!xr_module_identity_work(work,length+1)) { xr_compile_resources_free(copy); return NULL; }
    memcpy(copy,text,length+1); return copy;
}
static size_t decimal_digits(XrModuleIdentityWork *work, size_t value) {
    size_t digits = 1;
    while (value >= 10) {
        if (!xr_module_identity_work(work,1)) return 0;
        value /= 10;
        digits++;
    }
    return digits;
}

static bool checked_add(size_t *total, size_t value) {
    if (!total || value > SIZE_MAX - *total)
        return false;
    *total += value;
    return true;
}

static char *normalize_path(XrCompileResources *resources, XrModuleIdentityWork *work, const char *path) {
    if (!path || !path[0])
        return NULL;
    size_t length = identity_text_length(work,path);
    if (length == SIZE_MAX) { work->status = XR_MODULE_BUDGET; return NULL; }
    char *normalized = identity_alloc(resources,work,length + 1);
    if (!normalized)
        return NULL;
    if (!xr_module_identity_work(work,length+1)) { xr_compile_resources_free(normalized); return NULL; }
    for (size_t i = 0; i < length; i++)
        normalized[i] = path[i] == '\\' ? '/' : path[i];
    normalized[length] = '\0';
    while (length > 1) {
        if (!xr_module_identity_work(work,1)) { xr_compile_resources_free(normalized); return NULL; }
        if (normalized[length - 1] != '/') break;
        normalized[--length] = '\0';
    }
    return normalized;
}

static bool path_prefix_equal(XrModuleIdentityWork *work, const char *source, const char *root, size_t length) {
    for (size_t i = 0; i < length; i++) {
        if (!xr_module_identity_work(work,1)) return false;
#ifdef XR_OS_WINDOWS
        if (tolower((unsigned char) source[i]) != tolower((unsigned char) root[i]))
            return false;
#else
        if (source[i] != root[i])
            return false;
#endif
    }
    return true;
}

static const char *identity_kind_name(XrModuleIdentityKind kind) {
    switch (kind) {
        case XR_MODULE_IDENTITY_PROJECT:
            return "project";
        case XR_MODULE_IDENTITY_SCRIPT:
            return "script";
        case XR_MODULE_IDENTITY_PACKAGE:
            return "package";
        case XR_MODULE_IDENTITY_STDLIB:
            return "stdlib";
        case XR_MODULE_IDENTITY_MEMORY:
            return "memory";
        default:
            return NULL;
    }
}

static XrModuleStatus build_framed_identity(XrCompileResources *resources, XrModuleIdentityWork *work, const char *prefix, const char *first, const char *middle,
                                  const char *second, char **identity_out) {
    size_t first_length = identity_text_length(work,first);
    size_t second_length = middle ? identity_text_length(work,second) : 0;
    size_t identity_length = 0;
    bool size_valid = checked_add(&identity_length, identity_text_length(work,prefix)) &&
                      checked_add(&identity_length, decimal_digits(work,first_length)) &&
                      checked_add(&identity_length, 1) &&
                      checked_add(&identity_length, first_length) &&
                      (!middle ||
                       (checked_add(&identity_length, identity_text_length(work,middle)) &&
                        checked_add(&identity_length, decimal_digits(work,second_length)) &&
                        checked_add(&identity_length, 1) &&
                        checked_add(&identity_length, second_length))) &&
                      identity_length <= INT_MAX && identity_length < SIZE_MAX;
    if (work->status != XR_MODULE_OK) return work->status;
    if (!size_valid) return XR_MODULE_BUDGET;
    char *identity = identity_alloc(resources,work,identity_length + 1);
    if (!identity) return work->status;
    if (!xr_module_identity_work(work,identity_length+1)) { xr_compile_resources_free(identity); return work->status; }
    int written = middle ? snprintf(identity, identity_length + 1, "%s%zu:%s%s%zu:%s", prefix,
                                    first_length, first, middle, second_length, second)
                         : snprintf(identity, identity_length + 1, "%s%zu:%s", prefix,
                                    first_length, first);
    if (written < 0 || (size_t) written != identity_length) {
        xr_compile_resources_free(identity);
        return XR_MODULE_INVALID;
    }
    *identity_out = identity;
    return XR_MODULE_OK;
}

XR_FUNC XrModuleStatus xr_compile_module_identity_from_logical(XrCompileResources *resources, const XrModuleIdentityAuthority *authority,
                                             const char *logical_path, char **identity_out) {
    if (!resources || !identity_out) return XR_MODULE_INVALID;
    XrModuleIdentityWork work = {resources,identity_charge,XR_MODULE_OK};
    if (!xr_module_identity_authority_walk(&work,authority)) return identity_failure(&work);

    const char *namespace_id = authority->namespace_id ? authority->namespace_id : "";
    if (authority->kind == XR_MODULE_IDENTITY_MEMORY) {
        if (logical_path && logical_path[0])
            return XR_MODULE_INVALID;
        return build_framed_identity(resources,&work,"memory-module-v1:id=", namespace_id, NULL, "",
                                     identity_out);
    }
    size_t relative_length = identity_text_length(&work,logical_path);
    if (!xr_module_identity_logical_walk(&work,logical_path,relative_length)) return identity_failure(&work);
    if (authority->kind == XR_MODULE_IDENTITY_STDLIB)
        return build_framed_identity(resources,&work,"stdlib-module-v1:module=", namespace_id, ":path=",
                                     logical_path, identity_out);

    const char *kind_name = identity_kind_name(authority->kind);
    if (!kind_name || authority->kind == XR_MODULE_IDENTITY_MEMORY)
        return XR_MODULE_INVALID;
    size_t kind_length = identity_text_length(&work,kind_name);
    size_t namespace_length = identity_text_length(&work,namespace_id);
    size_t identity_length = 0;
    bool size_valid = checked_add(&identity_length, sizeof("module-id-v1:kind=") - 1) &&
                      checked_add(&identity_length, decimal_digits(&work,kind_length)) &&
                      checked_add(&identity_length, 1) &&
                      checked_add(&identity_length, kind_length) &&
                      checked_add(&identity_length, sizeof(":namespace=") - 1) &&
                      checked_add(&identity_length, decimal_digits(&work,namespace_length)) &&
                      checked_add(&identity_length, 1) &&
                      checked_add(&identity_length, namespace_length) &&
                      checked_add(&identity_length, sizeof(":path=") - 1) &&
                      checked_add(&identity_length, decimal_digits(&work,relative_length)) &&
                      checked_add(&identity_length, 1) &&
                      checked_add(&identity_length, relative_length) &&
                      identity_length <= INT_MAX && identity_length < SIZE_MAX;
    if (work.status != XR_MODULE_OK) return work.status;
    if (!size_valid) return XR_MODULE_BUDGET;
    char *identity = identity_alloc(resources,&work,identity_length + 1);
    if (!identity) return work.status;
    if (!xr_module_identity_work(&work,identity_length+1)) { xr_compile_resources_free(identity); return work.status; }
    int written = snprintf(identity, identity_length + 1,
                           "module-id-v1:kind=%zu:%s:namespace=%zu:%s:path=%zu:%s", kind_length,
                           kind_name, namespace_length, namespace_id, relative_length,
                           logical_path);
    if (written < 0 || (size_t) written != identity_length) {
        xr_compile_resources_free(identity);
        return XR_MODULE_INVALID;
    }
    *identity_out = identity;
    return XR_MODULE_OK;
}


XR_FUNC XrModuleStatus xr_compile_module_identity_from_source(XrCompileResources *resources,
    const XrModuleIdentityAuthority *authority, const char *source_path, char **identity_out, char **logical_out) {
    if (!resources || !authority || !identity_out || !logical_out || identity_out == logical_out)
        return XR_MODULE_INVALID;
    XrModuleIdentityWork work = {resources,identity_charge,XR_MODULE_OK};
    if (!xr_module_identity_absolute(&work,authority->physical_root) ||
        !xr_module_identity_absolute(&work,source_path) || !xr_module_identity_authority_walk(&work,authority) ||
        authority->kind == XR_MODULE_IDENTITY_MEMORY) return identity_failure(&work);
    char *root = normalize_path(resources,&work,authority->physical_root);
    char *source = normalize_path(resources,&work,source_path);
    char *logical = NULL, *identity = NULL;
    if (!root || !source) goto failed;
    size_t root_length = identity_text_length(&work,root), source_length = identity_text_length(&work,source);
    bool contained = work.status == XR_MODULE_OK && source_length >= root_length &&
        path_prefix_equal(&work,source,root,root_length) &&
        (root[root_length-1] == '/' || source[root_length] == '/' || source[root_length] == '\0');
    const char *relative = contained ? source + root_length : NULL;
    if (relative && relative[0] == '/') ++relative;
    size_t length = relative ? identity_text_length(&work,relative) : 0;
    if (!contained || !xr_module_identity_logical_walk(&work,relative,length)) goto failed;
    logical = identity_copy(resources,&work,relative);
    if (!logical) goto failed;
    work.status = xr_compile_module_identity_from_logical(resources,authority,relative,&identity);
    if (work.status != XR_MODULE_OK) goto failed;
    *identity_out = identity; *logical_out = logical;
    xr_compile_resources_free(root); xr_compile_resources_free(source); return XR_MODULE_OK;
failed:
    xr_compile_resources_free(identity); xr_compile_resources_free(logical);
    xr_compile_resources_free(root); xr_compile_resources_free(source); return identity_failure(&work);
}
XR_FUNC XrModuleStatus xr_compile_module_identity_script_authority_from_source(XrCompileResources *resources,
    const char *source_path, XrModuleIdentityAuthority *authority, char **root_out) {
    if (!resources || !source_path || !authority || !root_out) return XR_MODULE_INVALID;
    XrOsIoPolicy policy = xr_compile_io_policy(resources);
    char *source = NULL, *root = NULL;
    XrModuleStatus status = xr_module_status_from_io(xr_realpath_owned(&policy,source_path,&source));
    if (status != XR_MODULE_OK) return status;
    status = xr_module_status_from_io(xr_path_dirname_owned(&policy,source,&root));
    xr_compile_resources_free(source);
    if (status != XR_MODULE_OK) return status;
    XrModuleIdentityWork work = {resources,identity_charge,XR_MODULE_OK};
    if (!xr_module_identity_absolute(&work,root)) {
        xr_compile_resources_free(root); return identity_failure(&work);
    }
    *authority = (XrModuleIdentityAuthority){XR_MODULE_IDENTITY_SCRIPT,NULL,root};
    *root_out = root; return XR_MODULE_OK;
}
