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
 *   Only proved private owned captures cross internal activations; exported or
 *   external Cells, module state and runtime authority remain unadmitted.
 */
#include "xxir_compile_domain.h"
#include "../base/xchecks.h"
#include "xxir_type_arena.h"
#include "xxir_operand_roles.h"
#include "xxir_cell_owner_internal.h"
#include "xxir_cell_provenance_internal.h"
#include "xxir_panic.h"
#include "xxir_class.h"
#include "xxir_class_origin_internal.h"
typedef struct CtfeFunctionLease CtfeFunctionLease;

typedef struct CtfeOwnedState {
    XrXirDomain *domain;
    XrXirTypeArena *arena;
    const XrXirCompileContext *context;
    XrXirCtfeTypeShape *tuple_costs;
    uint64_t frame_spare,loan_base;
    uint64_t *loan_bytes;
    uint32_t *identity_roots;
    uint64_t value_header_work;
    XrXirFaultDetail fault;
    struct CtfeCallFrame *activation;
    const XrXirArtifact *artifact;
    struct CtfeOwnedState *identity;
    CtfeFunctionLease *functions;
    XrXirCellProvenance *cells;
    XrXirValue forbidden_cell;
    XrXirValue forbidden_class;
    XrXirCellAuthority authority;
    void *closure;
    uint8_t *reachable;
} CtfeOwnedState;
typedef XrXirCtfeStatus (*CtfeIdentityVisitor)(ScalarRun *,CtfeOwnedState *,const XrXirValue *,void *);
static uint64_t ctfe_frame_spare(const CtfeOwnedState *state) {
    uint64_t growth=state->loan_bytes && *state->loan_bytes>state->loan_base?*state->loan_bytes-state->loan_base:0;
    return growth>state->frame_spare?0:state->frame_spare-growth;
}
static XrXirCtfeStatus ctfe_mutation_work(ScalarRun *run,CtfeOwnedState *state,
    const XrXirValue *owner,const XrXirValue *incoming);

