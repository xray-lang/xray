/* Native-width narrowing and widening helpers for typed containers and fields. */

/* Map typed-array element type to XI_NARROW_* op (for stores).
 * Returns 0 if no narrowing needed (i64/f64/bool/any). */
static uint16_t xi_narrow_op_for_elem(struct XrType *elem_type) {
    if (!elem_type)
        return 0;
    uint8_t nw = elem_type->native_width;
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

static XiValue *xi_lower_narrow_for_native_field(XiLower *l, AstNode *node, XiValue *val,
                                                 uint8_t native_type) {
    uint16_t narrow_op = xi_narrow_op_for_native_type(native_type);
    if (!narrow_op || !val)
        return val;
    XiValue *n = xi_value_new(l->func, l->cur_block, narrow_op, val->type, 1);
    if (!n)
        return val;
    n->args[0] = val;
    n->line = (uint32_t) node->line;
    return n;
}

static XiValue *xi_lower_narrow_for_static_type(XiLower *l, AstNode *node, XiValue *val,
                                                struct XrType *target_type) {
    if (!target_type || !val)
        return val;
    return xi_lower_narrow_for_native_field(l, node, val, target_type->native_width);
}

static void xi_lower_narrow_map_method_args(XiLower *l, AstNode *node, const char *method,
                                            XiValue *recv, XiValue **args, int n) {
    if (!recv || !recv->type || !XR_TYPE_IS_MAP(recv->type) || !method)
        return;
    if (n == 1 && (strcmp(method, "get") == 0 || strcmp(method, "has") == 0 ||
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
    if (strcmp(method, "add") == 0 || strcmp(method, "has") == 0 || strcmp(method, "delete") == 0)
        args[0] =
            xi_lower_narrow_for_static_type(l, node, args[0], recv->type->container.element_type);
}

/* Map typed-array element type to XI_WIDEN_* op (for loads).
 * Returns 0 if no widening needed. */
static uint16_t xi_widen_op_for_elem(struct XrType *elem_type) {
    if (!elem_type)
        return 0;
    uint8_t nw = elem_type->native_width;
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

/* Get element type from a container type (Array<T> or [N]T). */
static struct XrType *xi_get_container_elem_type(struct XrType *container_type) {
    if (!container_type)
        return NULL;
    if (container_type->kind == XR_KIND_ARRAY)
        return container_type->container.element_type;
    if (container_type->kind == XR_KIND_FIXED_ARRAY)
        return container_type->fixed_array.element_type;
    return NULL;
}

static bool xi_type_is_bytes(struct XrType *type) {
    struct XrType *elem = xi_get_container_elem_type(type);
    return elem && elem->kind == XR_KIND_INT && elem->native_width == XR_NATIVE_U8;
}

static int64_t xi_array_cfield_from_type(struct XrType *type) {
    if (!type || !XR_TYPE_IS_ARRAY(type) || !type->container.element_type)
        return 0;
    uint8_t tid = xr_type_to_tid(type->container.element_type);
    return (int64_t) (tid << 2);
}
