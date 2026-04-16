/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_builtins.h - Built-in type member definitions
 *
 * KEY CONCEPT:
 *   Provides static information about built-in types (Array, Map, String, etc.)
 *   for LSP completion and hover without runtime introspection.
 */

#ifndef XANALYZER_BUILTINS_H
#define XANALYZER_BUILTINS_H

#include "../../runtime/value/xtype.h"
#include "xanalyzer_symbol.h"
#include "../../runtime/value/xtype_names.h"
#include "../../base/xdefs.h"

// Built-in member info
typedef struct XaBuiltinMember {
    const char *name;
    const char *signature;      // e.g., "(index: int): T"
    const char *doc;            // Documentation
    bool is_method;             // true = method, false = property
    bool is_static;             // true = static member
} XaBuiltinMember;

// Built-in type info
typedef struct XaBuiltinType {
    const char *name;
    const XaBuiltinMember *members;
    int member_count;
} XaBuiltinType;

// Handle type field info (for C module handle types like Connection, Listener)
typedef struct XaBuiltinHandleField {
    const char *name;
    const char *type_str;       // e.g., "int", "string", "bool"
    bool is_const;
} XaBuiltinHandleField;

// Handle type info
typedef struct XaBuiltinHandle {
    const char *name;           // e.g., "Connection", "Listener"
    const XaBuiltinHandleField *fields;
    int field_count;
} XaBuiltinHandle;

// Built-in C module info (for net, ws, http, etc.)
typedef struct XaBuiltinModule {
    const char *name;           // Module name (e.g., "net")
    const XaBuiltinMember *functions;
    int function_count;
    const XaBuiltinHandle *handles;
    int handle_count;
} XaBuiltinModule;

// Convert XrType to unified XrTypeId (O(1) enum mapping)
XR_FUNC XrTypeId xr_type_to_builtin_id(XrType *type);

// Get built-in type info by XrType
XR_FUNC const XaBuiltinType *xa_builtin_get_type_info(XrType *type);

// Get built-in type info by name
XR_FUNC const XaBuiltinType *xa_builtin_get_by_name(const char *name);

// Create fake symbols for built-in members (for LSP)
XR_FUNC XaSymbol **xa_builtin_get_members(XrType *type, int *count);

// Get member signature for hover
XR_FUNC const char *xa_builtin_get_member_signature(XrType *type, const char *member_name);

// Get member documentation
XR_FUNC const char *xa_builtin_get_member_doc(XrType *type, const char *member_name);

// Get method return type (for type inference)
// Returns the return type of a built-in method, with generic substitution
// e.g., Array<int>.pop() returns int?, Array<int>.map(fn) returns Array<U>
XR_FUNC XrType *xa_builtin_get_method_return_type(XrType *container_type, const char *method_name);

// Check if member is a method
XR_FUNC bool xa_builtin_is_method(XrType *type, const char *member_name);

// ============================================================================
// Generic API (used by both compiler and LSP)
// ============================================================================

// Get member info (returns member count, fills out array)
XR_FUNC int xa_builtin_get_members_for_type(XrType *type, const XaBuiltinMember **out_members);

// Get type name for display
XR_FUNC const char *xa_builtin_get_type_name(XrType *type);

// ============================================================================
// Module type info API (for C modules like net, ws, http)
// ============================================================================

// Get module info by name
XR_FUNC const XaBuiltinModule *xa_builtin_get_module_info(const char *module_name);

// Get module function signature
XR_FUNC const char *xa_builtin_get_module_func_signature(const char *module_name, const char *func_name);

// Get module function doc
XR_FUNC const char *xa_builtin_get_module_func_doc(const char *module_name, const char *func_name);

// Get module handle type info
XR_FUNC const XaBuiltinHandle *xa_builtin_get_handle_type(const char *module_name, const char *handle_name);

// Set script directory for .xrd file search
XR_FUNC void xa_builtin_set_script_dir(const char *dir);

// Parse return type from signature string (e.g., "(x: int): string" -> string type)
XR_FUNC XrType *xa_builtin_parse_return_type_from_sig(const char *sig);

// Parse full function signature including parameter types
// e.g., "(data: string, level?: int): string?" -> fn(string, int): string?
XR_FUNC XrType *xa_builtin_parse_full_signature(const char *sig);

#endif // XANALYZER_BUILTINS_H
