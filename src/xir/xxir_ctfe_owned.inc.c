/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_ctfe_owned.inc.c - Compiler-owned private scalar Cell execution
 *
 * KEY CONCEPT:
 *   Real Cells use the original compiler ledger and immutable type owner.
 *   No Cell parameter, exported Cell, module state or runtime authority enters.
 */
#include "xxir_compile_domain.h"
#include "xxir_type_arena.h"
#include "xxir_operand_roles.h"

typedef struct CtfeOwnedState {
    XrXirDomain *domain;
    XrXirTypeArena *arena;
    const XrXirCompileContext *context;
    XrXirCtfeTypeShape *tuple_costs;
    uint64_t frame_spare;
} CtfeOwnedState;

static XrXirCtfeStatus ctfe_value_status(XrXirValueStatus status) {
    return status==XR_XIR_VALUE_OK?XR_XIR_CTFE_OK:
        status==XR_XIR_VALUE_OOM?XR_XIR_CTFE_OUT_OF_MEMORY:
        status==XR_XIR_VALUE_LIMIT || status==XR_XIR_VALUE_REFCOUNT_LIMIT?XR_XIR_CTFE_BUDGET:
        status==XR_XIR_VALUE_UNSUPPORTED?XR_XIR_CTFE_NOT_SUPPORTED:XR_XIR_CTFE_BAD_ARTIFACT;
}
static bool ctfe_cell_type(const XrXirTypes *types,XrXirType type) {
    XrXirType element=xr_xir_cell_element(types,type);
    return xr_xir_type_is_cell(types,type) && (ctfe_result_type(element) || ctfe_composite_node(types,element));
}
static bool ctfe_storage_instruction(const XrXirModule *module,const XrXirFunction *body,
    const XrXirInstruction *op) {
    if (op->op==XR_XIR_CELL_READ || op->op==XR_XIR_CELL_WRITE || op->op==XR_XIR_CELL_LOCAL_WRITE)
        return ctfe_cell_type(module->types,xr_xir_operand_type(body,op->args[0]));
    if (ctfe_scalar_type(op->type) || op->type==XR_XIR_UNIT) return true;
    if (ctfe_composite_node(module->types,op->type)) return op->op==XR_XIR_TUPLE_NEW ||
        op->op==XR_XIR_NULLABLE_NONE || op->op==XR_XIR_NULLABLE_SOME || op->op==XR_XIR_NULLABLE_UNWRAP || op->op==XR_XIR_ARRAY_NEW || op->op==XR_XIR_ARRAY_REPEAT ||
        op->op==XR_XIR_ARRAY_WITH_CAPACITY || op->op==XR_XIR_ARRAY_RESERVE || op->op==XR_XIR_ARRAY_GET || op->op==XR_XIR_CELL_PLACE || op->op==XR_XIR_INDEX_PLACE || op->op==XR_XIR_PLACE_READ ||
        op->op==XR_XIR_TUPLE_FIELD || op->op==XR_XIR_CALL || op->op==XR_XIR_PHI || op->op==XR_XIR_LOCAL_UNINIT ||
        op->op==XR_XIR_OWNED_LOCAL_NEW || op->op==XR_XIR_OWNED_LOCAL_READ || op->op==XR_XIR_OWNED_RETAIN;
    if (op->type==XR_XIR_STRING) return op->op==XR_XIR_CONST_STRING || op->op==XR_XIR_CONCAT_STRING ||
        op->op==XR_XIR_CALL || op->op==XR_XIR_TUPLE_FIELD || op->op==XR_XIR_NULLABLE_UNWRAP || op->op==XR_XIR_ARRAY_GET || op->op==XR_XIR_INDEX_PLACE || op->op==XR_XIR_PLACE_READ || op->op==XR_XIR_PHI || op->op==XR_XIR_LOCAL_UNINIT ||
        op->op==XR_XIR_OWNED_LOCAL_NEW || op->op==XR_XIR_OWNED_LOCAL_READ || op->op==XR_XIR_OWNED_RETAIN;
    return (op->op==XR_XIR_CELL_NEW || op->op==XR_XIR_LOCAL_UNINIT || op->op==XR_XIR_PHI) &&
        ctfe_cell_type(module->types,op->type);
}
static void ctfe_owned_free(CtfeOwnedState *state) {
    xr_compile_resources_free(state->tuple_costs);xr_xir_domain_drop(state->domain);xr_xir_compile_type_arena_drop(state->arena);
    *state=(CtfeOwnedState){0};
}
static XrXirCtfeStatus ctfe_owned_new(const XrXirArtifact *artifact,bool needed,CtfeOwnedState *state) {
    if (!needed) return XR_XIR_CTFE_OK;
    state->context=&artifact->context;
    if (!xir_compile_work(&artifact->context,sizeof(*state))) return XR_XIR_CTFE_BUDGET;
    XrXirCtfeStatus status=ctfe_value_status(xr_xir_compile_type_arena_new(
        &artifact->context,artifact->module.types,&state->arena));
    /* Local capacity does not clone the compiler quota. Every allocation and
     * visit still pays the original finite ledger through this Domain. */
    if (status==XR_XIR_CTFE_OK) status=ctfe_value_status(xr_xir_compile_domain_new(
        artifact->context.resources,UINT64_MAX,&state->domain));
    if (status!=XR_XIR_CTFE_OK) ctfe_owned_free(state);
    return status;
}
static void ctfe_owned_frame_clear(const ScalarRun *run) {
    /* Each offset scan is prepaid when its frame is created. Even exhausted
     * execution must complete this allocation-free release path. */
    for (uint32_t i=0;i<run->layout->owned_count;++i)
        xr_xir_owned_slot_clear(run->frame,run->layout->owned_offsets[i]);
}
static bool ctfe_cell_opcode(XrXirOp op) {
    return op==XR_XIR_CELL_NEW || op==XR_XIR_CELL_READ || op==XR_XIR_CELL_WRITE || op==XR_XIR_CELL_LOCAL_WRITE;
}
static XrXirCtfeStatus ctfe_validation_work(ScalarRun *run,CtfeOwnedState *state,XrXirType type);
static XrXirCtfeStatus ctfe_tuple_export_bytes(ScalarRun *run,const XrXirValue *value,
    CtfeOwnedState *state,uint64_t maximum);
