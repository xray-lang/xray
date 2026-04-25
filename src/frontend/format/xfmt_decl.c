/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xfmt_decl.c - Declaration formatting
 *
 * KEY CONCEPT:
 *   Declarations: var / multi-var / destructure / function / class /
 *   struct / interface / enum / type alias. Each function emits a full
 *   trailing newline so the statement-level dispatcher does not need
 *   to add one.
 */

#include "xfmt_internal.h"
#include "../../runtime/value/xtype_names.h"

void xfmt_emit_var_decl(XrFmtContext *ctx, AstNode *node) {
    xfmt_write_indent(ctx);
    VarDeclNode *decl = &node->as.var_decl;

    if (decl->storage_mode == XR_STORAGE_SHARED) {
        xfmt_write_str(ctx, "shared ");
    }
    xfmt_write_str(ctx, decl->is_const ? "const " : "let ");
    xfmt_write_str(ctx, decl->name);

    if (decl->type_annotation) {
        xfmt_write_str(ctx, ": ");
        xfmt_emit_type(ctx, decl->type_annotation);
    }
    if (decl->initializer) {
        xfmt_write_str(ctx, " = ");
        xfmt_emit_expression(ctx, decl->initializer);
    }
    xfmt_write_newline(ctx);
}

void xfmt_emit_multi_var_decl(XrFmtContext *ctx, AstNode *node) {
    xfmt_write_indent(ctx);
    MultiVarDeclNode *decl = &node->as.multi_var_decl;

    xfmt_write_str(ctx, decl->is_const ? "const " : "let ");
    for (int i = 0; i < decl->name_count; i++) {
        if (i > 0) xfmt_write_str(ctx, ", ");
        xfmt_write_str(ctx, decl->names[i]);
    }
    xfmt_write_str(ctx, " = ");
    for (int i = 0; i < decl->value_count; i++) {
        if (i > 0) xfmt_write_str(ctx, ", ");
        xfmt_emit_expression(ctx, decl->values[i]);
    }
    xfmt_write_newline(ctx);
}

void xfmt_emit_destructure_decl(XrFmtContext *ctx, AstNode *node) {
    xfmt_write_indent(ctx);
    DestructureDeclNode *decl = &node->as.destructure_decl;

    xfmt_write_str(ctx, decl->is_const ? "const " : "let ");
    xfmt_emit_pattern(ctx, decl->pattern);
    xfmt_write_str(ctx, " = ");
    xfmt_emit_expression(ctx, decl->initializer);
    xfmt_write_newline(ctx);
}

void xfmt_emit_function_decl(XrFmtContext *ctx, AstNode *node) {
    xfmt_write_indent(ctx);
    FunctionDeclNode *fn = &node->as.function_decl;

    xfmt_write_str(ctx, "fn ");
    xfmt_write_str(ctx, fn->name);
    xfmt_emit_generic_params(ctx, fn->type_params, fn->type_param_count);

    xfmt_write_char(ctx, '(');
    for (int i = 0; i < fn->param_count; i++) {
        if (i > 0) xfmt_write_str(ctx, ", ");
        XrParamNode *param = fn->params[i];
        xfmt_write_str(ctx, param->name);
        if (param->type) {
            xfmt_write_str(ctx, ": ");
            xfmt_emit_type(ctx, param->type);
        }
        if (param->default_value) {
            xfmt_write_str(ctx, " = ");
            xfmt_emit_expression(ctx, param->default_value);
        }
    }
    xfmt_write_char(ctx, ')');

    if (fn->return_type) {
        xfmt_write_str(ctx, ": ");
        xfmt_emit_type(ctx, fn->return_type);
    }

    xfmt_write_space(ctx);
    xfmt_emit_block(ctx, fn->body);
    xfmt_write_newline(ctx);
}

