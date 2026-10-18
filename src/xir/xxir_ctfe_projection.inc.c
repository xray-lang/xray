/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_ctfe_projection.inc.c - Actual scoped descriptors from evaluated places
 *
 * KEY CONCEPT:
 *   Value owns selectors and canonical storage. Real loan preparation later
 *   rechecks the current path; no leaf address or private header is cached.
 */
static XrXirCtfeStatus ctfe_projection_path_work(ScalarRun *run,CtfeOwnedState *state,
    const XrXirValue *value,uint32_t suffix,uint64_t *span) {
    if (!scalar_compile_work(run,state->value_header_work)) return XR_XIR_CTFE_BUDGET;
    XrXirValueAdmission admission=ctfe_value_admission(state);uint32_t count=0;
    XrXirCtfeStatus status=ctfe_value_status(xr_xir_cell_projection_steps(value,&admission,&count));
    if (status!=XR_XIR_CTFE_OK) return status;
    uint64_t actual=(uint64_t)count+suffix+1;
    if (actual>UINT64_MAX/64 || !scalar_compile_work(run,actual*64)) return XR_XIR_CTFE_BUDGET;
    *span=actual;return XR_XIR_CTFE_OK;
}
static XrXirCtfeStatus ctfe_projection_step(ScalarRun *run,VmState *vm,CtfeOwnedState *state) {
    const XrXirInstruction *op=&run->function->instructions[vm->instruction];
    XrXirValuePath path={0};uint32_t root_id=0;
    XrXirCtfeStatus status=ctfe_index_path_build(run,op,state,&path,&root_id);
    if (status!=XR_XIR_CTFE_OK) return status;
    if (!path.count) { xr_compile_resources_free((void *)path.steps);return XR_XIR_CTFE_NOT_SUPPORTED; }
    CtfeOwnedState bounded=*state;bounded.frame_spare-=(uint64_t)path.count*sizeof(*path.steps);
    const XrXirInstruction *root=&run->function->instructions[root_id-run->function->parameter_count];
    XrXirValue cell=ctfe_array_operand(run,root->args[0]),result={0};
    status=ctfe_validation_work(run,&bounded,op->type);
    if (status==XR_XIR_CTFE_OK && !scalar_compile_work(run,state->value_header_work)) status=XR_XIR_CTFE_BUDGET;
    if (status==XR_XIR_CTFE_OK && root->op==XR_XIR_OBJECT_PLACE) status=ctfe_admission_work(run,&bounded,&cell);
    else if (status==XR_XIR_CTFE_OK && (!xr_xir_cell_in_domain(&cell,state->domain) || xr_xir_cell_module_storage(&cell))) status=XR_XIR_CTFE_NOT_SUPPORTED;
    uint64_t span=0;
    if (status==XR_XIR_CTFE_OK && root->op!=XR_XIR_OBJECT_PLACE)
        status=ctfe_projection_path_work(run,&bounded,&cell,path.count,&span);
    if (status==XR_XIR_CTFE_OK) {
        XrXirValueAdmission admission=ctfe_value_admission(&bounded);
        status=ctfe_value_status(xr_xir_cell_project_authorized(op->type,&cell,&path,&state->authority,&admission,&result));
    }
    if (status==XR_XIR_CTFE_OK) {
        xr_xir_owned_slot_move(run->frame,run->layout->offsets[run->function->parameter_count+vm->instruction],&result);
        ++vm->instruction;
    }
    state->tuple_costs=bounded.tuple_costs;xr_xir_value_drop(&result);xr_compile_resources_free((void *)path.steps);return status;
}
