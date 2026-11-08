/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_program_receive.inc.c - Current owned Input and Final reception
 */
#include "xxir_layout_input_internal.h"
#include "xxir_effect_program_final_storage.inc.c"

/* Source templates have authentic specialization contexts; closed execution
 * inputs have no such owner. Both cases read the current Control input and
 * locally check declared errors, never a saved constructor result. */

/* Pure final projections follow every temporary CFG release. Root and
 * suspension evidence retain their independent receiving checks. */

/* ROOT correspondence observes the owned equations after receiving callbacks.
 * Independent control recomputation is confined to paranoid builds. */

/* The structural checker derives the same Cell proof through the sole owned
 * Input reader. The raw Lowered projection is matched again after callbacks. */
XR_FUNC XrXirStatus xir_effects_program_cells_match(const XrXirCompileContext *work,
    const XrXirEffects *effects,XrXirDiagnostic *diagnostic) {
    const EffectBodyMaterial *body=NULL;
    XrXirStatus status=effect_program_input_storage(work,effects,&body);
    EffectCellBodyAdapter adapter={0};XrXirCellProofBodyRead read={0};
    if (status==XR_XIR_OK) status=effect_cell_body_owned(work,body,&adapter,&read);
    if (status==XR_XIR_OK)
        status=xr_xir_compile_cell_provenance_body_matches(work,body->cells,&read,diagnostic);
    return status;
}
