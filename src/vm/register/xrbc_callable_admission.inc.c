/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_callable_admission.inc.c - Owned exact invocation signatures
 */
static bool rbc_callable_home_type(const XrtValueImage *image, uint32_t type) {
    if (!image || !image->types || !type || type > image->type_count) return false;
    const XrtValueImageType *row = &image->types[type - 1];
    if (row->kind == XRT_VALUE_ARRAY_INLINE) return true;
    if (row->kind != XRT_VALUE_NULLABLE || !row->nullable_element || row->nullable_element >= type) return false;
    XrtValueKind payload = image->types[row->nullable_element - 1].kind;
    return payload == XRT_VALUE_ARRAY_U8 || payload == XRT_VALUE_ARRAY_I64 || payload == XRT_VALUE_ARRAY_STRING;
}

static bool rbc_callable_value_valid(const XrRbcModule *m, XrRbcCallableValue value, size_t before) {
    switch (value.kind) {
    case XR_RBC_CALLABLE_SCALAR:
        return value.type == XR_POD_UNIT || value.type == XR_POD_BOOL || value.type == XR_POD_I64 ||
            value.type == XR_POD_U8 || value.type == XR_POD_U64 || value.type == XR_POD_F64;
    case XR_RBC_CALLABLE_REFERENCE:
        return value.type == XR_RBC_STRING || value.type == XR_RBC_MAP_STRING_I64 || value.type == XR_RBC_SET_I64 || rbc_array_kind((XrRbcRegisterKind)value.type);
    case XR_RBC_CALLABLE_AGGREGATE: return value.type < m->aggregate_type_count;
    case XR_RBC_CALLABLE_FUNCTION: return value.type < before;
    case XR_RBC_CALLABLE_HOME: {
        const XrRbcCodeOwner *owner = xr_rbc_module_code_owner(m);
        return owner && rbc_callable_home_type(owner->values, value.type);
    }
    default: return false;
    }
}

static XrRbcRegisterKind rbc_callable_value_kind(XrRbcCallableValue value) {
    return value.kind == XR_RBC_CALLABLE_REFERENCE ? (XrRbcRegisterKind)value.type :
        value.kind == XR_RBC_CALLABLE_FUNCTION ? XR_RBC_FUNCTION : XR_RBC_BITS;
}

static bool rbc_callable_accepts(const XrRbcModule *m, uint32_t expected, uint32_t actual) {
    if (!m->callable_types || !expected || !actual ||
        expected > m->callable_type_count || actual > m->callable_type_count) return false;
    const XrRbcCallableType *a = &m->callable_types[expected - 1], *b = &m->callable_types[actual - 1];
    if (a->parameter_count != b->parameter_count || a->result.kind != b->result.kind || a->result.type != b->result.type ||
        (a->flags & 2u) != (b->flags & 2u) || ((a->flags & 1u) && !(b->flags & 1u))) return false;
    for (uint32_t p = 0; p < a->parameter_count; ++p) {
        XrRbcCallableParameter x = a->parameters[p], y = b->parameters[p];
        if (x.value.kind != y.value.kind || x.value.type != y.value.type ||
            x.mode != y.mode || x.escaping != y.escaping) return false;
    }
    return true;
}


