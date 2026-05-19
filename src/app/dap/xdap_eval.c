/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdap_eval.c - Debug expression evaluation (AST interpreter)
 *
 * KEY CONCEPT:
 *   Evaluates expressions in the context of a paused debug session.
 *   Parses expression text into AST, then interprets it against
 *   the current stack frame's local variables.
 */

#include "xdap_debug.h"
#include "../../runtime/xisolate_api.h"
#include "../../runtime/xexec_state.h"
#include "../../runtime/xexec_frame.h"
#include "../../runtime/value/xvalue.h"
#include "../../runtime/value/xchunk.h"
#include "../../runtime/object/xstring.h"
#include "../../runtime/object/xarray.h"
#include "../../runtime/object/xmap.h"
#include "../../runtime/object/xjson.h"
#include "../../runtime/class/xinstance.h"
#include "../../coro/xcoroutine.h"
#include "../../runtime/closure/xcell.h"
#include "../../frontend/parser/xparse.h"
#include "../../frontend/parser/xast.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "../../base/xmalloc.h"

// ============================================================================
// Value Helpers
// ============================================================================

// Truthiness check: null, false, 0, 0.0 are falsy; everything else is truthy
static inline bool eval_is_truthy(XrValue v) {
    if (XR_IS_NULL(v))
        return false;
    if (XR_IS_BOOL(v))
        return XR_TO_BOOL(v);
    if (XR_IS_INT(v))
        return XR_TO_INT(v) != 0;
    if (XR_IS_FLOAT(v))
        return XR_TO_FLOAT(v) != 0.0;
    return true;
}

// ============================================================================
// Evaluation Context
// ============================================================================

typedef struct {
    XrayIsolate *isolate;
    int frame_idx;
    XrBcCallFrame *frame;
    XrProto *proto;
    XrValue *stack_base;
    char *error;
} XrEvalContext;

// Forward declaration
static XrValue eval_ast(XrEvalContext *ctx, AstNode *node);

// Lookup local variable by name in a specific frame
static bool lookup_in_frame(XrBcCallFrame *frame, XrValue *stack, const char *name, XrValue *out) {
    if (!frame || !frame->closure || !frame->closure->proto)
        return false;
    XrProto *proto = frame->closure->proto;

    int locvar_count = PROTO_LOCVAR_COUNT(proto);
    XrValue *base = stack + frame->base_offset;
    for (int i = 0; i < locvar_count; i++) {
        XrLocVar locvar = PROTO_LOCVAR(proto, i);
        if (locvar.name && strcmp(locvar.name, name) == 0) {
            *out = base[i];
            return true;
        }
    }
    return false;
}

// Try to find a variable in closure upvalues by matching enclosing proto's locvar names
static bool lookup_in_upvalues(XrClosure *closure, const char *name, XrValue *out) {
    if (!closure || closure->upval_count == 0)
        return false;
    XrProto *proto = closure->proto;
    if (!proto)
        return false;

    // Walk the proto's upvalue descriptors and match against enclosing locvar names
    int upval_count = PROTO_UPVAL_COUNT(proto);
    XrProto *enclosing = proto->enclosing;

    for (int i = 0; i < upval_count && i < closure->upval_count; i++) {
        UpvalInfo uinfo = PROTO_UPVALUE(proto, i);

        // Direct capture from enclosing frame's register — match register to locvar name
        if (uinfo.source == UPVAL_SRC_REG && enclosing) {
            int locvar_count = PROTO_LOCVAR_COUNT(enclosing);
            for (int j = 0; j < locvar_count; j++) {
                XrLocVar lv = PROTO_LOCVAR(enclosing, j);
                if (lv.reg == uinfo.index && lv.name && strcmp(lv.name, name) == 0) {
                    XrValue val = closure->upvals[i];
                    // If value is a cell, dereference it
                    if (XR_IS_PTR(val)) {
                        XrGCHeader *hdr = XR_TO_PTR(val);
                        if (hdr->type == XR_TCELL) {
                            val = ((XrCell *) hdr)->value;
                        }
                    }
                    *out = val;
                    return true;
                }
            }
        }
    }
    return false;
}

