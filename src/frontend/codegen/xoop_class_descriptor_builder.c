/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xoop_class_descriptor_builder.c - ClassDescriptor builder implementation
 *
 * KEY CONCEPT:
 *   - Create ClassDescriptor from AST nodes
 *   - Collect fields, methods, interfaces info
 *   - Store ClassDescriptor in constant pool
 */

#include "../../base/xlog.h"

#include "xoop_class_descriptor_builder.h"
#include "../../base/xchecks.h"
#include "../../runtime/value/xtype.h"
#include "../../runtime/class/xclass.h"
#include "../../runtime/xisolate_api.h"    // XrayIsolate full definition
#include "../../runtime/xglobals_table.h"  // xr_globals_get
#include "../parser/xast.h"                // AST node types
#include "../../base/xmalloc.h"
#include "../../runtime/value/xvalue.h"
#include "../analyzer/xanalyzer.h"
#include "../analyzer/xanalyzer_symbol.h"
#include "xcompiler_context.h"
#include <string.h>
#include <stdio.h>

// ========== Helper Functions ==========

/*
 * Convert simple literal AST node to XrValue
 *
 * Supports: null, bool, int, float, string
 * Not supported: complex expressions (need runtime evaluation)
 *
 * @param X     Isolate (for creating strings)
 * @param node  AST node
 * @return      Corresponding XrValue, unsupported types return xr_null()
 */
static XrValue ast_literal_to_value(XrayIsolate *X, AstNode *node) {
    if (!node)
        return xr_null();

    // Handle based on AST node type
    switch (node->type) {
        case AST_LITERAL_NULL:
            return xr_null();

        case AST_LITERAL_TRUE:
            return xr_bool(true);

        case AST_LITERAL_FALSE:
            return xr_bool(false);

        case AST_LITERAL_INT:
            return xr_int(node->as.literal.raw_value.int_val);

        case AST_LITERAL_FLOAT:
            return xr_float(node->as.literal.raw_value.float_val);

        case AST_LITERAL_STRING: {
            // Use interned string (won't be GC collected)
            const char *str = node->as.literal.raw_value.string_val;
            if (str && X) {
                XrString *s = xr_compile_time_intern(X, str, strlen(str));
                if (s) {
                    return xr_string_value(s);
                }
            }
            return xr_null();
        }

        default:
            // Complex expressions need runtime evaluation
            return xr_null();
    }
}

/*
 * Count instance fields
 */
static uint32_t count_instance_fields(ClassDeclNode *node) {
    uint32_t count = 0;
    for (int i = 0; i < node->field_count; i++) {
        if (node->fields[i]->type != AST_FIELD_DECL)
            continue;
        FieldDeclNode *field = &node->fields[i]->as.field_decl;
        if (!field->is_static) {
            count++;
        }
    }
    return count;
}

/*
 * Count instance methods
 */
static uint32_t count_instance_methods(ClassDeclNode *node) {
    uint32_t count = 0;
    for (int i = 0; i < node->method_count; i++) {
        if (node->methods[i]->type != AST_METHOD_DECL)
            continue;
        MethodDeclNode *method = &node->methods[i]->as.method_decl;
        if (!method->is_static) {
            count++;
        }
    }
    return count;
}

/*
 * Count static fields
 */
static uint32_t count_static_fields(ClassDeclNode *node) {
    uint32_t count = 0;
    for (int i = 0; i < node->field_count; i++) {
        if (node->fields[i]->type != AST_FIELD_DECL)
            continue;
        FieldDeclNode *field = &node->fields[i]->as.field_decl;
        if (field->is_static) {
            count++;
        }
    }
    return count;
}

/*
 * Count static methods
 */
static uint32_t count_static_methods(ClassDeclNode *node) {
    uint32_t count = 0;
    for (int i = 0; i < node->method_count; i++) {
        if (node->methods[i]->type != AST_METHOD_DECL)
            continue;
        MethodDeclNode *method = &node->methods[i]->as.method_decl;
        // Only count static methods, exclude static constructor (handled separately)
        if (method->is_static && !method->is_static_constructor) {
            count++;
        }
    }
    return count;
}

/*
 * Collect instance fields
 */
