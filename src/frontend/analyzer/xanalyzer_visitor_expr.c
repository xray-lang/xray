/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_visitor_expr.c - Pass 2 expression type inference visitors
 *
 * KEY CONCEPT:
 *   Type inference for all expression kinds: literals, variables,
 *   operators, calls, member access, match expressions, optional
 *   chains, closures, and generic substitution.
 */

#include "xanalyzer_visitor_internal.h"
#include "xtype_ref_resolve.h"
#include "xa_selection.h"
#include "../../base/xchecks.h"

/* Record a selection fact for a member/index access node. */
static void record_selection(XaInferContext *ctx, AstNode *node, XaSelectionKind kind,
                             XrType *receiver, XaSymbol *target, int32_t field_idx, XrType *result,
                             bool is_optional) {
    XaSelectionTable *st = (XaSelectionTable *) ctx->analyzer->selection_table;
    if (!st)
        return;
    XaSelection sel = {
        .kind = kind,
        .receiver_type = receiver,
        .target_symbol = target,
        .field_index = field_idx,
        .result_type = result,
        .is_indirect = false,
        .is_optional = is_optional,
    };
    xa_selection_table_set(st, node, &sel);
}

static const char *json_type_label(XrType *type) {
    if (type && XR_TYPE_IS_JSON(type) && type->object.type_name)
        return type->object.type_name;
    return xr_type_to_string(type);
}

static int json_field_index(XrType *type, const char *name) {
    if (!type || !XR_TYPE_IS_JSON(type) || !name || !type->object.field_names)
        return -1;
    for (int i = 0; i < type->object.field_count; i++) {
        if (type->object.field_names[i] && strcmp(type->object.field_names[i], name) == 0)
            return i;
    }
    return -1;
}

static void add_index_type_error(XaInferContext *ctx, AstNode *node, XrType *index_type,
                                 XrType *expected_type) {
    XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
    char msg[256];
    snprintf(msg, sizeof(msg), "Index type '%s' is not assignable to expected type '%s'",
             xr_type_to_string(index_type), xr_type_to_string(expected_type));
    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH, msg,
                               &loc);
}

/* ============================================================================
 * Pass 2: Expression Visitors
 * ============================================================================
 * Type inference for all expression kinds: literals, variables, operators,
 * function calls, member access, containers, etc.
 * ========================================================================== */

// Forward declarations for visitors defined later in this file. The
// canonical decls also live in xanalyzer_visitor_internal.h.
// xa_visit_match_expr lives in xanalyzer_visitor_pattern.c and
// xa_visit_call lives in xanalyzer_visitor_call.c.
XrType *xa_visit_nullish_coalesce(XaInferContext *ctx, AstNode *node);
XrType *xa_visit_optional_chain(XaInferContext *ctx, AstNode *node);

// Check if an AST node is a typeof() call, return the argument variable name
const char *get_typeof_arg_name(AstNode *node) {
    if (!node || node->type != AST_CALL_EXPR)
        return NULL;
    CallExprNode *call = &node->as.call_expr;
    if (!call->callee || call->callee->type != AST_VARIABLE)
        return NULL;
    if (strcmp(call->callee->as.variable.name, "typeof") != 0)
        return NULL;
    if (call->arg_count != 1 || !call->arguments[0])
        return NULL;
    if (call->arguments[0]->type != AST_VARIABLE)
        return NULL;
    return call->arguments[0]->as.variable.name;
}

XrType *xa_visit_variable(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node)
        return xr_type_new_unknown(NULL);

    const char *name = node->as.variable.name;
    XaSymbol *sym = xa_scope_lookup(ctx->analyzer->current_scope, name);

    // Also check global scope for classes (needed when called from compile phase)
    if (!sym && ctx->analyzer->global_scope) {
        sym = xa_scope_lookup(ctx->analyzer->global_scope, name);
    }

    if (!sym) {
        // Undeclared variable — detect common cross-language mistakes
        XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
        char msg[256];
        if (strcmp(name, "True") == 0) {
            snprintf(msg, sizeof(msg), "Undeclared variable 'True'. Use lowercase 'true' in Xray");
        } else if (strcmp(name, "False") == 0) {
            snprintf(msg, sizeof(msg),
                     "Undeclared variable 'False'. Use lowercase 'false' in Xray");
        } else if (strcmp(name, "None") == 0) {
            snprintf(msg, sizeof(msg),
                     "Undeclared variable 'None'. Use 'null' instead of 'None' in Xray");
        } else if (strcmp(name, "nil") == 0) {
            snprintf(msg, sizeof(msg),
                     "Undeclared variable 'nil'. Use 'null' instead of 'nil' in Xray");
        } else if (strcmp(name, "undefined") == 0) {
            snprintf(msg, sizeof(msg), "Undeclared variable 'undefined'. Use 'null' in Xray");
        } else if (strcmp(name, "self") == 0) {
            snprintf(
                msg, sizeof(msg),
                "Undeclared variable 'self'. Use 'this' to refer to the current instance in Xray");
        } else {
            snprintf(msg, sizeof(msg), "Undeclared variable '%s'", name);
        }
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_UNDEFINED_VAR,
                                   msg, &loc);
        return xr_type_new_unknown(NULL);
    }

    /* Write back resolved symbol ID for Xi lowering (Braun SSA key). */
    node->as.variable.symbol_id = sym->id;

    // Record dependency: current function depends on this symbol
    if (ctx->current_function && ctx->analyzer->incremental) {
        XaIncrementalCtx *incr = (XaIncrementalCtx *) ctx->analyzer->incremental;
        xa_dep_add(incr, ctx->current_function->id, sym->id, XA_DEP_REFERENCE);
    }

    // Record reference location for Find References
    XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
    if (links) {
        uint32_t end_col = node->column + (name ? strlen(name) : 0);
        xa_symbol_add_ref(links, node->line, node->column, end_col, false);
    }

    // Definite assignment check: warn if variable used before initialization
    if (links && !links->is_definitely_assigned && sym->kind == XA_SYM_VARIABLE &&
        !sym->is_builtin) {
        XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
        char msg[256];
        snprintf(msg, sizeof(msg), "Variable '%s' is used before being assigned", name);
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH,
                                   msg, &loc);
    }

    // Get declared type
    XrType *declared_type = xa_analyzer_get_type(ctx->analyzer, sym);

    // Apply flow-based type narrowing for storage that can change type
    // along control flow: locals AND parameters. Functions, classes,
    // modules, type aliases never narrow because their type is the
    // declared identity, not a value.
    if (ctx->flow && ctx->flow->current_flow && declared_type &&
        (sym->kind == XA_SYM_VARIABLE || sym->kind == XA_SYM_PARAMETER)) {
        XrType *narrowed = xa_flow_get_type_of_reference(ctx->flow, name, declared_type,
                                                         ctx->flow->current_flow, ctx->cache);
        // Never means unreachable flow path — fall back to declared type
        if (narrowed && narrowed != declared_type && !XR_TYPE_IS_NEVER(narrowed)) {
            // Side table is the canonical type store.
            xa_analyzer_set_node_type(ctx->analyzer, node, narrowed);
            return narrowed;
        }
    }

    // Store the declared type in the analyzer side table for codegen.
    xa_analyzer_set_node_type(ctx->analyzer, node, declared_type);
    return declared_type;
}

// Compute arithmetic result for a single (left, right) pair of scalar types.
// Handles ADD/SUB/MUL/DIV/MOD with the same rules used before union
// distribution; returns NULL when the pair is incompatible so the caller
// can decide whether the overall result must collapse to unknown.
static XrType *binary_arith_pair(int op, XrType *left, XrType *right) {
    if (!left || !right)
        return NULL;
    if (XR_TYPE_IS_UNKNOWN(left) || XR_TYPE_IS_UNKNOWN(right))
        return NULL;

    if (op == AST_BINARY_ADD) {
        // string + string => string; string + (int|float|bool) => string
        // (dynamic concat handled by OP_ADD)
        if (XR_TYPE_IS_STRING(left) || XR_TYPE_IS_STRING(right))
            return xr_type_new_string(NULL);
    }

    if (XR_TYPE_IS_FLOAT(left) || XR_TYPE_IS_FLOAT(right)) {
        if (XR_TYPE_IS_FLOAT(left) || XR_TYPE_IS_INT(left)) {
            if (XR_TYPE_IS_FLOAT(right) || XR_TYPE_IS_INT(right))
                return xr_type_new_float(NULL);
        }
        return NULL;
    }
    if (XR_TYPE_IS_INT(left) && XR_TYPE_IS_INT(right))
        return xr_type_new_int(NULL);

    // Generic body: preserve type parameter through arithmetic so
    // `fn add_one<T>(x: T): T { return x + 1 }` type-checks before
    // monomorphisation.
    if (left->kind == XR_KIND_TYPE_PARAM)
        return left;
    if (right->kind == XR_KIND_TYPE_PARAM)
        return right;

    return NULL;
}

