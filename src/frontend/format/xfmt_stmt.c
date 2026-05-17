/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xfmt_stmt.c - Statement, control-flow, block, and program formatting
 *
 * KEY CONCEPT:
 *   xfmt_emit_statement() is the per-node statement dispatcher. It
 *   handles leading-comment output, then routes to the appropriate
 *   helper. Declarations route to xfmt_decl.c; expressions to
 *   xfmt_expr.c. xfmt_emit_program() handles top-level node-level
 *   blank-line layout.
 */

#include "xfmt_internal.h"
#include <string.h>

// ----------------------------------------------------------------------------
// Control-flow statements
// ----------------------------------------------------------------------------

static void fmt_if_stmt(XrFmtContext *ctx, AstNode *node) {
    xfmt_write_indent(ctx);
    xfmt_write_str(ctx, "if (");
    xfmt_emit_expression(ctx, node->as.if_stmt.condition);
    xfmt_write_str(ctx, ") ");
    xfmt_emit_block(ctx, node->as.if_stmt.then_branch);

    if (node->as.if_stmt.else_branch) {
        xfmt_write_str(ctx, " else ");
        if (node->as.if_stmt.else_branch->type == AST_IF_STMT) {
            ctx->line_start = 0;
            fmt_if_stmt(ctx, node->as.if_stmt.else_branch);
            return;
        } else {
            xfmt_emit_block(ctx, node->as.if_stmt.else_branch);
        }
    }
    xfmt_write_newline(ctx);
}

static void fmt_while_stmt(XrFmtContext *ctx, AstNode *node) {
    xfmt_write_indent(ctx);
    xfmt_write_str(ctx, "while (");
    xfmt_emit_expression(ctx, node->as.while_stmt.condition);
    xfmt_write_str(ctx, ") ");
    xfmt_emit_block(ctx, node->as.while_stmt.body);
    xfmt_write_newline(ctx);
}

static void fmt_for_stmt(XrFmtContext *ctx, AstNode *node) {
    xfmt_write_indent(ctx);
    xfmt_write_str(ctx, "for (");

    ForStmtNode *f = &node->as.for_stmt;
    if (f->initializer) {
        // Don't add newline for initializer
        int old_line_start = ctx->line_start;
        ctx->line_start = 0;
        if (f->initializer->type == AST_VAR_DECL || f->initializer->type == AST_CONST_DECL) {
            VarDeclNode *decl = &f->initializer->as.var_decl;
            xfmt_write_str(ctx, decl->is_const ? "const " : "let ");
            xfmt_write_str(ctx, decl->name);
            if (decl->initializer) {
                xfmt_write_str(ctx, " = ");
                xfmt_emit_expression(ctx, decl->initializer);
            }
        } else {
            xfmt_emit_expression(ctx, f->initializer);
        }
        ctx->line_start = old_line_start;
    }
    xfmt_write_str(ctx, "; ");
    if (f->condition) {
        xfmt_emit_expression(ctx, f->condition);
    }
    xfmt_write_str(ctx, "; ");
    if (f->increment) {
        xfmt_emit_expression(ctx, f->increment);
    }
    xfmt_write_str(ctx, ") ");
    xfmt_emit_block(ctx, f->body);
    xfmt_write_newline(ctx);
}

static void fmt_for_in_stmt(XrFmtContext *ctx, AstNode *node) {
    xfmt_write_indent(ctx);
    xfmt_write_str(ctx, "for (");

    ForInStmtNode *f = &node->as.for_in_stmt;
    if (f->is_keyvalue) {
        xfmt_write_str(ctx, f->item_name);
        xfmt_write_str(ctx, ", ");
        xfmt_write_str(ctx, f->value_name);
    } else {
        xfmt_write_str(ctx, f->item_name);
    }

    xfmt_write_str(ctx, " in ");
    xfmt_emit_expression(ctx, f->collection);
    xfmt_write_str(ctx, ") ");
    xfmt_emit_block(ctx, f->body);
    xfmt_write_newline(ctx);
}

static void fmt_try_catch(XrFmtContext *ctx, AstNode *node) {
    xfmt_write_indent(ctx);
    TryCatchNode *tc = &node->as.try_catch;

    xfmt_write_str(ctx, "try ");
    xfmt_emit_block(ctx, tc->try_body);

    if (tc->catch_body) {
        xfmt_write_str(ctx, " catch");
        if (tc->catch_var) {
            xfmt_write_str(ctx, " (");
            xfmt_write_str(ctx, tc->catch_var);
            xfmt_write_char(ctx, ')');
        }
        xfmt_write_space(ctx);
        xfmt_emit_block(ctx, tc->catch_body);
    }

    if (tc->finally_body) {
        xfmt_write_str(ctx, " finally ");
        xfmt_emit_block(ctx, tc->finally_body);
    }

    xfmt_write_newline(ctx);
}

