/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_builtin_interfaces.h - Built-in interface definitions for type constraints
 *
 * KEY CONCEPT:
 *   Defines core interfaces that built-in types implicitly implement.
 *   These interfaces enable compile-time generic constraint checking.
 *   User-defined classes must explicitly implement interfaces via 'implements'.
 *
 * WHY THIS DESIGN:
 *   - Compile-time only: no runtime duck-typing overhead
 *   - Built-in types have implicit implementations (compiler knows)
 *   - User types require explicit 'implements' declaration
 *   - Keeps type system simple and performant
 *
 * INTERFACES DEFINED:
 *   - Iterable<T>   : for...in loop support
 *   - Iterator<T>   : iteration protocol
 *   - Comparable    : ordering operations (<, >, <=, >=)
 *   - Hashable      : Map keys, Set elements
 *   - Stringable    : string conversion (toString)
 */

#ifndef XANALYZER_BUILTIN_INTERFACES_H
#define XANALYZER_BUILTIN_INTERFACES_H

#include "../../runtime/value/xtype.h"
#include "xanalyzer_symbol.h"
#include "../../base/xdefs.h"

// Built-in interface identifiers
typedef enum {
    XA_IFACE_ITERABLE,      // for...in support
    XA_IFACE_ITERATOR,      // iteration protocol
    XA_IFACE_COMPARABLE,    // <, >, <=, >= operators
    XA_IFACE_HASHABLE,      // Map key, Set element
    XA_IFACE_STRINGABLE,    // toString() support
    XA_IFACE_INDEXABLE,     // [] index access
    XA_IFACE_EQUATABLE,     // ==, != operators
    XA_IFACE_LENGTHABLE,    // .length property
    XA_IFACE_CALLABLE,      // () invocation
    XA_IFACE_CLOSEABLE,     // resource management (close)
    XA_IFACE_COUNT          // total count
} XaBuiltinInterface;

// Interface method signature
typedef struct {
    const char *name;           // Method name
    XrType *return_type;        // Return type (NULL = void)
    XrType **param_types;       // Parameter types (excluding 'this')
    int param_count;            // Number of parameters
} XaInterfaceMethod;

// Interface definition
typedef struct {
    const char *name;           // Interface name (e.g., "Comparable")
    XaInterfaceMethod *methods; // Required methods
    int method_count;           // Number of methods
    XrType *type;               // Cached interface type
} XaInterfaceDefinition;

// Initialize all built-in interfaces (call once at startup)
XR_FUNC void xa_builtin_interfaces_init(void);

// Cleanup built-in interfaces
XR_FUNC void xa_builtin_interfaces_cleanup(void);

// Get interface type by identifier
XR_FUNC XrType *xa_get_builtin_interface(XaBuiltinInterface iface);

// Get interface type by name (for constraint resolution)
XR_FUNC XrType *xa_get_builtin_interface_by_name(const char *name);

// Check if a built-in type implements an interface
XR_FUNC bool xa_builtin_type_implements(XrType *type, XaBuiltinInterface iface);

// Register built-in interfaces to a scope (for name resolution)
XR_FUNC void xa_register_builtin_interfaces(XaScope *global_scope);

#endif // XANALYZER_BUILTIN_INTERFACES_H