static XrFieldDescriptorEntry *collect_instance_fields_mvp(XrayIsolate *X, ClassDeclNode *node,
                                                           uint32_t *count_out) {
    uint32_t count = count_instance_fields(node);
    *count_out = count;

    if (count == 0)
        return NULL;

    // Allocate array
    XrFieldDescriptorEntry *fields =
        (XrFieldDescriptorEntry *) xr_malloc(count * sizeof(XrFieldDescriptorEntry));
    if (!fields)
        return NULL;

    // Fill field info
    uint32_t idx = 0;
    for (int i = 0; i < node->field_count; i++) {
        if (node->fields[i]->type != AST_FIELD_DECL)
            continue;
        FieldDeclNode *field = &node->fields[i]->as.field_decl;

        if (field->is_static)
            continue;  // Skip static fields

        // Compile-time error detection: duplicate field definition
        for (uint32_t j = 0; j < idx; j++) {
            if (strcmp(fields[j].name, field->name) == 0) {
                xr_log_warning("compiler", "duplicate field '%s' in class", field->name);
                xr_free(fields);
                *count_out = 0;
                return NULL;
            }
        }

        fields[idx].name = strdup(field->name);
        fields[idx].type_name = field->field_type ? xr_type_to_string(field->field_type) : NULL;
        fields[idx].flags = 0;

        if (field->is_private) {
            fields[idx].flags |= XR_FIELD_PRIVATE;
        }
        if (field->is_final) {
            fields[idx].flags |= XR_FIELD_FINAL;
        }

        // Field default value: supports simple literals (null, bool, int, float, string)
        fields[idx].default_value = ast_literal_to_value(X, field->initializer);

        idx++;
    }

    return fields;
}

/*
 * Collect instance methods
 */
static XrMethodDescriptorEntry *collect_instance_methods_mvp(ClassDeclNode *node,
                                                             uint32_t *count_out) {
    uint32_t count = count_instance_methods(node);
    *count_out = count;

    if (count == 0)
        return NULL;

    // Allocate array
    XrMethodDescriptorEntry *methods =
        (XrMethodDescriptorEntry *) xr_malloc(count * sizeof(XrMethodDescriptorEntry));
    if (!methods)
        return NULL;

    // Fill method info
    uint32_t idx = 0;
    for (int i = 0; i < node->method_count; i++) {
        if (node->methods[i]->type != AST_METHOD_DECL)
            continue;
        MethodDeclNode *method = &node->methods[i]->as.method_decl;

        if (method->is_static)
            continue;  // Skip static methods

        // Compile-time error detection: duplicate method definition
        for (uint32_t j = 0; j < idx; j++) {
            if (strcmp(methods[j].name, method->name) == 0) {
                xr_log_warning("compiler", "duplicate method '%s' in class", method->name);
                xr_free(methods);
                *count_out = 0;
                return NULL;
            }
        }

        methods[idx].name = strdup(method->name);
        methods[idx].return_type_name =
            method->return_type ? xr_type_to_string(method->return_type) : NULL;
        methods[idx].param_count = method->param_count;
        methods[idx].flags = 0;
        methods[idx].is_operator = method->is_operator;
        methods[idx].op_type = method->op_type;

        if (method->is_private) {
            methods[idx].flags |= XMETHOD_FLAG_PRIVATE;
        }
        if (method->is_constructor || strcmp(method->name, XR_KEYWORD_CONSTRUCTOR) == 0) {
            methods[idx].flags |= XMETHOD_FLAG_CONSTRUCTOR;
        }
        if (method->is_abstract) {
            methods[idx].flags |= XMETHOD_FLAG_ABSTRACT;
        }
        if (method->is_final) {
            methods[idx].flags |= XMETHOD_FLAG_FINAL;
        }

        // closure_index set to 0, updated later during compilation
        methods[idx].closure_index = 0;
        methods[idx].param_type_names = NULL;  // XrType not converted yet

        idx++;
    }

    return methods;
}

/*
 * Collect static fields
 */
static XrFieldDescriptorEntry *collect_static_fields(XrayIsolate *X, ClassDeclNode *node,
                                                     uint32_t *count_out) {
    uint32_t count = count_static_fields(node);
    *count_out = count;

    if (count == 0)
        return NULL;

    // Allocate array
    XrFieldDescriptorEntry *fields =
        (XrFieldDescriptorEntry *) xr_malloc(count * sizeof(XrFieldDescriptorEntry));
    if (!fields)
        return NULL;

    // Fill field info
    uint32_t idx = 0;
    for (int i = 0; i < node->field_count; i++) {
        if (node->fields[i]->type != AST_FIELD_DECL)
            continue;
        FieldDeclNode *field = &node->fields[i]->as.field_decl;

        if (!field->is_static)
            continue;  // Only collect static fields

        fields[idx].name = strdup(field->name);
        fields[idx].type_name = field->field_type ? xr_type_to_string(field->field_type) : NULL;
        fields[idx].flags = XR_FIELD_STATIC;

        if (field->is_private) {
            fields[idx].flags |= XR_FIELD_PRIVATE;
        }
        if (field->is_final) {
            fields[idx].flags |= XR_FIELD_FINAL;
        }

        // Static field default value: supports simple literals
        fields[idx].default_value = ast_literal_to_value(X, field->initializer);

        idx++;
    }

    return fields;
}

