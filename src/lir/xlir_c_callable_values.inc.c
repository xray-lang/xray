/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_c_callable_values.inc.c - Whole callable publication over real homes
 *
 * KEY CONCEPT:
 *   Final transport events select borrowed bytes or real owner slots. Payload
 *   publication runs inside the owner transaction before old owners retire.
 */
static void c_callable_publication(LirC *c) {
    if (!c->callables) return;
    bool needed = false;
    for (uint32_t f = 0; f < c->closure_count && c_request_poll(c); ++f) {
        const XrLirValueHomes *homes = c->value_homes ? c->value_homes[c->closure[f] - 1] : NULL;
        for (size_t i = 0; homes && i < homes->event_count && c_request_poll(c); ++i) {
            const XrLirHomeEvent *event = &homes->events[i];
            needed |= event->kind == XR_LIR_OWNERSHIP_CALLABLE_VALUE &&
                ((event->operation == XR_LIR_FUNCTION_REF && event->callable_transport == XR_LIR_CALLABLE_TRANSPORT_OWNED) ||
                 (event->operation == XR_LIR_FUNCTION_SCOPE_LEAVE && event->transfer_mode == XR_LIR_OWNERSHIP_TRANSFER_MOVE));
        }
    }
    if (!needed) return;
    c_text(c,"typedef struct XrlFnPublication { XrtFunction *destination, *source; const void *entry; uintptr_t environment; } XrlFnPublication;\n"
        "static void xrl_fn_publish(void *context) {\n"
        "    XrlFnPublication *value = context;\n"
        "    if (value->source && value->source != value->destination) {\n"
        "        value->source->entry = NULL; value->source->environment = 0;\n    }\n"
        "    value->destination->entry = value->entry; value->destination->environment = value->environment;\n}\n");
}
static uint32_t c_callable_environment(LirC *c, uint32_t target) {
    XrLirClosureTarget closure = {0}; const XrtValuePlan *plan = NULL; uint32_t type = 0;
    if (!c->value_layouts || xr_lir_closure_target_view(c->request->module,
        (XrLirFunction){c->request->module,target},&closure) != XR_LIR_OK) return 0;
    if (xr_lir_value_layouts_lookup(c->value_layouts,(XrLirValueLayoutKey){closure.callable,0,target},&plan,&type) != XR_LIR_VALUE_LAYOUT_OK)
        return 0;
    return type;
}
static void c_callable_move(LirC *c, uint32_t destination, uint32_t source, uint32_t source_id, bool constructor) {
    const XrLirValueHome *to = &c->homes->values[destination];
    size_t from = constructor ? to->temporary_first_owner : c->homes->values[source].first_owner;
    if (to->owner_count != 1 || (!constructor && c->homes->values[source].owner_count != 1)) {
        c_error(c,XR_LIR_C_INVALID_LIR,"callable move requires its actual single owner homes"); return;
    }
    c_text(c,"      XrlFnPublication context = {&"); c_value(c,destination); c_text(c,", ");
    if (constructor) c_text(c,"NULL, incoming.entry, incoming.environment};\n");
    else {
        c_text(c,"&"); c_value(c,source); c_text(c,", "); c_value(c,source);
        c_text(c,".entry, "); c_value(c,source); c_text(c,".environment};\n");
    }
    c_text(c,"      XrtPlacePublication publication = {xrl_fn_publish, &context};\n");
    c_format(c,"      XrtManagedStatus status = xrt_place_move_replace((XrtPlace){.frame=xrl_frame, .slot=%zu, .nulls=XRT_PLACE_NULLABLE},\n",to->first_owner);
    c_format(c,"          (XrtPlace){.frame=xrl_frame, .slot=%zu, .nulls=XRT_PLACE_NONNULL}, &publication);\n",from);
    c_format(c,"      if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status, UINT32_C(%" PRIu32 "));\n",source_id);
}
static void c_callable_owned_constructor(LirC *c, const XrLirInstructionView *v, uint32_t source) {
    const XrLirValueHome *home = &c->homes->values[v->result];
    if (home->transport != XR_LIR_CALLABLE_TRANSPORT_OWNED || !home->environment_type || home->owner_count != 1) {
        c_error(c,XR_LIR_C_INVALID_LIR,"owning callable constructor lost its exact environment home"); return;
    }
    c_values_ensure(c,source);
    c_text(c,"    { XrtNativeEnvironmentInput input = {.image=&xrl_value_image, .state=");
    if (c->view.parameter_count && c->view.parameter_types[c->view.parameter_count - 1] == XR_LIR_STATE)
        c_format(c,"p%" PRIu32,c->view.parameter_count - 1);
    else c_text(c,"NULL");
    c_format(c,", .entry=&xrl_entry_%" PRIu32 ", .captures=",v->callee);
    if (v->operand_count) c_format(c,"&xrl_env_%" PRIu32,v->result); else c_text(c,"NULL");
    c_format(c,", .type=UINT32_C(%" PRIu32 ")};\n",home->environment_type);
    c_format(c,"      XrtFunction incoming = xrt_native_environment_create(xrl_frame, %zu, &input, UINT32_C(%" PRIu32 "));\n",
        home->temporary_first_owner,source);
    c_callable_move(c,v->result,0,source,true); c_text(c,"    }\n");
}
static void c_callable_scope_release(LirC *c) {
    if (!c->homes) return;
    size_t at = c->callable_scope_event;
    while (at < c->homes->event_count && c_request_poll(c)) {
        const XrLirHomeEvent *event = &c->homes->events[at++];
        if (event->kind != XR_LIR_OWNERSHIP_CALLABLE_SCOPE_EXIT || event->block != c->block || event->instruction != c->instruction) continue;
        while (at < c->homes->event_count && c_request_poll(c)) {
            const XrLirHomeEvent *release = &c->homes->events[at];
            if (release->kind != XR_LIR_OWNERSHIP_RELEASE || release->block != event->block || release->instruction != event->instruction) break;
            const XrLirValueHome *home = &c->homes->values[release->source];
            if (home->transport != XR_LIR_CALLABLE_TRANSPORT_OWNED || home->owner_count != 1) {
                c_error(c,XR_LIR_C_INVALID_LIR,"callable scope release lost its actual owner home"); return;
            }
            c_text(c,"    if (xrl_frame) {\n      "); c_value(c,release->source); c_text(c,".entry = NULL;\n      ");
            c_value(c,release->source); c_text(c,".environment = 0;\n");
            c_format(c,"      XrtManagedStatus status = xrt_managed_slot_drop(xrl_frame, %zu);\n",home->first_owner);
            c_text(c,"      if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status, 0);\n    }\n"); ++at;
        }
        c->callable_scope_event = at; return;
    }
    c_error(c,XR_LIR_C_INVALID_LIR,"callable scope exit lost its final release position");
}
