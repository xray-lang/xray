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
    const XrXirFunctionIdentity ids[3]={{0,1,0,0,0,0,0, 0, 0},{0},{0}};
    const XrXirDeclarations declarations={&source,1,ids,NULL,0,NULL,0,0,2,NULL};
    const XrXirModule module={XR_XIR_BUILT,functions,3,&declarations,NULL,NULL,NULL,XR_XIR_PROGRAM,NULL};
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    size_t live=runtime_live,bytes=runtime_bytes;
    /* The framing contains 26 domain bytes and 43 little-endian words.
     * Work includes hash init/final, each encoded byte and each hash input byte. */
    for (unsigned bound=0;bound<3;++bound) {
        const XrCompileResourceLimits limits={UINT64_MAX,UINT64_MAX,UINT64_MAX};
        XrXirCompileContext context={0};context.limits=xr_xir_compile_default_limits();
        CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
        XrXirArtifact *checked=NULL,*lowered=NULL;
        CHECK(xr_xir_compile_check(&context,&module,&checked,NULL)==XR_XIR_OK);
        CHECK(xr_xir_compile_lower(checked,&target,&lowered,NULL)==XR_XIR_OK);
        xr_xir_compile_artifact_free(checked);
        XrXirSourceProductFacts facts={0};facts.target=target;facts.entry=2;facts.function_count=3;facts.module_count=1;
        static const uint8_t expected[]={0x08,0xe9,0x18,0xfe,0x59,0x68,0xec,0x5d,0xb5,0xa4,0x1b,0x17,0x5e,0xa7,0xf4,0x35,0x3b,0xd6,0x6f,0x02,0x98,0x84,0x5f,0x27,0x49,0xc1,0xfc,0x15,0x81,0x8b,0x8a,0x50};
        XrCompileResourceStats before={0},after={0};
        CHECK(xr_compile_resources_stats(context.resources,&before)==XR_COMPILE_RESOURCE_OK);
        const uint64_t units=27+43*8+33;
        if (bound) CHECK(xr_compile_resources_work(context.resources,UINT64_MAX-before.work-units+(bound==2))==XR_COMPILE_RESOURCE_OK);
        CHECK(xr_compile_resources_stats(context.resources,&before)==XR_COMPILE_RESOURCE_OK);
        uint8_t digest[32];memset(digest,0x5a,sizeof(digest));uint8_t saved[32];memcpy(saved,digest,32);
        XrXirStatus status=source_product_layout_digest(lowered,&facts,digest);
        if (bound==2) CHECK(status==XR_XIR_BUDGET && !memcmp(digest,saved,32));
        else {
            CHECK(status==XR_XIR_OK && !memcmp(digest,expected,32));
            CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK);
            CHECK(after.work-before.work==units);
        }
        const XrXirFunctionLayout *layout=xr_xir_compile_artifact_layout(lowered,0);
        CHECK(layout->slot_count==2 && layout->frame_bytes==8 && layout->result.size==16 && layout->result.alignment==8);
        CHECK(layout->owned_count==1 && !layout->outgoing_count && !layout->path_count);
        CHECK(layout->offsets[0]==0 && layout->offsets[1]==UINT32_MAX && layout->owned_offsets[0]==0);
        CHECK(layout->parameters[0].size==16 && layout->parameters[0].alignment==8);
        xr_compile_resources_release(context.resources);xr_xir_compile_artifact_free(lowered);
        CHECK(runtime_live==live && runtime_bytes==bytes);
    }
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
        CHECK(xr_xir_compile_source_product_verify(product,16777216,NULL)==XR_XIR_BAD_STRUCTURE);*words[i]=old;
        CHECK(runtime_live==live && runtime_bytes==bytes);
    }
    facts->lowered_layout_digest[0]^=1;
    CHECK(xr_xir_compile_source_product_verify(product,16777216,NULL)==XR_XIR_BAD_LAYOUT);facts->lowered_layout_digest[0]^=1;
    facts->source_digest[0]^=1;
    CHECK(xr_xir_compile_source_product_verify(product,16777216,NULL)==XR_XIR_BAD_STRUCTURE);facts->source_digest[0]^=1;
    facts->closed_digest[0]^=1;
    CHECK(xr_xir_compile_source_product_verify(product,16777216,NULL)==XR_XIR_BAD_STRUCTURE);facts->closed_digest[0]^=1;
    for (unsigned kind=0;kind<2;++kind) {
        XrXirCheckedPacket *packet=kind ? &product->closed_packet : &product->source_packet;
        uint8_t *identity=kind ? facts->closed_digest : facts->source_digest;
        uint8_t *copy=xr_malloc(packet->length);CHECK(copy);memcpy(copy,packet->bytes,packet->length);
        uint8_t *previous=packet->bytes;packet->bytes=copy;
        const unsigned positions[]={8,12,20};
        for (size_t i=0;i<sizeof(positions)/sizeof(*positions);++i) {
            memcpy(copy,previous,packet->length);copy[positions[i]]^=1;product_rehash(copy,packet->length);
            xr_sha256(copy,packet->length,identity);
            CHECK(xr_xir_compile_source_product_verify(product,16777216,NULL)==XR_XIR_BAD_STRUCTURE);
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
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(product->context.resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,directory};
    XrXirSourceProductRequest request={{session,path,&authority,&product->context,stdlib,NULL,XR_XIR_PROGRAM,NULL},
        {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION}};
    XrXirSourceProduct *other=NULL;CHECK(xr_xir_compile_source_product_build(&request,&other,NULL)==XR_XIR_OK);
    xr_compile_session_free(session);memset(&request,0xcc,sizeof(request));memset(&authority,0xcc,sizeof(authority));
    CHECK(xr_xir_compile_source_product_verify(other,16777216,NULL)==XR_XIR_OK);
    CHECK(product->facts.function_count==other->facts.function_count && product->facts.module_count==other->facts.module_count);
    CHECK(!memcmp(product->facts.lowered_layout_digest,other->facts.lowered_layout_digest,32));
    CHECK(memcmp(product->facts.source_digest,other->facts.source_digest,32));
    CHECK(memcmp(product->facts.closed_digest,other->facts.closed_digest,32));
    XrXirSourceProduct saved=*product;size_t held_live=runtime_live,held_bytes=runtime_bytes;
    product->source_packet=other->source_packet;memcpy(product->facts.source_digest,other->facts.source_digest,32);
    CHECK(xr_xir_compile_source_product_verify(product,16777216,NULL)==XR_XIR_BAD_STRUCTURE);
    *product=saved;CHECK(runtime_live==held_live && runtime_bytes==held_bytes);
    product->closed_packet=other->closed_packet;memcpy(product->facts.closed_digest,other->facts.closed_digest,32);
    CHECK(xr_xir_compile_source_product_verify(product,16777216,NULL)==XR_XIR_BAD_STRUCTURE);
    *product=saved;CHECK(runtime_live==held_live && runtime_bytes==held_bytes);
    product->lowered=other->lowered;
    CHECK(xr_xir_compile_source_product_verify(product,16777216,NULL)==XR_XIR_BAD_STRUCTURE);
    *product=saved;CHECK(runtime_live==held_live && runtime_bytes==held_bytes);
    xr_xir_compile_source_product_free(other);CHECK(runtime_live==live && runtime_bytes==bytes);
    puts("two genuine independently Checked owners: source/closed/Lowered cross-pair rejection with identical layout PASS");
}
#endif // PRODUCT_IDENTITY_GATES_H
