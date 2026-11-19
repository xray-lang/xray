/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_c_callable.inc.c - Typed native entries over borrowed stack environments
 *
 * KEY CONCEPT:
 *   One entry union contains real compatible function pointers. The public
 *   callable carries that object's address, never a converted code pointer.
 *   Constructor scope checks protect every borrowed capture through the call.
 */
static bool c_callable_type(LirC *c, XrLirType type, XrLirCallableType *output) {
    XrLirCallableType view;
    if (xr_lir_callable_type_view(c->request->module, type, &view) != XR_LIR_OK) return false;
    if (output) *output = view;
    return true;
}
static bool c_callable_fact(LirC *c, uint32_t value, XrLirCallableValueFact *output) {
    XrLirCallableFunctionFacts facts = {0};
    if (xr_lir_callable_facts_function(c->request->module->callable_facts,c->function.id,&facts) != XR_LIR_OK ||
        !value || value > facts.value_count || !facts.values || !facts.values[value].type) {
        c_error(c,XR_LIR_C_INVALID_LIR,"callable value lost its final constructor fact"); return false;
    }
    *output = facts.values[value];
    return true;
}
static void c_callable_events_prepare(LirC *c) {
    xr_compile_resources_free(c->callable_events); c->callable_events = NULL; c->callable_scope_event = 0;
    if (!c->homes || !c->callables) return;
    bool found = false;
    for (size_t i = 0; i < c->homes->event_count && c_request_poll(c); ++i)
        found |= c->homes->events[i].kind == XR_LIR_OWNERSHIP_CALLABLE_VALUE;
    if (!found || !c_request_poll(c)) return;
    void *memory = NULL;
    if (!c_resource(c,xr_compile_resources_calloc(c->request->resources,(size_t)c->view.value_count + 1,
        sizeof(*c->callable_events),&memory))) return;
    c->callable_events = memory;
    for (size_t i = 0; i < c->homes->event_count && c_request_poll(c); ++i) {
        const XrLirHomeEvent *event = &c->homes->events[i];
        if (event->kind != XR_LIR_OWNERSHIP_CALLABLE_VALUE) continue;
        if (!event->source || event->source > c->view.value_count || c->callable_events[event->source]) {
            c_error(c,XR_LIR_C_INVALID_LIR,"callable value has no unique final event"); return;
        }
        ((const XrLirHomeEvent **)c->callable_events)[event->source] = event;
    }
}
static const XrLirHomeEvent *c_callable_event(LirC *c, uint32_t value) {
    const XrLirHomeEvent *event = c->callable_events && value <= c->view.value_count ? c->callable_events[value] : NULL;
    if (!event) c_error(c,XR_LIR_C_INVALID_LIR,"callable production has no final transport event");
    return event;
}
static bool c_callable_point(LirC *c, const XrLirInstructionView *v) {
    return v->op == XR_LIR_FUNCTION_REF || v->op == XR_LIR_CALL_INDIRECT ||
        v->op == XR_LIR_FUNCTION_WEAKEN || v->op == XR_LIR_FUNCTION_SCOPE_ENTER ||
        v->op == XR_LIR_FUNCTION_SCOPE_LEAVE || v->op == XR_LIR_FUNCTION_BIND ||
        (v->op == XR_LIR_CELL_NEW && v->type == XR_LIR_I64_CELL) ||
        ((v->op == XR_LIR_CELL_READ || v->op == XR_LIR_CELL_WRITE) &&
         c_value_type(c, v->operands[0]) == XR_LIR_I64_CELL);
}
static const char *c_callable_parameter_type(LirC *c, XrLirCallableParameter parameter) {
    if (parameter.mode != XR_PARAM_REF) return c_type(c, parameter.type);
    if (parameter.type == XR_LIR_I64) return "int64_t *";
    if (parameter.type == XR_LIR_ARRAY || parameter.type == XR_LIR_MAP_STRING_I64 || parameter.type == XR_LIR_SET_I64) return "XrtPlace";
    return NULL;
}
static bool c_callable_signature_admit(LirC *c, XrLirCallableType signature) {
    if (!c_type(c,signature.result) || signature.result == XR_LIR_I64_CELL) {
        c_error(c,XR_LIR_C_UNSUPPORTED,"callable result has no established native landing ABI"); return false;
    }
    for (uint32_t p = 0; p < signature.parameter_count; ++p) {
        XrLirCallableParameter parameter = signature.parameters[p];
        if (parameter.mode == XR_PARAM_MOVE &&
            (c_managed_leaf(parameter.type) || xr_lir_boxed_type(c->request->module,parameter.type) ||
             c_callable_type(c,parameter.type,NULL))) {
            c_error(c,XR_LIR_C_UNSUPPORTED,"owned callable parameters require lifetime transport"); return false;
        }
        if (!c_callable_parameter_type(c,parameter)) {
            c_error(c,XR_LIR_C_UNSUPPORTED,"callable parameter has no established native binding"); return false;
        }
    }
    return true;
}
static uint32_t c_callable_result_type(LirC *c, XrLirCallableType signature) {
    const XrtValuePlan *plan = NULL; uint32_t type = 0;
    XrLirValueLayoutKey key = {signature.result,signature.result == XR_LIR_ARRAY ? signature.result_element : 0,0};
    if (xr_lir_value_layouts_lookup(c->value_layouts,key,&plan,&type) != XR_LIR_VALUE_LAYOUT_OK)
        c_error(c,XR_LIR_C_INVALID_LIR,"callable result lost its common landing plan");
    return type;
}
static bool c_callable_admit(LirC *c, const XrLirInstructionView *v) {
    c->callables = true;
    if (v->op == XR_LIR_FUNCTION_REF) {
        XrLirFunctionView target;
        xr_lir_function_view(c->request->module,(XrLirFunction){c->request->module,v->callee},&target);
        XrLirCallableValueFact fact;
        if (!c_callable_fact(c,v->result,&fact)) return false;
        if (fact.kind != XR_LIR_SCOPED_VALUE_FUNCTION || fact.type != v->type ||
            fact.operation != v->op || fact.constructor_storage != v->bits || fact.known_target != v->callee ||
            fact.block != c->block || fact.instruction != c->instruction || fact.capture_count != v->operand_count) {
            c_error(c,XR_LIR_C_INVALID_LIR,"callable constructor changed after its final facts were sealed"); return false;
        }
        bool owned = fact.transport == XR_LIR_CALLABLE_TRANSPORT_OWNED;
        bool assertion = target.kind == XR_LIR_FUNCTION_EXTERNAL && target.extern_kind == XR_LIR_EXTERN_ASSERT_CONDITION;
        if ((fact.transport != XR_LIR_CALLABLE_TRANSPORT_FRAME && !owned) ||
            (target.kind != XR_LIR_FUNCTION_DEFINITION && !assertion)) {
            c_error(c, XR_LIR_C_UNSUPPORTED, "escaping callable environments require owned lifetime transport");
            return false;
        }
        XrLirClosureTarget closure;
        XrLirCallableType signature;
        if (xr_lir_closure_target_view(c->request->module,(XrLirFunction){c->request->module,v->callee},&closure) != XR_LIR_OK ||
            !c_callable_type(c,closure.callable,&signature)) {
            c_error(c,XR_LIR_C_INVALID_LIR,"callable reference has no verified target signature"); return false;
        }
        if (!c_callable_signature_admit(c,signature)) return false;
        if (owned && closure.state_capture) {
            c_error(c,XR_LIR_C_UNSUPPORTED,"escaping State captures require an admitted acyclic lifetime"); return false;
        }
        c->runtime |= owned;
        if (v->operand_count) {
            if (!owned && fact.constructor_may_repeat) {
                c_error(c,XR_LIR_C_UNSUPPORTED,"reentered Frame environments require independent capture snapshots"); return false;
            }
            for (uint32_t p = 0; p < v->operand_count; ++p) {
                XrLirType type = c_value_type(c,v->operands[p]); XrLirTypeLayout layout;
                if (owned) {
                    bool scalar = type == XR_LIR_I1 || type == XR_LIR_I64 || type == XR_LIR_U8 || type == XR_LIR_U64 || type == XR_LIR_F64;
                    bool callable = c_callable_type(c,type,NULL);
                    if (callable) {
                        XrLirCallableValueFact capture;
                        if (!c_callable_fact(c,v->operands[p],&capture)) return false;
                        if (capture.transport != XR_LIR_CALLABLE_TRANSPORT_OWNED && capture.transport != XR_LIR_CALLABLE_TRANSPORT_BORROW) {
                            c_error(c,XR_LIR_C_UNSUPPORTED,"escaping callable capture has no owning lifetime"); return false;
                        }
                    }
                    if (!scalar && type != XR_LIR_MAP_STRING_I64 && !callable && !c_cell_type(c,type,NULL)) {
                        c_error(c,XR_LIR_C_UNSUPPORTED,"escaping capture has no admitted environment value plan"); return false;
                    }
                    continue;
                }
                if (type == XR_LIR_I64_CELL || type == XR_LIR_STATE || type == XR_LIR_STATE_REF ||
                    c_callable_type(c,type,NULL) || c_cell_type(c,type,NULL)) continue;
                /* The emitted Frame field uses its common natural layout.
                 * With no owner leaves it needs only a value snapshot. */
                if (xr_lir_inline_aggregate_type(c->request->module,type) &&
                    !xr_lir_boxed_type(c->request->module,type)) continue;
                if (xr_lir_type_layout(c->request->module,type,&layout) != XR_LIR_OK || layout.managed) {
                    c_error(c,XR_LIR_C_UNSUPPORTED,"Frame capture requires an established borrowed payload lifetime"); return false;
                }
            }
        }
        if (!c->callable_used) {
            void *memory = NULL;
            if (!c_resource(c,xr_compile_resources_calloc(c->request->resources,
                xr_lir_function_count(c->request->module),1,&memory))) return false;
            c->callable_used = memory;
        }
        c->callable_used[v->callee - 1] = 1;
        c_visit(c, v->callee);
    } else if (v->op == XR_LIR_CALL_INDIRECT) {
        XrLirCallableType signature;
        if (!c_callable_type(c,c_value_type(c,v->operands[0]),&signature)) {
            c_error(c,XR_LIR_C_INVALID_LIR,"indirect call has no verified value signature"); return false;
        }
        if (!c_callable_signature_admit(c,signature)) return false;
    }
    return c_request_poll(c);
}
static void c_callable_parameters(LirC *c, XrLirCallableType signature, bool names) {
    for (uint32_t p = 0; p < signature.parameter_count && c_request_poll(c); ++p) {
        const char *type = c_callable_parameter_type(c, signature.parameters[p]);
        if (!type) { c_error(c, XR_LIR_C_UNSUPPORTED, "callable parameter has no native type"); return; }
        c_format(c, ", %s", type);
        if (names) c_format(c, " a%" PRIu32, p);
    }
    if (xr_lir_callable_invocation_source(&signature))
        c_text(c, names ? ", uint32_t invocation_source" : ", uint32_t");
}
#include "xlir_c_callable_values.inc.c"
static void c_callable_types(LirC *c) {
    if (!c->callables) return;
    bool opened = false;
    for (uint32_t t = 0; t < c->constructed_count && c_request_poll(c); ++t) {
        if (c->constructed_types[t].shape.kind != XR_LIR_TYPE_CALLABLE || !c->constructed_types[t].layout.size) continue;
        XrLirCallableType signature;
        XR_CHECK(c_callable_type(c, XR_LIR_TYPE_COUNT + t, &signature), "receiving callable has its exact signature");
        if (!c_callable_signature_admit(c,signature)) return;
        if (!opened) { c_text(c, "typedef union XrlFnEntry {\n"); opened = true; }
        bool owned = c_values_owned_type(c,signature.result);
        c_format(c, "    %s (*s%" PRIu32 ")(uintptr_t", owned ? "void" : c_type(c, signature.result), XR_LIR_TYPE_COUNT + t);
        if (owned) c_format(c,", XrlLandingValue_%" PRIu32,c_callable_result_type(c,signature));
        c_callable_parameters(c, signature, false); c_text(c, ");\n");
    }
    if (opened) c_text(c, "} XrlFnEntry;\n");
    for (uint32_t n = 0; n < c->closure_count && c_request_poll(c); ++n) {
        uint32_t id = c->closure[n];
        if (!c->callable_used || !c->callable_used[id - 1]) continue;
        XrLirFunctionView target;
        xr_lir_function_view(c->request->module, (XrLirFunction){c->request->module,id}, &target);
        XrLirClosureTarget closure = {0};
        if (xr_lir_closure_target_view(c->request->module, (XrLirFunction){c->request->module,id}, &closure) != XR_LIR_OK ||
            !closure.callable || (!closure.capture_count && !closure.state_capture)) continue;
        uint32_t environment = c_callable_environment(c,id);
        if (environment) {
            c_format(c,"typedef XrlValue_%" PRIu32 " XrlFnEnv_%" PRIu32 ";\n",environment,id);
            continue;
        }
        c_format(c, "typedef struct XrlFnEnv_%" PRIu32 " {\n", id);
        uint32_t count = closure.capture_count + closure.state_capture;
        for (uint32_t p = 0; p < count; ++p) {
            uint32_t actual = p < closure.capture_count ? p : target.parameter_count - 1;
            c_format(c, "    %s c%" PRIu32 ";\n", c_type(c,target.parameter_types[actual]), p);
        }
        c_format(c, "} XrlFnEnv_%" PRIu32 ";\n", id);
    }
}
static void c_callable_entries(LirC *c) {
    if (!c->callables) return;
    for (uint32_t n = 0; n < c->closure_count && c_request_poll(c); ++n) {
        XrLirFunction function = {c->request->module,c->closure[n]};
        if (!c->callable_used || !c->callable_used[function.id - 1]) continue;
        XrLirFunctionView target; XrLirClosureTarget closure = {0};
        xr_lir_function_view(c->request->module, function, &target);
        if (xr_lir_closure_target_view(c->request->module, function, &closure) != XR_LIR_OK || !closure.callable) continue;
        XrLirCallableType signature;
        XR_CHECK(c_callable_type(c,closure.callable,&signature), "closure target carries an admitted signature");
        bool owned = c_values_owned_type(c,signature.result);
        c_format(c, "static %s xrl_thunk_%" PRIu32 "(uintptr_t context", owned ? "void" : c_type(c,signature.result), function.id);
        if (owned) c_format(c,", XrlLandingValue_%" PRIu32 " landing",c_callable_result_type(c,signature));
        c_callable_parameters(c,signature,true); c_text(c, ") {\n    (void)context;\n");
        if (target.kind == XR_LIR_FUNCTION_EXTERNAL && target.extern_kind == XR_LIR_EXTERN_ASSERT_CONDITION) {
            c_text(c, "    xrt_assert_condition(a0, (const XrtStringBacking *)a1, invocation_source);\n}\n");
            c_format(c,"static const XrlFnEntry xrl_entry_%" PRIu32 " = {.s%" PRIu32 "=xrl_thunk_%" PRIu32 "};\n",
                function.id,closure.callable,function.id);
            continue;
        }
        if (xr_lir_callable_invocation_source(&signature)) c_text(c, "    (void)invocation_source;\n");
        if (closure.capture_count || closure.state_capture)
            c_format(c, "    const XrlFnEnv_%" PRIu32 " *env = (const void *)context;\n", function.id);
        c_text(c, signature.result == XR_LIR_UNIT || owned ? "    " : "    return ");
        c_function_name(c,function.id); c_text(c,"(");
        if (owned) c_text(c,"landing");
        for (uint32_t p = 0; p < target.parameter_count; ++p) {
            if (p || owned) c_text(c,", ");
            if (p < closure.capture_count) c_format(c,"env->c%" PRIu32,p);
            else if (closure.state_capture && p + 1 == target.parameter_count) c_format(c,"env->c%" PRIu32,closure.capture_count);
            else c_format(c,"a%" PRIu32,p-closure.capture_count);
        }
        c_text(c, signature.result == XR_LIR_UNIT || owned ? ");\n    return;\n}\n" : ");\n}\n");
        c_format(c,"static const XrlFnEntry xrl_entry_%" PRIu32 " = {.s%" PRIu32 "=xrl_thunk_%" PRIu32 "};\n",
            function.id,closure.callable,function.id);
    }
}
static void c_callable_declarations(LirC *c) {
    for (uint32_t b = 0; b < c->view.block_count && c_request_poll(c); ++b) {
        XrLirBlock block; XrLirBlockView view;
        xr_lir_block_at(c->request->module,c->function,b,&block); xr_lir_block_view(c->request->module,block,&view);
        for (uint32_t n = 0; n < view.instruction_count; ++n) {
            XrLirInstructionView v; xr_lir_instruction_view(c->request->module,block,n,&v);
            if (v.op == XR_LIR_CELL_NEW && v.type == XR_LIR_I64_CELL)
                c_format(c,"    int64_t xrl_cell_%" PRIu32 ";\n",v.result);
            if (v.op == XR_LIR_FUNCTION_REF && v.operand_count)
                c_format(c,"    XrlFnEnv_%" PRIu32 " xrl_env_%" PRIu32 ";\n",v.callee,v.result);
        }
    }
}
static void c_callable_instruction(LirC *c, const XrLirInstructionView *v, uint32_t source) {
    if (v->op == XR_LIR_FUNCTION_REF) {
        const XrLirHomeEvent *event = c_callable_event(c,v->result);
        if (!event) return;
        for (uint32_t p = 0; p < v->operand_count; ++p) {
            c_format(c,"    xrl_env_%" PRIu32 ".c%" PRIu32 " = ",v->result,p);
            c_value(c,v->operands[p]); c_text(c,";\n");
        }
        if (event->callable_transport == XR_LIR_CALLABLE_TRANSPORT_OWNED) {
            c_callable_owned_constructor(c,v,source); return;
        }
        c_format(c,"    v%" PRIu32 " = (XrtFunction){&xrl_entry_%" PRIu32 ", ",v->result,v->callee);
        if (v->operand_count) c_format(c,"(uintptr_t)&xrl_env_%" PRIu32,v->result); else c_text(c,"0");
        c_text(c,", NULL};\n");
    } else if (v->op == XR_LIR_CALL_INDIRECT) {
        bool owned = v->result && c_values_owned_type(c,v->type);
        if (owned) c_values_ensure(c,source);
        c_text(c,"    "); if (v->result && !owned) { c_value(c,v->result); c_text(c," = "); }
        c_text(c,"((const XrlFnEntry *)"); c_value(c,v->operands[0]);
        c_format(c,".entry)->s%" PRIu32 "(",c_value_type(c,v->operands[0]));
        c_value(c,v->operands[0]); c_text(c,".environment");
        if (owned) {
            const XrLirValueHome *home = &c->homes->values[v->result];
            c_format(c,", (XrlLandingValue_%" PRIu32 "){xrl_frame, %zu, %zu,\n",home->type,home->first_owner,home->temporary_first_owner);
            c_format(c,"        (void *)((unsigned char *)xrl_payload + %zu), (void *)((unsigned char *)xrl_payload + %zu)}",
                home->offset,home->temporary_offset);
        }
        for (uint32_t p=1;p<v->operand_count;++p) { c_text(c,", "); c_value(c,v->operands[p]); }
        XrLirCallableType signature;
        if (c_callable_type(c, c_value_type(c, v->operands[0]), &signature) &&
            xr_lir_callable_invocation_source(&signature))
            c_format(c, ", UINT32_C(%" PRIu32 ")", source);
        c_text(c,");\n");
    } else if (v->op == XR_LIR_FUNCTION_SCOPE_ENTER)
        c_format(c,"    v%" PRIu32 " = UINT32_C(%" PRIu32 ");\n",v->result,v->result);
    else if (v->op == XR_LIR_FUNCTION_WEAKEN || v->op == XR_LIR_FUNCTION_BIND ||
        (v->op == XR_LIR_FUNCTION_SCOPE_LEAVE && v->result)) {
        const XrLirHomeEvent *event = c_callable_event(c,v->result);
        if (!event) return;
        uint32_t value = v->operands[v->operand_count-1];
        if (event->transfer_mode == XR_LIR_OWNERSHIP_TRANSFER_COPY) {
            c_values_replace_begin(c,v->result,source); c_text(c,"&"); c_value(c,value); c_values_replace_end(c,source);
        } else if (event->transfer_mode == XR_LIR_OWNERSHIP_TRANSFER_MOVE) {
            c_values_ensure(c,source); c_text(c,"    {\n"); c_callable_move(c,v->result,value,source,false); c_text(c,"    }\n");
        } else if (event->transfer_mode == XR_LIR_OWNERSHIP_TRANSFER_BORROW) {
            c_text(c,"    "); c_value(c,v->result); c_text(c," = "); c_value(c,value); c_text(c,";\n");
        } else c_error(c,XR_LIR_C_INVALID_LIR,"callable alias has no final transfer mode");
    } else if (v->op == XR_LIR_CELL_NEW) {
        bool may_repeat=true;
        XrLirBlock block={c->request->module,c->function.id,c->block};
        if (v->type!=XR_LIR_I64_CELL ||
            xr_lir_block_may_repeat(c->request->module,block,&may_repeat)!=XR_LIR_OK || may_repeat) {
            c_error(c,XR_LIR_C_INVALID_LIR,"Frame scalar Cell lacks completed one-shot construction facts");return;
        }
        c_format(c,"    xrl_cell_%" PRIu32 " = ",v->result); c_value(c,v->operands[0]);
        c_format(c,";\n    v%" PRIu32 " = &xrl_cell_%" PRIu32 ";\n",v->result,v->result);
    } else if (v->op == XR_LIR_CELL_READ) {
        c_format(c,"    v%" PRIu32 " = *",v->result); c_value(c,v->operands[0]); c_text(c,";\n");
    } else if (v->op == XR_LIR_CELL_WRITE) {
        c_text(c,"    *"); c_value(c,v->operands[0]); c_text(c," = "); c_value(c,v->operands[1]); c_text(c,";\n");
    }
    if (v->op == XR_LIR_FUNCTION_SCOPE_LEAVE) {
        c_callable_scope_release(c);
        c_cell_scope_release(c,source);
    }
}