/*
 * Collect static methods
 */
static XrMethodDescriptorEntry *collect_static_methods(ClassDeclNode *node, uint32_t *count_out) {
    uint32_t count = count_static_methods(node);
    *count_out = count;

    if (count == 0)
        return NULL;

    // Allocate array
    XrMethodDescriptorEntry *methods =
        (XrMethodDescriptorEntry *) xr_malloc(count * sizeof(XrMethodDescriptorEntry));
    if (!methods)
        return NULL;

    // Fill method info
    uint32_t idx = 0;
    for (int i = 0; i < node->method_count; i++) {
        if (node->methods[i]->type != AST_METHOD_DECL)
            continue;
        MethodDeclNode *method = &node->methods[i]->as.method_decl;

        // Only collect static methods, but exclude static constructor (handled separately)
        if (!method->is_static || method->is_static_constructor)
            continue;

        methods[idx].name = strdup(method->name);
        methods[idx].return_type_name =
            method->return_type ? xr_type_to_string(method->return_type) : NULL;
        methods[idx].param_count = method->param_count;
        methods[idx].flags = XMETHOD_FLAG_STATIC;
        methods[idx].is_operator = method->is_operator;
        methods[idx].op_type = method->op_type;

        if (method->is_private) {
            methods[idx].flags |= XMETHOD_FLAG_PRIVATE;
        }
        if (method->is_final) {
            methods[idx].flags |= XMETHOD_FLAG_FINAL;
        }

        // closure_index set to 0, updated later during compilation
        methods[idx].closure_index = 0;
        methods[idx].param_type_names = NULL;  // XrType not converted yet

        idx++;
    }

    return methods;
}

// ========== Main Functions ==========

/*
 * Create ClassDescriptor
 */
XrClassDescriptor *xoop_create_class_descriptor(XrCompilerContext *ctx, XrCompiler *compiler,
                                                ClassDeclNode *node) {
    // Check getter/setter name conflict with fields

    for (int m = 0; m < node->method_count; m++) {
        AstNode *method_node = node->methods[m];
        if (method_node->type != AST_METHOD_DECL)
            continue;

        MethodDeclNode *method = &method_node->as.method_decl;
        if (!method->is_getter && !method->is_setter)
            continue;

        // Extract actual property name (remove "get:" or "set:" prefix)
        const char *prop_name = method->name;
        if (method->is_getter && strncmp(method->name, "get:", 4) == 0) {
            prop_name = method->name + 4;
        } else if (method->is_setter && strncmp(method->name, "set:", 4) == 0) {
            prop_name = method->name + 4;
        }

        // Check if same name as field
        for (int f = 0; f < node->field_count; f++) {
            AstNode *field_node = node->fields[f];
            // Field may be AST_FIELD_DECL or AST_VAR_DECL
            const char *field_name = NULL;
            if (field_node->type == AST_FIELD_DECL) {
                field_name = field_node->as.field_decl.name;
            } else if (field_node->type == AST_VAR_DECL) {
                field_name = field_node->as.var_decl.name;
            }

            if (field_name && strcmp(prop_name, field_name) == 0) {
                const char *kind = method->is_getter ? "getter" : "setter";
                xr_compiler_error(ctx, compiler,
                                  "class '%s' %s '%s' has same name as field, use different name "
                                  "(suggest field with '_%s' prefix)",
                                  node->name, kind, prop_name, prop_name);
                return NULL;
            }
        }
    }

    // Allocate descriptor
    XrClassDescriptor *desc = (XrClassDescriptor *) xr_malloc(sizeof(XrClassDescriptor));
    if (!desc) {
        xr_log_warning("class", "failed to allocate descriptor");
        return NULL;
    }

    // Basic info (strdup all strings - AST may be freed before execution)
    desc->class_name = strdup(node->name);
    /*
     * For `extends module.Class` (module member access) we leave
     * super_name as NULL: the runtime OP_CLASS_NEW path will supply
     * the real parent through xr_class_from_descriptor's
     * super_override argument, so the descriptor itself stays neutral
     * and xr_class_from_descriptor defaults its super to Object if no
     * override is provided.
     */
    desc->super_name =
        (node->super_module != NULL || !node->super_name) ? NULL : strdup(node->super_name);
    desc->super_global_index = -1;  // Default -1 (no parent or unknown)
    desc->flags = 0;
    if (node->is_abstract)
        desc->flags |= XR_CLASS_ABSTRACT;
    if (node->is_final)
        desc->flags |= XR_CLASS_FINAL;
    if (ctx->is_compiling_struct)
        desc->flags |= XR_CLASS_VALUE_TYPE;
    desc->descriptor_version = XR_CLASS_DESCRIPTOR_VERSION;
    desc->checksum = 0;

    // Collect instance fields
    desc->instance_fields = collect_instance_fields_mvp(ctx->X, node, &desc->instance_field_count);

    // Analyze flat_copyable: all fields must be primitive or string
    if (ctx->is_compiling_struct && desc->instance_fields) {
        bool flat = true;
        for (uint32_t fi = 0; fi < desc->instance_field_count; fi++) {
            const char *tn = desc->instance_fields[fi].type_name;
            if (!tn || (strcmp(tn, "int") != 0 && strcmp(tn, "float") != 0 &&
                        strcmp(tn, "bool") != 0 && strcmp(tn, "string") != 0)) {
                flat = false;
                break;
            }
        }
        if (flat)
            desc->flags |= XR_CLASS_FLAT_COPYABLE;
    }

    // Set struct_layout from analyzer (for native struct storage)
    desc->struct_layout = NULL;
    if (ctx->is_compiling_struct && ctx->analyzer) {
        XaSymbol *xa_sym = xa_analyzer_lookup(ctx->analyzer, node->name);
        if (xa_sym) {
            XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, xa_sym);
            if (links && links->class_info && links->class_info->struct_layout) {
                desc->struct_layout = links->class_info->struct_layout;
            }
        }
    }

    // Collect instance methods
    desc->instance_methods = collect_instance_methods_mvp(node, &desc->instance_method_count);

    // Collect static fields
    desc->static_fields = collect_static_fields(ctx->X, node, &desc->static_field_count);

    // Collect static methods
    desc->static_methods = collect_static_methods(node, &desc->static_method_count);

    // Collect interfaces (store pointers directly)
    if (node->interface_count > 0) {
        desc->interfaces = (XrInterfaceDescriptorEntry *) xr_malloc(
            node->interface_count * sizeof(XrInterfaceDescriptorEntry));
        if (desc->interfaces) {
            for (int i = 0; i < node->interface_count; i++) {
                const char *iface_name = node->interfaces[i];

                // Interface pointer filled in compile_class_with_descriptor
                // Set to NULL here as interface may not be compiled yet
                desc->interfaces[i].interface_ptr = NULL;
                desc->interfaces[i].interface_name = iface_name;
            }
            desc->interface_count = (uint8_t) node->interface_count;
        } else {
            desc->interface_count = 0;
        }
    } else {
        desc->interfaces = NULL;
        desc->interface_count = 0;
    }

    // Abstract methods not supported
    desc->abstract_method_names = NULL;
    desc->abstract_method_count = 0;

    return desc;
}

