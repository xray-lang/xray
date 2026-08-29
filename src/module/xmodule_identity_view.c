/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmodule_identity_view.c - Dependency-free frozen module identity views
 */

#include "xmodule_identity.h"

#include <ctype.h>
#include <stdint.h>
#include <string.h>

static bool identity_view_namespace_valid(const char *value, size_t length) {
    if (!value || length == 0 || (length == 1 && value[0] == '.') ||
        (length == 2 && value[0] == '.' && value[1] == '.'))
        return false;
    for (size_t i = 0; i < length; i++) {
        unsigned char ch = (unsigned char) value[i];
        if (!isalnum(ch) && ch != '_' && ch != '-' && ch != '.')
            return false;
    }
    return true;
}

static bool identity_view_logical_path_valid(const char *path, size_t length) {
    if (!path || length == 0 || path[0] == '/')
        return false;
    size_t segment_begin = 0;
    for (size_t i = 0; i <= length; i++) {
        if (i < length && path[i] == '\\')
            return false;
        if (i < length && path[i] != '/')
            continue;
        size_t segment_length = i - segment_begin;
        if (segment_length == 0 || (segment_length == 1 && path[segment_begin] == '.') ||
            (segment_length == 2 && path[segment_begin] == '.' && path[segment_begin + 1] == '.'))
            return false;
        segment_begin = i + 1;
    }
    return true;
}

static bool identity_view_framed_component(const char **cursor, const char *prefix,
                                           const char **value_out, size_t *length_out) {
    const char *text = cursor ? *cursor : NULL;
    size_t prefix_length = prefix ? strlen(prefix) : 0;
    if (!text || !prefix || !value_out || !length_out || strncmp(text, prefix, prefix_length) != 0)
        return false;
    text += prefix_length;
    if (!isdigit((unsigned char) text[0]) || (text[0] == '0' && isdigit((unsigned char) text[1])))
        return false;
    size_t length = 0;
    while (isdigit((unsigned char) *text)) {
        unsigned digit = (unsigned) (*text - '0');
        if (length > (SIZE_MAX - digit) / 10u)
            return false;
        length = length * 10u + digit;
        text++;
    }
    if (*text++ != ':' || strlen(text) < length)
        return false;
    *value_out = text;
    *length_out = length;
    *cursor = text + length;
    return true;
}

bool xr_module_identity_stdlib_namespace(const char *identity, const char **namespace_out,
                                         size_t *namespace_length_out) {
    if (namespace_out)
        *namespace_out = NULL;
    if (namespace_length_out)
        *namespace_length_out = 0;
    if (!identity || !namespace_out || !namespace_length_out)
        return false;
    const char *cursor = identity;
    const char *module = NULL;
    const char *path = NULL;
    size_t module_length = 0;
    size_t path_length = 0;
    if (!identity_view_framed_component(&cursor, "stdlib-module-v1:module=", &module,
                                        &module_length) ||
        !identity_view_framed_component(&cursor, ":path=", &path, &path_length) ||
        cursor[0] != '\0' || !identity_view_namespace_valid(module, module_length) ||
        !identity_view_logical_path_valid(path, path_length))
        return false;
    *namespace_out = module;
    *namespace_length_out = module_length;
    return true;
}
