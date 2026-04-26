/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcompiler_class_registry.h - Compile-time class registry
 *
 * KEY CONCEPT:
 *   Records class structure info at compile time for type checking
 *   and optimized instruction generation with index mapping.
 */

#ifndef XCOMPILER_CLASS_REGISTRY_H
#define XCOMPILER_CLASS_REGISTRY_H

#include <stdint.h>
#include <stdbool.h>
#include "../../base/xdefs.h"

typedef struct FieldInfo {
    char *name;
    int index;
    uint8_t slot_type;  // XrSlotType: ANY/I64/F64 for TFIELD_SET optimization
} FieldInfo;

typedef struct MethodInfo {
    char *name;
    int index;  // method index in XrClass.methods[]
} MethodInfo;

typedef struct ClassInfo {
    char *class_name;

    FieldInfo *instance_fields;
    int instance_field_count;
    int instance_field_capacity;

    MethodInfo *methods;
    int method_count;
    int method_capacity;

    // Constructor info (for smart super() auto-insertion)
    bool has_constructor;
    int constructor_required_params;

    // Struct native storage (VALUE_TYPE only)
    bool is_value_type;
    struct XrStructLayout *struct_layout;  // NULL for class, set for struct
} ClassInfo;

typedef struct ClassRegistry {
    ClassInfo **classes;
    int class_count;
    int capacity;
} ClassRegistry;

XR_FUNC ClassRegistry *xr_class_registry_new(void);
XR_FUNC void xr_class_registry_free(ClassRegistry *registry);

XR_FUNC ClassInfo *xr_class_registry_register(ClassRegistry *registry, const char *class_name);

XR_FUNC ClassInfo *xr_class_registry_lookup(ClassRegistry *registry, const char *class_name);

XR_FUNC int xr_class_add_instance_field(ClassInfo *class_info, const char *field_name, int index);

XR_FUNC int xr_class_add_instance_field_typed(ClassInfo *class_info, const char *field_name,
                                              int index, uint8_t slot_type);

XR_FUNC int xr_class_find_instance_field_index(ClassInfo *class_info, const char *field_name);

XR_FUNC uint8_t xr_class_get_field_slot_type(ClassInfo *class_info, const char *field_name);

XR_FUNC bool xr_class_registry_is_class(ClassRegistry *registry, const char *name);

XR_FUNC int xr_class_add_method(ClassInfo *class_info, const char *method_name, int index);

XR_FUNC int xr_class_find_method_index(ClassInfo *class_info, const char *method_name);

#endif  // XCOMPILER_CLASS_REGISTRY_H
