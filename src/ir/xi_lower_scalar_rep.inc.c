/* Native-width narrowing and widening helpers for typed containers and fields. */

/* Map typed-array element type to XI_NARROW_* op (for stores).
 * Returns 0 if no narrowing needed (i64/f64/bool/any). */
static uint16_t xi_narrow_op_for_elem(struct XrType *elem_type) {
    if (!elem_type)
        return 0;
    uint8_t nw = elem_type->scalar_rep;
    if (nw == 0) {
        /* Default width: int->i64, float->f64; no narrowing. */
        return 0;
    }
    switch (nw) {
        case XR_NATIVE_I8:
            return XI_NARROW_I8;
        case XR_NATIVE_U8:
            return XI_NARROW_U8;
        case XR_NATIVE_I16:
            return XI_NARROW_I16;
        case XR_NATIVE_U16:
            return XI_NARROW_U16;
        case XR_NATIVE_I32:
            return XI_NARROW_I32;
        case XR_NATIVE_U32:
            return XI_NARROW_U32;
        case XR_NATIVE_F32:
            return XI_NARROW_F32;
        default:
            return 0; /* i64/u64/f64/bool: no narrowing */
    }
}

static uint16_t xi_narrow_op_for_native_type(uint8_t native_type) {
    switch (native_type) {
        case XR_NATIVE_I8:
            return XI_NARROW_I8;
        case XR_NATIVE_U8:
            return XI_NARROW_U8;
        case XR_NATIVE_I16:
            return XI_NARROW_I16;
        case XR_NATIVE_U16:
            return XI_NARROW_U16;
        case XR_NATIVE_I32:
            return XI_NARROW_I32;
        case XR_NATIVE_U32:
            return XI_NARROW_U32;
        case XR_NATIVE_F32:
            return XI_NARROW_F32;
        default:
            return 0;
    }
}

static struct XrType *xi_lower_native_result_type(XiLower *l, struct XrType *fallback,
                                                  uint8_t native_type) {
    struct XrType *type = NULL;
    if (!l || !l->isolate)
        return fallback;
    switch (native_type) {
        case XR_NATIVE_I8:
        case XR_NATIVE_U8:
        case XR_NATIVE_I16:
        case XR_NATIVE_U16:
        case XR_NATIVE_I32:
        case XR_NATIVE_U32:
        case XR_NATIVE_I64:
        case XR_NATIVE_U64:
        case XR_NATIVE_ISIZE:
        case XR_NATIVE_USIZE:
            type = xr_type_new_int_width(l->isolate, native_type);
            break;
        case XR_NATIVE_F32:
            type = xr_type_new_float_width(l->isolate, native_type);
            break;
        default:
            return fallback;
    }
    return type ? type : fallback;
}

static XiValue *xi_lower_narrow_for_native_type(XiLower *l, AstNode *node, XiValue *val,
                                                struct XrType *target_type, uint8_t native_type) {
    uint16_t narrow_op = xi_narrow_op_for_native_type(native_type);
    if (!val)
        return NULL;
    struct XrType *result_type =
        target_type ? target_type : xi_lower_native_result_type(l, val->type, native_type);
    if (!narrow_op) {
        if (!result_type || !val->type || xr_type_equals(result_type, val->type) ||
            !((XR_TYPE_IS_INT(result_type) && XR_TYPE_IS_INT(val->type)) ||
              (XR_TYPE_IS_FLOAT(result_type) && XR_TYPE_IS_FLOAT(val->type))))
            return val;
        XiValue *copy = xi_value_new(l->func, l->cur_block, XI_COPY, result_type, 1);
        if (!copy)
            return val;
        copy->args[0] = val;
        copy->line = (uint32_t) node->line;
        return copy;
    }
    XiValue *n = xi_value_new(l->func, l->cur_block, narrow_op, result_type, 1);
    if (!n)
        return val;
    n->args[0] = val;
    n->line = (uint32_t) node->line;
    return n;
}

static XiValue *xi_lower_narrow_for_native_field(XiLower *l, AstNode *node, XiValue *val,
                                                 uint8_t native_type) {
    return xi_lower_narrow_for_native_type(l, node, val, NULL, native_type);
}

