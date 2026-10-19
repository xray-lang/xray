/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_vm.c - Typed Lowered instruction execution
 *
 * KEY CONCEPT:
 *   Values reside at lowering-owned frame offsets; scalar rules are shared
 *   with generated native code and all exits pass through one frame owner.
 *   A panic raised in, or delivered to, a protected block enters its handler
 *   through the same landing helper that native entries use.
 */

#include "xxir_vm.h"
#include "xxir_vm_debug_internal.h"
#include "xxir_call_native_source_internal.h"
#include "xxir_call_observer_internal.h"
#include "xxir_map.h"
#include "xxir_program_internal.h"
#include "xxir_program_packet_internal.h"
#include "xxir_internal.h"
#include "xxir_tuple.h"
#include "xxir_atomic.h"
#include "xxir_channel.h"
#include "xxir_task.h"
#include "xxir_compile_memory.h"
#include "xxir_equal.h"
#include "xxir_nullable.h"
#include "xxir_float.h"
#include "xxir_types.h"
#include "xxir_type_arena.h"
#include "xxir_operand_roles.h"
#include "xxir_call_view_operation_internal.h"
#include "xxir_call_view_moves_internal.h"
#include "xxir_view_types_internal.h"
#include "xxir_instance_value.h"
#include "xxir_instance_hash.h"
#include "xxir_instance_function_internal.h"
#include "xxir_instance_generator_internal.h"
#include "xxir_struct.h"
#include "xxir_class.h"
#include "xxir_enum.h"
#include "xxir_error.h"
#include "xxir_panic.h"
#include "../base/xmalloc.h"

typedef struct ScalarRun {
    const XrXirModule *module;
    const XrXirFunction *function;
    const XrXirFunctionLayout *layout;
    void *frame;
    XrXirCallView *view;
    XrCompileResources *compile_resources;
} ScalarRun;



#include "xxir_interpreter_state.h"
#include "xxir_scalar_core.h"
_Static_assert(sizeof(ScalarRun)==sizeof(XirScalarCoreRun), "scalar descriptor physical size is unchanged");
#include "xxir_vm_debug_pending.inc.c"



static XrXirRunStatus value_run_status(XrXirValueStatus status) {
    if (status == XR_XIR_VALUE_OK) return XR_XIR_RUN_OK;
    if (status == XR_XIR_VALUE_OOM) return XR_XIR_RUN_OUT_OF_MEMORY;
    if (status == XR_XIR_VALUE_UNSUPPORTED) return XR_XIR_RUN_UNSUPPORTED;
    if (status == XR_XIR_VALUE_LIMIT || status == XR_XIR_VALUE_REFCOUNT_LIMIT)
        return XR_XIR_RUN_FRAME_LIMIT;
    return XR_XIR_RUN_BAD_ARTIFACT;
}

static XrXirValue vm_value_operand(const ScalarRun *run,uint32_t id);
static XrXirRunStatus instance_step(ScalarRun *run, VmState *state, const XrXirInstruction *op, uint32_t destination, XrXirAction *action) {
    XrXirFaultDetail fault={0};
    uint32_t ordinal=(uint32_t)(op-run->function->instructions);
    XrXirValue value = {0};
    XrXirCallStatus status = XR_XIR_CALL_READY;
    switch (op->op) {
    case XR_XIR_CELL_LOCAL_WRITE: {
        uint32_t offset = run->layout->offsets[op->args[0]];
        XrXirType type = xr_xir_operand_type(run->function, op->args[0]);
        XrXirValue cell = vm_value_operand(run,op->args[0]);
        XrXirValue incoming = {0};
        if (xr_xir_cell_payload_operands(run->module->types, type, op->op) == 2)
            incoming = vm_value_operand(run,op->args[1]);
        if (cell.payload) status = xr_xir_instance_cell_write_at(run->view, ordinal, &cell, &incoming, &fault);
        else {
            status = xr_xir_instance_cell(run->view, type, &incoming, &value);
            if (status == XR_XIR_CALL_READY) xr_xir_owned_slot_move(run->frame, offset, &value);
        }
        break;
    }
    case XR_XIR_CELL_STRING_APPEND: {
        XrXirValue cell = vm_value_operand(run,op->args[0]);
        XrXirValue suffix = vm_value_operand(run,op->args[1]);
        status = op->immediate ? xr_xir_instance_cell_string_append_scalar(run->view, &cell, &suffix) :
            xr_xir_instance_cell_string_append(run->view, &cell, &suffix); break;
    }
    case XR_XIR_CELL_NEW: case XR_XIR_CELL_READ: case XR_XIR_CELL_WRITE: {
        XrXirType cell = op->op == XR_XIR_CELL_NEW ? op->type : xr_xir_operand_type(run->function, op->args[0]);
        uint32_t count = xr_xir_cell_payload_operands(run->module->types, cell, op->op);
        XrXirValue left = {0};
        if (count) left = vm_value_operand(run,op->args[0]);
        if (op->op == XR_XIR_CELL_NEW) status = xr_xir_instance_cell(run->view, op->type, &left, &value);
        else if (op->op == XR_XIR_CELL_READ) status = xr_xir_instance_cell_read_at(run->view, ordinal, &left, &value, &fault);
        else {
            XrXirValue right = {0};
            if (count == 2) right = vm_value_operand(run,op->args[1]);
            status = xr_xir_instance_cell_write_at(run->view, ordinal, &left, &right, &fault);
        }
        break;
    }
    case XR_XIR_FUNCTION_WEAKEN: {
        XrXirValue input = vm_value_operand(run,op->args[0]);
        status = xr_xir_instance_weaken_function(run->view, op->type, &input, &value); break;
    }
    case XR_XIR_FUNCTION_REF:
        for (uint32_t i = 0; i < op->args[1]; ++i) {
            uint32_t id = run->function->operands[op->args[0] + i];
            state->arguments[i] = vm_value_operand(run,id);
        }
        status = xr_xir_instance_function_at(run->view,(uint32_t)(op-run->function->instructions),
            state->arguments,op->args[1],&value); break;
    case XR_XIR_CONST_STRING:
        status = xr_xir_instance_literal(run->view, (uint32_t) op->immediate, &value); break;
    case XR_XIR_TO_STRING: {
        XrXirValue scalar = {(uint32_t) xr_xir_operand_type(run->function, op->args[0]), 0,
            xr_xir_scalar_load(run->frame, run->layout->offsets[op->args[0]])};
        status = xr_xir_instance_scalar_text(run->view, &scalar, &value); break;
    }
    case XR_XIR_STRING_COPY_BYTES: {
        XrXirValue receiver=vm_value_operand(run,op->args[0]);
        status=xr_xir_instance_string_copy_bytes(run->view,op->type,&receiver,&value);break;
    }
    case XR_XIR_STRING_REPLACE: {
        const uint32_t *ids=run->function->operands+op->args[0];
        XrXirValue receiver=vm_value_operand(run,ids[0]),search=vm_value_operand(run,ids[1]);
        XrXirValue replacement=vm_value_operand(run,ids[2]);
        status=xr_xir_instance_string_replace(run->view,&receiver,&search,&replacement,op->immediate!=0,&value);break;
    }
    case XR_XIR_SLOT_LOAD:
        status = xr_xir_instance_slot_read(run->view, (uint32_t) op->immediate, &value); break;
    case XR_XIR_SLOT_INIT:
    case XR_XIR_SLOT_STORE:
        value = xr_xir_slot_is_unit(run->module->declarations->slots,run->module->declarations->slot_count,op) ?
            (XrXirValue){0} : vm_value_operand(run,op->args[0]);
        status = xr_xir_instance_slot_write(run->view, (uint32_t) op->immediate, &value, op->op == XR_XIR_SLOT_INIT);
        break;
    default: return XR_XIR_RUN_BAD_ARTIFACT;
    }
    if (status==XR_XIR_CALL_BOUNDS && xr_xir_fault_bounds_valid(fault)) {
        *action=(XrXirAction){XR_XIR_ACTION_FAULT,0,NULL,0,{XR_XIR_I64,0,status},{fault,{0}},0};
        return XR_XIR_RUN_OK;
    }
    if (status != XR_XIR_CALL_READY) return status == XR_XIR_CALL_OOM ? XR_XIR_RUN_OUT_OF_MEMORY :
        status == XR_XIR_CALL_LIMIT ? XR_XIR_RUN_FRAME_LIMIT : XR_XIR_RUN_BAD_ARTIFACT;
    if (xr_xir_type_is_owned(run->module->types, op->type)) xr_xir_owned_slot_move(run->frame, destination, &value);
    else if (op->type != XR_XIR_UNIT) xr_xir_scalar_store(run->frame, destination, value.payload);
    return XR_XIR_RUN_OK;
}