// Distribute a binary arithmetic op over union members so e.g.
// (int|string) + (int|string) infers int|string instead of unknown.
// Falls back to unknown if any member combination is incompatible
// (caller already validated the operand-set against the operator).
static XrType *binary_arith_distribute(XaInferContext *ctx, int op, XrType *left, XrType *right) {
    int lc = XR_TYPE_IS_UNION(left) ? left->union_type.member_count : 1;
    int rc = XR_TYPE_IS_UNION(right) ? right->union_type.member_count : 1;
    XrType *single_l = XR_TYPE_IS_UNION(left) ? NULL : left;
    XrType *single_r = XR_TYPE_IS_UNION(right) ? NULL : right;

    XrType *acc = NULL;
    XrayIsolate *X = ctx && ctx->analyzer ? ctx->analyzer->isolate : NULL;
    for (int i = 0; i < lc; i++) {
        XrType *li = single_l ? single_l : left->union_type.members[i];
        for (int j = 0; j < rc; j++) {
            XrType *rj = single_r ? single_r : right->union_type.members[j];
            XrType *r = binary_arith_pair(op, li, rj);
            if (!r) {
                // Skip incompatible pairs (e.g. string + int when both
                // sides are int|string) — runtime narrowing eliminates
                // these at the JIT/VM level.
                continue;
            }
            acc = acc ? xr_type_union(X, acc, r) : r;
        }
    }
    return acc ? acc : xr_type_new_unknown(NULL);
}

XrType *xa_visit_binary(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node)
        return xr_type_new_unknown(NULL);

    XrType *left = xa_visit_infer_expr(ctx, node->as.binary.left);
    XrType *right = xa_visit_infer_expr(ctx, node->as.binary.right);

    // Deterministic result types: language rules independent of operand types
    switch (node->type) {
        // Comparison → always bool
        case AST_BINARY_EQ:
        case AST_BINARY_NE:
        case AST_BINARY_EQ_STRICT:
        case AST_BINARY_NE_STRICT:
        case AST_BINARY_LT:
        case AST_BINARY_LE:
        case AST_BINARY_GT:
        case AST_BINARY_GE:
            return xr_type_new_bool(NULL);
        // Logical → always bool
        case AST_BINARY_AND:
        case AST_BINARY_OR:
            return xr_type_new_bool(NULL);
        // Bitwise → always int
        case AST_BINARY_BAND:
        case AST_BINARY_BOR:
        case AST_BINARY_BXOR:
        case AST_BINARY_LSHIFT:
        case AST_BINARY_RSHIFT:
            return xr_type_new_int(NULL);
        default:
            break;
    }

    // Arithmetic: result depends on operand types
    if (XR_TYPE_IS_UNKNOWN(left) || XR_TYPE_IS_UNKNOWN(right)) {
        return xr_type_new_unknown(NULL);
    }

    switch (node->type) {
        case AST_BINARY_ADD:
        case AST_BINARY_SUB:
        case AST_BINARY_MUL:
        case AST_BINARY_DIV:
        case AST_BINARY_MOD:
            return binary_arith_distribute(ctx, node->type, left, right);
        default:
            return xr_type_new_unknown(NULL);
    }
}

XrType *xa_visit_unary(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node)
        return xr_type_new_unknown(NULL);

    XrType *operand = xa_visit_infer_expr(ctx, node->as.unary.operand);

    switch (node->type) {
        case AST_UNARY_NEG:
            return operand;  // -x has same type as x
        case AST_UNARY_NOT:
            return xr_type_new_bool(NULL);
        case AST_UNARY_BNOT:
            return xr_type_new_int(NULL);
        default:
            return xr_type_new_unknown(NULL);
    }
}

/* ----------------------------------------------------------------------------
 * Member Access Type Inference
 * -------------------------------------------------------------------------- */
