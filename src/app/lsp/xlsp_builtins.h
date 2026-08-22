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

#include "../../base/xjson.h"
#include "../../runtime/value/xtype.h"
#include "../../runtime/value/xtype_names.h"

// Runtime categories use XrTypeId directly. Json is a compiler-only semantic
// surface, so LSP gives it a private slot outside the public TypeId range.
typedef int XlspBuiltinType;

#define XLSP_TYPE_UNRESOLVED XR_TID_NULL
#define XLSP_TYPE_I64 XR_TID_I64
#define XLSP_TYPE_F64 XR_TID_F64
#define XLSP_TYPE_STRING XR_TID_STRING
#define XLSP_TYPE_BOOL XR_TID_BOOL
#define XLSP_TYPE_ARRAY XR_TID_ARRAY
#define XLSP_TYPE_MAP XR_TID_MAP
#define XLSP_TYPE_SET XR_TID_SET
#define XLSP_TYPE_JSON ((XlspBuiltinType) XR_TID_COUNT)
#define XLSP_TYPE_CHANNEL XR_TID_CHANNEL
#define XLSP_TYPE_REGEX XR_TID_REGEX
#define XLSP_TYPE_BIGINT XR_TID_BIGINT
#define XLSP_TYPE_STRINGBUILDER XR_TID_STRINGBUILDER
#define XLSP_TYPE_PANIC_INFO XR_TID_PANIC_INFO
#define XLSP_TYPE_COROUTINE XR_TID_COROUTINE

// LSP completion item kinds (LSP protocol values)
#define XLSP_KIND_METHOD 2
#define XLSP_KIND_PROPERTY 10

// Resolve type name string (e.g., "Array", "string") to XlspBuiltinType
XR_FUNC XlspBuiltinType xlsp_builtin_type_from_name(const char *type_name);

// Get completions for a type
XR_FUNC XrJsonValue *xlsp_builtin_get_completions(XlspBuiltinType type);

// Get completions for a concrete analyzer type. This preserves receiver-specialized
// method sets such as Array<u8> and Slice<u8>.
XR_FUNC XrJsonValue *xlsp_builtin_get_completions_for_type(XrType *type);

// Get hover info for a type method
XR_FUNC const char *xlsp_builtin_get_hover(XlspBuiltinType type, const char *method_name, char *buf,
                                           size_t buf_size);

// Get hover/signature info for a concrete analyzer type method.
XR_FUNC const char *xlsp_builtin_get_hover_for_type(XrType *type, const char *method_name,
                                                    char *buf, size_t buf_size);
XR_FUNC const char *xlsp_builtin_get_signature_for_type(XrType *type, const char *method_name,
                                                        char *buf, size_t buf_size);

// Infer type from literal or constructor
XR_FUNC XlspBuiltinType xlsp_infer_literal_type(const char *text);

#endif  // XLSP_BUILTINS_H
