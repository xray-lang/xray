/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtype_generic.c - Generic type substitution, constraint checking, and iterable
 *
 * KEY CONCEPT:
 *   Implements generic type parameter substitution, interface/class constraint
 *   satisfaction checks, and built-in iterable type detection.
 *   Split from xtype.c for maintainability.
 */

#include "xtype.h"
#include "xtype_internal.h"
#include "../../frontend/analyzer/xanalyzer_symbol.h"
#include "../../base/xmalloc.h"
#include "../../base/xchecks.h"
#include <string.h>

// Helper: find method in class hierarchy
static XaSymbol *find_method_in_class(XrClassInfo *class_info, const char *name) {
    if (!class_info || !name) return NULL;
    
    // Check current class
    for (int i = 0; i < class_info->method_count; i++) {
        XaSymbol *method = class_info->methods[i];
        if (method && method->name && strcmp(method->name, name) == 0) {
            return method;
        }
    }
    
    // Check base class
    if (class_info->base) {
        return find_method_in_class(class_info->base, name);
    }
    
    return NULL;
}

// Helper: check if class implements all interface methods
static bool check_interface_implementation(XrType *class_type, XrType *interface_type) {
    if (!class_type || !interface_type) return false;
    
    // Get class info from class type
    XrClassInfo *class_info = class_type->instance.class_ref;
    if (!class_info) {
        // No class info available - cannot verify, assume compatible
        return true;
    }
    
    // If interface has no class_ref, it's a named interface without method list
    // In this case, we rely on naming convention (duck typing)
    XrClassInfo *iface_info = interface_type->instance.class_ref;
    if (!iface_info) {
        // No interface definition - assume compatible (duck typing)
        return true;
    }
    
    // Check each interface method exists in class with compatible signature
    for (int i = 0; i < iface_info->method_count; i++) {
        XaSymbol *iface_method = iface_info->methods[i];
        if (!iface_method || !iface_method->name) continue;
        
        // Find matching method in class hierarchy
        XaSymbol *class_method = find_method_in_class(class_info, iface_method->name);
        if (!class_method) {
            return false;  // Missing required method
        }
        
        // Check method signature compatibility (requires analyzer context)
        // For now, just check name match - signature check requires XaSymbolLinks
        // which we don't have access to here without the analyzer
    }
    
    return true;
}

// ============================================================================
// Generic Type Substitution
// ============================================================================