XrType *xa_visit_member_access(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node)
        return xr_type_new_unknown(NULL);

    MemberAccessNode *ma = &node->as.member_access;
    XrType *obj_type = xa_visit_infer_expr(ctx, ma->object);

    // Check module member access before the unknown-type early return (e.g., net.dial)
    if (XR_TYPE_IS_UNKNOWN(obj_type) && ma->object->type == AST_VARIABLE) {
        const char *var_name = ma->object->as.variable.name;
        XaSymbol *sym = xa_scope_lookup(ctx->analyzer->current_scope, var_name);
        if (sym && sym->kind == XA_SYM_MODULE) {
            XaSymbolLinks *sym_links = xa_analyzer_get_links(ctx->analyzer, sym);
            const char *mod_name =
                (sym_links && sym_links->module_name) ? sym_links->module_name : var_name;
            const XaBuiltinModule *mod = xa_builtin_get_module_info(mod_name);
            if (mod) {
                // Look up member (function or constant) in module type table
                const char *sig = xa_builtin_get_module_func_signature(mod_name, ma->name);
                if (sig) {
                    XrType *mod_result = NULL;
                    // Constant property: signature is ": type" (no parens)
                    if (sig[0] == ':') {
                        const char *type_str = sig + 1;
                        while (*type_str == ' ')
                            type_str++;
                        mod_result = xa_builtin_parse_type_string(ctx->analyzer->isolate, type_str);
                    } else {
                        // Function: parse complete signature (params + return type)
                        mod_result = xa_builtin_parse_full_signature(ctx->analyzer->isolate, sig);
                    }
                    if (mod_result) {
                        record_selection(ctx, node, XA_SEL_MODULE_EXPORT, obj_type, sym, -1,
                                         mod_result, false);
                    }
                    return mod_result;
                }
                // Known module but unknown member - still unknown for extensibility
                return xr_type_new_unknown(NULL);
            }
        }
    }

    // Enum static member access: EnumName.Member -> enum value of EnumName.
    // The member is checked against the declared enum_member_names so a
    // typo like Color.Yellow is flagged here rather than handed to the
    // EnumValue builtin probe below (which would also miss).
    if (obj_type->kind == XR_KIND_ENUM && obj_type->enum_type.enum_name) {
        XaSymbol *enum_sym =
            xa_scope_lookup(ctx->analyzer->current_scope, obj_type->enum_type.enum_name);
        if (enum_sym && enum_sym->kind == XA_SYM_ENUM) {
            XaSymbolLinks *el = xa_analyzer_get_links(ctx->analyzer, enum_sym);
            if (el) {
                for (int i = 0; i < el->enum_member_count; i++) {
                    if (el->enum_member_names[i] &&
                        strcmp(el->enum_member_names[i], ma->name) == 0) {
                        XrType *enum_type =
                            xr_type_new_enum(ctx->analyzer->isolate, obj_type->enum_type.enum_name);
                        record_selection(ctx, node, XA_SEL_ENUM_MEMBER, obj_type, enum_sym, i,
                                         enum_type, false);
                        return enum_type;
                    }
                }
                // Precise .value type: use the enum's actual backing type
                // instead of the generic `: Json` from the builtin table.
                if (strcmp(ma->name, "value") == 0 && el->enum_value_type) {
                    return el->enum_value_type;
                }
            }
        }
        // Fall through: name/ordinal/toString handled by EnumValue
        // builtin table below.
    }

    // Class static member access: ClassName.staticMethod
    if (obj_type->kind == XR_KIND_CLASS && obj_type->instance.class_name) {
        XaSymbol *class_sym =
            xa_scope_lookup(ctx->analyzer->current_scope, obj_type->instance.class_name);
        if (class_sym && class_sym->kind == XA_SYM_CLASS) {
            XaSymbolLinks *class_links = xa_analyzer_get_links(ctx->analyzer, class_sym);
            if (class_links && class_links->class_info) {
                XaSymbol *member = xa_class_info_lookup_member(class_links->class_info, ma->name);
                if (member) {
                    XaSymbolLinks *ml = xa_analyzer_get_links(ctx->analyzer, member);
                    if (ml && ml->type) {
                        record_selection(ctx, node, XA_SEL_STATIC_MEMBER, obj_type, member, -1,
                                         ml->type, false);
                        return ml->type;
                    }
                }
            }
        }
    }

    // Unknown preserves error recovery and IDE responsiveness after imprecise analysis.
    if (XR_TYPE_IS_UNKNOWN(obj_type)) {
        return xr_type_new_unknown(NULL);
    }

    // Union type member access: every member must declare the member with
    // compatible types. Returns the joined function/field type so callers
    // see a single coherent signature instead of `unknown`. This is what
    // makes virtual-style dispatch over `Array<Dog | Cat>` type-check.
    if (XR_TYPE_IS_UNION(obj_type)) {
        XrType *joined = NULL;
        for (int i = 0; i < obj_type->union_type.member_count; i++) {
            XrType *m = obj_type->union_type.members[i];
            if (!m)
                continue;
            XrType *member_ty = NULL;
            // Class instance: look up method/field by name in class info.
            if (XR_TYPE_IS_INSTANCE(m) && m->instance.class_name) {
                XaSymbol *cs =
                    xa_scope_lookup(ctx->analyzer->current_scope, m->instance.class_name);
                if (cs) {
                    XaSymbolLinks *cl = xa_analyzer_get_links(ctx->analyzer, cs);
                    if (cl && cl->class_info) {
                        XaSymbol *mem = xa_class_info_lookup_member(cl->class_info, ma->name);
                        if (mem) {
                            XaSymbolLinks *ml = xa_analyzer_get_links(ctx->analyzer, mem);
                            if (ml && ml->type)
                                member_ty = ml->type;
                        }
                    }
                }
            }
            if (!member_ty) {
                joined = NULL;
                break;
            }
            joined = joined ? xr_type_union(ctx->analyzer->isolate, joined, member_ty) : member_ty;
        }
        if (joined)
            return joined;
    }

    // Handle built-in properties
    SymbolId prop_sym = xr_builtin_symbol_from_name(ma->name);
    if (prop_sym == SYMBOL_LENGTH) {
        if (XR_TYPE_IS_ARRAY(obj_type) || XR_TYPE_IS_STRING(obj_type) || XR_TYPE_IS_MAP(obj_type) ||
            (obj_type->kind == XR_KIND_SET)) {
            return xr_type_new_int(NULL);
        }
    }
    if (obj_type->kind == XR_KIND_CHANNEL) {
        if (prop_sym == SYMBOL_CANCELLED)
            return xr_type_new_bool(NULL);
    }

    // Handle built-in methods (return function type for method access)
    if (xa_builtin_is_method(obj_type, ma->name)) {
        const char *sig = xa_builtin_get_member_signature(obj_type, ma->name);
        if (sig) {
            XrType *fn_type = xa_builtin_parse_full_signature(ctx->analyzer->isolate, sig);
            // Substitute generic type parameters with actual container types:
            //   Array<T>/Set<T>/Channel<T>: T -> element_type
            //   Map<K,V>: K -> key_type, V -> value_type
            if (fn_type) {
                if ((XR_TYPE_IS_ARRAY(obj_type) || obj_type->kind == XR_KIND_SET ||
                     obj_type->kind == XR_KIND_CHANNEL) &&
                    obj_type->container.element_type) {
                    const char *names[] = {"T"};
                    XrType *types[] = {obj_type->container.element_type};
                    fn_type = xr_type_substitute(ctx->analyzer->isolate, fn_type, names, types, 1);
                } else if (XR_TYPE_IS_MAP(obj_type)) {
                    XrType *kt = obj_type->map.key_type;
                    XrType *vt = obj_type->map.value_type;
                    if (kt && vt) {
                        const char *names[] = {"K", "V"};
                        XrType *types[] = {kt, vt};
                        fn_type =
                            xr_type_substitute(ctx->analyzer->isolate, fn_type, names, types, 2);
                    }
                }
            }
            return fn_type;
        }
        // Fallback: return function with unknown return type
        XrType *return_type =
            xa_builtin_get_method_return_type(ctx->analyzer->isolate, obj_type, ma->name);
        if (return_type) {
            return xr_type_new_function(ctx->analyzer->isolate, NULL, 0, return_type, false);
        }
    }

    // Built-in non-method properties (e.g. EnumValue.name/value/ordinal,
    // Channel.closed). The signature for properties is just `: T` with
    // no parameter list. Skip the method substitution machinery above
    // because there are no type-parameter container fields exposed as
    // property kind today.
    {
        const char *sig = xa_builtin_get_member_signature(obj_type, ma->name);
        if (sig && sig[0] == ':') {
            const char *type_str = sig + 1;
            while (*type_str == ' ')
                type_str++;
            return xa_builtin_parse_type_string(ctx->analyzer->isolate, type_str);
        }
    }

    // Handle class instance members
    if (XR_TYPE_IS_INSTANCE(obj_type) && obj_type->instance.class_name) {
        XaSymbol *class_sym =
            xa_scope_lookup(ctx->analyzer->current_scope, obj_type->instance.class_name);
        if (class_sym) {
            XaSymbolLinks *class_links = xa_analyzer_get_links(ctx->analyzer, class_sym);
            if (class_links && class_links->class_info) {
                XaSymbol *member = xa_class_info_lookup_member(class_links->class_info, ma->name);
                if (member) {
                    XaSymbolLinks *member_links = xa_analyzer_get_links(ctx->analyzer, member);
                    if (member_links && member_links->type) {
                        XrType *member_type = member_links->type;

                        // Apply type substitution for generic instances
                        int type_param_count = xa_symbol_links_get_type_param_count(class_links);
                        if (type_param_count > 0 && obj_type->instance.type_arg_count > 0) {
                            const char **param_names =
                                xr_malloc(sizeof(const char *) * type_param_count);
                            for (int i = 0; i < type_param_count; i++) {
                                param_names[i] =
                                    xa_symbol_links_get_type_param_name(class_links, i);
                            }
                            member_type = xr_type_substitute(
                                ctx->analyzer->isolate, member_type, param_names,
                                obj_type->instance.type_args, obj_type->instance.type_arg_count);
                            xr_free(param_names);
                        }
                        XaSelectionKind sk =
                            (member->kind == XA_SYM_METHOD) ? XA_SEL_METHOD : XA_SEL_FIELD;
                        record_selection(ctx, node, sk, obj_type, member, -1, member_type, false);
                        return member_type;
                    }
                }
            }
        }
    }

    // Handle methods on module handle types (e.g. SqliteDB.exec from .xrd).
    // The handle type is resolved as an instance type whose class_name
    // matches a registered handle; look up methods on that handle.
    if (XR_TYPE_IS_INSTANCE(obj_type) && obj_type->instance.class_name) {
        const XaBuiltinHandle *handle =
            xa_builtin_find_handle_by_name(obj_type->instance.class_name);
        if (handle) {
            // Check handle fields first
            for (int i = 0; i < handle->field_count; i++) {
                if (strcmp(handle->fields[i].name, ma->name) == 0) {
                    return xa_builtin_parse_type_string(ctx->analyzer->isolate,
                                                        handle->fields[i].type_str);
                }
            }
            // Check handle methods
            for (int i = 0; i < handle->method_count; i++) {
                if (strcmp(handle->methods[i].name, ma->name) == 0) {
                    return xa_builtin_parse_full_signature(ctx->analyzer->isolate,
                                                           handle->methods[i].signature);
                }
            }
        }
    }

    // Handle Json/Object field access.
    // Json represents any JSON value (including null), so field access returns Json.
    if (XR_TYPE_IS_JSON(obj_type) && obj_type->object.field_count == 0) {
        // Bare Json type (e.g. function parameter) — no static field info,
        // return Json since any field access is valid at runtime.
        return xr_type_new_json(ctx->analyzer->isolate);
    }
    if (XR_TYPE_IS_JSON(obj_type) && obj_type->object.field_count > 0) {
        int field_idx = json_field_index(obj_type, ma->name);
        if (field_idx >= 0 && obj_type->object.field_types) {
            XrType *ft = obj_type->object.field_types[field_idx];
            if (!ft)
                return xr_type_new_unknown(NULL);
            XrType *result_ft = xr_type_make_nullable(ctx->analyzer->isolate, ft);
            record_selection(ctx, node, XA_SEL_FIELD, obj_type, NULL, field_idx, result_ft, false);
            return result_ft;
        }
        if (obj_type->object.is_sealed) {
            XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
            char msg[256];
            snprintf(msg, sizeof(msg), "类型 '%s' 没有字段 '%s'", json_type_label(obj_type),
                     ma->name);
            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                       XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
            return xr_type_new_unknown(NULL);
        }
        return xr_type_new_unknown(NULL);
    }

    return xr_type_new_unknown(NULL);
}

XrType *xa_visit_index_get(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node)
        return xr_type_new_unknown(NULL);

    IndexGetNode *ig = &node->as.index_get;
    XrType *container = xa_visit_infer_expr(ctx, ig->array);

    /* Visit the index expression so variable references get their symbol_id resolved */
    XrType *index_type = NULL;
    if (ig->index) {
        index_type = xa_visit_infer_expr(ctx, ig->index);
    }

    if (XR_TYPE_IS_ARRAY(container) && container->container.element_type) {
        if (index_type && !XR_TYPE_IS_UNKNOWN(index_type) && !XR_TYPE_IS_INT(index_type))
            add_index_type_error(ctx, node, index_type, xr_type_new_int(NULL));
        return container->container.element_type;
    }
    if (XR_TYPE_IS_MAP(container) && container->map.value_type) {
        if (index_type && container->map.key_type && !XR_TYPE_IS_UNKNOWN(index_type) &&
            !xa_typecheck_assignable(container->map.key_type, index_type))
            add_index_type_error(ctx, node, index_type, container->map.key_type);
        return container->map.value_type;
    }
    if (XR_TYPE_IS_STRING(container)) {
        if (index_type && !XR_TYPE_IS_UNKNOWN(index_type) && !XR_TYPE_IS_INT(index_type))
            add_index_type_error(ctx, node, index_type, xr_type_new_int(NULL));
        return xr_type_new_string(NULL);  // string[i] => string
    }

    // Json subscript access: json["key"] → Json (or schema field type if known)
    if (XR_TYPE_IS_JSON(container)) {
        // If index is a string literal and Json has schema, look up field type
        if (ig->index && ig->index->type == AST_LITERAL_STRING &&
            container->object.field_count > 0 && container->object.field_names &&
            container->object.field_types) {
            const char *key = ig->index->as.literal.raw_value.string_val;
            for (int i = 0; i < container->object.field_count; i++) {
                if (container->object.field_names[i] &&
                    strcmp(container->object.field_names[i], key) == 0) {
                    XrType *ft = container->object.field_types[i];
                    if (ft)
                        return xr_type_make_nullable(ctx->analyzer->isolate, ft);
                }
            }
            if (container->object.is_sealed) {
                XrLocation loc = {
                    .file = ctx->file_path, .line = node->line, .column = node->column};
                char msg[256];
                snprintf(msg, sizeof(msg), "类型 '%s' 没有字段 '%s'", json_type_label(container),
                         key);
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
                return xr_type_new_unknown(NULL);
            }
        }
        // No schema or unknown key → result is Json (any JSON value including null)
        return xr_type_new_json(ctx->analyzer->isolate);
    }

    return xr_type_new_unknown(NULL);
}

