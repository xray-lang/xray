/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xaot_class_native.h - AOT native class receiver metadata
 */

#ifndef XAOT_CLASS_NATIVE_H
#define XAOT_CLASS_NATIVE_H

#include "xaot_bundle.h"
#include "../runtime/value/xstruct_layout.h"

typedef struct XaotClassNativeFunc {
    const XiClassData *class_data;
    const XiFunc *func;
    const XrStructLayout *layout;
    const char *class_name;
    bool is_constructor;
} XaotClassNativeFunc;

XR_FUNC XaotClassNativeFunc xaot_class_native_func(const XaotBundle *bundle, const XiFunc *func);
XR_FUNC const XiClassData *xaot_class_native_data_for_type(const XaotBundle *bundle,
                                                           const XrType *type);
XR_FUNC bool xaot_class_native_func_uses_receiver(const XaotBundle *bundle, const XiFunc *func);
XR_FUNC const XiValue *xaot_class_native_receiver_value(const XaotBundle *bundle,
                                                        const XiFunc *func, const XiValue *value);
XR_FUNC bool xaot_class_native_receiver_ref_field(const XaotBundle *bundle, const XiFunc *func,
                                                  const XiValue *value, uint8_t expected_native,
                                                  XaotClassNativeFunc *out_info, uint16_t *out_idx);
XR_FUNC bool xaot_class_native_receiver_store_field(const XaotBundle *bundle, const XiFunc *func,
                                                    const XiValue *value, uint8_t expected_native,
                                                    XaotClassNativeFunc *out_info,
                                                    uint16_t *out_idx);

#endif  // XAOT_CLASS_NATIVE_H