// Substitute type parameters with actual types
// e.g., substitute(T, ["T"], [int]) = int
//       substitute(Array<T>, ["T"], [int]) = Array<int>
XrType *xr_type_substitute(XrType *type, const char **param_names,
                           XrType **actual_types, int count) {
    if (!type || count == 0) return type;
    
    // If it's a type parameter, look it up
    if (type->kind == XR_KIND_TYPE_PARAM) {
        const char *param_name = type->type_param.name;
        if (param_name) {
            for (int i = 0; i < count; i++) {
                if (param_names[i] && strcmp(param_names[i], param_name) == 0) {
                    return actual_types[i] ? xr_type_copy(actual_types[i]) : type;
                }
            }
        }
        return type;  // Not found, keep as type parameter
    }
    
    // Recursively substitute in container types
    if (type->kind == XR_KIND_ARRAY) {
        XrType *elem = xr_type_substitute(type->container.element_type, 
                                          param_names, actual_types, count);
        if (elem != type->container.element_type) {
            return xr_type_new_array(elem);
        }
        return type;
    }
    
    if (type->kind == XR_KIND_SET) {
        XrType *elem = xr_type_substitute(type->container.element_type,
                                          param_names, actual_types, count);
        if (elem != type->container.element_type) {
            return xr_type_new_set(elem);
        }
        return type;
    }
    
    if (type->kind == XR_KIND_CHANNEL) {
        XrType *elem = xr_type_substitute(type->container.element_type,
                                          param_names, actual_types, count);
        if (elem != type->container.element_type) {
            return xr_type_new_channel(elem);
        }
        return type;
    }
    
    if (xr_type_is_named_class(type, "Task")) {
        XrType *old_result = (type->instance.type_arg_count > 0) ? type->instance.type_args[0] : NULL;
        XrType *result = xr_type_substitute(old_result, param_names, actual_types, count);
        if (result != old_result) {
            return xr_type_new_task(result);
        }
        return type;
    }
    
    if (type->kind == XR_KIND_MAP) {
        XrType *key = xr_type_substitute(type->map.key_type,
                                         param_names, actual_types, count);
        XrType *val = xr_type_substitute(type->map.value_type,
                                         param_names, actual_types, count);
        if (key != type->map.key_type || val != type->map.value_type) {
            return xr_type_new_map(key, val);
        }
        return type;
    }
    
    // Substitute in function types
    if (type->kind == XR_KIND_FUNCTION) {
        bool changed = false;
        XrType *stack_params[16];
        int pc = type->function.param_count;
        XrType **new_params = (pc <= 16) ? stack_params : xr_malloc(sizeof(XrType*) * pc);
        if (!new_params) return type;
        
        for (int i = 0; i < pc; i++) {
            new_params[i] = xr_type_substitute(type->function.param_types[i],
                                               param_names, actual_types, count);
            if (new_params[i] != type->function.param_types[i]) changed = true;
        }
        
        XrType *ret = xr_type_substitute(type->function.return_type,
                                         param_names, actual_types, count);
        if (ret != type->function.return_type) changed = true;
        
        if (changed) {
            XrType *result = xr_type_new_function(new_params, pc, ret, type->function.is_variadic);
            if (result) result->function.min_params = type->function.min_params;
            if (new_params != stack_params) xr_free(new_params);
            return result;
        }
        if (new_params != stack_params) xr_free(new_params);
        return type;
    }
    
    // Substitute in union types
    if (type->kind == XR_KIND_UNION) {
        bool changed = false;
        XrType *new_members[XR_UNION_MAX_MEMBERS];
        int mc = type->union_type.member_count;
        for (int i = 0; i < mc; i++) {
            new_members[i] = xr_type_substitute(type->union_type.members[i],
                                                param_names, actual_types, count);
            if (new_members[i] != type->union_type.members[i]) changed = true;
        }
        if (changed) return xr_type_new_union(new_members, mc);
        return type;
    }
    
    // Substitute in nullable types
    if (type->is_nullable) {
        XrType *non_null = xr_type_non_nullable(type);
        XrType *subst = xr_type_substitute(non_null,
                                           param_names, actual_types, count);
        if (subst != non_null) {
            return xr_type_make_nullable(subst);
        }
        return type;
    }
    
    // Substitute in tuple types
    if (type->kind == XR_KIND_TUPLE) {
        bool changed = false;
        XrType *stack_elems[16];
        int ec = type->tuple.element_count;
        XrType **new_elems = (ec <= 16) ? stack_elems : xr_malloc(sizeof(XrType*) * ec);
        if (!new_elems) return type;
        for (int i = 0; i < ec; i++) {
            new_elems[i] = xr_type_substitute(type->tuple.element_types[i],
                                              param_names, actual_types, count);
            if (new_elems[i] != type->tuple.element_types[i]) changed = true;
        }
        if (changed) {
            XrType *result = xr_type_new_tuple(new_elems, ec);
            if (new_elems != stack_elems) xr_free(new_elems);
            return result;
        }
        if (new_elems != stack_elems) xr_free(new_elems);
        return type;
    }
    
    // For class instances with type arguments, substitute them too
    if (type->kind == XR_KIND_INSTANCE && type->instance.type_arg_count > 0) {
        bool changed = false;
        XrType *stack_args[16];
        int ac = type->instance.type_arg_count;
        XrType **new_args = (ac <= 16) ? stack_args : xr_malloc(sizeof(XrType*) * ac);
        if (!new_args) return type;
        for (int i = 0; i < ac; i++) {
            new_args[i] = xr_type_substitute(type->instance.type_args[i],
                                             param_names, actual_types, count);
            if (new_args[i] != type->instance.type_args[i]) changed = true;
        }
        if (changed) {
            XrType *result = xr_type_new_generic_instance(type->instance.class_name,
                                                type->instance.class_ref,
                                                new_args, ac);
            if (new_args != stack_args) xr_free(new_args);
            return result;
        }
        if (new_args != stack_args) xr_free(new_args);
        return type;
    }
    
    // Substitute in fixed-length array types
    if (type->kind == XR_KIND_FIXED_ARRAY) {
        XrType *elem = xr_type_substitute(type->fixed_array.element_type,
                                          param_names, actual_types, count);
        if (elem != type->fixed_array.element_type) {
            return xr_type_new_fixed_array(elem, type->fixed_array.length);
        }
        return type;
    }
    
    // No substitution needed for other types
    return type;
}

