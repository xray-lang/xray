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

typedef struct XirClassMethodIndex XirClassMethodIndex;

/* Enumerate implementations of an authenticated call using the same slot index. */
XR_FUNC XrXirStatus xir_compile_class_method_index_candidates(const XirClassMethodIndex *,
    const XrXirArtifact *, uint32_t caller, uint32_t instruction, XirClassMethodVisitor,
    void *owner, XrCompileRequestControl *control);

/* The index borrows one owned Checked Program instance through disposal.
 * A successful query identifies the producer's own slot in a class without
 * descendants. Call shape, lifetime and execution permission are separate. */
XR_FUNC XrXirStatus xir_compile_class_method_index_build(const XrXirArtifact *,
    XirClassMethodIndex **output, XrCompileRequestControl *control);
XR_FUNC XrXirStatus xir_compile_class_method_index_target(const XirClassMethodIndex *,
    const XrXirArtifact *, uint32_t caller, uint32_t instruction, XirClassMethodTarget *output,
    XrCompileRequestControl *control);
XR_FUNC void xir_compile_class_method_index_free(XirClassMethodIndex *);


XR_FUNC XrXirStatus xir_compile_class_declarations_verify(const XrXirModule *module,
    XrXirDiagnostic *location,XrCompileRequestControl *control);

#endif // XXIR_CLASS_METHOD_INTERNAL_H
