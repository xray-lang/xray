/*
 * xray - Lightweight typed scripting with native concurrency
 *
 * xconsteval.h - Frontend compile-time expression evaluator.
 */

#ifndef XCONSTEVAL_H
#define XCONSTEVAL_H

#include "../../base/xdefs.h"
#include <stdbool.h>
#include <stdint.h>

struct AstNode;
struct XaAnalyzer;
struct XrType;

typedef enum XrCtValueKind {
    XR_CT_NONE,
    XR_CT_INT,
    XR_CT_FLOAT,
    XR_CT_BOOL,
    XR_CT_STRING,
    XR_CT_CHAR,
    XR_CT_NULL,
    XR_CT_FIXED_ARRAY,
    XR_CT_TUPLE,
    XR_CT_STRUCT_VALUE,
} XrCtValueKind;

typedef struct XrCtValue XrCtValue;

typedef struct XrCtElementListValue {
    XrCtValue *elements;
    int count;
} XrCtElementListValue;

typedef struct XrCtFixedArrayValue {
    XrCtValue *elements;
    int count;
    const uint8_t *byte_blob;
    bool is_byte_blob;
} XrCtFixedArrayValue;
typedef XrCtElementListValue XrCtTupleValue;

typedef struct XrCtStructValue {
    struct XrType *exact_type;
    const char *struct_name;
    const char **field_names;
    XrCtValue *field_values;
    int field_count;
} XrCtStructValue;

struct XrCtValue {
    XrCtValueKind kind;
    union {
        int64_t int_val;
        double float_val;
        bool bool_val;
        const char *string_val;
        uint32_t rune_val;
        XrCtFixedArrayValue fixed_array_val;
        XrCtTupleValue tuple_val;
        XrCtStructValue struct_val;
    } as;
};

/* A scalar compile-time value: one that a literal AST node can carry
 * verbatim, so it can be folded in place or published across modules
 * without naming its declaration. */
static inline bool xr_ct_value_kind_is_scalar(XrCtValueKind kind) {
    switch (kind) {
        case XR_CT_INT:
        case XR_CT_FLOAT:
        case XR_CT_BOOL:
        case XR_CT_STRING:
        case XR_CT_CHAR:
        case XR_CT_NULL:
            return true;
        default:
            return false;
    }
}

/* Rewrite an expression node in place into the literal of a scalar
 * compile-time value, keeping its source position. False for a value no
 * literal node can carry; the node is then untouched. */
XR_FUNC bool xa_consteval_fold_node(struct AstNode *node, const XrCtValue *value);
XR_FUNC bool xa_consteval_expr(struct XaAnalyzer *analyzer, const struct AstNode *expr,
                               XrCtValue *out_value, const char **out_error);

XR_FUNC bool xa_consteval_int_expr(struct XaAnalyzer *analyzer, const struct AstNode *expr,
                                   int64_t *out_value, const char **out_error);

#endif  // XCONSTEVAL_H