static XiValue *xi_lower_narrow_for_static_type(XiLower *l, AstNode *node, XiValue *val,
                                                struct XrType *target_type) {
    if (!target_type || !val)
        return val;
    XiValue *narrowed =
        xi_lower_narrow_for_native_type(l, node, val, target_type, target_type->scalar_rep);
    if (narrowed != val)
        return narrowed;
    if (val->type && !xr_type_equals(target_type, val->type) &&
        ((XR_TYPE_IS_INT(target_type) && XR_TYPE_IS_INT(val->type)) ||
         (XR_TYPE_IS_FLOAT(target_type) && XR_TYPE_IS_FLOAT(val->type)))) {
        XiValue *copy = xi_value_new(l->func, l->cur_block, XI_COPY, target_type, 1);
        if (!copy)
            return val;
        copy->args[0] = val;
        copy->line = (uint32_t) node->line;
        return copy;
    }
    return val;
}

static void xi_lower_check_map_method_args(XiLower *l, AstNode *node, const char *method,
                                           XiValue *recv, XiValue **args, int n) {
    if (!recv || !recv->type || !XR_TYPE_IS_MAP(recv->type) || !method)
        return;
    if (n == 1 && (strcmp(method, "get") == 0 || strcmp(method, "containsKey") == 0 ||
                   strcmp(method, "delete") == 0)) {
        args[0] = xi_lower_checktype_for_type(l, node, args[0], recv->type->map.key_type);
        return;
    }
    if (n == 2 && strcmp(method, "set") == 0) {
        args[0] = xi_lower_checktype_for_type(l, node, args[0], recv->type->map.key_type);
        args[1] = xi_lower_checktype_for_type(l, node, args[1], recv->type->map.value_type);
    }
}

static void xi_lower_narrow_map_method_args(XiLower *l, AstNode *node, const char *method,
                                            XiValue *recv, XiValue **args, int n) {
    if (!recv || !recv->type || !XR_TYPE_IS_MAP(recv->type) || !method)
        return;
    if (n == 1 && (strcmp(method, "get") == 0 || strcmp(method, "containsKey") == 0 ||
                   strcmp(method, "delete") == 0)) {
        args[0] = xi_lower_narrow_for_static_type(l, node, args[0], recv->type->map.key_type);
        return;
    }
    if (n == 2 && strcmp(method, "set") == 0) {
        args[0] = xi_lower_narrow_for_static_type(l, node, args[0], recv->type->map.key_type);
        args[1] = xi_lower_narrow_for_static_type(l, node, args[1], recv->type->map.value_type);
    }
}

static void xi_lower_narrow_set_method_args(XiLower *l, AstNode *node, const char *method,
                                            XiValue *recv, XiValue **args, int n) {
    if (!recv || !recv->type || !XR_TYPE_IS_SET(recv->type) || !method || n != 1)
        return;
    if (strcmp(method, "add") == 0 || strcmp(method, "contains") == 0 ||
        strcmp(method, "delete") == 0)
        args[0] =
            xi_lower_narrow_for_static_type(l, node, args[0], recv->type->container.element_type);
}

static void xi_lower_check_set_method_args(XiLower *l, AstNode *node, const char *method,
                                           XiValue *recv, XiValue **args, int n) {
    if (!recv || !recv->type || !XR_TYPE_IS_SET(recv->type) || !method || n != 1)
        return;
    if (strcmp(method, "add") == 0 || strcmp(method, "contains") == 0 ||
        strcmp(method, "delete") == 0)
        args[0] = xi_lower_checktype_for_type(l, node, args[0], recv->type->container.element_type);
}

/* Map typed-array element type to XI_WIDEN_* op (for loads).
 * Returns 0 if no widening needed. */
static uint16_t xi_widen_op_for_elem(struct XrType *elem_type) {
    if (!elem_type)
        return 0;
    uint8_t nw = elem_type->scalar_rep;
    if (nw == 0)
        return 0;
    switch (nw) {
        case XR_NATIVE_I8:
            return XI_WIDEN_I8;
        case XR_NATIVE_U8:
            return XI_WIDEN_U8;
        case XR_NATIVE_I16:
            return XI_WIDEN_I16;
        case XR_NATIVE_U16:
            return XI_WIDEN_U16;
        case XR_NATIVE_I32:
            return XI_WIDEN_I32;
        case XR_NATIVE_U32:
            return XI_WIDEN_U32;
        case XR_NATIVE_F32:
            return XI_WIDEN_F32;
        default:
            return 0;
    }
}

static struct XrType *xi_get_container_elem_type(struct XrType *container_type) {
    return (struct XrType *) xr_type_contiguous_element_type(container_type);
}

static int64_t xi_array_cfield_from_type(struct XrType *type) {
    if (!type || !(XR_TYPE_IS_ARRAY(type) || XR_TYPE_IS_SLICE(type)) ||
        !type->container.element_type)
        return 0;
    uint8_t tid = xr_type_to_tid(type->container.element_type);
    return (int64_t) (tid << 2);
}
