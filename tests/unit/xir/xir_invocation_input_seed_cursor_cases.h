/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_invocation_input_seed_cursor_cases.h - Independent current primitive replay
 */
#ifndef XIR_INVOCATION_INPUT_SEED_CURSOR_CASES_H
#define XIR_INVOCATION_INPUT_SEED_CURSOR_CASES_H
/* Preserve the complete original random reader as an independent test oracle. */
static XrXirStatus seed_cursor_random_oracle(const XrXirCompileContext *work,
    const EffectBodyMaterial *body,uint32_t function,XrXirRootEffects *output) {
    if (!xir_compile_context_valid(work) || !effect_body_material_shape(body) || !output ||
        body->resources!=work->resources || function>=effect_body_count(body)) return XR_XIR_BAD_STRUCTURE;
    const EffectBodyFunction *head=&body->functions[function];
    const XrXirDeclarations *decls=body->declarations;
    XrXirRootEffects facts={0};XrXirRootEffectWitness witnesses[2]={{0},{0}};
    if (decls) {
        if (!xir_compile_work(work,4)) return XR_XIR_BUDGET;
        if (!decls->modules || !decls->functions || decls->functions[function].module>=decls->module_count)
            return XR_XIR_BAD_STRUCTURE;
        if (decls->modules[decls->functions[function].module].initializer==function) {
            facts.requires_root=true;
            witnesses[0]=(XrXirRootEffectWitness){XR_XIR_ROOT_CAUSE_INITIALIZER,
                UINT32_MAX,UINT32_MAX,UINT32_MAX,0};
        }
    }
    EffectBodyRead read=effect_body_compile_read(work);
    for (uint32_t i=0;i<head->instruction_count;++i) {
        XrXirInstruction op;XrXirStatus status=effect_body_instruction_get(&read,head,i,&op);
        if (status!=XR_XIR_OK) return status;
        if (op.op==XR_XIR_CALL_INDIRECT || op.op==XR_XIR_INVOKE_INDIRECT) continue;
        if (op.op==XR_XIR_SLOT_LOAD || op.op==XR_XIR_SLOT_PLACE || op.op==XR_XIR_SLOT_STORE) {
            if (!decls || !decls->slots || op.immediate<0 || (uint64_t)op.immediate>=decls->slot_count)
                return XR_XIR_BAD_STRUCTURE;
        }
        const EffectRootSeedRecord request={&body->domain,&op,function,i,XR_XIR_UNIT};
        status=effect_root_seed_output(&request,&facts,&witnesses[0],&witnesses[1],work);
        if (status!=XR_XIR_OK) return status;
        if (op.op==XR_XIR_CALL_REQUIREMENT) { facts.unresolved=false;witnesses[1]=(XrXirRootEffectWitness){0}; }
    }
    if (!xir_compile_work(work,sizeof(*output))) return XR_XIR_BUDGET;
    *output=facts;return XR_XIR_OK;
}

