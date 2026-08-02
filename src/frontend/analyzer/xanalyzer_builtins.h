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

typedef enum XaBuiltinReturnOwnership {
    XA_BUILTIN_RETURN_UNKNOWN = 0,
    XA_BUILTIN_RETURN_FRESH,
    /* The member hands back its own receiver at +0 (`return self`), so the
     * result and the receiver are ONE object. Declared by `// @returns_receiver`
     * on the native declaration; it is a manifest property of the runtime
     * binding, never inferred from the signature (a method returning the
     * receiver's TYPE — Array.concat, Array.filter — usually returns a fresh
     * object). ARC and the independent RC verifier each read this fact to keep
     * the receiver's reference alive through the aliased result (contract C1). */
    XA_BUILTIN_RETURN_RECEIVER,
    XA_BUILTIN_RETURN_BORROWED_STATIC,
    XA_BUILTIN_RETURN_BORROWED_PARAM_0,
    XA_BUILTIN_RETURN_BORROWED_PARAM_1,
    XA_BUILTIN_RETURN_BORROWED_PARAM_2,
    XA_BUILTIN_RETURN_BORROWED_PARAM_3,
    XA_BUILTIN_RETURN_BORROWED_PARAM_4,
    XA_BUILTIN_RETURN_BORROWED_PARAM_5,
    XA_BUILTIN_RETURN_BORROWED_PARAM_6,
    XA_BUILTIN_RETURN_BORROWED_PARAM_7,
    XA_BUILTIN_RETURN_BORROWED_PARAM_8,
    XA_BUILTIN_RETURN_BORROWED_PARAM_9,
    XA_BUILTIN_RETURN_BORROWED_PARAM_10,
    XA_BUILTIN_RETURN_BORROWED_PARAM_11,
    XA_BUILTIN_RETURN_BORROWED_PARAM_12,
    XA_BUILTIN_RETURN_BORROWED_PARAM_13,
    XA_BUILTIN_RETURN_BORROWED_PARAM_14,
    XA_BUILTIN_RETURN_BORROWED_PARAM_15,
} XaBuiltinReturnOwnership;

static inline int
xa_builtin_return_ownership_param_index(XaBuiltinReturnOwnership ownership) {
    if (ownership < XA_BUILTIN_RETURN_BORROWED_PARAM_0 ||
        ownership > XA_BUILTIN_RETURN_BORROWED_PARAM_15)
        return -1;
    return (int) ownership - (int) XA_BUILTIN_RETURN_BORROWED_PARAM_0;
}

// Built-in member info
typedef struct XaBuiltinMember {
    const char *name;
    const char *signature;             // e.g., "(index: int): T"
    const char *doc;                   // Documentation
    bool is_method;                    // true = method, false = property
    bool is_static;                    // true = static member
    bool is_internal;                  // true = visible only to stdlib implementation modules
    bool is_lowered_only;              // true = compiler/VM lowering surface, not an XrClass method
    bool is_yieldable;                 // true = VM binding may suspend/resume the current coroutine
    XaEffectContract effect_contract;  // bodyless error contract; zero means missing/incomplete
    XaAllocationContractKind allocation_contract;  // explicit bodyless allocation contract
    bool mutates_receiver;  // native declaration manifest proof; never inferred from spelling
    XaBuiltinReturnOwnership return_ownership;  // sealed caller ownership result contract
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
    const char *type_str;  // e.g., "int", "string", "bool"
    bool is_const;
} XaBuiltinHandleField;

// Handle type info
typedef struct XaBuiltinHandle {
    const char *name;  // e.g., "Connection", "Listener"
    const XaBuiltinHandleField *fields;
    int field_count;
    const XaBuiltinMember *methods;  // instance methods on this handle type
    int method_count;
} XaBuiltinHandle;

// Named sealed record declared by a native stdlib module.
typedef struct XaBuiltinRecordField {
    const char *name;
    const char *type_str;
} XaBuiltinRecordField;

typedef struct XaBuiltinRecord {
    const char *name;
    const char *doc;
    const XaBuiltinRecordField *fields;
    int field_count;
    bool is_sealed;
} XaBuiltinRecord;

// Module-scoped enum declared by a native stdlib module.
typedef struct XaBuiltinEnumVariant {
    const char *name;
    const char *const *payload_type_strs;
    int payload_count;
} XaBuiltinEnumVariant;

typedef struct XaBuiltinEnum {
    const char *name;
    const char *doc;
    const XaBuiltinEnumVariant *variants;
    int variant_count;
    uint32_t layout_id;
} XaBuiltinEnum;