static XrXirValue vm_value_operand(const ScalarRun *run, uint32_t id) {
    XrXirValue value={0};
    XrXirValueAdmission *admission=xr_xir_call_admission(run->view);
    if (admission) (void)xr_xir_value_from_payload(admission->arena,xr_xir_operand_type(run->function,id),
        xr_xir_scalar_load(run->frame,run->layout->offsets[id]),&value);
    return value;
}
static XrXirValueReceiver vm_value_receiver(const ScalarRun *run, uint32_t id) {
    XrXirValueReceiver receiver = {0};
    receiver.type = xr_xir_operand_type(run->function, id);
    XrXirPlaceKind kind = xr_xir_place_kind(run->function, id);
    if (kind == XR_XIR_PLACE_NONE) receiver.value = vm_value_operand(run, id);
    else if (kind == XR_XIR_PLACE_LOCAL) {
        receiver.kind = XR_XIR_ROOT_LOCAL;
        receiver.local_payload = (unsigned char *) run->frame + run->layout->offsets[id];
    } else {
        const XrXirInstruction *place = &run->function->instructions[id - run->function->parameter_count];
        if (kind == XR_XIR_PLACE_CELL) {
            receiver.kind = XR_XIR_ROOT_CELL;
            receiver.value = vm_value_operand(run, place->args[0]);
        } else if (kind == XR_XIR_PLACE_OBJECT) {
            receiver.kind = XR_XIR_ROOT_OBJECT;
            receiver.value = vm_value_operand(run, place->args[0]);
        } else {
            receiver.kind = XR_XIR_ROOT_SLOT;
            receiver.slot = (uint32_t) place->immediate;
        }
    }
    return receiver;
}
#include "xxir_vm_atomic.inc.c"
#include "xxir_vm_channel.inc.c"
#include "xxir_vm_path.inc.c"
#include "xxir_vm_hash.inc.c"
#include "xxir_vm_view.inc.c"
#include "xxir_vm_view_moves.inc.c"
static XrXirRunStatus vm_array_capacity_step(ScalarRun *run, VmState *state,
    const XrXirInstruction *op, uint32_t destination, XrXirAction *action) {
    if (!run->view) return XR_XIR_RUN_BAD_ARTIFACT;
    XrXirValue output = {0};
    XrXirFaultDetail fault = {0};
    XrXirCallStatus status;
    if (op->op == XR_XIR_ARRAY_CAPACITY) {
        XrXirValueReceiver receiver = {0};
        XrXirValuePath path = {0};
        status = vm_path_receiver(run, state, op, &receiver, &path);
        if (status == XR_XIR_CALL_READY)
            status = path.count ? xr_xir_instance_path_capacity_at(run->view,
                (uint32_t)(op-run->function->instructions), &receiver, &path, &output, &fault) :
                xr_xir_instance_array_capacity(run->view, &receiver, &output);
    } else if (op->op == XR_XIR_ARRAY_WITH_CAPACITY) {
        status = xr_xir_instance_array_with_capacity(run->view, op->type,
            vm_value_operand(run, op->args[0]).payload, &output);
    } else {
        XrXirValue receiver = vm_value_operand(run, op->args[0]);
        status = xr_xir_instance_array_reserve(run->view, &receiver,
            vm_value_operand(run, op->args[1]).payload, &output);
    }
    if (status != XR_XIR_CALL_READY) {
        *action = status == XR_XIR_CALL_NUMERIC_RANGE ?
            xr_xir_call_fault(XR_XIR_RUN_NUMERIC_RANGE) :
            (XrXirAction){XR_XIR_ACTION_FAULT, 0, NULL, 0,
                {XR_XIR_I64, 0, status}, {fault, {0}}, 0};
        return XR_XIR_RUN_OK;
    }
    if (xr_xir_type_is_owned(run->module->types, op->type))
        xr_xir_owned_slot_move(run->frame, destination, &output);
    else xr_xir_scalar_store(run->frame, destination, output.payload);
    return XR_XIR_RUN_OK;
}
static XrXirRunStatus vm_array_step(ScalarRun *run, VmState *state,
    const XrXirInstruction *op, uint32_t destination, XrXirAction *action) {
    if (!run->view) return XR_XIR_RUN_BAD_ARTIFACT;
    XrXirValue output = {0};
    XrXirFaultDetail fault = {0};
    XrXirCallStatus status;
    if (op->op == XR_XIR_ARRAY_NEW) {
        for (uint32_t i = 0; i < op->args[1]; ++i)
            state->arguments[i] = vm_value_operand(run, run->function->operands[op->args[0] + i]);
        status = xr_xir_instance_array_new(run->view, op->type, state->arguments, op->args[1], &output);
    } else {
        bool setting = op->op == XR_XIR_ARRAY_SET;
        const uint32_t *args = setting ? &run->function->operands[op->args[0]] : op->args;
        XrXirValueReceiver receiver = vm_value_receiver(run, args[0]);
        int64_t index = setting || op->op == XR_XIR_ARRAY_GET ? vm_value_operand(run, args[1]).payload : 0;
        if (setting || op->op == XR_XIR_ARRAY_PUSH) {
            XrXirValue element = vm_value_operand(run, args[setting ? 2 : 1]);
            status = xr_xir_instance_array_write(run->view, &receiver, index, &element, !setting, &fault);
        } else status = xr_xir_instance_array_read(run->view, &receiver, index,
            op->op == XR_XIR_ARRAY_LEN, &output, &fault);
    }
    if (status != XR_XIR_CALL_READY) {
        *action = (XrXirAction) {XR_XIR_ACTION_FAULT, 0, NULL, 0, {XR_XIR_I64, 0, status}, {fault, {0}}, 0};
        return XR_XIR_RUN_OK;
    }
    if (xr_xir_type_is_owned(run->module->types, op->type)) xr_xir_owned_slot_move(run->frame, destination, &output);
    else if (op->type != XR_XIR_UNIT) xr_xir_scalar_store(run->frame, destination, output.payload);
    return XR_XIR_RUN_OK;
}

static XrXirRunStatus vm_class_step(ScalarRun *run, VmState *state,
    const XrXirInstruction *op, uint32_t destination) {
    XrXirValue output={0};XrXirValueStatus status;
    XrXirValueAdmission *admission=xr_xir_call_admission(run->view);
    if (!admission) return XR_XIR_RUN_BAD_ARTIFACT;
    if (op->op == XR_XIR_CLASS_NEW) {
        for(uint32_t i=0;i<op->args[1];++i)
            state->arguments[i]=vm_value_operand(run,run->function->operands[op->args[0]+i]);
        status=xr_xir_class_new(op->type,op->args[1]?state->arguments:NULL,op->args[1],admission,&output);
    } else {
        XrXirValue receiver=vm_value_operand(run,op->args[0]);
        if(op->op == XR_XIR_CLASS_SET) {
            XrXirValue value=vm_value_operand(run,op->args[1]);
            return value_run_status(xr_xir_class_set(&receiver,(uint32_t)op->immediate,&value,admission));
        }
        status=xr_xir_value_admit(&receiver,xr_xir_operand_type(run->function,op->args[0]),admission);
        if(status == XR_XIR_VALUE_OK) status=xr_xir_class_get(&receiver,(uint32_t)op->immediate,admission,&output);
    }
    if(status != XR_XIR_VALUE_OK) return value_run_status(status);
    if(xr_xir_type_is_owned(run->module->types,op->type)) xr_xir_owned_slot_move(run->frame,destination,&output);
    else xr_xir_scalar_store(run->frame,destination,output.payload);
    return XR_XIR_RUN_OK;
}

static XrXirRunStatus vm_tuple_step(ScalarRun *run,VmState *state,
    const XrXirInstruction *op,uint32_t destination) {
    if (op->op==XR_XIR_TUPLE_NEW && run->view && xr_xir_compile_type_arena_borrowed(run->view->arena,op->type))
        return vm_view_aggregate(run,state,op,destination);
    XrXirValue output={0};XrXirValueStatus status;
    if (op->op==XR_XIR_TUPLE_NEW) {
        const XrXirTypeNode *tuple=xr_xir_tuple_signature(run->module->types,op->type);
        if (!tuple || tuple->parameter_count>run->layout->outgoing_count) return XR_XIR_RUN_BAD_ARTIFACT;
        uint32_t payload=0;
        for (uint32_t i=0;i<tuple->parameter_count;++i)
            state->arguments[i]=tuple->parameters[i].type==XR_XIR_UNIT ? (XrXirValue){0} :
                vm_value_operand(run,run->function->operands[op->args[0]+payload++]);
        status=xr_xir_tuple_new(op->type,state->arguments,tuple->parameter_count,
            xr_xir_call_admission(run->view),&output);
    } else {
        XrXirValue receiver=vm_value_operand(run,op->args[0]);
        status=xr_xir_tuple_get(&receiver,(uint32_t)op->immediate,&output);
    }
    if (status!=XR_XIR_VALUE_OK) return value_run_status(status);
    if (xr_xir_type_is_owned(run->module->types,op->type)) xr_xir_owned_slot_move(run->frame,destination,&output);
    else if (op->type!=XR_XIR_UNIT) xr_xir_scalar_store(run->frame,destination,output.payload);
    return XR_XIR_RUN_OK;
}
static XrXirRunStatus vm_nominal_step(ScalarRun *run, VmState *state,
                                      const XrXirInstruction *op, uint32_t destination) {
    XrXirValueAdmission *admission = xr_xir_call_admission(run->view);
    if (!admission) return XR_XIR_RUN_BAD_ARTIFACT;
    if (op->op == XR_XIR_STRUCT_SET) {
        XrXirValueReceiver receiver = vm_value_receiver(run, op->args[0]);
        XrXirValue value = vm_value_operand(run, op->args[1]);
        XrXirCallStatus status = xr_xir_instance_struct_write(run->view, &receiver, (uint32_t) op->immediate, &value);
        return status == XR_XIR_CALL_READY ? XR_XIR_RUN_OK : status == XR_XIR_CALL_OOM ? XR_XIR_RUN_OUT_OF_MEMORY :
            status == XR_XIR_CALL_LIMIT ? XR_XIR_RUN_FRAME_LIMIT : XR_XIR_RUN_BAD_ARTIFACT;
    }
    XrXirValue output = {0}; XrXirValueStatus status;
    if (op->op == XR_XIR_STRUCT_NEW || op->op == XR_XIR_ENUM_NEW) {
        for (uint32_t i = 0; i < op->args[1]; ++i)
            state->arguments[i] = vm_value_operand(run, run->function->operands[op->args[0] + i]);
        bool unit_payload=op->op==XR_XIR_ENUM_NEW && !op->immediate && xr_xir_nominal_native_recv(run->module->types,op->type,XR_XIR_UNIT);
        XrXirValue unit={0};
        status = op->op == XR_XIR_ENUM_NEW ?
            xr_xir_enum_new(op->type, (uint32_t) op->immediate, unit_payload ? &unit : op->args[1] ? state->arguments : NULL,
                unit_payload ? 1u : op->args[1], admission, &output) :
            xr_xir_struct_new(op->type, op->args[1] ? state->arguments : NULL, op->args[1], admission, &output);
    } else {
        XrXirValue receiver = vm_value_operand(run, op->args[0]);
        if (op->op == XR_XIR_ERROR_ERASE) status = xr_xir_error_erase(&receiver, admission, &output);
        else if (op->op == XR_XIR_ERROR_NARROW) status = xr_xir_error_narrow(&receiver, op->type, admission, &output);
        else if (op->op == XR_XIR_ERROR_IS) {
            bool matches = false; status = xr_xir_error_is(&receiver, (XrXirType) op->immediate, admission, &matches);
            output = (XrXirValue) {XR_XIR_BOOL, 0, matches};
        } else if (op->op == XR_XIR_ENUM_TAG) {
            uint32_t variant = 0; status = xr_xir_enum_variant(&receiver, &variant);
            output = (XrXirValue) {XR_XIR_I64, 0, variant};
        } else status = op->op == XR_XIR_ENUM_GET ?
            xr_xir_enum_get(&receiver, (uint32_t) op->immediate, op->args[1], admission, &output) :
            xr_xir_struct_get(&receiver, (uint32_t) op->immediate, admission, &output);
    }
    if (status != XR_XIR_VALUE_OK) return value_run_status(status);
    if (xr_xir_type_is_owned(run->module->types, op->type)) xr_xir_owned_slot_move(run->frame, destination, &output);
    else if (op->type!=XR_XIR_UNIT) xr_xir_scalar_store(run->frame, destination, output.payload);
    return XR_XIR_RUN_OK;
}

static uint32_t vm_block(const XrXirFunction *function, uint32_t instruction) {
    uint32_t low = 0, high = function->block_count;
    while (low + 1 < high) {
        uint32_t middle = low + (high - low) / 2;
        if (function->blocks[middle].first <= instruction) low = middle;
        else high = middle;
    }
    return low;
}
static XrXirAction vm_panic_land(ScalarRun *run, VmState *state, uint32_t handler, XrXirAction action) {
    uint32_t target = run->function->blocks[handler].frontier;
    if (xr_xir_call_panic_action(&action) && state->frontier != target) {
        state->exit_target = target; state->landing = handler; state->leaving = true;
        action.kind = XR_XIR_ACTION_LEAVE; action.flags = XR_XIR_ACTION_LEAVE_PANIC;
        return action;
    }
    uint32_t first = run->function->blocks[handler].first;
    uint32_t destination = run->function->instructions[first].type == XR_XIR_UNIT ? UINT32_MAX :
        run->layout->offsets[run->function->parameter_count + first];
    return xr_xir_instance_panic_land(run->view, run->frame, action, destination, first + 1, &state->instruction);
}
static XrXirRunStatus scalar_edge(ScalarRun *run,uint32_t instruction,uint32_t target) {
    if (vm_view_phi_target(run,target)) return vm_view_phi_edge(run,vm_block(run->function,instruction),target);
    XirScalarCoreRun core={run->module,run->function,run->layout,run->frame,
        run->view ? run->view->arena : NULL,run->compile_resources};
    return xir_scalar_core_edge(&core,instruction,target);
}
#include "xxir_vm_cleanup.inc.c"

