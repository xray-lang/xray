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
#include "xxir_compile_memory.h"
#include "xxir_equal.h"
#include "xxir_nullable.h"
#include "xxir_float.h"
#include "xxir_types.h"
#include "xxir_operand_roles.h"
#include "xxir_instance_value.h"
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
} ScalarRun;

typedef struct VmState {
    uint32_t instruction, destination, invoke, panic;
    uint32_t frontier, exit_target, exit_block, landing;
    XrXirType expected;
    bool initialized, waiting, cleanup_waiting, leaving;
    XrXirValue *arguments;
    XrXirValuePathStep *path_steps;
} VmState;

static int arithmetic_operation(XrXirOp op) {
    switch (op) {
    case XR_XIR_ADD_INT: return XR_XIR_ARITH_ADD;
    case XR_XIR_SUB_INT: return XR_XIR_ARITH_SUB;
    case XR_XIR_MUL_INT: return XR_XIR_ARITH_MUL;
    case XR_XIR_DIV_INT: return XR_XIR_ARITH_DIV;
    case XR_XIR_REM_INT: return XR_XIR_ARITH_REM;
    case XR_XIR_AND_INT: return XR_XIR_ARITH_AND;
    case XR_XIR_OR_INT: return XR_XIR_ARITH_OR;
    case XR_XIR_XOR_INT: return XR_XIR_ARITH_XOR;
    case XR_XIR_SHL_INT: return XR_XIR_ARITH_SHL;
    case XR_XIR_SHR_INT: return XR_XIR_ARITH_SHR;
    default: return -1;
    }
}

static XrXirRunStatus value_run_status(XrXirValueStatus status) {
    if (status == XR_XIR_VALUE_OK) return XR_XIR_RUN_OK;
    if (status == XR_XIR_VALUE_OOM) return XR_XIR_RUN_OUT_OF_MEMORY;
    if (status == XR_XIR_VALUE_LIMIT || status == XR_XIR_VALUE_REFCOUNT_LIMIT)
        return XR_XIR_RUN_FRAME_LIMIT;
    return XR_XIR_RUN_BAD_ARTIFACT;
}

