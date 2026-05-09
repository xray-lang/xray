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
#include <string.h>

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
        if (i > 0)
            xfmt_write_str(ctx, ", ");
        xfmt_write_str(ctx, decl->names[i]);
    }
    xfmt_write_str(ctx, " = ");
    for (int i = 0; i < decl->value_count; i++) {
        if (i > 0)
            xfmt_write_str(ctx, ", ");
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
        if (i > 0)
            xfmt_write_str(ctx, ", ");
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

    if (cls->is_abstract)
        xfmt_write_str(ctx, "abstract ");
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
            if (i > 0)
                xfmt_write_str(ctx, ", ");
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
        if (f->is_private)
            xfmt_write_str(ctx, "private ");
        if (f->is_static)
            xfmt_write_str(ctx, "static ");
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

    // Methods — getter/setter pairs are emitted as property accessor syntax:
    //   propname: type { fn() { ... } fn(v: type) { ... } }
    for (int i = 0; i < cls->method_count; i++) {
        AstNode *method = cls->methods[i];
        MethodDeclNode *m = &method->as.method_decl;

        // Property accessor: emit getter/setter pair in fn() block syntax
        if (m->is_getter || m->is_setter) {
            // Extract base property name (strip "get:" or "set:" prefix)
            const char *prop_name = m->name;
            if (prop_name &&
                (strncmp(prop_name, "get:", 4) == 0 || strncmp(prop_name, "set:", 4) == 0)) {
                prop_name = prop_name + 4;
            }

            // Find the matching setter (if we are the getter) or skip if
            // we already emitted this property from a preceding getter
            MethodDeclNode *getter = m->is_getter ? m : NULL;
            MethodDeclNode *setter = m->is_setter ? m : NULL;
            int pair_idx = -1;

            for (int j = i + 1; j < cls->method_count; j++) {
                MethodDeclNode *other = &cls->methods[j]->as.method_decl;
                if ((other->is_getter || other->is_setter) && other->name) {
                    const char *oname = other->name;
                    if (strncmp(oname, "get:", 4) == 0 || strncmp(oname, "set:", 4) == 0)
                        oname = oname + 4;
                    if (strcmp(oname, prop_name) == 0) {
                        if (other->is_getter)
                            getter = other;
                        if (other->is_setter)
                            setter = other;
                        pair_idx = j;
                        break;
                    }
                }
            }

            // If this is a setter and its getter already emitted it, skip
            if (m->is_setter && i > 0) {
                int already_emitted = 0;
                for (int j = 0; j < i; j++) {
                    MethodDeclNode *prev = &cls->methods[j]->as.method_decl;
                    if (prev->is_getter && prev->name) {
                        const char *pname = prev->name;
                        if (strncmp(pname, "get:", 4) == 0)
                            pname = pname + 4;
                        if (strcmp(pname, prop_name) == 0) {
                            already_emitted = 1;
                            break;
                        }
                    }
                }
                if (already_emitted) {
                    if (i < cls->method_count - 1)
                        xfmt_write_newline(ctx);
                    continue;
                }
            }

            xfmt_write_indent(ctx);
            if (m->is_private)
                xfmt_write_str(ctx, "private ");
            if (m->is_static)
                xfmt_write_str(ctx, "static ");
            xfmt_write_str(ctx, prop_name);

            // Property type from getter return type or setter param type
            XrTypeRef *prop_type = getter ? getter->return_type : NULL;
            if (!prop_type && setter && setter->param_count > 0 && setter->param_types)
                prop_type = setter->param_types[0];
            if (prop_type) {
                xfmt_write_str(ctx, ": ");
                xfmt_emit_type(ctx, prop_type);
            }

            xfmt_write_str(ctx, " {");
            xfmt_write_newline(ctx);
            ctx->indent_level++;

            // Emit getter fn
            if (getter && getter->body) {
                xfmt_write_indent(ctx);
                xfmt_write_str(ctx, "fn() ");
                xfmt_emit_block(ctx, getter->body);
                xfmt_write_newline(ctx);
            }

            // Emit setter fn
            if (setter && setter->body) {
                xfmt_write_indent(ctx);
                xfmt_write_str(ctx, "fn(");
                for (int j = 0; j < setter->param_count; j++) {
                    if (j > 0)
                        xfmt_write_str(ctx, ", ");
                    xfmt_write_str(ctx, setter->parameters[j]);
                    if (setter->param_types && setter->param_types[j]) {
                        xfmt_write_str(ctx, ": ");
                        xfmt_emit_type(ctx, setter->param_types[j]);
                    }
                }
                xfmt_write_str(ctx, ") ");
                xfmt_emit_block(ctx, setter->body);
                xfmt_write_newline(ctx);
            }

            ctx->indent_level--;
            xfmt_write_indent(ctx);
            xfmt_write_char(ctx, '}');
            xfmt_write_newline(ctx);

            // Skip the paired method index
            if (pair_idx > i) {
                // Mark it so the loop skips — swap with i+1 not feasible,
                // rely on the "already_emitted" check above for the setter
            }

            if (i < cls->method_count - 1)
                xfmt_write_newline(ctx);
            continue;
        }

        xfmt_write_indent(ctx);
        if (m->is_private)
            xfmt_write_str(ctx, "private ");
        if (m->is_static)
            xfmt_write_str(ctx, "static ");
        if (m->is_abstract)
            xfmt_write_str(ctx, "abstract ");

        if (m->is_constructor) {
            xfmt_write_str(ctx, XR_KEYWORD_CONSTRUCTOR);
        } else if (m->is_operator) {
            xfmt_write_str(ctx, "operator");
            xfmt_write_str(ctx, m->name);
        } else {
            xfmt_write_str(ctx, m->name);
        }

        if (m->type_param_count > 0) {
            xfmt_write_char(ctx, '<');
            for (int j = 0; j < m->type_param_count; j++) {
                if (j > 0)
                    xfmt_write_str(ctx, ", ");
                xfmt_write_str(ctx, m->type_param_names[j]);
            }
            xfmt_write_char(ctx, '>');
        }

        xfmt_write_char(ctx, '(');
        for (int j = 0; j < m->param_count; j++) {
            if (j > 0)
                xfmt_write_str(ctx, ", ");
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
            if (i > 0)
                xfmt_write_str(ctx, ", ");
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
            if (j > 0)
                xfmt_write_str(ctx, ", ");
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
        if (i > 0)
            xfmt_write_str(ctx, ", ");
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