static XrXirRunStatus vm_call_step(ScalarRun *run, VmState *state, const XrXirInstruction *op,
    XrXirAction *action, uint32_t destination) {
    uint32_t callee = (uint32_t) op->immediate;
    XrXirValue value = {0};
    for (uint32_t i = 0; i < op->args[1]; ++i) {
        uint32_t id = run->function->operands[op->args[0] + i];
        state->arguments[i] = vm_value_operand(run,id);
    }
    uint32_t at = (uint32_t) (op - run->function->instructions);
    if (op->op == XR_XIR_CALL_INDIRECT || op->op == XR_XIR_INVOKE_INDIRECT) {
        value = vm_value_operand(run,callee);
        XrXirCallStatus status = xr_xir_instance_resolve_function_at(run->view,at,&value,
            op->args[1]?state->arguments:NULL,op->args[1],&callee);
        if (status != XR_XIR_CALL_READY) {
            *action = (XrXirAction) {XR_XIR_ACTION_FAULT, 0, NULL, 0, {XR_XIR_I64, 0, status}, {0}, 0};
            return XR_XIR_RUN_OK;
        }
    }
    if (op->op == XR_XIR_CALL || op->op == XR_XIR_INVOKE) {
        XrXirCallStatus status=xr_xir_instance_call_at(run->view,at,callee,
            op->args[1]?state->arguments:NULL,op->args[1]);
        if (status!=XR_XIR_CALL_READY) {
            *action=(XrXirAction){XR_XIR_ACTION_FAULT,0,NULL,0,{XR_XIR_I64,0,status},{0},0};
            return XR_XIR_RUN_OK;
        }
    }
    state->invoke = op->op == XR_XIR_INVOKE || op->op == XR_XIR_INVOKE_INDIRECT ? at + 1 : 0;
    state->panic = run->function->blocks[vm_block(run->function, at)].panic;
    state->waiting = true; state->destination = destination; state->expected = op->type;
    *action = (XrXirAction) {XR_XIR_ACTION_CALL, callee, state->arguments, op->args[1], value, {0},
        state->panic ? XR_XIR_ACTION_PROTECTED : 0};
    return XR_XIR_RUN_OK;
}

static XrXirRunStatus vm_go_step(ScalarRun *run, VmState *state, const XrXirInstruction *op,
    XrXirAction *action, uint32_t destination) {
    const XrXirTypeNode *task_type = xr_xir_type_node(run->module->types, op->type);
    if (!run->view || !task_type || task_type->kind != XR_XIR_TYPE_TASK ||
        op->args[1] > run->layout->outgoing_count) return XR_XIR_RUN_BAD_ARTIFACT;
    for (uint32_t i = 0; i < op->args[1]; ++i)
        state->arguments[i] = vm_value_operand(run, run->function->operands[op->args[0] + i]);
    XrXirValue task = {0};
    XrXirCallStatus status = xr_xir_task_go(run->view, op->type, (uint32_t)op->immediate,
        op->args[1] ? state->arguments : NULL, op->args[1], &task);
    if (status != XR_XIR_CALL_READY) {
        *action = (XrXirAction){XR_XIR_ACTION_FAULT, 0, NULL, 0,
            {XR_XIR_I64, 0, status}, {0}, 0};
        return XR_XIR_RUN_OK;
    }
    xr_xir_owned_slot_move(run->frame, destination, &task);
    ++state->instruction;
    return XR_XIR_RUN_OK;
}

static XrXirRunStatus vm_generator_new(ScalarRun *run,VmState *state,const XrXirInstruction *op,
    XrXirAction *action,uint32_t destination) {
    if(!run->view || op->args[1]>run->layout->outgoing_count) return XR_XIR_RUN_BAD_ARTIFACT;
    for(uint32_t i=0;i<op->args[1];++i)
        state->arguments[i]=vm_value_operand(run,run->function->operands[op->args[0]+i]);
    XrXirValue iterator={0};
    XrXirCallStatus status=xr_xir_instance_generator_at(run->view,
        (uint32_t)(op-run->function->instructions),op->args[1] ? state->arguments : NULL,op->args[1],&iterator);
    if(status!=XR_XIR_CALL_READY) {
        *action=(XrXirAction){XR_XIR_ACTION_FAULT,0,NULL,0,{XR_XIR_I64,0,status},{0},0};
        return XR_XIR_RUN_OK;
    }
    xr_xir_owned_slot_move(run->frame,destination,&iterator);++state->instruction;return XR_XIR_RUN_OK;
}

static XrXirRunStatus vm_await_step(ScalarRun *run, VmState *state, const XrXirInstruction *op,
    XrXirAction *action) {
    if (!run->view) return XR_XIR_RUN_BAD_ARTIFACT;
    XrXirValue task = vm_value_operand(run, op->args[0]);
    const XrXirTypeNode *task_type = xr_xir_type_node(run->module->types, (XrXirType)task.type);
    if (!task_type || task_type->kind != (uint32_t)(op->op==XR_XIR_TASK_AWAIT ? XR_XIR_TYPE_TASK : XR_XIR_TYPE_ITERATOR))
        return XR_XIR_RUN_BAD_ARTIFACT;
    XrXirType element = op->op==XR_XIR_ITERATOR_HAS_NEXT ? XR_XIR_BOOL : op->op==XR_XIR_ITERATOR_CLOSE ? XR_XIR_UNIT : task_type->element;
    bool nth=op->op==XR_XIR_ITERATOR_NTH;
    if(nth) {
        if(!run->layout->outgoing_count) return XR_XIR_RUN_BAD_ARTIFACT;
        state->arguments[0]=vm_value_operand(run,op->args[1]);
    }
    uint32_t at = (uint32_t)(op - run->function->instructions);
    if(op->op!=XR_XIR_TASK_AWAIT && op->op!=XR_XIR_ITERATOR_CLOSE) {
        XrXirCallStatus status=xr_xir_instance_iterator_at(run->view,at);
        if(status!=XR_XIR_CALL_READY) {
            *action=(XrXirAction){XR_XIR_ACTION_FAULT,0,NULL,0,{XR_XIR_I64,0,status},{0},0};
            return XR_XIR_RUN_OK;
        }
    }
    state->invoke = at + 1;
    state->panic = run->function->blocks[vm_block(run->function, at)].panic;
    state->waiting = true;
    state->expected = element;
    ++state->instruction;
    /* The frame owns this handle until the driver prepares its wait lease. */
    *action = (XrXirAction){op->op==XR_XIR_TASK_AWAIT ? XR_XIR_ACTION_AWAIT_TASK : XR_XIR_ACTION_AWAIT_ITERATOR,
        op->op==XR_XIR_TASK_AWAIT ? 0 : op->op==XR_XIR_ITERATOR_HAS_NEXT ? XR_XIR_WAIT_ITERATOR_HAS_NEXT :
        nth ? XR_XIR_WAIT_ITERATOR_NTH : op->op==XR_XIR_ITERATOR_CLOSE ? XR_XIR_WAIT_ITERATOR_CLOSE : XR_XIR_WAIT_ITERATOR_NEXT,
        nth ? state->arguments : NULL, nth ? 1u : 0u, task, {0},
        state->panic ? XR_XIR_ACTION_PROTECTED : 0};
    return XR_XIR_RUN_OK;
}

static XrXirRunStatus floating_step(ScalarRun *run, const XrXirInstruction *op, int64_t *value) {
    XrXirType type = xr_xir_operand_type(run->function, op->args[0]);
    int64_t left = xr_xir_scalar_load(run->frame, run->layout->offsets[op->args[0]]);
    if (op->op == XR_XIR_NEG_FLOAT) return xr_xir_float_negative(type, left, value);
    if (op->op >= XR_XIR_ADD_FLOAT && op->op <= XR_XIR_DIV_FLOAT)
        return xr_xir_float_binary(type, (XrXirFloatOperation) (op->op - XR_XIR_ADD_FLOAT), left,
            xr_xir_scalar_load(run->frame, run->layout->offsets[op->args[1]]), value);
    return xr_xir_float_relation(type, (XrXirFloatRelation) (op->op - XR_XIR_EQ_FLOAT), left,
        xr_xir_scalar_load(run->frame, run->layout->offsets[op->args[1]]), value);
}
static XrXirRunStatus vm_io_step(ScalarRun *run, VmState *state, XrXirAction *action) {
    uint32_t instruction = state->instruction;
    const XrXirInstruction *op = &run->function->instructions[instruction];
    uint32_t result_id = run->function->parameter_count + instruction;
    uint32_t next = instruction + 1;
    switch (op->op) {
    case XR_XIR_STRING_INDEX_OF: case XR_XIR_STRING_LAST_INDEX_OF: {
        const uint32_t *args = op->op == XR_XIR_STRING_INDEX_OF ?
            &run->function->operands[op->args[0]] : op->args;
        XrXirValue left = vm_value_operand(run, args[0]), right = vm_value_operand(run, args[1]);
        int64_t result = 0, start = op->op == XR_XIR_STRING_INDEX_OF ? vm_value_operand(run, args[2]).payload : 0;
        XrXirValueStatus status = op->op == XR_XIR_STRING_INDEX_OF ?
            xr_xir_string_index_of(&left, &right, start, &result) : xr_xir_string_last_index_of(&left, &right, &result);
        state->instruction = next;
        if (status == XR_XIR_VALUE_BOUNDS) {
            int64_t length = 0;
            if (xr_xir_string_length(&left, &length) != XR_XIR_VALUE_OK) return XR_XIR_RUN_BAD_ARTIFACT;
            *action = xr_xir_call_bounds(start, length);
            return XR_XIR_RUN_OK;
        }
        if (status == XR_XIR_VALUE_OK) xr_xir_scalar_store(run->frame, run->layout->offsets[result_id], result);
        return value_run_status(status);
    }
    case XR_XIR_STRING_CONTAINS: case XR_XIR_STRING_STARTS_WITH: case XR_XIR_STRING_ENDS_WITH: {
        XrXirValue left = {XR_XIR_STRING, 0, xr_xir_scalar_load(run->frame, run->layout->offsets[op->args[0]])};
        XrXirValue right = {XR_XIR_STRING, 0, xr_xir_scalar_load(run->frame, run->layout->offsets[op->args[1]])};
        bool result = false;
        bool valid = op->op == XR_XIR_STRING_CONTAINS ? xr_xir_string_contains(&left, &right, &result) :
            op->op == XR_XIR_STRING_STARTS_WITH ? xr_xir_string_starts_with(&left, &right, &result) :
            xr_xir_string_ends_with(&left, &right, &result);
        if (valid) xr_xir_scalar_store(run->frame, run->layout->offsets[result_id], result);
        state->instruction = next;
        return value_run_status(valid ? XR_XIR_VALUE_OK : XR_XIR_VALUE_BAD_ARGUMENT);
    }
    case XR_XIR_LT_STRING: case XR_XIR_LE_STRING: case XR_XIR_GT_STRING: case XR_XIR_GE_STRING: {
        XrXirValue left={XR_XIR_STRING,0,xr_xir_scalar_load(run->frame,run->layout->offsets[op->args[0]])};
        XrXirValue right={XR_XIR_STRING,0,xr_xir_scalar_load(run->frame,run->layout->offsets[op->args[1]])};
        int order=0;
        if (!xr_xir_string_compare(&left,&right,&order)) return XR_XIR_RUN_BAD_ARTIFACT;
        bool result=op->op==XR_XIR_LT_STRING?order<0:op->op==XR_XIR_LE_STRING?order<=0:
            op->op==XR_XIR_GT_STRING?order>0:order>=0;
        xr_xir_scalar_store(run->frame,run->layout->offsets[result_id],result);
        state->instruction=next;return XR_XIR_RUN_OK;
    }
    case XR_XIR_STRING_LEN: case XR_XIR_EQ_STRING: case XR_XIR_NE_STRING: {
        XrXirValue left = {XR_XIR_STRING, 0, xr_xir_scalar_load(run->frame, run->layout->offsets[op->args[0]])};
        int64_t result = 0;
        XrXirValueStatus status;
        if (op->op == XR_XIR_STRING_LEN) status = xr_xir_string_length(&left, &result);
        else {
            XrXirValue right = {XR_XIR_STRING, 0, xr_xir_scalar_load(run->frame, run->layout->offsets[op->args[1]])};
            bool equal = false;
            status = xr_xir_string_equal(&left, &right, &equal) ? XR_XIR_VALUE_OK : XR_XIR_VALUE_BAD_ARGUMENT;
            result = op->op == XR_XIR_EQ_STRING ? equal : !equal;
        }
        if (status == XR_XIR_VALUE_OK) xr_xir_scalar_store(run->frame, run->layout->offsets[result_id], result);
        state->instruction = next;
        return value_run_status(status);
    }
    case XR_XIR_OUTPUT:
    case XR_XIR_WRITE_STREAM:
    case XR_XIR_PRINT: {
        uint32_t count = op->op == XR_XIR_PRINT ? op->args[1] : 1;
        for (uint32_t p = 0; p < count; ++p) {
            uint32_t id = op->op == XR_XIR_PRINT ? run->function->operands[op->args[0] + p] : op->args[p];
            XrXirType type = id < run->function->parameter_count ? run->function->parameters[id] :
                run->function->instructions[id - run->function->parameter_count].type;
            state->arguments[p] = (XrXirValue) {(uint32_t) type, 0,
                xr_xir_scalar_load(run->frame, run->layout->offsets[id])};
        }
        if (op->op == XR_XIR_WRITE_STREAM) {
            state->waiting = true; state->destination = run->layout->offsets[result_id]; state->expected = XR_XIR_BOOL;
        }
        *action = (XrXirAction) {op->op == XR_XIR_WRITE_STREAM ? XR_XIR_ACTION_WRITE_STREAM : XR_XIR_ACTION_OUTPUT,
            op->op == XR_XIR_PRINT ? XR_XIR_OUTPUT_LINE :
            (uint32_t) op->immediate, state->arguments, count, {0}, {0}, 0};
        state->instruction = next;
        return XR_XIR_RUN_OK;
    }
    default: return XR_XIR_RUN_BAD_ARTIFACT;
    }
}