XrType *xa_visit_array_literal(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !ctx->analyzer || !node)
        return xr_type_new_array(ctx->analyzer->isolate, xr_type_new_unknown(NULL));

    ArrayLiteralNode *arr = &node->as.array_literal;
    if (arr->count == 0) {
        // Empty array: use expected type if available
        if (ctx->expected_type && XR_TYPE_IS_ARRAY(ctx->expected_type) &&
            ctx->expected_type->container.element_type) {
            return xr_type_new_array(ctx->analyzer->isolate,
                                     ctx->expected_type->container.element_type);
        }
        return xr_type_new_array(ctx->analyzer->isolate, xr_type_new_unknown(NULL));
    }

    // Propagate expected element type to children
    XrType *saved_expected = ctx->expected_type;
    if (ctx->expected_type && XR_TYPE_IS_ARRAY(ctx->expected_type) &&
        ctx->expected_type->container.element_type) {
        ctx->expected_type = ctx->expected_type->container.element_type;
    } else {
        ctx->expected_type = NULL;
    }

    // Infer element type from first element
    XrType *elem_type = xa_visit_infer_expr(ctx, arr->elements[0]);

    // Union with other element types
    for (int i = 1; i < arr->count; i++) {
        XrType *t = xa_visit_infer_expr(ctx, arr->elements[i]);
        if (!xr_type_equals(elem_type, t)) {
            elem_type = xr_type_union(ctx->analyzer->isolate, elem_type, t);
        }
    }

    ctx->expected_type = saved_expected;
    return xr_type_new_array(ctx->analyzer->isolate, elem_type);
}

XrType *xa_visit_map_literal(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !ctx->analyzer || !node)
        return xr_type_new_map(ctx->analyzer->isolate, xr_type_new_unknown(NULL),
                               xr_type_new_unknown(NULL));

    MapLiteralNode *map = &node->as.map_literal;
    if (map->count == 0) {
        // Empty map: use expected type if available
        if (ctx->expected_type && XR_TYPE_IS_MAP(ctx->expected_type)) {
            XrType *ek = ctx->expected_type->map.key_type;
            XrType *ev = ctx->expected_type->map.value_type;
            return xr_type_new_map(ctx->analyzer->isolate, ek ? ek : xr_type_new_unknown(NULL),
                                   ev ? ev : xr_type_new_unknown(NULL));
        }
        return xr_type_new_map(ctx->analyzer->isolate, xr_type_new_unknown(NULL),
                               xr_type_new_unknown(NULL));
    }

    // Propagate expected value type to children
    XrType *saved_expected = ctx->expected_type;
    if (ctx->expected_type && XR_TYPE_IS_MAP(ctx->expected_type) &&
        ctx->expected_type->map.value_type) {
        ctx->expected_type = ctx->expected_type->map.value_type;
    } else {
        ctx->expected_type = NULL;
    }

    // Infer key/value types from first element
    XrType *key_type = xa_visit_infer_expr(ctx, map->keys[0]);
    XrType *val_type = xa_visit_infer_expr(ctx, map->values[0]);

    // Union with remaining elements (same pattern as array_literal)
    for (int i = 1; i < map->count; i++) {
        XrType *k = xa_visit_infer_expr(ctx, map->keys[i]);
        XrType *v = xa_visit_infer_expr(ctx, map->values[i]);
        if (!xr_type_equals(key_type, k)) {
            key_type = xr_type_union(ctx->analyzer->isolate, key_type, k);
        }
        if (!xr_type_equals(val_type, v)) {
            val_type = xr_type_union(ctx->analyzer->isolate, val_type, v);
        }
    }

    ctx->expected_type = saved_expected;
    return xr_type_new_map(ctx->analyzer->isolate, key_type, val_type);
}

XrType *xa_visit_object_literal(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node)
        return xr_type_new_json(NULL);

    ObjectLiteralNode *obj = &node->as.object_literal;
    if (obj->count == 0)
        return xr_type_new_json(NULL);

    // Collect field names and types
    const char **field_names = (const char **) xr_malloc(sizeof(char *) * obj->count);
    XrType **field_types = (XrType **) xr_malloc(sizeof(XrType *) * obj->count);

    for (int i = 0; i < obj->count; i++) {
        // Get field name (computed properties have runtime-only keys)
        bool is_computed = obj->computed && obj->computed[i];
        if (is_computed) {
            field_names[i] = NULL;
        } else if (obj->keys[i] && obj->keys[i]->type == AST_VARIABLE) {
            field_names[i] = obj->keys[i]->as.variable.name;
        } else if (obj->keys[i] && obj->keys[i]->type == AST_LITERAL_STRING) {
            field_names[i] = obj->keys[i]->as.literal.raw_value.string_val;
        } else {
            field_names[i] = NULL;
        }

        // Infer field type
        field_types[i] = xa_visit_infer_expr(ctx, obj->values[i]);

        // Warn (not error) for non-serializable types in object literals.
        // Object literals can hold any value at runtime, but non-JSON types
        // will cause Json.stringify() to throw at runtime.
        if (field_types[i] && !xr_type_is_json_field_compatible(field_types[i])) {
            XrLocation loc = {.file = ctx->file_path,
                              .line = obj->values[i]->line,
                              .column = obj->values[i]->column};
            char msg[256];
            const char *fname = field_names[i] ? field_names[i] : "<computed>";
            snprintf(msg, sizeof(msg),
                     "field '%s' has type '%s' which is not JSON-serializable; "
                     "Json.stringify() will throw at runtime",
                     fname, xr_type_to_string(field_types[i]));
            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_WARNING,
                                       XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
        }
    }

    XrType *type = xr_type_new_json_with_fields(ctx->analyzer->isolate, field_names, field_types,
                                                obj->count, false);
    xr_free(field_names);
    xr_free(field_types);

    return type;
}

