/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xaot_class_native.c - AOT native class receiver metadata
 */

#include "xaot_class_native.h"
#include <string.h>

static XaotClassNativeFunc xaot_class_native_no_func(void) {
    XaotClassNativeFunc info;
    memset(&info, 0, sizeof(info));
    return info;
}

static XaotClassNativeFunc xaot_class_native_make_func(const XiClassData *class_data,
                                                       const XiFunc *func, bool is_constructor) {
    XaotClassNativeFunc info = xaot_class_native_no_func();
    if (!class_data || !func || !class_data->instance_layout)
        return info;
    info.class_data = class_data;
    info.func = func;
    info.layout = class_data->instance_layout;
    info.class_name = class_data->class_name;
    info.is_constructor = is_constructor;
    return info;
}

static XaotClassNativeFunc xaot_class_native_func_in_module(const XiModule *module,
                                                            const XiFunc *func,
                                                            bool want_constructor) {
    if (!module || !module->init || !func)
        return xaot_class_native_no_func();

    for (uint16_t ci = 0; ci < module->nclasses; ci++) {
        const XiClassData *class_data = module->classes ? module->classes[ci] : NULL;
        if (!class_data || !class_data->methods || !class_data->child_idx ||
            !class_data->instance_layout)
            continue;

        for (uint16_t mi = 0; mi < class_data->nmethod; mi++) {
            const XiClassMethod *method = &class_data->methods[mi];
            uint16_t child_idx;
            bool is_constructor;
            bool is_instance_method;

            if (method->is_static_constructor || mi >= class_data->ninst + class_data->nstat)
                continue;

            child_idx = class_data->child_idx[mi];
            if (child_idx >= module->init->nchildren || module->init->children[child_idx] != func)
                continue;

            is_constructor = method->is_constructor;
            is_instance_method = !method->is_constructor && !method->is_static;
            if (want_constructor && is_constructor)
                return xaot_class_native_make_func(class_data, func, true);
            if (!want_constructor && is_instance_method)
                return xaot_class_native_make_func(class_data, func, false);
        }
    }

    return xaot_class_native_no_func();
}

XR_FUNC XaotClassNativeFunc xaot_class_native_func(const XaotBundle *bundle, const XiFunc *func) {
    if (!bundle || !func)
        return xaot_class_native_no_func();

    for (uint32_t mi = 0; mi < bundle->nmodules; mi++) {
        XaotClassNativeFunc info = xaot_class_native_func_in_module(
            bundle->modules ? bundle->modules[mi] : NULL, func, false);
        if (info.layout)
            return info;
    }

    for (uint32_t mi = 0; mi < bundle->nmodules; mi++) {
        XaotClassNativeFunc info = xaot_class_native_func_in_module(
            bundle->modules ? bundle->modules[mi] : NULL, func, true);
        if (info.layout)
            return info;
    }

    return xaot_class_native_no_func();
}

XR_FUNC bool xaot_class_native_func_uses_receiver(const XaotBundle *bundle, const XiFunc *func) {
    return xaot_class_native_func(bundle, func).layout != NULL;
}

static const XiValue *xaot_class_native_unwrap_receiver_alias(const XiValue *value) {
    while (value && (value->op == XI_COPY || value->op == XI_MOVE) && value->nargs >= 1)
        value = value->args[0];
    return value;
}

static const XiValue *xaot_class_native_receiver_value_depth(const XaotBundle *bundle,
                                                             const XiFunc *func,
                                                             const XiValue *value, uint8_t depth) {
    if (!xaot_class_native_func_uses_receiver(bundle, func) || !value || depth > 8)
        return NULL;

    value = xaot_class_native_unwrap_receiver_alias(value);
    if (value && value->op == XI_PARAM && value->aux_int == 0)
        return value;
    if (!value || value->op != XI_PHI || value->nargs == 0)
        return NULL;

    const XiValue *receiver = NULL;
    bool saw_receiver_source = false;
    for (uint16_t i = 0; i < value->nargs; i++) {
        const XiValue *arg =
            xaot_class_native_unwrap_receiver_alias(value->args ? value->args[i] : NULL);
        if (arg == value)
            continue;
        const XiValue *arg_receiver =
            xaot_class_native_receiver_value_depth(bundle, func, arg, (uint8_t) (depth + 1));
        if (!arg_receiver)
            return NULL;
        if (receiver && receiver != arg_receiver)
            return NULL;
        receiver = arg_receiver;
        saw_receiver_source = true;
    }
    return saw_receiver_source ? receiver : NULL;
}

XR_FUNC const XiValue *xaot_class_native_receiver_value(const XaotBundle *bundle,
                                                        const XiFunc *func, const XiValue *value) {
    return xaot_class_native_receiver_value_depth(bundle, func, value, 0);
}

static int xaot_class_native_field_index(const XrStructLayout *layout, const char *field) {
    if (!layout || !field || !layout->field_names)
        return -1;
    for (uint16_t i = 0; i < layout->field_count; i++) {
        const char *name = layout->field_names[i];
        if (name && strcmp(name, field) == 0)
            return (int) i;
    }
    return -1;
}

static bool xaot_class_native_receiver_field(const XaotBundle *bundle, const XiFunc *func,
                                             const XiValue *value, XiOp expected_op,
                                             uint8_t expected_native, XaotClassNativeFunc *out_info,
                                             uint16_t *out_idx) {
    XaotClassNativeFunc info = xaot_class_native_func(bundle, func);
    int idx;
    const XrStructFieldLayout *field;

    if (out_info)
        memset(out_info, 0, sizeof(*out_info));
    if (out_idx)
        *out_idx = 0;
    if (!info.layout || !value || value->op != expected_op || value->nargs < 1 ||
        !xaot_class_native_receiver_value(bundle, func, value->args[0]))
        return false;

    idx = xaot_class_native_field_index(info.layout, (const char *) value->aux);
    if (idx < 0 || (uint16_t) idx >= info.layout->field_count)
        return false;

    field = &info.layout->fields[idx];
    if (!field || field->native_type != expected_native)
        return false;

    if (out_info)
        *out_info = info;
    if (out_idx)
        *out_idx = (uint16_t) idx;
    return true;
}

XR_FUNC bool xaot_class_native_receiver_ref_field(const XaotBundle *bundle, const XiFunc *func,
                                                  const XiValue *value, uint8_t expected_native,
                                                  XaotClassNativeFunc *out_info,
                                                  uint16_t *out_idx) {
    return xaot_class_native_receiver_field(bundle, func, value, XI_LOAD_FIELD, expected_native,
                                            out_info, out_idx);
}

XR_FUNC bool xaot_class_native_receiver_store_field(const XaotBundle *bundle, const XiFunc *func,
                                                    const XiValue *value, uint8_t expected_native,
                                                    XaotClassNativeFunc *out_info,
                                                    uint16_t *out_idx) {
    if (!value || value->nargs < 2)
        return false;
    return xaot_class_native_receiver_field(bundle, func, value, XI_STORE_FIELD, expected_native,
                                            out_info, out_idx);
}
