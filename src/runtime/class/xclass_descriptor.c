/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xclass_descriptor.c - Class descriptor implementation
 */

#include "xclass_descriptor.h"
#include "../../base/xchecks.h"
#include "../../base/xlog.h"
#include "xclass.h"
#include <stdio.h>
#include <string.h>

/* ========== Validation ========== */
bool xr_class_descriptor_validate(const XrClassDescriptor *descriptor) {
    if (!descriptor) {
        xr_log_warning("class", "descriptor validation: NULL descriptor");
        return false;
    }
    
    // Check version
    if (descriptor->descriptor_version != XR_CLASS_DESCRIPTOR_VERSION) {
        xr_log_warning("class", "descriptor validation: unsupported version %u (expected %u)",
                descriptor->descriptor_version, XR_CLASS_DESCRIPTOR_VERSION);
        return false;
    }
    
    // Check class name
    if (!descriptor->class_name || strlen(descriptor->class_name) == 0) {
        xr_log_warning("class", "descriptor validation: missing class name");
        return false;
    }
    
    // Check fields
    if (descriptor->instance_field_count > 0 && !descriptor->instance_fields) {
        xr_log_warning("class", "descriptor validation: instance_field_count > 0 but instance_fields is NULL");
        return false;
    }
    
    if (descriptor->static_field_count > 0 && !descriptor->static_fields) {
        xr_log_warning("class", "descriptor validation: static_field_count > 0 but static_fields is NULL");
        return false;
    }
    
    // Check methods
    if (descriptor->instance_method_count > 0 && !descriptor->instance_methods) {
        xr_log_warning("class", "descriptor validation: instance_method_count > 0 but instance_methods is NULL");
        return false;
    }
    
    if (descriptor->static_method_count > 0 && !descriptor->static_methods) {
        xr_log_warning("class", "descriptor validation: static_method_count > 0 but static_methods is NULL");
        return false;
    }
    
    // Check interfaces
    if (descriptor->interface_count > 0 && !descriptor->interfaces) {
        xr_log_warning("class", "descriptor validation: interface_count > 0 but interfaces is NULL");
        return false;
    }
    
    // Check abstract methods
    if (descriptor->abstract_method_count > 0 && !descriptor->abstract_method_names) {
        xr_log_warning("class", "descriptor validation: abstract_method_count > 0 but abstract_method_names is NULL");
        return false;
    }
    
    return true;
}

/* ========== Memory Estimation ========== */
size_t xr_class_descriptor_estimate_size(const XrClassDescriptor *descriptor) {
    if (!descriptor) return 0;
    
    size_t total = 0;
    
    // Base structure
    total += sizeof(XrClassDescriptor);
    
    // Field arrays
    total += descriptor->instance_field_count * sizeof(XrFieldDescriptorEntry);
    total += descriptor->static_field_count * sizeof(XrFieldDescriptorEntry);
    
    // Method arrays
    total += descriptor->instance_method_count * sizeof(XrMethodDescriptorEntry);
    total += descriptor->static_method_count * sizeof(XrMethodDescriptorEntry);
    
    // Interface array
    total += descriptor->interface_count * sizeof(XrInterfaceDescriptorEntry);
    
    // Abstract method array
    total += descriptor->abstract_method_count * sizeof(char*);
    
    // Symbol map (estimate: count * 1.5 * sizeof(int))
    uint32_t total_fields = descriptor->instance_field_count + descriptor->static_field_count;
    uint32_t total_methods = descriptor->instance_method_count + descriptor->static_method_count;
    total += (total_fields * 3 / 2) * sizeof(int);
    total += (total_methods * 3 / 2) * sizeof(int);
    
    // Buffer (20%)
    total = (total * 12) / 10;
    
    return total;
}

/* ========== Debug Output ========== */
void xr_class_descriptor_print(const XrClassDescriptor *descriptor, bool verbose) {
    if (!descriptor) {
        printf("[ClassDescriptor] NULL descriptor\n");
        return;
    }
    
    printf("========== ClassDescriptor ==========\n");
    printf("Class Name: %s\n", descriptor->class_name ? descriptor->class_name : "(null)");
    printf("Super Class: %s\n", descriptor->super_name ? descriptor->super_name : "(none)");
    printf("Version: %u\n", descriptor->descriptor_version);
    printf("Flags: 0x%04X\n", descriptor->flags);
    printf("\n");
    
    printf("Instance Fields: %u\n", descriptor->instance_field_count);
    if (verbose && descriptor->instance_fields) {
        for (uint32_t i = 0; i < descriptor->instance_field_count; i++) {
            XrFieldDescriptorEntry *field = &descriptor->instance_fields[i];
            printf("  [%u] %s : %s (flags=0x%04X)\n",
                   i, field->name ? field->name : "(null)",
                   field->type_name ? field->type_name : "unknown",
                   field->flags);
        }
    }
    
    printf("Static Fields: %u\n", descriptor->static_field_count);
    if (verbose && descriptor->static_fields) {
        for (uint32_t i = 0; i < descriptor->static_field_count; i++) {
            XrFieldDescriptorEntry *field = &descriptor->static_fields[i];
            printf("  [%u] %s : %s (flags=0x%04X)\n",
                   i, field->name ? field->name : "(null)",
                   field->type_name ? field->type_name : "unknown",
                   field->flags);
        }
    }
    
    printf("Instance Methods: %u\n", descriptor->instance_method_count);
    if (verbose && descriptor->instance_methods) {
        for (uint32_t i = 0; i < descriptor->instance_method_count; i++) {
            XrMethodDescriptorEntry *method = &descriptor->instance_methods[i];
            printf("  [%u] %s (closure_index=%u, params=%u, flags=0x%04X)\n",
                   i, method->name ? method->name : "(null)",
                   method->closure_index, method->param_count, method->flags);
        }
    }
    
    printf("Static Methods: %u\n", descriptor->static_method_count);
    if (verbose && descriptor->static_methods) {
        for (uint32_t i = 0; i < descriptor->static_method_count; i++) {
            XrMethodDescriptorEntry *method = &descriptor->static_methods[i];
            printf("  [%u] %s (closure_index=%u, params=%u, flags=0x%04X)\n",
                   i, method->name ? method->name : "(null)",
                   method->closure_index, method->param_count, method->flags);
        }
    }
    
    printf("Interfaces: %u\n", descriptor->interface_count);
    if (verbose && descriptor->interfaces) {
        for (uint8_t i = 0; i < descriptor->interface_count; i++) {
            printf("  [%u] %s\n", i, descriptor->interfaces[i].interface_name ?
                   descriptor->interfaces[i].interface_name : "(null)");
        }
    }
    
    printf("Abstract Methods: %u\n", descriptor->abstract_method_count);
    if (verbose && descriptor->abstract_method_names) {
        for (uint8_t i = 0; i < descriptor->abstract_method_count; i++) {
            printf("  [%u] %s\n", i, descriptor->abstract_method_names[i] ?
                   descriptor->abstract_method_names[i] : "(null)");
        }
    }
    
    // Memory estimation
    size_t estimated_size = xr_class_descriptor_estimate_size(descriptor);
    printf("\nEstimated Memory: %zu bytes\n", estimated_size);
    
    printf("=====================================\n");
}
