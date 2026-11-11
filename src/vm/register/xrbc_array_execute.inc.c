/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_array_execute.inc.c - Canonical Array operations on indexed activations
 */
static XrRbcRegister *xr_rbc_array_location(XrRbcStack *stack, const XrRbcFrame *frame,
    XrRbcArrayLocation location) {
    return location.spill ? &stack->spills[frame->spill_base + location.index] :
        &stack->slots[frame->base + location.index];
}

static bool xr_rbc_array_move_pair(const XrRbcFunction *function, XrRbcArrayAccess access, size_t pc) {
    if ((access.operation != XR_RBC_ARRAY_CELL_NEW && access.operation != XR_RBC_ARRAY_CELL_READ) ||
        pc >= function->code_count || (function->code[pc] & 255) != XR_RBC_ARRAY_ACCESS) return false;
    uint32_t index = function->code[pc] >> 16;
    if (index >= function->array_access_count) return false;
    const XrRbcArrayAccess *drop = &function->array_accesses[index];
    XrRbcArrayOperation expected = access.operation == XR_RBC_ARRAY_CELL_NEW ?
        XR_RBC_ARRAY_DROP : XR_RBC_ARRAY_CELL_DROP;
    if (drop->operation != expected || drop->kind != access.kind || drop->count != 1 || access.count != 1)
        return false;
    XrRbcArrayLocation from = function->array_operands[access.first], next = function->array_operands[drop->first];
    return from.index == next.index && from.spill == next.spill;
}

static const XrRbcArrayAccess *xr_rbc_array_at(const XrRbcFunction *function, size_t pc) {
    if (pc >= function->code_count || (function->code[pc] & 255) != XR_RBC_ARRAY_ACCESS) return NULL;
    uint32_t index = function->code[pc] >> 16;
    return index < function->array_access_count ? &function->array_accesses[index] : NULL;
}

static bool xr_rbc_array_move_tail(const XrRbcFunction *function, XrRbcArrayAccess create, size_t pc,
    const XrRbcArrayAccess **mutation, const XrRbcArrayAccess **read) {
    if (pc >= function->code_count || function->code_count - pc < 3) return false;
    const XrRbcArrayAccess *mutate = xr_rbc_array_at(function, pc);
    const XrRbcArrayAccess *observe = xr_rbc_array_at(function, pc + 1);
    const XrRbcArrayAccess *drop = xr_rbc_array_at(function, pc + 2);
    if (!mutate || !observe || !drop || (mutate->operation != XR_RBC_ARRAY_PUSH && mutate->operation != XR_RBC_ARRAY_SET) ||
        observe->operation != XR_RBC_ARRAY_CELL_READ || drop->operation != XR_RBC_ARRAY_CELL_DROP ||
        mutate->kind != create.kind || observe->kind != create.kind || drop->kind != create.kind) return false;
    const XrRbcArrayAccess *uses[] = {mutate, observe, drop};
    for (uint32_t i = 0; i < 3; ++i) {
        XrRbcArrayLocation source = function->array_operands[uses[i]->first];
        if (source.index != create.result.index || source.spill != create.result.spill) return false;
    }
    *mutation = mutate;
    *read = observe;
    return true;
}

static void xr_rbc_array_mutate_owned(XrRbcStack *stack, const XrRbcFrame *frame,
    const XrRbcFunction *function, const XrRbcArrayAccess *access, XrRbcRegister *binding, uint32_t source) {
    const XrRbcArrayLocation *args = function->array_operands + access->first;
    bool push = access->operation == XR_RBC_ARRAY_PUSH;
    XrRbcRegister *value = xr_rbc_array_location(stack, frame, args[push ? 1 : 2]);
    if (push) {
        if (access->kind == XR_RBC_ARRAY_STRING) xrt_array_string_push(&binding->array, value->string, source);
        else xrt_array_i64_push(&binding->array, xr_rbc_signed(value->bits), source);
    } else {
        int64_t index = xr_rbc_signed(xr_rbc_array_location(stack, frame, args[1])->bits);
        if (access->kind == XR_RBC_ARRAY_STRING) xrt_array_string_set(&binding->array, index, value->string, source);
        else xrt_array_i64_set(&binding->array, index, xr_rbc_signed(value->bits), source);
    }
}