// Lookup variable by name: locals → upvalues → enclosing frames
static XrValue eval_lookup_local(XrEvalContext *ctx, const char *name) {
    // 1. Current frame locals
    if (ctx->frame) {
        XrValue result;
        XrDebugFrameCtx fctx;
        xr_debug_get_frame_ctx_ex(ctx->isolate, &fctx);

        // Search current frame
        if (lookup_in_frame(ctx->frame, fctx.stack, name, &result)) {
            return result;
        }

        // 2. Upvalue lookup (closure captures)
        if (ctx->frame->closure) {
            if (lookup_in_upvalues(ctx->frame->closure, name, &result)) {
                return result;
            }
        }

        // 3. Walk enclosing frames (outer scopes)
        int actual_idx = fctx.frame_count - 1 - ctx->frame_idx;
        for (int i = actual_idx - 1; i >= 0; i--) {
            if (lookup_in_frame(&fctx.frames[i], fctx.stack, name, &result)) {
                return result;
            }
        }
    }

    return xr_null();
}

// Evaluate member access: obj.prop
static XrValue eval_member_access(XrEvalContext *ctx, XrValue obj, const char *name) {
    if (XR_IS_NULL(obj)) {
        ctx->error = xr_strdup("Cannot access property of null");
        return xr_null();
    }

    // Instance (including Json dynamic-layout objects)
    if (XR_IS_PTR(obj)) {
        XrGCHeader *hdr = XR_TO_PTR(obj);
        if (hdr->type == XR_TINSTANCE) {
            XrInstance *inst = (XrInstance *) hdr;
            if (inst->klass && inst->klass->builtin_kind == XR_BK_JSON) {
                return xr_json_get_by_key(ctx->isolate, (XrJson *) hdr, name);
            }
            return xr_instance_get_field(ctx->isolate, inst, name);
        }

        // Array length
        if (hdr->type == XR_TARRAY && strcmp(name, "length") == 0) {
            XrArray *arr = (XrArray *) hdr;
            return xr_int(xr_array_size(arr));
        }

        // String length
        if (hdr->type == XR_TSTRING && strcmp(name, "length") == 0) {
            XrString *str = (XrString *) hdr;
            return xr_int(str->length);
        }

        // Map length
        if (hdr->type == XR_TMAP && strcmp(name, "length") == 0) {
            XrMap *map = (XrMap *) hdr;
            return xr_int(map->count);
        }
    }

    ctx->error = xr_strdup("Property access not supported for this type");
    return xr_null();
}

// Evaluate index access: obj[idx]
static XrValue eval_index_access(XrEvalContext *ctx, XrValue obj, XrValue idx) {
    if (XR_IS_NULL(obj)) {
        ctx->error = xr_strdup("Cannot index null");
        return xr_null();
    }

    if (XR_IS_PTR(obj)) {
        XrGCHeader *hdr = XR_TO_PTR(obj);

        // Array
        if (hdr->type == XR_TARRAY && XR_IS_INT(idx)) {
            XrArray *arr = (XrArray *) hdr;
            return xr_array_get(arr, (int) XR_TO_INT(idx));
        }

        // Map
        if (hdr->type == XR_TMAP) {
            XrMap *map = (XrMap *) hdr;
            bool found = false;
            XrValue result = xr_map_get(map, idx, &found);
            if (found) {
                return result;
            }
            return xr_null();
        }

        // String
        if (hdr->type == XR_TSTRING && XR_IS_INT(idx)) {
            XrString *str = (XrString *) hdr;
            int i = (int) XR_TO_INT(idx);
            if (i >= 0 && i < (int) str->length) {
                char buf[2] = {str->data[i], '\0'};
                XrString *ch = xr_string_intern(ctx->isolate, buf, 1, 0);
                return xr_string_value(ch);
            }
            return xr_null();
        }
    }

    ctx->error = xr_strdup("Index access not supported for this type");
    return xr_null();
}

