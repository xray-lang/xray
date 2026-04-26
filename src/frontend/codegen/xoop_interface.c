/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xoop_interface.c - Xray interface compiler
 *
 * KEY CONCEPT:
 *   - Compile interface declarations
 *   - Handle interface inheritance
 *   - Register method signatures
 */

#include "xoop.h"
#include "../../base/xchecks.h"
#include "../../runtime/xisolate_api.h"    // XrayIsolate full definition
#include "../../runtime/xglobals_table.h"  // xr_globals_get
#include "xcompiler.h"
#include "xcompiler_context.h"
#include "xregalloc.h"
#include "xemit.h"
#include "../../runtime/object/xstring.h"
#include "../../runtime/class/xclass.h"
#include "../../runtime/class/xclass_builder.h"  // ClassBuilder API
#include "../../runtime/symbol/xsymbol_table.h"  // unified symbol table system
#include <stdio.h>
#include <string.h>

// ========== Public Interface ==========

/*
 * Compile interface definition
 *
 * interface Drawable extends Shape {
 *     draw(): void;
 *     getZIndex(): int;
 * }
 *
 * Uses ClassBuilder to build interface, supports:
 * - Interface inheritance (extends)
 * - Abstract method registration
 */
void compile_interface(XrCompilerContext *ctx, XrCompiler *compiler, InterfaceDeclNode *node) {
    // 1. Create interface using ClassBuilder (no parent class)
    XrClassBuilder *builder = xr_class_builder_new(ctx->X, node->name, NULL);
    if (!builder) {
        xr_compiler_error(ctx, compiler, "cannot create interface builder: %s", node->name);
        return;
    }

    // Mark as interface
    xr_class_builder_set_flags(builder, XR_CLASS_INTERFACE);

    // 2. Handle extends (interface inheritance)
    for (int i = 0; i < node->extends_count; i++) {
        const char *parent_name = node->extends[i];

        // Lookup parent interface (check shared first, then globals)
        XrString *parent_name_str =
            xr_compile_time_intern(ctx->X, parent_name, strlen(parent_name));
        int parent_idx = shared_get_in_scope(ctx, compiler, parent_name_str);
        if (parent_idx < 0) {
            parent_idx = builtin_get(ctx, parent_name_str);
        }

        if (parent_idx == -1) {
            xr_compiler_error(ctx, compiler, "parent interface '%s' not defined", parent_name);
            continue;
        }

        // Find parent interface object from constant pool (created at compile time)
        XrClass *parent_iface = NULL;
        XrProto *proto = compiler->proto;
        for (int k = 0; k < DYNARRAY_COUNT(&proto->constants); k++) {
            XrValue kv = DYNARRAY_GET(&proto->constants, k, XrValue);
            if (xr_value_is_class(kv)) {
                XrClass *cls = xr_value_to_class(kv);
                if (cls->name && strcmp(cls->name, parent_name) == 0) {
                    parent_iface = cls;
                    break;
                }
            }
        }

        if (parent_iface) {
            xr_class_builder_add_interface(builder, parent_iface);
        } else {
            xr_compiler_error(ctx, compiler, "'%s' is not a valid interface", parent_name);
        }
    }

    // 3. Register abstract methods
    for (int i = 0; i < node->method_count; i++) {
        AstNode *method_node = node->methods[i];

        if (method_node->type != AST_INTERFACE_METHOD) {
            continue;
        }

        InterfaceMethodNode *method = &method_node->as.interface_method;

        // Get method symbol
        int method_symbol = xr_symbol_register_in_table(
            (XrSymbolTable *) xr_isolate_get_symbol_table(ctx->X), method->name);

        // Add abstract method using ClassBuilder
        xr_class_builder_add_abstract_method(builder, method_symbol);
    }

    // 4. Finalize build, get interface object
    XrClass *iface = xr_class_builder_finalize(builder);
    if (!iface) {
        xr_compiler_error(ctx, compiler, "interface build failed: %s", node->name);
        return;
    }

    // 5. Store interface object as top-level variable
    XrString *iface_name_str = xr_compile_time_intern(ctx->X, node->name, strlen(node->name));
    int shared_index = shared_get_or_add(ctx, compiler, iface_name_str);
    shared_set_const(ctx, shared_index, true);

    // Wrap interface object as XrValue and add to constant table
    XrValue iface_val = XR_FROM_PTR(iface);
    int iface_const_idx = xr_vm_proto_add_constant(compiler->proto, iface_val);

    // 6. Generate code: load interface constant to register, then store to shared
    int iface_reg = reg_alloc(ctx, compiler);
    xemit_loadk((compiler)->emitter, iface_reg, iface_const_idx);
    xemit_setshared((compiler)->emitter, iface_reg, shared_index);
    reg_free(compiler, iface_reg);
}