static void fmt_select_stmt(XrFmtContext *ctx, AstNode *node) {
    xfmt_write_indent(ctx);
    xfmt_write_str(ctx, "select {");
    xfmt_write_newline(ctx);
    ctx->indent_level++;

    SelectStmtNode *sel = &node->as.select_stmt;
    for (int i = 0; i < sel->case_count; i++) {
        AstNode *c = sel->cases[i];
        SelectCaseNode *sc = &c->as.select_case;

        xfmt_write_indent(ctx);
        if (sc->is_default) {
            xfmt_write_char(ctx, '_');
        } else if (sc->is_timeout) {
            xfmt_write_str(ctx, "after ");
            xfmt_emit_expression(ctx, sc->value);
        } else if (sc->is_send) {
            xfmt_emit_expression(ctx, sc->value);
            xfmt_write_str(ctx, " to ");
            xfmt_emit_expression(ctx, sc->channel);
        } else {
            xfmt_write_str(ctx, sc->var_name);
            xfmt_write_str(ctx, " from ");
            xfmt_emit_expression(ctx, sc->channel);
        }
        xfmt_write_str(ctx, " => ");

        if (sc->body->type == AST_BLOCK) {
            xfmt_emit_block(ctx, sc->body);
        } else {
            xfmt_emit_expression(ctx, sc->body);
        }
        xfmt_write_newline(ctx);
    }

    ctx->indent_level--;
    xfmt_write_indent(ctx);
    xfmt_write_char(ctx, '}');
    xfmt_write_newline(ctx);
}

// ----------------------------------------------------------------------------
// Block & dispatch
// ----------------------------------------------------------------------------

void xfmt_emit_block(XrFmtContext *ctx, AstNode *node) {
    if (!node || node->type != AST_BLOCK) {
        xfmt_write_str(ctx, "{}");
        return;
    }

    xfmt_write_char(ctx, '{');
    xfmt_write_newline(ctx);
    ctx->indent_level++;

    BlockNode *block = &node->as.block;
    for (int i = 0; i < block->count; i++) {
        xfmt_emit_statement(ctx, block->statements[i]);
    }

    ctx->indent_level--;
    xfmt_write_indent(ctx);
    xfmt_write_char(ctx, '}');
}