static XrXirRunStatus vm_step_dispatch(ScalarRun *run,VmState *state,const XrXirInstruction *op,
    XrXirAction *action,bool *handled) {
    uint32_t instruction=state->instruction,result_id=run->function->parameter_count+instruction;
    uint32_t next=instruction+1;int64_t value=0;
    *handled=true;
    if(op->op==XR_XIR_SLOT_GROUP_INIT) {
        if(!run->view)return XR_XIR_RUN_BAD_ARTIFACT;
        for(uint32_t p=0;p<op->args[1];++p)
            state->arguments[p]=vm_value_operand(run,run->function->operands[op->args[0]+p]);
        uint64_t packed=(uint64_t)op->immediate;
        XrXirCallStatus status=xr_xir_instance_slot_group_init(run->view,(uint32_t)(packed>>32),(uint32_t)packed,
            op->args[1] ? state->arguments : NULL,op->args[1]);
        state->instruction=next;
        if(status!=XR_XIR_CALL_READY)*action=(XrXirAction){XR_XIR_ACTION_FAULT,0,NULL,0,{XR_XIR_I64,0,status},{0},0};
        return XR_XIR_RUN_OK;
    }
    if (op->op == XR_XIR_STRING_INDEX_OF || op->op == XR_XIR_STRING_LAST_INDEX_OF ||
        (op->op >= XR_XIR_STRING_CONTAINS && op->op <= XR_XIR_STRING_ENDS_WITH) ||
        op->op == XR_XIR_STRING_LEN || op->op == XR_XIR_EQ_STRING || op->op == XR_XIR_NE_STRING ||
        (op->op >= XR_XIR_LT_STRING && op->op <= XR_XIR_GE_STRING) ||
        op->op == XR_XIR_OUTPUT || op->op == XR_XIR_WRITE_STREAM || op->op == XR_XIR_PRINT)
        return vm_io_step(run, state, action);
    if (run->view && (op->op==XR_XIR_JUMP || op->op==XR_XIR_BRANCH)) {
        uint32_t edge=op->op==XR_XIR_BRANCH && !xr_xir_scalar_load(run->frame,run->layout->offsets[op->args[0]])?1u:0u;
        XrXirRunStatus status=scalar_edge(run,instruction,op->targets[edge]);
        if (status==XR_XIR_RUN_OK) state->instruction=run->function->blocks[op->targets[edge]].first;
        return status;
    }
    if (xir_scalar_core_opcode(op->op)) {
        XirScalarCoreRun core={run->module,run->function,run->layout,run->frame,
            run->view ? run->view->arena : NULL,run->compile_resources};
        XirScalarCoreAction result;
        XrXirRunStatus status=xir_scalar_core_step(&core,&state->instruction,&result);
        if (status==XR_XIR_RUN_OK && result.kind==XIR_SCALAR_RETURN) {
            if (run->view && xr_xir_compile_type_arena_borrowed(run->view->arena,run->function->result))
                *action=vm_view_return(run,state,op);
            else {action->kind=XR_XIR_ACTION_RETURN;action->value=result.value;}
        }
        return status;
    }
    if (op->op == XR_XIR_CLEANUP_REGISTER || op->op == XR_XIR_CLEANUP_LEAVE || op->op == XR_XIR_CLEANUP_ERROR)
        return vm_cleanup_step(run, state, op, action);
    if (op->op == XR_XIR_CELL_PLACE || op->op == XR_XIR_SLOT_PLACE || op->op == XR_XIR_OBJECT_PLACE ||
        op->op == XR_XIR_FIELD_PLACE || op->op == XR_XIR_INDEX_PLACE) {
        state->instruction = next;
        return XR_XIR_RUN_OK;
    }
    if (op->op == XR_XIR_ARRAY_CAPACITY || op->op == XR_XIR_ARRAY_WITH_CAPACITY ||
        op->op == XR_XIR_ARRAY_RESERVE) {
        state->instruction = next;
        return vm_array_capacity_step(run, state, op, run->layout->offsets[result_id], action);
    }
    if (op->op==XR_XIR_VIEW_CLEANUP) {
        state->instruction=next;return vm_view_cleanup(run,state,op,action);
    }
    if (op->op>=XR_XIR_VIEW_NEW && op->op<=XR_XIR_VIEW_END) {
        state->instruction=next;
        return vm_view_step(run,state,op,run->layout->offsets[result_id],action);
    }
    if (op->op >= XR_XIR_MAP_NEW && op->op <= XR_XIR_MAP_VALUE_AT) {
        state->instruction = next;
        return vm_hash_step(run, state, op, run->layout->offsets[result_id], action);
    }
    if (xr_xir_op_uses_value_path(run->function, op)) {
        state->instruction = next;
        return vm_path_step(run, state, op, run->layout->offsets[result_id], action);
    }
    if (op->op == XR_XIR_CLASS_NEW || op->op == XR_XIR_CLASS_GET || op->op == XR_XIR_CLASS_SET) {
        state->instruction = next;
        return vm_class_step(run, state, op, run->layout->offsets[result_id]);
    }
    if (xr_xir_op_is_channel(op->op))
        return vm_channel_step(run,state,op,run->layout->offsets[result_id],action);
    if (xr_xir_op_is_atomic(op->op))
        return vm_atomic_step(run, state, op, run->layout->offsets[result_id], action);
    if (op->op==XR_XIR_TUPLE_NEW || op->op==XR_XIR_TUPLE_FIELD) {
        state->instruction=next;
        return vm_tuple_step(run,state,op,run->layout->offsets[result_id]);
    }
    if ((op->op >= XR_XIR_STRUCT_NEW && op->op <= XR_XIR_STRUCT_SET) ||
        (op->op >= XR_XIR_ENUM_NEW && op->op <= XR_XIR_ENUM_GET) || op->op == XR_XIR_ERROR_ERASE ||
        op->op == XR_XIR_ERROR_IS || op->op == XR_XIR_ERROR_NARROW) {
        state->instruction = next;
        return vm_nominal_step(run, state, op, run->layout->offsets[result_id]);
    }
    if (op->op >= XR_XIR_ARRAY_NEW && op->op <= XR_XIR_ARRAY_LEN) {
        state->instruction = next;
        return vm_array_step(run, state, op, run->layout->offsets[result_id], action);
    }
    if ((op->op >= XR_XIR_CONST_STRING && op->op <= XR_XIR_SLOT_STORE) || op->op == XR_XIR_FUNCTION_REF || op->op == XR_XIR_FUNCTION_WEAKEN ||
        op->op == XR_XIR_TO_STRING || op->op==XR_XIR_STRING_COPY_BYTES || op->op==XR_XIR_STRING_REPLACE ||
        (op->op >= XR_XIR_CELL_NEW && op->op <= XR_XIR_CELL_WRITE) || op->op == XR_XIR_CELL_LOCAL_WRITE ||
        op->op == XR_XIR_CELL_STRING_APPEND) {
        state->instruction = next;
        return instance_step(run, state, op, run->layout->offsets[result_id], action);
    }
    if ((op->op >= XR_XIR_NEG_FLOAT && op->op <= XR_XIR_GE_FLOAT) ||
        (op->op >= XR_XIR_ADD_FLOAT && op->op <= XR_XIR_DIV_FLOAT)) {
        XrXirRunStatus status = floating_step(run, op, &value);
        if (status != XR_XIR_RUN_OK) return status;
        xr_xir_scalar_store(run->frame, run->layout->offsets[result_id], value);
        state->instruction = next; return XR_XIR_RUN_OK;
    }
    *handled=false;return XR_XIR_RUN_OK;
}
static XrXirRunStatus scalar_step(ScalarRun *run, VmState *state, XrXirAction *action) {
    uint32_t instruction = state->instruction;
    const XrXirInstruction *op = &run->function->instructions[instruction];
    uint32_t result_id = run->function->parameter_count + instruction;
    uint32_t next = instruction + 1;
    int64_t value = 0;
    *action = (XrXirAction) {XR_XIR_ACTION_CONTINUE, 0, NULL, 0, {0, 0, 0}, {0}, 0};
    bool handled=false;
    XrXirRunStatus dispatched=vm_step_dispatch(run,state,op,action,&handled);
    if (handled) return dispatched;
    switch (op->op) {
    case XR_XIR_OWNED_LOCAL_NEW:
    case XR_XIR_OWNED_LOCAL_READ:
    case XR_XIR_OWNED_RETAIN:
    case XR_XIR_CONCAT_STRING: {
        int64_t left = xr_xir_scalar_load(run->frame, run->layout->offsets[op->args[0]]);
        XrXirValueStatus status = op->op != XR_XIR_CONCAT_STRING ?
            xr_xir_owned_slot_copy(run->frame, run->layout->offsets[result_id], run->view ? run->view->arena : NULL, op->type, left) :
            xr_xir_string_slot_concat(run->frame, run->layout->offsets[result_id], xr_xir_call_admission(run->view), left,
                xr_xir_scalar_load(run->frame, run->layout->offsets[op->args[1]]));
        state->instruction = next;
        return value_run_status(status);
    }
    case XR_XIR_CALL: case XR_XIR_CALL_INDIRECT: case XR_XIR_INVOKE: case XR_XIR_INVOKE_INDIRECT:
        state->instruction = next;
        return vm_call_step(run, state, op, action, run->layout->offsets[result_id]);
    case XR_XIR_GENERATOR_NEW:
        return vm_generator_new(run,state,op,action,run->layout->offsets[result_id]);
    case XR_XIR_GO:
        return vm_go_step(run, state, op, action, run->layout->offsets[result_id]);
    case XR_XIR_TASK_AWAIT: case XR_XIR_ITERATOR_HAS_NEXT: case XR_XIR_ITERATOR_NEXT: case XR_XIR_ITERATOR_NTH: case XR_XIR_ITERATOR_CLOSE:
        return vm_await_step(run, state, op, action);
    case XR_XIR_PANIC_CODE: case XR_XIR_PANIC_MESSAGE: {
        if (!run->view) return XR_XIR_RUN_BAD_ARTIFACT;
        XrXirValue info = vm_value_operand(run, op->args[0]);
        state->instruction = next;
        if (op->op == XR_XIR_PANIC_CODE) {
            XrXirFaultDetail detail;
            if (!xr_xir_panic_info_detail(&info, &detail)) return XR_XIR_RUN_BAD_ARTIFACT;
            xr_xir_scalar_store(run->frame, run->layout->offsets[result_id], (int64_t) detail.code);
            return XR_XIR_RUN_OK;
        }
        XrXirValue message = {0};
        XrXirValueStatus status = xr_xir_panic_info_message(&info, &message);
        if (status != XR_XIR_VALUE_OK) return value_run_status(status);
        xr_xir_owned_slot_move(run->frame, run->layout->offsets[result_id], &message);
        return XR_XIR_RUN_OK;
    }
    case XR_XIR_GENERATOR_YIELD: {
        XrXirType element=xr_xir_iterator_element(run->module->types,run->function->result);
        action->kind=XR_XIR_ACTION_GENERATOR_YIELD;
        if (element!=XR_XIR_UNIT) action->value=vm_value_operand(run,op->args[0]);
        break;
    }
    case XR_XIR_SUSPEND:
        action->kind = XR_XIR_ACTION_SUSPEND;
        break;
    case XR_XIR_TIMER_AFTER_MS:
        *action = (XrXirAction) {XR_XIR_ACTION_TIMER, 0, NULL, 0, {XR_XIR_I64, 0,
            xr_xir_scalar_load(run->frame, run->layout->offsets[op->args[0]])}, {0}, 0};
        break;
    case XR_XIR_CLOCK_NANOS:
    case XR_XIR_UTC_OFFSET_AT: {
        if (!run->view) return XR_XIR_RUN_BAD_ARTIFACT;
        XrXirCallStatus status = op->op == XR_XIR_CLOCK_NANOS ?
            xr_xir_instance_clock_ns(run->view, (XrXirClockKind) op->immediate, &value) :
            xr_xir_instance_utc_offset(run->view,
                xr_xir_scalar_load(run->frame, run->layout->offsets[op->args[0]]), &value);
        if (status == XR_XIR_CALL_NUMERIC_RANGE) return XR_XIR_RUN_NUMERIC_RANGE;
        if (status == XR_XIR_CALL_HOST_ERROR) return XR_XIR_RUN_HOST_ERROR;
        if (status != XR_XIR_CALL_READY) return XR_XIR_RUN_BAD_ARTIFACT;
        break;
    }
    case XR_XIR_F64_TRY_PARSE: case XR_XIR_NULLABLE_NONE: case XR_XIR_NULLABLE_SOME: {
        if (!run->view) return XR_XIR_RUN_BAD_ARTIFACT;
        state->instruction = next;
        if (op->op==XR_XIR_NULLABLE_SOME && xr_xir_compile_type_arena_borrowed(run->view->arena,op->type))
            return vm_view_aggregate(run,state,op,run->layout->offsets[result_id]);
        XrXirValue payload = {0}, output = {0};
        if (op->op == XR_XIR_NULLABLE_SOME || op->op == XR_XIR_F64_TRY_PARSE) payload = vm_value_operand(run, op->args[0]);
        XrXirValueStatus status = op->op == XR_XIR_F64_TRY_PARSE ?
            xr_xir_f64_try_parse(&payload,op->type,xr_xir_call_admission(run->view),&output) : xr_xir_nullable_new(op->type,
            op->op == XR_XIR_NULLABLE_SOME ? &payload : NULL, xr_xir_call_admission(run->view), &output);
        if (status != XR_XIR_VALUE_OK) return value_run_status(status);
        xr_xir_owned_slot_move(run->frame, run->layout->offsets[result_id], &output);
        return XR_XIR_RUN_OK;
    }
    case XR_XIR_NULLABLE_IS_SOME: case XR_XIR_NULLABLE_UNWRAP: {
        XrXirValue nullable = vm_value_operand(run, op->args[0]);
        bool some = false; const XrXirValue *payload = NULL;
        if (!xr_xir_nullable_view(&nullable, &some, &payload)) return XR_XIR_RUN_BAD_ARTIFACT;
        if (op->op == XR_XIR_NULLABLE_IS_SOME) { value = op->immediate ? !some : some; break; }
        if (!some || !payload) return XR_XIR_RUN_NULL_UNWRAP;
        if (!xr_xir_type_is_owned(run->module->types, op->type)) { value = payload->payload; break; }
        XrXirValue owned = {0};
        XrXirValueStatus copied = xr_xir_value_copy(payload, &owned);
        if (copied != XR_XIR_VALUE_OK) return value_run_status(copied);
        state->instruction = next;
        xr_xir_owned_slot_move(run->frame, run->layout->offsets[result_id], &owned);
        return XR_XIR_RUN_OK;
    }
    case XR_XIR_ARRAY_REPEAT: {
        if (!run->view) return XR_XIR_RUN_BAD_ARTIFACT;
        state->instruction = next;
        XrXirValue fill = vm_value_operand(run, op->args[1]), output = {0};
        XrXirCallStatus status = xr_xir_instance_array_repeat(run->view, op->type,
            xr_xir_scalar_load(run->frame, run->layout->offsets[op->args[0]]), &fill, &output);
        if (status == XR_XIR_CALL_NUMERIC_RANGE) return XR_XIR_RUN_NUMERIC_RANGE;
        if (status != XR_XIR_CALL_READY) return status == XR_XIR_CALL_OOM ? XR_XIR_RUN_OUT_OF_MEMORY :
            status == XR_XIR_CALL_LIMIT ? XR_XIR_RUN_FRAME_LIMIT : XR_XIR_RUN_BAD_ARTIFACT;
        xr_xir_owned_slot_move(run->frame, run->layout->offsets[result_id], &output);
        return XR_XIR_RUN_OK;
    }
    case XR_XIR_EQUAL: {
        if (!run->view) return XR_XIR_RUN_BAD_ARTIFACT;
        XrXirValue left = vm_value_operand(run,op->args[0]);
        XrXirValue right = vm_value_operand(run,op->args[1]);
        XrXirValueAdmission *admission = xr_xir_call_admission(run->view);
        bool equal = false;
        XrXirValueStatus status = xr_xir_value_equal(&left,&right,(XrXirType)left.type,admission,&equal);
        if (status != XR_XIR_VALUE_OK) return value_run_status(status);
        value = op->immediate ? !equal : equal;
        break;
    }
    case XR_XIR_RANGE_CHECK: {
        const uint32_t *ids = &run->function->operands[op->args[0]];
        if (!xr_xir_range_valid(
                xr_xir_scalar_load(run->frame, run->layout->offsets[ids[0]]),
                xr_xir_scalar_load(run->frame, run->layout->offsets[ids[1]]),
                xr_xir_scalar_load(run->frame, run->layout->offsets[ids[2]])))
            return XR_XIR_RUN_NUMERIC_RANGE;
        break;
    }
    case XR_XIR_ASSERT_CONDITION:
        if (!xr_xir_scalar_load(run->frame, run->layout->offsets[op->args[0]])) {
            XrXirValue message = vm_value_operand(run, op->args[1]);
            *action = xr_xir_call_assertion(&message);
        }
        break;
    case XR_XIR_MATCH_FAIL:
        *action = xr_xir_call_match_failure();
        break;
    case XR_XIR_THROW:
        *action = (XrXirAction) {XR_XIR_ACTION_THROW, 0, NULL, 0, {(uint32_t) xr_xir_operand_type(run->function, op->args[0]), 0,
            xr_xir_scalar_load(run->frame, run->layout->offsets[op->args[0]])}, {0}, 0};
        if (action->value.type == XR_XIR_ERROR) {
            XrXirValue concrete = {0};
            if (!xr_xir_error_borrow(&action->value, &concrete)) return XR_XIR_RUN_BAD_ARTIFACT;
            action->value = concrete;
        }
        return XR_XIR_RUN_OK;
    default:
        return XR_XIR_RUN_BAD_ARTIFACT;
    }
    if (op->type != XR_XIR_UNIT)
        xr_xir_scalar_store(run->frame, run->layout->offsets[result_id], value);
    state->instruction = next;
    return XR_XIR_RUN_OK;
}

