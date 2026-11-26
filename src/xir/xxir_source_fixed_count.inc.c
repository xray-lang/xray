/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * Fixed-size annotations and repeat counts share the typed integer evaluator.
 * A literal-only operand takes its arithmetic context from its typed peer. */
#include "xxir_scalar.h"
#include "shared/xr_integer_conversion_core.h"

typedef struct SourceLengthInteger {
    int64_t value;
    XrXirType type;
    bool literal;
} SourceLengthInteger;

static bool source_direct_integer(SourceContext *, AstNode *, SourceInteger *);
static SourceName *visible_name(SourceContext *, const char *);
static bool source_exported_const(SourceContext *, const SourceName *);
static bool source_length_integer(SourceContext *, AstNode *, XrXirType,
    unsigned, uint32_t *, SourceLengthInteger *);

static bool source_length_name(SourceContext *ctx, AstNode *node, unsigned depth,
    uint32_t *steps, SourceLengthInteger *out) {
    const char *name = NULL;
    uint32_t owner_module = ctx->module;
    SourceName *binding = NULL;
    bool imported = false;
    if (node->type == AST_VARIABLE) {
        name = node->as.variable.name;
        binding = visible_name(ctx, name);
        if (binding && binding->kind == SOURCE_IMPORT) {
            owner_module = binding->module;
            name = binding->imported;
            imported = true;
        }
    } else if (node->as.member_access.object->type == AST_VARIABLE) {
        SourceName *owner = visible_name(ctx, node->as.member_access.object->as.variable.name);
        if (!owner || owner->kind != SOURCE_MODULE) return false;
        owner_module = owner->module;
        name = node->as.member_access.name;
        imported = true;
    }
    if (!name) return false;
    if (imported) binding = find_name(ctx, ctx->names[owner_module], name);
    if (binding && (binding->mutable || (imported && !source_exported_const(ctx, binding)))) return false;
    if (binding && xr_xir_type_is_integer(binding->type)) {
        if (binding->constant_key.kind == XR_XIR_KEY_SCALAR) {
            *out = (SourceLengthInteger){xr_integer_signed_from_bits(binding->constant_key.bits), binding->type, false};
            return true;
        }
        if (binding->has_length_integer) {
            *out = (SourceLengthInteger){binding->length_integer, binding->type, false};
            return true;
        }
    }
    if (!ctx->declarations_building) return false;
    /* Record fields and function signatures can precede slot installation.
     * Only a real const declaration can provide an early layout fact. */
    AstNode *declaration = binding ? binding->node : NULL;
    if (!binding) {
        AstNode *ast = ctx->graph->specs[owner_module].ast;
        if (!ast || ast->type != AST_PROGRAM) return false;
        for (int i = 0; i < ast->as.program.count; ++i) {
            if (!*steps || !source_request_poll(ctx, node)) return false;
            --*steps;
            AstNode *candidate = ast->as.program.statements[i];
            if (candidate->type == AST_CONST_DECL && source_text_same(ctx, node, candidate->as.var_decl.name, name)) {
                declaration = candidate;
                break;
            }
        }
    }
    if (!declaration || declaration->type != AST_CONST_DECL || (imported && !declaration->is_exported)) return false;
    VarDeclNode *decl = &declaration->as.var_decl;
    XrXirType hint = XR_XIR_UNIT;
    if (decl->type_annotation && (decl->type_annotation->kind != XR_TREF_SCALAR ||
        !source_type(ctx, decl->type_annotation, &hint) || !xr_xir_type_is_integer(hint))) return false;
    SourceName *locals = ctx->locals;
    uint32_t module = ctx->module;
    ctx->locals = NULL;
    ctx->module = owner_module;
    bool ok = source_length_integer(ctx, decl->initializer, hint, depth + 1, steps, out);
    ctx->module = module;
    ctx->locals = locals;
    if (ok) out->literal = false;
    return ok;
}

