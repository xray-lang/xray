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
        if (!source_closure(ctx, literal, (SourceExpectedType) {true, expected, !result.present}, callback)) return false;
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
    if (element.type == XR_XIR_BOOL || xr_xir_type_is_number(element.type))
        return source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_TO_STRING, XR_XIR_STRING, {element.id, 0}, {0}, 0, {0}}, piece);
    return source_fail(ctx, node, XR_XIR_BAD_TYPE, "join requires string, number or bool elements");
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
            if (!source_plan_expression(ctx, call->arguments[0], (SourceExpectedType) {true, XR_XIR_STRING, false}, &separator)) return false;
            if (separator.type != XR_XIR_STRING) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "join separator must be a string");
        } else if (!source_string_literal(ctx, node, "", 0, &separator)) return false;
        if (element_type != XR_XIR_STRING && element_type != XR_XIR_BOOL && !xr_xir_type_is_number(element_type))
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "join requires string, number or bool elements");
    } else {
        if (!source_plan_expression(ctx, call->arguments[0], (SourceExpectedType) {true, element_type, false}, &argument)) return false;
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
static bool source_array_recipe_call(SourceContext *ctx, AstNode *node, SourceArrayRecipe recipe,
    const SourceValue *evaluated, SourceValue *value) {
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
        SourceExpressionPlan *initial_plan = source_plan_collect(ctx, call->arguments[1], (SourceExpectedType) {false, XR_XIR_UNIT, false});
        if (!initial_plan || !source_plan_binary_prepare(ctx, initial_plan, initial_plan->expected, true) ||
            !source_plan_numeric_prepare(ctx, initial_plan, initial_plan->expected, true)) return false;
        if (!initial_plan->type_ready)
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "reduce initial type is not admitted without a known context");
        accumulator = initial_plan->ground_type;
        SourceArrayCallbackShape shape = {{accumulator, element_type}, 2, 2, {true, accumulator, false}};
        if (accumulator == XR_XIR_UNIT) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "reduce initial value cannot be unit");
        if (!source_array_callback(ctx, node, call->arguments[0], &shape, &callback) ||
            !source_plan_complete(ctx, initial_plan, &initial))
            return false;
        if (initial.type != accumulator)
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "reduce initial type differs from its callback accumulator");
    } else {
        bool indexed = recipe == SOURCE_ARRAY_MAP || recipe == SOURCE_ARRAY_FILTER || recipe == SOURCE_ARRAY_FOR_EACH;
        SourceExpectedType result = recipe == SOURCE_ARRAY_MAP ? (SourceExpectedType) {false, XR_XIR_UNIT, false} :
            recipe == SOURCE_ARRAY_FOR_EACH ? (SourceExpectedType) {true, XR_XIR_UNIT, false} : (SourceExpectedType) {true, XR_XIR_BOOL, false};
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
