/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_semantic_tokens.c - Semantic tokens implementation
 *
 * KEY CONCEPT:
 *   Uses XaAnalyzer scope information to accurately classify identifiers:
 *   - Functions vs variables
 *   - Parameters vs local variables
 *   - Constants (readonly modifier)
 *   - Classes, enums, interfaces
 */

#include "xlsp_semantic_tokens.h"
#include "../../frontend/lexer/xlex.h"
#include "../../frontend/parser/xast_nodes.h"
#include "../../frontend/analyzer/xanalyzer.h"
#include "../../frontend/analyzer/xanalyzer_symbol.h"
#include <stdlib.h>
#include <string.h>
#include "../../base/xmalloc.h"

// Token type names for legend
static const char *token_type_names[] = {
    "namespace", "type", "class", "enum", "interface", "struct",
    "typeParameter", "parameter", "variable", "property", "enumMember",
    "event", "function", "method", "macro", "keyword", "modifier",
    "comment", "string", "number", "regexp", "operator"
};

// Token modifier names for legend
static const char *token_modifier_names[] = {
    "declaration", "definition", "readonly", "static", "deprecated",
    "abstract", "async", "modification", "documentation", "defaultLibrary"
};

// Get semantic tokens legend
XrJsonValue *xlsp_semantic_tokens_legend(void) {
    XrJsonValue *legend = xlsp_json_new_object();
    
    // Token types
    XrJsonValue *types = xlsp_json_new_array();
    for (int i = 0; i < XLSP_TOKEN_COUNT; i++) {
        xlsp_json_array_push(types, xlsp_json_new_string(token_type_names[i]));
    }
    xlsp_json_object_set(legend, "tokenTypes", types);
    
    // Token modifiers
    XrJsonValue *modifiers = xlsp_json_new_array();
    for (int i = 0; i < 10; i++) {
        xlsp_json_array_push(modifiers, xlsp_json_new_string(token_modifier_names[i]));
    }
    xlsp_json_object_set(legend, "tokenModifiers", modifiers);
    
    return legend;
}

// Create result
static XlspSemanticTokensResult *result_new(void) {
    XlspSemanticTokensResult *r = xr_calloc(1, sizeof(XlspSemanticTokensResult));
    r->capacity = 256;
    r->tokens = xr_calloc(r->capacity, sizeof(XlspSemanticToken));
    return r;
}

// Add token to result
static void result_add(XlspSemanticTokensResult *r, int line, int start, 
                       int length, XlspSemanticTokenType type, int mods) {
    if (r->count >= r->capacity) {
        r->capacity *= 2;
        r->tokens = xr_realloc(r->tokens, r->capacity * sizeof(XlspSemanticToken));
    }
    
    XlspSemanticToken *t = &r->tokens[r->count++];
    t->line = line;
    t->start_char = start;
    t->length = length;
    t->type = type;
    t->modifiers = mods;
}

// Free result
void xlsp_semantic_tokens_free(XlspSemanticTokensResult *result) {
    if (!result) return;
    xr_free(result->tokens);
    xr_free(result);
}

// Compare tokens for sorting
static int compare_tokens(const void *a, const void *b) {
    const XlspSemanticToken *ta = a;
    const XlspSemanticToken *tb = b;
    if (ta->line != tb->line) return ta->line - tb->line;
    return ta->start_char - tb->start_char;
}

// Context for semantic token collection
typedef struct {
    XlspSemanticTokensResult *result;
    XaAnalyzer *analyzer;
    XaScope *current_scope;
} SemanticTokenContext;

// Determine token type for a variable reference based on analyzer info
static XlspSemanticTokenType get_var_token_type(SemanticTokenContext *ctx, const char *name, int *out_mods) {
    *out_mods = 0;
    
    if (!ctx->analyzer || !ctx->current_scope) {
        return XLSP_TOKEN_VARIABLE;
    }
    
    // Look up symbol in current scope chain
    XaSymbol *sym = xa_scope_lookup(ctx->current_scope, name);
    if (!sym) {
        return XLSP_TOKEN_VARIABLE;
    }
    
    // Set modifiers
    if (sym->is_const) *out_mods |= XLSP_MOD_READONLY;
    if (sym->is_static) *out_mods |= XLSP_MOD_STATIC;
    if (sym->is_builtin) *out_mods |= XLSP_MOD_DEFAULT_LIBRARY;
    
    // Determine type based on symbol kind
    switch (sym->kind) {
        case XA_SYM_FUNCTION:
            return XLSP_TOKEN_FUNCTION;
        case XA_SYM_CLASS:
            return XLSP_TOKEN_CLASS;
        case XA_SYM_PARAMETER:
            return XLSP_TOKEN_PARAMETER;
        case XA_SYM_VARIABLE:
            if (sym->is_const) *out_mods |= XLSP_MOD_READONLY;
            return XLSP_TOKEN_VARIABLE;
        case XA_SYM_FIELD:
        case XA_SYM_PROPERTY:
            return XLSP_TOKEN_PROPERTY;
        case XA_SYM_METHOD:
            return XLSP_TOKEN_METHOD;
        case XA_SYM_MODULE:
            return XLSP_TOKEN_NAMESPACE;
        default:
            return XLSP_TOKEN_VARIABLE;
    }
}

