/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_instance_buffer_origin.inc.c - Actual private byte-borrow return sources
 *
 * KEY CONCEPT:
 *   Sealed current construction rows select a real receiver field. Neither a
 *   Storage-shaped parameter nor a retained physical view invents that proof.
 */
static bool instance_buffer_view_work(XrXirValueAdmission *admission,uint64_t work) {
    if (!admission || work>admission->work || !admission->domain || !xr_xir_domain_work(admission->domain,work)) return false;
    admission->work-=work;return true;
}
XR_FUNC XrXirCallStatus xir_instance_buffer_view_recheck(const XrXirProgram *program,
    const XrXirInstance *instance,const XirBufferViewOriginQuery *query,
    uint32_t *parameter,uint32_t *field,XrXirValue *output) {
    if (!program || !query || !query->metadata || !parameter || !field || !output ||
        !value_unit(*output) || !query->admission)
        return XR_XIR_CALL_BAD_ARGUMENT;
    const XrXirCallMetadata *metadata=query->metadata;uint32_t target=query->target,count=query->count;
    const XrXirValue *arguments=query->arguments;const XrXirCellAuthority *authority=query->authority;
    XrXirValueAdmission *admission=query->admission;
    *parameter=UINT32_MAX;*field=UINT32_MAX;
    bool program_table=metadata->entries==program->entries && metadata->entry_count==program->entry_count;
    bool instance_table=instance && metadata->entries==instance->entries &&
        (uint64_t)metadata->entry_count==(uint64_t)program->entry_count+1u;
    if (!instance || !instance_owner_current(instance) || instance->program!=program ||
        metadata->admission_context!=instance ||
        (!program_table && !instance_table && !xir_call_buffer_metadata_owned(query)) ||
        admission->domain!=instance->domain || metadata->arena!=program->arena ||
        metadata->cell_role!=instance_cell_role || metadata->cell_context!=program || target>=program->entry_count)
        return XR_XIR_CALL_BAD_ARGUMENT;
    const XrXirProgramInput *input=program->permissions?program->permissions->input:NULL;
    const XrXirArtifact *artifact=input?input->artifact:NULL;
    if (!artifact || artifact->context.resources!=program->context.resources ||
        artifact->module.stage!=XR_XIR_LOWERED || artifact->module.function_count!=program->entry_count ||
        artifact->program_final!=program->permissions->effects || artifact->layouts!=program->permissions->layouts ||
        !artifact->module.declarations)
        return XR_XIR_CALL_BAD_STATE;
    const XrXirCallEntry *entry=&metadata->entries[target],*sealed=&program->entries[target];
    if (entry->parameter_count!=sealed->parameter_count ||
        entry->result!=sealed->result || entry->resume!=sealed->resume || entry->release!=sealed->release ||
        entry->state_bytes!=sealed->state_bytes || entry->environment!=sealed->environment ||
        entry->flags!=sealed->flags || entry->cleanup_owner!=sealed->cleanup_owner)
        return XR_XIR_CALL_BAD_ARGUMENT;
    const XrXirFunctionIdentity *identity=&program->declarations->functions[target];
    const XrXirTypeNode *result=xr_xir_type_node(program->types,entry->result);
    if (!result || result->kind!=XR_XIR_TYPE_SLICE || result->element!=XR_XIR_U8)
        return XR_XIR_CALL_READY;
    if (count!=entry->parameter_count || (count && !arguments)) return XR_XIR_CALL_BAD_ARGUMENT;
    for (uint32_t p=0;p<entry->parameter_count;++p) {
        if (!instance_buffer_view_work(admission,1)) return XR_XIR_CALL_LIMIT;
        if (!entry->parameters || !sealed->parameters || entry->parameters[p]!=sealed->parameters[p])
            return XR_XIR_CALL_BAD_ARGUMENT;
    }
    XrXirValue receiver={0};XrXirValueStatus status=XR_XIR_VALUE_OK;
    if (identity->native_callable==XR_NATIVE_CALLABLE_BUFFER_AS_BYTES) {
        if (!instance_buffer_view_work(admission,(uint64_t)program->declarations->module_count+1u))
            return XR_XIR_CALL_LIMIT;
        if (xir_native_callable_verify(&artifact->module,target)!=XR_XIR_OK ||
            entry->resume!=xr_xir_instance_native_callable || entry->release!=xr_xir_instance_native_callable_release ||
            entry->state_bytes!=sizeof(XrXirValue) || entry->environment || entry->flags || entry->cleanup_owner ||
            !xir_native_callable_types(identity->native_callable,program->types,entry->parameters,
                count,entry->result,xir_native_callable_error(program->types,identity->native_callable)))
            return XR_XIR_CALL_BAD_ARGUMENT;
        *parameter=0;status=xr_xir_value_copy(&arguments[0],output);
    } else {
        const XrXirBorrowReturns *rows=xr_xir_compile_construction_borrow_returns(artifact->construction);
        const XrXirBorrowReturn *row=NULL;
        if (!rows || (rows->count && !rows->rows)) return XR_XIR_CALL_BAD_STATE;
        for (uint32_t i=0;i<rows->count;++i) {
            if (!instance_buffer_view_work(admission,1)) return XR_XIR_CALL_LIMIT;
            if (rows->rows[i].function!=target) continue;
            if (row) return XR_XIR_CALL_BAD_ARGUMENT;
            row=&rows->rows[i];
        }
        if (!row) return XR_XIR_CALL_READY;
        if (row->receiver>=count || row->native_callable!=XR_NATIVE_CALLABLE_BUFFER_AS_BYTES ||
            identity->nominal_owner!=row->nominal+1u || identity->method_kind!=XR_XIR_READ_METHOD)
            return XR_XIR_CALL_BAD_ARGUMENT;
        XrXirType expected=entry->parameters[row->receiver];
        const XrXirTypeNode *node=xr_xir_type_node(program->types,expected);
        bool cell=node && node->kind==XR_XIR_TYPE_CELL;
        if (cell) {expected=node->element;node=xr_xir_type_node(program->types,expected);}
        if (!node || !xr_xir_type_is_class(program->types,expected) || node->nominal.declaration!=row->nominal ||
            row->field>=node->nominal.field_count || node->nominal.fields[row->field]!=XR_XIR_BUFFER_STORAGE)
            return XR_XIR_CALL_BAD_ARGUMENT;
        if (!xr_xir_value_argument(&arguments[row->receiver],program->arena,entry->parameters[row->receiver]))
            return XR_XIR_CALL_BAD_ARGUMENT;
        status=cell ? xr_xir_cell_authorized_read(&arguments[row->receiver],authority,&receiver) :
            xr_xir_value_copy(&arguments[row->receiver],&receiver);
        if (status==XR_XIR_VALUE_OK && !xr_xir_value_argument(&receiver,program->arena,expected))
            status=XR_XIR_VALUE_BAD_ARGUMENT;
        if (status==XR_XIR_VALUE_OK) status=xr_xir_class_get(&receiver,row->field,admission,output);
        xr_xir_value_drop(&receiver);
        *parameter=row->receiver;*field=row->field;
    }
    if (status==XR_XIR_VALUE_OK) status=xr_xir_value_admit(output,XR_XIR_BUFFER_STORAGE,admission);
    if (status!=XR_XIR_VALUE_OK) xr_xir_value_drop(output);
    return status==XR_XIR_VALUE_OK ? XR_XIR_CALL_READY : status==XR_XIR_VALUE_OOM ? XR_XIR_CALL_OOM :
        status==XR_XIR_VALUE_LIMIT || status==XR_XIR_VALUE_REFCOUNT_LIMIT ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_BAD_ARGUMENT;
}
XR_FUNC XrXirCallStatus xir_instance_buffer_view_origin(XrXirCallView *view,
    const XirBufferViewOriginQuery *query,XrXirProgram **owner,
    uint32_t *parameter,uint32_t *field,XrXirValue *output) {
    if (!owner || *owner || !query || !query->metadata || !view || query->activation!=view->activation ||
        query->admission!=xr_xir_call_admission(view))
        return XR_XIR_CALL_BAD_ARGUMENT;
    const XrXirCallMetadata *metadata=query->metadata;
    XrXirInstance *instance=view_instance(view);
    if (!instance) return XR_XIR_CALL_READY;
    XrXirInstance *root=instance_root(instance),*selected=NULL;
    if (metadata->admission_context==instance) selected=instance;
    else if (metadata->admission_context==root) selected=root;
    else for (XrXirInstance *state=root->generation_next;state;state=state->generation_next) {
        if (!instance_cell_work(view,1)) return XR_XIR_CALL_LIMIT;
        if (metadata->admission_context==state) {selected=state;break;}
    }
    if (!selected) return XR_XIR_CALL_READY;
    if (selected->domain!=instance->domain || !selected->program) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirCallStatus status=xir_instance_buffer_view_recheck(selected->program,selected,query,parameter,field,output);
    if (status==XR_XIR_CALL_READY && output->type) {
        if (!xr_xir_compile_program_retain(selected->program)) {xr_xir_value_drop(output);return XR_XIR_CALL_LIMIT;}
        *owner=selected->program;
    }
    return status;
}
