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
    if (parameter.type == XR_LIR_ARRAY || parameter.type == XR_LIR_MAP_STRING_I64) return "XrtPlace";
    return NULL;
}
static bool c_callable_signature_admit(LirC *c, XrLirCallableType signature) {
    if (!c_type(c,signature.result) || c_managed_leaf(signature.result) ||
        xr_lir_boxed_type(c->request->module,signature.result) ||
        c_callable_type(c,signature.result,NULL) || signature.result == XR_LIR_I64_CELL) {
        c_error(c,XR_LIR_C_UNSUPPORTED,"callable result has no established native landing ABI"); return false;
    }
    for (uint32_t p = 0; p < signature.parameter_count; ++p) {
        XrLirCallableParameter parameter = signature.parameters[p];
        if (parameter.escaping || (parameter.mode == XR_PARAM_MOVE &&
            (c_managed_leaf(parameter.type) || xr_lir_boxed_type(c->request->module,parameter.type) ||
             c_callable_type(c,parameter.type,NULL)))) {
            c_error(c,XR_LIR_C_UNSUPPORTED,"owned callable parameters require lifetime transport"); return false;
        }
        if (!c_callable_parameter_type(c,parameter)) {
            c_error(c,XR_LIR_C_UNSUPPORTED,"callable parameter has no established native binding"); return false;
        }
    }
    return true;
}
typedef struct LirCFnDfs { uint32_t block, parent, edge; } LirCFnDfs;
/* Reentering a constructor needs distinct capture snapshots. Identify only
 * cyclic blocks in linear time; an unrelated loop does not reject its caller. */
