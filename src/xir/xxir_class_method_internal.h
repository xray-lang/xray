/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_class_method_internal.h - Owned class slot target observations
 *
 * KEY CONCEPT:
 *   A selected target is derived from one complete module and the actual class.
 */
#ifndef XXIR_CLASS_METHOD_INTERNAL_H
#define XXIR_CLASS_METHOD_INTERNAL_H
#include "xxir.h"

typedef struct XirClassMethodTarget {
    uint32_t function;
    XrXirType receiver_type;
} XirClassMethodTarget;
typedef XrXirStatus (*XirClassMethodVisitor)(void *owner,const XirClassMethodTarget *target);

/* The caller and instruction identify a checked CLASS_METHOD_REF producer.
 * The actual receiver must come from that Program's owning class arena. */
XR_FUNC XrXirStatus xir_compile_class_method_site(const XrXirModule *module,
    uint32_t caller,uint32_t instruction,XrXirType actual_receiver,
    XirClassMethodTarget *output,XrCompileRequestControl *control);
XR_FUNC XrXirStatus xir_compile_class_method_candidates(const XrXirModule *module,
    XrXirType static_receiver,uint32_t slot,XirClassMethodVisitor visit,
    void *owner,XrCompileRequestControl *control);

XR_FUNC XrXirStatus xir_compile_class_declarations_verify(const XrXirModule *module,
    XrXirDiagnostic *location,XrCompileRequestControl *control);

#endif // XXIR_CLASS_METHOD_INTERNAL_H
