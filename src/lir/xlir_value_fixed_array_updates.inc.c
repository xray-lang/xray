/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * Bind the common loop route to homes already chosen by the ordinary frame.
 */

static bool lir_fixed_array_value(const XrLirValueHome *home, const XrLirValueHome *owner) {
    return home->type == owner->type && home->stationary_owner == owner->stationary_owner &&
        home->owner_count == 1 && !home->parameter && home->offset == owner->offset &&
        home->first_owner == owner->first_owner;
}

static bool lir_fixed_array_binding(const XrLirValueHomes *homes, uint32_t id, const XrLirValueHome *owner) {
    if (!homes->bindings) return false;
    const XrLirHomeBinding *binding = &homes->bindings[id];
    return binding->kind == XR_LIR_HOME_BINDING_LOCAL && binding->type == owner->type &&
        binding->offset == owner->offset && binding->first_owner == owner->first_owner;
}

static bool lir_fixed_array_step(const XrLirValueHomes *homes, const XrLirArraySetRegionStep *step,
    const XrLirValueHome *owner) {
    if (!step->source || step->source > homes->value_count || !step->target || step->target > homes->value_count)
        return false;
    const XrLirValueHome *from = &homes->values[step->source], *to = &homes->values[step->target];
    switch (step->kind) {
    case XR_LIR_ARRAY_SET_REGION_ENTRY_MOVE: return lir_fixed_array_value(to, owner);
    case XR_LIR_ARRAY_SET_REGION_EXIT_MOVE: return lir_fixed_array_value(from, owner);
    case XR_LIR_ARRAY_SET_REGION_EDGE_MOVE:
        return lir_fixed_array_value(from, owner) && lir_fixed_array_value(to, owner);
    case XR_LIR_ARRAY_SET_REGION_NEW_CANDIDATE:
        return lir_fixed_array_value(from, owner) && lir_fixed_array_binding(homes, step->target, owner);
    case XR_LIR_ARRAY_SET_REGION_READ_CANDIDATE:
        return lir_fixed_array_binding(homes, step->source, owner) && lir_fixed_array_value(to, owner);
    default: return false;
    }
}

static bool lir_fixed_array_project(LirHomeBuilder *b, const XrLirOwnershipPlan *plan,
    const XrLirArraySetRegion *region, const XrLirHomeWorkspace *workspace, XrLirFixedArrayUpdateHome *row) {
    const XrLirValueHomes *homes = b->homes;
    const XrLirValueHome *owner = &homes->values[region->canonical_value];
    if (!owner->type || !owner->stationary_owner || owner->owner_count != 1 || owner->parameter || owner->byte_count ||
        owner->size != sizeof(XrtManagedHeader *) || !lir_home_header_slot(&homes->frame, owner->first_owner, owner->offset))
        return false;
    XrtValuePlanSpec spec;
    if (!lir_home_binding_spec(b, owner->type, &spec) || spec.kind != XRT_VALUE_ARRAY_I64) return false;
    const XrLirFunctionData *function = &b->compiler.input->functions[b->compiler.function.id - 1];
    const XrLirBlockData *body = &function->blocks[region->primary_block - 1];
    if (!region->primary_instruction || region->primary_instruction > body->instruction_count) return false;
    const XrLirInstructionView *operation = &body->instructions[region->primary_instruction - 1].view;
    if (!operation->operand_count || !operation->operands[0] || operation->operands[0] > homes->value_count) return false;
    uint32_t receiver = operation->operands[0];
    size_t workspace_slot = SIZE_MAX;
    if (region->primary_kind == XR_LIR_ARRAY_REGION_PRIMARY_GET) {
        if (!lir_fixed_array_value(&homes->values[receiver], owner)) return false;
    } else if (region->primary_kind == XR_LIR_ARRAY_REGION_PRIMARY_SET ||
        region->primary_kind == XR_LIR_ARRAY_REGION_PRIMARY_PUSH) {
        if (!lir_fixed_array_binding(homes, receiver, owner) || !workspace ||
            workspace->instruction != region->primary_instruction || workspace->type != owner->type || workspace->value ||
            workspace->first_owner == owner->first_owner ||
            !lir_home_header_slot(&homes->frame, workspace->first_owner, workspace->offset)) return false;
        workspace_slot = workspace->first_owner;
    } else return false;
    uint32_t body_edge = 0;
    for (uint32_t n = 0; n < region->step_count; ++n) {
        XrLirArraySetRegionStep step;
        if (xr_lir_ownership_fixed_array_update_step_at(plan, region->first_step + n, &step) != XR_LIR_OK ||
            !lir_fixed_array_step(homes, &step, owner)) return false;
        if (step.kind == XR_LIR_ARRAY_SET_REGION_EDGE_MOVE && step.block == region->header) body_edge = step.edge;
    }
    if (!body_edge || body_edge > 2 || body_edge == region->exit_edge) return false;
    *row = (XrLirFixedArrayUpdateHome){*region, body_edge, owner->first_owner, workspace_slot};
    return true;
}