// Check if type satisfies a constraint (for generics)
bool xr_type_satisfies_constraint(XrType *type, XrType *constraint) {
    if (!constraint) return true;
    if (!type) return false;
    
    // Check built-in interface constraints by name
    if (constraint->kind == XR_KIND_INTERFACE) {
        const char *iface_name = constraint->instance.class_name;
        if (iface_name) {
            if (strcmp(iface_name, "Iterable") == 0) {
                return xr_kind_is_builtin_iterable(type->kind);
            }
            if (strcmp(iface_name, "Comparable") == 0) {
                XrTypeKind k = type->kind;
                return k == XR_KIND_INT || k == XR_KIND_FLOAT || k == XR_KIND_STRING;
            }
            if (strcmp(iface_name, "Hashable") == 0) {
                return xr_kind_is_primitive(type->kind);
            }
            if (strcmp(iface_name, "Stringable") == 0) {
                return true;  // All types are Stringable
            }
            if (strcmp(iface_name, "Iterator") == 0) {
                return false;  // No built-in type directly implements Iterator
            }
            if (strcmp(iface_name, "Indexable") == 0) {
                XrTypeKind k = type->kind;
                return k == XR_KIND_ARRAY || k == XR_KIND_STRING || k == XR_KIND_MAP || k == XR_KIND_BYTES;
            }
            if (strcmp(iface_name, "Equatable") == 0) {
                return true;  // All types support == and !=
            }
            if (strcmp(iface_name, "Lengthable") == 0) {
                XrTypeKind k = type->kind;
                return k == XR_KIND_ARRAY || k == XR_KIND_STRING || k == XR_KIND_MAP || k == XR_KIND_SET || k == XR_KIND_BYTES;
            }
            if (strcmp(iface_name, "Callable") == 0) {
                return type->kind == XR_KIND_FUNCTION || type->kind == XR_KIND_CLASS;
            }
            if (strcmp(iface_name, "Closeable") == 0) {
                // Closeable: Channel, File, or user types with close() method
                if (type->kind == XR_KIND_INSTANCE) {
                    const char *name = type->instance.class_name;
                    if (name && (strcmp(name, "Channel") == 0 || strcmp(name, "File") == 0)) {
                        return true;
                    }
                }
                // User types checked via method lookup below
            }
        }
        
        // For user-defined interfaces, check method implementation
        if (type->kind == XR_KIND_CLASS || type->kind == XR_KIND_INSTANCE) {
            return check_interface_implementation(type, constraint);
        }
        return false;
    }
    
    // Class constraint: check inheritance
    if (constraint->kind == XR_KIND_CLASS || constraint->kind == XR_KIND_INSTANCE) {
        return xr_type_is_subclass_of(type, constraint);
    }
    
    return xr_type_assignable(constraint, type);
}

// ============================================================================
// Iterable/Iterator Structural Type Checking (Built-in types only)
// For custom class checking, use xa_analyzer_is_iterable() in xanalyzer.c
// ============================================================================

// Check if type is a built-in iterable (Array, Set, Map, String)
bool xr_type_is_iterable(XrType *type, XrType **out_element_type) {
    if (!type) return false;
    
    // Built-in iterable types
    if (type->kind == XR_KIND_ARRAY) {
        if (out_element_type) {
            *out_element_type = type->container.element_type 
                ? type->container.element_type 
                : xr_type_new_unknown();
        }
        return true;
    }
    
    if (type->kind == XR_KIND_SET) {
        if (out_element_type) {
            *out_element_type = type->container.element_type 
                ? type->container.element_type 
                : xr_type_new_unknown();
        }
        return true;
    }
    
    if (type->kind == XR_KIND_MAP) {
        // Map iteration returns entries (key-value pairs)
        if (out_element_type) {
            *out_element_type = xr_type_new_unknown();  // [key, value] tuple
        }
        return true;
    }
    
    if (type->kind == XR_KIND_STRING) {
        if (out_element_type) {
            *out_element_type = xr_type_new_string();  // Character strings
        }
        return true;
    }
    
    // For custom class iterable checking, use xa_analyzer_is_iterable()
    return false;
}

// Stub for xr_type_is_iterator - full implementation in xanalyzer.c
bool xr_type_is_iterator(XrType *type, XrType **out_element_type) {
    (void)type;
    (void)out_element_type;
    // Custom class iterator checking requires analyzer context
    // Use xa_analyzer_is_iterator() instead
    return false;
}
