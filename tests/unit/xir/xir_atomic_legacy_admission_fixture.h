/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_atomic_legacy_admission_fixture.h - Current compiler graph for preserved Atomic responsibilities
 */
#ifndef XIR_ATOMIC_LEGACY_ADMISSION_FIXTURE_H
#define XIR_ATOMIC_LEGACY_ADMISSION_FIXTURE_H
typedef struct AtomicLegacyFixture {
    AtomicInstructionFixture base;
    XrXirInstruction ops[20];XrXirBlock blocks[3];uint32_t operands[16];
    unsigned cursor,used,atomic_index,alias_index,action_index,load_index;
} AtomicLegacyFixture;
typedef struct AtomicLegacyCase { unsigned mode,mutation;XrXirType scalar; } AtomicLegacyCase;
static uint32_t legacy_append(AtomicLegacyFixture *f,XrXirInstruction op) {
    CHECK(f->cursor<20);uint32_t id=f->cursor;f->ops[f->cursor++]=op;return id;
}
static uint32_t legacy_atomic(AtomicLegacyFixture *f,XrXirOp op,XrXirType result,
    const uint32_t *values,uint32_t count) {
    CHECK(count<=4 && f->used<=16-count);uint32_t begin=f->used;
    for(uint32_t i=0;i<count;++i)f->operands[f->used++]=values[i];
    return legacy_append(f,(XrXirInstruction){.op=op,.type=result,.args={begin,count}});
}
static void legacy_fixture(AtomicLegacyFixture *f,char *name,AtomicLegacyCase c) {
    *f=(AtomicLegacyFixture){0};atomic_instruction_fixture(&f->base,name,XR_XIR_ATOMIC_LOAD,c.scalar,0,-3);
    XrXirInstruction initial={.op=c.scalar==XR_XIR_BOOL?XR_XIR_CONST_BOOL:c.scalar==XR_XIR_F64?XR_XIR_CONST_FLOAT:XR_XIR_CONST_INT,.type=c.scalar};
    initial.immediate=c.scalar==XR_XIR_BOOL?0:c.scalar==XR_XIR_F64?(int64_t)UINT64_C(0x4044000000000000):40;
    legacy_append(f,initial);initial.immediate=c.scalar==XR_XIR_BOOL?1:c.scalar==XR_XIR_F64?(int64_t)(c.mode>=5?UINT64_C(0x4000000000000000):UINT64_C(0x4045000000000000)):c.mode>=5?2:42;
    legacy_append(f,initial);
    f->atomic_index=legacy_append(f,(XrXirInstruction){.op=XR_XIR_ATOMIC_NEW,.type=(XrXirType)258,.args={0}});
    f->alias_index=legacy_append(f,(XrXirInstruction){.op=XR_XIR_COPY,.type=(XrXirType)258,.args={f->atomic_index}});
    bool loop=c.mode==7 && c.scalar==XR_XIR_I64;
    uint32_t receiver=f->alias_index,values[3]={receiver,1,0},returned=0;
    if(loop){
        legacy_append(f,(XrXirInstruction){.op=XR_XIR_JUMP,.type=XR_XIR_UNIT,.targets={1}});
        f->blocks[0]=(XrXirBlock){.first=0,.count=f->cursor};uint32_t first=f->cursor;
        uint32_t old=legacy_atomic(f,XR_XIR_ATOMIC_LOAD,c.scalar,values,1);
        uint32_t next=legacy_append(f,(XrXirInstruction){.op=XR_XIR_XOR_INT,.type=XR_XIR_I64,.args={old,1}});
        uint32_t casargs[]={receiver,old,next};f->action_index=legacy_atomic(f,XR_XIR_ATOMIC_COMPARE_EXCHANGE,(XrXirType)259,casargs,3);
        uint32_t success=legacy_append(f,(XrXirInstruction){.op=XR_XIR_TUPLE_FIELD,.type=XR_XIR_BOOL,.args={f->action_index},.immediate=1});
        legacy_append(f,(XrXirInstruction){.op=XR_XIR_BRANCH,.type=XR_XIR_UNIT,.args={success},.targets={2,1}});
        f->blocks[1]=(XrXirBlock){.first=first,.count=f->cursor-first};first=f->cursor;
        f->load_index=legacy_atomic(f,XR_XIR_ATOMIC_LOAD,c.scalar,values,1);returned=f->load_index;
        legacy_append(f,(XrXirInstruction){.op=XR_XIR_RETURN,.type=XR_XIR_UNIT,.args={returned}});
        f->blocks[2]=(XrXirBlock){.first=first,.count=f->cursor-first};
    }else{
        XrXirOp action=c.mode==0?XR_XIR_ATOMIC_SWAP:c.mode<=4?XR_XIR_ATOMIC_COMPARE_EXCHANGE:c.mode==5?XR_XIR_ATOMIC_FETCH_ADD:c.mode==6?XR_XIR_ATOMIC_FETCH_SUB:XR_XIR_ATOMIC_TOGGLE;
        bool cas=c.mode>=1 && c.mode<=4;uint32_t count=cas?3:c.mode==7?1:2;
        if(cas){values[1]=c.mode==2||c.mode==4?1:0;values[2]=1;}
        f->action_index=legacy_atomic(f,action,cas?(XrXirType)259:c.scalar,values,count);
        uint32_t previous=f->action_index;
        if(cas)previous=legacy_append(f,(XrXirInstruction){.op=XR_XIR_TUPLE_FIELD,.type=c.scalar,.args={f->action_index},.immediate=0});
        values[0]=receiver;f->load_index=legacy_atomic(f,XR_XIR_ATOMIC_LOAD,c.scalar,values,1);
        returned=c.mode==3||c.mode==4?previous:f->load_index;
        legacy_append(f,(XrXirInstruction){.op=XR_XIR_RETURN,.type=XR_XIR_UNIT,.args={returned}});
        f->blocks[0]=(XrXirBlock){.count=f->cursor};
    }
    XrXirFunction *fn=&f->base.functions[3];fn->parameters=NULL;fn->parameter_count=0;fn->result=c.scalar;
    fn->blocks=f->blocks;fn->block_count=loop?3:1;fn->instructions=f->ops;fn->instruction_count=f->cursor;
    fn->operands=f->operands;fn->operand_count=f->used;f->base.declarations.entry_function=2;
    switch(c.mutation){
    case 0:break;
    case 1:f->base.nodes[2].element=c.scalar==XR_XIR_BOOL?XR_XIR_I64:XR_XIR_BOOL;break;
    case 2: /* Unknown Ordering must reject at its genuine governed enum constructor. */
        f->ops[1]=(XrXirInstruction){.op=XR_XIR_ENUM_NEW,.type=(XrXirType)256,.immediate=5};break;
    case 3: /* Retired owned-result bit maps to a prohibited owned LOAD result type. */
        f->ops[f->load_index].type=(XrXirType)258;break;
    case 4: { /* Retired early owner drop maps to an alias used before valid SSA acquisition. */
        f->ops[f->alias_index].args[0]=f->alias_index;break; }
    case 5:f->ops[f->action_index].immediate=UINT32_MAX;break;
    case 6: /* Value construction cannot publish a scalar/place representation. */
        f->ops[f->atomic_index].type=XR_XIR_I64;break;
    default:f->ops[f->action_index].op=c.scalar==XR_XIR_BOOL?XR_XIR_ATOMIC_FETCH_ADD:XR_XIR_ATOMIC_TOGGLE;break;
    }
}
static XrXirStatus legacy_pipeline(const XrXirCompileContext *context,void *data) {
    AtomicLegacyCase c=*(AtomicLegacyCase *)data;AtomicLegacyFixture fixture;char *name=NULL;
    XrXirArtifact *checked=NULL,*read=NULL,*specialized=NULL,*lowered=NULL;XrXirCheckedPacket packet={0};
    XrXirStatus status=ordering_factory(context,&name);
    if(status==XR_XIR_OK){legacy_fixture(&fixture,name,c);status=xr_xir_compile_check(context,&fixture.base.module,&checked,NULL);if(status!=XR_XIR_OK)CHECK(!checked);}
    xr_compile_resources_free(name);
    if(status==XR_XIR_OK){status=xr_xir_compile_checked_write(checked,&packet,NULL);if(status!=XR_XIR_OK)CHECK(!packet.bytes&&!packet.length);}
    xr_xir_compile_artifact_free(checked);
    if(status==XR_XIR_OK){status=xr_xir_compile_checked_read(context,packet.bytes,packet.length,&read,NULL);if(status!=XR_XIR_OK)CHECK(!read);}
    if(status==XR_XIR_OK){status=xr_xir_compile_specialize(read,&specialized,NULL);if(status!=XR_XIR_OK)CHECK(!specialized);}
    xr_xir_compile_artifact_free(read);
    if(status==XR_XIR_OK){status=xr_xir_compile_lower(specialized,&(XrXirTarget){XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},&lowered,NULL);if(status!=XR_XIR_OK)CHECK(!lowered);}
    xr_xir_compile_artifact_free(specialized);
    if(status==XR_XIR_OK)status=xr_xir_compile_artifact_verify(lowered,NULL);
    xr_xir_compile_artifact_free(lowered);xr_xir_compile_checked_packet_free(&packet);
    if(status!=XR_XIR_OK && source_program_compile_fail_at==SIZE_MAX)fprintf(stderr,"legacy mode%u scalar%u status%u\n",c.mode,c.scalar,status);
    return status;
}
#endif
