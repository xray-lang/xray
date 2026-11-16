/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * Synchronous observations of an actual function owner's declaration head. */
#ifndef XXIR_FUNCTION_SIGNATURE_H
#define XXIR_FUNCTION_SIGNATURE_H
#include "xxir.h"
#include "xxir_read_scratch.h"
typedef struct XirMetadataFunctionSignature {
    const char *name;
    uint32_t name_length;
    const XrXirType *parameters;
    uint32_t parameter_count;
    XrXirType result,error_type;
    const uint32_t *consume_parameters;
    uint32_t consume_parameter_count;
    const uint32_t *escaping_parameters;
    uint32_t escaping_parameter_count;
} XirMetadataFunctionSignature;
typedef XrXirStatus (*XirMetadataSignatureRead)(const void *,const XrXirCompileContext *,
    uint32_t,XirReadScratch *,XrCompileRequestControl *,XirMetadataFunctionSignature *);
/* The owner is borrowed through this call. The result lends only its actual
 * immutable declaration spans; no instruction, admission bit or call escapes. */
XR_FUNC XrXirStatus xir_metadata_module_signature(const void *,const XrXirCompileContext *,
    uint32_t,XirReadScratch *,XrCompileRequestControl *,XirMetadataFunctionSignature *);
#endif
