/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcompiler_class_registry.c - Class registry implementation
 */

#include "xcompiler_class_registry.h"
#include "../../base/xchecks.h"
#include "../../base/xmalloc.h"
#include <stdlib.h>
#include <string.h>

#define INITIAL_CAPACITY 16

ClassRegistry* xr_class_registry_new(void) {
    ClassRegistry *registry = (ClassRegistry*)xr_malloc(sizeof(ClassRegistry));
    if (!registry) return NULL;
    
    registry->classes = (ClassInfo**)xr_malloc(sizeof(ClassInfo*) * INITIAL_CAPACITY);
    if (!registry->classes) {
        xr_free(registry);
        return NULL;
    }
    
    registry->class_count = 0;
    registry->capacity = INITIAL_CAPACITY;
    
    return registry;
}

void xr_class_registry_free(ClassRegistry *registry) {
    if (!registry) return;
    
    for (int i = 0; i < registry->class_count; i++) {
        ClassInfo *info = registry->classes[i];
        if (info) {
            xr_free(info->class_name);
            
            for (int j = 0; j < info->instance_field_count; j++) {
                xr_free(info->instance_fields[j].name);
            }
            xr_free(info->instance_fields);
            
            for (int j = 0; j < info->method_count; j++) {
                xr_free(info->methods[j].name);
            }
            xr_free(info->methods);
            
            xr_free(info);
        }
    }
    
    xr_free(registry->classes);
    xr_free(registry);
}

ClassInfo* xr_class_registry_register(ClassRegistry *registry, 
                                       const char *class_name) {
    if (!registry || !class_name) return NULL;
    
    ClassInfo *existing = xr_class_registry_lookup(registry, class_name);
    if (existing) return existing;
    
    if (registry->class_count >= registry->capacity) {
        int new_capacity = registry->capacity * 2;
        ClassInfo **new_classes = (ClassInfo**)xr_realloc(registry->classes, 
                                                        sizeof(ClassInfo*) * new_capacity);
        if (!new_classes) return NULL;
        
        registry->classes = new_classes;
        registry->capacity = new_capacity;
    }
    
    ClassInfo *info = (ClassInfo*)xr_calloc(1, sizeof(ClassInfo));
    if (!info) return NULL;
    
    info->class_name = xr_strdup(class_name);
    
    info->instance_fields = (FieldInfo*)xr_malloc(sizeof(FieldInfo) * INITIAL_CAPACITY);
    info->instance_field_count = 0;
    info->instance_field_capacity = INITIAL_CAPACITY;
    
    info->methods = (MethodInfo*)xr_malloc(sizeof(MethodInfo) * INITIAL_CAPACITY);
    info->method_count = 0;
    info->method_capacity = INITIAL_CAPACITY;
    
    registry->classes[registry->class_count++] = info;
    
    return info;
}

ClassInfo* xr_class_registry_lookup(ClassRegistry *registry, 
                                     const char *class_name) {
    if (!registry || !class_name) return NULL;
    
    for (int i = 0; i < registry->class_count; i++) {
        if (strcmp(registry->classes[i]->class_name, class_name) == 0) {
            return registry->classes[i];
        }
    }
    
    return NULL;
}

bool xr_class_registry_is_class(ClassRegistry *registry, 
                                 const char *name) {
    return xr_class_registry_lookup(registry, name) != NULL;
}

int xr_class_add_instance_field(ClassInfo *class_info, 
                                 const char *field_name, 
                                 int index) {
    if (!class_info || !field_name) return -1;
    
    if (class_info->instance_field_count >= class_info->instance_field_capacity) {
        int new_capacity = class_info->instance_field_capacity * 2;
        FieldInfo *new_fields = (FieldInfo*)xr_realloc(class_info->instance_fields, 
                                                     sizeof(FieldInfo) * new_capacity);
        if (!new_fields) return -1;
        
        class_info->instance_fields = new_fields;
        class_info->instance_field_capacity = new_capacity;
    }
    
    FieldInfo *field = &class_info->instance_fields[class_info->instance_field_count++];
    field->name = xr_strdup(field_name);
    field->index = index;
    field->slot_type = 0;  // XR_SLOT_ANY
    return 0;
}

int xr_class_add_instance_field_typed(ClassInfo *class_info,
                                      const char *field_name,
                                      int index,
                                      uint8_t slot_type) {
    if (!class_info || !field_name) return -1;
    
    if (class_info->instance_field_count >= class_info->instance_field_capacity) {
        int new_capacity = class_info->instance_field_capacity * 2;
        FieldInfo *new_fields = (FieldInfo*)xr_realloc(class_info->instance_fields, 
                                                     sizeof(FieldInfo) * new_capacity);
        if (!new_fields) return -1;
        
        class_info->instance_fields = new_fields;
        class_info->instance_field_capacity = new_capacity;
    }
    
    FieldInfo *field = &class_info->instance_fields[class_info->instance_field_count++];
    field->name = xr_strdup(field_name);
    field->index = index;
    field->slot_type = slot_type;
    return 0;
}

uint8_t xr_class_get_field_slot_type(ClassInfo *class_info,
                                     const char *field_name) {
    if (!class_info || !field_name) return 0;
    
    for (int i = 0; i < class_info->instance_field_count; i++) {
        if (strcmp(class_info->instance_fields[i].name, field_name) == 0) {
            return class_info->instance_fields[i].slot_type;
        }
    }
    return 0;  // XR_SLOT_ANY
}

int xr_class_find_instance_field_index(ClassInfo *class_info, 
                                        const char *field_name) {
    if (!class_info || !field_name) return -1;
    
    for (int i = 0; i < class_info->instance_field_count; i++) {
        if (strcmp(class_info->instance_fields[i].name, field_name) == 0) {
            return class_info->instance_fields[i].index;
        }
    }
    
    return -1;
}

int xr_class_add_method(ClassInfo *class_info,
                        const char *method_name,
                        int index) {
    if (!class_info || !method_name) return -1;
    
    // Override: if method with same name exists, update its index in-place
    for (int i = 0; i < class_info->method_count; i++) {
        if (strcmp(class_info->methods[i].name, method_name) == 0) {
            class_info->methods[i].index = index;
            return 0;
        }
    }
    
    if (class_info->method_count >= class_info->method_capacity) {
        int new_capacity = class_info->method_capacity * 2;
        MethodInfo *new_methods = (MethodInfo*)xr_realloc(class_info->methods,
                                                         sizeof(MethodInfo) * new_capacity);
        if (!new_methods) return -1;
        
        class_info->methods = new_methods;
        class_info->method_capacity = new_capacity;
    }
    
    MethodInfo *m = &class_info->methods[class_info->method_count++];
    m->name = xr_strdup(method_name);
    m->index = index;
    return 0;
}

int xr_class_find_method_index(ClassInfo *class_info,
                               const char *method_name) {
    if (!class_info || !method_name) return -1;
    
    for (int i = 0; i < class_info->method_count; i++) {
        if (strcmp(class_info->methods[i].name, method_name) == 0) {
            return class_info->methods[i].index;
        }
    }
    
    return -1;
}