// Helper: find child scope by AST node
static XaScope *find_child_scope(XaScope *parent, void *ast_node) {
    if (!parent) return NULL;
    for (int i = 0; i < parent->child_count; i++) {
        if (parent->children[i]->ast_node == ast_node) {
            return parent->children[i];
        }
    }
    return NULL;
}

// Collect tokens from AST with analyzer context
static void collect_tokens_ast(SemanticTokenContext *ctx, AstNode *node) {
    if (!node) return;
    
    XlspSemanticTokensResult *result = ctx->result;
    
    switch (node->type) {
        case AST_PROGRAM: {
            // Enter global scope
            if (ctx->analyzer) {
                ctx->current_scope = ctx->analyzer->global_scope;
            }
            int count = node->as.program.count;
            for (int i = 0; i < count; i++) {
                collect_tokens_ast(ctx, node->as.program.statements[i]);
            }
            break;
        }
        
        case AST_BLOCK: {
            XaScope *saved = ctx->current_scope;
            XaScope *block_scope = find_child_scope(ctx->current_scope, node);
            if (block_scope) ctx->current_scope = block_scope;
            
            int count = node->as.block.count;
            for (int i = 0; i < count; i++) {
                collect_tokens_ast(ctx, node->as.block.statements[i]);
            }
            ctx->current_scope = saved;
            break;
        }
        
        case AST_VAR_DECL:
        case AST_CONST_DECL: {
            const char *name = node->as.var_decl.name;
            if (name) {
                int mods = XLSP_MOD_DECLARATION;
                if (node->type == AST_CONST_DECL || node->as.var_decl.is_const) {
                    mods |= XLSP_MOD_READONLY;
                }
                int col = node->column > 0 ? node->column - 1 : 0;
                result_add(result, node->line - 1, col, strlen(name), 
                           XLSP_TOKEN_VARIABLE, mods);
            }
            collect_tokens_ast(ctx, node->as.var_decl.initializer);
            break;
        }
        
        case AST_FUNCTION_DECL: {
            FunctionDeclNode *fn = &node->as.function_decl;
            if (fn->name) {
                int col = node->column > 0 ? node->column - 1 : 0;
                int mods = XLSP_MOD_DECLARATION | XLSP_MOD_DEFINITION;
                result_add(result, node->line - 1, col, strlen(fn->name),
                           XLSP_TOKEN_FUNCTION, mods);
            }
            
            // Enter function scope
            XaScope *saved = ctx->current_scope;
            XaScope *fn_scope = find_child_scope(ctx->current_scope, node);
            if (fn_scope) ctx->current_scope = fn_scope;
            
            // Parameters
            for (int i = 0; i < fn->param_count; i++) {
                XrParamNode *p = fn->params[i];
                if (!p || !p->name) continue;
                int param_col = p->column > 0 ? p->column - 1 : 0;
                int param_line = p->line > 0 ? p->line - 1 : node->line - 1;
                result_add(result, param_line, param_col, strlen(p->name),
                           XLSP_TOKEN_PARAMETER, XLSP_MOD_DECLARATION);
            }
            
            collect_tokens_ast(ctx, fn->body);
            ctx->current_scope = saved;
            break;
        }
        
        case AST_FUNCTION_EXPR: {
            FunctionDeclNode *fn = &node->as.function_expr;
            
            // Enter function scope
            XaScope *saved = ctx->current_scope;
            XaScope *fn_scope = find_child_scope(ctx->current_scope, node);
            if (fn_scope) ctx->current_scope = fn_scope;
            
            // Parameters
            for (int i = 0; i < fn->param_count; i++) {
                XrParamNode *p = fn->params[i];
                if (!p || !p->name) continue;
                int param_col = p->column > 0 ? p->column - 1 : 0;
                int param_line = p->line > 0 ? p->line - 1 : node->line - 1;
                result_add(result, param_line, param_col, strlen(p->name),
                           XLSP_TOKEN_PARAMETER, XLSP_MOD_DECLARATION);
            }
            
            collect_tokens_ast(ctx, fn->body);
            ctx->current_scope = saved;
            break;
        }
        
        case AST_CLASS_DECL:
        case AST_STRUCT_DECL: {
            ClassDeclNode *cls = &node->as.class_decl;
            if (cls->name) {
                int col = node->column > 0 ? node->column - 1 : 0;
                int mods = XLSP_MOD_DECLARATION | XLSP_MOD_DEFINITION;
                if (cls->is_abstract) mods |= XLSP_MOD_ABSTRACT;
                result_add(result, node->line - 1, col, strlen(cls->name),
                           XLSP_TOKEN_CLASS, mods);
            }
            
            // Enter class scope
            XaScope *saved = ctx->current_scope;
            XaScope *cls_scope = find_child_scope(ctx->current_scope, node);
            if (cls_scope) ctx->current_scope = cls_scope;
            
            // Fields
            for (int i = 0; i < cls->field_count; i++) {
                collect_tokens_ast(ctx, cls->fields[i]);
            }
            
            // Methods
            for (int i = 0; i < cls->method_count; i++) {
                AstNode *method = cls->methods[i];
                if (method && method->type == AST_FUNCTION_DECL) {
                    FunctionDeclNode *m = &method->as.function_decl;
                    if (m->name) {
                        int col = method->column > 0 ? method->column - 1 : 0;
                        int mods = XLSP_MOD_DECLARATION | XLSP_MOD_DEFINITION;
                        result_add(result, method->line - 1, col, strlen(m->name),
                                   XLSP_TOKEN_METHOD, mods);
                    }
                    
                    // Method parameters and body
                    XaScope *method_saved = ctx->current_scope;
                    XaScope *method_scope = find_child_scope(ctx->current_scope, method);
                    if (method_scope) ctx->current_scope = method_scope;
                    
                    for (int j = 0; j < m->param_count; j++) {
                        XrParamNode *p = m->params[j];
                        if (!p || !p->name) continue;
                        int param_col = p->column > 0 ? p->column - 1 : 0;
                        int param_line = p->line > 0 ? p->line - 1 : method->line - 1;
                        result_add(result, param_line, param_col, strlen(p->name),
                                   XLSP_TOKEN_PARAMETER, XLSP_MOD_DECLARATION);
                    }
                    
                    collect_tokens_ast(ctx, m->body);
                    ctx->current_scope = method_saved;
                }
            }
            
            ctx->current_scope = saved;
            break;
        }
        
        case AST_ENUM_DECL: {
            EnumDeclNode *en = &node->as.enum_decl;
            if (en->name) {
                int col = node->column > 0 ? node->column - 1 : 0;
                result_add(result, node->line - 1, col, strlen(en->name),
                           XLSP_TOKEN_ENUM, XLSP_MOD_DECLARATION | XLSP_MOD_DEFINITION);
            }
            // Enum members
            for (int i = 0; i < en->member_count; i++) {
                AstNode *member = en->members[i];
                if (member && member->type == AST_ENUM_MEMBER) {
                    const char *mname = member->as.enum_member.name;
                    if (mname) {
                        int mcol = member->column > 0 ? member->column - 1 : 0;
                        result_add(result, member->line - 1, mcol, strlen(mname),
                                   XLSP_TOKEN_ENUM_MEMBER, XLSP_MOD_READONLY);
                    }
                }
            }
            break;
        }
        
        case AST_INTERFACE_DECL: {
            InterfaceDeclNode *iface = &node->as.interface_decl;
            if (iface->name) {
                int col = node->column > 0 ? node->column - 1 : 0;
                result_add(result, node->line - 1, col, strlen(iface->name),
                           XLSP_TOKEN_INTERFACE, XLSP_MOD_DECLARATION | XLSP_MOD_DEFINITION);
            }
            // Interface methods
            for (int i = 0; i < iface->method_count; i++) {
                AstNode *method = iface->methods[i];
                if (method && method->type == AST_INTERFACE_METHOD) {
                    const char *mname = method->as.interface_method.name;
                    if (mname) {
                        int mcol = method->column > 0 ? method->column - 1 : 0;
                        result_add(result, method->line - 1, mcol, strlen(mname),
                                   XLSP_TOKEN_METHOD, XLSP_MOD_ABSTRACT);
                    }
                }
            }
            break;
        }
        
        case AST_VARIABLE: {
            const char *name = node->as.variable.name;
            if (name) {
                int mods = 0;
                XlspSemanticTokenType type = get_var_token_type(ctx, name, &mods);
                int col = node->column > 0 ? node->column - 1 : 0;
                result_add(result, node->line - 1, col, strlen(name), type, mods);
            }
            break;
        }
        
        case AST_ASSIGNMENT: {
            const char *name = node->as.assignment.name;
            if (name) {
                int mods = XLSP_MOD_MODIFICATION;
                XlspSemanticTokenType type = get_var_token_type(ctx, name, &mods);
                mods |= XLSP_MOD_MODIFICATION;  // Always add modification
                int col = node->column > 0 ? node->column - 1 : 0;
                result_add(result, node->line - 1, col, strlen(name), type, mods);
            }
            collect_tokens_ast(ctx, node->as.assignment.value);
            break;
        }
        
        case AST_CALL_EXPR: {
            AstNode *callee = node->as.call_expr.callee;
            
            // Handle callee specially - it's being called as a function
            if (callee) {
                if (callee->type == AST_VARIABLE) {
                    const char *name = callee->as.variable.name;
                    if (name) {
                        int mods = 0;
                        // Check if it's a known function or class (constructor)
                        XlspSemanticTokenType type = XLSP_TOKEN_FUNCTION;
                        if (ctx->analyzer && ctx->current_scope) {
                            XaSymbol *sym = xa_scope_lookup(ctx->current_scope, name);
                            if (sym) {
                                if (sym->kind == XA_SYM_CLASS) {
                                    type = XLSP_TOKEN_CLASS;
                                } else if (sym->is_builtin) {
                                    mods |= XLSP_MOD_DEFAULT_LIBRARY;
                                }
                            }
                        }
                        int col = callee->column > 0 ? callee->column - 1 : 0;
                        result_add(result, callee->line - 1, col, strlen(name), type, mods);
                    }
                } else if (callee->type == AST_MEMBER_ACCESS) {
                    // Method call: obj.method()
                    collect_tokens_ast(ctx, callee->as.member_access.object);
                    const char *name = callee->as.member_access.name;
                    if (name) {
                        int col = callee->column > 0 ? callee->column - 1 : 0;
                        result_add(result, callee->line - 1, col, strlen(name),
                                   XLSP_TOKEN_METHOD, 0);
                    }
                } else {
                    collect_tokens_ast(ctx, callee);
                }
            }
            
            // Arguments
            for (int i = 0; i < node->as.call_expr.arg_count; i++) {
                collect_tokens_ast(ctx, node->as.call_expr.arguments[i]);
            }
            break;
        }
        
        case AST_MEMBER_ACCESS: {
            collect_tokens_ast(ctx, node->as.member_access.object);
            const char *name = node->as.member_access.name;
            if (name) {
                int col = node->column > 0 ? node->column - 1 : 0;
                result_add(result, node->line - 1, col, strlen(name),
                           XLSP_TOKEN_PROPERTY, 0);
            }
            break;
        }
        
        case AST_FOR_IN_STMT: {
            ForInStmtNode *fi = &node->as.for_in_stmt;
            
            // Loop variable (item or key depending on is_keyvalue)
            if (fi->item_name) {
                int col = node->column > 0 ? node->column - 1 : 0;
                result_add(result, node->line - 1, col, strlen(fi->item_name),
                           XLSP_TOKEN_VARIABLE, XLSP_MOD_DECLARATION);
            }
            
            // Value variable in key-value iteration (for k, v in map)
            if (fi->is_keyvalue && fi->value_name) {
                // Value is after item in "for key, value in ..."
                int col = node->column > 0 ? node->column - 1 : 0;
                result_add(result, node->line - 1, col, strlen(fi->value_name),
                           XLSP_TOKEN_VARIABLE, XLSP_MOD_DECLARATION);
            }
            
            collect_tokens_ast(ctx, fi->collection);
            
            // Enter loop scope
            XaScope *saved = ctx->current_scope;
            XaScope *loop_scope = find_child_scope(ctx->current_scope, node);
            if (loop_scope) ctx->current_scope = loop_scope;
            
            collect_tokens_ast(ctx, fi->body);
            ctx->current_scope = saved;
            break;
        }
        
        case AST_IF_STMT:
            collect_tokens_ast(ctx, node->as.if_stmt.condition);
            collect_tokens_ast(ctx, node->as.if_stmt.then_branch);
            collect_tokens_ast(ctx, node->as.if_stmt.else_branch);
            break;
            
        case AST_WHILE_STMT:
            collect_tokens_ast(ctx, node->as.while_stmt.condition);
            collect_tokens_ast(ctx, node->as.while_stmt.body);
            break;
            
        case AST_FOR_STMT: {
            XaScope *saved = ctx->current_scope;
            XaScope *for_scope = find_child_scope(ctx->current_scope, node);
            if (for_scope) ctx->current_scope = for_scope;
            
            collect_tokens_ast(ctx, node->as.for_stmt.initializer);
            collect_tokens_ast(ctx, node->as.for_stmt.condition);
            collect_tokens_ast(ctx, node->as.for_stmt.increment);
            collect_tokens_ast(ctx, node->as.for_stmt.body);
            ctx->current_scope = saved;
            break;
        }
        
        case AST_TRY_CATCH: {
            TryCatchNode *tc = &node->as.try_catch;
            collect_tokens_ast(ctx, tc->try_body);
            
            // Catch variable
            if (tc->catch_var && tc->catch_var_line > 0) {
                int col = tc->catch_var_column > 0 ? tc->catch_var_column - 1 : 0;
                result_add(result, tc->catch_var_line - 1, col, strlen(tc->catch_var),
                           XLSP_TOKEN_PARAMETER, XLSP_MOD_DECLARATION);
            }
            
            XaScope *saved = ctx->current_scope;
            XaScope *catch_scope = find_child_scope(ctx->current_scope, tc->catch_body);
            if (catch_scope) ctx->current_scope = catch_scope;
            
            collect_tokens_ast(ctx, tc->catch_body);
            ctx->current_scope = saved;
            
            collect_tokens_ast(ctx, tc->finally_body);
            break;
        }
        
        case AST_RETURN_STMT:
            for (int i = 0; i < node->as.return_stmt.value_count; i++) {
                collect_tokens_ast(ctx, node->as.return_stmt.values[i]);
            }
            break;
            
        case AST_EXPR_STMT:
            collect_tokens_ast(ctx, node->as.expr_stmt);
            break;
            
        case AST_PRINT_STMT:
            for (int i = 0; i < node->as.print_stmt.expr_count; i++) {
                collect_tokens_ast(ctx, node->as.print_stmt.exprs[i]);
            }
            break;
        
        // Binary expressions
        case AST_BINARY_ADD:
        case AST_BINARY_SUB:
        case AST_BINARY_MUL:
        case AST_BINARY_DIV:
        case AST_BINARY_MOD:
        case AST_BINARY_EQ:
        case AST_BINARY_NE:
        case AST_BINARY_LT:
        case AST_BINARY_LE:
        case AST_BINARY_GT:
        case AST_BINARY_GE:
        case AST_BINARY_AND:
        case AST_BINARY_OR:
        case AST_BINARY_BAND:
        case AST_BINARY_BOR:
        case AST_BINARY_BXOR:
            collect_tokens_ast(ctx, node->as.binary.left);
            collect_tokens_ast(ctx, node->as.binary.right);
            break;
        
        // Unary expressions
        case AST_UNARY_NEG:
        case AST_UNARY_NOT:
        case AST_UNARY_BNOT:
            collect_tokens_ast(ctx, node->as.unary.operand);
            break;
        
        case AST_INDEX_GET:
            collect_tokens_ast(ctx, node->as.index_get.array);
            collect_tokens_ast(ctx, node->as.index_get.index);
            break;
        
        case AST_INDEX_SET:
            collect_tokens_ast(ctx, node->as.index_set.array);
            collect_tokens_ast(ctx, node->as.index_set.index);
            collect_tokens_ast(ctx, node->as.index_set.value);
            break;
        
        case AST_TERNARY:
            collect_tokens_ast(ctx, node->as.ternary.condition);
            collect_tokens_ast(ctx, node->as.ternary.true_expr);
            collect_tokens_ast(ctx, node->as.ternary.false_expr);
            break;
        
        case AST_ARRAY_LITERAL:
            for (int i = 0; i < node->as.array_literal.count; i++) {
                collect_tokens_ast(ctx, node->as.array_literal.elements[i]);
            }
            break;
            
        case AST_NEW_EXPR: {
            // Constructor call: new ClassName(...)
            const char *name = node->as.new_expr.class_name;
            if (name) {
                int col = node->column > 0 ? node->column - 1 : 0;
                result_add(result, node->line - 1, col, strlen(name),
                           XLSP_TOKEN_CLASS, 0);
            }
            for (int i = 0; i < node->as.new_expr.arg_count; i++) {
                collect_tokens_ast(ctx, node->as.new_expr.arguments[i]);
            }
            break;
        }
        
        case AST_ENUM_ACCESS: {
            // Enum.Member
            const char *enum_name = node->as.enum_access.enum_name;
            const char *member_name = node->as.enum_access.member_name;
            int col = node->column > 0 ? node->column - 1 : 0;
            if (enum_name) {
                result_add(result, node->line - 1, col, strlen(enum_name),
                           XLSP_TOKEN_ENUM, 0);
            }
            if (member_name) {
                result_add(result, node->line - 1, col + strlen(enum_name) + 1, 
                           strlen(member_name), XLSP_TOKEN_ENUM_MEMBER, XLSP_MOD_READONLY);
            }
            break;
        }
            
        default:
            break;
    }
}