static bool rbc_callable_parameter_owned(const XrRbcModule *m, const XrRbcFunction *f, uint32_t parameter) {
    if (parameter < f->capture_count && f->captures[parameter].kind == XR_RBC_CALLABLE_FUNCTION) return true;
    if (!m->function_callable_parameters) return false;
    XrRbcFunctionCallableParameters view = m->function_callable_parameters[f - m->functions];
    for (uint32_t p = 0; p < view.count; ++p)
        if (view.rows[p].parameter == parameter) return view.rows[p].contract.escaping;
    return false;
}
static bool rbc_callable_image_value(const XrRbcModule *module, uint32_t id, XrRbcCallableValue value) {
    const XrtValueImage *image=xr_rbc_module_value_image(module);
    if (!image || !image->types || !id || id>image->type_count) return false;
    XrtValueKind kind=image->types[id-1].kind;
    if (value.kind==XR_RBC_CALLABLE_FUNCTION) {
        const uint32_t *types=xr_rbc_module_type_callables(module);
        return types && kind==XRT_VALUE_FUNCTION && types[id-1]==value.type+1;
    }
    if (value.kind==XR_RBC_CALLABLE_SCALAR) {
        XrtValueKind scalar;
        return xr_pod_value_kind((XrPodScalar)value.type,&scalar) && scalar==kind;
    }
    if (value.kind == XR_RBC_CALLABLE_HOME) return id == value.type && rbc_callable_home_type(image, id);
    if (value.kind==XR_RBC_CALLABLE_REFERENCE)
        return value.type==XR_RBC_STRING ? kind==XRT_VALUE_STRING : value.type==XR_RBC_SET_I64 ?
            kind==XRT_VALUE_SET_I64 : value.type==XR_RBC_MAP_STRING_I64 ?
            kind==XRT_VALUE_MAP_STRING_I64 : rbc_array_kind((XrRbcRegisterKind)value.type) &&
            kind==(XrtValueKind)(XRT_VALUE_ARRAY_I64+xr_rbc_array_family((XrRbcRegisterKind)value.type));
    return false;
}
static bool rbc_callable_image_valid(const XrRbcModule *module) {
    const XrtValueImage *image=xr_rbc_module_value_image(module);
    const uint32_t *types=xr_rbc_module_type_callables(module);
    if (!image) return !types;
    if (!types && image->type_count) {
        for (uint32_t n=0;n<image->type_count;++n) if (image->types[n].kind==XRT_VALUE_FUNCTION) return false;
        return true;
    }
    for (uint32_t n=0;n<image->type_count;++n) {
        const XrtValueImageType *row=&image->types[n];
        if (row->kind!=XRT_VALUE_FUNCTION) { if (types[n]) return false; continue; }
        uint32_t id=types[n];
        if (!id || id>module->callable_type_count) return false;
        const XrRbcCallableType *signature=&module->callable_types[id-1];
        if (row->parameter_count!=signature->parameter_count || row->flags!=signature->flags || row->throws ||
            !rbc_callable_image_value(module,row->result,signature->result)) return false;
        for (uint32_t p=0;p<row->parameter_count;++p) {
            XrtValueImageParameter parameter=image->parameters[row->first_parameter+p];
            XrRbcCallableParameter expected=signature->parameters[p];
            if (parameter.mode!=expected.mode || (parameter.escaping!=0)!=expected.escaping ||
                !rbc_callable_image_value(module,parameter.type,expected.value)) return false;
        }
    }
    const XrRbcCallableEntry *entries=xr_rbc_module_callable_entries(module);
    size_t count=xr_rbc_module_callable_entry_count(module);
    const XrRbcCodeOwner *owner=xr_rbc_module_code_owner(module);
    if (entries && count<module->function_count) return false;
    if (entries) for (size_t n=0;n<count;++n) {
        uint32_t signature=entries[n].signature_type;
        if (entries[n].kind==XR_RBC_CALLABLE_ENTRY_ASSERT_CONDITION) {
            if (!owner || entries[n].producer!=owner || n<module->function_count ||
                entries[n].physical_function!=UINT32_MAX || !entries[n].source_function ||
                !signature || signature>image->type_count || !entries[n].callable ||
                types[signature-1]!=entries[n].callable) return false;
        } else {
            if (entries[n].kind!=XR_RBC_CALLABLE_ENTRY_CODE || n>=module->function_count ||
                entries[n].physical_function!=n) return false;
            if (module->functions[n].callable ? !signature || signature>image->type_count ||
                    !entries[n].source_function || types[signature-1]!=module->functions[n].callable : signature!=0) return false;
        }
    }
    return true;
}

