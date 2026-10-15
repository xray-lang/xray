/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_ctfe_string_append.inc.c - Compiler-owned append through the real String place
 *
 * KEY CONCEPT:
 *   The authenticated root is borrowed without retaining its String. The
 *   common append primitive keeps real COW aliases and charges actual growth.
 */
static XrXirCtfeStatus ctfe_cell_string_append(ScalarRun *run,uint32_t *cursor,CtfeOwnedState *state) {
    const XrXirInstruction *op=&run->function->instructions[*cursor];
    XrXirType type=xr_xir_operand_type(run->function,op->args[0]);
    XrXirCtfeStatus status=ctfe_validation_work(run,state,type);
    if (status!=XR_XIR_CTFE_OK) return status;
    if (state->frame_spare<512) return XR_XIR_CTFE_FRAME_LIMIT;
    if (state->value_header_work>(UINT64_MAX-128)/2 ||
        !scalar_compile_work(run,128+state->value_header_work*2)) return XR_XIR_CTFE_BUDGET;
    XrXirValue cell={(uint32_t)type,0,xr_xir_scalar_load(run->frame,run->layout->offsets[op->args[0]])};
    if (!xr_xir_cell_in_domain(&cell,state->domain)) return XR_XIR_CTFE_BAD_ARTIFACT;
    /* A projected root requires its own atomic Value mutation, not an
     * interior pointer retained across a possible ancestor replacement. */
    if (xr_xir_cell_module_storage(&cell) || xr_xir_cell_is_projection(&cell)) return XR_XIR_CTFE_NOT_SUPPORTED;
    XrXirValue suffix={(uint32_t)xr_xir_operand_type(run->function,op->args[1]),0,
        xr_xir_scalar_load(run->frame,run->layout->offsets[op->args[1]])};
    char buffer[XR_XIR_SCALAR_TEXT_BYTES];const char *bytes=NULL;size_t length=0;
    if (op->immediate) {
        status=ctfe_text_reserve(run,state,&suffix);if (status!=XR_XIR_CTFE_OK) return status;
        if (!xr_xir_scalar_text(&suffix,buffer,&bytes,&length) || length>sizeof(buffer)) return XR_XIR_CTFE_BAD_ARTIFACT;
    }
    XrXirValueAdmission admission=ctfe_value_admission(state);XrXirValuePlace place={0};
    status=ctfe_value_status(xr_xir_cell_authorized_place(&cell,&state->authority,&admission,&place));
    if (status!=XR_XIR_CTFE_OK) return status;
    if (place.type!=XR_XIR_STRING) return XR_XIR_CTFE_BAD_ARTIFACT;
    status=ctfe_value_status(op->immediate?
        xr_xir_string_place_append_utf8(&place,bytes,length,&admission):
        xr_xir_string_place_append(&place,&suffix,&admission));
    if (status==XR_XIR_CTFE_OK) ++*cursor;
    return status;
}
