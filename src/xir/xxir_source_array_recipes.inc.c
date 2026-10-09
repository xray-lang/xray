/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_array_recipes.inc.c - Callback-taking Array members lowered to counting loops
 *
 * KEY CONCEPT:
 *   map, filter, reduce, forEach, find, findIndex, every and some evaluate the receiver once, walk its
 *   logical snapshot with a counter cell, and call the callback through an ordinary indirect call.
 */
static SourceArrayRecipe source_array_recipe(XrNativeOperation operation) {
    switch (operation) {
    case XR_NATIVE_OPERATION_ARRAY_MAP: return SOURCE_ARRAY_MAP;
    case XR_NATIVE_OPERATION_ARRAY_FILTER: return SOURCE_ARRAY_FILTER;
    case XR_NATIVE_OPERATION_ARRAY_REDUCE: return SOURCE_ARRAY_REDUCE;
    case XR_NATIVE_OPERATION_ARRAY_FOR_EACH: return SOURCE_ARRAY_FOR_EACH;
    case XR_NATIVE_OPERATION_ARRAY_FIND: return SOURCE_ARRAY_FIND;
    case XR_NATIVE_OPERATION_ARRAY_FIND_INDEX: return SOURCE_ARRAY_FIND_INDEX;
    case XR_NATIVE_OPERATION_ARRAY_EVERY: return SOURCE_ARRAY_EVERY;
    case XR_NATIVE_OPERATION_ARRAY_SOME: return SOURCE_ARRAY_SOME;
    case XR_NATIVE_OPERATION_ARRAY_CONTAINS: return SOURCE_ARRAY_CONTAINS;
    case XR_NATIVE_OPERATION_ARRAY_INDEX_OF: return SOURCE_ARRAY_INDEX_OF;
    case XR_NATIVE_OPERATION_ARRAY_JOIN: return SOURCE_ARRAY_JOIN;
    case XR_NATIVE_OPERATION_ARRAY_CLEAR: return SOURCE_ARRAY_CLEAR;
    case XR_NATIVE_OPERATION_ARRAY_REVERSE: return SOURCE_ARRAY_REVERSE;
    case XR_NATIVE_OPERATION_ARRAY_UNSHIFT: return SOURCE_ARRAY_UNSHIFT;
    case XR_NATIVE_OPERATION_ARRAY_POP: return SOURCE_ARRAY_POP;
    case XR_NATIVE_OPERATION_ARRAY_SHIFT: return SOURCE_ARRAY_SHIFT;
    case XR_NATIVE_OPERATION_ARRAY_RESIZE: return SOURCE_ARRAY_RESIZE;
    case XR_NATIVE_OPERATION_ARRAY_ENTRIES: return SOURCE_ARRAY_ENTRIES;
    case XR_NATIVE_OPERATION_ARRAY_FILL: return SOURCE_ARRAY_FILL;
    case XR_NATIVE_OPERATION_ARRAY_CONCAT: return SOURCE_ARRAY_CONCAT;
    default: return SOURCE_ARRAY_NONE;
    }
}
/* An optional guarded region: the emitter is in the then block until the guard closes. */
typedef struct SourceGuard { uint32_t branch, jump; bool jumped, when_false; } SourceGuard;
static bool source_guard_open(SourceContext *ctx, SourceValue condition, bool when_false, SourceGuard *guard) {
    SourceFunction *body = &ctx->bodies[ctx->function];
    uint32_t yes = body->block_count;
    *guard = (SourceGuard) {body->count, 0, false, when_false};
    return source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_BRANCH, XR_XIR_UNIT, {condition.id},
            {when_false ? 0 : yes, when_false ? yes : 0}, 0, {0}}, NULL) && begin_block(ctx);
}
static bool source_guard_close(SourceContext *ctx, SourceGuard *guard) {
    SourceFunction *body = &ctx->bodies[ctx->function];
    if (!ctx->returned) {
        guard->jump = body->count; guard->jumped = true;
        if (!source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_JUMP, XR_XIR_UNIT, {0}, {0}, 0, {0}}, NULL)) return false;
    }
    uint32_t join = body->block_count;
    if (!begin_block(ctx)) return false;
    body->recipes[guard->branch].instruction.targets[guard->when_false ? 0 : 1] = join;
    if (guard->jumped) body->recipes[guard->jump].instruction.targets[0] = join;
    return true;
}
/* A callback is a closure literal typed by the receiver, or any value of a matching callable type. */
typedef struct SourceArrayCallbackShape {
    XrXirType parameters[2];
    uint32_t shortest, longest;
    SourceExpectedType result;
} SourceArrayCallbackShape;
typedef struct SourceArrayCallback { SourceValue value; uint32_t arity; } SourceArrayCallback;
static bool source_array_callback(SourceContext *ctx, AstNode *node, AstNode *argument,
    const SourceArrayCallbackShape *shape, SourceArrayCallback *output) {
    SourceValue *callback = &output->value;
    SourceExpectedType result = shape->result;
    AstNode *literal = argument;
    while (literal && literal->type == AST_GROUPING) literal = literal->as.grouping;
    if (literal && literal->type == AST_FUNCTION_EXPR) {
        uint32_t count = (uint32_t) literal->as.function_expr.param_count;
        if (count < shape->shortest || count > shape->longest)
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "Array callback takes a different number of parameters");
        XrXirCallableParameter *parameters = count ? source_alloc(ctx, count, sizeof(*parameters)) : NULL;
        if (count && !parameters) return false;
        for (uint32_t i = 0; i < count; ++i) parameters[i] = (XrXirCallableParameter) {shape->parameters[i], XR_PARAM_READ};
        XrXirType expected;
        if (!source_signature(ctx, parameters, count, result.present ? result.type : XR_XIR_UNIT, &expected)) return false;
        if (!source_closure(ctx, literal, (SourceExpectedType) {true, expected, !result.present, false}, callback)) return false;
        output->arity = count;
    } else {
        if (!expression(ctx, argument, callback)) return false;
        const XrXirTypeNode *signature = xr_xir_callable_signature(&ctx->types, callback->type);
        if (!signature || signature->parameter_count < shape->shortest || signature->parameter_count > shape->longest)
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "Array callback requires a function of the declared shape");
        output->arity = signature->parameter_count;
    }
    const XrXirTypeNode *signature = xr_xir_callable_signature(&ctx->types, callback->type);
    if (!signature) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "Array callback is not callable");
    for (uint32_t i = 0; i < signature->parameter_count; ++i)
        if (signature->parameters[i].type != shape->parameters[i] || signature->parameters[i].mode != XR_PARAM_READ)
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "Array callback parameter type differs from the element");
    if (result.present && signature->result != result.type)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "Array callback result differs from its declared result");
    return true;
}
static bool source_equal_admitted(SourceContext *ctx, AstNode *node, XrXirType type) {
    XrXirDeclarations declarations;
    XrXirModule module = source_module_view(ctx, &declarations);
    XrXirProofContext proof = {&module, {XR_XIR_CONTEXT_FUNCTION, ctx->function, 0}};
    XrXirStatus status = xr_xir_compile_type_markers_prove(&ctx->compile, &proof, type, XR_XIR_CONSTRAINT_EQUAL);
    return status == XR_XIR_OK || source_fail(ctx, node, status, "equality requires its declaration predicate");
}
/* The element text of join: strings as they are, numbers and bools as print spells them. */
static bool source_array_join_piece(SourceContext *ctx, AstNode *node, SourceValue element, SourceValue *piece) {
    if (element.type == XR_XIR_STRING) { *piece = element; return true; }
    if (element.type == XR_XIR_BOOL || element.type == XR_XIR_RUNE || xr_xir_type_is_number(element.type))
        return source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_TO_STRING, XR_XIR_STRING, {element.id, 0}, {0}, 0, {0}}, piece);
    return source_fail(ctx, node, XR_XIR_BAD_TYPE, "join requires string, number, bool or rune elements");
}
/* contains, indexOf and join walk the snapshot; clear rebinds the named root to an empty Array. */
static bool source_array_query_call(SourceContext *ctx, AstNode *node, SourceArrayRecipe recipe,
    const SourceValue *evaluated, SourceValue *value) {
    CallExprNode *call = &node->as.call_expr;
    MemberAccessNode *access = &call->callee->as.member_access;
    int most = recipe == SOURCE_ARRAY_CLEAR ? 0 : 1, fewest = recipe == SOURCE_ARRAY_JOIN || recipe == SOURCE_ARRAY_CLEAR ? 0 : 1;
    if (call->type_arg_count || call->default_arg_count || call->arg_count < fewest || call->arg_count > most ||
        (call->arg_count && !call->arguments))
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "Array method requires its declared arguments");
    for (int i = 0; i < call->arg_count; ++i)
        if (call->arg_accesses && call->arg_accesses[i] != XR_CALL_ARG_PLAIN)
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "Array method arguments use ordinary READ values");
    if (recipe == SOURCE_ARRAY_CLEAR) {
        SourceValue place, empty;
        if (!source_value_place(ctx, access->object, &place)) return false;
        if (!xr_xir_type_is_array(&ctx->types, place.type))
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "clear requires an Array place");
        if (!source_recipe_group(ctx, (XrXirInstruction) {XR_XIR_ARRAY_NEW, place.type, {0}, {0}, 0, {0}}, NULL, 0, &empty) ||
            !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_PLACE_WRITE, XR_XIR_UNIT, {place.id, empty.id}, {0}, 0, {0}}, NULL))
            return false;
        *value = (SourceValue) {UINT32_MAX, XR_XIR_UNIT};
        return true;
    }
    SourceValue array;
    if (evaluated) array = *evaluated;
    else if (!expression(ctx, access->object, &array)) return false;
    if (!xr_xir_type_is_array(&ctx->types, array.type))
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "Array method receiver is not an Array");
    XrXirType element_type = xr_xir_array_element(&ctx->types, array.type);
    SourceValue argument = {0}, separator = {0};
    if (recipe == SOURCE_ARRAY_JOIN) {
        if (call->arg_count) {
            if (!source_plan_expression(ctx, call->arguments[0], (SourceExpectedType) {true, XR_XIR_STRING, false, false}, &separator)) return false;
            if (separator.type != XR_XIR_STRING) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "join separator must be a string");
        } else if (!source_string_literal(ctx, node, "", 0, &separator)) return false;
        if (element_type != XR_XIR_STRING && element_type != XR_XIR_BOOL && element_type != XR_XIR_RUNE && !xr_xir_type_is_number(element_type))
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "join requires string, number, bool or rune elements");
    } else {
        if (!source_plan_expression(ctx, call->arguments[0], (SourceExpectedType) {true, element_type, false, false}, &argument)) return false;
        if (argument.type != element_type)
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "searched value must have the element type");
        if (!source_equal_admitted(ctx, node, element_type)) return false;
    }
    XrXirType store_type = recipe == SOURCE_ARRAY_CONTAINS ? XR_XIR_BOOL : recipe == SOURCE_ARRAY_INDEX_OF ? XR_XIR_I64 : XR_XIR_STRING;
    SourceValue seed, store;
    XrXirType store_cell;
    bool seeded = recipe == SOURCE_ARRAY_CONTAINS ?
            source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_CONST_BOOL, XR_XIR_BOOL, {0}, {0}, 0, {0}}, &seed) :
        recipe == SOURCE_ARRAY_INDEX_OF ?
            source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_CONST_INT, XR_XIR_I64, {0}, {0}, -1, {0}}, &seed) :
            source_string_literal(ctx, node, "", 0, &seed);
    if (!seeded || !source_cell_type(ctx, store_type, &store_cell) ||
        !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_CELL_NEW, store_cell, {seed.id}, {0}, 0, {0}}, &store)) return false;
    SourceValue length, zero;
    if (!source_native_array_declaration(ctx) ||
        !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_ARRAY_LEN, XR_XIR_I64, {array.id}, {0}, 0, {0}}, &length) ||
        !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_CONST_INT, XR_XIR_I64, {0}, {0}, 0, {0}}, &zero)) return false;
    SourceCounter counter;
    SourceValue element;
    if (!source_counter_open(ctx, zero, length, false, &counter) ||
        !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_ARRAY_GET, element_type, {array.id, counter.index.id}, {0}, 0, {0}}, &element))
        return false;
    SourceGuard guard;
    if (recipe == SOURCE_ARRAY_JOIN) {
        SourceValue later, current, joined, piece, extended;
        if (!source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_LT_INT, XR_XIR_BOOL, {zero.id, counter.index.id}, {0}, 0, {0}}, &later) ||
            !source_guard_open(ctx, later, false, &guard) ||
            !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_CELL_READ, XR_XIR_STRING, {store.id, 0}, {0}, 0, {0}}, &current) ||
            !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_CONCAT_STRING, XR_XIR_STRING, {current.id, separator.id}, {0}, 0, {0}}, &joined) ||
            !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_CELL_WRITE, XR_XIR_UNIT, {store.id, joined.id}, {0}, 0, {0}}, NULL) ||
            !source_guard_close(ctx, &guard) ||
            !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_CELL_READ, XR_XIR_STRING, {store.id, 0}, {0}, 0, {0}}, &current) ||
            !source_array_join_piece(ctx, node, element, &piece) ||
            !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_CONCAT_STRING, XR_XIR_STRING, {current.id, piece.id}, {0}, 0, {0}}, &extended) ||
            !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_CELL_WRITE, XR_XIR_UNIT, {store.id, extended.id}, {0}, 0, {0}}, NULL)) return false;
    } else {
        SourceValue same, decided;
        if (!source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_EQUAL, XR_XIR_BOOL, {element.id, argument.id}, {0}, 0, {0}}, &same) ||
            !source_guard_open(ctx, same, false, &guard)) return false;
        if (recipe == SOURCE_ARRAY_CONTAINS) {
            if (!source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_CONST_BOOL, XR_XIR_BOOL, {0}, {0}, 1, {0}}, &decided)) return false;
        } else decided = counter.index;
        if (!source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_CELL_WRITE, XR_XIR_UNIT, {store.id, decided.id}, {0}, 0, {0}}, NULL) ||
            !source_counter_break(ctx) || !source_guard_close(ctx, &guard)) return false;
    }
    return source_counter_close(ctx, &counter) &&
        source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_CELL_READ, store_type, {store.id, 0}, {0}, 0, {0}}, value);
}
/* REF recipes prepare an owned replacement before publishing the current root once. */
static bool source_array_reorder_call(SourceContext *ctx, AstNode *node, SourceArrayRecipe recipe,
    SourceValue *value) {
    CallExprNode *call = &node->as.call_expr;
    MemberAccessNode *access = &call->callee->as.member_access;
    int arity = recipe == SOURCE_ARRAY_UNSHIFT ? 1 : 0;
    if (call->type_arg_count || call->default_arg_count || call->arg_count != arity ||
        (arity && !call->arguments))
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "Array method requires its declared arguments");
    if (arity && call->arg_accesses && call->arg_accesses[0] != XR_CALL_ARG_PLAIN)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "Array method arguments use ordinary READ values");
    SourceValue root, input = {0}, array, empty, store, candidate, length, zero;
    if (!source_value_place(ctx, access->object, &root)) return false;
    if (!xr_xir_type_is_array(&ctx->types, root.type))
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "Array method receiver is not an Array place");
    XrXirType element_type = xr_xir_array_element(&ctx->types, root.type), cell_type;
    /* The input may rebind the root or suspend; only afterwards read its current value. */
    if (arity) {
        if (!source_plan_expression(ctx, call->arguments[0], (SourceExpectedType) {true, element_type, false, false}, &input))
            return false;
        if (input.type != element_type)
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "Array unshift input must have the element type");
    }
    if (!source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_PLACE_READ, root.type, {root.id}, {0}, 0, {0}}, &array) ||
        !source_recipe_group(ctx, (XrXirInstruction) {XR_XIR_ARRAY_NEW, root.type, {0}, {0}, 0, {0}}, NULL, 0, &empty) ||
        !source_cell_type(ctx, root.type, &cell_type) ||
        !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_CELL_NEW, cell_type, {empty.id}, {0}, 0, {0}}, &store) ||
        !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_CELL_PLACE, root.type, {store.id}, {0}, 0, {0}}, &candidate))
        return false;
    if (arity && !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_ARRAY_PUSH, XR_XIR_UNIT,
        {candidate.id, input.id}, {0}, 0, {0}}, NULL)) return false;
    if (!source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_ARRAY_LEN, XR_XIR_I64, {array.id}, {0}, 0, {0}}, &length) ||
        !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_CONST_INT, XR_XIR_I64, {0}, {0}, 0, {0}}, &zero)) return false;
    SourceValue last = {0};
    if (recipe == SOURCE_ARRAY_REVERSE) {
        SourceValue one;
        if (!source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_CONST_INT, XR_XIR_I64, {0}, {0}, 1, {0}}, &one) ||
            !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_SUB_INT, XR_XIR_I64, {length.id, one.id}, {0}, 0, {0}}, &last))
            return false;
    }
    SourceCounter counter;
    if (!source_counter_open(ctx, zero, length, false, &counter)) return false;
    SourceValue index = counter.index, element;
    if (recipe == SOURCE_ARRAY_REVERSE &&
        !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_SUB_INT, XR_XIR_I64, {last.id, counter.index.id}, {0}, 0, {0}}, &index))
        return false;
    if (!source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_ARRAY_GET, element_type, {array.id, index.id}, {0}, 0, {0}}, &element) ||
        !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_ARRAY_PUSH, XR_XIR_UNIT, {candidate.id, element.id}, {0}, 0, {0}}, NULL) ||
        !source_counter_close(ctx, &counter)) return false;
    SourceValue result;
    /* The expression result is independently owned before the sole fallible root write. */
    if (!source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_CELL_READ, root.type, {store.id}, {0}, 0, {0}}, &result) ||
        !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_PLACE_WRITE, XR_XIR_UNIT, {root.id, result.id}, {0}, 0, {0}}, NULL))
        return false;
    *value = recipe == SOURCE_ARRAY_REVERSE ? result : (SourceValue) {UINT32_MAX, XR_XIR_UNIT};
    return true;
}

