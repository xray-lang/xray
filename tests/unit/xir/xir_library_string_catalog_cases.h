/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_library_string_catalog_cases.h - Independent Catalog identity and ownership tests
 * KEY CONCEPT: Every input is checked before one atomic multi-library publication.
 */
#ifndef XIR_LIBRARY_STRING_CATALOG_CASES_H
#define XIR_LIBRARY_STRING_CATALOG_CASES_H
#include "xir_construction_fixture.h"
typedef struct StringCatalogFixture {
    XrXirCheckedPacket packet;
    size_t literal_id, literal_count, literal_length, literal_bytes, member_access;
    size_t unused_bytes;
} StringCatalogFixture;
static StringCatalogFixture string_fixtures[7];
static const char *string_fixture_names[]={"alpha.xr","beta.xr","nul.xr","empty.xr","long.xr","equal.xr","unused.xr"};
static void string_fixture_u32(uint8_t *bytes,uint32_t value) {
    for(unsigned i=0;i<4;++i)bytes[i]=(uint8_t)(value>>(8*i));
}
static size_t string_fixture_find(const XrXirCheckedPacket *packet,const void *bytes,size_t length) {
    CHECK(length && length<=packet->length);
    size_t offset=SIZE_MAX;
    for(size_t i=64;i<=packet->length-length;++i)if(!memcmp(packet->bytes+i,bytes,length)){
        CHECK(offset==SIZE_MAX);offset=i;
    }
    CHECK(offset!=SIZE_MAX);return offset;
}
static StringCatalogFixture string_catalog_fixture(unsigned index) {
    CHECK(index<7);
    char long_text[703];
    for(size_t i=0;i<233;++i)memcpy(long_text+i*3,"汉",3);
    memcpy(long_text+699,"tail",4);
    XrXirLiteral literals[3]={{"甲-owned",9},{0},{0}};
    if(index==1)literals[0]=(XrXirLiteral){"乙-owned",9};
    if(index==2)literals[0]=(XrXirLiteral){"a\0b",3};
    if(index==3)literals[0]=(XrXirLiteral){"",0};
    if(index==4)literals[0]=(XrXirLiteral){long_text,sizeof(long_text)};
    if(index==5){literals[0]=(XrXirLiteral){"=",1};literals[1]=literals[0];}
    if(index==6){literals[0]=(XrXirLiteral){"valid",5};literals[1]=(XrXirLiteral){"!",1};literals[2]=(XrXirLiteral){"x",1};}
    uint32_t literal_count=index==5?2:index==6?3:1;
    const XrXirInstruction answer[]={
        {XR_XIR_CONST_STRING,XR_XIR_STRING,{0},{0},0,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}}};
    const XrXirInstruction init={XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}};
    const XrXirBlock two={0,2,0,0},one={0,1,0,0};
    const XrXirFunction functions[]={
        {"answer",6,NULL,0,XR_XIR_STRING,&two,1,answer,2,NULL,0,.error_type=XR_XIR_NEVER},
        {"$init",5,NULL,0,XR_XIR_UNIT,&one,1,&init,1,NULL,0,.error_type=XR_XIR_NEVER}};
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,"E:/fixture"};
    char *canonical=NULL;
    CHECK(xr_compile_module_identity_from_logical(library_context->resources,&authority,string_fixture_names[index],&canonical)==XR_MODULE_OK);
    XrXirSourceModule source={canonical,(uint32_t)strlen(canonical),NULL,0,1,0,XR_XIR_MODULE_FRESH_STORAGE};
    const XrXirFunctionIdentity identities[2]={{.exported=1},{0}};
    const XrXirDeclarations declarations={.modules=&source,.module_count=1,.functions=identities,
        .literals=literals,.literal_count=literal_count,.root_module=UINT32_MAX,.entry_function=UINT32_MAX};
    const XrXirModule built={.stage=XR_XIR_BUILT,.functions=functions,.function_count=2,
        .declarations=&declarations,.linkage_kind=XR_XIR_LIBRARY};
    XrXirArtifact *owned=NULL;
    CHECK(xir_fixture_check(library_context,&built,&owned,NULL)==XR_XIR_OK);
    StringCatalogFixture fixture={0};
    CHECK(xr_xir_compile_checked_write(owned,&fixture.packet,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(owned);
    /* Locate semantic records by independent fixture values, never a schema
     * version or a private compiler allocation/layout formula. */
    uint8_t instruction[40]={0};string_fixture_u32(instruction,XR_XIR_CONST_STRING);
    string_fixture_u32(instruction+4,XR_XIR_STRING);
    fixture.literal_id=string_fixture_find(&fixture.packet,instruction,sizeof(instruction))+24;
    size_t name=string_fixture_find(&fixture.packet,canonical,strlen(canonical));
    CHECK(name>=24);
    fixture.literal_count=name-16;
    fixture.member_access=name+strlen(canonical)+28;
    fixture.literal_length=name+strlen(canonical)+16+2*40;
    fixture.literal_bytes=fixture.literal_length+4;
    fixture.unused_bytes=fixture.literal_bytes+literals[0].length+4+literals[1].length+4;
    CHECK(fixture.unused_bytes<fixture.packet.length);
    CHECK(!memcmp(fixture.packet.bytes+fixture.literal_bytes,literals[0].bytes,literals[0].length));
    uint8_t count_bytes[4];string_fixture_u32(count_bytes,literal_count);
    CHECK(!memcmp(fixture.packet.bytes+fixture.literal_count,count_bytes,4));
    xr_compile_resources_free(canonical);return fixture;
}
static void string_catalog_digest(const uint8_t *bytes,size_t length,uint8_t digest[32]) {
    XrSHA256Context hash; xr_sha256_init(&hash); xr_sha256_update(&hash,bytes,length);
    xr_sha256_final(&hash,digest);
}
static void string_catalog_rehash(uint8_t *bytes,size_t length) {
    XrSHA256Context hash; xr_sha256_init(&hash); xr_sha256_update(&hash,bytes,32);
    xr_sha256_update(&hash,bytes+64,length-64); xr_sha256_final(&hash,bytes+32);
}
static XrXirLibraryInput string_catalog_input(const char *name,const uint8_t *bytes,size_t length,XrXirLibraryModuleInput *binding) {
    XrXirLibraryInput input={0};input.modules=binding;input.module_count=1;
    input.modules[0].authority=(XrModuleIdentityAuthority){XR_MODULE_IDENTITY_SCRIPT,NULL,"E:/fixture"};
    input.modules[0].logical_path=name; input.packet=bytes; input.length=length;
    string_catalog_digest(bytes,length,input.sha256); return input;
}
static void string_catalog_expect(const XrXirLibraryInput *inputs,size_t count,XrXirStatus expected) {
    size_t live=runtime_live,bytes=runtime_bytes;
    XrXirLibraryCatalog *catalog=NULL;
    size_t clive=source_live,cbytes=source_bytes;
    XrXirStatus status=xr_xir_compile_library_catalog_new_v2(library_context,inputs,count,&catalog);
    if(status!=expected) fprintf(stderr,"catalog expected%u actual%u\n",expected,status);
    CHECK(status==expected && !catalog);
    catalog=(XrXirLibraryCatalog *)(uintptr_t)1;
    CHECK(xr_xir_compile_library_catalog_new_v2(library_context,inputs,count,&catalog)==expected&&catalog==(XrXirLibraryCatalog *)(uintptr_t)1);
    CHECK(source_live==clive&&source_bytes==cbytes);
    CHECK(runtime_live==live && runtime_bytes==bytes);
}
static void string_catalog_roundtrip(void) {
    const uint8_t *vectors[]={string_fixtures[0].packet.bytes,string_fixtures[1].packet.bytes,string_fixtures[2].packet.bytes,
        string_fixtures[3].packet.bytes,string_fixtures[4].packet.bytes,string_fixtures[5].packet.bytes,string_fixtures[6].packet.bytes};
    size_t sizes[]={string_fixtures[0].packet.length,string_fixtures[1].packet.length,string_fixtures[2].packet.length,
        string_fixtures[3].packet.length,string_fixtures[4].packet.length,string_fixtures[5].packet.length,string_fixtures[6].packet.length};
    const char *names[]={"alpha.xr","beta.xr","nul.xr","empty.xr","long.xr","equal.xr","unused.xr"};
    size_t lengths[]={9,9,3,0,703,1,5};
    size_t live=runtime_live,bytes=runtime_bytes;
    XrXirLibraryInput inputs[7];XrXirLibraryModuleInput bindings[7]={{0}};
    for(unsigned i=0;i<7;++i) {
        XrXirArtifact *artifact=NULL; XrXirCheckedPacket packet={0};
        CHECK(xr_xir_compile_checked_read(library_context,vectors[i],sizes[i],&artifact,NULL)==XR_XIR_OK);
        const XrXirDeclarations *d=xr_xir_compile_artifact_module(artifact)->declarations;
        CHECK(d->modules[0].initializer==1 && d->literals[0].length==lengths[i]);
        if(i==2) CHECK(!memcmp(d->literals[0].bytes,"a\0b",3));
        if(i==5) CHECK(d->literal_count==2 && d->literals[0].length==1 && d->literals[1].length==1 &&
            d->literals[0].bytes[0]=='=' && d->literals[1].bytes[0]=='=');
        CHECK(xr_xir_compile_checked_write(artifact,&packet,NULL)==XR_XIR_OK);
        CHECK(packet.length==sizes[i]&&!memcmp(packet.bytes,vectors[i],packet.length));
        xr_xir_compile_checked_packet_free(&packet);xr_xir_compile_artifact_free(artifact);
        inputs[i]=string_catalog_input(names[i],vectors[i],sizes[i],&bindings[i]);
    }
    XrXirLibraryCatalog *catalog=NULL;size_t count=91;
    CHECK(!xr_xir_compile_library_catalog_resources_v2(NULL,&count)&&count==0);
    CHECK(!xr_xir_compile_library_catalog_resources_v2(NULL,NULL));
    CHECK(xr_xir_compile_library_catalog_new_v2(library_context,inputs,7,&catalog)==XR_XIR_OK);
    const XrModuleResourceBinding *resources=xr_xir_compile_library_catalog_resources_v2(catalog,&count);
    CHECK(resources&&count==7&&!xr_xir_compile_library_catalog_resources_v2(catalog,NULL));
    for(size_t i=0;i<count;++i) CHECK(resources[i].checked && !strcmp(resources[i].logical_path,names[i]));
    CHECK(!memcmp(xr_xir_compile_artifact_module(resources[0].checked)->declarations->literals[0].bytes,"甲-owned",9));
    CHECK(!memcmp(xr_xir_compile_artifact_module(resources[1].checked)->declarations->literals[0].bytes,"乙-owned",9));
    xr_xir_compile_library_catalog_free(catalog);
    catalog=NULL;
    XrXirLibraryInput reverse[]={inputs[1],inputs[0]};
    CHECK(xr_xir_compile_library_catalog_new_v2(library_context,reverse,2,&catalog)==XR_XIR_OK);
    resources=xr_xir_compile_library_catalog_resources_v2(catalog,&count);
    CHECK(count==2&&!strcmp(resources[0].logical_path,"beta.xr")&&!strcmp(resources[1].logical_path,"alpha.xr"));
    xr_xir_compile_library_catalog_free(catalog);CHECK(runtime_live==live&&runtime_bytes==bytes);
}
static void string_catalog_bad_inputs(void) {
    XrXirLibraryInput pair[]={string_catalog_input("alpha.xr",string_fixtures[0].packet.bytes,string_fixtures[0].packet.length,(XrXirLibraryModuleInput[1]){0}),
        string_catalog_input("beta.xr",string_fixtures[1].packet.bytes,string_fixtures[1].packet.length,(XrXirLibraryModuleInput[1]){0})};
    string_catalog_expect(NULL,2,XR_XIR_BAD_STRUCTURE);string_catalog_expect(pair,0,XR_XIR_BAD_STRUCTURE);
    XrXirLibraryCatalog *catalog=NULL;
    CHECK(xr_xir_compile_library_catalog_new_v2(library_context,(const XrXirLibraryInput *)(uintptr_t)1,SIZE_MAX,&catalog)==XR_XIR_OVERFLOW&&!catalog);
    XrXirLibraryInput duplicate[]={pair[0],pair[0]};
    string_catalog_expect(duplicate,2,XR_XIR_BAD_STRUCTURE);
    XrXirLibraryModuleInput foreign_binding=*duplicate[1].modules;
    duplicate[1].modules=&foreign_binding;
    foreign_binding.authority.physical_root="E:/foreign";
    string_catalog_expect(duplicate,2,XR_XIR_BAD_STRUCTURE);
    uint8_t *altered=xr_malloc(string_fixtures[0].packet.length);CHECK(altered);memcpy(altered,string_fixtures[0].packet.bytes,string_fixtures[0].packet.length);
    altered[string_fixtures[0].literal_bytes+3]='X';string_catalog_rehash(altered,string_fixtures[0].packet.length);
    duplicate[1]=string_catalog_input("alpha.xr",altered,string_fixtures[0].packet.length,(XrXirLibraryModuleInput[1]){0});
    string_catalog_expect(duplicate,2,XR_XIR_BAD_STRUCTURE);
    pair[1]=string_catalog_input("wrong.xr",string_fixtures[1].packet.bytes,string_fixtures[1].packet.length,(XrXirLibraryModuleInput[1]){0});
    string_catalog_expect(pair,2,XR_XIR_BAD_STRUCTURE);xr_free(altered);
}
static void string_catalog_poison(void) {
    size_t live=runtime_live,bytes=runtime_bytes;
    uint8_t *altered=xr_malloc(string_fixtures[0].packet.length);CHECK(altered);
    const struct {size_t offset;uint32_t value;} attacks[]={
        {string_fixtures[0].literal_id,2}, /* Another input cannot legitimize a foreign literal ID. */
        {string_fixtures[0].literal_id,UINT32_MAX},{string_fixtures[0].literal_id+4,UINT32_MAX},{string_fixtures[0].literal_count,UINT32_MAX},{string_fixtures[0].member_access,UINT32_MAX}, /* Original malformed member_access remains an attack. */
        {string_fixtures[0].literal_length,UINT32_MAX}
    };
    for(size_t i=0;i<sizeof(attacks)/sizeof(attacks[0]);++i) {
        memcpy(altered,string_fixtures[0].packet.bytes,string_fixtures[0].packet.length);
        for(unsigned b=0;b<4;++b)altered[attacks[i].offset+b]=(uint8_t)(attacks[i].value>>(8*b));
        string_catalog_rehash(altered,string_fixtures[0].packet.length);
        XrXirLibraryInput pair[]={string_catalog_input("beta.xr",string_fixtures[1].packet.bytes,string_fixtures[1].packet.length,(XrXirLibraryModuleInput[1]){0}),
            string_catalog_input("alpha.xr",altered,string_fixtures[0].packet.length,(XrXirLibraryModuleInput[1]){0})};
        string_catalog_expect(pair,2,XR_XIR_BAD_STRUCTURE);
    }
    memcpy(altered,string_fixtures[0].packet.bytes,string_fixtures[0].packet.length);altered[string_fixtures[0].literal_bytes]=255;
    XrXirLibraryInput input=string_catalog_input("alpha.xr",altered,string_fixtures[0].packet.length,(XrXirLibraryModuleInput[1]){0});
    string_catalog_expect(&input,1,XR_XIR_BAD_STRUCTURE); /* Correct outerSHA, invalid innerdigest. */
    memcpy(altered,string_fixtures[0].packet.bytes,string_fixtures[0].packet.length);input=string_catalog_input("alpha.xr",altered,string_fixtures[0].packet.length,(XrXirLibraryModuleInput[1]){0});
    input.sha256[0]^=1;string_catalog_expect(&input,1,XR_XIR_BAD_STRUCTURE);
    uint8_t *unused=xr_malloc(string_fixtures[6].packet.length);CHECK(unused);memcpy(unused,string_fixtures[6].packet.bytes,string_fixtures[6].packet.length);
    unused[string_fixtures[6].unused_bytes]=255;string_catalog_rehash(unused,string_fixtures[6].packet.length);
    input=string_catalog_input("unused.xr",unused,string_fixtures[6].packet.length,(XrXirLibraryModuleInput[1]){0});string_catalog_expect(&input,1,XR_XIR_BAD_STRUCTURE);
    memcpy(altered,string_fixtures[0].packet.bytes,string_fixtures[0].packet.length);size_t length=string_fixtures[0].packet.length-1;
    uint64_t payload=length-64;for(unsigned b=0;b<8;++b)altered[24+b]=(uint8_t)(payload>>(8*b));
    string_catalog_rehash(altered,length);input=string_catalog_input("alpha.xr",altered,length,(XrXirLibraryModuleInput[1]){0});
    string_catalog_expect(&input,1,XR_XIR_BAD_STRUCTURE);
    xr_free(unused);xr_free(altered);CHECK(runtime_live==live&&runtime_bytes==bytes);
}
static void string_catalog_ownership(void) {
    size_t live=runtime_live,physical=runtime_bytes;
    uint8_t *bytes=xr_malloc(string_fixtures[0].packet.length);CHECK(bytes);memcpy(bytes,string_fixtures[0].packet.bytes,string_fixtures[0].packet.length);
    char name[]="alpha.xr",root[]="E:/fixture";
    XrXirLibraryInput input=string_catalog_input(name,bytes,string_fixtures[0].packet.length,(XrXirLibraryModuleInput[1]){0});input.modules[0].authority.physical_root=root;
    XrXirLibraryCatalog *catalog=NULL;CHECK(xr_xir_compile_library_catalog_new_v2(library_context,&input,1,&catalog)==XR_XIR_OK);
    memset(bytes,0,string_fixtures[0].packet.length);xr_free(bytes);name[0]='X';root[0]='X';memset(&input,0,sizeof(input));
    size_t count=0;const XrModuleResourceBinding *resources=xr_xir_compile_library_catalog_resources_v2(catalog,&count);
    CHECK(count==1&&!strcmp(resources[0].logical_path,"alpha.xr")&&!strcmp(resources[0].authority.physical_root,"E:/fixture"));
    XrXirArtifact *copy=NULL;CHECK(xr_xir_compile_recheck_v2(library_context,xr_xir_compile_artifact_module(resources[0].checked),xr_xir_compile_artifact_construction(resources[0].checked),&copy,NULL, NULL)==XR_XIR_OK);
    xr_xir_compile_library_catalog_free(catalog);CHECK(xr_xir_compile_artifact_verify(copy,NULL)==XR_XIR_OK);
    const XrXirDeclarations *d=xr_xir_compile_artifact_module(copy)->declarations;
    CHECK(d->literals[0].length==9&&!memcmp(d->literals[0].bytes,"甲-owned",9));
    xr_xir_compile_artifact_free(copy);CHECK(runtime_live==live&&runtime_bytes==physical);
}
static XrCompileResourceStats string_catalog_cost(const XrXirLibraryInput *inputs,size_t count) {
    LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
    LibrarySourceFixture fixture={inputs,count,NULL,{0},XR_XIR_LIBRARY};CHECK(library_catalog_operation(&owner.context,&fixture)==XR_XIR_OK);
    XrCompileResourceStats stats=library_compile_stats(&owner.context);library_compile_owner_drop(&owner);return stats;
}
static void string_catalog_budgets_and_faults(void) {
    XrXirLibraryInput inputs[]={string_catalog_input("alpha.xr",string_fixtures[0].packet.bytes,string_fixtures[0].packet.length,(XrXirLibraryModuleInput[1]){0}),
        string_catalog_input("beta.xr",string_fixtures[1].packet.bytes,string_fixtures[1].packet.length,(XrXirLibraryModuleInput[1]){0})};
    XrCompileResourceStats a=string_catalog_cost(inputs,1),b=string_catalog_cost(inputs+1,1),pair=string_catalog_cost(inputs,2);
    CHECK(pair.allocated_bytes>a.allocated_bytes&&pair.allocated_bytes>b.allocated_bytes);
    CHECK(pair.allocation_count>a.allocation_count&&pair.allocation_count>b.allocation_count);
    CHECK(pair.peak_bytes>a.peak_bytes&&pair.peak_bytes>b.peak_bytes);
    LibraryCompileOwner owner={0};XrCompileResourceLimits limits=library_compile_limits;
    limits.memory_limit=a.peak_bytes>b.peak_bytes?a.peak_bytes:b.peak_bytes;
    CHECK(library_compile_owner_new(&owner,&limits)==XR_XIR_OK);
    LibrarySourceFixture rejected={inputs,2,NULL,{0},XR_XIR_LIBRARY};
    CHECK(library_catalog_operation(&owner.context,&rejected)==XR_XIR_MEMORY_LIMIT);library_compile_owner_drop(&owner);
    LibrarySourceFixture fixture={inputs,1,NULL,{0},XR_XIR_LIBRARY};library_compile_operation_cases("Catalog firstinput",library_catalog_operation,&fixture);
    fixture.count=2;library_compile_operation_cases("Catalog secondprefix",library_catalog_operation,&fixture);
}
static void library_string_catalog_cases(void) {
    for(unsigned i=0;i<7;++i)string_fixtures[i]=string_catalog_fixture(i);
    string_catalog_roundtrip();string_catalog_bad_inputs();string_catalog_poison();
    string_catalog_ownership();string_catalog_budgets_and_faults();
    for(unsigned i=0;i<7;++i){xr_xir_compile_checked_packet_free(&string_fixtures[i].packet);string_fixtures[i]=(StringCatalogFixture){0};}
}
#endif // XIR_LIBRARY_STRING_CATALOG_CASES_H
