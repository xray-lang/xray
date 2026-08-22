/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_builtin_interfaces.c - Built-in interface implementations
 *
 * KEY CONCEPT:
 *   This file defines all built-in interfaces that enable generic constraints.
 *   Each interface is documented with its equivalent xray code for clarity.
 *
 * COMPILE-TIME ONLY:
 *   These interfaces are used for compile-time type checking only.
 *   No runtime duck-typing - built-in types have implicit implementations,
 *   user types must explicitly declare 'implements'.
 */

#include "../../base/xchecks.h"
#include "../../os/os_thread.h"
#include <stdlib.h>
#include <string.h>
#include "xanalyzer_builtin_interfaces.h"
#include "xanalyzer_symbol.h"

// ============================================================================
// Interface Definitions
// ============================================================================

/*
 * Equivalent xray code for Iterable<T>:
 *
 *   interface Iterable<T> {
 *       // Returns an iterator over elements of type T
 *       iterator(): Iterator<T>
 *   }
 *
 * Built-in types that implement Iterable:
 *   - Array<T>  : iterates over elements
 *   - Set<T>    : iterates over elements
 *   - Map<K,V>  : iterates over keys
 *   - string    : iterates over characters
 *   - Array<u8>: iterates over bytes
 *
 * Usage:
 *   fn process<T: Iterable>(collection: T) {
 *       for (item in collection) { ... }
 *   }
 */
static XaInterfaceMethod iterable_methods[] = {
    {"iterator", NULL, NULL, 0}  // return type set during init
};

/* Iterator<T> is deliberately absent from this registry. It is a sealed native
 * class with a real runtime representation, declared in stdlib/types/iterator.xr
 * and mapped to XR_TID_ITERATOR, so `Iterator<int>` must resolve to that class.
 * Registering the name here as well shadowed the class with an argument-less
 * interface alias, which made every `Iterator<T>` annotation unusable. */

/*
 * Equivalent xray code for Comparable:
 *
 *   interface Comparable {
 *       // Compares this object with another for ordering.
 *       // Returns:
 *       //   negative int : this < other
 *       //   zero         : this == other
 *       //   positive int : this > other
 *       compareTo(other: T): int
 *   }
 *
 * Built-in types that implement Comparable:
 *   - int       : numeric comparison
 *   - float     : numeric comparison (NaN handling: NaN != NaN)
 *   - string    : lexicographic comparison (Unicode code point order)
 *
 * Usage:
 *   fn sort<T: Comparable>(arr: Array<T>): Array<T> {
 *       // Can use compareTo() on elements
 *       if (arr[i].compareTo(arr[j]) > 0) { swap(i, j) }
 *   }
 *
 *   fn max<T: Comparable>(a: T, b: T): T {
 *       return a.compareTo(b) > 0 ? a : b
 *   }
 *
 * User implementation example:
 *   class Point implements Comparable {
 *       x: int
 *       y: int
 *
 *       compareTo(other: Point): int {
 *           // Compare by x first, then by y
 *           var dx = this.x - other.x
 *           if (dx != 0) return dx
 *           return this.y - other.y
 *       }
 *   }
 */
static XaInterfaceMethod comparable_methods[] = {
    {"compareTo", NULL, NULL, 1}  // param: other: T, return: int
};

/*
 * Equivalent xray code for Hashable:
 *
 *   interface Hashable {
 *       // Equal objects must have equal stable hashes.
 *       operator ==(other: Self) -> bool
 *       hash() -> int
 *   }
 *
 * Built-in types that implement Hashable:
 *   - int       : identity hash (value itself)
 *   - float     : IEEE 754 bit representation hash
 *   - string    : FNV-1a or similar string hash
 *   - bool      : 0 for false, 1 for true
 *
 * Usage:
 *   // Hashable is required for Map keys and Set elements
 *   var map: Map<K: Hashable, V> = {}
 *   var set: Set<T: Hashable> = #[]
 *
 * User implementation example:
 *   class Point implements Hashable {
 *       x: int
 *       y: int
 *
 *       operator ==(other: Point) -> bool {
 *           return this.x == other.x && this.y == other.y
 *       }
 *
 *       hash() -> int {
 *           return this.x * 31 + this.y
 *       }
 *   }
 */
