/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xhir_body_call_binding.inc.c - Declaration-owned source argument permutation
 *
 * KEY CONCEPT:
 *   Written arguments keep their evaluation order. The checked signature owns
 *   the separate mapping to formal operands and authentic omitted defaults.
 */
static HirBodySignature *body_call_binding_row(XrHirBody *body, XrHirBodySignature signature) {
    if (!body || body->ended || signature.owner != body || signature.index >= body->signatures.count)
        return NULL;
    HirBodySignature *row = &((HirBodySignature *)body->signatures.items)[signature.index];
    return row->finished ? NULL : row;
}

static XrXirStatus body_call_binding_error(XrHirCallBindingError *error,
    XrHirCallBindingFailure kind, uint32_t argument, uint32_t formal, uint32_t previous) {
    if (error) *error = (XrHirCallBindingError){kind, argument, formal, previous};
    return XR_XIR_BAD_TYPE;
}

static bool body_call_binding_rest(const XrHirBody *body, const HirBodySignature *row) {
    if(row->native_member)return row->native_variadic;
    if(row->declaration.declaration_binding.owner)
        return ((*body->declarations.identities)[row->default_owner].promises&XR_XIR_FUNCTION_REST)!=0;
    XrHirCallFamily family = row->declaration.family;
    return (family == XR_HIR_CALL_FUNCTION || family == XR_HIR_CALL_CONSTRUCTOR) &&
        ((*body->declarations.identities)[row->declaration.function].promises & XR_XIR_FUNCTION_REST);
}

typedef struct HirBodyCallNames {
    XrHirCallDeclarationNames names;
    const XrNativeMemberDeclaration *native;
} HirBodyCallNames;
static XrXirLiteral body_call_parameter_name(HirBodyCallNames input,uint32_t p) {
    if(!input.native)return input.names.parameters[p];
    const char *name=input.native->parameters[p].name;
    return (XrXirLiteral){name,name?(uint32_t)strlen(name):0};
}
static XrXirStatus body_call_names_receive(XrHirBody *body, HirBodyCallNames source,
    uint32_t offset, XrCompileRequestControl *control, XrXirLiteral **output) {
    XrHirCallDeclarationNames input=source.names;
    if ((input.count && !input.parameters&&!source.native) || offset > input.count) return XR_XIR_BAD_STRUCTURE;
    if (input.count && sizeof(XrXirLiteral) > SIZE_MAX / input.count) return XR_XIR_OVERFLOW;
    size_t bytes = (size_t)input.count * sizeof(XrXirLiteral);
    for (uint32_t p = 0; p < input.count; ++p) {
        if (!(p % 64)) { XrXirStatus status = body_poll(control); if (status != XR_XIR_OK) return status; }
        XrXirLiteral name = body_call_parameter_name(source,p);
        if ((!name.bytes && name.length) ||
            (name.length && memchr(name.bytes, 0, name.length))) return XR_XIR_BAD_STRUCTURE;
        if ((size_t)name.length + 1 > SIZE_MAX - bytes) return XR_XIR_OVERFLOW;
        bytes += (size_t)name.length + 1;
    }
    XrXirLiteral *names = NULL;
    XrXirStatus status = xir_compile_resource_status(xr_compile_resources_alloc(body->resources,
        bytes ? bytes : 1, (void **)&names));
    if (status != XR_XIR_OK) return status;
    char *text = (char *)(names + input.count);
    for (uint32_t p = 0; status == XR_XIR_OK && p < input.count; ++p) {
        if (!(p % 64)) status = body_poll(control);
        XrXirLiteral name = body_call_parameter_name(source,p);
        if (status == XR_XIR_OK) status = body_bytes(text, name.bytes, name.length, control);
        if (status != XR_XIR_OK) break;
        text[name.length] = 0;
        names[p] = (XrXirLiteral){text, name.length};
        text += (size_t)name.length + 1;
    }
    if (status != XR_XIR_OK) { xr_compile_resources_free(names); return status; }
    *output = names;
    return XR_XIR_OK;
}

