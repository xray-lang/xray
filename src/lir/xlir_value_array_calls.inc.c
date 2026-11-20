/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * Bind Array call candidates to the original owners in the shared Frame.
 */

static bool lir_home_array_call_value(const XrLirValueHomes *homes, const XrLirValueHome *value) {
    return value->type && !value->parameter && value->owner_count == 1 && !value->byte_count &&
        value->size == sizeof(XrtManagedHeader *) && lir_home_header_slot(&homes->frame, value->first_owner, value->offset);
}

static bool lir_home_array_call_places(LirHomeBuilder *b, const XrLirHomeCall *call,
    XrLirHomeArrayCall *bound) {
    const XrLirValueHomes *homes = b->homes;
    const XrLirArrayCallMove *move = &bound->logical;
    if (!move->owner || move->owner > homes->value_count || !move->temporary || move->temporary > homes->value_count ||
        !call->result || call->result > homes->value_count || bound->parameter >= call->count ||
        homes->arguments[call->first + bound->parameter] != move->temporary) return false;
    const XrLirValueHome *source = &homes->values[move->owner], *stage = &homes->values[move->temporary];
    const XrLirValueHome *result = &homes->values[call->result];
    if (!lir_home_array_call_value(homes, source) || !lir_home_array_call_value(homes, stage) ||
        !lir_home_array_call_value(homes, result) || source->type != stage->type || source->type != result->type ||
        !lir_home_header_slot(&homes->frame, result->temporary_first_owner, result->temporary_offset)) return false;
    XrtValuePlanSpec spec;
    if (!lir_home_binding_spec(b, source->type, &spec) || spec.kind != XRT_VALUE_ARRAY_I64) return false;
    size_t slots[] = {source->first_owner, stage->first_owner, result->first_owner, result->temporary_first_owner};
    for (size_t n = 1; n < sizeof(slots) / sizeof(*slots); ++n)
        for (size_t earlier = 0; earlier < n; ++earlier) if (slots[n] == slots[earlier]) return false;
    bound->source_slot = slots[0]; bound->staged_slot = slots[1];
    bound->result_slot = slots[2]; bound->landing_slot = slots[3];
    return true;
}

static bool lir_home_array_calls(LirHomeBuilder *b, const XrLirOwnershipPlan *plan) {
    XrLirOwnershipFunction function;
    LirHomeCompiler *c = &b->compiler;
    XrLirValueHomes *homes = b->homes;
    if (xr_lir_ownership_function_at(plan, c->function.id, &function) != XR_LIR_OK)
        return lir_home_fail(c, XR_LIR_VALUE_LAYOUT_ARGUMENT, "Array call lost its ownership function");
    homes->array_entry_owner = SIZE_MAX;
    XrLirArrayEntryRetain entry = function.array_entry_retain;
    if (entry.instruction && entry.result && entry.result <= homes->value_count) {
        const XrLirValueHome *value = &homes->values[entry.result];
        if (lir_home_array_call_value(homes, value)) {
            XrtValuePlanSpec spec;
            if (!lir_home_binding_spec(b, value->type, &spec)) return false;
            if (spec.kind == XRT_VALUE_ARRAY_I64) {
                homes->array_entry_retain = entry;
                homes->array_entry_owner = value->first_owner;
            }
        }
    }
    if (!function.array_call_count) return true;
    XrLirHomeArrayCall *moves = NULL;
    size_t capacity = 0;
    if (!lir_home_grow(c, (void **)&moves, &capacity, function.array_call_count, sizeof(*moves))) return false;
    homes->array_call_moves = moves;
    XrLirHomeCall *calls = b->calls.data;
    size_t position = 0;
    for (uint32_t n = 0; n < function.array_call_count; ++n) {
        XrLirHomeArrayCall bound = {0};
        XrLirArrayCallMove *move = &bound.logical;
        if (xr_lir_ownership_array_call_move_at(plan, function.array_call_offset + n, move) != XR_LIR_OK)
            return lir_home_fail(c, XR_LIR_VALUE_LAYOUT_ARGUMENT, "Array call proof disappeared");
        /* Call projections and logical candidates share the original order. */
        while (position < homes->call_count && (calls[position].block < move->block ||
            (calls[position].block == move->block && calls[position].instruction < move->instruction))) ++position;
        XrLirOwnershipFunction callee;
        if (position == homes->call_count || calls[position].block != move->block ||
            calls[position].instruction != move->instruction || calls[position].callee != move->callee ||
            xr_lir_ownership_function_at(plan, move->callee, &callee) != XR_LIR_OK || !callee.array_entry_retain.instruction)
            return lir_home_fail(c, XR_LIR_VALUE_LAYOUT_ARGUMENT, "Array call lost its receiving position");
        bound.parameter = callee.array_entry_retain.parameter;
        if (lir_home_array_call_places(b, &calls[position], &bound)) {
            moves[homes->array_call_move_count] = bound;
            calls[position].array_move = ++homes->array_call_move_count;
        }
        if (c->status != XR_LIR_VALUE_LAYOUT_OK) return false;
        ++position;
    }
    return true;
}