void xfmt_emit_statement(XrFmtContext *ctx, AstNode *node) {
    if (!node)
        return;

    // Output leading comments
    if (node->leading_comments) {
        xfmt_write_leading_comments(ctx, node->leading_comments);
    }

    switch (node->type) {
        case AST_VAR_DECL:
        case AST_CONST_DECL:
            xfmt_emit_var_decl(ctx, node);
            break;

        case AST_DESTRUCTURE_DECL:
            xfmt_emit_destructure_decl(ctx, node);
            break;

        case AST_DESTRUCTURE_ASSIGN:
            xfmt_write_indent(ctx);
            xfmt_emit_pattern(ctx, node->as.destructure_assign.pattern);
            xfmt_write_str(ctx, " = ");
            xfmt_emit_expression(ctx, node->as.destructure_assign.value);
            xfmt_write_newline(ctx);
            break;

        case AST_FUNCTION_DECL:
            xfmt_emit_function_decl(ctx, node);
            break;

        case AST_CLASS_DECL:
        case AST_STRUCT_DECL:
            xfmt_emit_class_decl(ctx, node);
            break;

        case AST_INTERFACE_DECL:
            xfmt_emit_interface_decl(ctx, node);
            break;

        case AST_ENUM_DECL:
            xfmt_emit_enum_decl(ctx, node);
            break;

        case AST_TYPE_ALIAS:
            xfmt_emit_type_alias(ctx, node);
            break;

        case AST_IF_STMT:
            fmt_if_stmt(ctx, node);
            break;

        case AST_WHILE_STMT:
            fmt_while_stmt(ctx, node);
            break;

        case AST_FOR_STMT:
            fmt_for_stmt(ctx, node);
            break;

        case AST_FOR_IN_STMT:
            fmt_for_in_stmt(ctx, node);
            break;

        case AST_RETURN_STMT: {
            xfmt_write_indent(ctx);
            xfmt_write_str(ctx, "return");
            ReturnStmtNode *ret = &node->as.return_stmt;
            if (ret->value_count > 0) {
                xfmt_write_space(ctx);
                for (int i = 0; i < ret->value_count; i++) {
                    if (i > 0)
                        xfmt_write_str(ctx, ", ");
                    xfmt_emit_expression(ctx, ret->values[i]);
                }
            }
            xfmt_write_newline(ctx);
            break;
        }

        case AST_BREAK_STMT:
            xfmt_write_indent(ctx);
            xfmt_write_str(ctx, "break");
            xfmt_write_newline(ctx);
            break;

        case AST_CONTINUE_STMT:
            xfmt_write_indent(ctx);
            xfmt_write_str(ctx, "continue");
            xfmt_write_newline(ctx);
            break;

        case AST_TRY_CATCH:
            fmt_try_catch(ctx, node);
            break;

        case AST_THROW_STMT:
            xfmt_write_indent(ctx);
            xfmt_write_str(ctx, "throw ");
            xfmt_emit_expression(ctx, node->as.throw_stmt.expression);
            xfmt_write_newline(ctx);
            break;

        case AST_IMPORT_STMT: {
            xfmt_write_indent(ctx);
            ImportStmtNode *imp = &node->as.import_stmt;

            if (imp->member_count > 0) {
                xfmt_write_str(ctx, "import { ");
                for (int i = 0; i < imp->member_count; i++) {
                    if (i > 0)
                        xfmt_write_str(ctx, ", ");
                    xfmt_write_str(ctx, imp->members[i].name);
                    if (imp->members[i].alias) {
                        xfmt_write_str(ctx, " as ");
                        xfmt_write_str(ctx, imp->members[i].alias);
                    }
                }
                xfmt_write_str(ctx, " } from ");
            } else {
                xfmt_write_str(ctx, "import ");
            }

            if (imp->import_type == IMPORT_FILE || imp->import_type == IMPORT_DIR) {
                xfmt_write_char(ctx, '"');
                xfmt_write_str(ctx, imp->module_name);
                xfmt_write_char(ctx, '"');
            } else {
                xfmt_write_str(ctx, imp->module_name);
            }

            // Only output "as alias" if alias is different from module name
            if (imp->alias && imp->member_count == 0 && strcmp(imp->alias, imp->module_name) != 0) {
                xfmt_write_str(ctx, " as ");
                xfmt_write_str(ctx, imp->alias);
            }
            xfmt_write_newline(ctx);
            break;
        }

        case AST_EXPORT_STMT: {
            xfmt_write_indent(ctx);
            xfmt_write_str(ctx, "export ");
            ExportStmtNode *exp = &node->as.export_stmt;

            if (exp->declaration) {
                ctx->line_start = 0;
                xfmt_emit_statement(ctx, exp->declaration);
            } else if (exp->from_path) {
                // Re-export: export { a, b as c } from "./file"
                //        or: export * from "./file"
                if (exp->is_reexport_all) {
                    xfmt_write_str(ctx, "* from \"");
                    xfmt_write_str(ctx, exp->from_path);
                    xfmt_write_char(ctx, '"');
                } else {
                    xfmt_write_str(ctx, "{ ");
                    for (int i = 0; i < exp->reexport_count; i++) {
                        if (i > 0)
                            xfmt_write_str(ctx, ", ");
                        xfmt_write_str(ctx, exp->reexport_members[i].name);
                        if (exp->reexport_members[i].alias) {
                            xfmt_write_str(ctx, " as ");
                            xfmt_write_str(ctx, exp->reexport_members[i].alias);
                        }
                    }
                    xfmt_write_str(ctx, " } from \"");
                    xfmt_write_str(ctx, exp->from_path);
                    xfmt_write_char(ctx, '"');
                }
                xfmt_write_newline(ctx);
            } else if (exp->export_count > 0) {
                for (int i = 0; i < exp->export_count; i++) {
                    if (i > 0)
                        xfmt_write_str(ctx, ", ");
                    xfmt_write_str(ctx, exp->export_names[i]);
                }
                xfmt_write_newline(ctx);
            }
            break;
        }

        case AST_SELECT_STMT:
            fmt_select_stmt(ctx, node);
            break;

        case AST_DEFER_STMT:
            xfmt_write_indent(ctx);
            xfmt_write_str(ctx, "defer ");
            xfmt_emit_expression(ctx, node->as.defer_stmt.expr);
            xfmt_write_newline(ctx);
            break;

        case AST_SCOPE_BLOCK: {
            xfmt_write_indent(ctx);
            ScopeBlockNode *sb = &node->as.scope_block;
            if (sb->scope_mode == XR_SCOPE_LINKED)
                xfmt_write_str(ctx, "linked ");
            else if (sb->scope_mode == XR_SCOPE_SUPERVISOR)
                xfmt_write_str(ctx, "supervisor ");
            xfmt_write_str(ctx, "scope ");
            xfmt_emit_block(ctx, sb->body);
            xfmt_write_newline(ctx);
            break;
        }

        case AST_YIELD_STMT:
            xfmt_write_indent(ctx);
            xfmt_write_str(ctx, "yield");
            xfmt_write_newline(ctx);
            break;

        case AST_BLOCK:
            xfmt_emit_block(ctx, node);
            xfmt_write_newline(ctx);
            break;

        case AST_EXPR_STMT:
            xfmt_write_indent(ctx);
            xfmt_emit_expression(ctx, node->as.expr_stmt);
            xfmt_write_newline(ctx);
            break;

        case AST_PRINT_STMT: {
            xfmt_write_indent(ctx);
            xfmt_write_str(ctx, "print(");
            PrintNode *p = &node->as.print_stmt;
            for (int i = 0; i < p->expr_count; i++) {
                if (i > 0)
                    xfmt_write_str(ctx, ", ");
                xfmt_emit_expression(ctx, p->exprs[i]);
            }
            xfmt_write_str(ctx, ")");
            xfmt_write_newline(ctx);
            break;
        }

        case AST_ASSIGNMENT:
            xfmt_write_indent(ctx);
            xfmt_write_str(ctx, node->as.assignment.name);
            xfmt_write_str(ctx, " = ");
            xfmt_emit_expression(ctx, node->as.assignment.value);
            xfmt_write_newline(ctx);
            break;

        case AST_MEMBER_SET:
            xfmt_write_indent(ctx);
            xfmt_emit_expression(ctx, node->as.member_set.object);
            xfmt_write_char(ctx, '.');
            xfmt_write_str(ctx, node->as.member_set.member);
            xfmt_write_str(ctx, " = ");
            xfmt_emit_expression(ctx, node->as.member_set.value);
            xfmt_write_newline(ctx);
            break;

        case AST_INDEX_SET:
            xfmt_write_indent(ctx);
            xfmt_emit_expression(ctx, node->as.index_set.array);
            xfmt_write_char(ctx, '[');
            xfmt_emit_expression(ctx, node->as.index_set.index);
            xfmt_write_str(ctx, "] = ");
            xfmt_emit_expression(ctx, node->as.index_set.value);
            xfmt_write_newline(ctx);
            break;

        case AST_COMPOUND_ASSIGNMENT: {
            xfmt_write_indent(ctx);
            CompoundAssignmentNode *ca = &node->as.compound_assignment;
            if (ca->object) {
                xfmt_emit_expression(ctx, ca->object);
                xfmt_write_char(ctx, '.');
            }
            xfmt_write_str(ctx, ca->name);
            xfmt_write_space(ctx);
            xfmt_write_str(ctx, xfmt_compound_op(ca->op));
            xfmt_write_space(ctx);
            xfmt_emit_expression(ctx, ca->value);
            xfmt_write_newline(ctx);
            break;
        }

        case AST_INC:
            xfmt_write_indent(ctx);
            xfmt_write_str(ctx, node->as.inc.name);
            xfmt_write_str(ctx, "++");
            xfmt_write_newline(ctx);
            break;

        case AST_DEC:
            xfmt_write_indent(ctx);
            xfmt_write_str(ctx, node->as.dec.name);
            xfmt_write_str(ctx, "--");
            xfmt_write_newline(ctx);
            break;

        default:
            xfmt_write_indent(ctx);
            xfmt_write_fmt(ctx, "/* unsupported statement: %d */", node->type);
            xfmt_write_newline(ctx);
            break;
    }

    // L-06: every statement variant terminates in a newline; if the
    // parser captured an inline trailing comment for this node, splice
    // it in before the newline.
    if (node->trailing_comments) {
        xfmt_write_trailing_comment(ctx, node->trailing_comments);
    }
}

// ----------------------------------------------------------------------------
// Program
// ----------------------------------------------------------------------------

void xfmt_emit_program(XrFmtContext *ctx, AstNode *node) {
    if (!node || node->type != AST_PROGRAM)
        return;

    // Output file-level leading comments
    if (node->leading_comments) {
        xfmt_write_leading_comments(ctx, node->leading_comments);
    }

    ProgramNode *prog = &node->as.program;
    int last_was_decl = 0;

    for (int i = 0; i < prog->count; i++) {
        AstNode *stmt = prog->statements[i];

        int is_decl = (stmt->type == AST_FUNCTION_DECL || stmt->type == AST_CLASS_DECL ||
                       stmt->type == AST_STRUCT_DECL || stmt->type == AST_INTERFACE_DECL ||
                       stmt->type == AST_ENUM_DECL);

        if (i > 0 && (is_decl || last_was_decl)) {
            xfmt_write_newline(ctx);
        }

        xfmt_emit_statement(ctx, stmt);
        last_was_decl = is_decl;
    }
}