static bool c_callable_cycle_index(LirC *c) {
    size_t slots = c->view.block_count;
    if (slots == SIZE_MAX) { c_error(c,XR_LIR_C_OVERFLOW,"callable CFG exceeds address space"); return false; }
    ++slots;
    void *memory = NULL;
    if (!c_resource(c,xr_compile_resources_calloc(c->request->resources,slots,
        3 * sizeof(uint32_t) + sizeof(LirCFnDfs) + 2,&memory))) return false;
    c->callable_cfg = memory;
    uint32_t *index = memory, *low = index + slots, *stack = low + slots;
    LirCFnDfs *frames = (LirCFnDfs *)(stack + slots);
    uint8_t *active = (uint8_t *)(frames + slots), *cycles = active + slots;
    c->callable_cycles = cycles;
    const XrLirFunctionData *function = &c->request->module->functions[c->function.id - 1];
    uint32_t clock = 0; size_t depth = 0, used = 0;
    for (uint32_t n = 0; n < c->view.block_count && c_request_poll(c); ++n) {
        uint32_t start = n + 1;
        if (index[start]) continue;
        frames[depth++] = (LirCFnDfs){start,0,0};
        while (depth && c_request_poll(c)) {
            LirCFnDfs *walk = &frames[depth - 1]; uint32_t block = walk->block;
            if (!index[block]) {
                index[block] = low[block] = ++clock; stack[used++] = block; active[block] = 1;
            }
            const XrLirBlockData *body = &function->blocks[block - 1];
            const XrLirInstructionView *end = &body->instructions[body->instruction_count - 1].view;
            if (walk->edge < end->edge_count) {
                uint32_t target = end->edges[walk->edge++].target;
                if (!index[target]) { frames[depth++] = (LirCFnDfs){target,block,0}; continue; }
                if (active[target] && low[block] > index[target]) low[block] = index[target];
                continue;
            }
            uint32_t parent = walk->parent; --depth;
            if (parent && low[parent] > low[block]) low[parent] = low[block];
            if (low[block] == index[block]) {
                size_t begin = used;
                do { XR_CHECK(begin,"an active component contains its root"); --begin; } while (stack[begin] != block);
                bool cyclic = used - begin > 1;
                for (uint32_t e = 0; e < end->edge_count; ++e) cyclic |= end->edges[e].target == block;
                for (size_t at = begin; at < used; ++at) { active[stack[at]] = 0; cycles[stack[at]] = (uint8_t)cyclic; }
                used = begin;
            }
        }
    }
    return c_request_poll(c);
}
static bool c_callable_admit(LirC *c, const XrLirInstructionView *v) {
    c->callables = true;
    if (v->op == XR_LIR_FUNCTION_REF) {
        XrLirFunctionView target;
        xr_lir_function_view(c->request->module,(XrLirFunction){c->request->module,v->callee},&target);
        if (v->bits != XR_LIR_CLOSURE_FRAME || target.kind != XR_LIR_FUNCTION_DEFINITION) {
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
        if (v->operand_count) {
            if (!c->callable_cycles && !c_callable_cycle_index(c)) return false;
            if (c->callable_cycles[c->block]) {
                c_error(c,XR_LIR_C_UNSUPPORTED,"reentered Frame environments require independent capture snapshots"); return false;
            }
            for (uint32_t p = 0; p < v->operand_count; ++p) {
                XrLirType type = c_value_type(c,v->operands[p]); XrLirTypeLayout layout;
                if (type == XR_LIR_I64_CELL || type == XR_LIR_STATE || type == XR_LIR_STATE_REF || c_callable_type(c,type,NULL)) continue;
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
}
static void c_callable_types(LirC *c) {
    if (!c->callables) return;
    bool opened = false;
    for (uint32_t t = 0; t < c->constructed_count && c_request_poll(c); ++t) {
        if (c->constructed_types[t].shape.kind != XR_LIR_TYPE_CALLABLE || !c->constructed_types[t].layout.size) continue;
        XrLirCallableType signature;
        XR_CHECK(c_callable_type(c, XR_LIR_TYPE_COUNT + t, &signature), "receiving callable has its exact signature");
        if (!c_callable_signature_admit(c,signature)) return;
        if (!opened) { c_text(c, "typedef union XrlFnEntry {\n"); opened = true; }
        c_format(c, "    %s (*s%" PRIu32 ")(uintptr_t", c_type(c, signature.result), XR_LIR_TYPE_COUNT + t);
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
        c_format(c, "static %s xrl_thunk_%" PRIu32 "(uintptr_t context", c_type(c,signature.result), function.id);
        c_callable_parameters(c,signature,true); c_text(c, ") {\n    (void)context;\n");
        if (closure.capture_count || closure.state_capture)
            c_format(c, "    const XrlFnEnv_%" PRIu32 " *env = (const void *)context;\n", function.id);
        c_text(c, signature.result == XR_LIR_UNIT ? "    " : "    return ");
        c_function_name(c,function.id); c_text(c,"(");
        for (uint32_t p = 0; p < target.parameter_count; ++p) {
            if (p) c_text(c,", ");
            if (p < closure.capture_count) c_format(c,"env->c%" PRIu32,p);
            else if (closure.state_capture && p + 1 == target.parameter_count) c_format(c,"env->c%" PRIu32,closure.capture_count);
            else c_format(c,"a%" PRIu32,p-closure.capture_count);
        }
        c_text(c, signature.result == XR_LIR_UNIT ? ");\n    return;\n}\n" : ");\n}\n");
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
static void c_callable_instruction(LirC *c, const XrLirInstructionView *v) {
    if (v->op == XR_LIR_FUNCTION_REF) {
        for (uint32_t p = 0; p < v->operand_count; ++p) {
            c_format(c,"    xrl_env_%" PRIu32 ".c%" PRIu32 " = ",v->result,p);
            c_value(c,v->operands[p]); c_text(c,";\n");
        }
        c_format(c,"    v%" PRIu32 " = (XrtFunction){&xrl_entry_%" PRIu32 ", ",v->result,v->callee);
        if (v->operand_count) c_format(c,"(uintptr_t)&xrl_env_%" PRIu32,v->result); else c_text(c,"0");
        c_text(c,", NULL};\n");
    } else if (v->op == XR_LIR_CALL_INDIRECT) {
        c_text(c,"    "); if (v->result) c_format(c,"v%" PRIu32 " = ",v->result);
        c_text(c,"((const XrlFnEntry *)"); c_value(c,v->operands[0]);
        c_format(c,".entry)->s%" PRIu32 "(",c_value_type(c,v->operands[0]));
        c_value(c,v->operands[0]); c_text(c,".environment");
        for (uint32_t p=1;p<v->operand_count;++p) { c_text(c,", "); c_value(c,v->operands[p]); }
        c_text(c,");\n");
    } else if (v->op == XR_LIR_FUNCTION_SCOPE_ENTER)
        c_format(c,"    v%" PRIu32 " = UINT32_C(%" PRIu32 ");\n",v->result,v->result);
    else if (v->op == XR_LIR_FUNCTION_WEAKEN || v->op == XR_LIR_FUNCTION_BIND ||
        (v->op == XR_LIR_FUNCTION_SCOPE_LEAVE && v->result)) {
        c_format(c,"    v%" PRIu32 " = ",v->result);
        c_value(c,v->operands[v->operand_count-1]); c_text(c,";\n");
    } else if (v->op == XR_LIR_CELL_NEW) {
        c_format(c,"    xrl_cell_%" PRIu32 " = ",v->result); c_value(c,v->operands[0]);
        c_format(c,";\n    v%" PRIu32 " = &xrl_cell_%" PRIu32 ";\n",v->result,v->result);
    } else if (v->op == XR_LIR_CELL_READ) {
        c_format(c,"    v%" PRIu32 " = *",v->result); c_value(c,v->operands[0]); c_text(c,";\n");
    } else if (v->op == XR_LIR_CELL_WRITE) {
        c_text(c,"    *"); c_value(c,v->operands[0]); c_text(c," = "); c_value(c,v->operands[1]); c_text(c,";\n");
    }
}