static XrXirRunStatus instance_step(ScalarRun *run, VmState *state, const XrXirInstruction *op, uint32_t destination) {
    XrXirValue value = {0};
    XrXirCallStatus status = XR_XIR_CALL_READY;
    switch (op->op) {
    case XR_XIR_CELL_LOCAL_WRITE: {
        uint32_t offset = run->layout->offsets[op->args[0]];
        XrXirType type = xr_xir_operand_type(run->function, op->args[0]);
        XrXirValue cell = {(uint32_t)type, 0, xr_xir_scalar_load(run->frame, offset)};
        XrXirValue incoming = {(uint32_t)xr_xir_operand_type(run->function, op->args[1]), 0,
            xr_xir_scalar_load(run->frame, run->layout->offsets[op->args[1]])};
        if (cell.payload) status = xr_xir_instance_cell_write(run->view, &cell, &incoming);
        else {
            status = xr_xir_instance_cell(run->view, type, &incoming, &value);
            if (status == XR_XIR_CALL_READY) xr_xir_owned_slot_move(run->frame, offset, &value);
        }
        break;
    }
    case XR_XIR_CELL_NEW: case XR_XIR_CELL_READ: case XR_XIR_CELL_WRITE: {
        XrXirValue left = {(uint32_t) xr_xir_operand_type(run->function, op->args[0]), 0,
            xr_xir_scalar_load(run->frame, run->layout->offsets[op->args[0]])};
        if (op->op == XR_XIR_CELL_NEW) status = xr_xir_instance_cell(run->view, op->type, &left, &value);
        else if (op->op == XR_XIR_CELL_READ) status = xr_xir_instance_cell_read(run->view, &left, &value);
        else {
            XrXirValue right = {(uint32_t) xr_xir_operand_type(run->function, op->args[1]), 0,
                xr_xir_scalar_load(run->frame, run->layout->offsets[op->args[1]])};
            status = xr_xir_instance_cell_write(run->view, &left, &right);
        }
        break;
    }
    case XR_XIR_FUNCTION_WEAKEN: {
        XrXirValue input = {(uint32_t)xr_xir_operand_type(run->function, op->args[0]), 0,
            xr_xir_scalar_load(run->frame, run->layout->offsets[op->args[0]])};
        status = xr_xir_instance_weaken_function(run->view, op->type, &input, &value); break;
    }
    case XR_XIR_FUNCTION_REF:
        for (uint32_t i = 0; i < op->args[1]; ++i) {
            uint32_t id = run->function->operands[op->args[0] + i];
            state->arguments[i] = (XrXirValue) {(uint32_t) xr_xir_operand_type(run->function, id), 0,
                xr_xir_scalar_load(run->frame, run->layout->offsets[id])};
        }
        status = xr_xir_instance_function(run->view, op->type, (uint32_t) op->immediate,
            state->arguments, op->args[1], &value); break;
    case XR_XIR_CONST_STRING:
        status = xr_xir_instance_literal(run->view, (uint32_t) op->immediate, &value); break;
    case XR_XIR_TO_STRING: {
        XrXirValue scalar = {(uint32_t) xr_xir_operand_type(run->function, op->args[0]), 0,
            xr_xir_scalar_load(run->frame, run->layout->offsets[op->args[0]])};
        status = xr_xir_instance_scalar_text(run->view, &scalar, &value); break;
    }
    case XR_XIR_SLOT_LOAD:
        status = xr_xir_instance_slot_read(run->view, (uint32_t) op->immediate, &value); break;
    case XR_XIR_SLOT_INIT:
    case XR_XIR_SLOT_STORE:
        value = (XrXirValue) {(uint32_t) run->module->declarations->slots[op->immediate].type, 0,
            xr_xir_slot_is_unit(run->module->declarations->slots,run->module->declarations->slot_count,op) ? 0 :
            xr_xir_scalar_load(run->frame, run->layout->offsets[op->args[0]])};
        status = xr_xir_instance_slot_write(run->view, (uint32_t) op->immediate, &value, op->op == XR_XIR_SLOT_INIT);
        break;
    case XR_XIR_ATOMIC_I64_NEW:
        status = xr_xir_instance_atomic(run->view,
            xr_xir_scalar_load(run->frame, run->layout->offsets[op->args[0]]), &value); break;
    case XR_XIR_ATOMIC_I64_LOAD:
    case XR_XIR_ATOMIC_I64_FETCH_ADD: {
        XrXirValue atomic = {XR_XIR_ATOMIC_I64, 0,
            xr_xir_scalar_load(run->frame, run->layout->offsets[op->args[0]])};
        value.type = XR_XIR_I64;
        bool valid = op->op == XR_XIR_ATOMIC_I64_LOAD ? xr_xir_atomic_i64_load(&atomic, &value.payload) :
            xr_xir_atomic_i64_fetch_add(&atomic,
                xr_xir_scalar_load(run->frame, run->layout->offsets[op->args[1]]), &value.payload);
        if (!valid) status = XR_XIR_CALL_BAD_STATE;
        break;
    }
    default: return XR_XIR_RUN_BAD_ARTIFACT;
    }
    if (status != XR_XIR_CALL_READY) return status == XR_XIR_CALL_OOM ? XR_XIR_RUN_OUT_OF_MEMORY :
        status == XR_XIR_CALL_LIMIT ? XR_XIR_RUN_FRAME_LIMIT : XR_XIR_RUN_BAD_ARTIFACT;
    if (xr_xir_type_is_owned(run->module->types, op->type)) xr_xir_owned_slot_move(run->frame, destination, &value);
    else if (op->type != XR_XIR_UNIT) xr_xir_scalar_store(run->frame, destination, value.payload);
    return XR_XIR_RUN_OK;
}