XrType *xa_visit_new_expr(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node)
        return xr_type_new_unknown(NULL);

    NewExprNode *ne = &node->as.new_expr;

    /* Visit argument expressions so their types are resolved. */
    for (int i = 0; i < ne->arg_count; i++) {
        if (ne->arguments[i])
            xa_visit_infer_expr(ctx, ne->arguments[i]);
    }

    /* Builtin heap types: return the correct container/channel type
     * directly, bypassing class-symbol lookup. Supports explicit
     * type arguments: new Map<string, int>(), new Channel<int>(). */
    if (ne->class_name && !ne->module_name) {
        XrayIsolate *X = ctx->analyzer->isolate;
        const char *cn = ne->class_name;
        XrType *bt = NULL;

        /* Resolve explicit type arguments if present */
        XrType *ta[8] = {0};
        int tac = ne->type_arg_count > 8 ? 8 : ne->type_arg_count;
        for (int i = 0; i < tac; i++)
            ta[i] =
                ne->type_args[i] ? xr_tref_resolve(X, ne->type_args[i]) : xr_type_new_unknown(NULL);

        if (strcmp(cn, "Map") == 0 || strcmp(cn, "WeakMap") == 0) {
            XrType *kt = tac >= 1 ? ta[0] : xr_type_new_unknown(X);
            XrType *vt = tac >= 2 ? ta[1] : xr_type_new_unknown(X);
            bt = xr_type_new_map(X, kt, vt);
            if (strcmp(cn, "WeakMap") == 0)
                bt->is_weak = true;
        } else if (strcmp(cn, "Array") == 0) {
            XrType *et = tac >= 1 ? ta[0] : xr_type_new_unknown(X);
            bt = xr_type_new_array(X, et);
        } else if (strcmp(cn, "Set") == 0 || strcmp(cn, "WeakSet") == 0) {
            XrType *et = tac >= 1 ? ta[0] : xr_type_new_unknown(X);
            bt = xr_type_new(X, XR_KIND_SET);
            if (bt) {
                bt->container.element_type = et;
                if (strcmp(cn, "WeakSet") == 0)
                    bt->is_weak = true;
            }
        } else if (strcmp(cn, "Bytes") == 0) {
            bt = xr_type_new_bytes(X);
        } else if (strcmp(cn, "Channel") == 0) {
            XrType *et = tac >= 1 ? ta[0] : xr_type_new_unknown(X);
            bt = xr_type_new(X, XR_KIND_CHANNEL);
            if (bt)
                bt->container.element_type = et;
        } else if (strcmp(cn, "StringBuilder") == 0) {
            bt = xr_type_new_named_instance(X, "StringBuilder");
        }
        if (bt)
            return bt;
    }

    // Look up class symbol to get XrClassInfo
    // Try current scope first, then global scope
    XaSymbol *class_sym = xa_scope_lookup(ctx->analyzer->current_scope, ne->class_name);
    if (!class_sym) {
        class_sym = xa_scope_lookup(ctx->analyzer->global_scope, ne->class_name);
    }

    XrClassInfo *class_info = NULL;
    XaSymbolLinks *class_links = NULL;
    if (class_sym && class_sym->kind == XA_SYM_CLASS) {
        class_links = xa_analyzer_get_links(ctx->analyzer, class_sym);
        if (class_links && class_links->class_info) {
            class_info = class_links->class_info;
        }
    }

    // Check type argument count matches type parameter count
    if (ne->type_arg_count > 0) {
        int expected = class_links ? xa_symbol_links_get_type_param_count(class_links) : 0;
        if (expected > 0 && ne->type_arg_count != expected) {
            XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
            char msg[256];
            snprintf(msg, sizeof(msg), "Generic class '%s' expects %d type argument(s), but got %d",
                     ne->class_name, expected, ne->type_arg_count);
            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                       XR_ERR_ANALYZE_GENERIC_COUNT, msg, &loc);
        }
    }

    // If we have generic type arguments, create a generic instance type
    if (ne->type_arg_count > 0) {
        // Resolve XrTypeRef** to XrType** for runtime use
        XrType *resolved_targs_buf[8];
        XrType **resolved_targs = (ne->type_arg_count <= 8)
                                      ? resolved_targs_buf
                                      : xr_malloc(sizeof(XrType *) * (size_t) ne->type_arg_count);
        for (int i = 0; i < ne->type_arg_count; i++)
            resolved_targs[i] = ne->type_args[i]
                                    ? xr_tref_resolve(ctx->analyzer->isolate, ne->type_args[i])
                                    : xr_type_new_unknown(NULL);

        // Check constructor argument types against substituted parameter types
        if (class_info && class_links && ne->arg_count > 0) {
            int type_param_count = xa_symbol_links_get_type_param_count(class_links);
            if (type_param_count > 0 && type_param_count == ne->type_arg_count) {
                XaSymbol *ctor = xa_class_info_lookup_member(class_info, XR_KEYWORD_CONSTRUCTOR);
                if (ctor && ctor->kind == XA_SYM_METHOD) {
                    XaSymbolLinks *ctor_links = xa_analyzer_get_links(ctx->analyzer, ctor);
                    if (ctor_links && ctor_links->type && XR_TYPE_IS_FUNCTION(ctor_links->type)) {
                        // Build param_names array
                        const char *param_names_buf[8];
                        const char **param_names =
                            (type_param_count <= 8)
                                ? param_names_buf
                                : xr_malloc(sizeof(const char *) * type_param_count);
                        for (int i = 0; i < type_param_count; i++) {
                            param_names[i] = xa_symbol_links_get_type_param_name(class_links, i);
                        }

                        int ctor_pc = ctor_links->type->function.param_count;
                        XrType **ctor_params = ctor_links->type->function.param_types;
                        int check_count = ctor_pc < ne->arg_count ? ctor_pc : ne->arg_count;

                        for (int i = 0; i < check_count; i++) {
                            XrType *expected = ctor_params ? ctor_params[i] : NULL;
                            if (!expected || XR_TYPE_IS_UNKNOWN(expected))
                                continue;
                            // Substitute T -> actual type arg
                            XrType *resolved =
                                xr_type_substitute(ctx->analyzer->isolate, expected, param_names,
                                                   resolved_targs, ne->type_arg_count);
                            if (resolved && !XR_TYPE_IS_UNKNOWN(resolved)) {
                                XrType *arg_type = xa_visit_infer_expr(ctx, ne->arguments[i]);
                                if (arg_type && !xa_typecheck_assignable(resolved, arg_type)) {
                                    XrLocation loc = {.file = ctx->file_path,
                                                      .line = node->line,
                                                      .column = node->column};
                                    char msg[256];
                                    snprintf(msg, sizeof(msg),
                                             "Type '%s' is not assignable to parameter type '%s' "
                                             "in new %s<>()",
                                             xr_type_to_string(arg_type),
                                             xr_type_to_string(resolved), ne->class_name);
                                    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                                               XR_ERR_ANALYZE_TYPE_MISMATCH, msg,
                                                               &loc);
                                }
                            }
                        }
                        if (param_names != param_names_buf)
                            xr_free((void *) param_names);
                    }
                }
            }
        }
        XrType *gi = xr_type_new_generic_instance(ctx->analyzer->isolate, ne->class_name,
                                                  class_info, resolved_targs, ne->type_arg_count);
        if (resolved_targs != resolved_targs_buf)
            xr_free(resolved_targs);
        return gi;
    }

    // Infer type arguments from constructor parameters: new Box(42) -> Box<int>
    if (class_links && ne->arg_count > 0) {
        int type_param_count = xa_symbol_links_get_type_param_count(class_links);
        if (type_param_count > 0 && class_info) {
            // Look up constructor
            XaSymbol *ctor = xa_class_info_lookup_member(class_info, XR_KEYWORD_CONSTRUCTOR);
            if (ctor && ctor->kind == XA_SYM_METHOD) {
                XaSymbolLinks *ctor_links = xa_analyzer_get_links(ctx->analyzer, ctor);
                if (ctor_links && ctor_links->type && XR_TYPE_IS_FUNCTION(ctor_links->type)) {
                    XrType **ctor_params = ctor_links->type->function.param_types;
                    int ctor_param_count = ctor_links->type->function.param_count;

                    // Build type parameter names
                    const char **param_names = xr_malloc(sizeof(const char *) * type_param_count);
                    for (int i = 0; i < type_param_count; i++) {
                        param_names[i] = xa_symbol_links_get_type_param_name(class_links, i);
                    }

                    // Infer type arguments from constructor arguments
                    XrType **inferred_args = xr_malloc(sizeof(XrType *) * type_param_count);
                    bool all_inferred = true;

                    for (int i = 0; i < type_param_count; i++) {
                        inferred_args[i] = NULL;
                        const char *tp_name = param_names[i];

                        // Find constructor parameter that uses this type parameter
                        for (int j = 0; j < ctor_param_count && j < ne->arg_count; j++) {
                            XrType *pt = ctor_params ? ctor_params[j] : NULL;
                            if (pt && (pt->kind == XR_KIND_TYPE_PARAM) && pt->type_param.name &&
                                strcmp(pt->type_param.name, tp_name) == 0) {
                                // Infer from argument type
                                inferred_args[i] = xa_visit_infer_expr(ctx, ne->arguments[j]);
                                break;
                            }
                        }

                        if (!inferred_args[i]) {
                            all_inferred = false;
                        }
                    }

                    // If all type parameters were inferred, create generic instance
                    if (all_inferred) {
                        XrType *result = xr_type_new_generic_instance(
                            ctx->analyzer->isolate, ne->class_name, class_info, inferred_args,
                            type_param_count);
                        xr_free(param_names);
                        // Don't free inferred_args - it's now owned by the type
                        return result;
                    }

                    xr_free(param_names);
                    xr_free(inferred_args);
                }
            }
        }
    }

    // No type arguments - use regular instance or class type
    if (class_info) {
        XrType *inst_type = xr_type_new_instance(ctx->analyzer->isolate, class_info);
        // Propagate is_value_type from class declaration (struct)
        if (class_links && class_links->type && class_links->type->is_value_type) {
            inst_type->is_value_type = true;
        }
        return inst_type;
    }

    // Fallback: create instance type with class name (new always produces instances)
    if (ne->class_name) {
        XrType *inst_type = xr_type_new_named_instance(ctx->analyzer->isolate, ne->class_name);
        // Propagate is_value_type from class declaration (struct)
        if (class_links && class_links->type && class_links->type->is_value_type) {
            inst_type->is_value_type = true;
        }
        return inst_type;
    }
    return xr_type_new_unknown(NULL);
}

XrType *xa_visit_struct_literal(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node)
        return xr_type_new_unknown(NULL);

    StructLiteralNode *sl = &node->as.struct_literal;
    const char *struct_name = sl->struct_name;

    // Infer field value types (for side effects / type checking)
    for (int i = 0; i < sl->field_count; i++) {
        xa_visit_infer_expr(ctx, sl->field_values[i]);
    }

    // Look up struct symbol
    XaSymbol *class_sym = xa_scope_lookup(ctx->analyzer->current_scope, struct_name);
    if (!class_sym) {
        class_sym = xa_scope_lookup(ctx->analyzer->global_scope, struct_name);
    }

    if (class_sym && class_sym->kind == XA_SYM_CLASS) {
        XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, class_sym);
        if (links && links->class_info) {
            XrType *inst_type = xr_type_new_instance(ctx->analyzer->isolate, links->class_info);
            if (links->type && links->type->is_value_type) {
                inst_type->is_value_type = true;
            }
            return inst_type;
        }
        if (links && links->type) {
            return links->type;
        }
    }

    if (struct_name) {
        XrType *t = xr_type_new_class(ctx->analyzer->isolate, struct_name);
        t->is_value_type = true;
        return t;
    }
    return xr_type_new_unknown(NULL);
}

