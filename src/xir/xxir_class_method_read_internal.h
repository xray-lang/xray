/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_class_method_read_internal.h - Borrowed class slot declaration observations
 *
 * KEY CONCEPT:
 *   Complete body owners supply headers synchronously; a header grants no execution.
 */
#ifndef XXIR_CLASS_METHOD_READ_INTERNAL_H
#define XXIR_CLASS_METHOD_READ_INTERNAL_H
#include "xxir_class_method_internal.h"
#include "xxir_metadata_domain_internal.h"

typedef struct XirClassMethodSignature {
    const char *name;uint32_t name_length;
    const XrXirType *parameters;uint32_t parameter_count;
    XrXirType result,error_type;
    const uint32_t *consume_parameters;uint32_t consume_parameter_count;
    uint32_t escaping_parameter_count;const uint32_t *escaping_parameters;
} XirClassMethodSignature;
typedef struct XirClassMethodRead {
    const XirMetadataDomain *domain;
    const void *owner;
    XrXirStatus (*signature)(const void *owner,uint32_t function,XirClassMethodSignature *output);
} XirClassMethodRead;

/* The domain and all returned spans belong to one already verified body owner.
 * These synchronous observations neither admit a body nor authorize a call. */
XR_FUNC XrXirStatus xir_compile_class_method_select_read(const XirClassMethodRead *input,
    uint32_t slot,XrXirType actual,XirClassMethodTarget *output,XrCompileRequestControl *control);
XR_FUNC XrXirStatus xir_compile_class_method_candidates_read(const XirClassMethodRead *input,
    XrXirType static_receiver,uint32_t slot,XirClassMethodVisitor visit,
    void *owner,XrCompileRequestControl *control);
#endif // XXIR_CLASS_METHOD_READ_INTERNAL_H
