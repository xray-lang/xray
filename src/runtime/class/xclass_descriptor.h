/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xclass_descriptor.h - Class descriptor for single-instruction class creation
 *
 * KEY CONCEPT:
 *   ClassDescriptor contains all information needed to create a class.
 *   Compiler generates it at compile-time, VM creates class with one instruction.
 *
 * WHY THIS DESIGN:
 *   - Single instruction class creation (OP_CLASS_CREATE_FROM_DESCRIPTOR)
 *   - No 255 field/method limit
 *   - 96% instruction reduction (25 -> 1)
 */

#ifndef XCLASS_DESCRIPTOR_H
#define XCLASS_DESCRIPTOR_H

#include <stdint.h>
#include <stdbool.h>
#include "xmethod.h"
#include "../value/xvalue.h"


/* ========== Field Descriptor Entry ========== */

typedef struct XrFieldDescriptorEntry {
    const char *name;
    const char *type_name;
    XrValue default_value;
    uint16_t flags;
} XrFieldDescriptorEntry;

/* ========== Method Descriptor Entry ========== */

typedef struct XrMethodDescriptorEntry {
    const char *name;
    uint32_t closure_index;
    const char *return_type_name;
    const char **param_type_names;
    uint8_t param_count;
    uint16_t flags;
    uint8_t op_type;
    bool is_operator;
} XrMethodDescriptorEntry;

/* ========== Interface Descriptor Entry ========== */

typedef struct XrInterfaceDescriptorEntry {
    struct XrClass *interface_ptr;
    const char *interface_name;
} XrInterfaceDescriptorEntry;

/* ========== Class Descriptor ========== */

typedef struct XrClassDescriptor {
    /* ========== Basic Info ========== */
    const char *class_name;
    const char *super_name;
    int32_t super_global_index;
    uint32_t flags;

    /* ========== Fields ========== */
    XrFieldDescriptorEntry *instance_fields;
    uint32_t instance_field_count;

    XrFieldDescriptorEntry *static_fields;
    uint32_t static_field_count;

    /* ========== Methods ========== */
    XrMethodDescriptorEntry *instance_methods;
    uint32_t instance_method_count;

    XrMethodDescriptorEntry *static_methods;
    uint32_t static_method_count;

    /* ========== Interfaces ========== */
    XrInterfaceDescriptorEntry *interfaces;
    uint8_t interface_count;

    /* ========== Abstract Methods ========== */
    const char **abstract_method_names;
    uint8_t abstract_method_count;

    /* ========== Static Constructor ========== */
    int32_t clinit_proto_index;

    /* ========== Struct Native Storage ========== */
    struct XrStructLayout *struct_layout;  // NULL for class, set for struct

    /* ========== Metadata ========== */
    uint32_t descriptor_version;
    uint32_t checksum;
} XrClassDescriptor;

#define XR_CLASS_DESCRIPTOR_VERSION 1

/* ========== Helper Functions ========== */

XR_FUNC bool xr_class_descriptor_validate(const XrClassDescriptor *descriptor);
XR_FUNC size_t xr_class_descriptor_estimate_size(const XrClassDescriptor *descriptor);
XR_FUNC void xr_class_descriptor_print(const XrClassDescriptor *descriptor, bool verbose);

/* ========== Create XrClass from Descriptor ========== */

// Forward declarations via xforward_decl.h

/*
 * Create an immutable XrClass from a descriptor.
 *
 * super_override, if non-NULL, takes precedence over any super information
 * encoded in the descriptor (super_global_index / super_name). The VM uses
 * this path to pass a runtime-resolved parent class (for `extends` whose
 * parent lives in a local / upvalue / imported module member) so that
 * inheritance is established in a single instruction without a follow-up
 * OP_INHERIT. When super_override is NULL the function falls back to
 * descriptor-encoded resolution.
 */
XR_FUNC XrClass* xr_class_from_descriptor(XrayIsolate *isolate, const XrClassDescriptor *descriptor,
                                   XrProto *proto, struct XrClosure *cl, XrValue *base,
                                   XrVMContext *vm_ctx, XrClass *super_override);

#endif // XCLASS_DESCRIPTOR_H