/* Every result and replacement read precedes the only conditional publication. */
static bool source_array_remove_call(SourceContext *ctx, AstNode *node, SourceArrayRecipe recipe,
    SourceValue *value) {
    CallExprNode *call = &node->as.call_expr;
    MemberAccessNode *access = &call->callee->as.member_access;
    if (call->type_arg_count || call->default_arg_count || call->arg_count)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "Array removal requires no explicit arguments");
    SourceValue root, array, length, zero, one, nonempty, empty, candidate_store, candidate;
    SourceValue none, result_store;
    XrXirType element_type, result_type, array_cell, result_cell;
    if (!source_value_place(ctx, access->object, &root)) return false;
    if (!xr_xir_type_is_array(&ctx->types, root.type))
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "Array removal receiver is not an Array place");
    element_type = xr_xir_array_element(&ctx->types, root.type);
    if (!source_nullable_type(ctx, element_type, &result_type) ||
        !source_cell_type(ctx, root.type, &array_cell) || !source_cell_type(ctx, result_type, &result_cell) ||
        !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_PLACE_READ, root.type, {root.id}, {0}, 0, {0}}, &array) ||
        !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_ARRAY_LEN, XR_XIR_I64, {array.id}, {0}, 0, {0}}, &length) ||
        !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_CONST_INT, XR_XIR_I64, {0}, {0}, 0, {0}}, &zero) ||
        !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_CONST_INT, XR_XIR_I64, {0}, {0}, 1, {0}}, &one) ||
        !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_GT_INT, XR_XIR_BOOL, {length.id, zero.id}, {0}, 0, {0}}, &nonempty) ||
        !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_NULLABLE_NONE, result_type, {0}, {0}, 0, {0}}, &none) ||
        !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_CELL_NEW, result_cell, {none.id}, {0}, 0, {0}}, &result_store) ||
        !source_recipe_group(ctx, (XrXirInstruction) {XR_XIR_ARRAY_NEW, root.type, {0}, {0}, 0, {0}}, NULL, 0, &empty) ||
        !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_CELL_NEW, array_cell, {empty.id}, {0}, 0, {0}}, &candidate_store) ||
        !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_CELL_PLACE, root.type, {candidate_store.id}, {0}, 0, {0}}, &candidate))
        return false;
    SourceGuard prepare;
    SourceValue last, removed, some;
    if (!source_guard_open(ctx, nonempty, false, &prepare) ||
        !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_SUB_INT, XR_XIR_I64, {length.id, one.id}, {0}, 0, {0}}, &last) ||
        !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_ARRAY_GET, element_type,
            {array.id, recipe == SOURCE_ARRAY_POP ? last.id : zero.id}, {0}, 0, {0}}, &removed) ||
        !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_NULLABLE_SOME, result_type, {removed.id}, {0}, 0, {0}}, &some) ||
        !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_CELL_WRITE, XR_XIR_UNIT, {result_store.id, some.id}, {0}, 0, {0}}, NULL))
        return false;
    SourceCounter counter;
    SourceValue first = recipe == SOURCE_ARRAY_POP ? zero : one;
    SourceValue bound = recipe == SOURCE_ARRAY_POP ? last : length;
    SourceValue element;
    if (!source_counter_open(ctx, first, bound, false, &counter) ||
        !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_ARRAY_GET, element_type, {array.id, counter.index.id}, {0}, 0, {0}}, &element) ||
        !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_ARRAY_PUSH, XR_XIR_UNIT, {candidate.id, element.id}, {0}, 0, {0}}, NULL) ||
        !source_counter_close(ctx, &counter) || !source_guard_close(ctx, &prepare)) return false;
    SourceValue result, replacement;
    SourceGuard publish;
    if (!source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_CELL_READ, result_type, {result_store.id}, {0}, 0, {0}}, &result) ||
        !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_CELL_READ, root.type, {candidate_store.id}, {0}, 0, {0}}, &replacement) ||
        !source_guard_open(ctx, nonempty, false, &publish) ||
        !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_PLACE_WRITE, XR_XIR_UNIT, {root.id, replacement.id}, {0}, 0, {0}}, NULL) ||
        !source_guard_close(ctx, &publish)) return false;
    *value = result;
    return true;
}