static XR_NOINLINE XrRbcStatus xr_rbc_array_step(const XrRbcModule *module, XrRbcStack *stack,
    const XrRbcFrame *frame, const XrRbcFunction *function, uint32_t index, uint32_t source, size_t *pc) {
    XrRbcArrayAccess access = function->array_accesses[index];
    const XrRbcArrayLocation *locations = access.count ? function->array_operands + access.first : NULL;
    XrRbcRegister *result = xr_rbc_array_location(stack, frame, access.result);
    bool string = access.kind == XR_RBC_ARRAY_STRING;
    if (access.operation == XR_RBC_ARRAY_NEW) {
        if (access.count > stack->array_argument_capacity) {
            size_t size = sizeof(XrRbcRegister);
            if (access.count > SIZE_MAX / size) return XR_RBC_OUT_OF_MEMORY;
            void *arguments = xr_malloc((size_t)access.count * size);
            if (!arguments) return XR_RBC_OUT_OF_MEMORY;
            xr_free(stack->array_arguments);
            stack->array_arguments = arguments; stack->array_argument_capacity = access.count;
        }
        for (uint32_t p = 0; p < access.count; ++p) {
            XrRbcRegister *value = xr_rbc_array_location(stack, frame, locations[p]);
            if (string) ((XrtString **)stack->array_arguments)[p] = value->string;
            else ((int64_t *)stack->array_arguments)[p] = xr_rbc_signed(value->bits);
        }
        const void *arguments = access.count ? stack->array_arguments : NULL;
        result->array = string ? xrt_array_string_new(arguments, access.count, source) :
            xrt_array_i64_new(arguments, access.count, source);
        return XR_RBC_OK;
    }
    XrRbcRegister *first = xr_rbc_array_location(stack, frame, locations[0]);
    XrRbcRegister *second = access.count > 1 ? xr_rbc_array_location(stack, frame, locations[1]) : NULL;
    bool move = xr_rbc_array_move_pair(function, access, *pc);
    if (access.operation == XR_RBC_ARRAY_CELL_NEW) {
        XrRbcFrame *active = &stack->frames[stack->depth - 1];
        size_t cell = active->cell_base + access.cell;
        stack->cells[cell].array = move ? first->array : xrt_array_retain(first->array, source);
        if (move) { first->array = NULL; ++*pc; }
        if (active->initialized_cells <= access.cell) active->initialized_cells = access.cell + 1;
        result->bits = (uint64_t)cell + 1;
        const XrRbcArrayAccess *mutation, *read;
        if (move && xr_rbc_array_move_tail(function, access, *pc, &mutation, &read)) {
            /* Keep the real Cell alive at a failing mutation. The final read
             * and DROP transfer its reference only after mutation succeeds. */
            uint32_t mutation_source = function->source_ids ? function->source_ids[*pc] : 0;
            xr_rbc_array_mutate_owned(stack, frame, function, mutation, &stack->cells[cell], mutation_source);
            XrRbcRegister *output = xr_rbc_array_location(stack, frame, read->result);
            output->array = stack->cells[cell].array;
            stack->cells[cell].array = NULL;
            result->bits = 0;
            *pc += 3;
        }
        return XR_RBC_OK;
    }
    bool cell = access.operation == XR_RBC_ARRAY_SET || access.operation == XR_RBC_ARRAY_PUSH ||
        access.operation == XR_RBC_ARRAY_CELL_READ || access.operation == XR_RBC_ARRAY_CELL_WRITE ||
        access.operation == XR_RBC_ARRAY_CELL_DROP;
    XrRbcRegister *binding = cell ? xr_rbc_cell(stack, module, first->bits, access.kind) : first;
    if (!binding) return XR_RBC_INVALID;
    switch (access.operation) {
    case XR_RBC_ARRAY_LEN:
        result->bits = (uint64_t)xrt_array_length(first->array, source); break;
    case XR_RBC_ARRAY_GET:
        if (string) result->string = xrt_array_string_get(first->array, xr_rbc_signed(second->bits), source);
        else result->bits = (uint64_t)xrt_array_i64_get(first->array, xr_rbc_signed(second->bits), source);
        break;
    case XR_RBC_ARRAY_SET: {
        XrRbcRegister *value = xr_rbc_array_location(stack, frame, locations[2]);
        if (string) xrt_array_string_set(&binding->array, xr_rbc_signed(second->bits), value->string, source);
        else xrt_array_i64_set(&binding->array, xr_rbc_signed(second->bits), xr_rbc_signed(value->bits), source);
        break;
    }
    case XR_RBC_ARRAY_PUSH:
        if (string) xrt_array_string_push(&binding->array, second->string, source);
        else xrt_array_i64_push(&binding->array, xr_rbc_signed(second->bits), source);
        break;
    case XR_RBC_ARRAY_RETAIN: case XR_RBC_ARRAY_CELL_READ:
        if (move) {
            result->array = binding->array;
            binding->array = NULL;
            first->bits = 0;
            ++*pc;
        } else result->array = xrt_array_retain(binding->array, source);
        break;
    case XR_RBC_ARRAY_DROP: case XR_RBC_ARRAY_CELL_DROP:
        xrt_array_drop(&binding->array);
        if (cell) first->bits = 0;
        break;
    case XR_RBC_ARRAY_TAKE:
        result->array = first->array; first->array = NULL; break;
    case XR_RBC_ARRAY_CELL_WRITE:
        xrt_array_assign(&binding->array, second->array, source); break;
    default: return XR_RBC_INVALID;
    }
    return XR_RBC_OK;
}
