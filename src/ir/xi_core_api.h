#ifndef XI_CORE_API_H
#define XI_CORE_API_H

#ifndef XI_H
#error "xi_core_api.h must be included from xi.h after core Xi types are defined"
#endif

#define XI_ARENA_INITIAL_SIZE (64 * 1024)

XR_FUNC XiFunc *xi_func_new(const char *name, struct XrType *return_type);
XR_FUNC void xi_func_free(XiFunc *f);
XR_FUNC void xi_func_set_stage_recursive(XiFunc *f, XiStage stage);
XR_FUNC void *xi_func_arena_alloc(XiFunc *f, uint32_t size);
XR_FUNC void xi_func_compute_effects(XiFunc *f);
XR_FUNC bool xi_func_set_param_passing_mode(XiFunc *f, uint16_t index, uint8_t mode);
XR_FUNC uint8_t xi_func_param_passing_mode(const XiFunc *f, uint16_t index);

XR_FUNC XiBlock *xi_block_new(XiFunc *f);
XR_FUNC void xi_block_add_pred(XiBlock *blk, XiBlock *pred);
XR_FUNC bool xi_block_ensure_value_capacity(XiBlock *blk, uint32_t min_cap);

XR_FUNC XiValue *xi_value_new(XiFunc *f, XiBlock *blk, uint16_t op, struct XrType *type,
                              uint16_t nargs);
XR_FUNC XiValue *xi_value_insert_after(XiFunc *f, XiBlock *blk, XiValue *anchor, uint16_t op,
                                       struct XrType *type, uint16_t nargs);
XR_FUNC XiValue *xi_const_int(XiFunc *f, XiBlock *blk, int64_t val, struct XrType *int_type);
XR_FUNC XiValue *xi_const_float(XiFunc *f, XiBlock *blk, double val, struct XrType *float_type);
XR_FUNC XiValue *xi_const_bool(XiFunc *f, XiBlock *blk, bool val, struct XrType *bool_type);
XR_FUNC XiValue *xi_const_char(XiFunc *f, XiBlock *blk, uint32_t val, struct XrType *char_type);
XR_FUNC XiValue *xi_const_null(XiFunc *f, XiBlock *blk, struct XrType *null_type);
XR_FUNC XiValue *xi_const_str(XiFunc *f, XiBlock *blk, const char *str, struct XrType *str_type);
XR_FUNC XiValue *xi_const_bigint(XiFunc *f, XiBlock *blk, const char *digits,
                                 struct XrType *bigint_type);
XR_FUNC XiValue *xi_binary(XiFunc *f, XiBlock *blk, uint16_t op, struct XrType *type, XiValue *lhs,
                           XiValue *rhs);
XR_FUNC XiValue *xi_unary(XiFunc *f, XiBlock *blk, uint16_t op, struct XrType *type, XiValue *arg);
XR_FUNC XiValue *xi_param(XiFunc *f, XiBlock *blk, uint16_t index, struct XrType *type);
XR_FUNC XiPhi *xi_phi_new(XiFunc *f, XiBlock *blk, struct XrType *type, uint16_t npreds);

XR_FUNC void xi_block_set_return(XiBlock *blk, XiValue *val);
XR_FUNC void xi_block_set_jump(XiBlock *blk, XiBlock *target);
XR_FUNC void xi_block_set_if(XiBlock *blk, XiValue *cond, XiBlock *then_blk, XiBlock *else_blk);

XR_FUNC void xi_func_dump(const XiFunc *f, void *stream);

#endif  // XI_CORE_API_H
