/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_mem_native_fixture.h - Governed memory through actual typed programs
 *
 * KEY CONCEPT:
 *   VM and generated C execute the same checked calls while the original
 *   storage fixture and independent rejection oracles remain intact.
 */
#ifndef XIR_MEM_NATIVE_FIXTURE_H
#define XIR_MEM_NATIVE_FIXTURE_H
#ifndef XIR_MEM_NATIVE_RUNTIME_ONLY
typedef struct MemNativeFixture {
    XrXirInstruction code[64];XrXirBlock blocks[8];uint32_t operands[128];
    uint32_t count,block_count,first,operand_count;
} MemNativeFixture;
static uint32_t mem_native_add(MemNativeFixture *b,XrXirInstruction value) {
    CHECK(b->count<64);uint32_t index=b->count;b->code[b->count++]=value;return index;
}
static uint32_t mem_native_integer(MemNativeFixture *b,int64_t value) {
    return mem_native_add(b,(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=value});
}
static uint32_t mem_native_call(MemNativeFixture *b,uint32_t id,XrXirType result,const uint32_t *args,uint32_t count) {
    CHECK(count<=3&&b->operand_count+count<=128);uint32_t first=count?b->operand_count:0;
    if(count)memcpy(b->operands+first,args,count*sizeof(*args));b->operand_count+=count;
    return mem_native_add(b,(XrXirInstruction){.op=XR_XIR_CALL,.type=result,.args={first,count},.immediate=id==5?2:id-8});
}
static void mem_native_guard(MemNativeFixture *b,uint32_t condition) {
    CHECK(b->block_count<6);
    mem_native_add(b,(XrXirInstruction){.op=XR_XIR_BRANCH,.args={condition},.targets={b->block_count+1,UINT32_MAX}});
    b->blocks[b->block_count++]=(XrXirBlock){b->first,b->count-b->first,0,0};b->first=b->count;
}
static XrXirArtifact *mem_native_fixture(const XrXirCompileContext *context) {
    const XrXirType ro=(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE,rw=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+1);
    const XrXirTypeNode nodes[]={{.kind=XR_XIR_TYPE_PTR,.element=XR_XIR_U8},{.kind=XR_XIR_TYPE_MUT_PTR,.element=XR_XIR_U8}};
    XrXirTypes types={nodes,2,NULL,NULL};
    const struct {uint32_t id,count;XrXirType result,parameters[3];} native[]={
        {5,1,XR_XIR_BUFFER_STORAGE,{XR_XIR_I64}},
        {11,1,rw,{XR_XIR_BUFFER_STORAGE}},{12,2,XR_XIR_BOOL,{XR_XIR_BUFFER_STORAGE,XR_XIR_I64}},
        {13,1,XR_XIR_UNIT,{XR_XIR_I64}},{14,2,XR_XIR_UNIT,{ro,XR_XIR_I64}},
        {15,2,XR_XIR_UNIT,{ro,XR_XIR_I64}},{16,3,XR_XIR_UNIT,{rw,XR_XIR_I64,XR_XIR_I64}},
        {17,0,XR_XIR_I64,{0}},{18,2,rw,{XR_XIR_I64,XR_XIR_I64}},
        {19,3,XR_XIR_BOOL,{rw,XR_XIR_I64,XR_XIR_I64}},{20,2,XR_XIR_BOOL,{rw,XR_XIR_I64}},
        {21,3,XR_XIR_UNIT,{rw,ro,XR_XIR_I64}},{22,2,XR_XIR_I64,{ro,XR_XIR_I64}},
        {23,3,XR_XIR_UNIT,{rw,XR_XIR_I64,XR_XIR_I64}}};
    XrXirFunction functions[16]={{0}};XrXirFunctionIdentity identities[16]={{0}};
    const XrXirInstruction done={.op=XR_XIR_RETURN};const XrXirBlock init={0,1,0,0};
    functions[0]=(XrXirFunction){"init",4,NULL,0,XR_XIR_UNIT,&init,1,&done,1,NULL,0,XR_XIR_NEVER};
    for(uint32_t n=0;n<14;++n) {
        const XrNativeCallableDeclaration *d=xr_native_callable_by_id(native[n].id);CHECK(d&&d->member);
        functions[n+2]=(XrXirFunction){d->member->name,(uint32_t)strlen(d->member->name),
            native[n].count?native[n].parameters:NULL,native[n].count,native[n].result,NULL,0,NULL,0,NULL,0,XR_XIR_NEVER};
        identities[n+2].native_callable=native[n].id;identities[n+2].promises=XR_XIR_FUNCTION_NO_SUSPEND;
    }
    MemNativeFixture b={0};uint32_t bytes=mem_native_integer(&b,64);
    uint32_t storage=mem_native_call(&b,5,XR_XIR_BUFFER_STORAGE,&bytes,1);
    uint32_t pointer=mem_native_call(&b,11,rw,&storage,1);
    uint32_t read=mem_native_add(&b,(XrXirInstruction){.op=XR_XIR_PTR_READONLY,.type=ro,.args={pointer}});
    uint32_t first=mem_native_integer(&b,91),width=mem_native_integer(&b,8);
    mem_native_call(&b,23,XR_XIR_UNIT,(uint32_t[]){pointer,first,width},3);
    uint32_t loaded=mem_native_call(&b,22,XR_XIR_I64,(uint32_t[]){read,width},2);
    mem_native_guard(&b,mem_native_add(&b,(XrXirInstruction){.op=XR_XIR_EQ_INT,.type=XR_XIR_BOOL,.args={loaded,first}}));
    uint32_t order=mem_native_integer(&b,4);mem_native_call(&b,13,XR_XIR_UNIT,&order,1);
    uint32_t zero=mem_native_integer(&b,0);mem_native_call(&b,14,XR_XIR_UNIT,(uint32_t[]){read,zero},2);
    mem_native_call(&b,15,XR_XIR_UNIT,(uint32_t[]){read,bytes},2);
    uint32_t answer=mem_native_integer(&b,42);mem_native_call(&b,16,XR_XIR_UNIT,(uint32_t[]){pointer,answer,width},3);
    uint32_t line=mem_native_call(&b,17,XR_XIR_I64,NULL,0);
    mem_native_guard(&b,mem_native_add(&b,(XrXirInstruction){.op=XR_XIR_EQ_INT,.type=XR_XIR_BOOL,.args={line,bytes}}));
    uint32_t offset=mem_native_add(&b,(XrXirInstruction){.op=XR_XIR_PTR_OFFSET,.type=rw,.args={pointer,width}});
    mem_native_call(&b,21,XR_XIR_UNIT,(uint32_t[]){offset,read,width},3);
    uint32_t copied=mem_native_add(&b,(XrXirInstruction){.op=XR_XIR_PTR_READONLY,.type=ro,.args={offset}});
    loaded=mem_native_call(&b,22,XR_XIR_I64,(uint32_t[]){copied,width},2);
    uint32_t protection=mem_native_integer(&b,3);
    uint32_t page=mem_native_call(&b,18,rw,(uint32_t[]){bytes,protection},2);
    mem_native_guard(&b,mem_native_call(&b,19,XR_XIR_BOOL,(uint32_t[]){page,bytes,protection},3));
    mem_native_guard(&b,mem_native_call(&b,20,XR_XIR_BOOL,(uint32_t[]){page,bytes},2));
    uint32_t resized=mem_native_integer(&b,16);
    mem_native_guard(&b,mem_native_call(&b,12,XR_XIR_BOOL,(uint32_t[]){storage,resized},2));
    mem_native_add(&b,(XrXirInstruction){.op=XR_XIR_RETURN,.args={loaded}});
    b.blocks[b.block_count++]=(XrXirBlock){b.first,b.count-b.first,0,0};b.first=b.count;
    uint32_t failed=b.block_count,number=mem_native_integer(&b,-1);
    mem_native_add(&b,(XrXirInstruction){.op=XR_XIR_RETURN,.args={number}});
    b.blocks[b.block_count++]=(XrXirBlock){b.first,b.count-b.first,0,0};
    for(uint32_t i=0;i<b.count;++i)if(b.code[i].op==XR_XIR_BRANCH)b.code[i].targets[1]=failed;
    functions[1]=(XrXirFunction){"memory",6,NULL,0,XR_XIR_I64,b.blocks,b.block_count,b.code,b.count,b.operands,b.operand_count,XR_XIR_NEVER};
    identities[1].exported=1;
    static const char identity[]="stdlib-module-v1:module=3:mem:path=10:mem/mem.xr";
    const XrXirSourceModule source={.name=identity,.name_length=sizeof(identity)-1,.initializer=0};
    const XrXirDeclarations declarations={&source,1,identities,NULL,0,NULL,0,0,1,NULL};
    XrXirModule module={.stage=XR_XIR_BUILT,.functions=functions,.function_count=16,.declarations=&declarations,
        .types=&types,.linkage_kind=XR_XIR_PROGRAM};
    XrXirArtifact *checked=NULL,*closed=NULL,*lowered=NULL;XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xir_fixture_check(context,&module,&checked,&diagnostic);
    if(status!=XR_XIR_OK) fprintf(stderr,"mem fixture check %u f%u b%u i%u\n",status,diagnostic.function,diagnostic.block,diagnostic.instruction);
    CHECK(status==XR_XIR_OK);
    b.code[read].type=rw;CHECK(xir_fixture_verify(context,&module,NULL)==XR_XIR_BAD_TYPE);b.code[read].type=ro;
    identities[3].exported=1;CHECK(xir_fixture_verify(context,&module,NULL)==XR_XIR_BAD_STRUCTURE);identities[3].exported=0;
    identities[3].native_callable=XR_NATIVE_CALLABLE_BUFFER_LENGTH;
    CHECK(xir_fixture_verify(context,&module,NULL)==XR_XIR_BAD_TYPE);identities[3].native_callable=XR_NATIVE_CALLABLE_BUFFER_BORROW_PTR;
    CHECK(xir_fixture_verify(context,&module,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_specialize(checked,&closed,&diagnostic,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(checked);
    const XrXirTarget target={XR_XIR_ARCH_LE64,XR_XIR_VALUE_ABI_VERSION};
    status=xr_xir_compile_lower(closed,&target,NULL,&lowered,&diagnostic);
    if(status!=XR_XIR_OK) fprintf(stderr,"mem lower status%u f%u b%u i%u\n",status,diagnostic.function,diagnostic.block,diagnostic.instruction);
    CHECK(status==XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);
    bool found=false;
    for(uint32_t f=0;f<lowered->module.function_count;++f) {
        if(lowered->module.declarations->functions[f].native_callable!=XR_NATIVE_CALLABLE_MEM_CACHE_LINE_SIZE) continue;
        CHECK(!found);found=true;XrXirFunctionLayout *layout=&lowered->layouts[f];uint32_t fake=0;
        CHECK(!layout->slot_count && !layout->offsets && !layout->owned_offsets && !layout->parameters &&
            !layout->frame_bytes && layout->result.size==16);
        layout->offsets=&fake;CHECK(xr_xir_compile_layout_verify(lowered)==XR_XIR_BAD_LAYOUT);layout->offsets=NULL;
        layout->owned_offsets=&fake;CHECK(xr_xir_compile_layout_verify(lowered)==XR_XIR_BAD_LAYOUT);layout->owned_offsets=NULL;
    }
    CHECK(found && xr_xir_compile_layout_verify(lowered)==XR_XIR_OK);return lowered;
}
#endif
static void mem_native_execute(XrXirProgram *program) {
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,1,NULL,0)==XR_XIR_CALL_READY);
    XrXirCallStatus status=xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status;
    if(status!=XR_XIR_CALL_RETURNED)fprintf(stderr,"mem native execution status%u\n",status);
    CHECK(status==XR_XIR_CALL_RETURNED);XrXirValue result={0};
    CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED&&result.type==XR_XIR_I64&&result.payload==42);
    xr_xir_value_drop(&result);CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
}
#endif