static XaInterfaceMethod hashable_methods[] = {
    {"==", NULL, NULL, 1},   // param: other: Self, return: bool
    {"hash", NULL, NULL, 0}  // return: int
};

/*
 * Equivalent xray code for Stringable:
 *
 *   interface Stringable {
 *       // Returns a string representation of this object.
 *       toString(): string
 *   }
 *
 * Built-in types that implement Stringable:
 *   - int       : decimal representation (e.g., "42", "-17")
 *   - float     : decimal representation (e.g., "3.14", "1e10")
 *   - string    : identity (returns itself)
 *   - bool      : "true" or "false"
 *   - null      : "null"
 *   - Array     : "[elem1, elem2, ...]"
 *   - Map       : "{key1 => value1, ...}"
 *   - Set       : "#[elem1, elem2, ...]"
 *
 * Usage:
 *   fn log<T: Stringable>(value: T) {
 *       print(value.toString())
 *   }
 *
 * User implementation example:
 *   class Point implements Stringable {
 *       x: int
 *       y: int
 *
 *       toString(): string {
 *           return `Point(${this.x}, ${this.y})`
 *       }
 *   }
 *
 * Note: print() automatically calls toString() on non-string values,
 * so explicit toString() calls are rarely needed in practice.
 */
static XaInterfaceMethod stringable_methods[] = {
    {"toString", NULL, NULL, 0}  // return: string
};

/*
 * Equivalent xray code for Indexable<K, V>:
 *
 *   interface Indexable<K, V> {
 *       // Get element at index/key
 *       get(index: K): V
 *
 *       // Set element at index/key (optional, for mutable containers)
 *       set(index: K, value: V) -> ()
 *   }
 *
 * Built-in types that implement Indexable:
 *   - Array<T>   : index by int, returns T        (get/set)
 *   - Map<K,V>   : index by K, returns V          (get/set)
 *   - Array<uint8>: index by int, returns int      (get/set)
 *
 * Usage:
 *   var arr = [1, 2, 3]
 *   var x = arr[0]     // calls get(0)
 *   arr[1] = 10        // calls set(1, 10)
 *
 *   var map = {"a" => 1}
 *   var v = map["a"]   // calls get("a")
 *
 * User implementation example:
 *   class Matrix implements Indexable {
 *       data: Array<Array<int>>
 *
 *       get(index: int): Array<int> {
 *           return this.data[index]
 *       }
 *
 *       set(index: int, row: Array<int>) {
 *           this.data[index] = row
 *       }
 *   }
 */
static XaInterfaceMethod indexable_methods[] = {
    {"get", NULL, NULL, 1},  // param: index, return: element type
    {"set", NULL, NULL, 2}   // params: index, value, return: Unit
};

/*
 * Equivalent xray code for Equatable:
 *
 *   interface Equatable {
 *       // Check equality with another object
 *       // Returns true if this equals other
 *       equals(other: T): bool
 *   }
 *
 * Built-in types that implement Equatable:
 *   - All primitive types (int, float, string, bool, null)
 *   - Array    : element-wise equality
 *   - Map      : key-value pair equality
 *   - Set      : element equality
 *   - Objects  : reference equality by default
 *
 * Note: In xray, == uses value equality for primitives and reference
 * equality for objects by default. Implementing Equatable allows
 * custom equality logic.
 *
 * Usage:
 *   fn contains<T: Equatable>(arr: Array<T>, item: T): bool {
 *       for (elem in arr) {
 *           if (elem.equals(item)) return true
 *       }
 *       return false
 *   }
 *
 * User implementation example:
 *   class Point implements Equatable {
 *       x: int
 *       y: int
 *
 *       equals(other: Point): bool {
 *           return this.x == other.x && this.y == other.y
 *       }
 *   }
 */
static XaInterfaceMethod equatable_methods[] = {
    {"equals", NULL, NULL, 1}  // param: other, return: bool
};