// Analyze document for semantic tokens
XlspSemanticTokensResult *xlsp_analyze_semantic_tokens(XrLspDocument *doc) {
    XlspSemanticTokensResult *result = result_new();
    
    if (!doc || !doc->content) return result;
    
    // Use AST if available
    if (doc->ast) {
        SemanticTokenContext ctx = {
            .result = result,
            .analyzer = doc->server ? doc->server->workspace_analyzer : NULL,
            .current_scope = NULL
        };
        
        // Initialize scope from analyzer if available
        if (ctx.analyzer) {
            ctx.current_scope = ctx.analyzer->global_scope;
        }
        
        collect_tokens_ast(&ctx, doc->ast);
    }
    
    // Sort tokens by position
    if (result->count > 1) {
        qsort(result->tokens, result->count, sizeof(XlspSemanticToken), compare_tokens);
    }
    
    return result;
}

// Encode tokens to raw uint32_t array (for caching/delta comparison)
uint32_t *xlsp_semantic_tokens_encode_raw(XlspSemanticTokensResult *result, int *out_count) {
    if (!result || result->count == 0) {
        *out_count = 0;
        return NULL;
    }
    int total = result->count * 5;
    uint32_t *data = xr_malloc(sizeof(uint32_t) * total);
    int prev_line = 0, prev_char = 0;
    
    for (int i = 0; i < result->count; i++) {
        XlspSemanticToken *t = &result->tokens[i];
        int delta_line = t->line - prev_line;
        int delta_char = (delta_line == 0) ? (t->start_char - prev_char) : t->start_char;
        int idx = i * 5;
        data[idx]     = (uint32_t)delta_line;
        data[idx + 1] = (uint32_t)delta_char;
        data[idx + 2] = (uint32_t)t->length;
        data[idx + 3] = (uint32_t)t->type;
        data[idx + 4] = (uint32_t)t->modifiers;
        prev_line = t->line;
        prev_char = t->start_char;
    }
    *out_count = total;
    return data;
}

// Encode tokens to LSP format (delta encoding)
XrJsonValue *xlsp_semantic_tokens_encode(XlspSemanticTokensResult *result) {
    XrJsonValue *response = xlsp_json_new_object();
    XrJsonValue *data = xlsp_json_new_array();
    
    int prev_line = 0;
    int prev_char = 0;
    
    for (int i = 0; i < result->count; i++) {
        XlspSemanticToken *t = &result->tokens[i];
        
        // Delta encoding
        int delta_line = t->line - prev_line;
        int delta_char = (delta_line == 0) ? (t->start_char - prev_char) : t->start_char;
        
        xlsp_json_array_push(data, xlsp_json_new_number(delta_line));
        xlsp_json_array_push(data, xlsp_json_new_number(delta_char));
        xlsp_json_array_push(data, xlsp_json_new_number(t->length));
        xlsp_json_array_push(data, xlsp_json_new_number(t->type));
        xlsp_json_array_push(data, xlsp_json_new_number(t->modifiers));
        
        prev_line = t->line;
        prev_char = t->start_char;
    }
    
    xlsp_json_object_set(response, "data", data);
    return response;
}