static XrXirValueStatus ctfe_function_admit(void *context,const XrXirFunctionBinding *binding,XrXirType type,uint64_t *work);
static XrXirCtfeStatus ctfe_function_binding(CtfeOwnedState *state,const XrXirValue *value,const XrXirFunctionBinding **output);
static XrXirCtfeStatus ctfe_projection_access_work(ScalarRun *run,CtfeOwnedState *state,const XrXirValue *value,bool mutate,uint32_t suffix);
static XrXirCtfeStatus ctfe_projection_incoming_work(ScalarRun *run,CtfeOwnedState *state,const XrXirValue *cell,const XrXirValue *value);
static XrXirCtfeStatus ctfe_projection_step(ScalarRun *run,VmState *vm,CtfeOwnedState *state);
static XrXirValueAdmission ctfe_value_admission(CtfeOwnedState *state) {
    return (XrXirValueAdmission){.arena=state->arena,.domain=state->domain,.function=ctfe_function_admit,
        .context=state,.work=UINT64_MAX,.scratch_bytes=ctfe_frame_spare(state)};
}
static XrXirCtfeStatus ctfe_value_status(XrXirValueStatus status) {
    return status==XR_XIR_VALUE_OK?XR_XIR_CTFE_OK:
        status==XR_XIR_VALUE_OOM?XR_XIR_CTFE_OUT_OF_MEMORY:
        status==XR_XIR_VALUE_LIMIT || status==XR_XIR_VALUE_REFCOUNT_LIMIT?XR_XIR_CTFE_BUDGET:
        status==XR_XIR_VALUE_UNSUPPORTED?XR_XIR_CTFE_NOT_SUPPORTED:XR_XIR_CTFE_BAD_ARTIFACT;
}
static XrXirCtfeStatus ctfe_bounds_fault(ScalarRun *run,CtfeOwnedState *state,XrXirFaultDetail fault) {
    if (!xr_xir_fault_bounds_valid(fault)) return XR_XIR_CTFE_BAD_ARTIFACT;
    if (!scalar_compile_work(run,sizeof(fault))) return XR_XIR_CTFE_BUDGET;
    state->fault=fault;return XR_XIR_CTFE_BOUNDS;
}
static bool ctfe_cell_type(const XrXirTypes *types,XrXirType type) {
    XrXirType element=xr_xir_cell_element(types,type);
    return xr_xir_type_is_cell(types,type) && (element==XR_XIR_ERROR || element==XR_XIR_PANIC_INFO || ctfe_result_type(element) || ctfe_execution_node(types,element,true));
}
static bool ctfe_storage_instruction(const XrXirModule *module,const XrXirFunction *body,
    const XrXirInstruction *op) {
    if (op->op==XR_XIR_INVOKE || op->op==XR_XIR_INVOKE_INDIRECT || op->op==XR_XIR_INVOKE_RESULT) return true;
    if (xr_xir_type_is_class(module->types,op->type)) return op->op==XR_XIR_CLASS_NEW || op->op==XR_XIR_CLASS_GET || op->op==XR_XIR_OBJECT_PLACE ||
        op->op==XR_XIR_CALL || op->op==XR_XIR_CALL_INDIRECT || op->op==XR_XIR_CELL_READ || op->op==XR_XIR_TUPLE_FIELD || op->op==XR_XIR_ARRAY_GET ||
        op->op==XR_XIR_NULLABLE_UNWRAP || op->op==XR_XIR_ENUM_GET || op->op==XR_XIR_STRUCT_GET || op->op==XR_XIR_FIELD_PLACE || op->op==XR_XIR_PLACE_READ || op->op==XR_XIR_PHI ||
        op->op==XR_XIR_LOCAL_UNINIT || op->op==XR_XIR_OWNED_LOCAL_NEW || op->op==XR_XIR_OWNED_LOCAL_READ || op->op==XR_XIR_OWNED_RETAIN;
    if (op->type==XR_XIR_PANIC_INFO) return op->op==XR_XIR_PANIC_CATCH || op->op==XR_XIR_CALL || op->op==XR_XIR_CALL_INDIRECT ||
        op->op==XR_XIR_PHI || op->op==XR_XIR_LOCAL_UNINIT || op->op==XR_XIR_OWNED_LOCAL_NEW || op->op==XR_XIR_OWNED_LOCAL_READ ||
        op->op==XR_XIR_OWNED_RETAIN || op->op==XR_XIR_CELL_READ || op->op==XR_XIR_TUPLE_FIELD || op->op==XR_XIR_NULLABLE_UNWRAP ||
        op->op==XR_XIR_ENUM_GET || op->op==XR_XIR_STRUCT_GET || op->op==XR_XIR_ARRAY_GET;
    if (op->type==XR_XIR_ERROR) return op->op==XR_XIR_ERROR_ERASE || op->op==XR_XIR_INVOKE_ERROR || op->op==XR_XIR_CALL ||
        op->op==XR_XIR_CALL_INDIRECT || op->op==XR_XIR_PHI || op->op==XR_XIR_LOCAL_UNINIT || op->op==XR_XIR_OWNED_LOCAL_NEW ||
        op->op==XR_XIR_OWNED_LOCAL_READ || op->op==XR_XIR_OWNED_RETAIN || op->op==XR_XIR_CELL_READ ||
        op->op==XR_XIR_TUPLE_FIELD || op->op==XR_XIR_NULLABLE_UNWRAP || op->op==XR_XIR_ENUM_GET || op->op==XR_XIR_STRUCT_GET || op->op==XR_XIR_ARRAY_GET;
    if (op->op==XR_XIR_CELL_READ || op->op==XR_XIR_CELL_WRITE || op->op==XR_XIR_CELL_LOCAL_WRITE)
        return ctfe_cell_type(module->types,xr_xir_operand_type(body,op->args[0]));
    if (ctfe_scalar_type(op->type) || op->type==XR_XIR_UNIT) return true;
    if (xr_xir_type_is_callable(module->types,op->type)) return op->op==XR_XIR_FUNCTION_REF || op->op==XR_XIR_FUNCTION_WEAKEN ||
        op->op==XR_XIR_CALL || op->op==XR_XIR_CALL_INDIRECT || op->op==XR_XIR_TUPLE_FIELD || op->op==XR_XIR_NULLABLE_UNWRAP ||
        op->op==XR_XIR_ENUM_GET || op->op==XR_XIR_STRUCT_GET || op->op==XR_XIR_ARRAY_GET ||
        op->op==XR_XIR_PHI || op->op==XR_XIR_LOCAL_UNINIT || op->op==XR_XIR_OWNED_LOCAL_NEW ||
        op->op==XR_XIR_OWNED_LOCAL_READ || op->op==XR_XIR_OWNED_LOCAL_WRITE || op->op==XR_XIR_OWNED_RETAIN;
    if (ctfe_composite_node(module->types,op->type)) return op->op==XR_XIR_CLASS_GET || op->op==XR_XIR_ERROR_NARROW || op->op==XR_XIR_STRUCT_NEW || op->op==XR_XIR_STRUCT_GET || op->op==XR_XIR_ENUM_NEW || op->op==XR_XIR_ENUM_GET || op->op==XR_XIR_TUPLE_NEW ||
        op->op==XR_XIR_NULLABLE_NONE || op->op==XR_XIR_NULLABLE_SOME || op->op==XR_XIR_NULLABLE_UNWRAP || op->op==XR_XIR_ARRAY_NEW || op->op==XR_XIR_ARRAY_REPEAT ||
        op->op==XR_XIR_ARRAY_WITH_CAPACITY || op->op==XR_XIR_ARRAY_RESERVE || op->op==XR_XIR_ARRAY_GET || op->op==XR_XIR_CELL_PLACE || op->op==XR_XIR_INDEX_PLACE || op->op==XR_XIR_FIELD_PLACE || op->op==XR_XIR_PLACE_READ ||
        op->op==XR_XIR_TUPLE_FIELD || op->op==XR_XIR_CALL || op->op==XR_XIR_CALL_INDIRECT || op->op==XR_XIR_PHI || op->op==XR_XIR_LOCAL_UNINIT ||
        op->op==XR_XIR_OWNED_LOCAL_NEW || op->op==XR_XIR_OWNED_LOCAL_READ || op->op==XR_XIR_OWNED_RETAIN;
    if (op->type==XR_XIR_STRING) return op->op==XR_XIR_CLASS_GET || op->op==XR_XIR_PANIC_MESSAGE || op->op==XR_XIR_STRUCT_GET || op->op==XR_XIR_ENUM_GET || op->op==XR_XIR_CELL_PLACE || op->op==XR_XIR_FIELD_PLACE || op->op==XR_XIR_CONST_STRING || op->op==XR_XIR_CONCAT_STRING || op->op==XR_XIR_TO_STRING ||
        op->op==XR_XIR_CALL || op->op==XR_XIR_CALL_INDIRECT || op->op==XR_XIR_TUPLE_FIELD || op->op==XR_XIR_NULLABLE_UNWRAP ||
        op->op==XR_XIR_ARRAY_GET || op->op==XR_XIR_INDEX_PLACE || op->op==XR_XIR_PLACE_READ || op->op==XR_XIR_PHI || op->op==XR_XIR_LOCAL_UNINIT ||
        op->op==XR_XIR_OWNED_LOCAL_NEW || op->op==XR_XIR_OWNED_LOCAL_READ || op->op==XR_XIR_OWNED_RETAIN;
    return (op->op==XR_XIR_CELL_PROJECT || op->op==XR_XIR_CELL_NEW || op->op==XR_XIR_LOCAL_UNINIT || op->op==XR_XIR_PHI) &&
        ctfe_cell_type(module->types,op->type);
}
static void ctfe_owned_free(CtfeOwnedState *state) {
    XR_CHECK(!state->functions,"compiler function leases escaped their execution scope");
    if (state->cells) xr_xir_compile_cell_provenance_free(state->cells);
    xr_compile_resources_free(state->closure);
    xr_compile_resources_free(state->tuple_costs);xr_xir_domain_drop(state->domain);xr_xir_compile_type_arena_drop(state->arena);
    *state=(CtfeOwnedState){0};
}
static XrXirCtfeStatus ctfe_owned_new(const XrXirArtifact *artifact,bool needed,CtfeOwnedState *state) {
    if (!needed) return XR_XIR_CTFE_OK;
    state->context=&artifact->context;state->artifact=artifact;state->identity=state;
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
    if (!scalar_compile_work(run,state->value_header_work)) return XR_XIR_CTFE_BUDGET;
    if (!create && !xr_xir_cell_in_domain(&cell,state->domain)) return XR_XIR_CTFE_BAD_ARTIFACT;
    bool projected=!create && xr_xir_cell_is_projection(&cell);
    if (op->op!=XR_XIR_CELL_READ && !xr_xir_cell_is_unit(run->module->types,type)) {
        uint32_t id=op->args[op->op==XR_XIR_CELL_NEW?0:1];
        value=(XrXirValue){(uint32_t)xr_xir_operand_type(run->function,id),0,
            xr_xir_scalar_load(run->frame,run->layout->offsets[id])};
    }
    XrXirValue forbidden=state->forbidden_cell;
    if (!create && op->op!=XR_XIR_CELL_READ) state->forbidden_cell=cell;
    XrXirCtfeStatus paid=op->op==XR_XIR_CELL_READ?XR_XIR_CTFE_OK:projected?
        ctfe_projection_incoming_work(run,state,&cell,&value):create?ctfe_admission_work(run,state,&value):ctfe_mutation_work(run,state,&cell,&value);
    state->forbidden_cell=forbidden;
    if (op->op!=XR_XIR_CELL_READ && paid!=XR_XIR_CTFE_OK) return paid;
    /* Scalar admission has no child traversal. This local bound is not a
     * replenished compiler budget: Domain work bills the shared ledger. */
    bool composite=xr_xir_cell_element(run->module->types,type)==XR_XIR_ERROR || ctfe_execution_node(run->module->types,xr_xir_cell_element(run->module->types,type),true)!=NULL;
    XrXirValueAdmission admission=ctfe_value_admission(state);admission.work=composite || projected?UINT64_MAX:8;
    XrXirValueStatus status;
    if (projected) {
        paid=ctfe_projection_access_work(run,state,&cell,op->op!=XR_XIR_CELL_READ,0);
        if (paid!=XR_XIR_CTFE_OK) return paid;
        XrXirValuePath empty={0};XrXirFaultDetail fault={0};
        status=op->op==XR_XIR_CELL_READ?xr_xir_cell_authorized_path_read(&cell,&state->authority,&empty,&admission,&result,&fault):
            xr_xir_cell_authorized_path_write(&cell,&state->authority,&empty,&value,&admission,&fault);
        if (status==XR_XIR_VALUE_BOUNDS) return ctfe_bounds_fault(run,state,fault);
    } else status=create?xr_xir_cell_new(state->domain,state->arena,type,&value,&admission,&result):
        op->op==XR_XIR_CELL_READ?xr_xir_cell_authorized_read(&cell,&state->authority,&result):
        xr_xir_cell_authorized_write(&cell,&state->authority,&value,&admission);
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
static const XrXirNominalVariant *ctfe_enum_variant_info(const XrXirTypes *types,const XrXirTypeNode *node,uint32_t variant);
#include "xxir_ctfe_tuple.inc.c"
#include "xxir_ctfe_nullable.inc.c"
#include "xxir_ctfe_nominal.inc.c"
#include "xxir_ctfe_error_value.inc.c"
#include "xxir_ctfe_panic_value.inc.c"
#include "xxir_ctfe_class.inc.c"
#include "xxir_ctfe_array.inc.c"
#include "xxir_ctfe_path.inc.c"
#include "xxir_ctfe_projection.inc.c"
#include "xxir_ctfe_equal.inc.c"

#include "xxir_ctfe_function.inc.c"

#include "xxir_ctfe_identity.inc.c"
#include "xxir_ctfe_import.inc.c"