XrType *xa_visit_ternary(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node)
        return xr_type_new_unknown(NULL);

    TernaryNode *tern = &node->as.ternary;
    // Visit condition to resolve variable symbol_ids
    xa_visit_infer_expr(ctx, tern->condition);
    // Bidirectional inference: propagate outer expected_type to both branches
    // (expected_type is already set by the caller, just pass through)
    XrType *then_type = xa_visit_infer_expr(ctx, tern->true_expr);
    XrType *else_type = xa_visit_infer_expr(ctx, tern->false_expr);

    if (xr_type_equals(then_type, else_type)) {
        return then_type;
    }
    return xr_type_union(ctx->analyzer->isolate, then_type, else_type);
}

/* ----------------------------------------------------------------------------
 * Nullish Coalesce: a ?? b
 * If a is T?, result is T | typeof(b). If a is T (non-nullable), result is T.
 * -------------------------------------------------------------------------- */
XrType *xa_visit_nullish_coalesce(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node)
        return xr_type_new_unknown(NULL);

    XrType *left = xa_visit_infer_expr(ctx, node->as.binary.left);
    XrType *right = xa_visit_infer_expr(ctx, node->as.binary.right);

    // If left is nullable (T?), strip null and union with right
    // T? ?? U => T | U (most common: T? ?? T => T)
    XrType *non_null_left = xr_type_non_nullable(ctx->analyzer->isolate, left);
    if (non_null_left != left) {
        // left was nullable, result is non-null version unified with right
        if (xr_type_equals(non_null_left, right)) {
            return non_null_left;
        }
        return xr_type_union(ctx->analyzer->isolate, non_null_left, right);
    }

    // Left is not nullable, ?? is a no-op, return left type
    return left;
}

/* ----------------------------------------------------------------------------
 * Optional Chain: obj?.prop, obj?.[index], obj?.method()
 * Result is always nullable: typeof(obj.prop) | null => T?
 * -------------------------------------------------------------------------- */
XrType *xa_visit_optional_chain(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node)
        return xr_type_new_unknown(NULL);

    XrType *obj_type = xa_visit_infer_expr(ctx, node->as.optional_chain.object);

    // If object is unknown, result is unknown
    if (XR_TYPE_IS_UNKNOWN(obj_type)) {
        return xr_type_new_unknown(NULL);
    }

    // Strip nullable from object for member lookup
    XrType *base_type = xr_type_non_nullable(ctx->analyzer->isolate, obj_type);

    // Property access: obj?.name
    if (node->as.optional_chain.name) {
        // Reuse member access logic by creating a temporary lookup
        // For now, handle common cases inline
        const char *prop_name = node->as.optional_chain.name;

        // Built-in properties — result is nullable (object may be null)
        if ((XR_TYPE_IS_ARRAY(base_type) || XR_TYPE_IS_STRING(base_type)) &&
            xr_builtin_symbol_from_name(prop_name) == SYMBOL_LENGTH) {
            return xr_type_make_nullable(ctx->analyzer->isolate, xr_type_new_int(NULL));
        }

        // Class instance member
        if (XR_TYPE_IS_INSTANCE(base_type) && base_type->instance.class_name) {
            XaSymbol *class_sym =
                xa_scope_lookup(ctx->analyzer->global_scope, base_type->instance.class_name);
            if (class_sym && class_sym->kind == XA_SYM_CLASS) {
                XaSymbolLinks *class_links = xa_analyzer_get_links(ctx->analyzer, class_sym);
                if (class_links && class_links->class_info) {
                    XaSymbol *member =
                        xa_class_info_lookup_member(class_links->class_info, prop_name);
                    if (member) {
                        XaSymbolLinks *ml = xa_analyzer_get_links(ctx->analyzer, member);
                        if (ml && ml->type) {
                            XrType *result = xr_type_copy(ctx->analyzer->isolate, ml->type);
                            if (result)
                                result->is_nullable = true;
                            return result;
                        }
                    }
                }
            }
        }

        // Json field access
        if (XR_TYPE_IS_JSON(base_type) && base_type->object.field_count > 0) {
            for (int i = 0; i < base_type->object.field_count; i++) {
                if (base_type->object.field_names[i] &&
                    strcmp(base_type->object.field_names[i], prop_name) == 0) {
                    XrType *result =
                        xr_type_copy(ctx->analyzer->isolate, base_type->object.field_types[i]);
                    if (result)
                        result->is_nullable = true;
                    return result;
                }
            }
        }
    }

    // Index access: obj?.[index]
    if (node->as.optional_chain.index) {
        xa_visit_infer_expr(ctx, node->as.optional_chain.index);

        if (XR_TYPE_IS_ARRAY(base_type) && base_type->container.element_type) {
            XrType *result =
                xr_type_copy(ctx->analyzer->isolate, base_type->container.element_type);
            if (result)
                result->is_nullable = true;
            return result;
        }
        if (XR_TYPE_IS_MAP(base_type) && base_type->map.value_type) {
            XrType *result = xr_type_copy(ctx->analyzer->isolate, base_type->map.value_type);
            if (result)
                result->is_nullable = true;
            return result;
        }
    }

    // Fallback: any?
    return xr_type_new_unknown(NULL);
}

/* as type cast: expr as T — returns T (non-safe), or T? (safe)
 * Analyzer simply returns the target type without checking operand type.
 * Runtime will perform the actual type check. */
XrType *xa_visit_as_expr(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node)
        return xr_type_new_unknown(NULL);
    // Visit operand to ensure it's analyzed (side effects, narrowing)
    xa_visit_infer_expr(ctx, node->as.as_expr.expr);
    XrType *target = node->as.as_expr.type
                         ? xr_tref_resolve(ctx->analyzer->isolate, node->as.as_expr.type)
                         : NULL;
    if (!target)
        return xr_type_new_unknown(NULL);
    return target;
}

/* Force unwrap: expr! strips nullable from T? to produce T.
 * If operand is already non-nullable, the ! is a no-op (no warning for now).
 * If operand is null type, the ! is always a runtime panic. */
XrType *xa_visit_force_unwrap(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node)
        return xr_type_new_unknown(NULL);
    XrType *inner = xa_visit_infer_expr(ctx, node->as.unary.operand);
    if (!inner)
        return xr_type_new_unknown(NULL);
    // Strip nullable: T? -> T
    if (inner->is_nullable) {
        return xr_type_non_nullable(ctx->analyzer->isolate, inner);
    }
    // Already non-nullable or any: return as-is
    return inner;
}