/* Resize prepares the exact-length replacement and owned result before one root write. */
static bool source_array_resize_call(SourceContext *ctx, AstNode *node, SourceValue *value) {
    CallExprNode *call = &node->as.call_expr;
    MemberAccessNode *access = &call->callee->as.member_access;
    if (call->type_arg_count || call->default_arg_count || call->arg_count != 2 || !call->arguments)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "Array resize requires length and fill");
    for (int a = 0; a < 2; ++a)
        if (call->arg_accesses && call->arg_accesses[a] != XR_CALL_ARG_PLAIN)
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "Array resize arguments use ordinary READ values");
    SourceValue root, length, fill, array, replacement, store, candidate, old_length, zero;
    if (!source_value_place(ctx, access->object, &root)) return false;
    if (!xr_xir_type_is_array(&ctx->types, root.type))
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "Array resize receiver is not an Array place");
    XrXirType element_type = xr_xir_array_element(&ctx->types, root.type), array_cell, count_cell;
    if (!source_plan_expression(ctx, call->arguments[0], (SourceExpectedType) {true, XR_XIR_I64, false, false}, &length) ||
        !source_plan_expression(ctx, call->arguments[1], (SourceExpectedType) {true, element_type, false, false}, &fill)) return false;
    if (length.type != XR_XIR_I64 || fill.type != element_type)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "Array resize arguments differ from length and element types");
    if (!source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_PLACE_READ, root.type, {root.id}, {0}, 0, {0}}, &array) ||
        !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_ARRAY_REPEAT, root.type, {length.id, fill.id}, {0}, 0, {0}}, &replacement) ||
        !source_cell_type(ctx, root.type, &array_cell) ||
        !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_CELL_NEW, array_cell, {replacement.id}, {0}, 0, {0}}, &store) ||
        !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_CELL_PLACE, root.type, {store.id}, {0}, 0, {0}}, &candidate) ||
        !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_ARRAY_LEN, XR_XIR_I64, {array.id}, {0}, 0, {0}}, &old_length) ||
        !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_CONST_INT, XR_XIR_I64, {0}, {0}, 0, {0}}, &zero)) return false;
    SourceValue bound_store, shorter, bound;
    SourceGuard limit;
    if (!source_cell_type(ctx, XR_XIR_I64, &count_cell) ||
        !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_CELL_NEW, count_cell, {length.id}, {0}, 0, {0}}, &bound_store) ||
        !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_LT_INT, XR_XIR_BOOL, {old_length.id, length.id}, {0}, 0, {0}}, &shorter) ||
        !source_guard_open(ctx, shorter, false, &limit) ||
        !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_CELL_WRITE, XR_XIR_UNIT, {bound_store.id, old_length.id}, {0}, 0, {0}}, NULL) ||
        !source_guard_close(ctx, &limit) ||
        !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_CELL_READ, XR_XIR_I64, {bound_store.id}, {0}, 0, {0}}, &bound)) return false;
    SourceCounter counter;
    if (!source_counter_open(ctx, zero, bound, false, &counter)) return false;
    SourceValue element, assignment[3];
    if (!source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_ARRAY_GET, element_type, {array.id, counter.index.id}, {0}, 0, {0}}, &element)) return false;
    assignment[0] = candidate; assignment[1] = counter.index; assignment[2] = element;
    if (!source_recipe_group(ctx, (XrXirInstruction) {XR_XIR_ARRAY_SET, XR_XIR_UNIT, {0}, {0}, 0, {0}}, assignment, 3, NULL) ||
        !source_counter_close(ctx, &counter)) return false;
    SourceValue result;
    if (!source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_CELL_READ, root.type, {store.id}, {0}, 0, {0}}, &result) ||
        !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_PLACE_WRITE, XR_XIR_UNIT, {root.id, result.id}, {0}, 0, {0}}, NULL)) return false;
    *value = result;
    return true;
}