typedef struct SeedCursorPhysical {
    EffectBodyMaterial body;
    EffectBodyFunction function;
    XrXirTypeNode callable;
    XrXirInterfaceMethod method;
    XrXirInterfaceDeclaration interface;
    XrXirInterfaceTable interfaces;
    XrXirSourceModule module;
    XrXirFunctionIdentity identity;
    XrXirSlot slot;
    XrXirDeclarations declarations;
} SeedCursorPhysical;
/* This storage-only material is never published as an artifact or authority. */
static void seed_cursor_physical_init(const XrXirCompileContext *context,SeedCursorPhysical *p) {
    memset(p,0,sizeof(*p));p->body.resources=context->resources;p->body.references=1;
    p->body.functions=&p->function;p->body.domain.function_count=1;
    p->body.domain.types=&p->body.terms.types;
    p->callable=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_I64,.flags=XR_XIR_CALLABLE_ROOT_NONE};
    p->method=(XrXirInterfaceMethod){.signature=(XrXirType)256};
    p->interface=(XrXirInterfaceDeclaration){.methods=&p->method,.method_count=1};
    p->interfaces=(XrXirInterfaceTable){&p->interface,1};
    p->body.terms.types=(XrXirTypes){.nodes=&p->callable,.count=1,.interfaces=&p->interfaces};
    p->module.initializer=UINT32_MAX;p->slot=(XrXirSlot){.type=XR_XIR_I64,.mutable=1};
    p->declarations=(XrXirDeclarations){.modules=&p->module,.module_count=1,.functions=&p->identity,.slots=&p->slot,.slot_count=1};
    p->body.declarations=&p->declarations;p->body.domain.declarations=&p->declarations;
    CHECK(effect_body_material_shape(&p->body));
}
static bool seed_cursor_equal(XrXirRootEffects a,XrXirRootEffects b) {
    return a.requires_root==b.requires_root && a.unresolved==b.unresolved;
}
static XrXirStatus seed_cursor_compare(const XrXirCompileContext *context,
    const EffectBodyMaterial *body,uint32_t function,XrXirRootEffects *value) {
    const XrXirRootEffects sentinel={true,true};XrXirRootEffects original=sentinel,current=sentinel;
    RootParameterMark held=rp_mark();uint64_t live=rp_stats(context).live_bytes;size_t attempts=rp_attempts;
    XrXirStatus before=seed_cursor_random_oracle(context,body,function,&original);
    XrXirStatus status=effect_invocation_input_seed(context,body,function,&current);
    CHECK(status==before && seed_cursor_equal(original,current));
    if(status!=XR_XIR_OK)CHECK(seed_cursor_equal(current,sentinel));
    CHECK(rp_attempts==attempts && rp_stats(context).live_bytes==live);rp_balanced(held);
    if(value)*value=current;return status;
}
static void seed_cursor_opcode_controls(void) {
    RootParameterMark held=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());uint64_t live=rp_stats(&context).live_bytes;
    SeedCursorPhysical p;seed_cursor_physical_init(&context,&p);XrXirInstruction op={.op=XR_XIR_RETURN};
    XrXirFunction source={.instructions=&op,.instruction_count=1};p.function.instruction_count=1;
    CHECK(effect_body_records_copy(&context,&source,&p.function)==XR_XIR_OK && p.function.run_count==1);
    for(unsigned code=1;code<XR_XIR_OP_COUNT;++code){p.function.runs[0].base=(XrXirInstruction){.op=(XrXirOp)code};
        XrXirRootEffects value={0};CHECK(seed_cursor_compare(&context,&p.body,0,&value)==XR_XIR_OK);
        bool root=code==XR_XIR_SLOT_LOAD || code==XR_XIR_SLOT_PLACE || code==XR_XIR_SLOT_STORE ||
            code==XR_XIR_SLOT_INIT || code==XR_XIR_SLOT_GROUP_INIT;
        CHECK(value.requires_root==root && !value.unresolved);}
    p.function.runs[0].base=(XrXirInstruction){.op=XR_XIR_CALL_REQUIREMENT};
    const uint32_t flags[]={XR_XIR_CALLABLE_ROOT_NONE,XR_XIR_CALLABLE_ROOT_REQUIRED,XR_XIR_CALLABLE_ROOT_UNRESOLVED};
    for(unsigned i=0;i<3;++i){p.callable.flags=flags[i];XrXirRootEffects value={0};
        CHECK(seed_cursor_compare(&context,&p.body,0,&value)==XR_XIR_OK &&
            value.requires_root==(flags[i]==XR_XIR_CALLABLE_ROOT_REQUIRED) && !value.unresolved);}
    p.callable.flags=XR_XIR_CALLABLE_ROOT_NONE;p.module.initializer=0;
    p.function.runs[0].base=(XrXirInstruction){.op=XR_XIR_CONST_INT};XrXirRootEffects value={0};
    CHECK(seed_cursor_compare(&context,&p.body,0,&value)==XR_XIR_OK && value.requires_root && !value.unresolved);
    p.module.initializer=UINT32_MAX;p.function.runs[0].base=(XrXirInstruction){.op=XR_XIR_SLOT_LOAD};p.slot.mutable=0;
    CHECK(seed_cursor_compare(&context,&p.body,0,&value)==XR_XIR_OK && !value.requires_root && !value.unresolved);
    p.slot.mutable=1;
    for(unsigned fault=0;fault<11;++fault){
        p.function.runs[0].base=(XrXirInstruction){.op=XR_XIR_SLOT_LOAD};
        if(!fault)p.function.runs[0].base.immediate=-1;else if(fault==1)p.function.runs[0].base.immediate=1;
        else if(fault==2)p.declarations.slots=NULL;else if(fault==3)p.declarations.modules=NULL;
        else if(fault==4)p.identity.module=1;else {p.function.runs[0].base.op=XR_XIR_CALL_REQUIREMENT;
            if(fault==5)p.function.runs[0].base.targets[0]=1;else if(fault==6)p.function.runs[0].base.targets[1]=1;
            else if(fault==7)p.method.signature=XR_XIR_I64;else if(fault==8)p.callable.flags=0;
            else p.function.runs[0].base.op=fault==9?XR_XIR_INVALID:XR_XIR_OP_COUNT;}
        CHECK(seed_cursor_compare(&context,&p.body,0,NULL)==(fault==7 || fault==8?XR_XIR_BAD_TYPE:XR_XIR_BAD_STRUCTURE));
        p.declarations.slots=&p.slot;p.declarations.modules=&p.module;p.identity.module=0;
        p.method.signature=(XrXirType)256;p.callable.flags=XR_XIR_CALLABLE_ROOT_NONE;
    }
    p.function.runs[0].base=(XrXirInstruction){.op=XR_XIR_RETURN};
    CHECK(seed_cursor_compare(&context,&p.body,1,NULL)==XR_XIR_BAD_STRUCTURE);
    XrXirCompileContext foreign=rp_owner(rp_caps());uint64_t other=rp_stats(&foreign).live_bytes;
    CHECK(seed_cursor_compare(&foreign,&p.body,0,NULL)==XR_XIR_BAD_STRUCTURE);rp_owner_free(&foreign,other);
    xr_compile_resources_free(p.function.runs);rp_owner_free(&context,live);rp_balanced(held);
}
static uint64_t seed_cursor_complete_work;
static XrXirStatus seed_cursor_physical_run(const XrXirCompileContext *context,unsigned pattern,uint64_t cut) {
    SeedCursorPhysical p;seed_cursor_physical_init(context,&p);XrXirInstruction original[17],input[17];
    uint32_t count=pattern==3?0:17;literal_block_values(original);
    if(pattern<2)for(uint32_t i=0;i<count;++i)original[i]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=pattern?i:7};
    memcpy(input,original,sizeof(input));XrXirFunction source={.instructions=input,.instruction_count=count};p.function.instruction_count=count;
    XrXirStatus status=count?effect_body_records_copy(context,&source,&p.function):XR_XIR_OK;
    if(status!=XR_XIR_OK){xr_compile_resources_free(p.function.runs);return status;}
    memset(input,0xCE,sizeof(input));
    if(cut==UINT64_MAX){CHECK(seed_cursor_compare(context,&p.body,0,NULL)==XR_XIR_OK);
        EffectBodyRead read=effect_body_compile_read(context);
        for(uint32_t i=0;i<count;++i){XrXirInstruction value;CHECK(effect_body_instruction_get(&read,&p.function,i,&value)==XR_XIR_OK && conditional_compact_same(&value,&original[i]));}}
    uint64_t entry=rp_stats(context).work;size_t attempts=rp_attempts;RootParameterMark held=rp_mark();
    if(cut!=UINT64_MAX)context->resources->limits.work=entry+cut;
    const XrXirRootEffects sentinel={true,true};XrXirRootEffects value=sentinel;
    status=effect_invocation_input_seed(context,&p.body,0,&value);
    if(status==XR_XIR_OK){CHECK(!value.requires_root && !value.unresolved);if(pattern==2 && cut==UINT64_MAX)seed_cursor_complete_work=rp_stats(context).work-entry;}
    else{CHECK(status==XR_XIR_BUDGET && seed_cursor_equal(value,sentinel));
        CHECK(effect_invocation_input_seed(context,&p.body,0,&value)==XR_XIR_BUDGET && seed_cursor_equal(value,sentinel));}
    CHECK(rp_attempts==attempts);rp_balanced(held);xr_compile_resources_free(p.function.runs);return status;
}
static void seed_cursor_directory_faults(void) {
    RootParameterMark held=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());uint64_t live=rp_stats(&context).live_bytes;
    SeedCursorPhysical p;seed_cursor_physical_init(&context,&p);XrXirInstruction original[17];literal_block_values(original);
    XrXirFunction source={.instructions=original,.instruction_count=17};p.function.instruction_count=17;
    CHECK(effect_body_records_copy(&context,&source,&p.function)==XR_XIR_OK && p.function.run_count==3);
    EffectBodyRun saved[3];memcpy(saved,p.function.runs,sizeof(saved));
    for(unsigned fault=0;fault<10;++fault){
        if(!fault)p.function.run_capacity=2;else if(fault==1)p.function.run_count=0;
        else if(fault==2)p.function.runs[0].first=1;else if(fault==3)++p.function.runs[1].first;
        else if(fault==4)--p.function.runs[2].first;else if(fault==5)p.function.runs[2].count=2;
        else if(fault==6)p.function.runs[0].count=0;else if(fault==7)p.function.runs[1].mask|=1;
        else if(fault==8)p.function.runs[0].count=9;else p.function.runs[1].mask=UINT32_MAX;
        CHECK(seed_cursor_compare(&context,&p.body,0,NULL)==XR_XIR_BAD_STRUCTURE);
        memcpy(p.function.runs,saved,sizeof(saved));p.function.run_capacity=p.function.run_count=3;
    }
    for(uint32_t i=0;i<17;++i)for(unsigned upper=0;upper<2;++upper){uint32_t run=i/8,offset=i%8;
        XrXirInstruction *op=offset?&p.function.runs[run].literal_tail[offset-1]:&p.function.runs[run].base;
        XrXirOp previous=op->op;op->op=upper?XR_XIR_OP_COUNT:XR_XIR_INVALID;
        CHECK(seed_cursor_compare(&context,&p.body,0,NULL)==XR_XIR_BAD_STRUCTURE);op->op=previous;
    }
    CHECK(seed_cursor_compare(&context,&p.body,0,NULL)==XR_XIR_OK);
    xr_compile_resources_free(p.function.runs);rp_owner_free(&context,live);rp_balanced(held);
}
/* Only the receiving numeric copy is damaged. Current owned Input remains
 * authentic, and no scratch copy is published as a semantic certificate. */