/*
 * Equivalent xray code for Lengthable:
 *
 *   interface Lengthable {
 *       operator len() -> int
 *   }
 *
 * Built-in types that implement Lengthable:
 *   - Array<T>  : number of elements
 *   - string    : number of Unicode characters (not bytes)
 *   - Map<K,V>  : number of key-value pairs
 *   - Set<T>    : number of elements
 *   - Array<uint8>: number of elements
 *
 * Usage:
 *   fn isEmpty<T: Lengthable>(container: T): bool {
 *       return len(container) == 0
 *   }
 *
 *   fn last<T: Lengthable & Indexable>(arr: T) {
 *       return arr[len(arr) - 1]
 *   }
 *
 * The compiler keeps this named operator outside ordinary member lookup.
 */
static XaInterfaceMethod lengthable_methods[] = {{"__operator_len", NULL, NULL, 0}};

/*
 * Equivalent xray code for Callable:
 *
 *   interface Callable<R> {
 *       // Invoke the callable and return result
 *       // Note: Parameter types vary, this is a marker interface
 *       (): R
 *   }
 *
 * Built-in types that implement Callable:
 *   - Function        : regular functions
 *   - Class           : constructor call returns instance
 *   - Closure         : anonymous functions / lambdas
 *
 * Usage:
 *   // Callable is mainly used for type checking function parameters
 *   fn apply<T, R>(f: (T) => R, x: T): R {
 *       return f(x)
 *   }
 *
 * Note: In practice, function types like (int, int) => int are more
 * commonly used than the Callable interface directly. Callable serves
 * as the base concept for all invocable types.
 */
static XaInterfaceMethod callable_methods[] = {
    {"call", NULL, NULL, 0}  // variadic, return: varies
};

/*
 * ========== Closeable Interface ==========
 *
 * Closeable represents types that hold resources requiring cleanup.
 * Used with defer for automatic resource management.
 *
 * Equivalent xray interface definition:
 *   interface Closeable {
 *       fn close() -> ()
 *   }
 *
 * Built-in types that implement Closeable:
 *   - File             : file handles
 *   - Channel          : communication channels
 *   - Connection       : network connections
 *   - (User-defined)   : any class with close() method
 *
 * Usage:
 *   fn withResource<T: Closeable>(resource: T, action: (T) -> ()) {
 *       defer resource.close()
 *       action(resource)
 *   }
 *
 *   var file = fs.open("data.txt")
 *   defer file.close()  // automatic cleanup
 */
static XaInterfaceMethod closeable_methods[] = {
    {"close", NULL, NULL, 0}  // return: Unit
};

// ============================================================================
// Interface Registry
// ============================================================================

static XaInterfaceDefinition builtin_interfaces[XA_IFACE_COUNT] = {
    [XA_IFACE_ITERABLE] = {"Iterable", iterable_methods, 1},
    [XA_IFACE_COMPARABLE] = {"Comparable", comparable_methods, 1},
    [XA_IFACE_HASHABLE] = {"Hashable", hashable_methods, 2},
    [XA_IFACE_STRINGABLE] = {"Stringable", stringable_methods, 1},
    [XA_IFACE_INDEXABLE] = {"Indexable", indexable_methods, 2},
    [XA_IFACE_EQUATABLE] = {"Equatable", equatable_methods, 1},
    [XA_IFACE_LENGTHABLE] = {"Lengthable", lengthable_methods, 1},
    [XA_IFACE_CALLABLE] = {"Callable", callable_methods, 1},
    [XA_IFACE_CLOSEABLE] = {"Closeable", closeable_methods, 1},
    /* A marker, deliberately without methods. What an error value must support
     * is already covered elsewhere -- every enum is Stringable and Equatable,
     * and `throw` accepts nothing but an enum error value. Error exists to give
     * that set a name a signature can use, not to add a contract. */
    [XA_IFACE_ERROR] = {"Error", NULL, 0},
};

/* The table above binds each interface to its method signatures; the language
 * surface (which names exist, and how many type arguments each takes) is owned
 * by builtin_symbols.def, which the resolver, the LSP and the specification
 * generator all read. Neither list may grow a name the other lacks. */
static const char *const g_builtin_iface_surface[] = {
#define XR_BUILTIN_IFACE(name, arity) name,
#include "../../../stdlib/prelude/builtin_symbols.def"
};