// Evaluate binary operation
static XrValue eval_binary(XrEvalContext *ctx, AstNodeType op, XrValue left, XrValue right) {
    // Integer operations
    if (XR_IS_INT(left) && XR_IS_INT(right)) {
        int64_t l = XR_TO_INT(left);
        int64_t r = XR_TO_INT(right);

        switch (op) {
            case AST_BINARY_ADD:
                return xr_int(l + r);
            case AST_BINARY_SUB:
                return xr_int(l - r);
            case AST_BINARY_MUL:
                return xr_int(l * r);
            case AST_BINARY_DIV:
                return r != 0 ? xr_int(l / r) : xr_null();
            case AST_BINARY_MOD:
                return r != 0 ? xr_int(l % r) : xr_null();
            case AST_BINARY_BAND:
                return xr_int(l & r);
            case AST_BINARY_BOR:
                return xr_int(l | r);
            case AST_BINARY_BXOR:
                return xr_int(l ^ r);
            case AST_BINARY_LSHIFT:
                return (r >= 0 && r < 64) ? xr_int(l << r) : xr_int(0);
            case AST_BINARY_RSHIFT:
                return (r >= 0 && r < 64) ? xr_int(l >> r) : xr_int(0);
            case AST_BINARY_LT:
                return xr_bool(l < r);
            case AST_BINARY_LE:
                return xr_bool(l <= r);
            case AST_BINARY_GT:
                return xr_bool(l > r);
            case AST_BINARY_GE:
                return xr_bool(l >= r);
            case AST_BINARY_EQ:
            case AST_BINARY_EQ_STRICT:
                return xr_bool(l == r);
            case AST_BINARY_NE:
            case AST_BINARY_NE_STRICT:
                return xr_bool(l != r);
            default:
                break;
        }
    }

    // Float operations
    if ((XR_IS_INT(left) || XR_IS_FLOAT(left)) && (XR_IS_INT(right) || XR_IS_FLOAT(right))) {
        double l = XR_IS_INT(left) ? (double) XR_TO_INT(left) : XR_TO_FLOAT(left);
        double r = XR_IS_INT(right) ? (double) XR_TO_INT(right) : XR_TO_FLOAT(right);

        switch (op) {
            case AST_BINARY_ADD:
                return xr_float(l + r);
            case AST_BINARY_SUB:
                return xr_float(l - r);
            case AST_BINARY_MUL:
                return xr_float(l * r);
            case AST_BINARY_DIV:
                return r != 0.0 ? xr_float(l / r) : xr_null();
            case AST_BINARY_LT:
                return xr_bool(l < r);
            case AST_BINARY_LE:
                return xr_bool(l <= r);
            case AST_BINARY_GT:
                return xr_bool(l > r);
            case AST_BINARY_GE:
                return xr_bool(l >= r);
            case AST_BINARY_EQ:
                return xr_bool(l == r);
            case AST_BINARY_NE:
                return xr_bool(l != r);
            default:
                break;
        }
    }

    // String concatenation
    if (XR_IS_STRING(left) && XR_IS_STRING(right) && op == AST_BINARY_ADD) {
        XrString *ls = XR_TO_STRING(left);
        XrString *rs = XR_TO_STRING(right);
        size_t new_len = ls->length + rs->length;
        char *buf = (char *) xr_malloc(new_len + 1);
        memcpy(buf, ls->data, ls->length);
        memcpy(buf + ls->length, rs->data, rs->length);
        buf[new_len] = '\0';
        XrString *result = xr_string_intern(ctx->isolate, buf, new_len, 0);
        xr_free(buf);
        return xr_string_value(result);
    }

    // Logical AND/OR
    if (op == AST_BINARY_AND) {
        return eval_is_truthy(left) ? right : left;
    }
    if (op == AST_BINARY_OR) {
        return eval_is_truthy(left) ? left : right;
    }

    ctx->error = xr_strdup("Unsupported binary operation");
    return xr_null();
}