static XrXirAction vm_resume_observed(XrXirCallView *view, const XrXirVmBinding *binding,
    XrXirVmDebug *debug) {
    if (xr_xir_call_observer_vm(view) != XR_XIR_CALL_READY) return xr_xir_call_fault(XR_XIR_RUN_BAD_ARTIFACT);
    if (xir_artifact_is_evaluation(binding->artifact))
        return (XrXirAction){XR_XIR_ACTION_FAULT,0,NULL,0,{0},{0},0};
    const XrXirModule *module = xr_xir_compile_artifact_module(binding->artifact);
    const XrXirFunction *function = &module->functions[binding->function];
    const XrXirFunctionLayout *layout = xr_xir_compile_artifact_layout(binding->artifact, binding->function);
    VmState *state = view->state;
    ScalarRun run = {module, function, layout, state + 1, view, NULL};
    state->arguments = (XrXirValue *) ((unsigned char *) run.frame + layout->frame_bytes);
    state->path_steps = (XrXirValuePathStep *)(state->arguments + layout->outgoing_count);
    if (view->phase == XR_XIR_CALL_EXIT) return vm_cleanup_exit(&run, state);
    if (state->leaving) return vm_cleanup_continue(&run, state);
    if (!state->initialized) {
        if (view->argument_count != function->parameter_count)
            return (XrXirAction) {XR_XIR_ACTION_FAULT, 0, NULL, 0, {0, 0, 0}, {0}, 0};
        for (uint32_t i = 0; i < view->argument_count; ++i)
            if (!xr_xir_value_argument(&view->arguments[i], view->arena, function->parameters[i]))
                return (XrXirAction) {XR_XIR_ACTION_FAULT, 0, NULL, 0, {0, 0, 0}, {0}, 0};
        for (uint32_t i = 0; i < view->argument_count; ++i) {
            if (xr_xir_compile_type_arena_borrowed(view->arena,function->parameters[i])) {
                state->arguments[0]=(XrXirValue){0};
                XrXirValueStatus status=xr_xir_call_view_argument_take(view,i,state->arguments);
                if (status!=XR_XIR_VALUE_OK) return xr_xir_call_view_operation_fault(status,(XrXirFaultDetail){0});
                xr_xir_owned_slot_move(run.frame,layout->offsets[i],state->arguments);
            } else if (xr_xir_type_is_owned(module->types, function->parameters[i])) {
                if (xr_xir_owned_slot_copy(run.frame, layout->offsets[i], view->arena, function->parameters[i], view->arguments[i].payload) != XR_XIR_VALUE_OK)
                    return (XrXirAction) {XR_XIR_ACTION_FAULT, 0, NULL, 0, {XR_XIR_I64, 0, XR_XIR_CALL_LIMIT}, {0}, 0};
            } else xr_xir_scalar_store(run.frame, layout->offsets[i], view->arguments[i].payload);
        }
        state->initialized = true;
    }
    if (state->waiting) {
        state->waiting = false;
        uint32_t handler = state->panic;
        state->panic = 0;
        if (xr_xir_call_panic_status(view->inbox.status)) {
            if (!handler) return xr_xir_call_fault(XR_XIR_RUN_BAD_ARTIFACT);
            state->invoke = 0;
            return vm_panic_land(&run, state, handler, (XrXirAction) {XR_XIR_ACTION_FAULT, 0, NULL, 0,
                {XR_XIR_I64, 0, view->inbox.status}, view->inbox.panic, 0});
        }
        XrXirValue inbox = view->inbox.value;
        bool discarded = false;
        if (state->invoke) {
            const XrXirInstruction *op = &function->instructions[state->invoke - 1];
            bool error = view->inbox.status == XR_XIR_CALL_THROWN;
            if (!error && view->inbox.status != XR_XIR_CALL_RETURNED) return xr_xir_call_fault(XR_XIR_RUN_BAD_ARTIFACT);
            uint32_t first = function->blocks[op->targets[error ? 1 : 0]].first;
            state->expected = error ? XR_XIR_ERROR : op->op == XR_XIR_TASK_AWAIT ?
                xr_xir_task_element(module->types, xr_xir_operand_type(function, op->args[0])) :
                op->op==XR_XIR_ITERATOR_HAS_NEXT ? XR_XIR_BOOL : op->op==XR_XIR_ITERATOR_NEXT || op->op==XR_XIR_ITERATOR_NTH ?
                xr_xir_iterator_element(module->types,xr_xir_operand_type(function,op->args[0])) : op->type;
            discarded = !error && function->instructions[first].op == XR_XIR_INVOKE_DISCARD;
            if (discarded && xr_xir_call_discard_inbox(view,state->expected) != XR_XIR_CALL_READY)
                return xr_xir_call_fault(XR_XIR_RUN_BAD_ARTIFACT);
            state->destination = discarded || state->expected == XR_XIR_UNIT ? UINT32_MAX : layout->offsets[function->parameter_count + first];
            state->instruction = first + (discarded || state->expected != XR_XIR_UNIT);
            state->invoke = 0;
            if (error) inbox.type = XR_XIR_ERROR;
        } else
        if (view->inbox.status == XR_XIR_CALL_THROWN)
            return (XrXirAction) {XR_XIR_ACTION_THROW, 0, NULL, 0, inbox, {0}, 0};
        if (!discarded) {
        if (view->inbox.status != XR_XIR_CALL_RETURNED && view->inbox.status != XR_XIR_CALL_THROWN)
            return (XrXirAction) {XR_XIR_ACTION_FAULT, 0, NULL, 0, {0, 0, 0}, {0}, 0};
        if (state->expected == XR_XIR_UNIT ?
            (inbox.type != XR_XIR_UNIT || inbox.reserved || inbox.payload) :
            !xr_xir_value_argument(&inbox, view->arena, state->expected))
            return (XrXirAction) {XR_XIR_ACTION_FAULT, 0, NULL, 0, {0, 0, 0}, {0}, 0};
        if (xr_xir_compile_type_arena_borrowed(view->arena,state->expected)) {
            state->arguments[0]=(XrXirValue){0};
            XrXirValueStatus status=xr_xir_call_view_inbox_take(view,(XrXirType)state->expected,state->arguments);
            if (status!=XR_XIR_VALUE_OK) return xr_xir_call_view_operation_fault(status,(XrXirFaultDetail){0});
            xr_xir_owned_slot_move(run.frame,state->destination,state->arguments);
        } else if (xr_xir_type_is_owned(module->types, state->expected)) {
            if (xr_xir_owned_slot_copy(run.frame, state->destination, view->arena, state->expected, inbox.payload) != XR_XIR_VALUE_OK)
                return (XrXirAction) {XR_XIR_ACTION_FAULT, 0, NULL, 0, {XR_XIR_I64, 0, XR_XIR_CALL_LIMIT}, {0}, 0};
        } else if (state->destination != UINT32_MAX)
            xr_xir_scalar_store(run.frame, state->destination, inbox.payload);
        }
        if (debug && xir_vm_debug_inbox(debug, state, state->destination) != XR_XIR_VALUE_OK)
            return (XrXirAction){.kind=XR_XIR_ACTION_FAULT,
                .value={XR_XIR_I64,0,XR_XIR_CALL_LIMIT}};
    }
    XirVmDebugPoint point = {binding, layout, state, run.frame, view, state->instruction};
    if (debug) {
        bool stopped = false;
        XrXirValueStatus observed = xir_vm_debug_before(debug, &point, &stopped);
        if (observed != XR_XIR_VALUE_OK) {
            xir_vm_debug_faulted(debug,view);
            return (XrXirAction){XR_XIR_ACTION_FAULT,0,NULL,0,
                {XR_XIR_I64,0,observed == XR_XIR_VALUE_OOM ? XR_XIR_CALL_OOM :
                    observed == XR_XIR_VALUE_BAD_ARGUMENT ? XR_XIR_CALL_BAD_ARGUMENT :
                    observed == XR_XIR_VALUE_UNSUPPORTED ? XR_XIR_CALL_UNSUPPORTED : XR_XIR_CALL_LIMIT},{0},0};
        }
        if (stopped) return (XrXirAction){.kind=XR_XIR_ACTION_CONTINUE};
    }
    uint32_t at = state->instruction;
    XrXirAction action;
    XrXirRunStatus status = scalar_step(&run, state, &action);
    if (status != XR_XIR_RUN_OK)
        action = xr_xir_call_numeric_fault(status, at < function->instruction_count &&
            function->instructions[at].op == XR_XIR_REM_INT);
    if (debug) {
        XirVmDebugPendingCall pending={0};
        if (action.kind==XR_XIR_ACTION_CALL && vm_debug_pending(&point,&action,&pending)!=XR_XIR_VALUE_OK)
            action=xr_xir_call_fault(XR_XIR_RUN_BAD_ARTIFACT);
        xir_vm_debug_after(debug,&point,action.kind,action.kind==XR_XIR_ACTION_CALL ? &pending : NULL);
    }
    if (action.kind == XR_XIR_ACTION_FAULT && at < function->instruction_count) {
        uint32_t handler = function->blocks[vm_block(function, at)].panic;
        if (handler) return vm_panic_land(&run, state, handler, action);
    }
    return action;
}