static XrXirValue vm_value_operand(const ScalarRun *run, uint32_t id) {
    return (XrXirValue) {(uint32_t) xr_xir_operand_type(run->function, id), 0,
        xr_xir_scalar_load(run->frame, run->layout->offsets[id])};
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
#include "xxir_vm_path.inc.c"
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
        status=xr_xir_value_admit(&receiver,(XrXirType)receiver.type,admission);
        if(status == XR_XIR_VALUE_OK) status=xr_xir_class_get(&receiver,(uint32_t)op->immediate,admission,&output);
    }
    if(status != XR_XIR_VALUE_OK) return value_run_status(status);
    if(xr_xir_type_is_owned(run->module->types,op->type)) xr_xir_owned_slot_move(run->frame,destination,&output);
    else xr_xir_scalar_store(run->frame,destination,output.payload);
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
        status = op->op == XR_XIR_ENUM_NEW ?
            xr_xir_enum_new(op->type, (uint32_t) op->immediate, op->args[1] ? state->arguments : NULL, op->args[1], admission, &output) :
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
    else xr_xir_scalar_store(run->frame, destination, output.payload);
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
static XrXirRunStatus scalar_edge(ScalarRun *run, uint32_t instruction, uint32_t target) {
    const XrXirFunction *function = run->function;
    uint32_t low = vm_block(function, instruction);
    uint32_t first = function->blocks[target].first, end = first;
    while (end < function->instruction_count && function->instructions[end].op == XR_XIR_PHI) {
        const XrXirInstruction *phi = &function->instructions[end];
        uint32_t source = UINT32_MAX;
        for (uint32_t a = 0; a < phi->args[1]; a += 2)
            if (function->operands[phi->args[0] + a] == low) {
                source = function->operands[phi->args[0] + a + 1]; break;
            }
        if (source == UINT32_MAX) return XR_XIR_RUN_BAD_ARTIFACT;
        uint32_t scratch = run->layout->offsets[function->parameter_count + end] + 8;
        int64_t value = xr_xir_scalar_load(run->frame, run->layout->offsets[source]);
        if (xr_xir_type_is_owned(run->module->types, phi->type)) {
            XrXirValueStatus status = xr_xir_owned_slot_copy(run->frame, scratch,
                run->view ? run->view->arena : NULL, phi->type, value);
            if (status != XR_XIR_VALUE_OK) return value_run_status(status);
        } else xr_xir_scalar_store(run->frame, scratch, value);
        ++end;
    }
    for (uint32_t i = first; i < end; ++i) {
        const XrXirInstruction *phi = &function->instructions[i];
        uint32_t destination = run->layout->offsets[function->parameter_count + i];
        XrXirValue value = {(uint32_t) phi->type, 0, xr_xir_scalar_load(run->frame, destination + 8)};
        xr_xir_scalar_store(run->frame, destination + 8, 0);
        if (xr_xir_type_is_owned(run->module->types, phi->type)) xr_xir_owned_slot_move(run->frame, destination, &value);
        else xr_xir_scalar_store(run->frame, destination, value.payload);
    }
    return XR_XIR_RUN_OK;
}
#include "xxir_vm_cleanup.inc.c"

