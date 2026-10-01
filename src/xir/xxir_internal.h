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
XR_FUNC XrXirStatus xr_xir_provenance_functions_match(const XrXirModule *source,
    const XrXirModule *destination, const XrXirOrigin *origins,
    XrXirBudget *remaining, XrXirDiagnostic *diagnostic);
XR_FUNC void xr_xir_provenance_free(XrXirProvenance *provenance);
XR_FUNC XrXirStatus xr_xir_provenance_copy(const XrXirModule *source,
    const XrXirOrigin *origins, uint32_t count, XrXirBudget *remaining,
    XrXirProvenance **output);

struct XrXirArtifact {
    XrXirModule module;
    XrXirBudget budget;
    XrXirTarget target;
    XrXirFunctionLayout *layouts;
    XrXirCheckedPacket checked_packet;
    uint8_t checked_identity[32];
};

/* Requires completed structural and type verification; never re-enters verification. */
XR_FUNC XrXirStatus xr_xir_effects_infer_verified(const XrXirModule *module,
    XrXirBudget *remaining, XrXirEffects **output);

/* Consumes cumulative quotas even on failure; scratch and frame limits remain caps. */
XR_FUNC XrXirStatus xr_xir_verify_remaining(const XrXirModule *module, XrXirBudget *remaining,
    XrXirDiagnostic *diagnostic);
XR_FUNC XrXirStatus xr_xir_layout_build(XrXirArtifact *artifact, const XrXirBudget *budget);
XR_FUNC XrXirStatus xr_xir_recheck(const XrXirModule *checked, const XrXirBudget *budget,
    XrXirArtifact **output, XrXirDiagnostic *diagnostic);
XR_FUNC XrXirStatus xr_xir_layout_verify(const XrXirArtifact *artifact, const XrXirBudget *budget);

/* Internal pipeline entry: preserve accumulated verification budget. */
XR_FUNC XrXirStatus xr_xir_checked_read_remaining(const void *bytes, size_t length,
    XrXirBudget *budget, XrXirArtifact **output, XrXirDiagnostic *diagnostic);
#endif // XXIR_INTERNAL_H