static void vm_release_bound(XrXirCallView *view, const XrXirVmBinding *binding) {
    const XrXirFunctionLayout *layout = xr_xir_compile_artifact_layout(binding->artifact, binding->function);
    VmState *state = view->state;
    xr_xir_atomic_progress_clear(&state->atomic);
    for (uint32_t i = layout->owned_count; i > 0; --i)
        xr_xir_owned_slot_clear(state + 1, layout->owned_offsets[i - 1]);
}

static XrXirAction vm_resume(XrXirCallView *view) {
    return vm_resume_observed(view, view->environment, NULL);
}
static void vm_release(XrXirCallView *view, XrXirCallStatus reason) {
    (void) reason;
    vm_release_bound(view, view->environment);
}
typedef struct DebugVmBinding {
    XrXirVmBinding base;
    XrXirVmDebug *debug;
} DebugVmBinding;
static XrXirAction debug_vm_resume(XrXirCallView *view) {
    const DebugVmBinding *binding = view->environment;
    return vm_resume_observed(view, &binding->base, binding->debug);
}
static void debug_vm_release(XrXirCallView *view, XrXirCallStatus reason) {
    (void) reason;
    const DebugVmBinding *binding = view->environment;
    xir_vm_debug_release(binding->debug, view->state);
    vm_release_bound(view, &binding->base);
}

static XrXirStatus bind_verified(const XrXirArtifact *artifact, uint32_t function,
                                 XrXirVmBinding *binding, XrXirCallEntry *entry) {
    if (xir_artifact_is_evaluation(artifact)) return XR_XIR_BAD_STAGE;
    const XrXirCompileContext *context = xr_xir_compile_artifact_context(artifact);
    if (!xir_compile_work(context, 1)) return XR_XIR_BUDGET;
    const XrXirModule *module = xr_xir_compile_artifact_module(artifact);
    const XrXirFunction *body = &module->functions[function];
    const XrXirFunctionLayout *layout = xr_xir_compile_artifact_layout(artifact, function);
    uint64_t bytes = sizeof(VmState) + (uint64_t)layout->frame_bytes +
        (uint64_t)layout->outgoing_count * sizeof(XrXirValue) +
        (uint64_t)layout->path_count * sizeof(XrXirValuePathStep);
    if (bytes > UINT32_MAX) return XR_XIR_BUDGET;
    XrXirCallEntry result = {XR_XIR_CALL_ABI_VERSION, body->parameters, body->parameter_count,
        body->result, (uint32_t)bytes, vm_resume, vm_release, binding, 0, 0};
    if (module->declarations) {
        result.cleanup_owner = module->declarations->functions[function].cleanup_owner;
        if (xr_xir_function_is_generator(module,function)) result.flags|=XR_XIR_ENTRY_GENERATOR;
        for (uint32_t i = function + 1; i < module->function_count; ++i) {
            if (!xir_compile_work(context, 1)) return XR_XIR_BUDGET;
            if (module->declarations->functions[i].cleanup_owner == function + 1) result.flags |= XR_XIR_ENTRY_EXIT;
        }
    }
    if (!xir_compile_work(context, sizeof(*binding) + sizeof(*entry))) return XR_XIR_BUDGET;
    *binding = (XrXirVmBinding) {artifact, function};
    *entry = result;
    return XR_XIR_OK;
}
XR_FUNC XrXirStatus xr_xir_compile_vm_bind(const XrXirArtifact *artifact, uint32_t function,
    XrXirVmBinding *binding, XrXirCallEntry *entry) {
    if (!binding || !entry) return XR_XIR_BAD_STRUCTURE;
    const XrXirModule *module = xr_xir_compile_artifact_module(artifact);
    if (!module || module->stage != XR_XIR_LOWERED || xir_artifact_is_evaluation(artifact)) return XR_XIR_BAD_STAGE;
    if (function >= module->function_count) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status = xr_xir_compile_artifact_verify(artifact, NULL);
    return status == XR_XIR_OK ? bind_verified(artifact, function, binding, entry) : status;
}

XR_FUNC XrXirStatus xr_xir_compile_vm_bind_table(const XrXirArtifact *artifact,
    XrXirVmBinding **bindings, XrXirCallEntry **entries) {
    if (!bindings || !entries || (void *) bindings == (void *) entries || *bindings || *entries)
        return XR_XIR_BAD_STRUCTURE;
    const XrXirModule *module = xr_xir_compile_artifact_module(artifact);
    if (!module || module->stage != XR_XIR_LOWERED || xir_artifact_is_evaluation(artifact)) return XR_XIR_BAD_STAGE;
    const XrXirCompileContext *context = xr_xir_compile_artifact_context(artifact);
    XrXirStatus status = xr_xir_compile_artifact_verify(artifact, NULL);
    if (status != XR_XIR_OK) return status;
    XrXirVmBinding *bound = xir_compile_calloc(context, module->function_count, sizeof(*bound), &status);
    XrXirCallEntry *table = NULL;
    if (status != XR_XIR_OK) goto finish;
    table = xir_compile_calloc(context, module->function_count, sizeof(*table), &status);
    if (status != XR_XIR_OK) goto finish;
    for (uint32_t i = 0; i < module->function_count; ++i) {
        status = bind_verified(artifact, i, &bound[i], &table[i]);
        if (status != XR_XIR_OK) goto finish;
    }
    if (!xir_compile_work(context, sizeof(*bindings) + sizeof(*entries))) {
        status = XR_XIR_BUDGET;
        goto finish;
    }
    *bindings = bound;
    *entries = table;
    return XR_XIR_OK;
finish:
    xr_compile_resources_free(table);
    xr_compile_resources_free(bound);
    return status;
}