void xfmt_emit_class_decl(XrFmtContext *ctx, AstNode *node) {
    xfmt_write_indent(ctx);
    ClassDeclNode *cls = &node->as.class_decl;

    if (cls->is_abstract) xfmt_write_str(ctx, "abstract ");
    xfmt_write_str(ctx, "class ");
    xfmt_write_str(ctx, cls->name);
    xfmt_emit_generic_params(ctx, cls->type_params, cls->type_param_count);

    if (cls->super_name) {
        xfmt_write_str(ctx, " extends ");
        if (cls->super_module) {
            xfmt_write_str(ctx, cls->super_module);
            xfmt_write_char(ctx, '.');
        }
        xfmt_write_str(ctx, cls->super_name);
    }

    if (cls->interface_count > 0) {
        xfmt_write_str(ctx, " implements ");
        for (int i = 0; i < cls->interface_count; i++) {
            if (i > 0) xfmt_write_str(ctx, ", ");
            xfmt_write_str(ctx, cls->interfaces[i]);
        }
    }

    xfmt_write_str(ctx, " {");
    xfmt_write_newline(ctx);
    ctx->indent_level++;

    // Fields
    for (int i = 0; i < cls->field_count; i++) {
        AstNode *field = cls->fields[i];
        FieldDeclNode *f = &field->as.field_decl;

        xfmt_write_indent(ctx);
        if (f->is_private) xfmt_write_str(ctx, "private ");
        if (f->is_static) xfmt_write_str(ctx, "static ");
        xfmt_write_str(ctx, f->name);
        if (f->field_type) {
            xfmt_write_str(ctx, ": ");
            xfmt_emit_type(ctx, f->field_type);
        }
        if (f->initializer) {
            xfmt_write_str(ctx, " = ");
            xfmt_emit_expression(ctx, f->initializer);
        }
        xfmt_write_newline(ctx);
    }

    if (cls->field_count > 0 && cls->method_count > 0) {
        xfmt_write_newline(ctx);
    }

    // Methods
    for (int i = 0; i < cls->method_count; i++) {
        AstNode *method = cls->methods[i];
        MethodDeclNode *m = &method->as.method_decl;

        xfmt_write_indent(ctx);
        if (m->is_private) xfmt_write_str(ctx, "private ");
        if (m->is_static) xfmt_write_str(ctx, "static ");
        if (m->is_abstract) xfmt_write_str(ctx, "abstract ");
        if (m->is_getter) xfmt_write_str(ctx, "get ");
        if (m->is_setter) xfmt_write_str(ctx, "set ");

        if (m->is_constructor) {
            xfmt_write_str(ctx, XR_KEYWORD_CONSTRUCTOR);
        } else {
            xfmt_write_str(ctx, m->name);
        }

        if (m->type_param_count > 0) {
            xfmt_write_char(ctx, '<');
            for (int j = 0; j < m->type_param_count; j++) {
                if (j > 0) xfmt_write_str(ctx, ", ");
                xfmt_write_str(ctx, m->type_param_names[j]);
            }
            xfmt_write_char(ctx, '>');
        }

        xfmt_write_char(ctx, '(');
        for (int j = 0; j < m->param_count; j++) {
            if (j > 0) xfmt_write_str(ctx, ", ");
            xfmt_write_str(ctx, m->parameters[j]);
            if (m->param_types && m->param_types[j]) {
                xfmt_write_str(ctx, ": ");
                xfmt_emit_type(ctx, m->param_types[j]);
            }
        }
        xfmt_write_char(ctx, ')');

        if (m->return_type) {
            xfmt_write_str(ctx, ": ");
            xfmt_emit_type(ctx, m->return_type);
        }

        if (m->is_abstract) {
            xfmt_write_newline(ctx);
        } else if (m->body) {
            xfmt_write_space(ctx);
            xfmt_emit_block(ctx, m->body);
            xfmt_write_newline(ctx);
        }

        if (i < cls->method_count - 1) {
            xfmt_write_newline(ctx);
        }
    }

    ctx->indent_level--;
    xfmt_write_indent(ctx);
    xfmt_write_char(ctx, '}');
    xfmt_write_newline(ctx);
}