/* Bind once, evaluate inputs, check the current range, then publish the owned replacement. */
static bool source_array_fill_call(SourceContext *ctx, AstNode *node, SourceValue *value) {
    CallExprNode *call = &node->as.call_expr;
    MemberAccessNode *access = &call->callee->as.member_access;
    if (call->type_arg_count || call->default_arg_count || call->arg_count < 1 || call->arg_count > 3 || !call->arguments)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "Array fill requires value and optional start and end");
    for (int a = 0; a < call->arg_count; ++a)
        if (call->arg_accesses && call->arg_accesses[a] != XR_CALL_ARG_PLAIN)
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "Array fill arguments use ordinary READ values");
    SourceValue root, fill, start = {0}, end = {0}, array, length;
    if (!source_value_place(ctx, access->object, &root)) return false;
    if (!xr_xir_type_is_array(&ctx->types, root.type))
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "Array fill receiver is not an Array place");
    XrXirType element = xr_xir_array_element(&ctx->types, root.type), cell;
    if (!source_plan_expression(ctx, call->arguments[0], (SourceExpectedType) {true, element, false, false}, &fill)) return false;
    if (fill.type != element) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "Array fill value differs from its element type");
    if (call->arg_count > 1) {
        if (!source_plan_expression(ctx, call->arguments[1], (SourceExpectedType) {true, XR_XIR_I64, false, false}, &start)) return false;
        if (start.type != XR_XIR_I64) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "Array fill start must be i64");
    }
    if (call->arg_count > 2) {
        if (!source_plan_expression(ctx, call->arguments[2], (SourceExpectedType) {true, XR_XIR_I64, false, false}, &end)) return false;
        if (end.type != XR_XIR_I64) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "Array fill end must be i64");
    }
    if (!source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_PLACE_READ, root.type, {root.id}, {0}, 0, {0}}, &array) ||
        !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_ARRAY_LEN, XR_XIR_I64, {array.id}, {0}, 0, {0}}, &length)) return false;
    if (call->arg_count < 2 &&
        !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_CONST_INT, XR_XIR_I64, {0}, {0}, 0, {0}}, &start)) return false;
    if (call->arg_count < 3) end = length;
    SourceValue bounds[3] = {start, end, length};
    if (!source_recipe_group(ctx, (XrXirInstruction) {XR_XIR_RANGE_CHECK, XR_XIR_UNIT, {0}, {0}, 0, {0}}, bounds, 3, NULL)) return false;
    SourceValue store, candidate;
    if (!source_cell_type(ctx, root.type, &cell) ||
        !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_CELL_NEW, cell, {array.id}, {0}, 0, {0}}, &store) ||
        !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_CELL_PLACE, root.type, {store.id}, {0}, 0, {0}}, &candidate)) return false;
    SourceCounter counter;
    if (!source_counter_open(ctx, start, end, false, &counter)) return false;
    SourceValue assignment[3] = {candidate, counter.index, fill};
    if (!source_recipe_group(ctx, (XrXirInstruction) {XR_XIR_ARRAY_SET, XR_XIR_UNIT, {0}, {0}, 0, {0}}, assignment, 3, NULL) ||
        !source_counter_close(ctx, &counter)) return false;
    SourceValue result;
    if (!source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_CELL_READ, root.type, {store.id}, {0}, 0, {0}}, &result) ||
        !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_PLACE_WRITE, XR_XIR_UNIT, {root.id, result.id}, {0}, 0, {0}}, NULL)) return false;
    *value = result;
    return true;
}

