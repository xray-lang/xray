/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xclass_builder.h - Class builder API for compile/load time
 *
 * KEY CONCEPT:
 *   Dynamic arrays for collecting fields/methods during build.
 *   Optimizes and freezes into immutable class on finalize.
 */

#ifndef XCLASS_BUILDER_H
#define XCLASS_BUILDER_H

#include "xclass.h"
#include <xray_isolate.h>
#include <stdbool.h>

/* ========== Build Item Containers ========== */

typedef struct XrFieldBuildItem {
    char *name;
    int symbol;
    XrValue default_value;
    uint32_t flags;
    uint16_t offset;         // Computed on finalize
} XrFieldBuildItem;

typedef struct XrMethodBuildItem {
    char *name;
    int symbol;
    XrMethodType method_type;    // CLOSURE/PRIMITIVE/GETTER/SETTER/OPERATOR
    union {
        XrCFunctionPtr primitive;
        XrClosure *closure;
    } impl;
    int param_count;
    uint32_t flags;
    uint8_t op_type;             // for operator methods
} XrMethodBuildItem;

typedef struct XrStaticFieldBuildItem {
    char *name;
    int symbol;
    XrValue value;
    uint32_t flags;
} XrStaticFieldBuildItem;

/* ========== Class Builder Structure ========== */

/*
 * Lifecycle:
 *   1. xr_class_builder_new()      - Create
 *   2. xr_class_builder_add_xxx()  - Add members
 *   3. xr_class_builder_finalize() - Freeze into immutable class, destroy builder
 */
struct XrClassBuilder {
    XrGCHeader gc;
    
    // Basic info
    XrayIsolate *isolate;
    char *name;
    XrClass *super;
    
    // Fields (dynamic array)
    XrFieldBuildItem *fields;
    int field_count;
    int field_capacity;
    
    // Methods (dynamic array)
    XrMethodBuildItem *methods;
    int method_count;
    int method_capacity;
    
    // Static fields (dynamic array)
    XrStaticFieldBuildItem *static_fields;
    int static_field_count;
    int static_field_capacity;
    
    // Static methods (dynamic array)
    XrMethodBuildItem *static_methods;
    int static_method_count;
    int static_method_capacity;
    
    // Interfaces (dynamic array)
    XrClass **interfaces;
    int interface_count;
    int interface_capacity;
    
    // Abstract methods (dynamic array)
    int *abstract_methods;
    int abstract_method_count;
    int abstract_method_capacity;
    
    // Flags
    uint32_t flags;
    bool finalized;
};

/* ========== Builder API ========== */

XR_FUNC XrClassBuilder* xr_class_builder_new(XrayIsolate *isolate, 
                                      const char *name,
                                      XrClass *super);

// Returns 0 on success, -1 on failure (e.g., duplicate field)
XR_FUNC int xr_class_builder_add_field(XrClassBuilder *builder,
                                const char *name,
                                uint32_t flags);

// Returns 0 on success, -1 on failure (e.g., duplicate method)
XR_FUNC int xr_class_builder_add_method(XrClassBuilder *builder,
                                 const char *name,
                                 XrCFunctionPtr impl,
                                 int param_count,
                                 uint32_t flags);

// Add method with closure (for descriptor path)
XR_FUNC int xr_class_builder_add_method_closure(XrClassBuilder *builder,
                                         const char *name,
                                         XrClosure *closure,
                                         XrMethodType method_type,
                                         int param_count,
                                         uint32_t flags,
                                         uint8_t op_type);

XR_FUNC int xr_class_builder_add_static_field(XrClassBuilder *builder,
                                       const char *name,
                                       XrValue value,
                                       uint32_t flags);

XR_FUNC int xr_class_builder_add_static_method(XrClassBuilder *builder,
                                        const char *name,
                                        XrCFunctionPtr impl,
                                        int param_count,
                                        uint32_t flags);

// Add static method with closure (for descriptor path)
XR_FUNC int xr_class_builder_add_static_method_closure(XrClassBuilder *builder,
                                                const char *name,
                                                XrClosure *closure,
                                                int param_count,
                                                uint32_t flags);

XR_FUNC int xr_class_builder_add_interface(XrClassBuilder *builder,
                                    XrClass *interface);

XR_FUNC int xr_class_builder_add_abstract_method(XrClassBuilder *builder,
                                          int method_symbol);

XR_FUNC void xr_class_builder_set_flags(XrClassBuilder *builder, uint32_t flags);

// Finalize: compute offsets, generate vtable, freeze class, destroy builder
XR_FUNC XrClass* xr_class_builder_finalize(XrClassBuilder *builder);

// Manual cleanup if finalize fails
XR_FUNC void xr_class_builder_destroy(XrClassBuilder *builder);

/* ========== Internal Helpers ========== */

XR_FUNC bool xr_class_builder_has_field(const XrClassBuilder *builder, const char *name);
XR_FUNC bool xr_class_builder_has_method(const XrClassBuilder *builder, const char *name);
XR_FUNC uint16_t xr_class_builder_calculate_instance_size(const XrClassBuilder *builder);
XR_FUNC int xr_class_builder_generate_vtable(XrClassBuilder *builder, XrClass *cls);

#endif // XCLASS_BUILDER_H