void xfmt_emit_interface_decl(XrFmtContext *ctx, AstNode *node) {
    xfmt_write_indent(ctx);
    InterfaceDeclNode *iface = &node->as.interface_decl;

    xfmt_write_str(ctx, "interface ");
    xfmt_write_str(ctx, iface->name);

    if (iface->extends_count > 0) {
        xfmt_write_str(ctx, " extends ");
        for (int i = 0; i < iface->extends_count; i++) {
            if (i > 0) xfmt_write_str(ctx, ", ");
            xfmt_write_str(ctx, iface->extends[i]);
        }
    }

    xfmt_write_str(ctx, " {");
    xfmt_write_newline(ctx);
    ctx->indent_level++;

    for (int i = 0; i < iface->method_count; i++) {
        AstNode *method = iface->methods[i];
        InterfaceMethodNode *m = &method->as.interface_method;

        xfmt_write_indent(ctx);
        xfmt_write_str(ctx, m->name);
        xfmt_write_char(ctx, '(');
        for (int j = 0; j < m->param_count; j++) {
            if (j > 0) xfmt_write_str(ctx, ", ");
            xfmt_write_str(ctx, m->parameters[j]);
            if (m->param_types && m->param_types[j]) {
                xfmt_write_str(ctx, ": ");
                xfmt_emit_type(ctx, m->param_types[j]);
            }
        }
        xfmt_write_char(ctx, ')');
        if (m->return_type) {
            xfmt_write_str(ctx, ": ");
            xfmt_emit_type(ctx, m->return_type);
        }
        xfmt_write_newline(ctx);
    }

    ctx->indent_level--;
    xfmt_write_indent(ctx);
    xfmt_write_char(ctx, '}');
    xfmt_write_newline(ctx);
}

void xfmt_emit_enum_decl(XrFmtContext *ctx, AstNode *node) {
    xfmt_write_indent(ctx);
    EnumDeclNode *en = &node->as.enum_decl;

    xfmt_write_str(ctx, "enum ");
    xfmt_write_str(ctx, en->name);
    if (en->type_hint) {
        xfmt_write_str(ctx, ": ");
        xfmt_write_str(ctx, en->type_hint);
    }
    xfmt_write_str(ctx, " {");
    xfmt_write_newline(ctx);
    ctx->indent_level++;

    for (int i = 0; i < en->member_count; i++) {
        AstNode *member = en->members[i];
        EnumMemberNode *m = &member->as.enum_member;

        xfmt_write_indent(ctx);
        xfmt_write_str(ctx, m->name);
        if (m->value) {
            xfmt_write_str(ctx, " = ");
            xfmt_emit_expression(ctx, m->value);
        }
        if (i < en->member_count - 1) {
            xfmt_write_char(ctx, ',');
        }
        xfmt_write_newline(ctx);
    }

    ctx->indent_level--;
    xfmt_write_indent(ctx);
    xfmt_write_char(ctx, '}');
    xfmt_write_newline(ctx);
}

void xfmt_emit_type_alias(XrFmtContext *ctx, AstNode *node) {
    xfmt_write_indent(ctx);
    TypeAliasNode *ta = &node->as.type_alias;

    xfmt_write_str(ctx, "type ");
    xfmt_write_str(ctx, ta->name);
    xfmt_write_str(ctx, " = { ");

    for (int i = 0; i < ta->field_count; i++) {
        if (i > 0) xfmt_write_str(ctx, ", ");
        xfmt_write_str(ctx, ta->field_names[i]);
        if (ta->field_optional && ta->field_optional[i]) {
            xfmt_write_char(ctx, '?');
        }
        xfmt_write_str(ctx, ": ");
        xfmt_emit_type(ctx, ta->field_types[i]);
    }

    xfmt_write_str(ctx, " }");
    xfmt_write_newline(ctx);
}
