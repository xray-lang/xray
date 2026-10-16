/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_instance_binding.inc.c - Transactional physical declaration bindings
 */
struct XrXirInstanceBinding {
    XrXirInstance *birth;
    const char *symbol;
    uint32_t length, kind, ordinal;
};
typedef struct XirInstanceBindingTable {
    uint64_t bytes;
    uint32_t count;
    XrXirInstanceBinding *rows;
} XirInstanceBindingTable;
_Static_assert(_Alignof(XirInstanceBindingTable)>=_Alignof(XrXirInstanceBinding),
    "binding rows follow an equally aligned header");

static void instance_bindings_free(XrXirInstance *instance) {
    if (!instance->symbol_bindings) return;
    xr_xir_domain_metadata_deallocate(instance->domain,instance->symbol_bindings,
        instance->symbol_bindings->bytes);
    instance->symbol_bindings=NULL;
}
static bool instance_binding_declaration(const XrXirProgram *program,uint32_t kind,uint32_t ordinal) {
    const XrXirDeclarations *d=program->declarations;
    if (kind==XR_XIR_INSTANCE_BINDING_SLOT)
        return ordinal<d->slot_count && program->active_modules[d->slots[ordinal].module];
    if (kind!=XR_XIR_INSTANCE_BINDING_FUNCTION || ordinal>=program->entry_count) return false;
    const XrXirFunctionIdentity *identity=&d->functions[ordinal];
    return program->active_modules[identity->module] && !identity->cleanup_owner &&
        !identity->test_role && ordinal!=d->modules[identity->module].initializer;
}
static bool instance_bindings_ready(const XrXirInstance *instance) {
    const XirInstanceBindingTable *table=instance->symbol_bindings;
    if (!table) return true;
    const XrXirProgram *program=instance->program;
    const XrXirDeclarations *d=program->declarations;
    /* Each bounded observation and retirement was prepaid before the Call. */
    for (uint32_t i=0;i<table->count;++i) {
        const XrXirInstanceBinding *binding=&table->rows[i];
        if (binding->birth!=instance || !instance_binding_declaration(program,binding->kind,binding->ordinal))
            return false;
        uint32_t module=binding->kind==XR_XIR_INSTANCE_BINDING_SLOT?
            d->slots[binding->ordinal].module:d->functions[binding->ordinal].module;
        if (!instance->ready[module]) return false;
        if (binding->kind!=XR_XIR_INSTANCE_BINDING_SLOT) continue;
        uint32_t slot=binding->ordinal;
        /* Slot publication already admitted the exact Program storage. Rewalking
         * a value graph here would introduce unbounded work into the commit. */
        if (!instance->published[slot] || instance->slots[slot].type!=(uint32_t)d->slots[slot].type) return false;
    }
    return true;
}
XR_FUNC XrXirCallStatus xr_xir_instance_generation_bind(XrXirInstance *instance,uint64_t generation,
    const XrXirInstanceBindingSpec *bindings,uint32_t count) {
    if (!count || !bindings) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirCallStatus status=generation_owner(instance);
    if (status!=XR_XIR_CALL_READY) return status;
    XrXirInstance *candidate=instance->prepared_generation;
    if (!generation || !candidate || candidate->generation_id!=generation || candidate->call ||
        candidate->symbol_bindings) return XR_XIR_CALL_BAD_STATE;
    uint64_t maximum=(uint64_t)candidate->program->entry_count+candidate->program->declarations->slot_count;
    if (count>maximum) return XR_XIR_CALL_BAD_ARGUMENT;
    uint64_t names=0;
    for (uint32_t i=0;i<count;++i) {
        if (!xr_xir_domain_work(instance->domain,8)) return XR_XIR_CALL_LIMIT;
        const XrXirInstanceBindingSpec *spec=&bindings[i];
        if (!spec->symbol || !spec->length ||
            !instance_binding_declaration(candidate->program,spec->kind,spec->ordinal))
            return XR_XIR_CALL_BAD_ARGUMENT;
        names+=spec->length;
    }
    uint64_t base=sizeof(XirInstanceBindingTable)+(uint64_t)count*sizeof(XrXirInstanceBinding);
    if (names>SIZE_MAX || base>SIZE_MAX-names) return XR_XIR_CALL_LIMIT;
    uint64_t bytes=base+names;
    instance->driving=true;
    XirInstanceBindingTable *table=instance_metadata_allocate(instance->domain,bytes,&status);
    if (table) {
        table->bytes=bytes;table->count=count;table->rows=(XrXirInstanceBinding *)(table+1);
        char *next=(char *)(table->rows+count);uint64_t remaining=names;
        for (uint32_t i=0;i<count && status==XR_XIR_CALL_READY;++i) {
            XrXirInstanceBindingSpec spec=bindings[i];
            if (!spec.symbol || !spec.length || spec.length>remaining ||
                !instance_binding_declaration(candidate->program,spec.kind,spec.ordinal)) {
                status=XR_XIR_CALL_BAD_ARGUMENT;break;
            }
            if (!xr_xir_domain_work(instance->domain,(uint64_t)spec.length+8)) {
                status=XR_XIR_CALL_LIMIT;break;
            }
            for (uint32_t j=0;j<i;++j) {
                if (!xr_xir_domain_work(instance->domain,1)) {status=XR_XIR_CALL_LIMIT;break;}
                if (table->rows[j].length!=spec.length) continue;
                if (!xr_xir_domain_work(instance->domain,spec.length)) {status=XR_XIR_CALL_LIMIT;break;}
                if (!memcmp(table->rows[j].symbol,spec.symbol,spec.length)) {
                    status=XR_XIR_CALL_BAD_ARGUMENT;break;
                }
            }
            if (status!=XR_XIR_CALL_READY) break;
            memcpy(next,spec.symbol,spec.length);
            table->rows[i]=(XrXirInstanceBinding){candidate,next,spec.length,spec.kind,spec.ordinal};
            next+=spec.length;remaining-=spec.length;
        }
        if (status==XR_XIR_CALL_READY) {
            candidate->symbol_bindings=table;
            candidate->metadata_bytes=xr_xir_domain_budget_stats(instance->domain).metadata_live;
        }
        else xr_xir_domain_metadata_deallocate(instance->domain,table,bytes);
    }
    instance->driving=false;
    return status;
}
static XrXirInstance *instance_binding_next(XrXirInstance *root,XrXirInstance *state) {
    if (!state) return root->generation_next?root->generation_next:root;
    return state==root?NULL:state->generation_next?state->generation_next:root;
}
static bool instance_binding_work(XrXirInstance *root,XrXirCallView *view,uint64_t units) {
    return view?instance_cell_work(view,units):xr_xir_domain_work(root->domain,units);
}
static XrXirCallStatus instance_binding_find(XrXirInstance *root,const XrXirInstanceBinding *binding,
    XrXirCallView *view,XrXirInstance **birth) {
    if (!binding) return XR_XIR_CALL_BAD_ARGUMENT;
    for (XrXirInstance *state=instance_binding_next(root,NULL);state;state=instance_binding_next(root,state)) {
        if (!instance_binding_work(root,view,1)) return XR_XIR_CALL_LIMIT;
        if (!state->bindings_published || !state->symbol_bindings) continue;
        for (uint32_t i=0;i<state->symbol_bindings->count;++i) {
            if (!instance_binding_work(root,view,1)) return XR_XIR_CALL_LIMIT;
            if (&state->symbol_bindings->rows[i]==binding) { *birth=state;return XR_XIR_CALL_READY; }
        }
    }
    return XR_XIR_CALL_BAD_ARGUMENT;
}
XR_FUNC XrXirCallStatus xr_xir_instance_binding_lookup(XrXirInstance *instance,const char *symbol,
    uint32_t length,const XrXirInstanceBinding **output) {
    if (!symbol || !length || !output || *output) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirCallStatus status=generation_owner(instance);
    if (status!=XR_XIR_CALL_READY) return status;
    for (XrXirInstance *state=instance_binding_next(instance,NULL);state;state=instance_binding_next(instance,state)) {
        if (!xr_xir_domain_work(instance->domain,1)) return XR_XIR_CALL_LIMIT;
        if (!state->bindings_published || !state->symbol_bindings) continue;
        for (uint32_t i=0;i<state->symbol_bindings->count;++i) {
            if (!xr_xir_domain_work(instance->domain,1)) return XR_XIR_CALL_LIMIT;
            const XrXirInstanceBinding *binding=&state->symbol_bindings->rows[i];
            if (length!=binding->length) continue;
            if (!xr_xir_domain_work(instance->domain,length)) return XR_XIR_CALL_LIMIT;
            if (!memcmp(symbol,binding->symbol,length)) { *output=binding;return XR_XIR_CALL_READY; }
        }
    }
    return XR_XIR_CALL_BAD_STATE;
}
XR_FUNC XrXirCallStatus xr_xir_instance_binding_view(XrXirInstance *instance,
    const XrXirInstanceBinding *binding,XrXirInstanceBindingView *output) {
    if (!output || output->generation || output->program || output->arena || output->types || output->symbol ||
        output->length || output->kind || output->ordinal || output->slot_type || output->mutable ||
        output->parameters || output->parameter_count || output->result) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirCallStatus status=generation_owner(instance);
    if (status!=XR_XIR_CALL_READY) return status;
    XrXirInstance *birth=NULL;status=instance_binding_find(instance,binding,NULL,&birth);
    if (status!=XR_XIR_CALL_READY) return status;
    XrXirInstanceBindingView result={.generation=birth->generation_id,.program=birth->program,
        .arena=birth->program->arena,.types=birth->program->types,.symbol=binding->symbol,
        .length=binding->length,.kind=binding->kind,.ordinal=binding->ordinal};
    if (binding->kind==XR_XIR_INSTANCE_BINDING_SLOT) {
        const XrXirSlot *slot=&birth->program->declarations->slots[binding->ordinal];
        result.slot_type=slot->type;result.mutable=slot->mutable;
    } else {
        const XrXirCallEntry *entry=&birth->program->entries[binding->ordinal];
        result.parameters=entry->parameters;result.parameter_count=entry->parameter_count;result.result=entry->result;
    }
    *output=result;return XR_XIR_CALL_READY;
}
XR_FUNC XrXirCallStatus xr_xir_instance_binding_start(XrXirInstance *instance,
    const XrXirInstanceBinding *binding,const XrXirValue *arguments,uint32_t count) {
    XrXirCallStatus status=generation_owner(instance);
    if (status!=XR_XIR_CALL_READY) return status;
    if (instance->prepared_generation) return XR_XIR_CALL_BUSY;
    XrXirInstance *birth=NULL;status=instance_binding_find(instance,binding,NULL,&birth);
    if (status!=XR_XIR_CALL_READY) return status;
    if (binding->kind!=XR_XIR_INSTANCE_BINDING_FUNCTION) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirValueAdmission admission=instance_candidate_admission(birth);
    return instance_start(birth,binding->ordinal,arguments,count,NULL,&admission,false);
}
XR_FUNC XrXirCallStatus xr_xir_instance_binding_slot_read(XrXirCallView *view,
    const XrXirInstanceBinding *binding,XrXirValue *output) {
    if (!instance_view_current(view)) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirInstance *current=view_instance(view);
    if (!current) return XR_XIR_CALL_BAD_STATE;
    XrXirInstance *birth=NULL;
    XrXirCallStatus status=instance_binding_find(instance_root(current),binding,view,&birth);
    if (status!=XR_XIR_CALL_READY) return status;
    if (binding->kind!=XR_XIR_INSTANCE_BINDING_SLOT) return XR_XIR_CALL_BAD_ARGUMENT;
    if (current!=birth) return XR_XIR_CALL_BAD_STATE;
    return xr_xir_instance_slot_read(view,binding->ordinal,output);
}