// Evaluate unary operation
static XrValue eval_unary(XrEvalContext *ctx, AstNodeType op, XrValue operand) {
    switch (op) {
        case AST_UNARY_NEG:
            if (XR_IS_INT(operand))
                return xr_int(-XR_TO_INT(operand));
            if (XR_IS_FLOAT(operand))
                return xr_float(-XR_TO_FLOAT(operand));
            break;
        case AST_UNARY_NOT:
            return xr_bool(!eval_is_truthy(operand));
        case AST_UNARY_BNOT:
            if (XR_IS_INT(operand))
                return xr_int(~XR_TO_INT(operand));
            break;
        default:
            break;
    }
    ctx->error = xr_strdup("Unsupported unary operation");
    return xr_null();
}

// Main AST evaluation function
static XrValue eval_ast(XrEvalContext *ctx, AstNode *node) {
    if (!node || ctx->error)
        return xr_null();

    switch (node->type) {
        // Literals
        case AST_LITERAL_INT:
            return xr_int(node->as.literal.raw_value.int_val);
        case AST_LITERAL_FLOAT:
            return xr_float(node->as.literal.raw_value.float_val);
        case AST_LITERAL_STRING: {
            const char *s = node->as.literal.raw_value.string_val;
            XrString *str = xr_string_intern(ctx->isolate, s, strlen(s), 0);
            return xr_string_value(str);
        }
        case AST_LITERAL_TRUE:
            return xr_bool(true);
        case AST_LITERAL_FALSE:
            return xr_bool(false);
        case AST_LITERAL_NULL:
            return xr_null();

        // Variable
        case AST_VARIABLE:
            return eval_lookup_local(ctx, node->as.variable.name);

        // Grouping
        case AST_GROUPING:
            return eval_ast(ctx, node->as.grouping);

        // Member access
        case AST_MEMBER_ACCESS: {
            XrValue obj = eval_ast(ctx, node->as.member_access.object);
            if (ctx->error)
                return xr_null();
            return eval_member_access(ctx, obj, node->as.member_access.name);
        }

        // Index access
        case AST_INDEX_GET: {
            XrValue obj = eval_ast(ctx, node->as.index_get.array);
            if (ctx->error)
                return xr_null();
            XrValue idx = eval_ast(ctx, node->as.index_get.index);
            if (ctx->error)
                return xr_null();
            return eval_index_access(ctx, obj, idx);
        }

        // Binary operations
        case AST_BINARY_ADD:
        case AST_BINARY_SUB:
        case AST_BINARY_MUL:
        case AST_BINARY_DIV:
        case AST_BINARY_MOD:
        case AST_BINARY_BAND:
        case AST_BINARY_BOR:
        case AST_BINARY_BXOR:
        case AST_BINARY_LSHIFT:
        case AST_BINARY_RSHIFT:
        case AST_BINARY_LT:
        case AST_BINARY_LE:
        case AST_BINARY_GT:
        case AST_BINARY_GE:
        case AST_BINARY_EQ:
        case AST_BINARY_NE:
        case AST_BINARY_EQ_STRICT:
        case AST_BINARY_NE_STRICT:
        case AST_BINARY_AND:
        case AST_BINARY_OR: {
            XrValue left = eval_ast(ctx, node->as.binary.left);
            if (ctx->error)
                return xr_null();
            XrValue right = eval_ast(ctx, node->as.binary.right);
            if (ctx->error)
                return xr_null();
            return eval_binary(ctx, node->type, left, right);
        }

        // Unary operations
        case AST_UNARY_NEG:
        case AST_UNARY_NOT:
        case AST_UNARY_BNOT: {
            XrValue operand = eval_ast(ctx, node->as.unary.operand);
            if (ctx->error)
                return xr_null();
            return eval_unary(ctx, node->type, operand);
        }

        // Ternary
        case AST_TERNARY: {
            XrValue cond = eval_ast(ctx, node->as.ternary.condition);
            if (ctx->error)
                return xr_null();
            return eval_is_truthy(cond) ? eval_ast(ctx, node->as.ternary.true_expr)
                                        : eval_ast(ctx, node->as.ternary.false_expr);
        }

        // Nullish coalesce
        case AST_NULLISH_COALESCE: {
            XrValue left = eval_ast(ctx, node->as.binary.left);
            if (ctx->error)
                return xr_null();
            if (!XR_IS_NULL(left))
                return left;
            return eval_ast(ctx, node->as.binary.right);
        }

        default:
            ctx->error = xr_strdup("Expression type not supported in debugger");
            return xr_null();
    }
}

