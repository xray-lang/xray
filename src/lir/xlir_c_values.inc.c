/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_c_values.inc.c - Receiving plans for ordinary typed C workers
 *
 * KEY CONCEPT:
 *   Compiler resources own temporary physical descriptions. Emitted functions
 *   own values only through the execution's stable Root storage.
 */
static void c_value_allocation_status(LirC *c, XrCompileResourceStatus status) {
    XrCompileResourceStatus expected = XR_COMPILE_RESOURCE_OK;
    if (status != XR_COMPILE_RESOURCE_OK)
        (void)atomic_compare_exchange_strong_explicit(&c->value_allocation_failure, &expected,
            status, memory_order_relaxed, memory_order_relaxed);
}
static void *c_value_allocate(void *context, size_t bytes) {
    LirC *c = context; void *memory = NULL;
    c_value_allocation_status(c, xr_compile_resources_alloc(c->request->resources, bytes, &memory));
    return memory;
}
static void c_value_free(void *context, void *memory, size_t bytes) {
    (void)context; (void)bytes; xr_compile_resources_free(memory);
}
static bool c_value_resources_retain(void *context) {
    LirC *c = context;
    XrCompileResourceStatus status = xr_compile_resources_retain(c->request->resources);
    c_value_allocation_status(c, status);
    return status == XR_COMPILE_RESOURCE_OK;
}
static void c_value_resources_drop(void *context) {
    LirC *c = context; xr_compile_resources_release(c->request->resources);
}
static bool c_value_layout_status(LirC *c, XrLirValueLayoutStatus status) {
    if (!c_resource(c, atomic_load_explicit(&c->value_allocation_failure, memory_order_relaxed))) return false;
    if (status == XR_LIR_VALUE_LAYOUT_OK) return true;
    XrLirCStatus result = status == XR_LIR_VALUE_LAYOUT_OOM ? XR_LIR_C_OUT_OF_MEMORY :
        status == XR_LIR_VALUE_LAYOUT_OVERFLOW ? XR_LIR_C_OVERFLOW :
        status == XR_LIR_VALUE_LAYOUT_LIMIT ? XR_LIR_C_MEMORY_LIMIT :
        status == XR_LIR_VALUE_LAYOUT_UNSUPPORTED ? XR_LIR_C_UNSUPPORTED : XR_LIR_C_INVALID_LIR;
    c_error(c, result, "C value has no receiving physical projection");
    return false;
}
static void c_values_destroy(LirC *c) {
    c_array_regions_destroy(c);
    if (c->value_homes) {
        uint32_t count = xr_lir_function_count(c->request->module);
        for (uint32_t f = 0; f < count; ++f) xr_lir_value_homes_destroy(c->value_homes[f]);
    }
    xr_compile_resources_free(c->value_homes);
    if (c->value_types) {
        xr_compile_resources_free(c->value_types->rows);
        xr_compile_resources_free(c->value_types->index);
        xr_compile_resources_free(c->value_types);
    }
    xr_lir_ownership_plan_destroy(c->ownership);
    xr_lir_state_projection_destroy(c->state_projection);
    xr_lir_value_image_destroy(c->value_image);
    xr_lir_value_layouts_destroy(c->value_layouts);
    xrt_billing_owner_drop(c->value_billing);
}
static bool c_value_mark(LirC *c, uint8_t *needed, size_t count, XrLirType type) {
    if (!type || type >= count) {
        c_error(c, XR_LIR_C_INVALID_LIR, "C value has an invalid physical type identity");
        return false;
    }
    /* Arrays are keyed by their admitted element shape in the separate family
     * bitmap; the bare Array tag cannot name a receiving value layout. */
    if (type==XR_LIR_SET_I64_CELL || type==XR_LIR_SET_I64_REF) type=XR_LIR_SET_I64;
    if ((type >= XR_LIR_UNIT && type <= XR_LIR_F64) || type==XR_LIR_STRING ||
        type==XR_LIR_MAP_STRING_I64 || type==XR_LIR_SET_I64 || type >= XR_LIR_TYPE_COUNT)
        needed[type] = 1;
    return true;
}
static void c_values_array_mark(const XrLirModule *module, uint8_t *needed, size_t types, XrLirType element) {
    uint32_t family = xr_lir_array_layout_family(module, element);
    XR_CHECK(family != UINT32_MAX, "a completed Array has a common layout row");
    needed[types + family] = 1;
}
static void c_values_prepare(LirC *c) {
    if (!c_request_poll(c)) return;
    const XrLirModule *module = c->request->module;
    size_t type_count = (size_t)XR_LIR_TYPE_COUNT + c->constructed_count;
    size_t function_count = xr_lir_function_count(module);
    void *flags = NULL, *memory = NULL;
    size_t arrays = (size_t)XR_LIR_ARRAY_FAMILY_COUNT + module->inline_array_count;
    if (type_count > SIZE_MAX - arrays || function_count > SIZE_MAX - type_count - arrays) {
        c_error(c,XR_LIR_C_OVERFLOW,"C value root inventory overflows"); return;
    }
    if (!c_resource(c, xr_compile_resources_calloc(c->request->resources, type_count + arrays + function_count, 1, &flags))) return;
    uint8_t *needed = flags; bool projected = c->state || c->arrays || c->maps || c->sets;
    uint8_t *environments = needed + type_count + arrays;
    static const XrLirType elements[] = {XR_LIR_I64, XR_LIR_STRING, XR_LIR_U8, XR_LIR_U64};
    for (uint32_t slot = 0; slot < c->state_layout.slot_count && c_request_poll(c); ++slot) {
        XrLirType payload = c->state_layout.slots[slot].payload;
        if (payload == XR_LIR_I64_CELL) {
            c_error(c,XR_LIR_C_UNSUPPORTED,"callable State requires owned code and environment transport"); break;
        }
        if (payload == XR_LIR_ARRAY) c_values_array_mark(module,needed,type_count,c->state_layout.slots[slot].array_element);
        if (!c_scalar(c, payload) || !c_value_mark(c, needed, type_count, payload)) break;
    }
    for (uint32_t i = 0; i < c->closure_count && c_request_poll(c); ++i) {
        XrLirFunction function = {module, c->closure[i]}; XrLirFunctionView view;
        xr_lir_function_view(module, function, &view);
        XrLirCallableFunctionFacts callables = {0};
        if (module->callable_facts && xr_lir_callable_facts_function(module->callable_facts,function.id,&callables) != XR_LIR_OK) {
            c_error(c,XR_LIR_C_INVALID_LIR,"C value roots lost final callable facts"); break;
        }
        for (uint32_t v = 0; callables.values && v < callables.value_count && c_request_poll(c); ++v) {
            const XrLirCallableValueFact *fact = &callables.values[v + 1];
            if (fact->operation != XR_LIR_FUNCTION_REF || fact->transport != XR_LIR_CALLABLE_TRANSPORT_OWNED) continue;
            if (!fact->known_target || fact->known_target > function_count) {
                c_error(c,XR_LIR_C_INVALID_LIR,"owning callable has no exact environment target"); break;
            }
            environments[fact->known_target - 1] = 1;
        }
        if (c->callable_used && c->callable_used[function.id - 1]) {
            XrLirClosureTarget target = {0};
            XR_CHECK(xr_lir_closure_target_view(module,function,&target) == XR_LIR_OK && target.callable,
                "an admitted callable entry keeps its exact target signature");
            projected = true;
            if (!c_value_mark(c,needed,type_count,target.callable)) break;
        }
        projected |= c_managed_leaf(view.result) || view.result >= XR_LIR_TYPE_COUNT;
        if (!c_value_mark(c, needed, type_count, view.result)) break;
        for (uint32_t p = 0; p < view.parameter_count && c_request_poll(c); ++p) {
            projected |= c_managed_leaf(view.parameter_types[p]) || view.parameter_types[p] >= XR_LIR_TYPE_COUNT;
            if (!c_value_mark(c, needed, type_count, view.parameter_types[p])) break;
        }
        for (uint32_t v = 1; v <= view.value_count && c_request_poll(c); ++v) {
            XrLirType type = 0; xr_lir_value_type(module, (XrLirValue){module, function.id, v}, &type);
            if (xr_lir_string_binding(module, (XrLirValue){module, function.id, v})) {
                needed[XR_LIR_STRING] = 1; projected = true;
                c->seen[function.id - 1] |= LIR_C_FUNCTION_STRING_BINDING;
            }
            if (type==XR_LIR_ARRAY || type==XR_LIR_ARRAY_CELL || type==XR_LIR_ARRAY_REF) {
                XrLirArrayShape shape={0};
                if (xr_lir_array_shape_view(module,(XrLirValue){module,function.id,v},&shape)!=XR_LIR_OK) {
                    c_error(c,XR_LIR_C_INVALID_LIR,"Array root lost its admitted element shape"); break;
                }
                c_values_array_mark(module,needed,type_count,shape.element);
            }
            projected |= c_managed_leaf(type) || type >= XR_LIR_TYPE_COUNT;
            if (!c_value_mark(c, needed, type_count, type)) break;
        }
    }
    if (!projected || c->status != XR_LIR_C_OK) { xr_compile_resources_free(flags); return; }
    /* Ownership events cover the complete immutable final module, so its exact
     * Record/Nullable facts include definitions outside the selected closure. */
    bool class_values = false;
    for (uint32_t i = 0; i < c->constructed_count; ++i) {
        XrLirTypeKind kind = c->constructed_types[i].shape.kind;
        if (kind == XR_LIR_TYPE_RECORD || kind == XR_LIR_TYPE_TUPLE || kind == XR_LIR_TYPE_NULLABLE ||
            kind == XR_LIR_TYPE_CLASS) needed[XR_LIR_TYPE_COUNT + i] = 1;
        class_values |= kind == XR_LIR_TYPE_CLASS;
    }
    size_t count = 0;
    for (size_t t = 1; t < type_count + arrays; ++t) count += needed[t] != 0;
    for (size_t f = 0; f < function_count; ++f) count += environments[f] != 0;
    if (!c_resource(c, xr_compile_resources_calloc(c->request->resources, count, sizeof(XrLirValueLayoutKey), &memory))) {
        xr_compile_resources_free(flags); return;
    }
    XrLirValueLayoutKey *roots = memory; size_t next = 0;
    for (size_t t = 1; t < type_count + XR_LIR_ARRAY_FAMILY_COUNT; ++t)
        if (needed[t]) roots[next++] = t < type_count ? (XrLirValueLayoutKey){(XrLirType)t, 0, 0} :
            (XrLirValueLayoutKey){XR_LIR_ARRAY, elements[t-type_count], 0};
    for (uint32_t t = 0; module->inline_array_count && t < module->type_count; ++t) {
        uint32_t ordinal = module->types[t].inline_array_ordinal;
        if (ordinal && needed[type_count + XR_LIR_ARRAY_FAMILY_COUNT + ordinal - 1])
            roots[next++] = (XrLirValueLayoutKey){XR_LIR_ARRAY, XR_LIR_TYPE_COUNT + t, 0};
    }
    bool environment_values = c->cells;
    for (uint32_t f = 0; f < function_count; ++f) if (environments[f]) {
        XrLirClosureTarget target = {0};
        if (xr_lir_closure_target_view(module,(XrLirFunction){module,f + 1},&target) != XR_LIR_OK) {
            c_error(c,XR_LIR_C_INVALID_LIR,"environment root lost its exact signature"); break;
        }
        roots[next++] = (XrLirValueLayoutKey){target.callable,0,f + 1}; environment_values = true;
    }
    XrtManagedAllocator provider = {c, c_value_allocate, c_value_free, {c, c_value_resources_retain, c_value_resources_drop}};
    XrtManagedStatus created = xrt_billing_owner_new(&provider, SIZE_MAX, &c->value_billing);
    (void)c_resource(c, atomic_load_explicit(&c->value_allocation_failure, memory_order_relaxed));
    if (created != XRT_MANAGED_OK) c_error(c, XR_LIR_C_OUT_OF_MEMORY, "C receiving metadata allocation failed");
    if (c_request_poll(c)) c_value_layout_status(c, xr_lir_value_layouts_create(module, roots, count, c->value_billing, &c->value_layouts));
    xr_compile_resources_free(roots); xr_compile_resources_free(flags);
    if (!c_request_poll(c)) return;
    memory = NULL;
    if (!c_resource(c, xr_compile_resources_calloc(c->request->resources, 1, sizeof(*c->value_types), &memory))) return;
    c->value_types = memory; c_value_types_prepare(c, c->value_layouts, c->value_types);
    if ((c->state || environment_values || module->inline_array_count || c->tuple_displays || class_values) && c_request_poll(c)) {
        c_value_layout_status(c, xr_lir_value_image_create(c->value_layouts, c->value_billing, &c->value_image));
        if (c->state && c_request_poll(c)) c_value_layout_status(c, xr_lir_state_projection_create(module, c->value_layouts,
            xr_lir_value_image_view(c->value_image), c->value_billing, &c->state_projection));
    }
    const XrLirOwnershipTypeFact *facts = NULL; uint32_t fact_count = 0;
    if (!c_request_poll(c) || !c_value_layout_status(c, xr_lir_value_layouts_ownership(c->value_layouts, &facts, &fact_count))) return;
    XrLirDiagnostic invalid = {0};
    XrLirStatus status = xr_lir_internal_ownership_collect(&(XrLirOwnershipRequest){module, facts, fact_count}, &c->ownership, &invalid);
    if (status != XR_LIR_OK) {
        c->function.id = invalid.function; c->block = invalid.block; c->instruction = invalid.instruction; c->source = invalid.source;
        c_error(c, status == XR_LIR_OUT_OF_MEMORY ? XR_LIR_C_OUT_OF_MEMORY : XR_LIR_C_INVALID_LIR, invalid.message);
        return;
    }
    memory = NULL;
    if (!c_resource(c, xr_compile_resources_calloc(c->request->resources, xr_lir_function_count(module),
        sizeof(*c->value_homes), &memory))) return;
    c->value_homes = memory;
    for (uint32_t i = 0; i < c->closure_count && c_request_poll(c); ++i) {
        XrLirFunction function = {module, c->closure[i]}; XrLirFunctionView view;
        xr_lir_function_view(module, function, &view);
        if (view.kind != XR_LIR_FUNCTION_DEFINITION) continue;
        c_value_layout_status(c, xr_lir_value_homes_create(&(XrLirValueHomesRequest){module, function,
            c->value_layouts, c->ownership}, &c->value_homes[function.id - 1]));
        c->array_local_runtime |= c_array_local_function(c,c->value_homes[function.id-1]);
    }
    c_array_regions_admit(c);
    for (size_t i = 0; i < c->value_types->count && c_request_poll(c); ++i) {
        XrLirValueLayoutKey key; const XrtValuePlan *plan = NULL;
        xr_lir_value_layouts_at(c->value_layouts, i, &key, &plan);
        if (key.type < XR_LIR_TYPE_COUNT || key.environment_function) continue;
        LirCConstructedType *type = &c->constructed_types[key.type - XR_LIR_TYPE_COUNT];
        XrtValuePlanSpec spec; xrt_value_plan_view(plan, &spec);
        type->layout = (XrLirTypeLayout){spec.size, (uint32_t)spec.alignment, 0, spec.owned_count != 0};
        c->inline_values |= spec.owned_count != 0;
        snprintf(type->name, sizeof(type->name), "XrlValue_%zu", i + 1);
    }
}
static void c_values_emit_types(LirC *c) {
    if (!c->value_types) return;
    for (size_t i = 0; i < c->value_types->count && c_request_poll(c); ++i)
        c_value_type_declaration(c, c->value_types, i);
}
static void c_values_function(LirC *c) {
    c->homes = c->value_homes ? c->value_homes[c->function.id - 1] : NULL;
    c->array_region = c->array_regions ? &c->array_regions[c->function.id - 1] : NULL;
    c->home_edge_cursor = 0;
    c->string_event_cursor = c->string_workspace_cursor = 0;
    c->array_stage_event_cursor = c->array_stage_workspace_cursor = 0;
    c->map_stage_event_cursor = c->map_stage_workspace_cursor = 0;
    c->set_stage_event_cursor = c->set_stage_workspace_cursor = c->set_event_cursor = 0;
    c->view_call_cursor = c->view_event_cursor = 0;
    c->array_frame_block = 0;
    c->array_local_event_cursor = 0;
    c->array_local = c_array_local_function(c,c->homes);
    c->binding_forward_cursor = 0;
    c->binding_forward_pending = NULL;
    c->binding_forwarding = false;
    for (size_t n=0;c->homes && n<c->homes->event_count;++n)
        c->binding_forwarding |= c->homes->events[n].forwarding_mode!=XR_LIR_OWNERSHIP_TRANSFER_NONE;
    c->header_edges = false;
    for (size_t n = 0; c->homes && n < c->homes->edge_count && c_request_poll(c); ++n)
        c->header_edges |= c->homes->edges[n].direct_header_move;
}
static bool c_values_owned_type(LirC *c, XrLirType type) {
    return c_managed_leaf(type) || (type >= XR_LIR_TYPE_COUNT &&
        type - XR_LIR_TYPE_COUNT < c->constructed_count && c->constructed_types[type - XR_LIR_TYPE_COUNT].layout.managed);
}
static void c_values_declarations(LirC *c) {
    if (c_values_owned_type(c, c->view.result) || c_slice_type(c, c->view.result)) c_text(c, "    (void)xrl_result;\n");
    if (!c->homes || !c->homes->owned_frame_needed) return;
    c_text(c, "    XrtManagedRoot *xrl_root = NULL;\n    XrtManagedFrame *xrl_frame = NULL;\n    void *xrl_payload = NULL;\n");
    if (c->header_edges || c->binding_forwarding || c->array_local || c_array_region_present(c) || c_array_push_present(c))
        c_text(c, "    XrtManagedFrameLocations xrl_frame_locations = {0};\n");
    if (c->binding_forwarding) c_text(c, "    bool xrl_binding_forwarded = false;\n    (void)xrl_binding_forwarded;\n");
    if (c->array_local) {
        c_text(c,"    bool xrl_array_ready = false;\n    (void)xrl_array_ready;\n");
        c_format(c,"    XrlArrayLocals_%" PRIu32 " xrl_array_locals = {0};\n",c->function.id);
    }
    c_array_region_declarations(c);
}
static bool c_values_declare(LirC *c, uint32_t value) {
    if (c->homes && c->homes->bindings && (c->homes->bindings[value].kind == XR_LIR_HOME_BINDING_STATE_BORROW ||
        c->homes->bindings[value].kind == XR_LIR_HOME_BINDING_CELL_PAYLOAD_BORROW))
        c_format(c, "    XrtExternalOwner xrl_array_binding%" PRIu32 ";\n    (void)&xrl_array_binding%" PRIu32 ";\n", value, value);
    return c->homes && (c->homes->values[value].owner_count || c->homes->values[value].view_workspace ||
        (c->homes->bindings && c->homes->bindings[value].kind == XR_LIR_HOME_BINDING_LOCAL));
}
static void c_values_ensure(LirC *c, uint32_t source) {
    XR_CHECK(c->homes && c->homes->owned_frame_needed, "owned production has a stable receiving frame");
    if (c->array_frame_block && c->array_frame_block == c->block) return;
    c_text(c, "    if (!xrl_frame) {\n");
    c_format(c, "        xrl_root = xrt_native_managed_root(UINT32_C(%" PRIu32 "));\n", source);
    if (c->header_edges || c->binding_forwarding || c->array_local || c_array_region_present(c) || c_array_push_present(c))
        c_format(c, "        XrtManagedStatus status = xrt_managed_frame_enter_locations(xrl_root, &xrl_frame_%" PRIu32 ", &xrl_frame_locations);\n", c->function.id);
    else c_format(c, "        XrtManagedStatus status = xrt_managed_frame_enter_payload(xrl_root, &xrl_frame_%" PRIu32 ", &xrl_frame, &xrl_payload);\n", c->function.id);
    c_format(c, "        if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status, UINT32_C(%" PRIu32 "));\n", source);
    if (c->header_edges || c->binding_forwarding || c->array_local || c_array_region_present(c) || c_array_push_present(c))
        c_text(c, "        xrl_frame = xrl_frame_locations.frame; xrl_payload = xrl_frame_locations.payload;\n");
    if (c->array_local)
        c_format(c,"        xrl_array_local_prepare_%" PRIu32 "(xrl_frame, xrl_payload, &xrl_frame_locations, &xrl_array_locals);\n",c->function.id);
    if (c->seen[c->function.id - 1] & LIR_C_FUNCTION_STRING_BINDING)
        c_format(c, "        xrt_native_string_binding_prepare(UINT32_C(%" PRIu32 "));\n", source);
    c_text(c, "    }\n");
}
static void c_values_replace_begin(LirC *c, uint32_t value, uint32_t source) {
    const XrLirValueHome *home = &c->homes->values[value];
    XR_CHECK(home->owner_count && !home->parameter && home->temporary_offset != SIZE_MAX,
        "an inline replacement has its own real value and staging owner homes");
    const XrtValuePlanSpec *spec = &c->value_types->rows[home->type - 1].spec;
    c_values_ensure(c, source);
    c_text(c, "    { XrtManagedStatus status = xrt_inline_value_replace(&(XrtInlineValueHome){xrl_frame, ");
    c_format(c, "(void *)((unsigned char *)xrl_payload + %zu), (void *)((unsigned char *)xrl_payload + %zu), %zu, %zu},\n",
        home->offset, home->temporary_offset, home->first_owner, home->temporary_first_owner);
    c_format(c, "        &(XrtInlineValueSpec){%zu, %zu, (const size_t[]){", spec->size, spec->alignment);
    for (size_t i = 0; i < spec->owned_count && c_request_poll(c); ++i) {
        if (i) c_text(c, ", ");
        c_format(c, "%zu", spec->owned_offsets[i]);
    }
    c_format(c, "}, %zu}, ", spec->owned_count);
}
static void c_values_replace_end(LirC *c, uint32_t source) {
    c_text(c, ");\n");
    c_format(c, "      if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status, UINT32_C(%" PRIu32 ")); }\n", source);
}
static uint32_t c_values_scalar_count(LirC *c, const XrLirEdgeView *edge) {
    uint32_t count = 0;
    for (uint32_t a = 0; a < edge->argument_count; ++a)
        count += c->homes->values[edge->arguments[a]].type == 0;
    return count;
}
/* These SSA locations have no language-visible address. Mutable bindings use
 * separate Cell/State storage, so owner retirement cannot observe these phi
 * destinations. Keep every capability, pointer and aggregate on the guarded
 * publication path, including an edge that mixes them with primitives. */