XrType *xa_visit_function_expr(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !ctx->analyzer || !node)
        return xr_type_new_function(NULL, NULL, 0, xr_type_new_unknown(NULL), false);

    FunctionDeclNode *fn = &node->as.function_expr;
    const char **type_param_names = NULL;
    const char *type_param_buf[8];
    if (fn->type_param_count > 0 && fn->type_params) {
        type_param_names = (fn->type_param_count <= 8)
                               ? type_param_buf
                               : xr_malloc(sizeof(const char *) * fn->type_param_count);
        if (type_param_names) {
            for (int i = 0; i < fn->type_param_count; i++)
                type_param_names[i] = fn->type_params[i] ? fn->type_params[i]->name : NULL;
        }
    }

    // Check if has rest parameter
    bool has_rest = false;
    for (int i = 0; i < fn->param_count; i++) {
        if (fn->params[i] && fn->params[i]->is_rest) {
            has_rest = true;
            break;
        }
    }

    // Extract expected function type for bidirectional inference
    XrType *expected_fn = NULL;
    if (ctx->expected_type && XR_TYPE_IS_FUNCTION(ctx->expected_type)) {
        expected_fn = ctx->expected_type;
    }

    XrType **param_types = NULL;
    if (fn->param_count > 0) {
        param_types = xr_malloc(sizeof(XrType *) * fn->param_count);
        for (int i = 0; i < fn->param_count; i++) {
            XrParamNode *p = fn->params[i];
            // Check for explicit type annotation first
            if (p && p->type) {
                param_types[i] = xr_tref_resolve(ctx->analyzer->isolate, p->type);
                if (type_param_names) {
                    param_types[i] =
                        resolve_class_to_type_param(ctx->analyzer->isolate, param_types[i],
                                                    type_param_names, fn->type_param_count);
                }
            }
            // Use expected function type (bidirectional inference)
            else if (expected_fn && i < expected_fn->function.param_count &&
                     expected_fn->function.param_types[i]) {
                param_types[i] = expected_fn->function.param_types[i];
            }
            // Use generic inference from callback context
            else if (i == 0 && ctx->callback_accumulator_type) {
                param_types[i] = ctx->callback_accumulator_type;
            } else if (i == 0 && ctx->callback_element_type) {
                param_types[i] = ctx->callback_element_type;
            } else if (i == 1 && ctx->callback_accumulator_type && ctx->callback_element_type) {
                param_types[i] = ctx->callback_element_type;
            } else if (i == 1 && ctx->callback_index_type) {
                param_types[i] = ctx->callback_index_type;
            } else if (i == 2 && ctx->callback_accumulator_type && ctx->callback_index_type) {
                param_types[i] = ctx->callback_index_type;
            } else if (i == 2 && ctx->callback_array_type) {
                param_types[i] = ctx->callback_array_type;
            } else if (i == 3 && ctx->callback_array_type) {
                param_types[i] = ctx->callback_array_type;
            } else {
                // Cannot infer parameter type — report error
                param_types[i] = xr_type_new_unknown(NULL);
                if (p && p->name && !p->is_rest) {
                    XrLocation loc = {.file = ctx->file_path, .line = p->line, .column = p->column};
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                             "Parameter '%s' of anonymous function cannot be inferred, add "
                             "explicit type annotation",
                             p->name);
                    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                               XR_ERR_ANALYZE_MISSING_TYPE, msg, &loc);
                }
            }
        }
    }

    // Use expected return type if not explicitly declared
    XrType *return_type = fn->return_type ? xr_tref_resolve(ctx->analyzer->isolate, fn->return_type)
                                          : xr_type_new_unknown(NULL);
    if (fn->return_type && type_param_names) {
        return_type = resolve_class_to_type_param(ctx->analyzer->isolate, return_type,
                                                  type_param_names, fn->type_param_count);
    }
    if (XR_TYPE_IS_UNKNOWN(return_type) && expected_fn && expected_fn->function.return_type) {
        return_type = expected_fn->function.return_type;
    }

    // Enter function scope, register params, and analyze body.
    // This is required even when return_type is already known (from
    // expected context) — the body visit resolves variable symbol_ids
    // and validates scope constraints.  Without it, captured variables
    // get symbol_id=0 and upvalue resolution fails.
    bool need_return_infer = XR_TYPE_IS_UNKNOWN(return_type);
    if (fn->body) {
        // When visiting purely for symbol resolution (return type already
        // known), suppress diagnostics: closure bodies execute lazily, so
        // definite-assignment checks produce false positives for variables
        // from enclosing scopes that are assigned after the closure literal.
        int saved_diag_count = ctx->analyzer->diagnostic_count;
        XaDiagnostic *saved_diag_tail = ctx->analyzer->diagnostics_tail;

        // Save outer function's return type collection state
        XrType **saved_return_types = ctx->return_types;
        int saved_return_count = ctx->return_type_count;
        int saved_return_cap = ctx->return_type_capacity;
        ctx->return_types = NULL;
        ctx->return_type_count = 0;
        ctx->return_type_capacity = 0;

        // Isolate flow graph for lambda body (same reason as named functions)
        XaFlowNode *saved_flow = NULL;
        XrFlowLabel *saved_break = NULL;
        XrFlowLabel *saved_continue = NULL;
        XrFlowLabel *saved_return_tgt = NULL;
        XrFlowLabel *saved_exception = NULL;
        if (ctx->flow) {
            saved_flow = ctx->flow->current_flow;
            saved_break = ctx->flow->current_break_target;
            saved_continue = ctx->flow->current_continue_target;
            saved_return_tgt = ctx->flow->current_return_target;
            saved_exception = ctx->flow->current_exception_target;
            xa_flow_create_start(ctx->flow);
            ctx->flow->current_break_target = NULL;
            ctx->flow->current_continue_target = NULL;
            ctx->flow->current_return_target = NULL;
            ctx->flow->current_exception_target = NULL;
        }

        // Enter function scope
        xa_analyzer_enter_scope(ctx->analyzer, XA_SCOPE_FUNCTION, node);

        // Register parameters in scope (with their inferred types)
        for (int i = 0; i < fn->param_count; i++) {
            XrParamNode *p = fn->params[i];
            if (p && p->name) {
                XaSymbol *param_sym = xa_symbol_new(p->name, XA_SYM_PARAMETER);
                param_sym->location.line = p->line > 0 ? p->line : node->line;
                xa_scope_add_symbol(ctx->analyzer->current_scope, param_sym);
                p->symbol_id = param_sym->id;
                XaSymbolLinks *pl = xa_analyzer_get_links(ctx->analyzer, param_sym);
                pl->type = param_types ? param_types[i] : xr_type_new_unknown(NULL);
                pl->is_definitely_assigned = true;
            }
        }

        // Set expected return type for bidirectional inference on return stmts.
        // If contextual type provides a return type, use it for checking.
        // Otherwise, reset to NULL so the outer function's expected_return_type
        // doesn't leak into this closure (e.g. void from enclosing fn).
        XrType *saved_expected_ret = ctx->expected_return_type;
        if (expected_fn && expected_fn->function.return_type &&
            !XR_TYPE_IS_UNKNOWN(expected_fn->function.return_type)) {
            ctx->expected_return_type = expected_fn->function.return_type;
        } else if (fn->return_type) {
            ctx->expected_return_type = return_type;
        } else {
            ctx->expected_return_type = NULL;
        }

        // Unified body visitor: idempotent collect + direct traversal
        xa_visit_function_body_unified(ctx, fn->body);

        ctx->expected_return_type = saved_expected_ret;

        // Compute unified return type from all collected return statements
        if (need_return_infer) {
            XrType *inferred_ret = xa_infer_compute_return_type(ctx);
            if (inferred_ret && !XR_TYPE_IS_UNKNOWN(inferred_ret)) {
                return_type = inferred_ret;
            }
        }

        xa_analyzer_exit_scope(ctx->analyzer);

        // Discard diagnostics from symbol-resolution-only body visit
        if (!need_return_infer && ctx->analyzer->diagnostic_count > saved_diag_count) {
            XaDiagnostic *first_new =
                saved_diag_tail ? saved_diag_tail->next : ctx->analyzer->diagnostics;
            XaDiagnostic *d = first_new;
            while (d) {
                XaDiagnostic *next = d->next;
                if (d->message)
                    xr_free((void *) d->message);
                xr_free(d);
                d = next;
            }
            if (saved_diag_tail) {
                saved_diag_tail->next = NULL;
            } else {
                ctx->analyzer->diagnostics = NULL;
            }
            ctx->analyzer->diagnostics_tail = saved_diag_tail;
            ctx->analyzer->diagnostic_count = saved_diag_count;
        }

        // Restore flow state to enclosing function's context
        if (ctx->flow) {
            ctx->flow->current_flow = saved_flow;
            ctx->flow->current_break_target = saved_break;
            ctx->flow->current_continue_target = saved_continue;
            ctx->flow->current_return_target = saved_return_tgt;
            ctx->flow->current_exception_target = saved_exception;
        }

        // Restore outer function's return type state
        if (ctx->return_types)
            xr_free(ctx->return_types);
        ctx->return_types = saved_return_types;
        ctx->return_type_count = saved_return_count;
        ctx->return_type_capacity = saved_return_cap;
    }

    // After full body analysis, report error if return type still unknown
    if (XR_TYPE_IS_UNKNOWN(return_type) && fn->body && xa_body_has_return_expr(fn->body)) {
        XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
        xa_analyzer_add_diagnostic(
            ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_MISSING_TYPE,
            "Anonymous function returns a value but return type cannot be inferred, "
            "add explicit type annotation",
            &loc);
    }

    XrType *result = xr_type_new_function(ctx->analyzer->isolate, param_types, fn->param_count,
                                          return_type, has_rest);
    if (result) {
        result->function.min_params = fn->required_count;
        bool has_modes = false;
        for (int i = 0; i < fn->param_count && !has_modes; i++) {
            if (fn->params[i] && fn->params[i]->passing_mode != XR_PARAM_VALUE)
                has_modes = true;
        }
        if (has_modes) {
            uint8_t *modes = xr_calloc(fn->param_count, sizeof(uint8_t));
            if (modes) {
                for (int i = 0; i < fn->param_count; i++) {
                    if (fn->params[i])
                        modes[i] = fn->params[i]->passing_mode;
                }
                result->function.param_passing_modes = modes;
            }
        }
    }
    xa_set_function_type_params_from_ast(ctx, result, fn->type_params, fn->type_param_count);

    if (param_types)
        xr_free(param_types);
    if (type_param_names && type_param_names != type_param_buf)
        xr_free((void *) type_param_names);
    return result;
}

/* ----------------------------------------------------------------------------
 * Coroutine Closure Capture Validation
 * Ensures coroutines don't capture non-shared variables unsafely.
 * Function arguments are deep-copied (safe), but closure captures need checking.
 * -------------------------------------------------------------------------- */