/*
 * Add ClassDescriptor to constant pool
 */
int xoop_add_descriptor_to_constant_pool(XrProto *proto, XrClassDescriptor *desc) {
    if (!proto || !desc)
        return -1;

    // Wrap descriptor pointer as XrValue
    XrValue desc_val = XR_FROM_PTR(desc);

    // Add to constant pool
    return xr_valuearray_add(&proto->constants, desc_val);
}

/*
 * Free ClassDescriptor
 */
void xoop_free_class_descriptor(XrClassDescriptor *desc) {
    if (!desc)
        return;

    // Free strdup'd strings
    if (desc->class_name)
        xr_free((void *) desc->class_name);
    if (desc->super_name)
        xr_free((void *) desc->super_name);

    // Free field/method name strings
    for (uint32_t i = 0; i < desc->instance_field_count; i++) {
        if (desc->instance_fields[i].name)
            xr_free((void *) desc->instance_fields[i].name);
    }
    for (uint32_t i = 0; i < desc->static_field_count; i++) {
        if (desc->static_fields[i].name)
            xr_free((void *) desc->static_fields[i].name);
    }
    for (uint32_t i = 0; i < desc->instance_method_count; i++) {
        if (desc->instance_methods[i].name)
            xr_free((void *) desc->instance_methods[i].name);
    }
    for (uint32_t i = 0; i < desc->static_method_count; i++) {
        if (desc->static_methods[i].name)
            xr_free((void *) desc->static_methods[i].name);
    }

    if (desc->instance_fields)
        xr_free(desc->instance_fields);
    if (desc->static_fields)
        xr_free(desc->static_fields);
    if (desc->instance_methods)
        xr_free(desc->instance_methods);
    if (desc->static_methods)
        xr_free(desc->static_methods);
    if (desc->interfaces)
        xr_free(desc->interfaces);
    if (desc->abstract_method_names)
        xr_free(desc->abstract_method_names);

    xr_free(desc);
}
