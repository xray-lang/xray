/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xoop_enum.c - Enum declaration compiler
 *
 * KEY CONCEPT:
 *   Compiles enum declarations with compile-time constant evaluation,
 *   type inference, and duplicate value detection.
 */

#include "xoop.h"
#include "../../base/xchecks.h"
#include "xcompiler.h"
#include "xcompiler_context.h"
#include "xregalloc.h"
#include "xemit.h"
#include "../../runtime/object/xstring.h"
#include "../../runtime/class/xenum.h"
#include "../../base/xmalloc.h"
#include "../../runtime/value/xtype_names.h"
#include <stdio.h>
#include <string.h>

// Evaluate compile-time constant expression
static XrValue eval_const_expr(XrCompilerContext *ctx, XrCompiler *compiler, AstNode *expr) {
    if (expr == NULL) {
        return xr_null();
    }

    switch (expr->type) {
        case AST_LITERAL_INT:
            return xr_int(expr->as.literal.raw_value.int_val);

        case AST_LITERAL_FLOAT:
            return xr_float(expr->as.literal.raw_value.float_val);

        case AST_LITERAL_STRING: {
            const char *str = expr->as.literal.raw_value.string_val;
            XrString *xstr = xr_compile_time_intern(ctx->X, str, strlen(str));
            return xr_string_value(xstr);
        }

        case AST_LITERAL_TRUE:
            return xr_bool(true);
        case AST_LITERAL_FALSE:
            return xr_bool(false);

        default:
            xr_compiler_error(
                ctx, compiler,
                "Enum value must be compile-time constant (int/string/float/bool literal)");
            return xr_null();
    }
}

static XrTypeId type_hint_to_typeid(const char *type_hint) {
    if (type_hint == NULL) {
        return XR_TID_INT;
    }

    if (strcmp(type_hint, TYPE_NAME_INT) == 0) {
        return XR_TID_INT;
    } else if (strcmp(type_hint, TYPE_NAME_STRING) == 0) {
        return XR_TID_STRING;
    } else if (strcmp(type_hint, TYPE_NAME_FLOAT) == 0) {
        return XR_TID_FLOAT;
    } else if (strcmp(type_hint, TYPE_NAME_BOOL) == 0) {
        return XR_TID_BOOL;
    }

    return XR_TID_INT;
}

// Compare values for duplicate detection (uses string interning for strings)
static bool values_equal(XrValue a, XrValue b) {
    XrTypeId tid_a = xr_value_typeid(a);
    XrTypeId tid_b = xr_value_typeid(b);

    if (tid_a != tid_b) {
        return false;
    }

    if (tid_a == XR_TID_NULL)
        return true;
    if (tid_a == XR_TID_BOOL)
        return XR_TO_BOOL(a) == XR_TO_BOOL(b);
    if (XR_TID_IS_INT(tid_a))
        return XR_TO_INT(a) == XR_TO_INT(b);
    if (XR_TID_IS_FLOAT(tid_a))
        return XR_TO_FLOAT(a) == XR_TO_FLOAT(b);
    if (tid_a == XR_TID_STRING)
        return XR_TO_STRING(a) == XR_TO_STRING(b);
    return false;
}

void compile_enum_decl(XrCompilerContext *ctx, XrCompiler *compiler, EnumDeclNode *node) {
    XR_DCHECK(ctx != NULL, "compile_enum_decl: NULL ctx");
    XR_DCHECK(compiler != NULL, "compile_enum_decl: NULL compiler");
    XR_DCHECK(node != NULL, "compile_enum_decl: NULL node");
    // Step 1: Determine base type
    XrTypeId base_type = XR_TID_INT;

    if (node->type_hint) {
        base_type = type_hint_to_typeid(node->type_hint);
    } else {
        // Type inference from first valued member
        for (int i = 0; i < node->member_count; i++) {
            AstNode *member = node->members[i];
            if (member->as.enum_member.value) {
                XrValue val = eval_const_expr(ctx, compiler, member->as.enum_member.value);
                base_type = xr_value_typeid(val);
                break;
            }
        }
    }

    // Step 2: Allocate member arrays
    char **member_names = (char **) xr_malloc(sizeof(char *) * node->member_count);
    XrValue *member_values = (XrValue *) xr_malloc(sizeof(XrValue) * node->member_count);

    xr_Integer auto_int_value = 0;

    // Duplicate value detection table
    typedef struct {
        XrValue value;
        bool used;
    } ValueEntry;

    ValueEntry *seen_values = (ValueEntry *) xr_malloc(sizeof(ValueEntry) * node->member_count);
    int seen_count = 0;

    // Step 3: Process each member
    for (int i = 0; i < node->member_count; i++) {
        AstNode *member = node->members[i];
        const char *member_name = member->as.enum_member.name;
        XrValue member_value;

        if (member->as.enum_member.value) {
            member_value = eval_const_expr(ctx, compiler, member->as.enum_member.value);

            // Type check: value type must match base type
            if (xr_value_typeid(member_value) != base_type) {
                xr_compiler_error(ctx, compiler,
                                  "Enum member '%s' type mismatch: expected %s, got %s",
                                  member_name, xr_typeid_name(base_type),
                                  xr_typeid_name(xr_value_typeid(member_value)));
                continue;
            }

            // Update auto-increment value
            if (base_type == XR_TID_INT) {
                auto_int_value = XR_TO_INT(member_value) + 1;
            }
        } else {
            // Auto-assign (only int type supports auto-increment)
            if (base_type != XR_TID_INT) {
                xr_compiler_error(ctx, compiler,
                                  "Enum member '%s' requires explicit value (only int enums "
                                  "support auto-increment)",
                                  member_name);
                member_value = xr_null();
            } else {
                member_value = xr_int(auto_int_value);
                auto_int_value++;
            }
        }

        // Duplicate value detection
        for (int j = 0; j < seen_count; j++) {
            if (seen_values[j].used && values_equal(seen_values[j].value, member_value)) {
                xr_compiler_error(ctx, compiler, "Enum member '%s' has duplicate value",
                                  member_name);
                break;
            }
        }

        seen_values[seen_count].value = member_value;
        seen_values[seen_count].used = true;
        seen_count++;

        member_names[i] = strdup(member_name);
        member_values[i] = member_value;
    }

    xr_free(seen_values);

    // Create enum type object
    XrEnumType *enum_type_obj = xr_enum_type_new(ctx->X, node->name, base_type, member_names,
                                                 member_values, node->member_count);

    // Register enum type name for type inference
    xr_compiler_ctx_register_enum_type(ctx, node->name);

    // Step 4: Store enum type object as top-level variable
    XrString *enum_name_str = xr_compile_time_intern(ctx->X, node->name, strlen(node->name));
    int shared_index = shared_get_or_add(ctx, compiler, enum_name_str);
    shared_set_const(ctx, shared_index, true);

    XrValue enum_val = XR_FROM_PTR(enum_type_obj);
    int enum_const_idx = xr_vm_proto_add_constant(compiler->proto, enum_val);

    // Emit bytecode: load constant and store to shared
    int enum_reg = reg_alloc(ctx, compiler);
    xemit_loadk((compiler)->emitter, enum_reg, enum_const_idx);
    xemit_setshared((compiler)->emitter, enum_reg, shared_index);
    reg_free(compiler, enum_reg);
}
