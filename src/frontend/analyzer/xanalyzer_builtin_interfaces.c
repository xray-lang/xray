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
 *   - Bytes     : iterates over bytes
 *
 * Usage:
 *   fn process<T: Iterable>(collection: T) {
 *       for (item in collection) { ... }
 *   }
 */
static XaInterfaceMethod iterable_methods[] = {
    {"iterator", NULL, NULL, 0}  // return type set during init
};

/*
 * Equivalent xray code for Iterator<T>:
 *
 *   interface Iterator<T> {
 *       // Returns the next element, or null if exhausted
 *       next(): T?
 *
 *       // Returns true if there are more elements
 *       hasNext(): bool
 *   }
 *
 * Note: This is the iteration protocol used internally by for...in loops.
 * Users typically don't implement Iterator directly, but implement Iterable
 * and return a built-in iterator type.
 */
static XaInterfaceMethod iterator_methods[] = {
    {"next", NULL, NULL, 0},    // return type: T? (nullable element type)
    {"hasNext", NULL, NULL, 0}  // return type: bool
};

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
 *           let dx = this.x - other.x
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
 *       // Returns a hash code for this object.
 *       // Contract:
 *       //   - Equal objects must have equal hash codes
 *       //   - Hash code should be stable during object lifetime
 *       hashCode(): int
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
 *   let map: Map<K: Hashable, V> = {}
 *   let set: Set<T: Hashable> = #[]
 *
 * User implementation example:
 *   class Point implements Hashable {
 *       x: int
 *       y: int
 *
 *       hashCode(): int {
 *           // Combine x and y into a single hash
 *           return this.x * 31 + this.y
 *       }
 *   }
 *
 * Note: Objects used as Map keys should also implement equals() for
 * correct behavior, but xray uses === by default for reference equality.
 */
static XaInterfaceMethod hashable_methods[] = {
    {"hashCode", NULL, NULL, 0}  // return: int
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
 *       set(index: K, value: V): void
 *   }
 *
 * Built-in types that implement Indexable:
 *   - Array<T>   : index by int, returns T        (get/set)
 *   - string     : index by int, returns string   (get only, immutable)
 *   - Map<K,V>   : index by K, returns V          (get/set)
 *   - Array<uint8>: index by int, returns int      (get/set)
 *
 * Usage:
 *   let arr = [1, 2, 3]
 *   let x = arr[0]     // calls get(0)
 *   arr[1] = 10        // calls set(1, 10)
 *
 *   let str = "hello"
 *   let c = str[0]     // "h"
 *
 *   let map = {"a" => 1}
 *   let v = map["a"]   // calls get("a")
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
    {"set", NULL, NULL, 2}   // params: index, value, return: void
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
 *       // Returns the number of elements/characters
 *       length: int  // property, not method
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
 *       return container.length == 0
 *   }
 *
 *   fn last<T: Lengthable & Indexable>(arr: T) {
 *       return arr[arr.length - 1]
 *   }
 *
 * Note: Lengthable is a property-based interface, not method-based.
 * The compiler checks for .length property access.
 */
static XaInterfaceMethod lengthable_methods[] = {
    {"length", NULL, NULL, 0}  // property getter, return: int
};

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
 *       fn close(): void
 *   }
 *
 * Built-in types that implement Closeable:
 *   - File             : file handles
 *   - Channel          : communication channels
 *   - Connection       : network connections
 *   - (User-defined)   : any class with close() method
 *
 * Usage:
 *   fn withResource<T: Closeable>(resource: T, action: fn(T): void) {
 *       defer resource.close()
 *       action(resource)
 *   }
 *
 *   let file = fs.open("data.txt")
 *   defer file.close()  // automatic cleanup
 */
static XaInterfaceMethod closeable_methods[] = {
    {"close", NULL, NULL, 0}  // return: void
};

// ============================================================================
// Interface Registry
// ============================================================================

static XaInterfaceDefinition builtin_interfaces[XA_IFACE_COUNT] = {
    [XA_IFACE_ITERABLE] = {"Iterable", iterable_methods, 1, NULL},
    [XA_IFACE_ITERATOR] = {"Iterator", iterator_methods, 2, NULL},
    [XA_IFACE_COMPARABLE] = {"Comparable", comparable_methods, 1, NULL},
    [XA_IFACE_HASHABLE] = {"Hashable", hashable_methods, 1, NULL},
    [XA_IFACE_STRINGABLE] = {"Stringable", stringable_methods, 1, NULL},
    [XA_IFACE_INDEXABLE] = {"Indexable", indexable_methods, 2, NULL},
    [XA_IFACE_EQUATABLE] = {"Equatable", equatable_methods, 1, NULL},
    [XA_IFACE_LENGTHABLE] = {"Lengthable", lengthable_methods, 1, NULL},
    [XA_IFACE_CALLABLE] = {"Callable", callable_methods, 1, NULL},
    [XA_IFACE_CLOSEABLE] = {"Closeable", closeable_methods, 1, NULL},
};

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
 * |    -     |     -     | Json             |    -     |     -      |    -     |     ✓      |     ✓
 * |     ✓     |     ✓      |    -     |     -     | Array<T>         |    ✓     |     -      |    -
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