// ============================================================================
// Public API
// ============================================================================

// Internal evaluate function that returns both string and value
static char *debug_evaluate_internal(XrayIsolate *isolate, const char *expression, int frame_idx,
                                     XrValue *out_value) {
    if (!expression || !isolate) {
        if (out_value)
            *out_value = xr_null();
        return xr_strdup("<error: invalid arguments>");
    }

    // Setup evaluation context - use coroutine context if available
    XrEvalContext ctx = {0};
    ctx.isolate = isolate;
    ctx.frame_idx = frame_idx;

    // Get frame and stack from coroutine or main VM
    XrCoroutine *coro = xr_debug_get_coro(isolate);
    int frame_count =
        coro ? coro->vm_ctx.frame_count : xr_isolate_get_vm_state(isolate)->frame_count;
    XrBcCallFrame *frames = coro ? coro->vm_ctx.frames : xr_isolate_get_vm_state(isolate)->frames;
    XrValue *stack = coro ? coro->vm_ctx.stack : xr_isolate_get_vm_state(isolate)->stack;

    if (frame_idx >= 0 && frame_idx < frame_count) {
        int actual_idx = frame_count - 1 - frame_idx;
        ctx.frame = &frames[actual_idx];
        if (ctx.frame->closure && ctx.frame->closure->proto) {
            ctx.proto = ctx.frame->closure->proto;
            ctx.stack_base = stack + ctx.frame->base_offset;
        }
    }

    // Parse expression as a self-contained translation unit. The returned
    // node is an AST_PROGRAM whose first child is the parsed expression;
    // xr_program_destroy() releases the program and its owning arena.
    AstNode *program = xr_parse_expression_string(isolate, expression, "<eval>");
    if (!program) {
        if (out_value)
            *out_value = xr_null();
        char buf[256];
        snprintf(buf, sizeof(buf), "<parse error: %s>", expression);
        return xr_strdup(buf);
    }

    AstNode *ast = program->as.program.statements[0];

    // Evaluate AST
    XrValue result = eval_ast(&ctx, ast);

    // Free AST (also releases the owning arena)
    xr_program_destroy(program);

    // Check for evaluation error
    if (ctx.error) {
        if (out_value)
            *out_value = xr_null();
        char buf[512];
        snprintf(buf, sizeof(buf), "<error: %s>", ctx.error);
        xr_free(ctx.error);
        return xr_strdup(buf);
    }

    if (out_value)
        *out_value = result;

    // Convert result to string
    return xr_value_to_debug_string(isolate, result);
}

char *xr_debug_evaluate(XrayIsolate *isolate, const char *expression, int frame_idx) {
    return debug_evaluate_internal(isolate, expression, frame_idx, NULL);
}

char *xr_debug_evaluate_ex(XrayIsolate *isolate, const char *expression, int frame_idx,
                           int *out_var_ref) {
    XrValue result;
    char *str = debug_evaluate_internal(isolate, expression, frame_idx, &result);

    if (out_var_ref) {
        *out_var_ref = 0;
        // Create variablesReference if result is expandable
        if (xr_debug_value_is_expandable(isolate, result)) {
            *out_var_ref =
                xr_debug_create_var_ref(isolate, xr_debug_get_ref_type(result), frame_idx, result);
        }
    }

    return str;
}
