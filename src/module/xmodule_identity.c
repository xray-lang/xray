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

#include "../base/xfileio.h"
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

static const char *find_char(const char *text, size_t length, char needle) {
    for (size_t i = 0; i < length; i++) {
        if (text[i] == needle)
            return text + i;
    }
    return NULL;
}

static bool namespace_value_valid(XrModuleIdentityKind kind, const char *value, size_t length) {
    if (kind == XR_MODULE_IDENTITY_SCRIPT)
        return length == 0;
    if (!value || length == 0)
        return false;
    if (kind == XR_MODULE_IDENTITY_PROJECT || kind == XR_MODULE_IDENTITY_STDLIB ||
        kind == XR_MODULE_IDENTITY_MEMORY)
        return namespace_segment_valid(value, length, false);
    if (kind != XR_MODULE_IDENTITY_PACKAGE)
        return false;
    const char *slash = find_char(value, length, '/');
    size_t slash_offset = slash ? (size_t) (slash - value) : 0;
    const char *at = slash ? find_char(slash + 1, length - slash_offset - 1, '@') : NULL;
    size_t at_offset = at ? (size_t) (at - value) : 0;
    return slash && at && !find_char(slash + 1, (size_t) (at - slash - 1), '/') &&
           !find_char(at + 1, length - at_offset - 1, '@') &&
           namespace_segment_valid(value, slash_offset, false) &&
           namespace_segment_valid(slash + 1, (size_t) (at - slash - 1), false) &&
           namespace_segment_valid(at + 1, length - at_offset - 1, true);
}