static bool is_map_type(XrType *type) {
    return type && (type->kind == XR_KIND_MAP);
}

static bool is_set_type(XrType *type) {
    return type && (type->kind == XR_KIND_SET);
}

static bool is_bigint_type(XrType *type) {
    return type && (xr_type_is_named_class(type, "BigInt"));
}

static bool is_json_type(XrType *type) {
    return type && type->kind == XR_KIND_JSON;
}

// ============================================================================
// Public API Implementation
// ============================================================================

static bool g_interfaces_initialized = false;

static void xa_builtin_interfaces_init(XrayIsolate *X) {
    if (g_interfaces_initialized)
        return;

    // Create interface types
    for (int i = 0; i < XA_IFACE_COUNT; i++) {
        XaInterfaceDefinition *def = &builtin_interfaces[i];
        def->type = xr_type_new_interface(X, def->name);

        // Set up method signatures based on interface
        // (Simplified: actual method types would need proper setup)
    }

    // Set specific return types
    // Comparable.compareTo returns int
    comparable_methods[0].return_type = xr_type_new_int(NULL);

    // Hashable.hashCode returns int
    hashable_methods[0].return_type = xr_type_new_int(NULL);

    // Stringable.toString returns string
    stringable_methods[0].return_type = xr_type_new_string(NULL);

    // Iterator.hasNext returns bool
    iterator_methods[1].return_type = xr_type_new_bool(NULL);

    g_interfaces_initialized = true;
}

void xa_builtin_interfaces_cleanup(void) {
    // Types are managed by the type pool, no explicit cleanup needed
    g_interfaces_initialized = false;
}

XrType *xa_get_builtin_interface(XaBuiltinInterface iface) {
    XR_DCHECK(iface >= 0, "xa_get_builtin_interface: invalid iface");
    if (iface < 0 || iface >= XA_IFACE_COUNT)
        return NULL;
    return builtin_interfaces[iface].type;
}

XrType *xa_get_builtin_interface_by_name(const char *name) {
    if (!name)
        return NULL;
    for (int i = 0; i < XA_IFACE_COUNT; i++) {
        if (strcmp(builtin_interfaces[i].name, name) == 0) {
            return builtin_interfaces[i].type;
        }
    }
    return NULL;
}

bool xa_builtin_type_implements(XrType *type, XaBuiltinInterface iface) {
    if (!type)
        return false;

    switch (iface) {
        case XA_IFACE_ITERABLE:
            // Iterable: Array, Map, Set, string
            return is_array_type(type) || is_map_type(type) || is_set_type(type) ||
                   is_string_type(type);

        case XA_IFACE_ITERATOR:
            // Iterator: internal use only, no built-in types directly implement
            return false;

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
            // Indexable: Array, string, Map, Json
            return is_array_type(type) || is_string_type(type) || is_map_type(type) ||
                   is_json_type(type);

        case XA_IFACE_EQUATABLE:
            // Equatable: all types support == and !=
            return true;

        case XA_IFACE_LENGTHABLE:
            // Lengthable: Array, string, Map, Set, Json
            return is_array_type(type) || is_string_type(type) || is_map_type(type) ||
                   is_set_type(type) || is_json_type(type);

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

void xa_register_builtin_interfaces(XrayIsolate *X, XaScope *global_scope) {
    if (!global_scope)
        return;
    // Interface XrType*s live in the isolate's type pool, so the global static
    // cache must be rebuilt for every fresh isolate (e.g. each test file gets
    // its own isolate via xr_cli_isolate_create()). Without this reset, the
    // cached pointers would dangle after the first isolate's pool is freed.
    g_interfaces_initialized = false;
    xa_builtin_interfaces_init(X);

    // Register each interface as a type alias in global scope
    for (int i = 0; i < XA_IFACE_COUNT; i++) {
        XaInterfaceDefinition *def = &builtin_interfaces[i];
        if (def->name && def->type) {
            xa_scope_define_type_alias(global_scope, def->name, def->type);
        }
    }
}