typedef struct VmProgramOwner {
    XrXirArtifact *artifact;
    XrXirVmBinding *bindings;
} VmProgramOwner;
static void vm_program_release(void *pointer) {
    VmProgramOwner *owner = pointer;
    if (!owner) return;
    /* The Program input owns the artifact; this lease owns binding storage. */
    xr_compile_resources_free(owner->bindings);
    xr_compile_resources_free(owner);
}
/* Dispatch identity checks wiring, never execution authority. Complete body,
 * descriptor and layout correspondence still consume the independently proved input. */
static XrXirStatus vm_program_match(const XrXirCompileContext *context,
    const XrXirArtifact *artifact, const VmProgramOwner *owner, const XrXirProgram *program) {
    XrXirStatus status=xr_xir_compile_program_body_match(context,program->permissions,artifact);
    if (status!=XR_XIR_OK) return status;
    const XrXirModule *module=xr_xir_compile_artifact_module(artifact);
    if (!owner->bindings || program->entry_count!=module->function_count) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t f=0;f<program->entry_count;++f) {
        if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
        const XrXirCallEntry *entry=&program->entries[f];
        const XrXirVmBinding *binding=&owner->bindings[f];
        const XrXirFunctionLayout *layout=xr_xir_compile_artifact_layout(artifact,f);
        uint64_t bytes=sizeof(VmState)+(uint64_t)layout->frame_bytes+
            (uint64_t)layout->outgoing_count*sizeof(XrXirValue)+
            (uint64_t)layout->path_count*sizeof(XrXirValuePathStep);
        if (bytes>UINT32_MAX || entry->abi_version!=XR_XIR_CALL_ABI_VERSION ||
            entry->state_bytes!=bytes || entry->resume!=vm_resume || entry->release!=vm_release ||
            entry->environment!=binding || binding->artifact!=artifact || binding->function!=f ||
            (entry->parameter_count && !entry->parameters)) return XR_XIR_BAD_STRUCTURE;
    }
    XrXirProgramSpec spec={XR_XIR_PROGRAM_ABI_VERSION,program->permissions->target,
        program->entries,program->entry_count,program->declarations,{0},program->types,{0}};
    return xr_xir_compile_program_match(context,&spec,program->permissions->layouts,artifact);
}

XR_FUNC XrXirStatus xr_xir_compile_vm_program_take(XrXirArtifact **artifact,
    XrXirProgram **output) {
    if (!output || *output || !artifact) return XR_XIR_BAD_STRUCTURE;
    const XrXirModule *module = xr_xir_compile_artifact_module(*artifact);
    if (!module || module->stage != XR_XIR_LOWERED || xir_artifact_is_evaluation(*artifact)) return XR_XIR_BAD_STAGE;
    if (!module->declarations) return XR_XIR_BAD_STRUCTURE;
    XrXirCompileContext current_context=*xr_xir_compile_artifact_context(*artifact);
    const XrXirCompileContext *context=&current_context;
    XrXirArtifact *original=*artifact;
    XrXirProgramInput *input=NULL;XrXirArtifact *owned=original;
    XrXirProgram *program=NULL;
    XrXirStatus status=xir_program_input_take(context,&owned,&input);
    if (status!=XR_XIR_OK) return status;
    uint32_t entry_count=module->function_count;
    VmProgramOwner *owner = xir_compile_calloc(context, 1, sizeof(*owner), &status);
    if (!owner) { input->artifact=NULL;xir_program_input_free(input);return status; }
    owner->bindings = xir_compile_calloc(context, entry_count, sizeof(*owner->bindings), &status);
    XrXirCallEntry *entries = xir_compile_calloc(context, entry_count, sizeof(*entries), &status);
    if (status != XR_XIR_OK) goto finish;
    for (uint32_t i = 0; i < entry_count; ++i) {
        status = bind_verified(*artifact, i, &owner->bindings[i], &entries[i]);
        if (status != XR_XIR_OK) goto finish;
        const XrXirType *parameters=entries[i].parameters;
        entries[i].parameters=NULL;
        uint64_t bytes=(uint64_t)entries[i].parameter_count*sizeof(*parameters);
        if (bytes>SIZE_MAX) { status=XR_XIR_BUDGET;goto finish; }
        entries[i].parameters=xir_compile_copy(context,parameters,(size_t)bytes,&status);
        if (status!=XR_XIR_OK) goto finish;
    }
    XrXirProgramSpec spec = {XR_XIR_PROGRAM_ABI_VERSION, *xr_xir_compile_artifact_target(*artifact),
        entries, entry_count, module->declarations, {owner, vm_program_release}, module->types,
        {0}};
    status = xir_program_receive(context,&spec,&input,&entries,&program);
finish:
    if (entries) for (uint32_t i=0;i<entry_count;++i)
        xr_compile_resources_free((void *)entries[i].parameters);
    xr_compile_resources_free(entries);
    if (status==XR_XIR_OK && (*output || *artifact!=original)) status=XR_XIR_BAD_STRUCTURE;

    /* No allocating or releasing operation follows successful correspondence.
     * The original artifact remains caller-owned on every failed preparation. */
    if (status==XR_XIR_OK) status=vm_program_match(context,*artifact,owner,program);
    if (status!=XR_XIR_OK) {
        if (program) {
            program->permissions->input->artifact=NULL;
            xr_xir_compile_program_drop(program);
        } else {
            if (input) { input->artifact=NULL;xir_program_input_free(input); }
            vm_program_release(owner);
        }
        return status;
    }
    owner->artifact=*artifact;*artifact=NULL;*output=program;
    return XR_XIR_OK;
}
typedef struct DebugVmProgramOwner {
    XrXirArtifact *artifact;
    DebugVmBinding *bindings;
    XrXirVmDebug *debug;
    XrXirCodeLease native;
    XirCallNativeSourceCode native_code;
    uint32_t *native_offsets;
    bool bound;
} DebugVmProgramOwner;
static void debug_vm_program_release(void *pointer) {
    DebugVmProgramOwner *owner = pointer;
    if (owner->bound) xir_vm_debug_unbind(owner->debug);
    if (owner->native.release) owner->native.release(owner->native.owner);
    /* The canonical Program input owns the artifact; this lease owns wiring. */
    xr_compile_resources_free(owner->bindings);
    xr_compile_resources_free(owner->native_offsets);
    xr_xir_vm_debug_drop(owner->debug);
    xr_compile_resources_free(owner);
}
typedef struct DebugProgramAdmission {
    const XrXirVmDebugNative *native;
    XrXirCodeLease lease;
} DebugProgramAdmission;
/* Current wire identity detects drift only. The canonical Input receiver below
 * separately repeats complete body, metadata, layout and Final verification. */
static XrXirStatus debug_native_match(const XrXirArtifact *artifact,const XrXirVmDebugNative *input) {
    const XrXirProgramSpec *native=input->program;const unsigned char *mask=input->mask;uint32_t count=input->count;
    const XirCallNativeSourceCode *code=input->source;
    if (!native || !code || code->version!=XIR_CALL_NATIVE_SOURCE_VERSION || code->count!=count ||
        !code->entries || !code->offsets) return XR_XIR_BAD_STRUCTURE;
    const XrXirModule *module=xr_xir_compile_artifact_module(artifact);
    const XrXirTarget *target=xr_xir_compile_artifact_target(artifact);
    if (!mask || count!=module->function_count || native->entry_count!=count || !native->entries ||
        native->abi_version!=XR_XIR_PROGRAM_ABI_VERSION || native->target.architecture!=target->architecture ||
        native->target.abi_version!=target->abi_version || (!!native->code.owner!=!!native->code.release)) return XR_XIR_BAD_STRUCTURE;
    XrXirProgramPacket packet=xir_program_packet_borrow(artifact);
    if (!packet.bytes || !packet.identity || !packet.length || !native->packet.bytes ||
        !native->packet.identity || native->packet.length!=packet.length) return XR_XIR_BAD_STRUCTURE;
    const XrXirCompileContext *context=xr_xir_compile_artifact_context(artifact);
    if (!xir_compile_work(context,packet.length+32+count)) return XR_XIR_BUDGET;
    if (memcmp(packet.bytes,native->packet.bytes,packet.length) || memcmp(packet.identity,native->packet.identity,32))
        return XR_XIR_BAD_STRUCTURE;
    uint32_t natives=0;
    for (uint32_t i=0;i<count;++i) {
        if (mask[i]>1) return XR_XIR_BAD_STRUCTURE;natives+=mask[i];
        if (!mask[i]) continue;
        const XrXirCallEntry *actual=&native->entries[i],*emitted=&code->entries[i];
        const XrXirFunctionLayout *layout=xr_xir_compile_artifact_layout(artifact,i);
        if (actual->resume!=emitted->resume || actual->release!=emitted->release ||
            actual->environment!=emitted->environment || actual->state_bytes!=emitted->state_bytes ||
            code->offsets[i]==UINT32_MAX || code->offsets[i]>actual->state_bytes ||
            layout->frame_bytes>actual->state_bytes-code->offsets[i]) return XR_XIR_BAD_STRUCTURE;
    }
    return natives?XR_XIR_OK:XR_XIR_UNSUPPORTED;
}
static XrXirStatus debug_native_admit(const XrXirCompileContext *context,
    const XrXirProgramInput *owned,const XrXirVmDebugNative *native) {
    XrXirStatus status=debug_native_match(owned->artifact,native);
    XrXirProgramPermissions *permissions=NULL;
    if (status==XR_XIR_OK) status=xir_program_input_prepare(context,native->program,owned,&permissions);
    xir_program_permissions_free(permissions);return status;
}
/* All allocating preparation and temporary retirement precede this comparison.
 * Neither a copied callback table nor a previous read grants execution rights. */
