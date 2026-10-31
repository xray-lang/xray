/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_public_slot_authority.h - Direct value reads with independent permissions
 *
 * KEY CONCEPT:
 *   Complete real Source inputs qualify a foreign load, while the common
 *   checker rejects private facts, missing imports and place substitution.
 */
#ifndef XIR_PUBLIC_SLOT_AUTHORITY_H
#define XIR_PUBLIC_SLOT_AUTHORITY_H
static void ci_direct_slot_authority(const XrXirCompileContext *context,const XrXirArtifact *producer) {
    const XrXirModule *module=xr_xir_compile_artifact_module(producer);
    const XrXirConstruction *original=xr_xir_compile_artifact_construction(producer);
    uint32_t f=ci_entry(module,"all"),selected=UINT32_MAX;
    const XrXirFunction *function=&module->functions[f];
    for(uint32_t i=0;i<function->instruction_count;++i) {
        const XrXirInstruction *op=&function->instructions[i];
        if(op->op==XR_XIR_SLOT_LOAD && module->declarations->slots[op->immediate].module!=module->declarations->functions[f].module) {
            if(selected==UINT32_MAX)selected=i;
        }
    }
    CHECK(selected!=UINT32_MAX);
    uint32_t slot=(uint32_t)function->instructions[selected].immediate;
    XrXirArtifact *received=NULL;
    CHECK(xr_xir_compile_recheck_v2(context,module,original,&received,NULL,NULL)==XR_XIR_OK && received);
    xr_xir_compile_artifact_free(received);received=NULL;
    const XrXirSlotFacts *slots=xr_xir_compile_construction_slots(original);
    CHECK(slots && slots->count==module->declarations->slot_count && slots->rows[slot].exported==1 && !module->declarations->slots[slot].mutable);
    XrXirSlotFact *rows=NULL;
    CHECK(xr_compile_resources_alloc(context->resources,slots->count*sizeof(*rows),(void **)&rows)==XR_COMPILE_RESOURCE_OK);
    memcpy(rows,slots->rows,slots->count*sizeof(*rows));rows[slot].exported=0;
    XrXirSlotFacts private_slots={rows,slots->count};
    XrXirConstructionSpec spec={original->rows,original->count,original->materializations,original->materialization_count,&original->comptime};
    XrXirConstruction *private_owner=NULL;
    CHECK(xr_xir_compile_construction_new_v4(context,module->types,&spec,&private_slots,&private_owner,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_recheck_v2(context,module,private_owner,&received,NULL,NULL)==XR_XIR_BAD_STRUCTURE && !received);
    xr_xir_compile_construction_free(private_owner);private_owner=NULL;
    CHECK(xr_xir_compile_recheck_v2(context,module,NULL,&received,NULL,NULL)==XR_XIR_BAD_STRUCTURE && !received);
    XrXirFunction *functions=NULL;XrXirInstruction *ops=NULL;
    CHECK(xr_compile_resources_alloc(context->resources,module->function_count*sizeof(*functions),(void **)&functions)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_alloc(context->resources,function->instruction_count*sizeof(*ops),(void **)&ops)==XR_COMPILE_RESOURCE_OK);
    memcpy(functions,module->functions,module->function_count*sizeof(*functions));
    memcpy(ops,function->instructions,function->instruction_count*sizeof(*ops));functions[f].instructions=ops;
    XrXirModule current=*module;current.functions=functions;
    const XrXirOp writes[]={XR_XIR_SLOT_PLACE,XR_XIR_SLOT_INIT,XR_XIR_SLOT_STORE};
    for(size_t i=0;i<sizeof(writes)/sizeof(*writes);++i) {
        ops[selected].op=writes[i];
        CHECK(xr_xir_compile_recheck_v2(context,&current,original,&received,NULL,NULL)==XR_XIR_BAD_STRUCTURE && !received);
    }
    ops[selected]=function->instructions[selected];
    uint32_t mutable_slot=UINT32_MAX;
    for(uint32_t i=0;i<slots->count;++i)
        if(module->declarations->slots[i].module==module->declarations->slots[slot].module &&
            module->declarations->slots[i].mutable)mutable_slot=i;
    CHECK(mutable_slot!=UINT32_MAX);
    memcpy(rows,slots->rows,slots->count*sizeof(*rows));rows[mutable_slot].exported=1;
    CHECK(xr_xir_compile_construction_new_v4(context,module->types,&spec,&private_slots,&private_owner,NULL)==XR_XIR_OK);
    ops[selected].immediate=mutable_slot;ops[selected].type=module->declarations->slots[mutable_slot].type;
    CHECK(xr_xir_compile_recheck_v2(context,&current,private_owner,&received,NULL,NULL)==XR_XIR_BAD_STRUCTURE && !received);
    xr_xir_compile_construction_free(private_owner);xr_compile_resources_free(rows);
    ops[selected]=function->instructions[selected];
    XrXirSourceModule *modules=NULL;
    CHECK(xr_compile_resources_alloc(context->resources,module->declarations->module_count*sizeof(*modules),(void **)&modules)==XR_COMPILE_RESOURCE_OK);
    memcpy(modules,module->declarations->modules,module->declarations->module_count*sizeof(*modules));
    uint32_t from=module->declarations->functions[f].module;
    uint32_t dependencies[64],count=0,target=module->declarations->slots[slot].module;
    CHECK(modules[from].dependency_count<=64);
    for(uint32_t i=0;i<modules[from].dependency_count;++i)
        if(modules[from].dependencies[i]!=target)dependencies[count++]=modules[from].dependencies[i];
    CHECK(count+1==modules[from].dependency_count && count);
    modules[from].dependencies=dependencies;modules[from].dependency_count=count;
    XrXirDeclarations declarations=*module->declarations;declarations.modules=modules;current.declarations=&declarations;
    CHECK(!xr_xir_module_imports(&declarations,from,target));
    bool transitive=false;
    for(uint32_t m=0;m<declarations.module_count;++m)
        for(uint32_t n=0;n<declarations.module_count;++n)
            if(xr_xir_module_imports(&declarations,from,m) && xr_xir_module_imports(&declarations,m,n) &&
                xr_xir_module_imports(&declarations,n,target))transitive=true;
    CHECK(transitive);
    CHECK(xr_xir_compile_recheck_v2(context,&current,original,&received,NULL,NULL)==XR_XIR_BAD_STRUCTURE && !received);
    current.declarations=module->declarations;
    CHECK(xr_xir_compile_recheck_v2(context,&current,original,&received,NULL,NULL)==XR_XIR_OK && received);
    xr_xir_compile_artifact_free(received);xr_compile_resources_free(modules);xr_compile_resources_free(ops);xr_compile_resources_free(functions);
}
#endif // XIR_PUBLIC_SLOT_AUTHORITY_H
