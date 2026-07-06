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

typedef enum XrCtValueKind {
    XR_CT_NONE,
    XR_CT_INT,
    XR_CT_FLOAT,
    XR_CT_BOOL,
    XR_CT_STRING,
    XR_CT_CHAR,
    XR_CT_NULL,
    XR_CT_FIXED_ARRAY,
} XrCtValueKind;

typedef struct XrCtValue XrCtValue;

typedef struct XrCtFixedArrayValue {
    XrCtValue *elements;
    int count;
} XrCtFixedArrayValue;

struct XrCtValue {
    XrCtValueKind kind;
    union {
        int64_t int_val;
        double float_val;
        bool bool_val;
        const char *string_val;
        uint32_t char_val;
        XrCtFixedArrayValue fixed_array_val;
    } as;
};

XR_FUNC bool xa_consteval_expr(struct XaAnalyzer *analyzer, const struct AstNode *expr,
                               XrCtValue *out_value, const char **out_error);

XR_FUNC bool xa_consteval_int_expr(struct XaAnalyzer *analyzer, const struct AstNode *expr,
                                   int64_t *out_value, const char **out_error);

#endif  // XCONSTEVAL_H