// Built-in C module info (for net, ws, http, etc.)
typedef struct XaBuiltinModule {
    const char *name;  // Module name (e.g., "net")
    const XaBuiltinMember *functions;
    int function_count;
    const XaBuiltinHandle *handles;
    int handle_count;
    const XaBuiltinRecord *records;
    int record_count;
    const XaBuiltinEnum *enums;
    int enum_count;
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

// Get built-in type member error-effect contract. Static members are used for
// namespace-style calls such as string.fromUtf8(...); instance members are used
// for receiver calls such as s.sliceBytes(...).
XR_FUNC const XaEffectContract *
xa_builtin_get_type_member_effect_contract(XrType *type, const char *member_name, bool is_static);
XR_FUNC XaAllocationContractKind xa_builtin_get_type_member_allocation_contract(
    XrType *type, const char *member_name, bool is_static);

// Same as above, but starts from a built-in type namespace name.
XR_FUNC const XaEffectContract *
xa_builtin_get_named_type_member_effect_contract(const char *type_name, const char *member_name,
                                                 bool is_static);
XR_FUNC XaAllocationContractKind xa_builtin_get_named_type_member_allocation_contract(
    const char *type_name, const char *member_name, bool is_static);

// Get member documentation
XR_FUNC const char *xa_builtin_get_member_doc(XrType *type, const char *member_name);

// Get method return type (for type inference)
// Returns the return type of a built-in method, with generic substitution
// e.g., Array<int>.pop() returns int?, Array<int>.map(fn) returns Array<U>
XR_FUNC XrType *xa_builtin_get_method_return_type(XrVMRuntime *X, XrType *container_type,
                                                  const char *method_name);

// R2-2 stopgap: overflow-control methods on fixed-width int receivers.
// checked*/saturating*/*Overflows still compute at int64 width in both
// runtimes, so a fixed-width receiver would silently get int64 overflow
// boundaries; reject them at compile time until a width-carrying lowering
// lands. wrappingAdd/Sub/Mul ARE width-lowered (IR arith + receiver-width
// narrow) and stay allowed. Returns true when the call must be rejected and
// fills msg with the diagnostic text.
XR_FUNC bool xa_builtin_int_overflow_method_unsupported(XrType *receiver, const char *method_name,
                                                        char *msg, size_t msg_cap);

// Check if member is a method
XR_FUNC bool xa_builtin_is_method(XrType *type, const char *member_name);
XR_FUNC bool xa_builtin_member_mutates_receiver(XrType *type, const char *member_name);
// True when the member's runtime binding returns its own receiver (`return
// self`) rather than a fresh reference. See XA_BUILTIN_RETURN_RECEIVER.
XR_FUNC bool xa_builtin_member_returns_receiver(XrType *type, const char *member_name);

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

// Iterate all builtin module declarations known to the analyzer.
XR_FUNC int xa_builtin_get_module_count(void);
XR_FUNC const XaBuiltinModule *xa_builtin_get_module_at(int index);

// Get module function signature
XR_FUNC const char *xa_builtin_get_module_func_signature(const char *module_name,
                                                         const char *func_name);

// Get the ABI signature of a registered native primitive, including private
// primitives used only by an Xray stdlib implementation.
XR_FUNC const char *xa_builtin_get_module_func_abi_signature(const char *module_name,
                                                             const char *func_name);

// Get module function doc
XR_FUNC const char *xa_builtin_get_module_func_doc(const char *module_name, const char *func_name);

// Get module function error-effect contract.
XR_FUNC const XaEffectContract *xa_builtin_get_module_func_effect_contract(const char *module_name,
                                                                           const char *func_name);
// Get the ABI error-effect contract of a registered native primitive,
// including private primitives used only by an Xray stdlib implementation.
XR_FUNC const XaEffectContract *
xa_builtin_get_module_func_abi_effect_contract(const char *module_name, const char *func_name);
XR_FUNC XaAllocationContractKind
xa_builtin_get_module_func_allocation_contract(const char *module_name, const char *func_name);
XR_FUNC XaBuiltinReturnOwnership
xa_builtin_get_module_func_return_ownership(const char *module_name, const char *func_name);

// Check if a module function is registered as yieldable in stdlib metadata.
XR_FUNC bool xa_builtin_module_func_is_yieldable(const char *module_name, const char *func_name);

// Get module handle type info
XR_FUNC const XaBuiltinHandle *xa_builtin_get_handle_type(const char *module_name,
                                                          const char *handle_name);

// Get native module named record/enum declarations.
XR_FUNC const XaBuiltinRecord *xa_builtin_get_record_type(const char *module_name,
                                                          const char *record_name);
XR_FUNC const XaBuiltinRecord *xa_builtin_find_record_by_name(const char *record_name);
XR_FUNC const XaBuiltinEnum *xa_builtin_get_enum_type(const char *module_name,
                                                      const char *enum_name);
XR_FUNC const XaBuiltinEnum *xa_builtin_find_enum_by_name(const char *enum_name);

// Materialize analyzer types and enum metadata from native declarations.
XR_FUNC XrType *xa_builtin_record_decl_type(XrVMRuntime *X, const XaBuiltinRecord *record);
XR_FUNC XrType *xa_builtin_enum_decl_type(XrVMRuntime *X, const XaBuiltinEnum *enum_decl,
                                          XaEnumInfo **out_info);

// Find a handle type by name across all loaded modules (builtin + .xrd)
XR_FUNC const XaBuiltinHandle *xa_builtin_find_handle_by_name(const char *handle_name);

// Get a handle method error-effect contract by handle and method name.
XR_FUNC const XaEffectContract *
xa_builtin_get_handle_method_effect_contract(const char *handle_name, const char *method_name);
XR_FUNC XaAllocationContractKind
xa_builtin_get_handle_method_allocation_contract(const char *handle_name, const char *method_name);

// Owning builtin module name for a handle type, or NULL if no builtin module
// declares one with this name (used for user-class name-collision diagnostics)
XR_FUNC const char *xa_builtin_find_handle_module(const char *handle_name);

// Set script directory for .xrd file search
XR_FUNC void xa_builtin_set_script_dir(const char *dir);

// Parse a type string (e.g., "float", "int?", "Array<string>") into XrType
XR_FUNC XrType *xa_builtin_parse_type_string(XrVMRuntime *X, const char *s);

// Parse return type from signature string (e.g., "(x: int): string" -> string type)
XR_FUNC XrType *xa_builtin_parse_return_type_from_sig(XrVMRuntime *X, const char *sig);

// Parse full function signature including parameter types
// e.g., "(data: string, level?: int): string?" -> fn(string, int): string?
XR_FUNC XrType *xa_builtin_parse_full_signature(XrVMRuntime *X, const char *sig);

#endif  // XANALYZER_BUILTINS_H