XR_FUNC XrXirStatus xr_hir_body_call_bind_open(XrHirBody *body, XrHirBodySignature signature,
    XrCompileRequestControl *control, XrHirCallBindingError *error) {
    if (error) *error = (XrHirCallBindingError){0};
    HirBodySignature *found = body_call_binding_row(body, signature);
    if (!found || found->binding_open || found->access_count || found->argument_count)
        return XR_XIR_BAD_STRUCTURE;
    HirBodySignature row = *found;
    if ((row.declaration.family == XR_HIR_CALL_INDIRECT && !row.declaration.declaration_binding.owner) ||
        (row.declaration.family == XR_HIR_CALL_FUNCTION && row.declaration.callable != XR_XIR_UNIT))
        return body_call_binding_error(error, XR_HIR_CALL_BIND_INDIRECT, 0, UINT32_MAX, UINT32_MAX);
    XrHirCallDeclarationNames input = {0};
    const XrNativeMemberDeclaration *native=NULL;
    XrXirStatus status=XR_XIR_OK;
    if(row.native_member) {
        const XrNativeTypeDeclaration *owner=xr_native_declaration_by_id(row.native_owner);
        const XrNativeMemberDeclaration *member=NULL;
        for(uint32_t m=0;owner&&m<owner->member_count;++m) {
            status=body_poll(control);if(status!=XR_XIR_OK)return status;
            if(owner->members[m].id==row.native_member){member=&owner->members[m];break;}
        }
        if(!member||member->parameter_count!=row.declaration.count)
            return XR_XIR_BAD_STRUCTURE;
        for(uint32_t p=0;p<member->parameter_count;++p) {
            const char *name=member->parameters[p].name;
            if(!name||!*name||strlen(name)>UINT32_MAX)return XR_XIR_BAD_STRUCTURE;
        }
        input=(XrHirCallDeclarationNames){NULL,member->parameter_count};native=member;
    } else {
        if (!body->declarations.call_names) return XR_XIR_BAD_STRUCTURE;
        status = body->declarations.call_names(body->declarations.call_names_data,&row.declaration,&input);
    }
    if (status == XR_XIR_BAD_TYPE)
        return body_call_binding_error(error, XR_HIR_CALL_BIND_INDIRECT, 0, UINT32_MAX, UINT32_MAX);
    if (status != XR_XIR_OK) return status;
    if (input.count != row.declaration.count) return XR_XIR_BAD_STRUCTURE;
    XrXirLiteral *names = NULL;
    status = body_call_names_receive(body, (HirBodyCallNames){input,native}, row.declaration.parameter_offset, control, &names);
    uint32_t *arguments = NULL;
    if (status == XR_XIR_OK && input.count)
        status = xir_compile_resource_status(xr_compile_resources_alloc(body->resources,
            (size_t)input.count * sizeof(*arguments), (void **)&arguments));
    if (status == XR_XIR_OK) status = body_poll(control);
    if (status != XR_XIR_OK) {
        xr_compile_resources_free(names); xr_compile_resources_free(arguments); return status;
    }
    for (uint32_t p = 0; p < input.count; ++p) arguments[p] = UINT32_MAX;
    found = body_call_binding_row(body, signature);
    if (!found || found->binding_open) {
        xr_compile_resources_free(names); xr_compile_resources_free(arguments); return XR_XIR_BAD_STRUCTURE;
    }
    found->parameter_names = names; found->formal_arguments = arguments;
    found->next_positional = row.declaration.parameter_offset; found->binding_open = true;
    return XR_XIR_OK;
}

static XrXirStatus body_call_binding_reserve(XrHirBody *body, HirBodySignature *row,
    uint32_t count, XrCompileRequestControl *control) {
    if (count <= row->binding_capacity) return body_poll(control);
    uint32_t capacity = row->binding_capacity ? row->binding_capacity : 4;
    while (capacity < count) {
        if (capacity > UINT32_MAX / 2) { capacity = count; break; }
        capacity *= 2;
    }
    if (sizeof(uint32_t) > SIZE_MAX / capacity) return XR_XIR_OVERFLOW;
    uint32_t *formals = NULL;
    XrXirStatus status = xir_compile_resource_status(xr_compile_resources_alloc(body->resources,
        (size_t)capacity * sizeof(*formals), (void **)&formals));
    if (status == XR_XIR_OK) status = body_bytes(formals, row->argument_formals,
        (size_t)row->binding_count * sizeof(*formals), control);
    if (status != XR_XIR_OK) { xr_compile_resources_free(formals); return status; }
    xr_compile_resources_free(row->argument_formals);
    row->argument_formals = formals; row->binding_capacity = capacity;
    return XR_XIR_OK;
}

