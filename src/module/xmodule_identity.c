/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmodule_identity.c - Relocatable source-module identity authority
 */

#include "xmodule_identity.h"

#include "../base/xmalloc.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static bool physical_path_is_absolute(const char *path) {
    if (!path || !path[0])
        return false;
    if (path[0] == '/' && path[1] == '/')
        return path[2] != '\0' && path[2] != '?' && path[2] != '.';
    if (path[0] == '/')
        return true;
    if (path[0] == '\\' && path[1] == '\\')
        return path[2] != '\0' && path[2] != '?' && path[2] != '.';
    return isalpha((unsigned char) path[0]) && path[1] == ':' &&
           (path[2] == '/' || path[2] == '\\');
}

static bool namespace_segment_valid(const char *segment, size_t length, bool version) {
    if (!segment || length == 0 || (length == 1 && segment[0] == '.') ||
        (length == 2 && segment[0] == '.' && segment[1] == '.'))
        return false;
    for (size_t i = 0; i < length; i++) {
        unsigned char ch = (unsigned char) segment[i];
        if (!isalnum(ch) && ch != '_' && ch != '-' && ch != '.' && !(version && ch == '+'))
            return false;
    }
    return true;
}

XR_FUNC bool xr_module_identity_authority_valid(const XrModuleIdentityAuthority *authority) {
    const char *value = authority ? authority->namespace_id : NULL;
    if (!authority)
        return false;
    if (authority->kind == XR_MODULE_IDENTITY_SCRIPT)
        return !value || !value[0];
    if (!value || !value[0])
        return false;
    if (authority->kind == XR_MODULE_IDENTITY_PROJECT)
        return namespace_segment_valid(value, strlen(value), false);
    if (authority->kind != XR_MODULE_IDENTITY_PACKAGE)
        return false;
    const char *slash = strchr(value, '/');
    const char *at = slash ? strchr(slash + 1, '@') : NULL;
    return slash && at && !strchr(slash + 1, '/') && !strchr(at + 1, '@') &&
           namespace_segment_valid(value, (size_t) (slash - value), false) &&
           namespace_segment_valid(slash + 1, (size_t) (at - slash - 1), false) &&
           namespace_segment_valid(at + 1, strlen(at + 1), true);
}

static size_t decimal_digits(size_t value) {
    size_t digits = 1;
    while (value >= 10) {
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

static char *normalize_path(const char *path) {
    if (!path || !path[0])
        return NULL;
    size_t length = strlen(path);
    char *normalized = xr_malloc(length + 1);
    if (!normalized)
        return NULL;
    for (size_t i = 0; i < length; i++)
        normalized[i] = path[i] == '\\' ? '/' : path[i];
    normalized[length] = '\0';
    while (length > 1 && normalized[length - 1] == '/')
        normalized[--length] = '\0';
    return normalized;
}

static bool path_prefix_equal(const char *source, const char *root, size_t length) {
    for (size_t i = 0; i < length; i++) {
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

static bool logical_path_valid(const char *logical) {
    if (!logical || !logical[0] || logical[0] == '/' || strchr(logical, '\\'))
        return false;
    const char *segment = logical;
    while (*segment) {
        const char *end = strchr(segment, '/');
        size_t length = end ? (size_t) (end - segment) : strlen(segment);
        if (length == 0 || (length == 1 && segment[0] == '.') ||
            (length == 2 && segment[0] == '.' && segment[1] == '.'))
            return false;
        if (!end)
            break;
        segment = end + 1;
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
        default:
            return NULL;
    }
}

XR_FUNC bool xr_module_identity_from_source(const XrModuleIdentityAuthority *authority,
                                            const char *source_path, char **identity_out,
                                            char **logical_path_out) {
    if (identity_out)
        *identity_out = NULL;
    if (logical_path_out)
        *logical_path_out = NULL;
    if (!authority || !source_path || !identity_out || !logical_path_out ||
        !physical_path_is_absolute(authority->physical_root) ||
        !physical_path_is_absolute(source_path) || !xr_module_identity_authority_valid(authority))
        return false;
    const char *kind_name = identity_kind_name(authority->kind);
    if (!kind_name)
        return false;

    char *root = normalize_path(authority->physical_root);
    char *source = normalize_path(source_path);
    if (!root || !source) {
        xr_free(root);
        xr_free(source);
        return false;
    }
    size_t root_length = strlen(root);
    bool contained = path_prefix_equal(source, root, root_length) &&
                     (source[root_length] == '/' || source[root_length] == '\0');
    const char *relative = contained ? source + root_length : NULL;
    if (relative && relative[0] == '/')
        relative++;
    if (!contained || !logical_path_valid(relative)) {
        xr_free(root);
        xr_free(source);
        return false;
    }

    size_t kind_length = strlen(kind_name);
    size_t namespace_length = authority->namespace_id ? strlen(authority->namespace_id) : 0;
    size_t relative_length = strlen(relative);
    size_t identity_length = 0;
    bool size_valid = checked_add(&identity_length, sizeof("module-id-v1:kind=") - 1) &&
                      checked_add(&identity_length, decimal_digits(kind_length)) &&
                      checked_add(&identity_length, 1) &&
                      checked_add(&identity_length, kind_length) &&
                      checked_add(&identity_length, sizeof(":namespace=") - 1) &&
                      checked_add(&identity_length, decimal_digits(namespace_length)) &&
                      checked_add(&identity_length, 1) &&
                      checked_add(&identity_length, namespace_length) &&
                      checked_add(&identity_length, sizeof(":path=") - 1) &&
                      checked_add(&identity_length, decimal_digits(relative_length)) &&
                      checked_add(&identity_length, 1) &&
                      checked_add(&identity_length, relative_length) &&
                      identity_length <= INT_MAX && identity_length < SIZE_MAX;
    char *identity = size_valid ? xr_malloc(identity_length + 1) : NULL;
    char *logical = xr_strdup(relative);
    if (!identity || !logical) {
        xr_free(identity);
        xr_free(logical);
        xr_free(root);
        xr_free(source);
        return false;
    }
    int written = snprintf(identity, identity_length + 1,
                           "module-id-v1:kind=%zu:%s:namespace=%zu:%s:path=%zu:%s", kind_length,
                           kind_name, namespace_length,
                           authority->namespace_id ? authority->namespace_id : "",
                           relative_length, relative);
    if (written < 0 || (size_t) written != identity_length) {
        xr_free(identity);
        xr_free(logical);
        xr_free(root);
        xr_free(source);
        return false;
    }
    *identity_out = identity;
    *logical_path_out = logical;
    xr_free(root);
    xr_free(source);
    return true;
}
