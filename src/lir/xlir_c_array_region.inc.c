/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_c_array_region.inc.c - Call-free scalar loops over actual Frame owners
 */
static bool c_array_region_present(const LirC *c) {
    return c->array_region && c->array_region->bound_count != 0;
}
static void c_array_regions_destroy(LirC *c) {
    if (!c->array_regions) return;
    for (uint32_t f = 0; f < xr_lir_function_count(c->request->module); ++f) {
        xr_lir_array_region_bindings_destroy(c->array_regions[f].bindings);
        xr_compile_resources_free(c->array_regions[f].helpers);
        xr_compile_resources_free(c->array_regions[f].scalars);
    }
    xr_compile_resources_free(c->array_regions);
}
static void c_array_regions_admit(LirC *c) {
    bool needed = false;
    for (uint32_t n = 0; n < c->closure_count; ++n) {
        const XrLirValueHomes *homes = c->value_homes[c->closure[n] - 1];
        needed |= homes && homes->array_region_count;
    }
    if (!needed || !c_request_poll(c)) return;
    void *memory = NULL;
    if (!c_resource(c,xr_compile_resources_calloc(c->request->resources,xr_lir_function_count(c->request->module),
        sizeof(*c->array_regions),&memory))) return;
    c->array_regions = memory;
    for (uint32_t n = 0; n < c->closure_count && c_request_poll(c); ++n) {
        uint32_t function = c->closure[n];
        const XrLirValueHomes *homes = c->value_homes[function - 1];
        if (!homes || !homes->array_region_count) continue;
        LirCArrayRegions *receiver = &c->array_regions[function - 1];
        if (!c_value_layout_status(c,xr_lir_array_region_bindings_create(homes,c->value_layouts,&receiver->bindings))) return;
        for (size_t r = 0; r < receiver->bindings->region_count; ++r)
            receiver->bound_count += receiver->bindings->regions[r].slot_count != 0;
        if (!receiver->bound_count) continue;
        c->array_local_runtime = true;
        c_array_helpers_prepare(c,function,receiver);
    }
}
static const XrLirArrayRegionOperation *c_array_region_operation(const LirC *c) {
    if (!c_array_region_present(c)) return NULL;
    const XrLirArrayRegionBindings *binding = c->array_region->bindings;
    if (c->block >= binding->block_index_count) return NULL;
    const XrLirArrayRegionBlock *block = &binding->block_index[c->block];
    if (!block->region || block->region > binding->region_count ||
        !binding->regions[block->region - 1].slot_count || !c->instruction || c->instruction > block->instruction_count)
        return NULL;
    return &binding->operations[block->first_operation + c->instruction - 1];
}
static void c_array_region_clear(LirC *c, size_t region, unsigned phase) {
    c_format(c,"    xrl_ar_%zu_cache = (XrtArrayI64RegionCache){0}; xrl_ar_%zu_phase = %u;\n",region,region,phase);
}
static void c_array_region_specs(LirC *c) {
    if (!c_array_region_present(c)) return;
    const XrLirArrayRegionBindings *binding = c->array_region->bindings;
    for (size_t r = 0; r < binding->region_count && c_request_poll(c); ++r) {
        XrLirArrayRegionBinding row = binding->regions[r];
        if (!row.slot_count) continue;
        XR_DCHECK(row.canonical_slot >= row.first_slot && row.canonical_slot-row.first_slot < row.slot_count,
            "runtime canonical index is relative to this region slot span");
        c_format(c,"static const XrtArrayI64RegionSlot xrl_ar_slots_%" PRIu32 "_%zu[] = {\n",c->function.id,r);
        for (size_t n = 0; n < row.slot_count; ++n) {
            XrtArrayI64RegionSlot slot = binding->slots[row.first_slot + n];
            c_format(c,"    {%zu, %zu},\n",slot.slot,slot.offset);
        }
        c_format(c,"};\nstatic const XrtArrayI64RegionSpec xrl_ar_spec_%" PRIu32 "_%zu = {xrl_ar_slots_%" PRIu32
            "_%zu, %zu, %zu};\n",c->function.id,r,c->function.id,r,row.slot_count,row.canonical_slot-row.first_slot);
    }
}
static void c_array_region_declarations(LirC *c) {
    if (!c_array_region_present(c)) return;
    const XrLirArrayRegionBindings *binding = c->array_region->bindings;
    for (size_t r = 0; r < binding->region_count; ++r) if (binding->regions[r].slot_count) {
        c_format(c,"    XrtArrayI64RegionCache xrl_ar_%zu_cache = {0};\n    unsigned xrl_ar_%zu_phase = 0;\n",r,r);
    }
}
static void c_array_region_slow(LirC *c) {
    const XrLirArrayRegionOperation *operation = c_array_region_operation(c);
    if (!operation || !operation->access) return;
    const XrLirArrayRegionBindings *binding = c->array_region->bindings;
    size_t access = operation->access - 1;
    size_t region = binding->block_index[c->block].region - 1;
    if (c_array_helper_enabled(c,region)) return;
    if (binding->accesses[access].operation != XR_LIR_ARRAY_OP_LEN)
        c_format(c,"xrl_ar_slow_%" PRIu32 "_%" PRIu32 ": ;\n",c->block,c->instruction);
}
static bool c_array_region_coalesced_move(LirC *c, size_t region, size_t ordinal) {
    if (!c->array_helper_active) return false;
    const XrLirArrayRegionBindings *binding = c->array_region->bindings;
    const XrLirArrayRegionBinding row = binding->regions[region];
    if (row.owner_home.kind != XR_LIR_ARRAY_SET_OWNER_HOME_CANONICAL_WINDOW) return false;
    if (ordinal < row.first_move || ordinal - row.first_move >= row.move_count) {
        c_error(c,XR_LIR_C_INVALID_LIR,"Array owner home move lost its common slice"); return true;
    }
    XrLirArraySetRegionOwnerHome fact = row.region.owner_home;
    size_t step = ordinal - row.first_move + 1;
    if (step >= fact.first_active_step && step < fact.exit_step) {
        /* The common producer discharged this exact logical transition and
         * death. Its real owner remains in the canonical Frame field. */
        c_text(c,"    ;\n"); return true;
    }
    if (step != fact.exit_step) {
        c_error(c,XR_LIR_C_INVALID_LIR,"Array owner home move is outside its common window"); return true;
    }
    XrtArrayI64RegionSlot stable = binding->slots[row.owner_home.stable_slot];
    XrtArrayI64RegionSlot exit = binding->slots[row.owner_home.exit_slot];
    XR_DCHECK(stable.slot != exit.slot && stable.offset != exit.offset,"owner exit transfers between real distinct homes");
    /* Read the actual owning field, never manufacture an owner from the
     * borrowed cache. Publish the original exit before any return/observer. */
    c_format(c,"    XrtManagedHeader *xrl_ar_owner = *(XrtManagedHeader **)(xrl_ar_payload + %zu);\n"
        "    *(XrtManagedHeader **)(xrl_ar_payload + %zu) = xrl_ar_owner;\n"
        "    *(XrtManagedHeader **)(xrl_ar_payload + %zu) = NULL;\n",stable.offset,exit.offset,stable.offset);
    return true;
}
static void c_array_region_move(LirC *c, size_t region, size_t ordinal) {
    if (c_array_region_coalesced_move(c,region,ordinal)) return;
    const XrLirArrayRegionBindings *binding = c->array_region->bindings;
    const XrLirArrayRegionMove *move = &binding->moves[ordinal];
    XR_DCHECK(move->source_slot < binding->slot_count && move->target_slot < binding->slot_count,
        "common region move selects actual projected slots");
    XrtArrayI64RegionSlot source = binding->slots[move->source_slot], target = binding->slots[move->target_slot];
    /* The once-acquired window and the common schedule prove these actual
     * fields. The pair cannot allocate, call, fail or observe an intermediate
     * representation. The cache is only a borrow; the Frame owns the result. */
    const char *payload = c->array_helper_active ? "xrl_ar_payload" : "xrl_payload";
    c_format(c,"    *(XrtManagedHeader **)((unsigned char *)%s + %zu) = xrl_ar_%zu_cache.owner;\n"
        "    *(XrtManagedHeader **)((unsigned char *)%s + %zu) = NULL;\n",payload,target.offset,region,payload,source.offset);
}
static void c_array_region_fast_edge(LirC *c, size_t region, size_t ordinal, const XrLirEdgeView *edge) {
    const XrLirArrayRegionBindings *binding = c->array_region->bindings;
    const XrLirArrayRegionMove *move = &binding->moves[ordinal];
    XrLirBlockView target = {0};
    if (xr_lir_block_view(c->request->module,(XrLirBlock){c->request->module,c->function.id,edge->target},&target) != XR_LIR_OK) {
        c_error(c,XR_LIR_C_INVALID_LIR,"Array region edge lost its actual target"); return;
    }
    c_text(c,"    {\n");
    for (uint32_t a = 0; a < edge->argument_count; ++a) if (!c->homes->values[edge->arguments[a]].type) {
        c_format(c,"    %s xrl_ar_phi_%" PRIu32 " = ",c_type(c,c_value_type(c,edge->arguments[a])),a);
        c_value(c,edge->arguments[a]); c_text(c,";\n");
    }
    c_array_region_move(c,region,ordinal);
    for (uint32_t a = 0; a < edge->argument_count; ++a) if (!c->homes->values[edge->arguments[a]].type) {
        c_text(c,"    "); c_value(c,target.parameters[a]); c_format(c," = xrl_ar_phi_%" PRIu32 ";\n",a);
    }
    if (move->step.kind == XR_LIR_ARRAY_SET_REGION_EXIT_MOVE) {
        c_array_region_clear(c,region,0);
        if (c->array_helper_active) c_array_helper_return(c,region);
        else c_format(c,"    goto bb%" PRIu32 ";\n",edge->target);
    } else c_format(c,"    goto xrl_ar_%zu_bb%" PRIu32 ";\n",region,edge->target);
    c_text(c,"    }\n");
}
static void c_array_region_edge_begin(LirC *c, size_t edge, const XrLirEdgeView *selected) {
    if (!c_array_region_present(c)) return;
    const XrLirArrayRegionBindings *binding = c->array_region->bindings;
    if (edge >= binding->edge_count || !binding->edges[edge].region) return;
    XrLirArrayRegionEdge role = binding->edges[edge]; size_t region = role.region - 1;
    const XrLirArrayRegionBinding *row = &binding->regions[region];
    if (!row->slot_count) return;
    if (role.kind == XR_LIR_ARRAY_REGION_EDGE_ENTRY || role.kind == XR_LIR_ARRAY_REGION_EDGE_EXIT) {
        c_array_region_clear(c,region,0); return;
    }
    if (role.kind != XR_LIR_ARRAY_REGION_EDGE_BODY || c->block != row->region.header) return;
    c_format(c,"    if (xrl_ar_%zu_phase == 1) {\n",region);
    c_format(c,"    if (xrt_array_i64_region_try_begin(&(XrtArrayI64RegionContext){xrl_frame, xrl_payload,"
        " &xrl_frame_locations, &xrl_frame_%" PRIu32 "}, &xrl_ar_spec_%" PRIu32 "_%zu, true, &xrl_ar_%zu_cache)) {\n",
        c->function.id,c->function.id,region,region);
    if (c_array_helper_enabled(c,region)) c_array_helper_guard(c,region);
    else {
        c_format(c,"    xrl_ar_%zu_phase = 2;\n",region);
        c_array_region_fast_edge(c,region,role.move-1,selected);
    }
    c_format(c,"    }\n    xrl_ar_%zu_phase = 3;\n    }\n",region);
}
static void c_array_region_edge_end(LirC *c, size_t edge) {
    if (!c_array_region_present(c)) return;
    const XrLirArrayRegionBindings *binding = c->array_region->bindings;
    if (edge >= binding->edge_count || binding->edges[edge].kind != XR_LIR_ARRAY_REGION_EDGE_LATCH ||
        !binding->edges[edge].region) return;
    size_t region = binding->edges[edge].region - 1;
    if (binding->regions[region].slot_count)
        c_format(c,"    if (xrl_ar_%zu_phase == 0) xrl_ar_%zu_phase = 1;\n",region,region);
}
static void c_array_region_access(LirC *c, size_t region, size_t ordinal) {
    const XrLirArrayRegionAccess *access = &c->array_region->bindings->accesses[ordinal];
    if (access->operation == XR_LIR_ARRAY_OP_LEN) {
        c_format(c,"    v%" PRIu32 " = (int64_t)xrl_ar_%zu_cache.length;\n",access->result,region); return;
    }
    if (!c->array_helper_active) {
        c_text(c,"    if ("); c_value(c,access->index);
        c_text(c," < INT64_C(0) || (uint64_t)"); c_value(c,access->index);
        c_format(c," >= xrl_ar_%zu_cache.length) {\n",region);
        c_array_region_clear(c,region,3);
        c_format(c,"    goto xrl_ar_slow_%" PRIu32 "_%" PRIu32 ";\n    }\n",access->block,access->instruction);
    }
    if (access->operation == XR_LIR_ARRAY_OP_GET) {
        c_format(c,"    memcpy(&v%" PRIu32 ", ",access->result);
        if (c->array_helper_active) c_text(c,"xrl_ar_elements");
        else c_format(c,"xrl_ar_%zu_cache.elements",region);
        c_text(c," + (size_t)");
        c_value(c,access->index); c_text(c," * sizeof(int64_t), sizeof(int64_t));\n");
    } else {
        c_text(c,"    memcpy(");
        if (c->array_helper_active) c_text(c,"xrl_ar_elements");
        else c_format(c,"xrl_ar_%zu_cache.elements",region);
        c_text(c," + (size_t)"); c_value(c,access->index);
        c_text(c," * sizeof(int64_t), &"); c_value(c,access->item); c_text(c,", sizeof(int64_t));\n");
    }
}
static void c_array_region_clone(LirC *c, size_t region) {
    const XrLirArrayRegionBindings *binding = c->array_region->bindings;
    const XrLirArrayRegionBinding *row = &binding->regions[region];
    for (size_t n = 0; n < row->block_count && c_request_poll(c); ++n) {
        uint32_t block = binding->blocks[row->first_block + n];
        const XrLirArrayRegionBlock *index = &binding->block_index[block];
        c->block = block;
        c_format(c,"xrl_ar_%zu_bb%" PRIu32 ": ;\n",region,block);
        for (size_t i = 0; i < index->instruction_count && c_request_poll(c); ++i) {
            c->instruction = (uint32_t)i + 1;
            XrLirInstructionView operation = {0};
            if (xr_lir_instruction_view(c->request->module,(XrLirBlock){c->request->module,c->function.id,block},
                (uint32_t)i,&operation) != XR_LIR_OK) {
                c_error(c,XR_LIR_C_INVALID_LIR,"Array region clone lost its original instruction"); return;
            }
            c->source = operation.source;
            XrLirArrayRegionOperation role = binding->operations[index->first_operation + i];
            if (role.access) c_array_region_access(c,region,role.access-1);
            else if (role.move) c_array_region_move(c,region,role.move-1);
            else if (role.drop) c_text(c,"    ;\n");
            else if (operation.op == XR_LIR_BR) {
                size_t edge = index->edges[0];
                if (!edge || edge > binding->edge_count || !binding->edges[edge-1].move) {
                    c_error(c,XR_LIR_C_INVALID_LIR,"Array region branch lost its common MOVE"); return;
                }
                c_array_region_fast_edge(c,region,binding->edges[edge-1].move-1,&operation.edges[0]);
            } else if (operation.op == XR_LIR_CONDBR) {
                size_t moves[2];
                for (uint32_t e = 0; e < 2; ++e) {
                    size_t edge = index->edges[e];
                    if (!edge || edge > binding->edge_count || !binding->edges[edge-1].move) {
                        c_error(c,XR_LIR_C_INVALID_LIR,"Array region header lost its common selected MOVEs"); return;
                    }
                    moves[e] = binding->edges[edge-1].move-1;
                }
                c_text(c,"    if ("); c_value(c,operation.operands[0]); c_text(c,") {\n");
                c_array_region_fast_edge(c,region,moves[0],&operation.edges[0]); c_text(c,"    } else {\n");
                c_array_region_fast_edge(c,region,moves[1],&operation.edges[1]); c_text(c,"    }\n");
            } else {
                uint32_t source = c->location_cursor;
                c_instruction(c,&operation);
                if (source != c->location_cursor) {
                    c_error(c,XR_LIR_C_INVALID_LIR,"Array region scalar changed its original source identity"); return;
                }
            }
        }
    }
}
static void c_array_region_clones(LirC *c) {
    if (!c_array_region_present(c)) return;
    const XrLirArrayRegionBindings *binding = c->array_region->bindings;
    for (size_t r = 0; r < binding->region_count && c_request_poll(c); ++r)
        if (binding->regions[r].slot_count && !c_array_helper_enabled(c,r)) c_array_region_clone(c,r);
}