/* The immutable receiver snapshot owns every element until the completed result is retained. */
static bool source_array_entries_call(SourceContext *ctx, AstNode *node,
    const SourceValue *evaluated, SourceValue *value) {
    CallExprNode *call = &node->as.call_expr;
    if (call->type_arg_count || call->default_arg_count || call->arg_count)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "Array entries takes no arguments");
    SourceValue array;
    if (evaluated) array = *evaluated;
    else if (!expression(ctx, call->callee->as.member_access.object, &array)) return false;
    if (!xr_xir_type_is_array(&ctx->types, array.type))
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "Array entries receiver is not an Array");
    XrXirType element = xr_xir_array_element(&ctx->types, array.type), tuple, result_type, cell_type;
    XrXirCallableParameter *fields = source_alloc(ctx, 2, sizeof(*fields));
    if (!fields) return false;
    fields[0] = (XrXirCallableParameter) {XR_XIR_I64, 0};
    fields[1] = (XrXirCallableParameter) {element, 0};
    if (!source_intern_type(ctx, (XrXirTypeNode) {.kind = XR_XIR_TYPE_TUPLE,
            .parameters = fields, .parameter_count = 2}, &tuple) ||
        !source_array_element_type(ctx, tuple, &result_type) ||
        !source_cell_type(ctx, result_type, &cell_type)) return false;
    SourceValue seed, store, candidate, length, zero;
    if (!source_recipe_group(ctx, (XrXirInstruction) {XR_XIR_ARRAY_NEW, result_type, {0}, {0}, 0, {0}}, NULL, 0, &seed) ||
        !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_CELL_NEW, cell_type, {seed.id}, {0}, 0, {0}}, &store) ||
        !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_CELL_PLACE, result_type, {store.id}, {0}, 0, {0}}, &candidate) ||
        !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_ARRAY_LEN, XR_XIR_I64, {array.id}, {0}, 0, {0}}, &length) ||
        !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_CONST_INT, XR_XIR_I64, {0}, {0}, 0, {0}}, &zero)) return false;
    SourceCounter counter;
    if (!source_counter_open(ctx, zero, length, false, &counter)) return false;
    SourceValue payload, entry, pair[2];
    if (!source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_ARRAY_GET, element,
            {array.id, counter.index.id}, {0}, 0, {0}}, &payload)) return false;
    pair[0] = counter.index; pair[1] = payload;
    if (!source_recipe_group(ctx, (XrXirInstruction) {XR_XIR_TUPLE_NEW, tuple, {0}, {0}, 0, {0}},
            pair, element == XR_XIR_UNIT ? 1 : 2, &entry) ||
        !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_ARRAY_PUSH, XR_XIR_UNIT,
            {candidate.id, entry.id}, {0}, 0, {0}}, NULL) ||
        !source_counter_close(ctx, &counter)) return false;
    return source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_CELL_READ, result_type,
        {store.id}, {0}, 0, {0}}, value);
}