static XrXirCtfeStatus ctfe_admission_work(ScalarRun *run,CtfeOwnedState *state,const XrXirValue *value);
static XrXirCtfeStatus ctfe_array_scan_work(ScalarRun *run,CtfeOwnedState *state,XrXirType element,uint64_t count);
static XrXirCtfeStatus ctfe_cell_step(ScalarRun *run,uint32_t *cursor,CtfeOwnedState *state) {
    const XrXirInstruction *op=&run->function->instructions[*cursor];
    uint32_t destination=run->function->parameter_count+*cursor;
    XrXirType type=op->op==XR_XIR_CELL_NEW?op->type:xr_xir_operand_type(run->function,op->args[0]);
    XrXirValue cell={0},value={0},result={0};
    bool create=op->op==XR_XIR_CELL_NEW;
    if (!create) cell=(XrXirValue){(uint32_t)type,0,
        xr_xir_scalar_load(run->frame,run->layout->offsets[op->args[0]])};
    XrXirCtfeStatus validation=ctfe_validation_work(run,state,type);
    if (validation!=XR_XIR_CTFE_OK) return validation;
    bool local=op->op==XR_XIR_CELL_LOCAL_WRITE;
    if (local && !cell.payload) create=true;
    if (!create && !xr_xir_cell_in_domain(&cell,state->domain)) return XR_XIR_CTFE_BAD_ARTIFACT;
    if (op->op!=XR_XIR_CELL_READ && !xr_xir_cell_is_unit(run->module->types,type)) {
        uint32_t id=op->args[op->op==XR_XIR_CELL_NEW?0:1];
        value=(XrXirValue){(uint32_t)xr_xir_operand_type(run->function,id),0,
            xr_xir_scalar_load(run->frame,run->layout->offsets[id])};
    }
    XrXirCtfeStatus paid=op->op==XR_XIR_CELL_READ?XR_XIR_CTFE_OK:ctfe_admission_work(run,state,&value);
    if (op->op!=XR_XIR_CELL_READ && paid!=XR_XIR_CTFE_OK) return paid;
    /* Scalar admission has no child traversal. This local bound is not a
     * replenished compiler budget: Domain work bills the shared ledger. */
    bool composite=ctfe_composite_node(run->module->types,xr_xir_cell_element(run->module->types,type))!=NULL;
    XrXirValueAdmission admission={.arena=state->arena,.domain=state->domain,
        .work=composite?UINT64_MAX:8,.scratch_bytes=state->frame_spare};
    XrXirValueStatus status=create?xr_xir_cell_new(state->domain,state->arena,type,&value,&admission,&result):
        op->op==XR_XIR_CELL_READ?xr_xir_cell_read(&cell,&result):xr_xir_cell_write(&cell,&value,&admission);
    if (status!=XR_XIR_VALUE_OK) { xr_xir_value_drop(&result);return ctfe_value_status(status); }
    if (create) xr_xir_owned_slot_move(run->frame,
        run->layout->offsets[local?op->args[0]:destination],&result);
    else if (op->op==XR_XIR_CELL_READ && op->type!=XR_XIR_UNIT) {
        if (xr_xir_type_is_owned(run->module->types,op->type)) xr_xir_owned_slot_move(run->frame,run->layout->offsets[destination],&result);
        else xr_xir_scalar_store(run->frame,run->layout->offsets[destination],result.payload);
    }
    xr_xir_value_drop(&result);++*cursor;return XR_XIR_CTFE_OK;
}

#include "xxir_ctfe_string.inc.c"
#include "xxir_ctfe_tuple.inc.c"
#include "xxir_ctfe_nullable.inc.c"
static XrXirCtfeStatus ctfe_index_path_step(ScalarRun *run,VmState *vm,CtfeOwnedState *state);
#include "xxir_ctfe_array.inc.c"
#include "xxir_ctfe_path.inc.c"
#include "xxir_ctfe_equal.inc.c"