static bool c_values_private_scalars(LirC *c, const XrLirEdgeView *edge) {
    bool found = false;
    for (uint32_t a = 0; a < edge->argument_count; ++a) {
        XrLirType type = c_value_type(c, edge->arguments[a]);
        if (c->homes->values[edge->arguments[a]].type) {
            if (!c_managed_leaf(type)) return false;
            continue;
        }
        switch (type) {
        case XR_LIR_I1: case XR_LIR_I8: case XR_LIR_I16: case XR_LIR_I32: case XR_LIR_I64:
        case XR_LIR_U8: case XR_LIR_U16: case XR_LIR_U32: case XR_LIR_U64: case XR_LIR_F64:
            found = true; break;
        default:
            return false;
        }
    }
    return found;
}
static void c_values_scalar_publication(LirC *c, const XrLirHomeEdge *edge) {
    XrLirBlockView block; XrLirInstructionView term;
    XrLirBlock id = {c->request->module, c->function.id, edge->block};
    xr_lir_block_view(c->request->module, id, &block);
    xr_lir_instruction_view(c->request->module, id, block.instruction_count - 1, &term);
    const XrLirEdgeView *selected = &term.edges[edge->edge - 1];
    if (!c_values_scalar_count(c, selected) || c_values_private_scalars(c, selected)) return;
    c_format(c, "typedef struct XrlScalars_%" PRIu32 "_%" PRIu32 "_%" PRIu32 " {\n", c->function.id, edge->block, edge->edge);
    for (uint32_t a = 0; a < selected->argument_count && c_request_poll(c); ++a)
        if (!c->homes->values[selected->arguments[a]].type) {
            const char *type = c_type(c, c_value_type(c, selected->arguments[a]));
            c_format(c, "    %s *destination_%" PRIu32 "; %s source_%" PRIu32 ";\n", type, a, type, a);
            if (c_value_type(c, selected->arguments[a]) == XR_LIR_STATE_REF)
                c_format(c, "    XrtModuleAccess *destination_access_%" PRIu32 "; XrtModuleAccess source_access_%" PRIu32 ";\n", a, a);
        }
    c_format(c, "} XrlScalars_%" PRIu32 "_%" PRIu32 "_%" PRIu32 ";\n", c->function.id, edge->block, edge->edge);
    c_format(c, "static void xrl_scalars_%" PRIu32 "_%" PRIu32 "_%" PRIu32 "(void *context) {\n", c->function.id, edge->block, edge->edge);
    c_format(c, "    XrlScalars_%" PRIu32 "_%" PRIu32 "_%" PRIu32 " *values = context;\n", c->function.id, edge->block, edge->edge);
    for (uint32_t a = 0; a < selected->argument_count && c_request_poll(c); ++a)
        if (!c->homes->values[selected->arguments[a]].type) {
            c_format(c, "    *values->destination_%" PRIu32 " = values->source_%" PRIu32 ";\n", a, a);
            if (c_value_type(c, selected->arguments[a]) == XR_LIR_STATE_REF)
                c_format(c, "    *values->destination_access_%" PRIu32 " = values->source_access_%" PRIu32 ";\n", a, a);
        }
    c_text(c, "}\n");
}
/* Function entries with an identical managed result use one compatible C
 * parameter type, regardless of the selected worker's function identity. */