/* Capture every input before constructing output so later argument effects cannot replace a snapshot. */
static bool source_array_concat_call(SourceContext *ctx, AstNode *node,
    const SourceValue *evaluated, SourceValue *value) {
    CallExprNode *call = &node->as.call_expr;
    if (call->type_arg_count || call->default_arg_count || call->arg_count < 0 ||
        (call->arg_count && !call->arguments))
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "Array concat takes ordinary trailing Array arguments");
    for (int i = 0; i < call->arg_count; ++i)
        if (call->arg_accesses && call->arg_accesses[i] != XR_CALL_ARG_PLAIN)
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "Array concat arguments use ordinary READ values");
    SourceValue receiver;
    if (evaluated) receiver = *evaluated;
    else if (!expression(ctx, call->callee->as.member_access.object, &receiver)) return false;
    if (!xr_xir_type_is_array(&ctx->types, receiver.type))
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "Array concat receiver is not an Array");
    const uint32_t count = (uint32_t)call->arg_count + 1;
    SourceValue *arrays = source_alloc(ctx, count, sizeof(*arrays));
    if (!arrays) return false;
    arrays[0] = receiver;
    for (uint32_t i = 1; i < count; ++i) {
        if (!source_plan_expression(ctx, call->arguments[i - 1],
                (SourceExpectedType){true, receiver.type, false, false}, &arrays[i])) return false;
        if (arrays[i].type != receiver.type)
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "Array concat argument type differs from its receiver");
    }
    XrXirType element = xr_xir_array_element(&ctx->types, receiver.type), cell_type;
    SourceValue seed, store, candidate, zero;
    if (!source_cell_type(ctx, receiver.type, &cell_type) ||
        !source_recipe_group(ctx, (XrXirInstruction){XR_XIR_ARRAY_NEW, receiver.type, {0}, {0}, 0, {0}}, NULL, 0, &seed) ||
        !source_recipe_record(ctx, (XrXirInstruction){XR_XIR_CELL_NEW, cell_type, {seed.id}, {0}, 0, {0}}, &store) ||
        !source_recipe_record(ctx, (XrXirInstruction){XR_XIR_CELL_PLACE, receiver.type, {store.id}, {0}, 0, {0}}, &candidate) ||
        !source_recipe_record(ctx, (XrXirInstruction){XR_XIR_CONST_INT, XR_XIR_I64, {0}, {0}, 0, {0}}, &zero)) return false;
    for (uint32_t i = 0; i < count; ++i) {
        SourceValue length, payload;
        if (!source_recipe_record(ctx, (XrXirInstruction){XR_XIR_ARRAY_LEN, XR_XIR_I64,
                {arrays[i].id}, {0}, 0, {0}}, &length)) return false;
        SourceCounter counter;
        if (!source_counter_open(ctx, zero, length, false, &counter) ||
            !source_recipe_record(ctx, (XrXirInstruction){XR_XIR_ARRAY_GET, element,
                {arrays[i].id, counter.index.id}, {0}, 0, {0}}, &payload) ||
            !source_recipe_record(ctx, (XrXirInstruction){XR_XIR_ARRAY_PUSH, XR_XIR_UNIT,
                {candidate.id, payload.id}, {0}, 0, {0}}, NULL) ||
            !source_counter_close(ctx, &counter)) return false;
    }
    return source_recipe_record(ctx, (XrXirInstruction){XR_XIR_CELL_READ, receiver.type,
        {store.id}, {0}, 0, {0}}, value);
}

