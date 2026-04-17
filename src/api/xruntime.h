/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xruntime.h - Unified runtime API for all backends
 *
 * KEY CONCEPT:
 *   This header defines the public runtime API shared by all execution backends.
 *   It provides memory allocation, object creation, type checking, and GC control.
 *
 * WHY THIS DESIGN:
 *   - Backend-agnostic: Works with VM, LLVM JIT, or WASM backends
 *   - Clean interface: Simple, consistent API for embedders
 *   - High performance: Minimal overhead, direct access to core functions
 *
 * RELATED MODULES:
 *   - xray_isolate.h: Isolate lifecycle management
 *   - xvalue.h: Value representation (tagged union)
 *   - xgc.h: Garbage collection internals
 */

#ifndef XRUNTIME_H
#define XRUNTIME_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "../base/xdefs.h"

#ifdef __cplusplus
extern "C" {
#endif // ========== Forward Declarations ==========

// XrValue is defined in xvalue.h, forward decls via xforward_decl.h
typedef struct XrSet XrSet;
typedef struct XrString XrString;
typedef struct XrClass XrClass;
typedef struct XrInstance XrInstance;
typedef struct XrClosure XrClosure;
typedef struct XrProto XrProto;
typedef struct Upvalue Upvalue;

// XrValue is defined in xvalue.h (16-byte tagged union struct)
// Include xvalue.h to get the complete definition

/* ========== Runtime Initialization and Cleanup ========== */

// Create runtime state, returns NULL on failure
XRAY_API XrayIsolate* xray_runtime_init(void);

// Destroy runtime and release all resources
XRAY_API void xray_runtime_cleanup(XrayIsolate *X);

// Get runtime version string
XRAY_API const char* xray_runtime_version(void);

/* ========== Memory Allocation ========== */

// Allocate GC-managed memory. type_info is optional but recommended.
XRAY_API void* xray_alloc(XrayIsolate *X, size_t size);

// Reallocate memory (for non-GC-managed memory only)
XRAY_API void* xray_realloc(XrayIsolate *X, void *ptr, size_t old_size, size_t new_size);

// Mark memory for GC collection (no-op, GC handles actual freeing)
XRAY_API void xray_free(XrayIsolate *X, void *ptr, size_t size);

/* ========== GC Control ========== */

// Trigger garbage collection
XRAY_API void xray_gc_collect(XrayIsolate *X);

// Get GC statistics (allocated bytes and threshold)
XRAY_API void xray_gc_stats(XrayIsolate *X, size_t *allocated, size_t *gc_threshold);

// Set GC threshold in bytes
XRAY_API void xray_gc_set_threshold(XrayIsolate *X, size_t threshold);

/* ========== Object Creation ========== */

// Create empty array
XRAY_API XrArray* xray_array_new(XrayIsolate *X);

// Create array with pre-allocated capacity
XRAY_API XrArray* xray_array_new_with_capacity(XrayIsolate *X, size_t capacity);

// Create empty map
XRAY_API XrMap* xray_map_new(XrayIsolate *X);

// Create empty set
XRAY_API XrSet* xray_set_new(XrayIsolate *X);

// Create interned string from C string
XRAY_API XrString* xray_string_new(XrayIsolate *X, const char *str, size_t len);

// Create class - declared in xclass.h
// Use: XrClass* xr_class_new(XrayIsolate *X, const char *name, XrClass *super);

// Create instance of a class
XR_FUNC XrInstance* xr_instance_new(XrayIsolate *X, XrClass *klass);

// Closure creation is declared in runtime/closure/xclosure.h.

/* ========== Array Operations ========== */

// Push element to array end
XR_FUNC void xr_array_push(XrayIsolate *X, XrArray *arr, XrValue val);

// Pop element from array end
XR_FUNC XrValue xr_array_pop(XrayIsolate *X, XrArray *arr);

// Get array element by index
XR_FUNC XrValue xr_array_get(XrayIsolate *X, XrArray *arr, int index);

// Set array element by index
XR_FUNC void xr_array_set(XrayIsolate *X, XrArray *arr, int index, XrValue val);

// Get array length
XR_FUNC size_t xr_array_length(XrArray *arr);

/* ========== Map Operations ========== */

// Set map key-value pair
XR_FUNC void xr_map_set(XrayIsolate *X, XrMap *map, XrValue key, XrValue val);

// Get map value, returns null if key not found
XR_FUNC XrValue xr_map_get(XrayIsolate *X, XrMap *map, XrValue key);

// Delete map entry, returns true if key existed
XR_FUNC bool xr_map_delete(XrayIsolate *X, XrMap *map, XrValue key);

// Check if key exists in map
XR_FUNC bool xr_map_contains(XrayIsolate *X, XrMap *map, XrValue key);

// Get number of entries in map
XR_FUNC size_t xr_map_size(XrMap *map);

/* ========== String Operations ========== */

// Concatenate two strings
XR_FUNC XrString* xr_string_concat(XrayIsolate *X, XrString *a, XrString *b);

// Compare strings: 0=equal, <0=a<b, >0=a>b
XR_FUNC int xr_string_compare(XrString *a, XrString *b);

// Get string length
XR_FUNC size_t xr_string_length(XrString *str);

// Get C string pointer (null-terminated)
XR_FUNC const char* xr_string_cstr(XrString *str);

/* ========== Closure and Upvalue ========== */

// Create upvalue pointing to stack slot
XR_FUNC Upvalue* xr_upvalue_new(XrayIsolate *X, XrValue *slot);

// Close upvalues: copy stack values to heap
XR_FUNC void xr_upvalue_close(XrayIsolate *X, XrValue *last);

/* ========== Type Checking ========== */
XR_FUNC bool xr_is_null(XrValue val);
XR_FUNC bool xr_is_bool(XrValue val);
XR_FUNC bool xr_is_int(XrValue val);
XR_FUNC bool xr_is_float(XrValue val);
XR_FUNC bool xr_is_number(XrValue val);
XR_FUNC bool xr_is_string(XrValue val);
XR_FUNC bool xr_is_array(XrValue val);
XR_FUNC bool xr_is_map(XrValue val);
XR_FUNC bool xr_is_set(XrValue val);
XR_FUNC bool xr_is_object(XrValue val);
XR_FUNC bool xr_is_closure(XrValue val);

/* ========== Type Extraction ========== */
XR_FUNC bool xr_to_bool(XrValue val);
XR_FUNC int64_t xr_to_int(XrValue val);
XR_FUNC double xr_to_float(XrValue val);
XR_FUNC XrString* xr_to_string(XrValue val);
XR_FUNC XrArray* xr_to_array(XrValue val);
XR_FUNC XrMap* xr_to_map(XrValue val);
XR_FUNC XrClosure* xr_to_closure(XrValue val);

/* ========== Value Construction ========== */
XR_FUNC XrValue xr_value_null(void);
XR_FUNC XrValue xr_value_bool(bool b);
XR_FUNC XrValue xr_value_int(int64_t i);
XR_FUNC XrValue xr_value_float(double f);
XR_FUNC XrValue xr_value_string(XrString *s);
XR_FUNC XrValue xr_value_array(XrArray *a);
XR_FUNC XrValue xr_value_map(XrMap *m);
XR_FUNC XrValue xr_value_object(void *obj);

/* ========== Debug and Printing ========== */

// Print value for debugging
XR_FUNC void xr_value_print(XrayIsolate *X, XrValue val);

// Get type name of value
XR_FUNC const char* xr_value_type_name(XrValue val);

/* ========== Error Handling ========== */

// Report runtime error with printf-style format
XR_FUNC void xr_runtime_error(XrayIsolate *X, const char *fmt, ...);

// Check if there is a pending error
XR_FUNC bool xr_runtime_has_error(XrayIsolate *X);

// Get error message string
XR_FUNC const char* xr_runtime_error_message(XrayIsolate *X);

// Clear error state
XR_FUNC void xr_runtime_clear_error(XrayIsolate *X);

#ifdef __cplusplus
}
#endif

#endif // XRUNTIME_H