static bool rbc_callable_needs_homes(XrRbcCallableType signature) {
    if (signature.result.kind == XR_RBC_CALLABLE_HOME) return true;
    for (uint32_t p = 0; p < signature.parameter_count; ++p)
        if (signature.parameters[p].value.kind == XR_RBC_CALLABLE_HOME) return true;
    return false;
}
static bool rbc_callable_signatures_valid(const XrRbcModule *m) {
    if (m->callable_type_count>UINT32_MAX || (!m->callable_types && m->callable_type_count)) return false;
    if (m->callable_results ? m->callable_result_count!=m->function_count : m->callable_result_count!=0) return false;
    if (m->function_scopes ? m->function_scope_count!=m->function_count : m->function_scope_count!=0) return false;
    if (m->function_callable_parameters ? m->function_callable_parameter_count!=m->function_count :
        m->function_callable_parameter_count!=0) return false;
    for (size_t t = 0; t < m->callable_type_count; ++t) {
        XrRbcCallableType type = m->callable_types[t];
        if (type.parameter_count > 65536 || (type.parameter_count && !type.parameters) ||
            (type.flags & ~3u) || ((type.flags & 2u) && !(type.flags & 1u)) ||
            ((type.flags & 2u) && rbc_callable_needs_homes(type)) ||
            !rbc_callable_value_valid(m, type.result, t)) return false;
        for (uint32_t p = 0; p < type.parameter_count; ++p) {
            XrRbcCallableParameter parameter = type.parameters[p];
            if (parameter.mode > 2u || !rbc_callable_value_valid(m, parameter.value, t) ||
                (parameter.value.kind == XR_RBC_CALLABLE_HOME && parameter.mode != XR_PARAM_READ) ||
                (parameter.escaping && (parameter.value.kind != XR_RBC_CALLABLE_FUNCTION || parameter.mode != 0))) return false;
        }
    }
    return true;
}
static bool rbc_callable_home_parameter(const XrRbcModule *module, const XrRbcValueCode *code,
    uint32_t parameter, uint32_t type) {
    const XrtValueImage *image = xr_rbc_module_value_image(module);
    if (!rbc_callable_home_type(image, type) || !code || !code->homes || !code->homes->values ||
        !code->parameters || !code->locations || parameter >= code->parameter_count) return false;
    uint32_t id = code->parameters[parameter];
    if (!id || id > code->value_count || id > code->homes->value_count) return false;
    const XrLirValueHome *home = &code->homes->values[id];
    const XrtValueImageType *row = &image->types[type - 1];
    return home->type == type && home->parameter == parameter + 1 && home->owner_count == 1 &&
        row->owned_count == home->owner_count && home->size == row->size &&
        code->locations[id].kind == XR_RBC_VALUE_NOWHERE;
}
static bool rbc_callable_metadata_valid(const XrRbcModule *m) {
    if (!rbc_callable_signatures_valid(m)) return false;
    for (size_t n = 0; n < m->function_count; ++n) {
        const XrRbcFunction *f = &m->functions[n];
        const XrRbcValueCode *callable_code=f->callable ? xr_rbc_module_value_code(m,(uint32_t)n) : NULL;
        if (callable_code && callable_code->error_type) return false;
        XrRbcFunctionScopes scopes = rbc_function_scopes(m, f);
        if (scopes.count > f->code_count || (scopes.count ? !scopes.parents : scopes.parents != NULL)) return false;
        for (uint32_t p = 0; p < scopes.count; ++p)
            if (scopes.parents[p] > scopes.count || scopes.parents[p] == p + 1) return false;
        uint32_t result = m->callable_results ? m->callable_results[n] : 0;
        if (f->result_kind == XR_RBC_FUNCTION ? !result || result > m->callable_type_count : result != 0) return false;
        if (f->capture_count > f->parameter_count || f->parameter_count > f->register_count ||
            (f->capture_count ? !f->captures : f->captures != NULL) ||
            (f->environment_count ? !f->environments : f->environments != NULL) ||
            (!m->callable_type_count && (f->environment_count || f->environment_size)) ||
            f->environment_count > UINT16_MAX + 1u) return false;
        if (f->register_callables ? f->register_callable_count != f->register_count :
            f->register_callable_count != 0) return false;
        for (uint32_t p = 0; p < f->capture_count; ++p) {
            XrRbcCallableValue value = f->captures[p];
            if (value.kind == XR_RBC_CALLABLE_CELL) {
                const XrRbcValueCode *code=xr_rbc_module_value_code(m,(uint32_t)n);
                const XrtValueImage *image=xr_rbc_module_value_image(m);
                if (!xr_rbc_module_code_owner(m) || !code || !code->homes || !code->parameters || !code->locations ||
                    p>=code->parameter_count || !image || !image->types || !value.type || value.type>image->type_count)
                    return false;
                uint32_t formal=code->parameters[p];
                if (!formal || formal>code->value_count || formal>code->homes->value_count || !code->homes->values)
                    return false;
                const XrLirValueHome *home=&code->homes->values[formal];
                const XrtValueImageType *type=&image->types[value.type-1];
                if (type->kind!=XRT_VALUE_CELL || !type->element || type->element>image->type_count ||
                    (image->types[type->element-1].kind!=XRT_VALUE_I64 && image->types[type->element-1].kind!=XRT_VALUE_MAP_STRING_I64) ||
                    home->type!=value.type || home->cell_payload_type!=type->element || home->parameter!=p+1 ||
                    home->cell_transport!=XR_LIR_CELL_TRANSPORT_BORROW || home->owner_count ||
                    home->offset!=SIZE_MAX || home->temporary_offset!=SIZE_MAX || home->first_owner!=SIZE_MAX ||
                    home->temporary_first_owner!=SIZE_MAX || code->locations[formal].kind!=XR_RBC_VALUE_NOWHERE ||
                    rbc_register_kind(f,p)!=XR_RBC_BITS) return false;
                continue;
            }
            if (value.kind == XR_RBC_CALLABLE_BINDING) {
                if (value.type != XR_POD_I64 || rbc_register_kind(f, p) != XR_RBC_I64_BINDING) return false;
                continue;
            }
            if (value.kind == XR_RBC_CALLABLE_FUNCTION) {
                if (!rbc_callable_value_valid(m, value, m->callable_type_count) ||
                    rbc_register_kind(f, p) != XR_RBC_FUNCTION || !f->register_callables ||
                    f->register_callables[p] != value.type + 1) return false;
                continue;
            }
            if (value.kind == XR_RBC_CALLABLE_REFERENCE) {
                if (value.type != XR_RBC_MAP_STRING_I64 || rbc_register_kind(f, p) != XR_RBC_MAP_STRING_I64) return false;
                continue;
            }
            if (value.kind != XR_RBC_CALLABLE_SCALAR || value.type == XR_POD_UNIT ||
                !rbc_callable_value_valid(m, value, 0) || rbc_register_kind(f, p) != XR_RBC_BITS) return false;
        }
        for (uint32_t r = 0; r < f->register_count; ++r) {
            uint32_t type = f->register_callables ? f->register_callables[r] : 0;
            if (rbc_register_kind(f, r) == XR_RBC_FUNCTION ? !type || type > m->callable_type_count : type != 0)
                return false;
        }
        if (m->function_callable_parameters) {
            XrRbcFunctionCallableParameters parameters = m->function_callable_parameters[n];
            if (parameters.count > f->parameter_count || (parameters.count ? !parameters.rows : parameters.rows != NULL))
                return false;
            for (uint32_t p = 0; p < parameters.count; ++p) {
                XrRbcFunctionCallableParameter row = parameters.rows[p];
                if (row.parameter >= f->parameter_count || (p && parameters.rows[p - 1].parameter >= row.parameter) ||
                    row.contract.mode != 0 || row.contract.value.kind != XR_RBC_CALLABLE_FUNCTION ||
                    !rbc_callable_value_valid(m, row.contract.value, m->callable_type_count) ||
                    rbc_register_kind(f, row.parameter) != XR_RBC_FUNCTION ||
                    f->register_callables[row.parameter] != row.contract.value.type + 1) return false;
            }
        }
        if (!f->callable) {
            if (f->capture_count || f->state_capture) return false;
            continue;
        }
        if (f->callable > m->callable_type_count) return false;
        XrRbcCallableType type = m->callable_types[f->callable - 1];
        if (f->state_capture && (!m->state_spec || f->parameter_count == f->capture_count ||
            rbc_register_kind(f, f->parameter_count - 1) != XR_RBC_STATE)) return false;
        const XrRbcValueCode *typed=xr_rbc_module_value_code(m,(uint32_t)n);
        if (type.parameter_count != f->parameter_count - f->capture_count - f->state_capture || f->aggregate_parameter_count ||
            type.result.kind == XR_RBC_CALLABLE_AGGREGATE ||
            ((type.result.kind==XR_RBC_CALLABLE_FUNCTION || type.result.kind==XR_RBC_CALLABLE_HOME) && (!typed || !typed->homes ||
                !rbc_callable_image_value(m,typed->homes->result_type,type.result))) ||
            rbc_callable_value_kind(type.result) != f->result_kind) return false;
        for (uint32_t p = 0; p < type.parameter_count; ++p) {
            XrRbcCallableValue value = type.parameters[p].value;
            uint32_t formal = f->capture_count + p;
            if (type.parameters[p].mode!=XR_PARAM_READ || value.kind == XR_RBC_CALLABLE_AGGREGATE ||
                rbc_callable_value_kind(value) != rbc_register_kind(f, formal) ||
                (value.kind == XR_RBC_CALLABLE_HOME && !rbc_callable_home_parameter(m, typed, formal, value.type)) ||
                (value.kind == XR_RBC_CALLABLE_FUNCTION && (f->register_callables[formal] != value.type + 1 ||
                 type.parameters[p].escaping != rbc_callable_parameter_owned(m, f, formal)))) return false;
        }
    }
    for (size_t n = 0; m->callable_type_count && n < m->function_count; ++n) {
        const XrRbcFunction *f = &m->functions[n];
        uint32_t bytes = 0;
        for (uint32_t e = 0; e < f->environment_count; ++e) {
            XrRbcEnvironment environment = f->environments[e];
            if (environment.callee >= m->function_count || environment.offset != bytes) return false;
            const XrRbcFunction *callee = &m->functions[environment.callee];
            uint32_t count = callee->capture_count + callee->state_capture;
            if (environment.producer==UINT32_MAX) {
                const XrRbcValueCode *code=xr_rbc_module_value_code(m,(uint32_t)n);
                bool found=false;
                for (size_t a=0;code && a<code->access_count;++a) {
                    XrRbcValueAccess access=code->accesses[a];
                    if (access.operation==XR_RBC_VALUE_FUNCTION_NEW && access.window==e+1) {
                        if (found || access.index!=environment.callee || access.count!=count) return false;
                        found=true;
                    }
                }
                if (!found || !count || bytes>UINT32_MAX-sizeof(XrtFunction)) return false;
                bytes+=(uint32_t)sizeof(XrtFunction);
                for (uint32_t p=0;p<count;++p) {
                    uint32_t size=p<callee->capture_count && callee->captures[p].kind==XR_RBC_CALLABLE_FUNCTION ?
                        (uint32_t)sizeof(XrtFunction) : 8u;
                    if (bytes>UINT32_MAX-size) return false;
                    bytes+=size;
                }
                continue;
            }
            if (!f->aggregate_accesses || environment.producer>=f->aggregate_access_count) return false;
            if (!count || bytes > UINT32_MAX - sizeof(XrtFunction) ||
                count > (UINT32_MAX - bytes - sizeof(XrtFunction)) / 8u) return false;
            XrRbcAggregateAccess access = f->aggregate_accesses[environment.producer];
            if (access.operation != XR_RBC_FUNCTION_REFERENCE || access.destination != e + 1 ||
                access.callee != environment.callee || access.count != count) return false;
            bytes += (uint32_t)sizeof(XrtFunction) + count * 8u;
        }
        size_t offset, total;
        if (bytes != f->environment_size || !xr_rbc_activation_size(f, &offset, &total)) return false;
    }
    return true;
}

