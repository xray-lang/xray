/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_ctfe_path.inc.c - Bounded private Array and struct field routes
 *
 * KEY CONCEPT:
 *   Paths contain selectors, never retained interior addresses. Receiving and
 *   every possible ancestor copy are prepaid before the sole Value mutation.
 */
static XrXirCtfeStatus ctfe_index_path_build(ScalarRun *run,const XrXirInstruction *op,
    CtfeOwnedState *state,XrXirValuePath *path,uint32_t *root) {
    const uint32_t *ids=op->op==XR_XIR_ARRAY_SET?&run->function->operands[op->args[0]]:op->args;
    bool final=op->op==XR_XIR_ARRAY_GET || op->op==XR_XIR_ARRAY_SET || op->op==XR_XIR_STRUCT_SET;
    uint32_t id=ids[0],count=final?1:0;
    while (xr_xir_place_kind(run->function,id)==XR_XIR_PLACE_INDEX || xr_xir_place_kind(run->function,id)==XR_XIR_PLACE_FIELD) {
        if (!scalar_compile_work(run,4)) return XR_XIR_CTFE_BUDGET;
        if (count>=run->layout->path_count) return XR_XIR_CTFE_BAD_ARTIFACT;
        ++count;id=run->function->instructions[id-run->function->parameter_count].args[0];
    }
    if (xr_xir_place_kind(run->function,id)!=XR_XIR_PLACE_CELL && xr_xir_place_kind(run->function,id)!=XR_XIR_PLACE_OBJECT) return XR_XIR_CTFE_NOT_SUPPORTED;
    if (!count) { *root=id;*path=(XrXirValuePath){0};return XR_XIR_CTFE_OK; }
    uint64_t bytes=(uint64_t)count*sizeof(XrXirValuePathStep);
    if (bytes>SIZE_MAX || bytes>state->frame_spare) return XR_XIR_CTFE_FRAME_LIMIT;
    if (!scalar_compile_work(run,bytes)) return XR_XIR_CTFE_BUDGET;
    void *memory=NULL;XrXirCtfeStatus status=ctfe_compile_status(xir_compile_resource_status(
        xr_compile_resources_calloc(run->compile_resources,1,(size_t)bytes,&memory)));
    if (status!=XR_XIR_CTFE_OK) return status;
    XrXirValuePathStep *steps=memory;uint32_t next=count;*root=id;id=ids[0];
    if (final) steps[--next]=(XrXirValuePathStep){op->op==XR_XIR_STRUCT_SET?XR_XIR_PATH_FIELD:XR_XIR_PATH_INDEX,
        xr_xir_operand_type(run->function,id),op->op==XR_XIR_STRUCT_SET?op->immediate:ctfe_array_operand(run,ids[1]).payload};
    while (next) {
        if (!scalar_compile_work(run,sizeof(*steps)+4)) { xr_compile_resources_free(memory);return XR_XIR_CTFE_BUDGET; }
        const XrXirInstruction *projection=&run->function->instructions[id-run->function->parameter_count];
        steps[--next]=(XrXirValuePathStep){projection->op==XR_XIR_FIELD_PLACE?XR_XIR_PATH_FIELD:XR_XIR_PATH_INDEX,
            xr_xir_operand_type(run->function,projection->args[0]),projection->op==XR_XIR_FIELD_PLACE?projection->immediate:ctfe_array_operand(run,projection->args[1]).payload};
        id=projection->args[0];
    }
    *path=(XrXirValuePath){steps,count};return XR_XIR_CTFE_OK;
}
static XrXirCtfeStatus ctfe_index_path_preflight(ScalarRun *run,CtfeOwnedState *state,
    const XrXirValue *root,const XrXirValuePath *path,bool mutate,XrXirValue *target) {
    XrXirValue current=*root;bool owned=false;XrXirCtfeStatus status=XR_XIR_CTFE_OK;
    for (uint32_t i=0;;++i) {
        if (!scalar_compile_work(run,64)) { status=XR_XIR_CTFE_BUDGET;break; }
        status=ctfe_admission_work(run,state,&current);if (status!=XR_XIR_CTFE_OK) break;
        if (i==path->count) {
            if (owned) { *target=current;owned=false; }
            else status=ctfe_value_status(xr_xir_value_copy(&current,target));break;
        }
        if (i && xr_xir_type_is_class(run->module->types,(XrXirType)current.type)) { status=XR_XIR_CTFE_NOT_SUPPORTED;break; }
        if (current.type!=(uint32_t)path->steps[i].container) { status=XR_XIR_CTFE_BAD_ARTIFACT;break; }
        XrXirValueAdmission admission=ctfe_value_admission(state);
        bool field=path->steps[i].kind==XR_XIR_PATH_FIELD;
        if (mutate && field) {
            const XrXirTypeNode *node=xr_xir_type_node(run->module->types,(XrXirType)current.type);
            if (!node || !scalar_compile_work(run,(uint64_t)node->nominal.field_count*sizeof(XrXirValue))) { status=XR_XIR_CTFE_BUDGET;break; }
        } else if (mutate) {
            int64_t capacity=0;status=ctfe_value_status(xr_xir_array_capacity(&current,&admission,&capacity));
            if (status==XR_XIR_CTFE_OK) status=ctfe_array_bytes_work(run,state,(XrXirType)current.type,(uint64_t)capacity);
            if (status!=XR_XIR_CTFE_OK) break;
        }
        XrXirValue child={0};XrXirFaultDetail fault={0};
        if (field && xr_xir_type_is_class(run->module->types,(XrXirType)current.type))
            status=ctfe_class_member_read(run,state,&current,(uint32_t)path->steps[i].selector,&child);
        else {
            XrXirValueStatus received=field?xr_xir_struct_get(&current,(uint32_t)path->steps[i].selector,&admission,&child):
                xr_xir_array_get(&current,path->steps[i].selector,&admission,&child,&fault);
            status=received==XR_XIR_VALUE_BOUNDS?ctfe_bounds_fault(run,state,fault):ctfe_value_status(received);
        }
        if (status!=XR_XIR_CTFE_OK) { xr_xir_value_drop(&child);break; }
        if (owned) xr_xir_value_drop(&current);current=child;owned=true;
    }
    if (owned) xr_xir_value_drop(&current);return status;
}
static XrXirCtfeStatus ctfe_index_path_step(ScalarRun *run,VmState *vm,CtfeOwnedState *state) {
    const XrXirInstruction *op=&run->function->instructions[vm->instruction];
    XrXirValuePath path={0};uint32_t root_id=0;
    XrXirCtfeStatus status=ctfe_index_path_build(run,op,state,&path,&root_id);
    if (status!=XR_XIR_CTFE_OK) return status;
    CtfeOwnedState bounded=*state;bounded.frame_spare-=(uint64_t)path.count*sizeof(*path.steps);
    XrXirValueAdmission admission=ctfe_value_admission(&bounded);
    const XrXirInstruction *root_op=&run->function->instructions[root_id-run->function->parameter_count];
    XrXirValue cell=ctfe_array_operand(run,root_op->args[0]),root={0},target={0},result={0};
    XrXirValuePlace place={0};XrXirFaultDetail fault={0};XrXirCellLoan object_loan={0};bool held=false;
    bool mutate=op->op==XR_XIR_ARRAY_SET || op->op==XR_XIR_ARRAY_PUSH || op->op==XR_XIR_PLACE_WRITE || op->op==XR_XIR_STRUCT_SET;
    if (!scalar_compile_work(run,state->value_header_work)) { status=XR_XIR_CTFE_BUDGET;goto done; }
    bool object=root_op->op==XR_XIR_OBJECT_PLACE,projected=!object && xr_xir_cell_is_projection(&cell);
    if (!object && !xr_xir_cell_in_domain(&cell,state->domain)) { status=XR_XIR_CTFE_BAD_ARTIFACT;goto done; }
    status=ctfe_validation_work(run,&bounded,root_op->type);if (status!=XR_XIR_CTFE_OK) goto done;
    if (object) root=cell;
    else if (projected) {
        status=ctfe_projection_access_work(run,&bounded,&cell,mutate);if (status!=XR_XIR_CTFE_OK) goto done;
        XrXirValuePath empty={0};XrXirValueStatus received=xr_xir_cell_authorized_path_read(&cell,&state->authority,&empty,&admission,&root,&fault);
        status=received==XR_XIR_VALUE_BOUNDS?ctfe_bounds_fault(run,&bounded,fault):ctfe_value_status(received);
        if (status!=XR_XIR_CTFE_OK) goto done;held=true;
    } else {
        status=ctfe_value_status(xr_xir_cell_authorized_place(&cell,&state->authority,&admission,&place));if (status!=XR_XIR_CTFE_OK) goto done;
        root.type=(uint32_t)place.type;memcpy(&root.payload,place.payload,sizeof(root.payload));
    }
    status=ctfe_index_path_preflight(run,&bounded,&root,&path,mutate,&target);if (status!=XR_XIR_CTFE_OK) goto done;
    status=ctfe_admission_work(run,&bounded,&root);if (status!=XR_XIR_CTFE_OK) goto done;
    XrXirValueStatus value_status=XR_XIR_VALUE_OK;
    if (mutate) {
        const uint32_t *ids=op->op==XR_XIR_ARRAY_SET?&run->function->operands[op->args[0]]:op->args;
        XrXirValue incoming=ctfe_array_operand(run,ids[op->op==XR_XIR_ARRAY_SET?2:1]);
        if (object) bounded.forbidden_class=cell;else bounded.forbidden_cell=cell;
        status=projected?ctfe_projection_incoming_work(run,&bounded,&cell,&incoming):ctfe_admission_work(run,&bounded,&incoming);
        if (status!=XR_XIR_CTFE_OK) goto done;
        if (op->op==XR_XIR_ARRAY_PUSH) {
            int64_t capacity=0;status=ctfe_value_status(xr_xir_array_capacity(&target,&admission,&capacity));
            if (status!=XR_XIR_CTFE_OK) goto done;
            if ((uint64_t)capacity>(UINT64_MAX-4)/2) { status=XR_XIR_CTFE_BUDGET;goto done; }
            status=ctfe_array_bytes_work(run,&bounded,(XrXirType)target.type,(uint64_t)capacity*2+4);
            if (status!=XR_XIR_CTFE_OK) goto done;
        }
        /* Observing a child must not manufacture extra sharing during mutation. */
        xr_xir_value_drop(&target);if (held) { xr_xir_value_drop(&root);held=false; }
        if (object) {
            status=ctfe_class_loan_begin(run,&bounded,&cell,&admission,&object_loan);if (status!=XR_XIR_CTFE_OK) goto done;
            value_status=op->op==XR_XIR_ARRAY_PUSH?xr_xir_class_authorized_path_push(&cell,&state->authority,&path,&incoming,&admission,&fault):
                xr_xir_class_authorized_path_write(&cell,&state->authority,&path,&incoming,&admission,&fault);
        } else if (projected) value_status=op->op==XR_XIR_ARRAY_PUSH?
            xr_xir_cell_authorized_path_push(&cell,&state->authority,&path,&incoming,&admission,&fault):
            xr_xir_cell_authorized_path_write(&cell,&state->authority,&path,&incoming,&admission,&fault);
        else value_status=op->op==XR_XIR_ARRAY_PUSH?xr_xir_value_path_push(&place,&path,&incoming,&admission,&fault):
            xr_xir_value_path_write(&place,&path,&incoming,&admission,&fault);
    } else if (op->op==XR_XIR_ARRAY_LEN || op->op==XR_XIR_ARRAY_CAPACITY) {
        int64_t number=0;value_status=op->op==XR_XIR_ARRAY_LEN?xr_xir_array_len(&target,&admission,&number):xr_xir_array_capacity(&target,&admission,&number);
        result=(XrXirValue){XR_XIR_I64,0,number};
    } else if (op->op==XR_XIR_ARRAY_RESERVE) {
        int64_t capacity=ctfe_array_operand(run,op->args[1]).payload;
        if (capacity<0) { status=XR_XIR_CTFE_NUMERIC_RANGE;goto done; }
        status=ctfe_array_bytes_work(run,&bounded,(XrXirType)target.type,(uint64_t)capacity);if (status!=XR_XIR_CTFE_OK) goto done;
        value_status=xr_xir_array_reserve(&target,capacity,&admission,&result);
    } else {
        xr_xir_value_drop(&target);if (held) { xr_xir_value_drop(&root);held=false; }
        if (object) {
            status=ctfe_class_loan_begin(run,&bounded,&cell,&admission,&object_loan);if (status!=XR_XIR_CTFE_OK) goto done;
            value_status=xr_xir_class_authorized_path_read(&cell,&state->authority,&path,&admission,&result,&fault);
        } else value_status=projected?xr_xir_cell_authorized_path_read(&cell,&state->authority,&path,&admission,&result,&fault):
            xr_xir_value_path_read(&place,&path,&admission,&result,&fault);
    }
    status=value_status==XR_XIR_VALUE_BOUNDS?ctfe_bounds_fault(run,&bounded,fault):ctfe_value_status(value_status);
    if (status==XR_XIR_CTFE_OK) {
        if (op->type!=XR_XIR_UNIT) {
            uint32_t offset=run->layout->offsets[run->function->parameter_count+vm->instruction];
            if (xr_xir_type_is_owned(run->module->types,op->type)) xr_xir_owned_slot_move(run->frame,offset,&result);
            else xr_xir_scalar_store(run->frame,offset,result.payload);
        }
        ++vm->instruction;
    }
done:
    if (object_loan.cell) xr_xir_cell_loan_release(&object_loan);
    state->tuple_costs=bounded.tuple_costs;
    state->fault=bounded.fault;
    if (held) xr_xir_value_drop(&root);
    xr_xir_value_drop(&target);xr_xir_value_drop(&result);xr_compile_resources_free((void *)path.steps);return status;
}