XR_FUNC XrXirStatus xr_hir_body_call_bind_argument(XrHirBody *body, XrHirBodySignature signature,
    const char *label, uint32_t length, uint32_t arity, bool spread, XrCompileRequestControl *control,
    uint32_t *first_formal, XrHirCallBindingError *error) {
    if (error) *error = (XrHirCallBindingError){0};
    HirBodySignature *row = body_call_binding_row(body, signature);
    if (!row || !row->binding_open || row->binding_sealed || !first_formal ||
        (!label && length) || (label && !length)) return XR_XIR_BAD_STRUCTURE;
    uint32_t count = row->declaration.count, first = row->next_positional;
    bool rest = body_call_binding_rest(body, row);
    uint32_t argument = row->binding_count;
    if (length) {
        if (spread || arity != 1)
            return body_call_binding_error(error, XR_HIR_CALL_BIND_NAMED_SPREAD, argument, UINT32_MAX, UINT32_MAX);
        for (first = row->declaration.parameter_offset; first < count; ++first) {
            if (!(first % 64)) { XrXirStatus status=body_poll(control); if(status!=XR_XIR_OK)return status; }
            XrXirLiteral name = row->parameter_names[first];
            if (name.length == length && !memcmp(name.bytes, label, length)) break;
        }
        if (first == count)
            return body_call_binding_error(error, XR_HIR_CALL_BIND_UNKNOWN, argument, UINT32_MAX, UINT32_MAX);
        if (rest && first + 1 == count)
            return body_call_binding_error(error, XR_HIR_CALL_BIND_NAMED_REST, argument, first, UINT32_MAX);
    } else if (row->named_seen) {
        return body_call_binding_error(error, XR_HIR_CALL_BIND_POSITIONAL_AFTER_NAMED, argument, UINT32_MAX, UINT32_MAX);
    }
    if (arity > UINT32_MAX - argument || arity > UINT32_MAX - first) return XR_XIR_OVERFLOW;
    if ((!rest && (first > count || arity > count - first)) || (rest && !count))
        return body_call_binding_error(error, XR_HIR_CALL_BIND_TOO_MANY, argument, UINT32_MAX, UINT32_MAX);
    for (uint32_t a = 0; a < arity; ++a) {
        if (!(a % 64)) { XrXirStatus status=body_poll(control); if(status!=XR_XIR_OK)return status; }
        uint32_t p = rest && first + a >= count - 1 ? count - 1 : first + a;
        if (row->formal_arguments[p] != UINT32_MAX && !(rest && p + 1 == count))
            return body_call_binding_error(error, XR_HIR_CALL_BIND_DUPLICATE,
                argument + a, p, row->formal_arguments[p]);
    }
    XrXirStatus status = body_call_binding_reserve(body, row, argument + arity, control);
    if (status != XR_XIR_OK) return status;
    for (uint32_t a = 0; a < arity; ++a) {
        uint32_t p = rest && first + a >= count - 1 ? count - 1 : first + a;
        row->argument_formals[argument + a] = p;
        if (row->formal_arguments[p] == UINT32_MAX) row->formal_arguments[p] = argument + a;
    }
    row->binding_count += arity;
    if (length) row->named_seen = true;
    else row->next_positional += arity;
    *first_formal = first;
    return XR_XIR_OK;
}

static XrXirStatus body_call_binding_default(const XrHirBody *body, const HirBodySignature *row,
    uint32_t p, XrCompileRequestControl *control, bool *output) {
    *output=false;
    if(row->native_member) {
        if(p>=row->declaration.count||!row->native_optional)return XR_XIR_BAD_STRUCTURE;
        *output=row->native_optional[p]!=0;return body_poll(control);
    }
    if (row->declaration.family != XR_HIR_CALL_FUNCTION && row->declaration.family != XR_HIR_CALL_CONSTRUCTOR && !row->declaration.declaration_binding.owner)
        return XR_XIR_OK;
    const XrXirDefaultTable *table = body->declarations.defaults;
    for (uint32_t d = 0; table && d < table->count; ++d) {
        if (!(d % 64)) { XrXirStatus status=body_poll(control); if(status!=XR_XIR_OK)return status; }
        XrXirDefaultBinding binding = table->records[d];
        if (binding.owner_kind == XR_XIR_DEFAULT_PARAMETER && binding.owner == row->default_owner &&
            binding.ordinal == p + row->default_parameter_offset) { *output=true; return XR_XIR_OK; }
    }
    return XR_XIR_OK;
}

XR_FUNC XrXirStatus xr_hir_body_call_bind_seal(XrHirBody *body, XrHirBodySignature signature,
    XrCompileRequestControl *control, XrHirCallBinding *output, XrHirCallBindingError *error) {
    if (error) *error = (XrHirCallBindingError){0};
    HirBodySignature *row = body_call_binding_row(body, signature);
    if (!row || !row->binding_open || row->binding_sealed || !output) return XR_XIR_BAD_STRUCTURE;
    bool rest = body_call_binding_rest(body, row);
    for (uint32_t p = row->declaration.parameter_offset; p < row->declaration.count; ++p) {
        if (!(p % 64)) { XrXirStatus status = body_poll(control); if (status != XR_XIR_OK) return status; }
        if (row->formal_arguments[p] != UINT32_MAX || (rest && p + 1 == row->declaration.count)) continue;
        bool defaulted=false;
        XrXirStatus status=body_call_binding_default(body,row,p,control,&defaulted);
        if(status!=XR_XIR_OK)return status;
        if (!defaulted)
            return body_call_binding_error(error, XR_HIR_CALL_BIND_MISSING, UINT32_MAX, p, UINT32_MAX);
    }
    row->binding_sealed = true;
    *output = (XrHirCallBinding){row->argument_formals, row->formal_arguments,
                               row->binding_count, row->declaration.count};
    return XR_XIR_OK;
}

XR_FUNC bool xr_hir_body_call_binding(const XrHirBody *body, XrHirBodySignature signature,
                                    XrHirCallBinding *output) {
    if (!body || body->ended || !output || signature.owner != body || signature.index >= body->signatures.count)
        return false;
    HirBodySignature row = ((HirBodySignature *)body->signatures.items)[signature.index];
    if (!row.binding_sealed) return false;
    *output = (XrHirCallBinding){row.argument_formals, row.formal_arguments,
                               row.binding_count, row.declaration.count};
    return true;
}