#define XA_IFACE_SURFACE_COUNT                                                                     \
    (sizeof(g_builtin_iface_surface) / sizeof(g_builtin_iface_surface[0]))

static xr_once_t builtin_interface_methods_once = XR_ONCE_INITIALIZER;

static void init_builtin_interface_methods(void) {
    comparable_methods[0].return_type = xr_type_new_int(NULL);
    hashable_methods[0].return_type = xr_type_new_int(NULL);
    stringable_methods[0].return_type = xr_type_new_string(NULL);

    XR_STATIC_ASSERT(XA_IFACE_SURFACE_COUNT == (size_t) XA_IFACE_COUNT,
                     "builtin_symbols.def and builtin_interfaces[] disagree on interface count");
    for (size_t i = 0; i < XA_IFACE_SURFACE_COUNT; i++) {
        bool found = false;
        for (int j = 0; j < XA_IFACE_COUNT && !found; j++)
            found = builtin_interfaces[j].name &&
                    strcmp(builtin_interfaces[j].name, g_builtin_iface_surface[i]) == 0;
        XR_DCHECK(found, "builtin_symbols.def declares an interface with no method table");
    }
}

const XaInterfaceDefinition *xa_builtin_interface_definition(const char *name) {
    if (!name)
        return NULL;
    xr_once_call(&builtin_interface_methods_once, init_builtin_interface_methods);
    for (int i = 0; i < XA_IFACE_COUNT; i++) {
        if (builtin_interfaces[i].name && strcmp(builtin_interfaces[i].name, name) == 0)
            return &builtin_interfaces[i];
    }
    return NULL;
}

// ============================================================================
// Built-in Type Implementation Matrix
// ============================================================================

/*
 * This matrix defines which built-in types implement which interfaces.
 *
 * Legend:
 *   ✓ = implements
 *   - = does not implement
 *
 *                  | Iterable | Comparable | Hashable | Stringable | Indexable | Equatable |
 * Lengthable | Callable | Closeable |
 * -----------------|----------|------------|----------|------------|-----------|-----------|------------|----------|-----------|
 * int              |    -     |     ✓      |    ✓     |     ✓      |     -     |     ✓     |     -
 * |    -     |     -     | float            |    -     |     ✓      |    ✓     |     ✓      |     -
 * |     ✓     |     -      |    -     |     -     | string           |    ✓     |     ✓      |    ✓
 * |     ✓      |     ✓     |     ✓     |     ✓      |    -     |     -     | bool             | -
 * |     -      |    ✓     |     ✓      |     -     |     ✓     |     -      |    -     |     - |
 * BigInt           |    -     |     ✓      |    ✓     |     ✓      |     -     |     ✓     |     -
 * |    -     |     -     | Array<T>         |    ✓     |     -      |    -
 * |     ✓      |     ✓     |     ✓     |     ✓      |    -     |     -     | Map<K,V>         | ✓
 * |     -      |    -     |     ✓      |     ✓     |     ✓     |     ✓      |    -     |     - |
 * Set<T>           |    ✓     |     -      |    -     |     ✓      |     -     |     ✓     |     ✓
 * |    -     |     -     | Array<uint8>     |    ✓     |     -      |    -     |     ✓      |     ✓
 * |     ✓     |     ✓      |    -     |     -     | null             |    -     |     -      |    -
 * |     ✓      |     -     |     ✓     |     -      |    -     |     -     | Function         | -
 * |     -      |    -     |     ✓      |     -     |     -     |     -      |    ✓     |     - |
 * Class            |    -     |     -      |    -     |     ✓      |     -     |     -     |     -
 * |    ✓     |     -     | Channel          |    -     |     -      |    -     |     ✓      |     -
 * |     -     |     -      |    -     |     ✓     | File             |    -     |     -      |    -
 * |     ✓      |     -     |     -     |     -      |    -     |     ✓     |
 */

// Check if a type flag indicates a specific built-in type
static bool is_int_type(XrType *type) {
    return type && (type->kind == XR_KIND_INT);
}

