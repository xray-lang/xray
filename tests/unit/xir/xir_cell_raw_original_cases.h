/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_cell_raw_original_cases.h - Independent scalar inventory and full proof replay
 */
#ifndef XIR_CELL_RAW_ORIGINAL_CASES_H
#define XIR_CELL_RAW_ORIGINAL_CASES_H

static bool raw_inventory_original(CellProofBuild *build) {
    const XirMetadataDomain *module=&build->body->domain;
    if (!cell_proof_inventory_begin(build)) return false;
    XrXirCellProvenance *proof=build->proof;
    uint64_t values = 0, physical = 0;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        XrXirCellProofBodyHead head={0};if (!cell_proof_head(build,f,&head)) return false;
        const XrXirCellProofBodyHead *function=&head;
        uint64_t count = (uint64_t)function->parameter_count+function->instruction_count;
        if ((function->parameter_count && !function->parameters) ||
            (function->operand_count && !function->operands))
            return cell_proof_error(build,f,UINT32_MAX,XR_XIR_BAD_STRUCTURE);
        physical += count;
        if (count > UINT32_MAX || physical > UINT32_MAX)
            return cell_proof_error(build,f,UINT32_MAX,XR_XIR_BUDGET);
        bool tracked = false;
        if (build->body->range) {
            for (uint32_t v=0;v<function->parameter_count;++v) {
                if (!cell_proof_work(build,3)) return false;
                XrXirType type=cell_proof_type(build,f,v);
                if (build->status!=XR_XIR_OK ||
                    !cell_proof_inventory_parameter(build,f,function,v,type,&tracked)) return false;
            }
            if (!cell_proof_work(build,sizeof(XrXirCellProofRunCursor))) return false;
            XrXirCellProofRunCursor cursor={.function=f};
            while (cursor.next<function->instruction_count) {
                XrXirCellProofRun run;
                if (!cell_proof_run(build,function,&cursor,&run) ||
                    !cell_proof_inventory_run(build,&cursor,function,&run,&tracked)) return false;
            }
        } else {
            for (uint32_t v = 0; v < (uint32_t)count; ++v) {
                if (!cell_proof_work(build,3)) return false;
                XrXirType type=cell_proof_type(build,f,v);
                if (build->status!=XR_XIR_OK) return false;
                XrXirCellProofInstructionShape shape={0};
                const XrXirCellProofInstructionShape *instruction=NULL;
                if (v>=function->parameter_count) {
                    if (!cell_proof_work(build,sizeof(shape))) return false;
                    if (!cell_proof_shape(build,f,v-function->parameter_count,&shape)) return false;
                    XrXirType independent=cell_proof_type(build,f,v);
                    if (build->status!=XR_XIR_OK) return false;
                    if (type!=independent)
                        return cell_proof_error(build,f,v-function->parameter_count,XR_XIR_BAD_TYPE);
                    instruction=&shape;
                }
                if (!instruction) {
                    if (!cell_proof_inventory_parameter(build,f,function,v,type,&tracked)) return false;
                } else {
                    if (!cell_proof_inventory_value(build,f,function,v)) return false;
                    uint32_t i=v-function->parameter_count;
                    if (instruction->type!=type) return cell_proof_error(build,f,i,XR_XIR_BAD_TYPE);
                    if (!cell_proof_inventory_instruction(build,f,i,type,instruction->op,&tracked)) return false;
                }
            }
        }
        if (!cell_proof_work(build,8)) return false;
        proof->functions[f] = (CellProofFunction){.begin=(uint32_t)values,
            .count=(uint32_t)count,.parameters=function->parameter_count,
            .stored=tracked?(uint32_t)count:0};
        if (tracked) values += count;
        if (values > UINT32_MAX)
            return cell_proof_error(build,f,UINT32_MAX,XR_XIR_BUDGET);
    }
    return cell_proof_inventory_storage(build,values);
}


static XrXirStatus raw_inventory_original_derive(const XrXirCompileContext *context,
    const XrXirCellProofBodyRead *body, XrXirCellProvenance **output, XrXirDiagnostic *diagnostic) {
    if (!cell_proof_body_valid(context,body) || !output || *output) return XR_XIR_BAD_STRUCTURE;
    CellProofBuild build = {0}; build.context = context; build.body = body;
    build.location = (XrXirDiagnostic){XR_XIR_OK,UINT32_MAX,UINT32_MAX,UINT32_MAX,XR_XIR_DIAGNOSTIC_NONE};
    if (raw_inventory_original(&build)) cell_proof_complete(&build);
    cell_proof_workspace_free(&build);
    build.location.status = build.status;
    if (diagnostic) *diagnostic = build.location;
    if (build.status != XR_XIR_OK) xr_xir_compile_cell_provenance_free(build.proof);
    else *output = build.proof;
    return build.status;
}
#endif // XIR_CELL_RAW_ORIGINAL_CASES_H
