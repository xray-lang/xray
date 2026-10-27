/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_emit_view_phi.inc.c - Portable simultaneous borrowed PHI publication
 */
static void emit_view_move_status(CBuffer *buffer) {
    append(buffer," if (status!=XR_XIR_VALUE_OK) return xr_xir_call_fault(\n"
        " status==XR_XIR_VALUE_OOM ? XR_XIR_RUN_OUT_OF_MEMORY :\n"
        " status==XR_XIR_VALUE_LIMIT || status==XR_XIR_VALUE_REFCOUNT_LIMIT ? XR_XIR_RUN_FRAME_LIMIT :\n"
        " status==XR_XIR_VALUE_UNSUPPORTED ? XR_XIR_RUN_UNSUPPORTED : XR_XIR_RUN_BAD_ARTIFACT);\n");
}
static bool emit_view_phi_target(CBuffer *buffer,const XrXirFunction *function,uint32_t target) {
    for (uint32_t i=function->blocks[target].first;i<function->instruction_count && ((buffer)->status == XR_XIR_OK);++i) {
        const XrXirInstruction *phi=&function->instructions[i];if (phi->op!=XR_XIR_PHI) break;
        if (emit_borrowed_flags(buffer,phi->type)) return true;
    }
    return false;
}
static void emit_view_phi_edge(CBuffer *buffer,const XrXirFunction *function,
    const XrXirFunctionLayout *layout,uint32_t instruction,uint32_t target) {
    uint32_t predecessor=emit_block(buffer,function,instruction),count=0,first=function->blocks[target].first;
    append(buffer," { static const XrXirViewPhiTransfer transfers[]={\n");
    for (uint32_t i=first;i<function->instruction_count && ((buffer)->status == XR_XIR_OK);++i) {
        const XrXirInstruction *phi=&function->instructions[i];if (phi->op!=XR_XIR_PHI) break;
        if (phi->type==XR_XIR_UNIT) continue;
        uint32_t source=UINT32_MAX;
        for (uint32_t a=0;a<phi->args[1] && ((buffer)->status == XR_XIR_OK);a+=2)
            if (function->operands[phi->args[0]+a]==predecessor) {source=function->operands[phi->args[0]+a+1];break;}
        if (source==UINT32_MAX) {emit_reject(buffer,XR_XIR_BAD_STRUCTURE);return;}
        uint32_t destination=layout->offsets[function->parameter_count+i];
        append(buffer," {(XrXirType)%uu,%uu,%uu,%uu},\n",(uint32_t)phi->type,layout->offsets[source],destination,destination+8);
        ++count;
    }
    append(buffer," }; XrXirViewPhiBatch batch={state->frame,%uu,transfers,%uu};\n"
        " XrXirValueStatus status=xr_xir_call_view_phi(view,&batch);\n",layout->frame_bytes,count);
    emit_view_move_status(buffer);append(buffer," state->pc=%uu; }\n",first);
}