static uint32_t rbc_result_value_type(const XrRbcValueCode *code) {
    return code && code->homes ? code->homes->result_type : 0;
}

static XrtValueKind rbc_image_value_kind(const XrtValueImage *image, uint32_t type) {
    return type && image && image->types && type <= image->type_count ?
        image->types[type - 1].kind : XRT_VALUE_UNIT;
}

static uint32_t rbc_formal_value_type(const XrRbcValueCode *code, uint32_t parameter) {
    if (!code || !code->homes || !code->homes->values || !code->parameters ||
        parameter >= code->parameter_count) return 0;
    uint32_t value = code->parameters[parameter];
    if (!value || value > code->value_count || value > code->homes->value_count) return 0;
    return code->homes->values[value].type;
}

static bool rbc_requires_typed_value(const XrtValueImage *image, uint32_t type) {
    XrtValueKind kind = rbc_image_value_kind(image, type);
    return kind == XRT_VALUE_FUNCTION || kind == XRT_VALUE_CLASS || rbc_callable_home_type(image, type);
}

static bool rbc_requires_typed_call(const XrRbcModule *module,uint32_t function) {
    const XrRbcValueCode *code=xr_rbc_module_value_code(module,function);
    const XrtValueImage *image=xr_rbc_module_value_image(module);
    if (code && code->error_type) return true;
    if (!code || !code->homes || !image) return false;
    if (rbc_requires_typed_value(image, rbc_result_value_type(code))) return true;
    for (uint32_t p=0;p<code->parameter_count;++p)
        if (rbc_requires_typed_value(image, rbc_formal_value_type(code, p))) return true;
    return false;
}
static bool rbc_callable_access_valid(const XrRbcModule *m, const XrRbcFunction *f,
    XrRbcAggregateAccess access) {
    if ((access.operation >= XR_RBC_CAPTURED_CELL_NEW && access.operation <= XR_RBC_CAPTURED_CELL_BIND) ||
        (access.operation >= XR_RBC_FUNCTION_STATE_READ && access.operation <= XR_RBC_FUNCTION_STATE_INITIALIZE) ||
        access.operation == XR_RBC_FUNCTION_OWNED_REFERENCE) return false;
    if (access.operation >= XR_RBC_FUNCTION_SCOPE_ENTER && access.operation <= XR_RBC_FUNCTION_BIND) {
        XrRbcFunctionScopes scopes = rbc_function_scopes(m, f);
        if (!access.destination || access.destination > scopes.count || access.first || access.callee || access.result.spill)
            return false;
        if (access.operation == XR_RBC_FUNCTION_SCOPE_ENTER)
            return access.source == scopes.parents[access.destination - 1] && !access.result.index &&
                !access.count && !access.slot && !access.window;
        if (access.operation == XR_RBC_FUNCTION_BIND ? access.count != 1 || access.window == UINT32_MAX :
                access.count > 1 || access.window != 0) return false;
        if (!access.count) return !access.source && !access.result.index && !access.slot;
        return access.source < f->register_count && access.result.index < f->register_count &&
            rbc_register_kind(f, access.source) == XR_RBC_FUNCTION &&
            rbc_register_kind(f, access.result.index) == XR_RBC_FUNCTION && access.slot &&
            f->register_callables[access.source] == access.slot && f->register_callables[access.result.index] == access.slot;
    }
    if (access.operation == XR_RBC_FUNCTION_SCOPE_RETURN) {
        uint32_t function = (uint32_t)(f - m->functions);
        uint32_t result = m->callable_results ? m->callable_results[function] : 0;
        if (access.slot != result || access.first || access.destination || access.callee || access.window ||
            access.result.index || access.result.spill || access.count > 1) return false;
        const XrRbcValueCode *code = xr_rbc_module_value_code(m, function);
        if (rbc_callable_home_type(xr_rbc_module_value_image(m), rbc_result_value_type(code))) return false;
        if (f->callable) {
            XrRbcCallableValue declared = m->callable_types[f->callable - 1].result;
            if (declared.kind == XR_RBC_CALLABLE_HOME) return false;
            if (declared.kind == XR_RBC_CALLABLE_SCALAR && declared.type == XR_POD_UNIT)
                return !access.count && !access.source;
            if (access.count != 1) return false;
        }
        if (!access.count) return !access.source && f->result_kind == XR_RBC_BITS;
        return access.source < f->register_count && rbc_register_kind(f, access.source) == f->result_kind;
    }
    if (access.operation == XR_RBC_FUNCTION_RETURN) {
        if (!f->callable || access.slot != f->callable || access.first || access.destination ||
            access.callee || access.window || access.result.index || access.result.spill) return false;
        XrRbcCallableValue result = m->callable_types[f->callable - 1].result;
        if (result.kind == XR_RBC_CALLABLE_HOME) return false;
        if (result.kind == XR_RBC_CALLABLE_SCALAR && result.type == XR_POD_UNIT)
            return !access.count && !access.source;
        return access.count == 1 && access.source < f->register_count &&
            rbc_register_kind(f, access.source) == rbc_callable_value_kind(result);
    }
    if (!access.slot || access.slot > m->callable_type_count || access.result.spill) return false;
    if (access.operation == XR_RBC_FUNCTION_REFERENCE) {
        if (access.source || access.window || access.callee >= m->function_count) return false;
        const XrRbcFunction *callee = &m->functions[access.callee];
        if (rbc_requires_typed_call(m,access.callee)) return false;
        uint32_t count = callee->capture_count + callee->state_capture;
        if (count) {
            if (!access.destination || access.destination > f->environment_count || access.count != count ||
                access.first > f->aggregate_operand_count || count > f->aggregate_operand_count - access.first ||
                f->environments[access.destination - 1].callee != access.callee) return false;
            for (uint32_t p = 0; p < count; ++p) {
                XrRbcArrayLocation location = f->aggregate_operands[access.first + p];
                if (p < callee->capture_count && callee->captures[p].kind == XR_RBC_CALLABLE_FUNCTION) return false;

                XrRbcRegisterKind kind = p < callee->capture_count ?
                    (callee->captures[p].kind == XR_RBC_CALLABLE_BINDING ? XR_RBC_I64_BINDING :
                     XR_RBC_BITS) : XR_RBC_STATE;
                if (!rbc_array_location_valid(f, location) || rbc_array_location_kind(f, location) != kind) return false;
            }
        } else if (access.first || access.count || access.destination) return false;
        return
            rbc_callable_accepts(m, access.slot, m->functions[access.callee].callable) &&
            access.result.index < f->register_count && rbc_register_kind(f, access.result.index) == XR_RBC_FUNCTION &&
            f->register_callables[access.result.index] == access.slot;
    }
    if (access.first || access.count || access.destination) return false;
    XrRbcCallableType type = m->callable_types[access.slot - 1];
    if (access.callee || access.result.index || access.source >= f->register_count ||
        rbc_register_kind(f, access.source) != XR_RBC_FUNCTION ||
        f->register_callables[access.source] != access.slot || access.window >= f->register_count ||
        type.parameter_count != f->register_count - access.window - 1 ||
        type.result.kind == XR_RBC_CALLABLE_FUNCTION || type.result.kind == XR_RBC_CALLABLE_AGGREGATE ||
        rbc_callable_needs_homes(type)) return false;
    for (uint32_t p = 0; p <= type.parameter_count; ++p)
        if (rbc_register_kind(f, access.window + p) != XR_RBC_CALL_WINDOW) return false;
    return true;
}