static bool lir_home_fixed_array_updates(LirHomeBuilder *b, const XrLirOwnershipPlan *plan) {
    LirHomeCompiler *c = &b->compiler;
    XrLirOwnershipFunction function;
    if (xr_lir_ownership_function_at(plan, c->function.id, &function) != XR_LIR_OK)
        return lir_home_fail(c, XR_LIR_VALUE_LAYOUT_ARGUMENT, "fixed Array function is unavailable");
    if (!function.fixed_update_count) return true;
    /* Each proved write block contains one mutation and no other operation that
     * reserves a VALUE workspace. Ambiguous geometry stays on ordinary LIR. */
    size_t *by_block = NULL;
    if (b->homes->workspace_count) {
        by_block = xr_calloc(c->view.block_count, sizeof(*by_block));
        if (!by_block) return lir_home_fail(c, XR_LIR_VALUE_LAYOUT_OOM, "cannot index fixed Array workspaces");
    }
    for (size_t n = 0; n < b->homes->workspace_count; ++n) {
        const XrLirHomeWorkspace *workspace = &b->homes->workspaces[n];
        if (workspace->kind != XR_LIR_HOME_WORKSPACE_VALUE || !workspace->block ||
            workspace->block > c->view.block_count) continue;
        size_t *index = &by_block[workspace->block - 1];
        *index = *index ? SIZE_MAX : n + 1;
    }
    XrLirFixedArrayUpdateHome *rows = NULL;
    XrLirArraySetRegionStep *steps = NULL;
    size_t row_capacity = 0, step_capacity = 0;
    uint32_t row_count = 0, step_count = 0;
    for (uint32_t n = 0; n < function.fixed_update_count; ++n) {
        XrLirArraySetRegion region;
        if (xr_lir_ownership_fixed_array_update_at(plan, function.fixed_update_offset + n, &region) != XR_LIR_OK ||
            !region.primary_block || region.primary_block > c->view.block_count ||
            !region.canonical_value || region.canonical_value > b->homes->value_count ||
            region.step_count > UINT32_MAX - step_count) {
            lir_home_fail(c, XR_LIR_VALUE_LAYOUT_ARGUMENT, "fixed Array route lost its representation");
            break;
        }
        size_t index = by_block ? by_block[region.primary_block - 1] : 0;
        const XrLirHomeWorkspace *workspace = index && index != SIZE_MAX ? &b->homes->workspaces[index - 1] : NULL;
        XrLirFixedArrayUpdateHome row;
        if (!lir_fixed_array_project(b, plan, &region, workspace, &row)) {
            if (c->status != XR_LIR_VALUE_LAYOUT_OK) break;
            continue;
        }
        if (!lir_home_grow(c, (void **)&rows, &row_capacity, (size_t)row_count + 1, sizeof(*rows)) ||
            !lir_home_grow(c, (void **)&steps, &step_capacity, (size_t)step_count + region.step_count, sizeof(*steps))) break;
        row.region.first_step = step_count;
        for (uint32_t s = 0; s < region.step_count; ++s) {
            if (xr_lir_ownership_fixed_array_update_step_at(plan, region.first_step + s, &steps[step_count + s]) != XR_LIR_OK) {
                lir_home_fail(c, XR_LIR_VALUE_LAYOUT_ARGUMENT, "fixed Array route lost its step");
                break;
            }
        }
        if (c->status != XR_LIR_VALUE_LAYOUT_OK) break;
        rows[row_count++] = row;
        step_count += region.step_count;
    }
    xr_free(by_block);
    b->homes->fixed_array_updates = rows; b->homes->fixed_array_update_count = row_count;
    b->homes->fixed_array_update_steps = steps; b->homes->fixed_array_update_step_count = step_count;
    return c->status == XR_LIR_VALUE_LAYOUT_OK;
}
