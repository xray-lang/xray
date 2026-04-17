/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xclass_info.h - Class metadata descriptor (runtime class layer)
 *
 * KEY CONCEPT:
 *   XrClassInfo is the canonical class metadata structure. It is populated
 *   by the analyzer during compilation and read by runtime/type, runtime/class
 *   and VM/JIT during execution.
 *
 * WHY THIS LAYER:
 *   Previously defined in frontend/analyzer and caused runtime/value -> frontend
 *   upward dependencies. Class metadata belongs to the class layer; analyzer
 *   is merely one of its producers.
 *
 * LAYERING:
 *   Only primitive types and opaque forward declarations are used here so the
 *   header can be included by any runtime/ module without pulling analyzer in.
 */

#ifndef XCLASS_INFO_H
#define XCLASS_INFO_H

#include "../../base/xdefs.h"
#include "../../base/xlocation.h"
#include "../value/xtype.h"  // XrType* (runtime/value is lower than runtime/class)

#include <stdbool.h>
#include <stdint.h>

// Forward declarations (analyzer-owned; only ever used as pointers here)
typedef struct XaSymbol XaSymbol;
typedef struct XaScope XaScope;
struct XrHashMap;
struct XrStructLayout;

// Virtual method slot for JIT devirtualization.
// Populated by the analyzer (Pass 1.5); consumed by JIT.
typedef struct XaMethodSlot {
    const char *name;           // Method name
    XaSymbol *symbol;           // Method symbol (analyzer-owned)
    bool is_overridden;         // Overridden by a subclass
    bool is_final;              // No subclass overrides (safe for direct call)
    int vtable_index;           // Slot index in vtable (-1 if not virtual)
} XaMethodSlot;

// Class metadata. Fields are filled during analysis and remain stable at runtime.
typedef struct XrClassInfo XrClassInfo;
struct XrClassInfo {
    const char *name;
    const char *base_name;      // Base class name (for deferred linking)
    XrClassInfo *base;          // Base class (NULL if none, linked after collect)

    XaScope *scope;             // Class body scope (analyzer-owned)

    // Members (computed from scope)
    XaSymbol **fields;
    int field_count;
    XaSymbol **methods;
    int method_count;
    XaSymbol **static_fields;
    int static_field_count;
    XaSymbol **static_methods;
    int static_method_count;

    // O(1) member lookup (name -> XaSymbol*, own members only; inherited via base chain)
    struct XrHashMap *members_map;

    // Constructor info (set during class collection)
    bool has_constructor;               // true if class explicitly defines a constructor
    int constructor_required_params;    // number of params without default values
    XrType **constructor_params;
    int constructor_param_count;

    // Virtual method table (built in Pass 1.5 for JIT devirtualization)
    XaMethodSlot *vtable;       // Virtual method slots (NULL if not built)
    int vtable_size;            // Number of vtable entries
    bool has_subclass;          // true if any class extends this one

    // Struct layout (VALUE_TYPE only, computed by analyzer)
    struct XrStructLayout *struct_layout;  // NULL for class, set for struct

    // Implemented interfaces (populated from 'implements' clause)
    const char **interface_names;
    int interface_count;

    XrLocation location;
};

#endif // XCLASS_INFO_H