static XrXirStatus debug_program_match(const XrXirCompileContext *context,const XrXirArtifact *artifact,
    const DebugVmProgramOwner *owner,const XrXirProgram *program,const DebugProgramAdmission *request) {
    if (!program->permissions || !program->permissions->input ||
        program->permissions->input->artifact!=artifact) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=xr_xir_compile_program_body_match(context,program->permissions,artifact);
    if (status!=XR_XIR_OK) return status;
    const XrXirModule *module=xr_xir_compile_artifact_module(artifact);
    if (!owner->bindings || program->entry_count!=module->function_count) return XR_XIR_BAD_STRUCTURE;
    if (request->native) {
        status=debug_native_match(artifact,request->native);
        if (status!=XR_XIR_OK) return status;
        const XrXirProgramSpec *native=request->native->program;
        if (native->code.owner!=request->lease.owner || native->code.release!=request->lease.release)
            return XR_XIR_BAD_STRUCTURE;
        status=xr_xir_compile_program_descriptor_match(context,native,program->permissions);
        if (status!=XR_XIR_OK) return status;
    }
    for (uint32_t f=0;f<program->entry_count;++f) {
        if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
        const XrXirCallEntry *entry=&program->entries[f];
        if (request->native && request->native->mask[f]) {
            const XrXirCallEntry *native=&request->native->program->entries[f];
            if (!owner->native_offsets || owner->native_offsets[f]!=request->native->source->offsets[f] ||
                entry->resume!=native->resume || entry->release!=native->release ||
                entry->environment!=native->environment || entry->state_bytes!=native->state_bytes)
                return XR_XIR_BAD_STRUCTURE;
            continue;
        }
        const DebugVmBinding *binding=&owner->bindings[f];
        const XrXirFunctionLayout *layout=xr_xir_compile_artifact_layout(artifact,f);
        uint64_t bytes=sizeof(VmState)+(uint64_t)layout->frame_bytes+
            (uint64_t)layout->outgoing_count*sizeof(XrXirValue)+(uint64_t)layout->path_count*sizeof(XrXirValuePathStep);
        if (bytes>UINT32_MAX || entry->abi_version!=XR_XIR_CALL_ABI_VERSION || entry->state_bytes!=bytes ||
            entry->resume!=debug_vm_resume || entry->release!=debug_vm_release || entry->environment!=binding ||
            binding->base.artifact!=artifact || binding->base.function!=f || binding->debug!=owner->debug ||
            (request->native && owner->native_offsets[f]!=UINT32_MAX)) return XR_XIR_BAD_STRUCTURE;
    }
    XrXirProgramSpec descriptor={XR_XIR_PROGRAM_ABI_VERSION,program->permissions->target,
        program->entries,program->entry_count,program->declarations,{0},program->types,{0}};
    return xr_xir_compile_program_match(context,&descriptor,program->permissions->layouts,artifact);
}
static XrXirStatus debug_program_take(XrXirArtifact **artifact,XrXirVmDebug *debug,
    const XrXirVmDebugNative *native,XrXirProgram **output) {
    if (!output || *output || !artifact || !debug || (native && !native->program)) return XR_XIR_BAD_STRUCTURE;
    const XrXirModule *module=xr_xir_compile_artifact_module(*artifact);
    if (!module || module->stage!=XR_XIR_LOWERED || xir_artifact_is_evaluation(*artifact)) return XR_XIR_BAD_STAGE;
    if (!module->declarations) return XR_XIR_BAD_STRUCTURE;
    XrXirCompileContext current_context=*xr_xir_compile_artifact_context(*artifact);
    const XrXirCompileContext *context=&current_context;
    DebugProgramAdmission request={native,native?native->program->code:(XrXirCodeLease){0}};
    if (!xir_compile_work(context,sizeof(request))) return XR_XIR_BUDGET;
    XrXirArtifact *original=*artifact,*moved=original;XrXirProgramInput *input=NULL;
    XrXirStatus status=xir_program_input_take(context,&moved,&input);
    if (status!=XR_XIR_OK) return status;
    if (native) {
        status=debug_native_admit(context,input,native);
        if (status!=XR_XIR_OK) { input->artifact=NULL;xir_program_input_free(input);return status; }
    }
    uint32_t count=module->function_count;XrXirProgram *program=NULL;
    DebugVmProgramOwner *owner=xir_compile_calloc(context,1,sizeof(*owner),&status);
    if (!owner) { input->artifact=NULL;xir_program_input_free(input);return status; }
    if (!xir_vm_debug_retain(debug)) {
        input->artifact=NULL;xir_program_input_free(input);xr_compile_resources_free(owner);return XR_XIR_BAD_STRUCTURE;
    }
    owner->debug=debug;
    owner->bindings=xir_compile_calloc(context,count,sizeof(*owner->bindings),&status);
    XrXirCallEntry *entries=NULL;
    if (!owner->bindings) goto finish;
    if (native) {
        owner->native_offsets=xir_compile_calloc(context,count,sizeof(*owner->native_offsets),&status);
        if (!owner->native_offsets) goto finish;
        if (!xir_compile_work(context,(uint64_t)count*sizeof(*owner->native_offsets)+sizeof(owner->native_code))) {
            status=XR_XIR_BUDGET;goto finish;
        }
        for (uint32_t f=0;f<count;++f) owner->native_offsets[f]=native->mask[f]?native->source->offsets[f]:UINT32_MAX;
        owner->native_code=(XirCallNativeSourceCode){XIR_CALL_NATIVE_SOURCE_VERSION,count,NULL,owner->native_offsets};
    }
    entries=xir_compile_calloc(context,count,sizeof(*entries),&status);
    if (status!=XR_XIR_OK) goto finish;
    for (uint32_t f=0;f<count;++f) {
        if (native && native->mask[f]) entries[f]=native->program->entries[f];
        else {
            status=bind_verified(original,f,&owner->bindings[f].base,&entries[f]);
            if (status!=XR_XIR_OK) goto finish;
            owner->bindings[f].debug=debug;entries[f].environment=&owner->bindings[f];
            entries[f].resume=debug_vm_resume;entries[f].release=debug_vm_release;
        }
        const XrXirType *parameters=entries[f].parameters;entries[f].parameters=NULL;
        uint64_t bytes=(uint64_t)entries[f].parameter_count*sizeof(*parameters);
        if (bytes>SIZE_MAX) { status=XR_XIR_BUDGET;goto finish; }
        entries[f].parameters=xir_compile_copy(context,parameters,(size_t)bytes,&status);
        if (status!=XR_XIR_OK) goto finish;
    }
    if (!xir_vm_debug_bind(debug,original)) { status=XR_XIR_BAD_STRUCTURE;goto finish; }
    owner->bound=true;
    XrXirProgramSpec spec={XR_XIR_PROGRAM_ABI_VERSION,*xr_xir_compile_artifact_target(original),
        entries,count,module->declarations,{owner,debug_vm_program_release},module->types,{0}};
    status=xir_program_receive(context,&spec,&input,&entries,&program);
finish:
    if (entries) for (uint32_t f=0;f<count;++f) xr_compile_resources_free((void *)entries[f].parameters);
    xr_compile_resources_free(entries);
    if (status==XR_XIR_OK && (*output || *artifact!=original)) status=XR_XIR_BAD_STRUCTURE;
    if (status==XR_XIR_OK) status=debug_program_match(context,original,owner,program,&request);
    if (status!=XR_XIR_OK) {
        if (program) { program->permissions->input->artifact=NULL;xr_xir_compile_program_drop(program); }
        else { if (input) { input->artifact=NULL;xir_program_input_free(input); }debug_vm_program_release(owner); }
        return status;
    }
    owner->artifact=original;*artifact=NULL;
    if (native) { owner->native=request.lease;native->program->code=(XrXirCodeLease){0}; }
    owner->native_code.entries=program->entries;
    *output=program;xir_vm_debug_program(debug,program,native?&owner->native_code:NULL);return XR_XIR_OK;
}
XR_FUNC XrXirStatus xr_xir_compile_vm_debug_program_take(XrXirArtifact **artifact,
    XrXirVmDebug *debug,XrXirProgram **output) {
    return debug_program_take(artifact,debug,NULL,output);
}
XR_FUNC XrXirStatus xr_xir_compile_vm_debug_mixed_program_take(XrXirArtifact **artifact,
    const XrXirVmDebugNative *native,XrXirVmDebug *debug,XrXirProgram **output) {
    if (!native) return XR_XIR_BAD_STRUCTURE;
    return debug_program_take(artifact,debug,native,output);
}

XR_FUNC XrXirRunStatus xr_xir_compile_vm_run(const XrXirArtifact *artifact, uint32_t function,
                           XrXirRunContext *context, const XrXirValue *arguments,
                           uint32_t argument_count, XrXirValue *result) {
    if (!result)
        return XR_XIR_RUN_BAD_ARGUMENT;
    *result = (XrXirValue) {0, 0, 0};
    if (!context || (argument_count && !arguments))
        return XR_XIR_RUN_BAD_ARGUMENT;
    const XrXirModule *module = xr_xir_compile_artifact_module(artifact);
    if (!module || module->stage != XR_XIR_LOWERED || function >= module->function_count || xir_artifact_is_evaluation(artifact))
        return XR_XIR_RUN_BAD_ARTIFACT;
    XrXirStatus verified = xr_xir_compile_artifact_verify(artifact, NULL);
    if (verified != XR_XIR_OK)
        return verified == XR_XIR_OUT_OF_MEMORY ? XR_XIR_RUN_OUT_OF_MEMORY : XR_XIR_RUN_BAD_ARTIFACT;
    if (module->declarations && (module->declarations->functions[function].cleanup_owner ||
        module->declarations->functions[function].method_kind==XR_XIR_MATERIALIZER)) return XR_XIR_RUN_BAD_ARTIFACT;
    const XrXirFunction *body = &module->functions[function];
    if (xr_xir_type_is_owned(module->types, body->result)) return XR_XIR_RUN_BAD_ARTIFACT;
    for (uint32_t i = 0; i < body->parameter_count; ++i)
        if (xr_xir_type_is_owned(module->types, body->parameters[i])) return XR_XIR_RUN_BAD_ARTIFACT;
    if (argument_count != body->parameter_count)
        return XR_XIR_RUN_BAD_ARGUMENT;
    for (uint32_t i = 0; i < argument_count; ++i)
        if (!xr_xir_value_argument(&arguments[i], NULL, body->parameters[i]))
            return XR_XIR_RUN_BAD_ARGUMENT;
    for (uint32_t i = 0; i < body->instruction_count; ++i)
        if (body->instructions[i].op == XR_XIR_CALL || body->instructions[i].op == XR_XIR_INVOKE ||
            body->instructions[i].op == XR_XIR_INVOKE_INDIRECT || body->instructions[i].op == XR_XIR_SUSPEND ||
            body->instructions[i].op == XR_XIR_TIMER_AFTER_MS || body->instructions[i].op == XR_XIR_CLOCK_NANOS ||
            body->instructions[i].op == XR_XIR_UTC_OFFSET_AT ||
            body->instructions[i].op == XR_XIR_THROW || body->instructions[i].op == XR_XIR_MATCH_FAIL ||
            body->instructions[i].op == XR_XIR_ASSERT_CONDITION || body->instructions[i].op == XR_XIR_EQUAL ||
            body->instructions[i].op == XR_XIR_PANIC_CATCH ||
            body->instructions[i].op == XR_XIR_CLEANUP_REGISTER || body->instructions[i].op == XR_XIR_CLEANUP_LEAVE ||
            body->instructions[i].op == XR_XIR_CLEANUP_ERROR ||
            xr_xir_type_is_owned(module->types, body->instructions[i].type) ||
            body->instructions[i].op == XR_XIR_OUTPUT || body->instructions[i].op == XR_XIR_PRINT ||
            body->instructions[i].op == XR_XIR_WRITE_STREAM)
            return XR_XIR_RUN_BAD_ARTIFACT;
    for (uint32_t i = 0; i < body->instruction_count; ++i)
        if ((body->instructions[i].op >= XR_XIR_CONST_STRING &&
            body->instructions[i].op <= XR_XIR_SLOT_STORE) ||
            xr_xir_op_is_channel(body->instructions[i].op) || xr_xir_op_is_atomic(body->instructions[i].op) || body->instructions[i].op==XR_XIR_SLOT_GROUP_INIT) return XR_XIR_RUN_BAD_ARTIFACT;
    const XrXirFunctionLayout *layout = xr_xir_compile_artifact_layout(artifact, function);
    void *frame = NULL;
    XrXirRunStatus status = xr_xir_scalar_frame_begin(context, layout->frame_bytes, &frame);
    if (status != XR_XIR_RUN_OK)
        return status;
    for (uint32_t i = 0; i < argument_count; ++i)
        xr_xir_scalar_store(frame, layout->offsets[i], arguments[i].payload);
    ScalarRun run = {module, body, layout, frame, NULL, NULL};
    VmState state = {0};
    for (;;) {
        if (!xr_xir_scalar_step(context)) { status = XR_XIR_RUN_STEP_LIMIT; break; }
        XrXirAction action;
        status = scalar_step(&run, &state, &action);
        if (status != XR_XIR_RUN_OK) break;
        if (action.kind == XR_XIR_ACTION_RETURN) { *result = action.value; break; }
    }
    xr_xir_scalar_frame_end(context, layout->frame_bytes, frame);
    if (status != XR_XIR_RUN_OK)
        *result = (XrXirValue) {0, 0, 0};
    return status;
}
