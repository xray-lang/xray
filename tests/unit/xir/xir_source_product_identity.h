/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * product_identity_gates.h - Independent layout framing and packet corruption gates
 */
#ifndef PRODUCT_IDENTITY_GATES_H
#define PRODUCT_IDENTITY_GATES_H
static void product_layout_vector(void) {
    const XrXirType parameter=XR_XIR_STRING;
    const XrXirInstruction identity[]={{XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}}};
    const XrXirInstruction initialize[]={{XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}}};
    const XrXirInstruction entry[]={
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},0,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}}};
    const XrXirBlock one={0,1,0,0},two={0,2,0,0};
    const XrXirFunction functions[]={
        {"identity",8,&parameter,1,XR_XIR_STRING,&one,1,identity,1,NULL,0},
        {"init",4,NULL,0,XR_XIR_UNIT,&one,1,initialize,1,NULL,0},
        {"entry",5,NULL,0,XR_XIR_I64,&two,1,entry,2,NULL,0}};
    const XrXirSourceModule source={"m",1,NULL,0,1};
    const XrXirFunctionIdentity ids[3]={{0,1,0,0,0,0,0},{0},{0}};
    const XrXirDeclarations declarations={&source,1,ids,NULL,0,NULL,0,0,2,NULL};
    const XrXirModule module={XR_XIR_BUILT,functions,3,&declarations,NULL,NULL,NULL,XR_XIR_PROGRAM,NULL};
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    size_t live=runtime_live,bytes=runtime_bytes;XrXirArtifact *checked=NULL,*lowered=NULL;
    CHECK(xr_xir_check(&module,NULL,&checked,NULL)==XR_XIR_OK);
    CHECK(xr_xir_lower(checked,&target,NULL,&lowered,NULL)==XR_XIR_OK);xr_xir_artifact_free(checked);
    XrXirSourceProductFacts facts={0};facts.target=target;facts.entry=2;facts.function_count=3;facts.module_count=1;
    static const uint8_t expected[]={0xf6,0xbe,0xad,0x7d,0xe6,0x18,0x0c,0x25,0x55,0x84,0xd0,0xe7,0x10,0xca,0x85,0x16,0x31,0xa3,0x9b,0xb6,0x93,0x58,0x6c,0x00,0x9a,0xda,0x50,0x39,0x92,0x00,0x66,0x26};
    uint8_t digest[32];CHECK(source_product_layout_digest(lowered,&facts,198,digest)==XR_XIR_OK);
    CHECK(!memcmp(digest,expected,32));memset(digest,0x5a,sizeof(digest));uint8_t saved[32];memcpy(saved,digest,32);
    CHECK(source_product_layout_digest(lowered,&facts,197,digest)==XR_XIR_BUDGET && !memcmp(digest,saved,32));
    const XrXirFunctionLayout *layout=xr_xir_artifact_layout(lowered,0);
    CHECK(layout->slot_count==2 && layout->frame_bytes==8 && layout->result.size==16 && layout->result.alignment==8);
    CHECK(layout->owned_count==1 && !layout->outgoing_count && !layout->path_count);
    CHECK(layout->offsets[0]==0 && layout->offsets[1]==UINT32_MAX && layout->owned_offsets[0]==0);
    CHECK(layout->parameters[0].size==16 && layout->parameters[0].alignment==8);
    XrXirCheckedPacket a={NULL,51},b={NULL,63};XrXirBudget quota=xr_xir_default_budget();uint64_t remaining=0;
    quota.metadata_bytes=sizeof(XrXirSourceProduct)+114;quota.work=114;
    CHECK(source_product_facts_budget(&a,&b,&quota,&remaining)==XR_XIR_OK && !remaining);
    --quota.metadata_bytes;remaining=19;
    CHECK(source_product_facts_budget(&a,&b,&quota,&remaining)==XR_XIR_BUDGET && remaining==19);
    ++quota.metadata_bytes;--quota.work;
    CHECK(source_product_facts_budget(&a,&b,&quota,&remaining)==XR_XIR_BUDGET && remaining==19);
    xr_xir_artifact_free(lowered);CHECK(runtime_live==live && runtime_bytes==bytes);
    puts("independent static 198-byte layout KAT; result/parameter/owned/Unit slots; exact/minus1 work PASS");
}
static void product_rehash(uint8_t *bytes,size_t length) {
    XrSHA256Context sha;xr_sha256_init(&sha);
    xr_sha256_update(&sha,bytes,32);xr_sha256_update(&sha,bytes+64,length-64);xr_sha256_final(&sha,bytes+32);
}
static void product_corruptions(XrXirSourceProduct *product) {
    size_t live=runtime_live,bytes=runtime_bytes;XrXirSourceProduct original=*product;
    XrXirSourceProductFacts *facts=&product->facts;
    uint32_t *words[]={&facts->entry,&facts->target.architecture,&facts->target.abi_version,&facts->function_count,&facts->module_count};
    for (size_t i=0;i<sizeof(words)/sizeof(*words);++i) {
        uint32_t old=*words[i];*words[i]=old+1;
        CHECK(xr_xir_source_product_verify(product,NULL,16777216,NULL)==XR_XIR_BAD_STRUCTURE);*words[i]=old;
        CHECK(runtime_live==live && runtime_bytes==bytes);
    }
    facts->lowered_layout_digest[0]^=1;
    CHECK(xr_xir_source_product_verify(product,NULL,16777216,NULL)==XR_XIR_BAD_LAYOUT);facts->lowered_layout_digest[0]^=1;
    facts->source_digest[0]^=1;
    CHECK(xr_xir_source_product_verify(product,NULL,16777216,NULL)==XR_XIR_BAD_STRUCTURE);facts->source_digest[0]^=1;
    facts->closed_digest[0]^=1;
    CHECK(xr_xir_source_product_verify(product,NULL,16777216,NULL)==XR_XIR_BAD_STRUCTURE);facts->closed_digest[0]^=1;
    for (unsigned kind=0;kind<2;++kind) {
        XrXirCheckedPacket *packet=kind ? &product->closed_packet : &product->source_packet;
        uint8_t *identity=kind ? facts->closed_digest : facts->source_digest;
        uint8_t *copy=xr_malloc(packet->length);CHECK(copy);memcpy(copy,packet->bytes,packet->length);
        uint8_t *previous=packet->bytes;packet->bytes=copy;
        const unsigned positions[]={8,12,20};
        for (size_t i=0;i<sizeof(positions)/sizeof(*positions);++i) {
            memcpy(copy,previous,packet->length);copy[positions[i]]^=1;product_rehash(copy,packet->length);
            xr_sha256(copy,packet->length,identity);
            CHECK(xr_xir_source_product_verify(product,NULL,16777216,NULL)==XR_XIR_BAD_STRUCTURE);
        }
        packet->bytes=previous;xr_sha256(previous,packet->length,identity);xr_free(copy);
        CHECK(runtime_live==live && runtime_bytes==bytes);
    }
    CHECK(!memcmp(product,&original,sizeof(original)));
    puts("opaque internal tamper gates: entry/target/count/layout/twoSHA; 6 valid-rehash wire/semantic/reserved refusals PASS");
}
static void product_crossed_owners(XrXirSourceProduct *product,const char *directory,const char *stdlib) {
    size_t live=runtime_live,bytes=runtime_bytes;char path[1024];
    int length=snprintf(path,sizeof(path),"%s/product2.xr",directory);CHECK(length>0 && (size_t)length<sizeof(path));
    XrCompilerSession *session=xr_compiler_session_new(NULL);CHECK(session);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,directory};
    XrXirSourceProductRequest request={{session,path,&authority,NULL,stdlib,NULL,XR_XIR_PROGRAM,NULL},
        {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION}};
    XrXirSourceProduct *other=NULL;CHECK(xr_xir_source_product_build(&request,&other,NULL)==XR_XIR_OK);
    xr_compiler_session_delete(session);memset(&request,0xcc,sizeof(request));memset(&authority,0xcc,sizeof(authority));
    CHECK(xr_xir_source_product_verify(other,NULL,16777216,NULL)==XR_XIR_OK);
    CHECK(product->facts.function_count==other->facts.function_count && product->facts.module_count==other->facts.module_count);
    CHECK(!memcmp(product->facts.lowered_layout_digest,other->facts.lowered_layout_digest,32));
    CHECK(memcmp(product->facts.source_digest,other->facts.source_digest,32));
    CHECK(memcmp(product->facts.closed_digest,other->facts.closed_digest,32));
    XrXirSourceProduct saved=*product;size_t held_live=runtime_live,held_bytes=runtime_bytes;
    product->source_packet=other->source_packet;memcpy(product->facts.source_digest,other->facts.source_digest,32);
    CHECK(xr_xir_source_product_verify(product,NULL,16777216,NULL)==XR_XIR_BAD_STRUCTURE);
    *product=saved;CHECK(runtime_live==held_live && runtime_bytes==held_bytes);
    product->closed_packet=other->closed_packet;memcpy(product->facts.closed_digest,other->facts.closed_digest,32);
    CHECK(xr_xir_source_product_verify(product,NULL,16777216,NULL)==XR_XIR_BAD_STRUCTURE);
    *product=saved;CHECK(runtime_live==held_live && runtime_bytes==held_bytes);
    product->lowered=other->lowered;
    CHECK(xr_xir_source_product_verify(product,NULL,16777216,NULL)==XR_XIR_BAD_STRUCTURE);
    *product=saved;CHECK(runtime_live==held_live && runtime_bytes==held_bytes);
    xr_xir_source_product_free(other);CHECK(runtime_live==live && runtime_bytes==bytes);
    puts("two genuine independently Checked owners: source/closed/Lowered cross-pair rejection with identical layout PASS");
}
#endif // PRODUCT_IDENTITY_GATES_H