static bool source_array_recipe_call(SourceContext *ctx, AstNode *node, SourceArrayRecipe recipe,
    const SourceValue *evaluated, SourceValue *value) {
    if (recipe == SOURCE_ARRAY_CONCAT) return source_array_concat_call(ctx, node, evaluated, value);
    if (recipe == SOURCE_ARRAY_ENTRIES) return source_array_entries_call(ctx, node, evaluated, value);
    if (recipe == SOURCE_ARRAY_RESIZE) return source_array_resize_call(ctx, node, value);
    if (recipe == SOURCE_ARRAY_FILL) return source_array_fill_call(ctx, node, value);
    if (recipe == SOURCE_ARRAY_POP || recipe == SOURCE_ARRAY_SHIFT)
        return source_array_remove_call(ctx, node, recipe, value);
    if (recipe == SOURCE_ARRAY_REVERSE || recipe == SOURCE_ARRAY_UNSHIFT)
        return source_array_reorder_call(ctx, node, recipe, value);
    if (recipe >= SOURCE_ARRAY_CONTAINS) return source_array_query_call(ctx, node, recipe, evaluated, value);
    CallExprNode *call = &node->as.call_expr;
    MemberAccessNode *access = &call->callee->as.member_access;
    int expected_arguments = recipe == SOURCE_ARRAY_REDUCE ? 2 : 1;
    if (call->type_arg_count || call->default_arg_count || call->arg_count != expected_arguments || !call->arguments)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "Array method requires its declared arguments");
    for (int i = 0; i < call->arg_count; ++i)
        if (call->arg_accesses && call->arg_accesses[i] != XR_CALL_ARG_PLAIN)
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "Array method arguments use ordinary READ values");
    SourceValue array;
    if (evaluated) array = *evaluated;
    else if (!expression(ctx, access->object, &array)) return false;
    if (!xr_xir_type_is_array(&ctx->types, array.type))
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "Array method receiver is not an Array");
    XrXirType element_type = xr_xir_array_element(&ctx->types, array.type);
    SourceArrayCallback callback = {0};
    SourceValue initial = {0};
    XrXirType accumulator = XR_XIR_UNIT;
    if (recipe == SOURCE_ARRAY_REDUCE) {
        /* Collecting the initial value's type does not evaluate it. The callback
         * must be completed first so effects retain ordinary argument order. */
        SourceExpressionPlan *initial_plan = source_plan_collect(ctx, call->arguments[1], (SourceExpectedType) {false, XR_XIR_UNIT, false, false});
        if (!initial_plan || !source_plan_binary_prepare(ctx, initial_plan, initial_plan->expected, true) ||
            !source_plan_numeric_prepare(ctx, initial_plan, initial_plan->expected, true)) return false;
        if (!initial_plan->type_ready)
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "reduce initial type is not admitted without a known context");
        accumulator = initial_plan->ground_type;
        SourceArrayCallbackShape shape = {{accumulator, element_type}, 2, 2, {true, accumulator, false, false}};
        if (accumulator == XR_XIR_UNIT) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "reduce initial value cannot be unit");
        if (!source_array_callback(ctx, node, call->arguments[0], &shape, &callback) ||
            !source_plan_complete(ctx, initial_plan, &initial))
            return false;
        if (initial.type != accumulator)
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "reduce initial type differs from its callback accumulator");
    } else {
        bool indexed = recipe == SOURCE_ARRAY_MAP || recipe == SOURCE_ARRAY_FILTER || recipe == SOURCE_ARRAY_FOR_EACH;
        SourceExpectedType result = recipe == SOURCE_ARRAY_MAP ? (SourceExpectedType) {false, XR_XIR_UNIT, false, false} :
            recipe == SOURCE_ARRAY_FOR_EACH ? (SourceExpectedType) {true, XR_XIR_UNIT, false, false} : (SourceExpectedType) {true, XR_XIR_BOOL, false, false};
        SourceArrayCallbackShape shape = {{element_type, XR_XIR_I64}, 1, indexed ? 2 : 1, result};
        if (!source_array_callback(ctx, node, call->arguments[0], &shape, &callback)) return false;
    }
    const XrXirTypeNode *signature = xr_xir_callable_signature(&ctx->types, callback.value.type);
    XrXirType result_type = signature->result, store_type = XR_XIR_UNIT, store_cell = XR_XIR_UNIT;
    /* The accumulating cell holds the output array, the running value, or the lookup result. */
    SourceValue store = {0};
    switch (recipe) {
    case SOURCE_ARRAY_MAP:
        if (result_type == XR_XIR_UNIT) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "map callback must produce a value");
        if (!source_array_element_type(ctx, result_type, &store_type)) return false;
        break;
    case SOURCE_ARRAY_FILTER: store_type = array.type; break;
    case SOURCE_ARRAY_REDUCE: store_type = accumulator; break;
    case SOURCE_ARRAY_FIND:
        if (!source_nullable_type(ctx, element_type, &store_type)) return false;
        break;
    case SOURCE_ARRAY_FIND_INDEX: store_type = XR_XIR_I64; break;
    case SOURCE_ARRAY_EVERY: case SOURCE_ARRAY_SOME: store_type = XR_XIR_BOOL; break;
    default: break;
    }
    if (store_type != XR_XIR_UNIT) {
        SourceValue seed;
        bool ok;
        switch (recipe) {
        case SOURCE_ARRAY_MAP: case SOURCE_ARRAY_FILTER:
            ok = source_recipe_group(ctx, (XrXirInstruction) {XR_XIR_ARRAY_NEW, store_type, {0}, {0}, 0, {0}}, NULL, 0, &seed); break;
        case SOURCE_ARRAY_REDUCE: seed = initial; ok = true; break;
        case SOURCE_ARRAY_FIND:
            ok = source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_NULLABLE_NONE, store_type, {0}, {0}, 0, {0}}, &seed); break;
        case SOURCE_ARRAY_FIND_INDEX:
            ok = source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_CONST_INT, XR_XIR_I64, {0}, {0}, -1, {0}}, &seed); break;
        default:
            ok = source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_CONST_BOOL, XR_XIR_BOOL, {0}, {0}, recipe == SOURCE_ARRAY_EVERY, {0}}, &seed); break;
        }
        if (!ok || !source_cell_type(ctx, store_type, &store_cell) ||
            !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_CELL_NEW, store_cell, {seed.id}, {0}, 0, {0}}, &store)) return false;
    }
    SourceValue length, zero;
    if (!source_native_array_declaration(ctx) ||
        !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_ARRAY_LEN, XR_XIR_I64, {array.id}, {0}, 0, {0}}, &length) ||
        !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_CONST_INT, XR_XIR_I64, {0}, {0}, 0, {0}}, &zero)) return false;
    SourceCounter counter;
    if (!source_counter_open(ctx, zero, length, false, &counter)) return false;
    SourceValue element, called, arguments[2];
    if (!source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_ARRAY_GET, element_type, {array.id, counter.index.id}, {0}, 0, {0}}, &element))
        return false;
    uint32_t used = 0;
    if (recipe == SOURCE_ARRAY_REDUCE) {
        if (!source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_CELL_READ, accumulator, {store.id}, {0}, 0, {0}}, &arguments[used++])) return false;
        arguments[used++] = element;
    } else {
        arguments[used++] = element;
        if (callback.arity == 2) arguments[used++] = counter.index;
    }
    if (!source_recipe_group(ctx, (XrXirInstruction) {XR_XIR_CALL_INDIRECT, result_type, {0}, {0}, callback.value.id, {0}}, arguments, used, &called))
        return false;
    SourceGuard guard;
    SourceValue place;
    switch (recipe) {
    case SOURCE_ARRAY_MAP:
        if (!source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_CELL_PLACE, store_type, {store.id, 0}, {0}, 0, {0}}, &place) ||
            !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_ARRAY_PUSH, XR_XIR_UNIT, {place.id, called.id}, {0}, 0, {0}}, NULL)) return false;
        break;
    case SOURCE_ARRAY_FILTER:
        if (!source_guard_open(ctx, called, false, &guard) ||
            !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_CELL_PLACE, store_type, {store.id, 0}, {0}, 0, {0}}, &place) ||
            !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_ARRAY_PUSH, XR_XIR_UNIT, {place.id, element.id}, {0}, 0, {0}}, NULL) ||
            !source_guard_close(ctx, &guard)) return false;
        break;
    case SOURCE_ARRAY_REDUCE:
        if (!source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_CELL_WRITE, XR_XIR_UNIT, {store.id, called.id}, {0}, 0, {0}}, NULL)) return false;
        break;
    case SOURCE_ARRAY_FIND: case SOURCE_ARRAY_FIND_INDEX: case SOURCE_ARRAY_SOME: case SOURCE_ARRAY_EVERY: {
        /* The first element that decides the answer records it and leaves the loop. */
        SourceValue decided;
        if (!source_guard_open(ctx, called, recipe == SOURCE_ARRAY_EVERY, &guard)) return false;
        if (recipe == SOURCE_ARRAY_FIND) {
            if (!source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_NULLABLE_SOME, store_type, {element.id, 0}, {0}, 0, {0}}, &decided)) return false;
        } else if (recipe == SOURCE_ARRAY_FIND_INDEX) decided = counter.index;
        else if (!source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_CONST_BOOL, XR_XIR_BOOL, {0}, {0}, recipe == SOURCE_ARRAY_SOME, {0}}, &decided))
            return false;
        if (!source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_CELL_WRITE, XR_XIR_UNIT, {store.id, decided.id}, {0}, 0, {0}}, NULL) ||
            !source_counter_break(ctx) || !source_guard_close(ctx, &guard)) return false;
        break;
    }
    default: break;
    }
    if (!source_counter_close(ctx, &counter)) return false;
    if (store_type == XR_XIR_UNIT) { *value = called; return true; }
    return source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_CELL_READ, store_type, {store.id, 0}, {0}, 0, {0}}, value);
}