static void c_values_landing_types(LirC *c) {
    if (!c->value_types) return;
    for (uint32_t i = 0; i < c->closure_count && c_request_poll(c); ++i) {
        const XrLirValueHomes *homes = c->value_homes[c->closure[i] - 1];
        if (homes && homes->result_type && (c->value_types->rows[homes->result_type - 1].spec.owned_count ||
            c->value_types->rows[homes->result_type - 1].spec.kind == XRT_VALUE_SLICE_I64 ||
            c->value_types->rows[homes->result_type - 1].spec.kind == XRT_VALUE_SLICE_U8))
            c_value_landing_type(c,c->value_types,homes->result_type);
    }
    for (size_t i = 0; i < c->value_types->count && c_request_poll(c); ++i) {
        const XrtValuePlanSpec *spec = &c->value_types->rows[i].spec;
        if (spec->kind != XRT_VALUE_FUNCTION) continue;
        size_t result = c_value_type_row(c->value_types,spec->signature.result);
        XR_CHECK(result < c->value_types->count,"a callable landing has its receiving result plan");
        if (c->value_types->rows[result].spec.owned_count)
            c_value_landing_type(c,c->value_types,(uint32_t)result + 1);
    }
}
static void c_values_helpers(LirC *c) {
    if (!c->value_homes) return;
    for (uint32_t i = 0; i < c->closure_count && c_request_poll(c); ++i) {
        c->function = (XrLirFunction){c->request->module, c->closure[i]};
        xr_lir_function_view(c->request->module, c->function, &c->view);
        c_values_function(c);
        if (!c->homes) continue;
        if (c_values_owned_type(c, c->view.result) || c_slice_type(c, c->view.result)) {
            bool returns = false;
            for (size_t event = 0; event < c->homes->event_count; ++event)
                returns |= c->homes->events[event].kind == XR_LIR_OWNERSHIP_RETURN_TRANSFER;
            if (!returns) c_value_landing(c, c->homes);
        }
        if (c->homes->owned_frame_needed) c_value_frame(c, c->homes);
        c_array_local_specs(c);
        c_array_region_specs(c);
        if (c_values_owned_type(c, c->view.result) || c_slice_type(c, c->view.result)) c_value_returns(c, c->homes);
        if (!c->homes->owned_frame_needed) continue;
        LirCEdgeEvents events = {0}; c_edge_events_prepare(c, c->homes, &events);
        for (size_t e = 0; e < c->homes->edge_count && c_request_poll(c); ++e) {
            const XrLirHomeEdge *edge = &c->homes->edges[e];
            if (c_value_edge_owners(c, c->homes, &events, edge)) {
                c_value_edge(c, c->homes, &events, edge);
                c_values_scalar_publication(c, edge);
            }
        }
        xr_compile_resources_free(events.index);
    }
    c->homes = NULL;
}
static bool c_values_edge(LirC *c, const XrLirEdgeView *edge, uint32_t selected) {
    if (!c->homes) return false;
    const XrLirHomeEdge *home = NULL;
    while (c->home_edge_cursor < c->homes->edge_count) {
        const XrLirHomeEdge *candidate = &c->homes->edges[c->home_edge_cursor];
        if (candidate->block > c->block || (candidate->block == c->block && candidate->edge > selected)) break;
        ++c->home_edge_cursor;
        if (candidate->block == c->block && candidate->edge == selected) { home = candidate; break; }
    }
    if (!home) return false;
    bool owned = false;
    for (size_t a = 0; a < home->count; ++a) {
        const XrLirHomeTransfer *transfer = &c->homes->transfers[home->first + a];
        owned |= c->homes->values[transfer->target].owner_count != 0 || transfer->target_workspace != 0;
    }
    if (!owned) return false;
    size_t region_edge = (size_t)(home - c->homes->edges);
    c_array_region_edge_begin(c,region_edge,edge);
    XrLirBlockView target;
    xr_lir_block_view(c->request->module, (XrLirBlock){c->request->module, c->function.id, edge->target}, &target);
    c_text(c, "    {\n");
    c_values_ensure(c,0);
    uint32_t scalars = c_values_scalar_count(c, edge);
    bool private_scalars = c_values_private_scalars(c, edge);
    if (private_scalars) {
        /* Snapshot all sources before the transaction or any phi assignment. */
        for (uint32_t a = 0; a < edge->argument_count && c_request_poll(c); ++a)
            if (!c->homes->values[edge->arguments[a]].type) {
                c_format(c, "    %s xrl_phi_%" PRIu32 " = ", c_type(c,c_value_type(c,edge->arguments[a])), a);
                c_value(c,edge->arguments[a]); c_text(c,";\n");
            }
    } else if (scalars) {
        c_format(c, "    XrlScalars_%" PRIu32 "_%" PRIu32 "_%" PRIu32 " values = {", c->function.id, c->block, selected);
        bool first = true;
        for (uint32_t a = 0; a < edge->argument_count && c_request_poll(c); ++a)
            if (!c->homes->values[edge->arguments[a]].type) {
                if (!first) c_text(c, ", ");
                first = false;
                c_text(c, "&"); c_value(c, target.parameters[a]); c_text(c, ", "); c_value(c, edge->arguments[a]);
                if (c_value_type(c, edge->arguments[a]) == XR_LIR_STATE_REF)
                    c_format(c, ", &xrl_access%" PRIu32 ", xrl_access%" PRIu32, target.parameters[a], edge->arguments[a]);
            }
        c_text(c, "};\n");
        c_format(c, "    XrtPlacePublication scalar_publication = {xrl_scalars_%" PRIu32 "_%" PRIu32 "_%" PRIu32 ", &values};\n", c->function.id, c->block, selected);
    }
    c_format(c, "    XrtManagedStatus status = xrl_edge_%" PRIu32 "_%" PRIu32 "_%" PRIu32 "(xrl_frame, xrl_payload, %s",
        c->function.id, c->block, selected, scalars && !private_scalars ? "&scalar_publication" : "NULL");
    if (home->direct_header_move) c_text(c, ", &xrl_frame_locations");
    for (size_t a = 0; a < home->count && c_request_poll(c); ++a) {
        XrLirHomeTransfer transfer = c->homes->transfers[home->first + a];
        if (transfer.borrowed_owner_workspace != SIZE_MAX && !transfer.target_workspace) {
            c_text(c,", &"); c_value(c,transfer.source); continue;
        }
        if (c->homes->values[transfer.target].owner_count) continue;
        c_text(c, ", &"); c_value(c, transfer.target); c_text(c, ", &"); c_value(c, transfer.source);
    }
    c_text(c, ");\n    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status, 0);\n");
    if (private_scalars) {
        /* Failure is nonreturning. All old owners have retired successfully;
         * only now publish the private, already snapshotted scalar values. */
        for (uint32_t a = 0; a < edge->argument_count && c_request_poll(c); ++a)
            if (!c->homes->values[edge->arguments[a]].type) {
                c_text(c,"    "); c_value(c,target.parameters[a]);
                c_format(c," = xrl_phi_%" PRIu32 ";\n",a);
            }
    }
    c_array_region_edge_end(c,region_edge);
    c_array_push_edge_end(c,region_edge);
    c_format(c, "    goto bb%" PRIu32 ";\n    }\n", edge->target);
    return true;
}
static bool c_values_signature(LirC *c, bool definition) {
    if (!c_values_owned_type(c, c->view.result) && !c_slice_type(c, c->view.result)) return false;
    c_text(c, "static void "); c_function_name(c, c->function.id);
    c_format(c, "(XrlLanding_%" PRIu32 " xrl_result", c->function.id);
    for (uint32_t p = 0; p < c->view.parameter_count && c_request_poll(c); ++p)
        c_format(c, ", %s p%" PRIu32, c_type(c, c->view.parameter_types[p]), p);
    c_text(c, definition ? ") {\n" : ");\n");
    return true;
}
static void c_values_root(LirC *c) {
    uint32_t entry = c->request->entry.id;
    c->function = c->request->entry;
    c_text(c, c->state ? "static void xrl_invoke(void *context, XrtModuleState *state) {\n" :
        "static void xrl_invoke(void *context) {\n");
    c_text(c,
        "    XrtManagedRoot *root = xrt_native_managed_root(0);\n"
        "    XrtManagedFrame *landing = NULL;\n"
        "    XrtManagedStatus status = xrt_managed_frame_enter(root, 2, &landing);\n"
        "    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status, 0);\n"
        "    XrtManagedSlotLocation destination, temporary;\n"
        "    status = xrt_managed_slot_location(landing, 0, &destination);\n"
        "    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status, 0);\n"
        "    status = xrt_managed_slot_location(landing, 1, &temporary);\n"
        "    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status, 0);\n    ");
    c_function_name(c, entry);
    c_format(c, "((XrlLanding_%" PRIu32 "){landing, 0, 1, destination.owner, temporary.owner}%s);\n", entry,
        c->state ? ", state" : "");
    c_text(c, "    status = xrt_managed_slot_take(landing, 0, (XrtManagedHeader **)context);\n"
        "    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status, 0);\n"
        "    status = xrt_managed_frame_leave(root, landing);\n"
        "    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status, 0);\n}\n"
        "XrtManagedHeader *xray_lir_entry(void) {\n    XrtManagedHeader *result = NULL;\n");
    c_text(c, c->state ?
        "    xrt_run_standalone_state(&xrt_standalone_source_map, xrl_invoke, &result, &xrl_state_spec);\n" :
        "    xrt_run_standalone(&xrt_standalone_source_map, xrl_invoke, &result);\n");
    c_text(c, "    return result;\n}\n");
}
static void c_values_string_literal(LirC *c, const XrLirInstructionView *v, uint32_t source) {
    if (!c->homes || !c->homes->values || !c->homes->owned_frame_needed) {
        c_error(c, XR_LIR_C_INVALID_LIR, "String literal has no common activation cache"); return;
    }
    const XrLirValueHome *home = c_string_home(c, v->result, false);
    if (!home || !c_string_owner_slot(c, home, true)) return;
    const XrtManagedFrameSpec *frame = &c->homes->frame;
    size_t slot = home->literal_cache_owner, offset = home->literal_cache_offset;
    if (slot >= frame->slot_count || slot == home->first_owner || slot == home->temporary_first_owner ||
        offset % _Alignof(XrtManagedHeader *) || offset > frame->payload_size ||
        sizeof(XrtManagedHeader *) > frame->payload_size - offset || frame->owned_offsets[slot] != offset) {
        c_error(c, XR_LIR_C_INVALID_LIR, "String literal cache lost its distinct physical owner slot"); return;
    }
    XrLirLiteral literal = {c->request->module, (uint32_t)v->bits};
    const uint8_t *bytes = NULL; size_t length = 0;
    if (xr_lir_literal_view(c->request->module, literal, &bytes, &length) != XR_LIR_OK) {
        c_error(c, XR_LIR_C_INVALID_LIR, "String literal cache has no admitted byte source"); return;
    }
    c_values_ensure(c, source);
    c_format(c, "    if (!*(XrtManagedHeader **)((unsigned char *)xrl_payload + %zu))\n", offset);
    c_format(c, "        xrt_native_string_create(xrl_frame, %zu, (const char *)xrl_literal%" PRIu64 ", %zu, UINT32_C(%" PRIu32 "));\n",
        slot, v->bits, length, source);
    if (home->literal_cache_borrow) return;
    /* The keeper survives logical SSA consumption. Each executed definition
     * acquires its own reference, so ordinary MOVE and DROP remain exact. */
    c_format(c, "    { XrtManagedStatus status = xrt_place_copy_replace((XrtPlace){.frame=xrl_frame, .slot=%zu, .nulls=XRT_PLACE_NONNULL},\n",
        home->first_owner);
    c_format(c, "        (XrtPlace){.frame=xrl_frame, .slot=%zu, .nulls=XRT_PLACE_NONNULL}, NULL);\n", slot);
    c_format(c, "      if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status, UINT32_C(%" PRIu32 ")); }\n", source);
}
static bool c_values_instruction(LirC *c, const XrLirInstructionView *v, uint32_t source) {
    if (v->op == XR_LIR_CLASS_IS) {
        /* The target class identity is an image row; resolve its one-based id. */
        const XrtValuePlan *plan = NULL; uint32_t target = 0;
        if (xr_lir_value_layouts_lookup(c->value_layouts, (XrLirValueLayoutKey){(XrLirType)v->bits, 0, 0},
            &plan, &target) != XR_LIR_VALUE_LAYOUT_OK) {
            c_error(c, XR_LIR_C_INVALID_LIR, "Class test target has no receiving class identity");
            return true;
        }
        c_format(c, "    v%" PRIu32 " = xrt_native_class_is(", v->result);
        c_value(c, v->operands[0]);
        c_format(c, ", &xrl_value_image, UINT32_C(%" PRIu32 "), UINT32_C(%" PRIu32 "));\n", target, source);
        return true;
    }
    if (v->op == XR_LIR_ASSERT_CONDITION) {
        c_text(c, "    xrt_assert_condition("); c_value(c, v->operands[0]);
        c_text(c, ", (const XrtStringBacking *)"); c_value(c, v->operands[1]);
        c_format(c, ", UINT32_C(%" PRIu32 "));\n", source);
        return true;
    }
    if (v->op == XR_LIR_CALL && c_slice_type(c, v->type)) {
        c_view_call_instruction(c, v, source); return true;
    }
    if (v->op == XR_LIR_CALL && c_values_owned_type(c, v->type)) {
        XrLirFunctionView callee;
        xr_lir_function_view(c->request->module,(XrLirFunction){c->request->module,v->callee},&callee);
        c_values_ensure(c, source);
        const XrLirValueHome *home = &c->homes->values[v->result];
        c_text(c, "    "); c_function_name(c, v->callee);
        c_format(c, "((XrlLanding_%" PRIu32 "){xrl_frame, %zu, %zu, (void *)((unsigned char *)xrl_payload + %zu), (void *)((unsigned char *)xrl_payload + %zu)}",
            v->callee, home->first_owner, home->temporary_first_owner, home->offset, home->temporary_offset);
        for (uint32_t a = 0; a < v->operand_count && c_request_poll(c); ++a) {
            c_text(c, ", "); c_call_argument(c, callee.parameter_types[a], v->operands[a]);
        }
        c_text(c, ");\n");
        return true;
    }
    if (v->op == XR_LIR_RET && (c_values_owned_type(c, c->view.result) || c_slice_type(c, c->view.result))) {
        c_format(c, "    { XrtManagedStatus status = xrl_return_%" PRIu32 "_%" PRIu32 "_%" PRIu32 "(",
            c->function.id, c->block, c->instruction);
        c_text(c,c->homes->owned_frame_needed ? "xrl_root, xrl_frame, " : "NULL, NULL, ");
        if (!c->homes->values[v->operands[0]].owner_count) { c_text(c,"&"); c_value(c,v->operands[0]); }
        else c_text(c,"xrl_payload");
        c_text(c,", xrl_result);\n");
        c_format(c, "      if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status, UINT32_C(%" PRIu32 "));\n      return; }\n", source);
        return true;
    }
    if (v->op == XR_LIR_RET && c->homes && c->homes->owned_frame_needed) {
        c_text(c, "    if (xrl_frame) { XrtManagedStatus status = xrt_managed_frame_leave(xrl_root, xrl_frame);\n");
        c_format(c, "        if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status, UINT32_C(%" PRIu32 ")); }\n", source);
    }
    if (v->op == XR_LIR_STRING_I64) {
        c_values_ensure(c, source);
        c_format(c, "    xrt_native_string_i64_create(xrl_frame, %zu, ",c->homes->values[v->result].temporary_first_owner);
        c_value(c,v->operands[0]); c_format(c, ", UINT32_C(%" PRIu32 "));\n",source);
        c_values_publish_leaf(c,v->result,source);
        return true;
    }
    if (v->op == XR_LIR_STRING_F64) {
        c_values_ensure(c, source);
        c_text(c, "    { uint64_t bits; memcpy(&bits, &"); c_value(c, v->operands[0]); c_text(c, ", sizeof(bits));\n");
        c_format(c, "      xrt_native_string_f64_create(xrl_frame, %zu, bits, UINT32_C(%" PRIu32 ")); }\n",
            c->homes->values[v->result].temporary_first_owner, source);
        c_values_publish_leaf(c, v->result, source); return true;
    }
    if (v->op == XR_LIR_STRING_LITERAL) {
        c_values_string_literal(c, v, source);
        return true;
    }
    if (v->op == XR_LIR_STRING_RETAIN) {
        c_values_ensure(c, source);
        c_text(c, "    { XrtManagedHeader *owner = "); c_value(c, v->operands[0]); c_text(c, ";\n");
        c_format(c, "        if (!xrt_managed_retain(owner)) xrt_native_managed_failure(XRT_MANAGED_REFCOUNT, UINT32_C(%" PRIu32 "));\n", source);
        c_format(c, "        XrtManagedStatus status = xrt_managed_slot_replace(xrl_frame, %zu, &owner);\n", c->homes->values[v->result].first_owner);
        c_text(c, "        if (status != XRT_MANAGED_OK) {\n            xrt_managed_drop(owner);\n");
        c_format(c, "            xrt_native_managed_failure(status, UINT32_C(%" PRIu32 "));\n        }\n    }\n", source);
        return true;
    }
    if (v->op == XR_LIR_STRING_CONCAT) {
        c_values_ensure(c, source);
        c_format(c, "    xrt_native_string_concat_create(xrl_frame, %zu, ", c->homes->values[v->result].first_owner);
        c_value(c, v->operands[0]); c_text(c, ", "); c_value(c, v->operands[1]);
        c_format(c, ", UINT32_C(%" PRIu32 "));\n", source);
        return true;
    }
    if (v->op == XR_LIR_STRING_DROP) {
        if (c->homes->values[v->operands[0]].literal_cache_borrow) return true;
        c_format(c, "    { XrtManagedStatus status = xrt_managed_slot_drop(xrl_frame, %zu);\n", c->homes->values[v->operands[0]].first_owner);
        c_format(c, "      if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status, UINT32_C(%" PRIu32 ")); }\n", source);
        return true;
    }
    if (v->op == XR_LIR_HOST_PATH_BOOL) {
        c_format(c, "    v%" PRIu32 " = xrt_native_host_path_bool(UINT32_C(%" PRIu64 "), (const XrtStringBacking *)",
            v->result, v->bits);
        c_value(c, v->operands[0]);
        c_format(c, ", UINT32_C(%" PRIu32 "));\n", source);
        return true;
    }
    if (v->op == XR_LIR_I64_TRY_PARSE) {
        c_format(c, "    v%" PRIu32 " = (%s){0};\n", v->result, c_type(c, v->type));
        c_format(c, "    v%" PRIu32 ".tag = xrt_native_string_try_parse_i64((const XrtStringBacking *)",
            v->result);
        c_value(c, v->operands[0]);
        c_format(c, ", &v%" PRIu32 ".payload);\n", v->result);
        return true;
    }
    if (v->op == XR_LIR_STRING_EQUAL) {
        c_text(c, "    { const XrtStringBacking *xrl_left = (const XrtStringBacking *)");
        c_value(c, v->operands[0]);
        c_text(c, "; const XrtStringBacking *xrl_right = (const XrtStringBacking *)");
        c_value(c, v->operands[1]);
        c_text(c, ";\n");
        c_format(c, "      v%" PRIu32 " = xrl_left->length == xrl_right->length &&\n"
                    "          (!xrl_left->length || !memcmp(xrl_left->bytes, xrl_right->bytes, xrl_left->length));\n",
                 v->result);
        if (v->predicate == XR_LIR_NE)
            c_format(c, "      v%" PRIu32 " = !v%" PRIu32 ";\n", v->result, v->result);
        c_text(c, "    }\n");
        return true;
    }
    if (v->op == XR_LIR_STRING_PRINT || v->op == XR_LIR_STRING_LEN) {
        c_text(c, "    { const XrtStringBacking *value = (const XrtStringBacking *)");
        c_value(c, v->operands[0]); c_text(c, ";\n");
        if (v->op == XR_LIR_STRING_PRINT)
            c_format(c, "      xrt_print_string(value->bytes, value->length, UINT32_C(%" PRIu32 "));\n", source);
        else c_format(c, "      v%" PRIu32 " = (int64_t)value->runes;\n", v->result);
        c_text(c, "    }\n");
        return true;
    }
    return false;
}