static XrXirRunStatus vm_call_step(ScalarRun *run, VmState *state, const XrXirInstruction *op,
    XrXirAction *action, uint32_t destination) {
    uint32_t callee = (uint32_t) op->immediate;
    XrXirValue value = {0};
    if (op->op == XR_XIR_CALL_INDIRECT || op->op == XR_XIR_INVOKE_INDIRECT) {
        value = (XrXirValue) {(uint32_t) xr_xir_operand_type(run->function, callee), 0,
            xr_xir_scalar_load(run->frame, run->layout->offsets[callee])};
        XrXirCallStatus status = xr_xir_instance_resolve_function(run->view, &value, &callee);
        if (status != XR_XIR_CALL_READY) {
            *action = (XrXirAction) {XR_XIR_ACTION_FAULT, 0, NULL, 0, {XR_XIR_I64, 0, status}, {0}, 0};
            return XR_XIR_RUN_OK;
        }
    }
    for (uint32_t i = 0; i < op->args[1]; ++i) {
        uint32_t id = run->function->operands[op->args[0] + i];
        state->arguments[i] = (XrXirValue) {(uint32_t) xr_xir_operand_type(run->function, id), 0,
            xr_xir_scalar_load(run->frame, run->layout->offsets[id])};
    }
    uint32_t at = (uint32_t) (op - run->function->instructions);
    state->invoke = op->op == XR_XIR_INVOKE || op->op == XR_XIR_INVOKE_INDIRECT ? at + 1 : 0;
    state->panic = run->function->blocks[vm_block(run->function, at)].panic;
    state->waiting = true; state->destination = destination; state->expected = op->type;
    *action = (XrXirAction) {XR_XIR_ACTION_CALL, callee, state->arguments, op->args[1], value, {0},
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
static XrXirRunStatus vm_integer_step(ScalarRun *run, VmState *state) {
    uint32_t instruction = state->instruction;
    const XrXirInstruction *op = &run->function->instructions[instruction];
    uint32_t result_id = run->function->parameter_count + instruction;
    uint32_t next = instruction + 1;
    int64_t value = 0;
    switch (op->op) {
    case XR_XIR_ADD_INT: case XR_XIR_SUB_INT: case XR_XIR_MUL_INT:
    case XR_XIR_AND_INT: case XR_XIR_OR_INT: case XR_XIR_XOR_INT:
    case XR_XIR_SHL_INT: case XR_XIR_SHR_INT:
    case XR_XIR_DIV_INT: case XR_XIR_REM_INT: {
        int64_t left = xr_xir_scalar_load(run->frame, run->layout->offsets[op->args[0]]);
        int64_t right = xr_xir_scalar_load(run->frame, run->layout->offsets[op->args[1]]);
        XrXirRunStatus status = xr_xir_integer_arithmetic(xr_xir_integer_format(op->type), (XrXirArithmetic) arithmetic_operation(op->op), left, right, &value);
        if (status != XR_XIR_RUN_OK) return status;
        break;
    }
    case XR_XIR_EQ_INT: case XR_XIR_NE_INT: case XR_XIR_LT_INT:
    case XR_XIR_LE_INT: case XR_XIR_GT_INT: case XR_XIR_GE_INT: {
        int ordering = 0;
        XrXirType type = xr_xir_operand_type(run->function, op->args[0]);
        XrXirRunStatus status = xr_xir_integer_compare(xr_xir_integer_format(type),
            xr_xir_scalar_load(run->frame, run->layout->offsets[op->args[0]]),
            xr_xir_scalar_load(run->frame, run->layout->offsets[op->args[1]]), &ordering);
        if (status != XR_XIR_RUN_OK) return status;
        switch (op->op) {
        case XR_XIR_EQ_INT: value = ordering == 0; break;
        case XR_XIR_NE_INT: value = ordering != 0; break;
        case XR_XIR_LT_INT: value = ordering < 0; break;
        case XR_XIR_LE_INT: value = ordering <= 0; break;
        case XR_XIR_GT_INT: value = ordering > 0; break;
        default: value = ordering >= 0; break;
        }
        break;
    }
    case XR_XIR_RUNE_TO_INTEGER: case XR_XIR_INTEGER_TO_RUNE: {
        XrXirType from=xr_xir_operand_type(run->function,op->args[0]);
        XrXirRunStatus status=xr_xir_rune_convert(from,op->type,
            xr_xir_scalar_load(run->frame,run->layout->offsets[op->args[0]]),&value);
        if (status!=XR_XIR_RUN_OK) return status;
        break;
    }
    case XR_XIR_CONVERT_NUMBER: {
        XrXirType from = xr_xir_operand_type(run->function, op->args[0]);
        XrXirRunStatus status = xr_xir_number_convert(from, op->type,
            xr_xir_scalar_load(run->frame, run->layout->offsets[op->args[0]]), &value);
        if (status != XR_XIR_RUN_OK) return status;
        break;
    }
    default: return XR_XIR_RUN_BAD_ARTIFACT;
    }
    xr_xir_scalar_store(run->frame, run->layout->offsets[result_id], value);
    state->instruction = next;
    return XR_XIR_RUN_OK;
}
static XrXirRunStatus scalar_step(ScalarRun *run, VmState *state, XrXirAction *action) {
    uint32_t instruction = state->instruction;
    const XrXirInstruction *op = &run->function->instructions[instruction];
    uint32_t result_id = run->function->parameter_count + instruction;
    uint32_t next = instruction + 1;
    int64_t value = 0;
    *action = (XrXirAction) {XR_XIR_ACTION_CONTINUE, 0, NULL, 0, {0, 0, 0}, {0}, 0};
    if (op->op == XR_XIR_STRING_INDEX_OF || op->op == XR_XIR_STRING_LAST_INDEX_OF ||
        (op->op >= XR_XIR_STRING_CONTAINS && op->op <= XR_XIR_STRING_ENDS_WITH) ||
        op->op == XR_XIR_STRING_LEN || op->op == XR_XIR_EQ_STRING || op->op == XR_XIR_NE_STRING ||
        (op->op >= XR_XIR_LT_STRING && op->op <= XR_XIR_GE_STRING) ||
        op->op == XR_XIR_OUTPUT || op->op == XR_XIR_WRITE_STREAM || op->op == XR_XIR_PRINT)
        return vm_io_step(run, state, action);
    if (arithmetic_operation(op->op) >= 0 || (op->op == XR_XIR_EQ_INT || op->op == XR_XIR_NE_INT || op->op == XR_XIR_LT_INT ||
        op->op == XR_XIR_LE_INT || op->op == XR_XIR_GT_INT || op->op == XR_XIR_GE_INT) ||
        op->op == XR_XIR_CONVERT_NUMBER || op->op == XR_XIR_RUNE_TO_INTEGER ||
        op->op == XR_XIR_INTEGER_TO_RUNE) return vm_integer_step(run, state);
    if (op->op == XR_XIR_CLEANUP_REGISTER || op->op == XR_XIR_CLEANUP_LEAVE || op->op == XR_XIR_CLEANUP_ERROR)
        return vm_cleanup_step(run, state, op, action);
    if (op->op == XR_XIR_CELL_PLACE || op->op == XR_XIR_SLOT_PLACE || op->op == XR_XIR_OBJECT_PLACE ||
        op->op == XR_XIR_FIELD_PLACE || op->op == XR_XIR_INDEX_PLACE) {
        state->instruction = next;
        return XR_XIR_RUN_OK;
    }
    if (xr_xir_op_uses_value_path(run->function, op)) {
        state->instruction = next;
        return vm_path_step(run, state, op, run->layout->offsets[result_id], action);
    }
    if (op->op == XR_XIR_CLASS_NEW || op->op == XR_XIR_CLASS_GET || op->op == XR_XIR_CLASS_SET) {
        state->instruction = next;
        return vm_class_step(run, state, op, run->layout->offsets[result_id]);
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
    if ((op->op >= XR_XIR_CONST_STRING && op->op <= XR_XIR_ATOMIC_I64_FETCH_ADD) || op->op == XR_XIR_FUNCTION_REF || op->op == XR_XIR_FUNCTION_WEAKEN ||
        op->op == XR_XIR_TO_STRING ||
        (op->op >= XR_XIR_CELL_NEW && op->op <= XR_XIR_CELL_WRITE) || op->op == XR_XIR_CELL_LOCAL_WRITE) {
        state->instruction = next;
        return instance_step(run, state, op, run->layout->offsets[result_id]);
    }
    if (op->op == XR_XIR_JUMP || op->op == XR_XIR_BRANCH) {
        uint32_t edge = op->op == XR_XIR_BRANCH && !xr_xir_scalar_load(run->frame, run->layout->offsets[op->args[0]]) ? 1 : 0;
        XrXirRunStatus status = scalar_edge(run, instruction, op->targets[edge]);
        state->instruction = run->function->blocks[op->targets[edge]].first;
        return status;
    }
    if (op->op == XR_XIR_PHI) { state->instruction = next; return XR_XIR_RUN_OK; }
    if ((op->op >= XR_XIR_NEG_FLOAT && op->op <= XR_XIR_GE_FLOAT) ||
        (op->op >= XR_XIR_ADD_FLOAT && op->op <= XR_XIR_DIV_FLOAT)) {
        XrXirRunStatus status = floating_step(run, op, &value);
        if (status != XR_XIR_RUN_OK) return status;
        xr_xir_scalar_store(run->frame, run->layout->offsets[result_id], value);
        state->instruction = next; return XR_XIR_RUN_OK;
    }
    switch (op->op) {
    case XR_XIR_CONST_RUNE:
    case XR_XIR_CONST_BOOL:
    case XR_XIR_CONST_FLOAT:
    case XR_XIR_CONST_INT:
        value = op->immediate;
        break;
    case XR_XIR_LOCAL_UNINIT:
        if (xr_xir_type_is_owned(run->module->types, op->type))
            xr_xir_owned_slot_clear(run->frame, run->layout->offsets[result_id]);
        state->instruction = next; return XR_XIR_RUN_OK;
    case XR_XIR_SCALAR_LOCAL_NEW:
    case XR_XIR_SCALAR_LOCAL_READ:
    case XR_XIR_SCALAR_COPY:
        value = xr_xir_scalar_load(run->frame, run->layout->offsets[op->args[0]]);
        break;
    case XR_XIR_SCALAR_LOCAL_WRITE:
        xr_xir_scalar_store(run->frame, run->layout->offsets[op->args[0]],
            xr_xir_scalar_load(run->frame, run->layout->offsets[op->args[1]]));
        break;
    case XR_XIR_OWNED_LOCAL_WRITE: {
        XrXirType type = run->function->instructions[op->args[0] - run->function->parameter_count].type;
        XrXirValueStatus status = xr_xir_owned_slot_copy(run->frame, run->layout->offsets[op->args[0]], run->view ? run->view->arena : NULL, type,
            xr_xir_scalar_load(run->frame, run->layout->offsets[op->args[1]]));
        state->instruction = next; return value_run_status(status);
    }
    case XR_XIR_OWNED_LOCAL_NEW:
    case XR_XIR_OWNED_LOCAL_READ:
    case XR_XIR_OWNED_RETAIN:
    case XR_XIR_CONCAT_STRING: {
        int64_t left = xr_xir_scalar_load(run->frame, run->layout->offsets[op->args[0]]);
        XrXirValueStatus status = op->op != XR_XIR_CONCAT_STRING ?
            xr_xir_owned_slot_copy(run->frame, run->layout->offsets[result_id], run->view ? run->view->arena : NULL, op->type, left) :
            xr_xir_string_slot_concat(run->frame, run->layout->offsets[result_id], left,
                xr_xir_scalar_load(run->frame, run->layout->offsets[op->args[1]]));
        state->instruction = next;
        return value_run_status(status);
    }
    case XR_XIR_CALL: case XR_XIR_CALL_INDIRECT: case XR_XIR_INVOKE: case XR_XIR_INVOKE_INDIRECT:
        state->instruction = next;
        return vm_call_step(run, state, op, action, run->layout->offsets[result_id]);
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
    case XR_XIR_NULLABLE_NONE: case XR_XIR_NULLABLE_SOME: {
        if (!run->view) return XR_XIR_RUN_BAD_ARTIFACT;
        state->instruction = next;
        XrXirValue payload = {0}, output = {0};
        if (op->op == XR_XIR_NULLABLE_SOME) payload = vm_value_operand(run, op->args[0]);
        XrXirValueStatus status = xr_xir_nullable_new(op->type,
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
    case XR_XIR_RETURN:
        action->kind = XR_XIR_ACTION_RETURN;
        action->value.type = (uint32_t) run->function->result;
        if (run->function->result != XR_XIR_UNIT)
            action->value.payload = xr_xir_scalar_load(run->frame, run->layout->offsets[op->args[0]]);
        return XR_XIR_RUN_OK;
    default:
        return XR_XIR_RUN_BAD_ARTIFACT;
    }
    if (op->type != XR_XIR_UNIT)
        xr_xir_scalar_store(run->frame, run->layout->offsets[result_id], value);
    state->instruction = next;
    return XR_XIR_RUN_OK;
}

static XrXirAction vm_resume(XrXirCallView *view) {
    const XrXirVmBinding *binding = view->environment;
    const XrXirModule *module = xr_xir_compile_artifact_module(binding->artifact);
    const XrXirFunction *function = &module->functions[binding->function];
    const XrXirFunctionLayout *layout = xr_xir_compile_artifact_layout(binding->artifact, binding->function);
    VmState *state = view->state;
    ScalarRun run = {module, function, layout, state + 1, view};
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
            if (xr_xir_type_is_owned(module->types, function->parameters[i])) {
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
            state->expected = error ? XR_XIR_ERROR : op->type;
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
        if (xr_xir_type_is_owned(module->types, state->expected)) {
            if (xr_xir_owned_slot_copy(run.frame, state->destination, view->arena, state->expected, inbox.payload) != XR_XIR_VALUE_OK)
                return (XrXirAction) {XR_XIR_ACTION_FAULT, 0, NULL, 0, {XR_XIR_I64, 0, XR_XIR_CALL_LIMIT}, {0}, 0};
        } else if (state->destination != UINT32_MAX)
            xr_xir_scalar_store(run.frame, state->destination, inbox.payload);
        }
    }
    uint32_t at = state->instruction;
    XrXirAction action;
    XrXirRunStatus status = scalar_step(&run, state, &action);
    if (status != XR_XIR_RUN_OK)
        action = xr_xir_call_numeric_fault(status, at < function->instruction_count &&
            function->instructions[at].op == XR_XIR_REM_INT);
    if (action.kind == XR_XIR_ACTION_FAULT && at < function->instruction_count) {
        uint32_t handler = function->blocks[vm_block(function, at)].panic;
        if (handler) return vm_panic_land(&run, state, handler, action);
    }
    return action;
}

static void vm_release(XrXirCallView *view, XrXirCallStatus reason) {
    (void) reason;
    const XrXirVmBinding *binding = view->environment;
    const XrXirFunctionLayout *layout = xr_xir_compile_artifact_layout(binding->artifact, binding->function);
    VmState *state = view->state;
    for (uint32_t i = layout->owned_count; i > 0; --i)
        xr_xir_owned_slot_clear(state + 1, layout->owned_offsets[i - 1]);
}

static XrXirStatus bind_verified(const XrXirArtifact *artifact, uint32_t function,
                                 XrXirVmBinding *binding, XrXirCallEntry *entry) {
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
        for (uint32_t i = function + 1; i < module->function_count; ++i) {
            if (!xir_compile_work(context, 1)) return XR_XIR_BUDGET;
            if (module->declarations->functions[i].cleanup_owner == function + 1) result.flags = XR_XIR_ENTRY_EXIT;
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
    if (!module || module->stage != XR_XIR_LOWERED) return XR_XIR_BAD_STAGE;
    if (function >= module->function_count) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status = xr_xir_compile_artifact_verify(artifact, NULL);
    return status == XR_XIR_OK ? bind_verified(artifact, function, binding, entry) : status;
}

typedef struct VmProgramOwner { XrXirArtifact *artifact; XrXirVmBinding *bindings; } VmProgramOwner;
static void vm_program_release(void *pointer) {
    VmProgramOwner *owner = pointer;
    xr_xir_compile_artifact_free(owner->artifact);
    xr_compile_resources_free(owner->bindings);
    xr_compile_resources_free(owner);
}
XR_FUNC XrXirStatus xr_xir_compile_vm_program_take(XrXirArtifact **artifact, XrXirProgram **output) {
    if (!output || *output || !artifact) return XR_XIR_BAD_STRUCTURE;
    const XrXirModule *module = xr_xir_compile_artifact_module(*artifact);
    if (!module || module->stage != XR_XIR_LOWERED) return XR_XIR_BAD_STAGE;
    if (!module->declarations) return XR_XIR_BAD_STRUCTURE;
    const XrXirCompileContext *context = xr_xir_compile_artifact_context(*artifact);
    XrXirStatus status = xr_xir_compile_artifact_verify(*artifact, NULL);
    if (status != XR_XIR_OK) return status;
    VmProgramOwner *owner = xir_compile_calloc(context, 1, sizeof(*owner), &status);
    if (!owner) return status;
    owner->bindings = xir_compile_calloc(context, module->function_count, sizeof(*owner->bindings), &status);
    XrXirCallEntry *entries = xir_compile_calloc(context, module->function_count, sizeof(*entries), &status);
    if (status != XR_XIR_OK) goto finish;
    for (uint32_t i = 0; i < module->function_count; ++i) {
        status = bind_verified(*artifact, i, &owner->bindings[i], &entries[i]);
        if (status != XR_XIR_OK) goto finish;
    }
    XrXirProgramSpec spec = {XR_XIR_PROGRAM_ABI_VERSION, *xr_xir_compile_artifact_target(*artifact),
        entries, module->function_count, module->declarations, {owner, vm_program_release}, module->types,
        xr_xir_compile_program_proof(*artifact)};
    status = xr_xir_compile_program_seal(context, &spec, output);
    if (status == XR_XIR_OK) { owner->artifact = *artifact; *artifact = NULL; }
finish:
    xr_compile_resources_free(entries);
    if (status != XR_XIR_OK) vm_program_release(owner);
    return status;
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
    if (!module || module->stage != XR_XIR_LOWERED || function >= module->function_count)
        return XR_XIR_RUN_BAD_ARTIFACT;
    XrXirStatus verified = xr_xir_compile_artifact_verify(artifact, NULL);
    if (verified != XR_XIR_OK)
        return verified == XR_XIR_OUT_OF_MEMORY ? XR_XIR_RUN_OUT_OF_MEMORY : XR_XIR_RUN_BAD_ARTIFACT;
    if (module->declarations && module->declarations->functions[function].cleanup_owner) return XR_XIR_RUN_BAD_ARTIFACT;
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
        if (body->instructions[i].op >= XR_XIR_CONST_STRING &&
            body->instructions[i].op <= XR_XIR_ATOMIC_I64_FETCH_ADD) return XR_XIR_RUN_BAD_ARTIFACT;
    const XrXirFunctionLayout *layout = xr_xir_compile_artifact_layout(artifact, function);
    void *frame = NULL;
    XrXirRunStatus status = xr_xir_scalar_frame_begin(context, layout->frame_bytes, &frame);
    if (status != XR_XIR_RUN_OK)
        return status;
    for (uint32_t i = 0; i < argument_count; ++i)
        xr_xir_scalar_store(frame, layout->offsets[i], arguments[i].payload);
    ScalarRun run = {module, body, layout, frame, NULL};
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