static void seed_cursor_receiving_erasure(const XrXirCompileContext *context,const EffectInvocationCertificate *certificate) {
    const EffectInvocationFinal *real=certificate->final;EffectInvocationNode nodes[32];EffectFinalFunction functions[8];
    CHECK(real->count<=32 && real->summary.count<=8);EffectInvocationFinal final=*real;
    final.nodes=nodes;final.summary.functions=functions;EffectInvocationCertificate copy=*certificate;copy.final=&final;
    for(uint32_t f=0;f<effect_body_count(certificate->body);++f){XrXirRootEffects facts={0};
        CHECK(effect_invocation_input_seed(context,certificate->body,f,&facts)==XR_XIR_OK);
        if(!facts.requires_root)continue;
        memcpy(nodes,real->nodes,(size_t)real->count*sizeof(*nodes));
        memcpy(functions,real->summary.functions,(size_t)real->summary.count*sizeof(*functions));
        for(uint32_t n=0;n<final.count;++n)if(nodes[n].body==f){nodes[n].intrinsic_mask&=~XR_XIR_CALLABLE_ROOT_REQUIRED;nodes[n].local_mask&=~XR_XIR_CALLABLE_ROOT_REQUIRED;}
        functions[f].formula.constant_mask&=~XR_XIR_CALLABLE_ROOT_REQUIRED;
        CHECK(effect_invocation_input_seeds_match(context,&copy,&final.summary)==XR_XIR_BAD_STRUCTURE);
    }
}
static XrXirStatus seed_cursor_genuine(const XrXirCompileContext *context,uint32_t mode,bool controls) {
    InvocationClassCellFixture fixture;invocation_class_cell_fixture(&fixture,mode);XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;
    XrXirStatus status=xir_fixture_check(context,&fixture.source.module,&checked,NULL);
    if(status==XR_XIR_OK)status=xr_xir_compile_artifact_verify(checked,NULL);
    if(status==XR_XIR_OK)status=xr_xir_compile_effects_analyze(checked,&effects);
    xr_xir_compile_artifact_free(checked);memset(&fixture,0xCE,sizeof(fixture));
    if(status==XR_XIR_OK){EffectInvocationCertificate *certificate=effects->invocations;
        for(uint32_t f=0;status==XR_XIR_OK && f<effect_body_count(certificate->body);++f)
            status=seed_cursor_compare(context,certificate->body,f,NULL);
        if(status==XR_XIR_OK && controls)seed_cursor_receiving_erasure(context,certificate);
        if(status==XR_XIR_OK)status=effect_invocation_publication_match(context,certificate,&certificate->final->summary);
        EffectInvocationWorkspace replay={0};EffectBodyRequest body=effect_body_owned_request(context,certificate->body);
        if(status==XR_XIR_OK)status=effect_invocation_read_open(context,effects,certificate->declared,certificate->final,&replay);
        if(status==XR_XIR_OK)status=effect_invocation_class_verify_read(&replay,&body);effect_invocation_read_close(&replay);
    }
    xr_xir_compile_effects_free(effects);return status;
}
static void invocation_input_seed_cursor_controls(void) {
    seed_cursor_opcode_controls();seed_cursor_directory_faults();RootParameterMark held=rp_mark();
    for(unsigned pattern=0;pattern<4;++pattern){XrXirCompileContext context=rp_owner(rp_caps());uint64_t live=rp_stats(&context).live_bytes;
        CHECK(seed_cursor_physical_run(&context,pattern,UINT64_MAX)==XR_XIR_OK);rp_owner_free(&context,live);rp_balanced(held);}
    CHECK(seed_cursor_complete_work);
    for(uint64_t cut=0;cut<=seed_cursor_complete_work;++cut){XrXirCompileContext context=rp_owner(rp_caps());uint64_t live=rp_stats(&context).live_bytes;
        CHECK(seed_cursor_physical_run(&context,2,cut)==(cut==seed_cursor_complete_work?XR_XIR_OK:XR_XIR_BUDGET));rp_owner_free(&context,live);rp_balanced(held);}
    for(uint32_t mode=0;mode<7;++mode){XrXirCompileContext context=rp_owner(rp_caps());uint64_t live=rp_stats(&context).live_bytes;
        CHECK(seed_cursor_genuine(&context,mode,true)==XR_XIR_OK);rp_owner_free(&context,live);rp_balanced(held);}
    XrXirCompileContext context=rp_owner(rp_caps());uint64_t live=rp_stats(&context).live_bytes;
    CHECK(seed_cursor_genuine(&context,6,false)==XR_XIR_OK);XrCompileResourceStats measured=rp_stats(&context);rp_owner_free(&context,live);rp_balanced(held);
    for(unsigned axis=0;axis<3;++axis)for(unsigned minus=0;minus<2;++minus){XrCompileResourceLimits caps=rp_caps();
        if(!axis)caps.allocated_bytes=measured.allocated_bytes-minus;else if(axis==1)caps.live_bytes=measured.peak_bytes-minus;else caps.work=measured.work-minus;
        context=rp_owner(caps);live=rp_stats(&context).live_bytes;CHECK(seed_cursor_genuine(&context,6,false)==(minus?XR_XIR_BUDGET:XR_XIR_OK));
        rp_owner_free(&context,live);rp_balanced(held);}
    fprintf(stderr,"Input seed cursor: original complete random-reader and literal all-opcode ROOT oracle, slot/initializer/requirement flags and eleven faults, complete constant/affine/literal/multi-run/empty bodies and every ten-word row after source death, ten directory/neighbor/EOF and all17 invalid/upper row faults, foreign ledger and atomic failed facts, no allocation; every work unit through%llu and same finite-owner refusal without cap reset, all7 genuine Class/Cell bodies after producer/Checked death and receiving-mask erasure, original independent publication and Class verification; whole genuine six axes%llu/%llu/%llu exact/minusone physical0 PASS\n",
        (unsigned long long)seed_cursor_complete_work,(unsigned long long)measured.allocated_bytes,(unsigned long long)measured.peak_bytes,(unsigned long long)measured.work);
}
#endif // XIR_INVOCATION_INPUT_SEED_CURSOR_CASES_H
