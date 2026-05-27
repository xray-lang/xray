#ifndef XI_LOWER_EXPR_HELPERS_H
#define XI_LOWER_EXPR_HELPERS_H

#include "xi.h"
#include "../base/xdefs.h"
#include "../frontend/parser/xast_types.h"

struct XrStructLayout;
struct XrType;
typedef struct XiLower XiLower;

XR_FUNC struct XrStructLayout *xi_lower_struct_layout_of(struct XrType *t);
XR_FUNC int xi_lower_struct_field_index(const struct XrStructLayout *layout, const char *name);
XR_FUNC struct XrType *xi_lower_infer_binary_type(XiLower *l, AstNodeType ast_type,
                                                  struct XrType *left, struct XrType *right);
XR_FUNC struct XrType *xi_lower_infer_unary_type(XiLower *l, AstNodeType ast_type,
                                                 struct XrType *operand);
XR_FUNC uint16_t xi_lower_binary_ast_to_xi_op(AstNodeType ast_type);

#endif  // XI_LOWER_EXPR_HELPERS_H
