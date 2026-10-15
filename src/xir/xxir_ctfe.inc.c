/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_ctfe.inc.c - Bounded compiler execution through the shared scalar step
 *
 * KEY CONCEPT:
 *   Admission inspects actual operations, not promises of effect absence.
 *   The frame and all variable work use the artifact's retained compiler ledger.
 */
static XrXirCtfeStatus ctfe_compile_status(XrXirStatus status) {
    if (status == XR_XIR_OK) return XR_XIR_CTFE_OK;
    if (status == XR_XIR_BUDGET) return XR_XIR_CTFE_BUDGET;
    if (status == XR_XIR_OUT_OF_MEMORY) return XR_XIR_CTFE_OUT_OF_MEMORY;
    return XR_XIR_CTFE_BAD_ARTIFACT;
}
static bool ctfe_scalar_type(XrXirType type) {
    return type == XR_XIR_BOOL || type == XR_XIR_RUNE || xr_xir_type_is_number(type);
}
static bool ctfe_parameter_type(XrXirType type) {
    return ctfe_scalar_type(type) || type == XR_XIR_STRING;
}
static bool ctfe_result_type(XrXirType type) {
    return type == XR_XIR_UNIT || ctfe_parameter_type(type);
}
/* Unit is a result with no scalar slot, never a value argument. */
static bool ctfe_result_value(const XrXirValue *value, XrXirType type, const XrXirTypeArena *arena) {
    return value && (type == XR_XIR_UNIT ?
        (!value->type && !value->reserved && !value->payload) :
        xr_xir_value_argument(value, arena, type));
}

#include "xxir_ctfe_types.inc.c"
#include "xxir_ctfe_owned.inc.c"
#include "xxir_ctfe_float.inc.c"

/* The count describes frame reads/writes in scalar_step; edge transfers charge
 * their actual predecessor scans and copies inside scalar_edge. */