XR_FUNC bool xr_module_identity_authority_valid(const XrModuleIdentityAuthority *authority) {
    const char *value = authority ? authority->namespace_id : NULL;
    if (!authority)
        return false;
    size_t length = value ? strlen(value) : 0;
    if (!namespace_value_valid(authority->kind, value, length))
        return false;
    if (authority->kind == XR_MODULE_IDENTITY_MEMORY)
        return !authority->physical_root || !authority->physical_root[0];
    if (authority->kind == XR_MODULE_IDENTITY_STDLIB && authority->physical_root &&
        authority->physical_root[0])
        return physical_path_is_absolute(authority->physical_root);
    return true;
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

static bool logical_path_range_valid(const char *logical, size_t total_length) {
    if (!logical || total_length == 0 || logical[0] == '/')
        return false;
    size_t segment_start = 0;
    for (size_t i = 0; i <= total_length; i++) {
        if (i < total_length && logical[i] == '\\')
            return false;
        if (i < total_length && logical[i] != '/')
            continue;
        size_t length = i - segment_start;
        if (length == 0 || (length == 1 && logical[segment_start] == '.') ||
            (length == 2 && logical[segment_start] == '.' &&
             logical[segment_start + 1] == '.'))
            return false;
        segment_start = i + 1;
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

static bool build_framed_identity(const char *prefix, const char *first, const char *middle,
                                  const char *second, char **identity_out) {
    size_t first_length = strlen(first);
    size_t second_length = middle ? strlen(second) : 0;
    size_t identity_length = 0;
    bool size_valid = checked_add(&identity_length, strlen(prefix)) &&
                      checked_add(&identity_length, decimal_digits(first_length)) &&
                      checked_add(&identity_length, 1) &&
                      checked_add(&identity_length, first_length) &&
                      (!middle ||
                       (checked_add(&identity_length, strlen(middle)) &&
                        checked_add(&identity_length, decimal_digits(second_length)) &&
                        checked_add(&identity_length, 1) &&
                        checked_add(&identity_length, second_length))) &&
                      identity_length <= INT_MAX && identity_length < SIZE_MAX;
    char *identity = size_valid ? xr_malloc(identity_length + 1) : NULL;
    if (!identity)
        return false;
    int written = middle ? snprintf(identity, identity_length + 1, "%s%zu:%s%s%zu:%s", prefix,
                                    first_length, first, middle, second_length, second)
                         : snprintf(identity, identity_length + 1, "%s%zu:%s", prefix,
                                    first_length, first);
    if (written < 0 || (size_t) written != identity_length) {
        xr_free(identity);
        return false;
    }
    *identity_out = identity;
    return true;
}

XR_FUNC bool xr_module_identity_from_logical(const XrModuleIdentityAuthority *authority,
                                             const char *logical_path, char **identity_out) {
    if (identity_out)
        *identity_out = NULL;
    if (!authority || !identity_out || !xr_module_identity_authority_valid(authority))
        return false;

    const char *namespace_id = authority->namespace_id ? authority->namespace_id : "";
    if (authority->kind == XR_MODULE_IDENTITY_MEMORY) {
        if (logical_path && logical_path[0])
            return false;
        return build_framed_identity("memory-module-v1:id=", namespace_id, NULL, "",
                                     identity_out);
    }
    if (!logical_path_valid(logical_path))
        return false;
    if (authority->kind == XR_MODULE_IDENTITY_STDLIB)
        return build_framed_identity("stdlib-module-v1:module=", namespace_id, ":path=",
                                     logical_path, identity_out);

    const char *kind_name = identity_kind_name(authority->kind);
    if (!kind_name || authority->kind == XR_MODULE_IDENTITY_MEMORY)
        return false;
    size_t kind_length = strlen(kind_name);
    size_t namespace_length = strlen(namespace_id);
    size_t relative_length = strlen(logical_path);
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
    if (!identity)
        return false;
    int written = snprintf(identity, identity_length + 1,
                           "module-id-v1:kind=%zu:%s:namespace=%zu:%s:path=%zu:%s", kind_length,
                           kind_name, namespace_length, namespace_id, relative_length,
                           logical_path);
    if (written < 0 || (size_t) written != identity_length) {
        xr_free(identity);
        return false;
    }
    *identity_out = identity;
    return true;
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
    if (authority->kind == XR_MODULE_IDENTITY_MEMORY)
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

    char *logical = xr_strdup(relative);
    char *identity = NULL;
    if (!logical || !xr_module_identity_from_logical(authority, relative, &identity)) {
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

XR_FUNC bool xr_module_identity_script_authority_from_source(
    const char *source_path, XrModuleIdentityAuthority *authority, char **root_out) {
    if (authority)
        *authority = (XrModuleIdentityAuthority) {0};
    if (root_out)
        *root_out = NULL;
    if (!source_path || !authority || !root_out)
        return false;
    char *source = xr_realpath(source_path);
    char *root = source ? xr_path_dirname(source) : NULL;
    xr_free(source);
    if (!root || !physical_path_is_absolute(root)) {
        xr_free(root);
        return false;
    }
    *authority = (XrModuleIdentityAuthority) {
        .kind = XR_MODULE_IDENTITY_SCRIPT,
        .physical_root = root,
    };
    *root_out = root;
    return true;
}

static bool parse_framed_component(const char **cursor, const char *prefix, const char **value_out,
                                   size_t *length_out) {
    size_t prefix_length = strlen(prefix);
    const char *text = cursor ? *cursor : NULL;
    if (!text || strncmp(text, prefix, prefix_length) != 0)
        return false;
    text += prefix_length;
    if (!isdigit((unsigned char) text[0]))
        return false;
    if (text[0] == '0' && isdigit((unsigned char) text[1]))
        return false;
    size_t length = 0;
    while (isdigit((unsigned char) *text)) {
        unsigned digit = (unsigned) (*text - '0');
        if (length > (SIZE_MAX - digit) / 10)
            return false;
        length = length * 10 + digit;
        text++;
    }
    if (*text++ != ':' || strlen(text) < length)
        return false;
    *value_out = text;
    *length_out = length;
    *cursor = text + length;
    return true;
}

XR_FUNC bool xr_module_identity_valid(const char *identity, XrModuleIdentityKind *kind_out) {
    if (kind_out)
        *kind_out = 0;
    if (!identity || !identity[0])
        return false;

    const char *cursor = identity;
    const char *first = NULL;
    const char *second = NULL;
    const char *third = NULL;
    size_t first_length = 0;
    size_t second_length = 0;
    size_t third_length = 0;
    XrModuleIdentityKind kind = 0;
    if (strncmp(identity, "memory-module-v1:", sizeof("memory-module-v1:") - 1) == 0) {
        if (!parse_framed_component(&cursor, "memory-module-v1:id=", &first, &first_length) ||
            cursor[0] != '\0' ||
            !namespace_value_valid(XR_MODULE_IDENTITY_MEMORY, first, first_length))
            return false;
        kind = XR_MODULE_IDENTITY_MEMORY;
    } else if (strncmp(identity, "stdlib-module-v1:", sizeof("stdlib-module-v1:") - 1) == 0) {
        if (!parse_framed_component(&cursor, "stdlib-module-v1:module=", &first,
                                    &first_length) ||
            !parse_framed_component(&cursor, ":path=", &second, &second_length) ||
            cursor[0] != '\0' ||
            !namespace_value_valid(XR_MODULE_IDENTITY_STDLIB, first, first_length) ||
            !logical_path_range_valid(second, second_length))
            return false;
        kind = XR_MODULE_IDENTITY_STDLIB;
    } else {
        if (!parse_framed_component(&cursor, "module-id-v1:kind=", &first, &first_length) ||
            !parse_framed_component(&cursor, ":namespace=", &second, &second_length) ||
            !parse_framed_component(&cursor, ":path=", &third, &third_length) ||
            cursor[0] != '\0')
            return false;
        if (first_length == strlen("project") && memcmp(first, "project", first_length) == 0)
            kind = XR_MODULE_IDENTITY_PROJECT;
        else if (first_length == strlen("script") && memcmp(first, "script", first_length) == 0)
            kind = XR_MODULE_IDENTITY_SCRIPT;
        else if (first_length == strlen("package") && memcmp(first, "package", first_length) == 0)
            kind = XR_MODULE_IDENTITY_PACKAGE;
        else
            return false;
        if (!namespace_value_valid(kind, second, second_length) ||
            !logical_path_range_valid(third, third_length))
            return false;
    }
    if (kind_out)
        *kind_out = kind;
    return true;
}