static bool source_length_integer(SourceContext *ctx, AstNode *node, XrXirType hint,
    unsigned depth, uint32_t *steps, SourceLengthInteger *out) {
    if (!node || depth >= 128 || !*steps || !source_request_poll(ctx, node)) return false;
    --*steps;
    if (node->type == AST_GROUPING)
        return source_length_integer(ctx, node->as.grouping, hint, depth + 1, steps, out);
    if (node->type == AST_VARIABLE || node->type == AST_MEMBER_ACCESS)
        return source_length_name(ctx, node, depth, steps, out);
    if (node->type == AST_LITERAL_INT || node->type == AST_UNARY_NEG) {
        SourceInteger literal;
        if (!source_direct_integer(ctx, node, &literal)) return false;
        if (literal.present) {
            XrHirLiteralInput input = {.expected = hint != XR_XIR_UNIT, .expected_type = hint,
                .negative = literal.negative, .magnitude = literal.magnitude};
            XrHirLiteral checked;
            const char *reason;
            XrXirStatus status = xr_hir_literal_check(&ctx->types, &input, ctx->request, &checked, &reason);
            if (status != XR_XIR_OK)
                return status == XR_XIR_BAD_TYPE ? false : source_fail(ctx, node, status, reason);
            *out = (SourceLengthInteger){xr_integer_signed_from_bits(checked.bits), checked.type, true};
            return true;
        }
    }
    SourceLengthInteger left, right;
    if (node->type == AST_UNARY_NEG || node->type == AST_UNARY_BNOT) {
        if (!source_length_integer(ctx, node->as.unary.operand, hint, depth + 1, steps, &left)) return false;
        uint64_t bits = (uint64_t)left.value;
        bits = node->type == AST_UNARY_NEG ? UINT64_C(0) - bits : ~bits;
        *out = left;
        out->value = xr_integer_signed_from_bits(xr_integer_convert_bits(bits, 64, false,
            (uint8_t)xr_xir_integer_bits(left.type), xr_xir_integer_signed(left.type)));
        return true;
    }
    if (node->type < AST_BINARY_ADD || node->type > AST_BINARY_RSHIFT) return false;
    bool shift = node->type == AST_BINARY_LSHIFT || node->type == AST_BINARY_RSHIFT;
    bool have_left = source_length_integer(ctx, node->as.binary.left, hint, depth + 1, steps, &left);
    if (!have_left && shift) return false;
    XrXirType right_hint = shift ? XR_XIR_UNIT : (have_left && !left.literal ? left.type : hint);
    if (!source_length_integer(ctx, node->as.binary.right, right_hint, depth + 1, steps, &right)) return false;
    if (!have_left || (!shift && left.literal && !right.literal && left.type != right.type)) {
        if (right.literal || (!have_left && hint == right.type)) return false;
        if (!source_length_integer(ctx, node->as.binary.left, right.type, depth + 1, steps, &left)) return false;
    }
    if (!shift && xr_xir_integer_signed(left.type) != xr_xir_integer_signed(right.type)) return false;
    XrXirType type = !shift && xr_xir_integer_bits(right.type) > xr_xir_integer_bits(left.type) ? right.type : left.type;
    static const XrXirArithmetic operations[] = {
        [AST_BINARY_ADD - AST_BINARY_ADD] = XR_XIR_ARITH_ADD,
        [AST_BINARY_SUB - AST_BINARY_ADD] = XR_XIR_ARITH_SUB,
        [AST_BINARY_MUL - AST_BINARY_ADD] = XR_XIR_ARITH_MUL,
        [AST_BINARY_DIV - AST_BINARY_ADD] = XR_XIR_ARITH_DIV,
        [AST_BINARY_MOD - AST_BINARY_ADD] = XR_XIR_ARITH_REM,
        [AST_BINARY_BAND - AST_BINARY_ADD] = XR_XIR_ARITH_AND,
        [AST_BINARY_BOR - AST_BINARY_ADD] = XR_XIR_ARITH_OR,
        [AST_BINARY_BXOR - AST_BINARY_ADD] = XR_XIR_ARITH_XOR,
        [AST_BINARY_LSHIFT - AST_BINARY_ADD] = XR_XIR_ARITH_SHL,
        [AST_BINARY_RSHIFT - AST_BINARY_ADD] = XR_XIR_ARITH_SHR
    };
    out->type = type;
    out->literal = left.literal && (shift || right.literal);
    return xr_xir_integer_arithmetic(xr_xir_integer_format(type), operations[node->type - AST_BINARY_ADD],
        left.value, right.value, &out->value) == XR_XIR_RUN_OK;
}
static bool source_fold_length(SourceContext *ctx, AstNode *node, XrXirType hint, SourceLengthInteger *out) {
    uint32_t steps = ctx->compile.limits.instructions;
    return source_length_integer(ctx, node, hint, 0, &steps, out);
}
static bool source_fixed_count(SourceContext *ctx, AstNode *node, uint32_t *out) {
    SourceLengthInteger integer;
    if (!source_fold_length(ctx, node, XR_XIR_UNIT, &integer) || integer.value <= 0 || integer.value > 65535)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "fixed array length must be a constant integer from 1 to 65535");
    *out = (uint32_t)integer.value;
    return true;
}