static bool ctfe_error_operation(XrXirOp op) {
    return op==XR_XIR_INVOKE || op==XR_XIR_INVOKE_INDIRECT || op==XR_XIR_INVOKE_RESULT || op==XR_XIR_INVOKE_ERROR ||
        op==XR_XIR_INVOKE_DISCARD || op==XR_XIR_THROW || op==XR_XIR_CLEANUP_ERROR || op==XR_XIR_ERROR_ERASE ||
        op==XR_XIR_ERROR_IS || op==XR_XIR_ERROR_NARROW;
}
static bool ctfe_panic_operation(XrXirOp op) {
    return op==XR_XIR_PANIC_CATCH || op==XR_XIR_PANIC_CODE || op==XR_XIR_PANIC_MESSAGE ||
        op==XR_XIR_ASSERT_CONDITION || op==XR_XIR_MATCH_FAIL;
}
static bool ctfe_opcode_work(XrXirOp op, uint64_t *work) {
    switch (op) {
    case XR_XIR_PANIC_CATCH: case XR_XIR_PANIC_CODE: case XR_XIR_PANIC_MESSAGE:
    case XR_XIR_ASSERT_CONDITION: case XR_XIR_MATCH_FAIL: *work=8;return true;
    case XR_XIR_CONST_FLOAT: case XR_XIR_CONST_RUNE: case XR_XIR_CONST_BOOL: case XR_XIR_CONST_INT: *work = 1; return true;
    case XR_XIR_THROW: case XR_XIR_CLEANUP_ERROR: case XR_XIR_ERROR_ERASE: case XR_XIR_ERROR_IS: case XR_XIR_ERROR_NARROW:
    case XR_XIR_INVOKE_RESULT: case XR_XIR_INVOKE_ERROR: case XR_XIR_INVOKE_DISCARD:
    case XR_XIR_FUNCTION_REF: case XR_XIR_FUNCTION_WEAKEN:
    case XR_XIR_CLEANUP_REGISTER: case XR_XIR_CLEANUP_LEAVE: *work=8;return true;
    case XR_XIR_SCALAR_COPY: case XR_XIR_CONVERT_NUMBER:
    case XR_XIR_RUNE_TO_INTEGER: case XR_XIR_INTEGER_TO_RUNE:
    case XR_XIR_SCALAR_LOCAL_NEW: case XR_XIR_SCALAR_LOCAL_READ:
    case XR_XIR_SCALAR_LOCAL_WRITE: *work = 2; return true;
    case XR_XIR_CELL_PROJECT: case XR_XIR_CELL_NEW: case XR_XIR_CELL_READ: case XR_XIR_CELL_WRITE:
    case XR_XIR_CELL_LOCAL_WRITE: *work = 8; return true;
    case XR_XIR_NULLABLE_NONE: case XR_XIR_NULLABLE_SOME:
    case XR_XIR_NULLABLE_IS_SOME: case XR_XIR_NULLABLE_UNWRAP:
    case XR_XIR_ARRAY_NEW: case XR_XIR_ARRAY_REPEAT: case XR_XIR_ARRAY_GET: case XR_XIR_ARRAY_LEN:
    case XR_XIR_ARRAY_SET: case XR_XIR_ARRAY_PUSH: case XR_XIR_ARRAY_CAPACITY:
    case XR_XIR_ARRAY_WITH_CAPACITY: case XR_XIR_ARRAY_RESERVE: case XR_XIR_CELL_PLACE: case XR_XIR_INDEX_PLACE: case XR_XIR_FIELD_PLACE:
    case XR_XIR_PLACE_READ: case XR_XIR_PLACE_WRITE:
    case XR_XIR_TUPLE_NEW: case XR_XIR_TUPLE_FIELD:
    case XR_XIR_CLASS_NEW: case XR_XIR_CLASS_GET: case XR_XIR_CLASS_SET: case XR_XIR_OBJECT_PLACE:
    case XR_XIR_STRUCT_NEW: case XR_XIR_STRUCT_GET: case XR_XIR_STRUCT_SET:
    case XR_XIR_ENUM_NEW: case XR_XIR_ENUM_TAG: case XR_XIR_ENUM_GET: *work = 8; return true;
    case XR_XIR_CONST_STRING: case XR_XIR_CONCAT_STRING: case XR_XIR_STRING_LEN: case XR_XIR_TO_STRING:
    case XR_XIR_EQ_STRING: case XR_XIR_NE_STRING:
    case XR_XIR_LT_STRING: case XR_XIR_LE_STRING: case XR_XIR_GT_STRING: case XR_XIR_GE_STRING:
    case XR_XIR_STRING_CONTAINS: case XR_XIR_STRING_STARTS_WITH: case XR_XIR_STRING_ENDS_WITH:
    case XR_XIR_STRING_INDEX_OF: case XR_XIR_STRING_LAST_INDEX_OF: *work = 8; return true;
    case XR_XIR_OWNED_LOCAL_NEW: case XR_XIR_OWNED_LOCAL_READ: case XR_XIR_OWNED_RETAIN:
    case XR_XIR_OWNED_LOCAL_WRITE: *work = 2; return true;
    case XR_XIR_LOCAL_UNINIT: *work = 1; return true;
    case XR_XIR_NEG_FLOAT: *work = 2; return true;
    case XR_XIR_EQ_FLOAT: case XR_XIR_NE_FLOAT: case XR_XIR_LT_FLOAT: case XR_XIR_LE_FLOAT:
    case XR_XIR_GT_FLOAT: case XR_XIR_GE_FLOAT: case XR_XIR_ADD_FLOAT: case XR_XIR_SUB_FLOAT:
    case XR_XIR_MUL_FLOAT: case XR_XIR_DIV_FLOAT: *work = 3; return true;
    case XR_XIR_EQUAL: *work = 5; return true;
    case XR_XIR_RANGE_CHECK: *work = 4; return true;
    case XR_XIR_ADD_INT: case XR_XIR_SUB_INT: case XR_XIR_MUL_INT:
    case XR_XIR_DIV_INT: case XR_XIR_REM_INT: case XR_XIR_AND_INT:
    case XR_XIR_OR_INT: case XR_XIR_XOR_INT: case XR_XIR_SHL_INT:
    case XR_XIR_SHR_INT: case XR_XIR_EQ_INT: case XR_XIR_NE_INT:
    case XR_XIR_LT_INT: case XR_XIR_LE_INT: case XR_XIR_GT_INT:
    case XR_XIR_GE_INT: *work = 3; return true;
    case XR_XIR_JUMP: case XR_XIR_PHI: *work = 0; return true;
    case XR_XIR_BRANCH: case XR_XIR_RETURN: *work = 1; return true;
    default: return false;
    }
}
/* Local writes produce Unit but keep ordinary fallthrough control flow. */
static bool ctfe_unit_result(XrXirOp op) {
    return op == XR_XIR_CLASS_SET || op == XR_XIR_ASSERT_CONDITION || op == XR_XIR_MATCH_FAIL || op == XR_XIR_THROW || op == XR_XIR_CLEANUP_ERROR || op == XR_XIR_INVOKE_DISCARD || op == XR_XIR_CLEANUP_REGISTER || op == XR_XIR_CLEANUP_LEAVE || op == XR_XIR_STRUCT_SET || op == XR_XIR_RANGE_CHECK || op == XR_XIR_PLACE_WRITE || op == XR_XIR_ARRAY_SET || op == XR_XIR_ARRAY_PUSH || op == XR_XIR_JUMP || op == XR_XIR_BRANCH || op == XR_XIR_RETURN ||
        op == XR_XIR_SCALAR_LOCAL_WRITE || op == XR_XIR_CELL_WRITE || op == XR_XIR_CELL_LOCAL_WRITE || op == XR_XIR_OWNED_LOCAL_WRITE;
}
static XrXirCtfeStatus ctfe_admit(const XrXirArtifact *artifact, uint32_t selected) {
    const XrXirModule *module = &artifact->module;
    const XrXirCompileContext *context = &artifact->context;
    const XrXirFunction *body = &module->functions[selected];
    if (!xir_compile_work(context, 1)) return XR_XIR_CTFE_BUDGET;
    if (!module->declarations || !module->declarations->functions) return XR_XIR_CTFE_NOT_LEAF;
    const XrXirFunctionIdentity *identity = &module->declarations->functions[selected];
    XrXirCtfeStatus owner=ctfe_nominal_owner_admit(artifact,identity->nominal_owner);
    if (owner!=XR_XIR_CTFE_OK) return owner==XR_XIR_CTFE_NOT_SUPPORTED?XR_XIR_CTFE_NOT_LEAF:owner;
    if (identity->cleanup_owner ||
        (module->generics && (module->generics[selected].parameter_count ||
        module->generics[selected].argument_count)) )
        return XR_XIR_CTFE_NOT_LEAF;
    XrXirCtfeStatus admitted=ctfe_body_type_admit(artifact,body->result,false);
    if (admitted!=XR_XIR_CTFE_OK) return admitted==XR_XIR_CTFE_NOT_SUPPORTED?XR_XIR_CTFE_NOT_LEAF:admitted;
    for (uint32_t m = 0; m < module->declarations->module_count; ++m) {
        if (!xir_compile_work(context, 1)) return XR_XIR_CTFE_BUDGET;
        if (module->declarations->modules[m].initializer == selected) return XR_XIR_CTFE_NOT_LEAF;
    }
    for (uint32_t p = 0; p < body->parameter_count; ++p) {
        if (!xir_compile_work(context, 1)) return XR_XIR_CTFE_BUDGET;
        if (xr_xir_type_is_cell(module->types,body->parameters[p])) return XR_XIR_CTFE_NOT_LEAF;
        admitted=ctfe_body_type_admit(artifact,body->parameters[p],true);
        if (admitted!=XR_XIR_CTFE_OK) return admitted==XR_XIR_CTFE_NOT_SUPPORTED?XR_XIR_CTFE_NOT_LEAF:admitted;
    }
    for (uint32_t b = 0; b < body->block_count; ++b) {
        if (!xir_compile_work(context, 1)) return XR_XIR_CTFE_BUDGET;
        const XrXirBlock *block = &body->blocks[b];
        if (block->panic || block->frontier) return XR_XIR_CTFE_NOT_LEAF;
    }
    for (uint32_t i = 0; i < body->instruction_count; ++i) {
        if (!xir_compile_work(context, 1)) return XR_XIR_CTFE_BUDGET;
        const XrXirInstruction *op = &body->instructions[i]; uint64_t work;
        if (ctfe_class_operation(op->op) || ctfe_error_operation(op->op) || ctfe_panic_operation(op->op) || op->op==XR_XIR_CELL_PROJECT || op->op==XR_XIR_CLEANUP_REGISTER || op->op==XR_XIR_CLEANUP_LEAVE || op->op==XR_XIR_FUNCTION_REF || op->op==XR_XIR_FUNCTION_WEAKEN || op->op==XR_XIR_CALL_INDIRECT ||
            !ctfe_opcode_work(op->op, &work)) return XR_XIR_CTFE_NOT_LEAF;
        admitted=ctfe_tuple_instruction_admit(artifact,body,op);
        if (admitted!=XR_XIR_CTFE_OK) return admitted==XR_XIR_CTFE_NOT_SUPPORTED?XR_XIR_CTFE_NOT_LEAF:admitted;
        bool unit = ctfe_unit_result(op->op) || ((op->op == XR_XIR_CELL_READ || op->op == XR_XIR_TUPLE_FIELD || op->op == XR_XIR_NULLABLE_UNWRAP) && op->type == XR_XIR_UNIT);
        if (!ctfe_storage_instruction(module, body, op) || (unit && op->type != XR_XIR_UNIT) ||
            (!unit && op->type == XR_XIR_UNIT))
            return XR_XIR_CTFE_NOT_LEAF;
        if (op->op == XR_XIR_EQUAL) {
            if (!xir_compile_work(context, 2)) return XR_XIR_CTFE_BUDGET;
            admitted=ctfe_equal_admit(artifact,xr_xir_operand_type(body,op->args[0]));
            if (admitted!=XR_XIR_CTFE_OK) return admitted==XR_XIR_CTFE_NOT_SUPPORTED?XR_XIR_CTFE_NOT_LEAF:admitted;
        }
        if (op->op == XR_XIR_CONVERT_NUMBER &&
            (!xr_xir_type_is_number(op->type) ||
            !xr_xir_type_is_number(xr_xir_operand_type(body, op->args[0])))) return XR_XIR_CTFE_NOT_LEAF;
    }
    return XR_XIR_CTFE_OK;
}
XR_FUNC XrXirCtfeStatus xr_xir_compile_ctfe_leaf(const XrXirCtfeRequest *request,
    XrXirValue *output) {
    if (!request || !output || output->type || output->reserved || output->payload ||
        !request->artifact || (request->argument_count && !request->arguments) ||
        request->limits.steps > XR_XIR_CTFE_MAX_STEPS ||
        request->limits.frame_bytes > XR_XIR_CTFE_MAX_FRAME_BYTES ||
        request->limits.output_bytes > XR_XIR_CTFE_MAX_OUTPUT_BYTES) return XR_XIR_CTFE_BAD_ARGUMENT;
    const XrXirArtifact *artifact = request->artifact;
    XrXirCtfeStatus status = ctfe_compile_status(xr_xir_compile_artifact_verify(artifact, NULL));
    if (status != XR_XIR_CTFE_OK) return status;
    if (artifact->module.stage != XR_XIR_LOWERED || request->function >= artifact->module.function_count)
        return XR_XIR_CTFE_BAD_ARTIFACT;
    if (!xir_compile_work(&artifact->context, 1)) return XR_XIR_CTFE_BUDGET;
    const XrXirTarget *target = xr_xir_compile_artifact_target(artifact);
    if (!target || target->architecture != XR_XIR_ARCH_X86_64 ||
        target->abi_version != XR_XIR_VALUE_ABI_VERSION ||
        request->target.architecture != target->architecture ||
        request->target.abi_version != target->abi_version) return XR_XIR_CTFE_PROFILE;
    for (uint32_t i = 0; i < 32; ++i) {
        if (!xir_compile_work(&artifact->context, 1)) return XR_XIR_CTFE_BUDGET;
        if (request->checked_identity[i] != artifact->checked_identity[i]) return XR_XIR_CTFE_IDENTITY;
    }
    status = ctfe_admit(artifact, request->function);
    if (status != XR_XIR_CTFE_OK) return status;
    const XrXirFunction *body = &artifact->module.functions[request->function];
    status=xr_xir_compile_ctfe_type_admit(&artifact->context,artifact->module.types,body->result);
    if (status!=XR_XIR_CTFE_OK) return status;
    const XrXirFunctionLayout *layout = &artifact->layouts[request->function];
    if (request->argument_count != body->parameter_count) return XR_XIR_CTFE_BAD_ARGUMENT;
    for (uint32_t p = 0; p < body->parameter_count; ++p) {
        if (!xir_compile_work(&artifact->context, 1)) return XR_XIR_CTFE_BUDGET;
        if (!ctfe_parameter_type(body->parameters[p])) return XR_XIR_CTFE_NOT_SUPPORTED;
        if (!xr_xir_value_argument(&request->arguments[p], NULL, body->parameters[p]))
            return XR_XIR_CTFE_BAD_ARGUMENT;
    }
    if (!xr_xir_type_is_owned(artifact->module.types,body->result)) {
        XrXirLayout storage={0};
        status=ctfe_compile_status(xr_xir_compile_layout(&artifact->context,artifact->module.types,
            body->result,target,XR_XIR_LAYOUT_STORAGE,&storage));
        if (status!=XR_XIR_CTFE_OK) return status;
        if (storage.size>request->limits.output_bytes) return XR_XIR_CTFE_OUTPUT_LIMIT;
    }
    /* Target offsets come only from the verified layout. sizeof accounts for
     * physical interpreter bookkeeping, never target type or ABI selection. */
    uint64_t scratch=0;
    status=ctfe_tuple_scratch(artifact,request->function,&scratch);
    if (status!=XR_XIR_CTFE_OK) return status;
    if (scratch>SIZE_MAX-sizeof(VmState)-layout->frame_bytes) return XR_XIR_CTFE_FRAME_LIMIT;
    size_t activation_bytes = sizeof(VmState) + layout->frame_bytes + (size_t)scratch;
    if (activation_bytes > request->limits.frame_bytes) return XR_XIR_CTFE_FRAME_LIMIT;
    CtfeOwnedState owned = {0};
    status = ctfe_owned_new(artifact, layout->owned_count != 0 || body->result == XR_XIR_STRING, &owned);
    if (status != XR_XIR_CTFE_OK) return status;
    if (!xir_compile_work(&artifact->context, (uint64_t)layout->owned_count * 2)) {
        ctfe_owned_free(&owned); return XR_XIR_CTFE_BUDGET;
    }
    void *activation = NULL;
    status = ctfe_compile_status(xir_compile_resource_status(xr_compile_resources_calloc(
        artifact->context.resources, 1, activation_bytes, &activation)));
    if (status != XR_XIR_CTFE_OK) { ctfe_owned_free(&owned); return status; }
    VmState *state = activation; void *frame = state + 1;
    ScalarRun run = {&artifact->module, body, layout, frame, owned.arena, artifact->context.resources};
    owned.frame_spare=request->limits.frame_bytes-activation_bytes;
    if (scratch) {
        if (!scalar_compile_work(&run,sizeof(state->arguments))) { status=XR_XIR_CTFE_BUDGET;goto done; }
        state->arguments=(XrXirValue *)((uint8_t *)frame+layout->frame_bytes);
    }
    for (uint32_t p = 0; p < body->parameter_count; ++p) {
        if (!xir_compile_work(&artifact->context, 1)) { status = XR_XIR_CTFE_BUDGET; goto done; }
        status = ctfe_argument_store(&run, layout->offsets[p], &request->arguments[p], &owned, true);
        if (status != XR_XIR_CTFE_OK) goto done;
    }
    {
        uint64_t remaining = request->limits.steps;
        for (;;) {
            /* Every watchdog observation consumes work, including exhaustion. */
            if (!scalar_compile_work(&run, 1)) { status = XR_XIR_CTFE_BUDGET; break; }
            if (!remaining) { status = XR_XIR_CTFE_STEP_LIMIT; break; }
            --remaining;
            if (state->instruction >= body->instruction_count) { status = XR_XIR_CTFE_BAD_ARTIFACT; break; }
            uint64_t work = 0;
            if (!ctfe_opcode_work(body->instructions[state->instruction].op, &work)) {
                status = XR_XIR_CTFE_NOT_LEAF; break;
            }
            if (!scalar_compile_work(&run, work + 1)) { status = XR_XIR_CTFE_BUDGET; break; }
            status=ctfe_tuple_flow_work(&run,&body->instructions[state->instruction],&owned);
            if (status!=XR_XIR_CTFE_OK) break;
            if (ctfe_float_opcode(&run,&body->instructions[state->instruction])) {
                status=ctfe_float_step(&run,&state->instruction);
                if (status!=XR_XIR_CTFE_OK) break;
                continue;
            }
            if (ctfe_owned_equal_opcode(&run,&body->instructions[state->instruction])) {
                status=ctfe_owned_equal_step(&run,&state->instruction,&owned);
                if (status!=XR_XIR_CTFE_OK) break;continue;
            }
            if (ctfe_nominal_opcode(body->instructions[state->instruction].op)) {
                status=ctfe_nominal_step(&run,state,&owned);if (status!=XR_XIR_CTFE_OK) break;continue;
            }
            if (ctfe_array_opcode(body->instructions[state->instruction].op)) {
                status=ctfe_array_step(&run,state,&owned);if (status!=XR_XIR_CTFE_OK) break;continue;
            }
            if (ctfe_nullable_opcode(&body->instructions[state->instruction])) {
                status=ctfe_nullable_step(&run,&state->instruction,&owned);
                if (status!=XR_XIR_CTFE_OK) break;
                continue;
            }
            if (ctfe_tuple_opcode(body->instructions[state->instruction].op)) {
                status=ctfe_tuple_step(&run,state,&owned);
                if (status!=XR_XIR_CTFE_OK) break;
                continue;
            }
            if (ctfe_cell_opcode(body->instructions[state->instruction].op)) {
                status = ctfe_cell_step(&run, &state->instruction, &owned);
                if (status != XR_XIR_CTFE_OK) break;
                continue;
            }
            if (ctfe_string_opcode(&run, &body->instructions[state->instruction])) {
                status = ctfe_string_step(&run, &state->instruction, &owned);
                if (status != XR_XIR_CTFE_OK) break;
                continue;
            }
            XirScalarCoreAction action; XrXirRunStatus ran = xir_scalar_core_step(&run, &state->instruction, &action);
            if (ran != XR_XIR_RUN_OK) {
                status = ran == XR_XIR_RUN_STEP_LIMIT ? XR_XIR_CTFE_BUDGET :
                    ran == XR_XIR_RUN_DIVIDE_BY_ZERO ? XR_XIR_CTFE_DIVIDE_BY_ZERO :
                    ran == XR_XIR_RUN_NUMERIC_RANGE ? XR_XIR_CTFE_NUMERIC_RANGE : XR_XIR_CTFE_BAD_ARTIFACT;
                break;
            }
            if (action.kind == XIR_SCALAR_RETURN) {
                if (!scalar_compile_work(&run, sizeof(action.value))) { status = XR_XIR_CTFE_BUDGET; break; }
                status = ctfe_result_publish(&run, &action.value, body->result, request->limits.output_bytes, &owned, output); break;
            }
            if (action.kind != XIR_SCALAR_CONTINUE) { status = XR_XIR_CTFE_BAD_ARTIFACT; break; }
        }
    }
done:
    {
        ScalarRun cleanup = {&artifact->module, body, layout, frame, owned.arena, artifact->context.resources};
        ctfe_owned_frame_clear(&cleanup);
    }
    xr_compile_resources_free(activation);ctfe_owned_free(&owned);
    return status;
}

#include "xxir_ctfe_calls.inc.c"