static bool is_float_type(XrType *type) {
    return type && (type->kind == XR_KIND_FLOAT);
}

static bool is_string_type(XrType *type) {
    return type && (type->kind == XR_KIND_STRING);
}

static bool is_bool_type(XrType *type) {
    return type && (type->kind == XR_KIND_BOOL);
}

static bool is_array_type(XrType *type) {
    return type && (type->kind == XR_KIND_ARRAY);
}

static bool is_span_type(XrType *type) {
    return type && (type->kind == XR_KIND_SLICE);
}

static bool is_view_type(XrType *type) {
    return type && (type->kind == XR_KIND_SLICE);
}

static bool is_map_type(XrType *type) {
    return type && (type->kind == XR_KIND_MAP);
}

static bool is_set_type(XrType *type) {
    return type && (type->kind == XR_KIND_SET);
}

static bool is_bigint_type(XrType *type) {
    return type && (xr_type_is_builtin_named_class(type, "BigInt"));
}

// ============================================================================
// Public API Implementation
// ============================================================================

bool xa_is_builtin_interface_name(const char *name) {
    if (!name)
        return false;
    for (int i = 0; i < XA_IFACE_COUNT; i++) {
        if (strcmp(builtin_interfaces[i].name, name) == 0)
            return true;
    }
    return false;
}

bool xa_builtin_type_implements(XrType *type, XaBuiltinInterface iface) {
    if (!type)
        return false;

    switch (iface) {
        case XA_IFACE_ITERABLE:
            // Iterable: Array, View, Slice, Map, Set, string
            return is_array_type(type) || is_view_type(type) || is_span_type(type) ||
                   is_map_type(type) || is_set_type(type) || is_string_type(type);

        case XA_IFACE_COMPARABLE:
            // Comparable: int, float, string, BigInt
            return is_int_type(type) || is_float_type(type) || is_string_type(type) ||
                   is_bigint_type(type);

        case XA_IFACE_HASHABLE:
            // Hashable: int, float, string, bool, BigInt
            return is_int_type(type) || is_float_type(type) || is_string_type(type) ||
                   is_bool_type(type) || is_bigint_type(type);

        case XA_IFACE_STRINGABLE:
            // Stringable: all types (everything can be converted to string)
            return true;

        case XA_IFACE_INDEXABLE:
            // Indexable: Array, View, Slice, string, Map
            return is_array_type(type) || is_view_type(type) || is_span_type(type) ||
                   is_string_type(type) || is_map_type(type);

        case XA_IFACE_EQUATABLE:
            // Equatable: all types support == and !=
            return true;

        case XA_IFACE_LENGTHABLE:
            // Lengthable: Array, View, Slice, string, Map, Set
            return is_array_type(type) || is_view_type(type) || is_span_type(type) ||
                   is_string_type(type) || is_map_type(type) || is_set_type(type);

        case XA_IFACE_CALLABLE:
            // Callable: Function, Class
            return (type->kind == XR_KIND_FUNCTION) || (type->kind == XR_KIND_CLASS);

        case XA_IFACE_CLOSEABLE:
            // Channel has XR_KIND_CHANNEL flag, not XR_KIND_INSTANCE
            if (type->kind == XR_KIND_CHANNEL)
                return true;
            if (type->kind == XR_KIND_INSTANCE) {
                const char *name = type->instance.class_name;
                if (name && strcmp(name, "File") == 0)
                    return true;
            }
            return false;

        default:
            return false;
    }
}

void xa_register_builtin_interfaces(XrVMRuntime *X, XaScope *global_scope) {
    if (!global_scope)
        return;

    xr_once_call(&builtin_interface_methods_once, init_builtin_interface_methods);

    // Create per-isolate interface types and register as scope aliases.
    // Each isolate gets its own XrType* instances so no global mutable
    // state is shared between concurrent isolates.
    for (int i = 0; i < XA_IFACE_COUNT; i++) {
        XaInterfaceDefinition *def = &builtin_interfaces[i];
        if (def->name) {
            XrType *iface_type = xr_type_new_interface(X, def->name);
            xa_scope_define_type_alias(global_scope, def->name, iface_type);
        }
    }
}
