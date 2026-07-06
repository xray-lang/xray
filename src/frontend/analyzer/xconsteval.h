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
} XrCtValueKind;

typedef struct XrCtValue {
    XrCtValueKind kind;
    union {
        int64_t int_val;
        double float_val;
        bool bool_val;
        const char *string_val;
        uint32_t char_val;
    } as;
} XrCtValue;

XR_FUNC bool xa_consteval_expr(struct XaAnalyzer *analyzer, const struct AstNode *expr,
                               XrCtValue *out_value, const char **out_error);

XR_FUNC bool xa_consteval_int_expr(struct XaAnalyzer *analyzer, const struct AstNode *expr,
                                   int64_t *out_value, const char **out_error);

#endif  // XCONSTEVAL_H