void check_closure_capture(XaInferContext *ctx, AstNode *node, int line) {
    if (!node)
        return;

    switch (node->type) {
        case AST_VARIABLE: {
            const char *name = node->as.variable.name;
            XaSymbol *sym = xa_scope_lookup(ctx->analyzer->current_scope, name);
            if (sym && sym->kind != XA_SYM_PARAMETER) {
                // Check if variable is shared, builtin, or a type (function/class)
                if (!sym->is_shared && !sym->is_builtin && sym->kind != XA_SYM_FUNCTION &&
                    sym->kind != XA_SYM_CLASS && sym->kind != XA_SYM_MODULE &&
                    sym->kind != XA_SYM_IMPORT) {
                    // Non-shared variable captured in coroutine closure
                    XrLocation loc = {.file = ctx->file_path, .line = line, .column = node->column};
                    char msg[512];
                    snprintf(msg, sizeof(msg),
                             "go closure cannot capture mutable variable '%s'\n"
                             "hint: use one of the following:\n"
                             "  1. pass through argument: go worker(%s)  // deep-copied\n"
                             "  2. declare as 'shared const %s = ...' for concurrent reads",
                             name, name, name);
                    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                               XR_ERR_ANALYZE_CLOSURE_CAPTURE, msg, &loc);
                }
            }
            break;
        }
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
            check_closure_capture(ctx, node->as.binary.left, line);
            check_closure_capture(ctx, node->as.binary.right, line);
            break;
        case AST_UNARY_NEG:
        case AST_UNARY_NOT:
        case AST_UNARY_BNOT:
            check_closure_capture(ctx, node->as.unary.operand, line);
            break;
        case AST_CALL_EXPR:
            // Only check callee, not arguments (arguments are deep-copied, safe)
            check_closure_capture(ctx, node->as.call_expr.callee, line);
            break;
        case AST_MEMBER_ACCESS:
            check_closure_capture(ctx, node->as.member_access.object, line);
            break;
        case AST_FUNCTION_EXPR:
            // Nested closure - check its body for captured variables
            if (node->as.function_expr.body) {
                check_closure_capture(ctx, node->as.function_expr.body, line);
            }
            break;
        case AST_BLOCK:
            for (int i = 0; i < node->as.block.count; i++) {
                check_closure_capture(ctx, node->as.block.statements[i], line);
            }
            break;
        case AST_VAR_DECL:
        case AST_CONST_DECL:
            // Check initializer
            if (node->as.var_decl.initializer) {
                check_closure_capture(ctx, node->as.var_decl.initializer, line);
            }
            break;
        case AST_ASSIGNMENT:
            check_closure_capture(ctx, node->as.assignment.value, line);
            break;
        case AST_IF_STMT:
            check_closure_capture(ctx, node->as.if_stmt.condition, line);
            check_closure_capture(ctx, node->as.if_stmt.then_branch, line);
            if (node->as.if_stmt.else_branch) {
                check_closure_capture(ctx, node->as.if_stmt.else_branch, line);
            }
            break;
        case AST_WHILE_STMT:
            check_closure_capture(ctx, node->as.while_stmt.condition, line);
            check_closure_capture(ctx, node->as.while_stmt.body, line);
            break;
        case AST_FOR_STMT:
            if (node->as.for_stmt.initializer) {
                check_closure_capture(ctx, node->as.for_stmt.initializer, line);
            }
            if (node->as.for_stmt.condition) {
                check_closure_capture(ctx, node->as.for_stmt.condition, line);
            }
            if (node->as.for_stmt.increment) {
                check_closure_capture(ctx, node->as.for_stmt.increment, line);
            }
            check_closure_capture(ctx, node->as.for_stmt.body, line);
            break;
        case AST_RETURN_STMT:
            for (int i = 0; i < node->as.return_stmt.value_count; i++) {
                check_closure_capture(ctx, node->as.return_stmt.values[i], line);
            }
            break;
        default:
            break;
    }
}

// Check coroutine expression for closure capture issues
// go fn(args) - args are passed by deep copy (safe)
// go { ... }  - closure captures need checking
void check_coro_capture(XaInferContext *ctx, AstNode *node, int line) {
    if (!node)
        return;

    // go fn(args) - function call with arguments passed by value (safe)
    if (node->type == AST_CALL_EXPR) {
        // Don't check arguments - they are deep copied
        // Only check if callee itself is a closure that captures variables
        CallExprNode *call = &node->as.call_expr;
        if (call->callee && call->callee->type == AST_FUNCTION_EXPR) {
            // go (fn() { ... })(args) - check the closure body
            check_closure_capture(ctx, call->callee->as.function_expr.body, line);
        }
        return;
    }

    // go { ... } - block or closure, check for captured variables
    if (node->type == AST_FUNCTION_EXPR) {
        if (node->as.function_expr.body) {
            check_closure_capture(ctx, node->as.function_expr.body, line);
        }
        return;
    }

    // go block { ... }
    if (node->type == AST_BLOCK) {
        check_closure_capture(ctx, node, line);
        return;
    }
}

XrType *xa_visit_go_expr(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !ctx->analyzer || !node)
        return xr_type_new_task(ctx->analyzer->isolate, xr_type_new_unknown(NULL));

    GoExprNode *go = &node->as.go_expr;

    // Infer the type of the expression being spawned
    XrType *result_type = xr_type_new_unit(NULL);
    if (go->expr) {
        XrType *expr_type = xa_visit_infer_expr(ctx, go->expr);
        // If spawning a function call, get its return type
        if (XR_TYPE_IS_FUNCTION(expr_type) && expr_type->function.return_type) {
            result_type = expr_type->function.return_type;
        } else if (!XR_TYPE_IS_FUNCTION(expr_type)) {
            // Direct expression result
            result_type = expr_type;
        }

        // Check coroutine closure capture rules
        // Coroutine closures can only capture shared variables
        // Regular variables must be passed as arguments (deep copy)
        check_coro_capture(ctx, go->expr, node->line);
    }

    // go expr returns Task<T> where T is the result type
    return xr_type_new_task(ctx->analyzer->isolate, result_type);
}

XrType *xa_visit_await_expr(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node)
        return xr_type_new_unknown(NULL);

    AwaitExprNode *await = &node->as.await_expr;

    // Infer the type of the awaited expression
    if (await->expr) {
        XrType *expr_type = xa_visit_infer_expr(ctx, await->expr);

        // P2-2: await all/any/anySuccess operates on Array<Task<T>> → Array<T> / T
        if (await->is_all || await->is_any || await->is_any_success) {
            // These forms take an array of tasks; extract element type
            if (expr_type && XR_TYPE_IS_ARRAY(expr_type)) {
                XrType *elem = expr_type->container.element_type;
                XrType *result_elem = xr_type_new_unknown(NULL);
                if (xr_type_is_named_class(elem, "Task") && elem->instance.type_arg_count > 0) {
                    result_elem = elem->instance.type_args[0];
                }
                if (await->is_all) {
                    return xr_type_new_array(ctx->analyzer->isolate, result_elem);
                }
                // await any / anySuccess returns single element
                return result_elem;
            }
            return xr_type_new_unknown(NULL);
        }

        // Single await: extract result type from Task<T>
        if (xr_type_is_named_class(expr_type, "Task")) {
            XrType *result_type =
                (expr_type->instance.type_arg_count > 0) ? expr_type->instance.type_args[0] : NULL;
            return result_type ? result_type : xr_type_new_unknown(NULL);
        }

        // await [arr] is syntactic sugar for await all — treat array as Task array
        if (XR_TYPE_IS_ARRAY(expr_type)) {
            XrType *elem = expr_type->container.element_type;
            XrType *result_elem = xr_type_new_unknown(NULL);
            if (elem && xr_type_is_named_class(elem, "Task") && elem->instance.type_arg_count > 0) {
                result_elem = elem->instance.type_args[0];
            }
            return xr_type_new_array(ctx->analyzer->isolate, result_elem);
        }

        // If not a Task, report error (skip for unknown type which means inference failed)
        if (expr_type && !XR_TYPE_IS_UNKNOWN(expr_type)) {
            XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_AWAIT_TYPE,
                                       "await expects a Task type", &loc);
        }
    }

    return xr_type_new_unknown(NULL);
}

/*
 * Visit move expression: move var
 * Compile-time checks:
 *   - must be a variable (enforced by parser)
 *   - cannot move const value
 *   - cannot move Channel (thread-safe, shared by incref)
 *   - cannot move value types (int/float/bool/string — no heap object)
 */
XrType *xa_visit_move_expr(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node)
        return xr_type_new_unknown(NULL);

    MoveExprNode *move = &node->as.move_expr;
    AstNode *inner = move->expr;
    if (!inner)
        return xr_type_new_unknown(NULL);

    // Infer type of the variable being moved
    XrType *var_type = xa_visit_infer_expr(ctx, inner);

    XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};

    // Check: variable must exist and be a let variable (not const)
    if (inner->type == AST_VARIABLE) {
        const char *name = inner->as.variable.name;
        XaSymbol *sym = xa_scope_lookup(ctx->analyzer->current_scope, name);
        if (sym && sym->is_const) {
            char msg[128];
            snprintf(msg, sizeof(msg), "cannot move const value '%s'", name);
            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_ARG_TYPE,
                                       msg, &loc);
        }
    }

    // Check: cannot move Channel types
    if (var_type && var_type->kind == XR_KIND_CHANNEL) {
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_ARG_TYPE,
                                   "cannot move Channel (thread-safe, shared by reference)", &loc);
    }

    // Check: cannot move value types (no heap object to transfer)
    if (var_type && xr_kind_is_primitive(var_type->kind)) {
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_ARG_TYPE,
                                   "move is not meaningful for value type", &loc);
    }

    // move expr has the same type as the inner expression
    return var_type ? var_type : xr_type_new_unknown(NULL);
}
