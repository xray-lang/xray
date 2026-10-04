/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_internal.h - Private ownership of immutable XIR artifacts
 *
 * KEY CONCEPT:
 *   Only checked stage transitions construct physical layout tables.
 */

#ifndef XXIR_INTERNAL_H
#define XXIR_INTERNAL_H

#include "xxir.h"
#include "xxir_checked.h"
#include "xxir_effects.h"

typedef struct XrXirOrigin {
    uint32_t function;
    const XrXirType *arguments;
    uint32_t argument_count;
} XrXirOrigin;

/* Owned evidence storage alone grants no type or declaration authority. */
typedef struct XrXirProvenance {
    XrXirArtifact *source;
    XrXirOrigin *origins;
    uint32_t count;
} XrXirProvenance;

/* Requires verified source and type pools; proves function correspondence and instance closure. */
XR_FUNC XrXirStatus xr_xir_compile_provenance_functions_match(const XrXirCompileContext *compile_context, const XrXirModule *source, const XrXirModule *destination, const XrXirOrigin *origins, XrXirDiagnostic *diagnostic);
XR_FUNC void xr_xir_compile_provenance_free(XrXirProvenance *provenance);
XR_FUNC XrXirStatus xr_xir_compile_provenance_copy(const XrXirCompileContext *compile_context, const XrXirModule *source, const XrXirOrigin *origins, uint32_t count, XrXirProvenance **output);

struct XrXirArtifact {
    XrXirModule module;
    XrXirCompileContext context;
    XrXirTarget target;
    XrXirFunctionLayout *layouts;
    XrXirCheckedPacket checked_packet;
    uint8_t checked_identity[32];
};

/* Requires completed structural and type verification; never re-enters verification. */
XR_FUNC XrXirStatus xr_xir_compile_effects_infer_verified(const XrXirCompileContext *compile_context, const XrXirModule *module, XrXirEffects **output);

XR_FUNC XrXirStatus xr_xir_compile_layout_build(XrXirArtifact *artifact);
XR_FUNC XrXirStatus xr_xir_compile_recheck(const XrXirCompileContext *compile_context, const XrXirModule *checked, XrXirArtifact **output, XrXirDiagnostic *diagnostic);
XR_FUNC XrXirStatus xr_xir_compile_layout_verify(const XrXirArtifact *artifact);

/* Couples fresh owned decoding and its full verification to immediate lowering.
 * Accepts bytes only; no published artifact grants a verification exemption. */
XR_FUNC XrXirStatus xr_xir_compile_checked_read_lower(const XrXirCompileContext *context,
    const void *bytes, size_t length, const XrXirTarget *target,
    XrXirArtifact **output, XrXirDiagnostic *diagnostic);


#endif // XXIR_INTERNAL_H
