/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_builtins.h - Built-in type method definitions for LSP
 *
 * KEY CONCEPT:
 *   Provides method/property metadata for built-in types like Array, String,
 *   Map, Set, etc. for auto-completion and hover information.
 */

#ifndef XLSP_BUILTINS_H
#define XLSP_BUILTINS_H

#include "xlsp_json.h"
#include "../../runtime/value/xtype_names.h"

// LSP uses unified XrTypeId directly
typedef XrTypeId XlspBuiltinType;

#define XLSP_TYPE_UNKNOWN       XR_TID_NULL
#define XLSP_TYPE_INT           XR_TID_INT
#define XLSP_TYPE_FLOAT         XR_TID_FLOAT
#define XLSP_TYPE_STRING        XR_TID_STRING
#define XLSP_TYPE_BOOL          XR_TID_BOOL
#define XLSP_TYPE_ARRAY         XR_TID_ARRAY
#define XLSP_TYPE_MAP           XR_TID_MAP
#define XLSP_TYPE_SET           XR_TID_SET
#define XLSP_TYPE_JSON          XR_TID_JSON
#define XLSP_TYPE_CHANNEL       XR_TID_CHANNEL
#define XLSP_TYPE_REGEX         XR_TID_REGEX
#define XLSP_TYPE_BIGINT        XR_TID_BIGINT
#define XLSP_TYPE_STRINGBUILDER XR_TID_STRINGBUILDER
#define XLSP_TYPE_EXCEPTION     XR_TID_EXCEPTION
#define XLSP_TYPE_COROUTINE     XR_TID_COROUTINE

// LSP completion item kinds (LSP protocol values)
#define XLSP_KIND_METHOD   2
#define XLSP_KIND_PROPERTY 10

// Resolve type name string (e.g., "Array", "string") to XlspBuiltinType
XR_FUNC XlspBuiltinType xlsp_builtin_type_from_name(const char *type_name);

// Get completions for a type
XR_FUNC XrJsonValue *xlsp_builtin_get_completions(XlspBuiltinType type);

// Get hover info for a type method
XR_FUNC const char *xlsp_builtin_get_hover(XlspBuiltinType type, const char *method_name, 
                                    char *buf, size_t buf_size);

// Infer type from literal or constructor
XR_FUNC XlspBuiltinType xlsp_infer_literal_type(const char *text);

#endif // XLSP_BUILTINS_H
