/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_ctfe_stringbuilder_append.inc.c - Compiler-owned Cell String mutation
 *
 * The ordinary String kernel bills its actual copies and growth through the
 * compiler Domain. Scalar rendering and alias summaries use their existing
 * bounded owners; no independent work allowance replenishes that ledger.
 */
static XrXirCtfeStatus ctfe_cell_string_append_step(ScalarRun *run,uint32_t *cursor,CtfeOwnedState *state) {
    const XrXirInstruction *op=&run->function->instructions[*cursor];
    XrXirType type=xr_xir_operand_type(run->function,op->args[0]);
    XrXirValue cell={(uint32_t)type,0,xr_xir_scalar_load(run->frame,run->layout->offsets[op->args[0]])};
    XrXirValue suffix={(uint32_t)xr_xir_operand_type(run->function,op->args[1]),0,
        xr_xir_scalar_load(run->frame,run->layout->offsets[op->args[1]])};
    XrXirCtfeStatus status=ctfe_validation_work(run,state,type);
    if(status!=XR_XIR_CTFE_OK) return status;
    if(!scalar_compile_work(run,state->value_header_work)) return XR_XIR_CTFE_BUDGET;
    if(!xr_xir_cell_in_domain(&cell,state->domain)) return XR_XIR_CTFE_BAD_ARTIFACT;
    if(xr_xir_cell_is_projection(&cell)) return XR_XIR_CTFE_NOT_SUPPORTED;
    char buffer[XR_XIR_SCALAR_TEXT_BYTES];const char *bytes=NULL;size_t length=0;
    XrXirValue rendered={0};const XrXirValue *incoming=&suffix;
    if(op->immediate) {
        status=ctfe_text_reserve(run,state,&suffix);if(status!=XR_XIR_CTFE_OK) return status;
        if(!xr_xir_scalar_text(&suffix,buffer,&bytes,&length) || length>sizeof(buffer)) return XR_XIR_CTFE_BAD_ARTIFACT;
        if(!scalar_compile_work(run,(uint64_t)length)) return XR_XIR_CTFE_BUDGET;
        status=ctfe_value_status(xr_xir_string_new(state->domain,bytes,length,&rendered));
        if(status!=XR_XIR_CTFE_OK) return status;
        incoming=&rendered;
    }
    status=ctfe_mutation_work(run,state,&cell,incoming);
    XrXirValueAdmission admission=ctfe_value_admission(state);XrXirValuePlace place={0};
    if(status==XR_XIR_CTFE_OK) status=ctfe_value_status(xr_xir_cell_authorized_place(&cell,&state->authority,&admission,&place));
    if(status==XR_XIR_CTFE_OK) status=ctfe_value_status(op->immediate?
        xr_xir_string_place_append_utf8(&place,bytes,length,&admission):xr_xir_string_place_append(&place,&suffix,&admission));
    xr_xir_value_drop(&rendered);
    if(status==XR_XIR_CTFE_OK) ++*cursor;
    return status;
}
