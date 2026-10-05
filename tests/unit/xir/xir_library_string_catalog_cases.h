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
#include "xir_library_string_goldens.h"
#include "xir_library_string64_goldens.h"
#include "xir_library_string65_goldens.h"
#include "xir_library_prior_packet_rejection.h"
static void string_catalog_digest(const uint8_t *bytes,size_t length,uint8_t digest[32]) {
    XrSHA256Context hash; xr_sha256_init(&hash); xr_sha256_update(&hash,bytes,length);
    xr_sha256_final(&hash,digest);
}
static void string_catalog_rehash(uint8_t *bytes,size_t length) {
    XrSHA256Context hash; xr_sha256_init(&hash); xr_sha256_update(&hash,bytes,32);
    xr_sha256_update(&hash,bytes+64,length-64); xr_sha256_final(&hash,bytes+32);
}
static XrXirLibraryInput string_catalog_input(const char *name,const uint8_t *bytes,size_t length) {
    XrXirLibraryInput input={0};
    input.authority=(XrModuleIdentityAuthority){XR_MODULE_IDENTITY_SCRIPT,NULL,"E:/fixture"};
    input.logical_path=name; input.packet=bytes; input.length=length;
    string_catalog_digest(bytes,length,input.sha256); return input;
}
static void string_catalog_expect(const XrXirLibraryInput *inputs,size_t count,XrXirStatus expected) {
    size_t live=runtime_live,bytes=runtime_bytes;
    XrXirLibraryCatalog *catalog=NULL;
    size_t clive=source_live,cbytes=source_bytes;
    XrXirStatus status=xr_xir_compile_library_catalog_new(library_context,inputs,count,&catalog);
    if(status!=expected) fprintf(stderr,"catalog expected%u actual%u\n",expected,status);
    CHECK(status==expected && !catalog);
    catalog=(XrXirLibraryCatalog *)(uintptr_t)1;
    CHECK(xr_xir_compile_library_catalog_new(library_context,inputs,count,&catalog)==expected&&catalog==(XrXirLibraryCatalog *)(uintptr_t)1);
    CHECK(source_live==clive&&source_bytes==cbytes);
    CHECK(runtime_live==live && runtime_bytes==bytes);
}
static void string_catalog_golden_roundtrip(void) {
    library_prior_packet_rejection(library_context,library_string_alpha,sizeof(library_string_alpha));
    library_prior_packet_rejection(library_context,library_string64_alpha,sizeof(library_string64_alpha));
    library_prior_packet_rejection(library_context,library_string_beta,sizeof(library_string_beta));
    library_prior_packet_rejection(library_context,library_string64_beta,sizeof(library_string64_beta));
    library_prior_packet_rejection(library_context,library_string_nul,sizeof(library_string_nul));
    library_prior_packet_rejection(library_context,library_string64_nul,sizeof(library_string64_nul));
    library_prior_packet_rejection(library_context,library_string_empty,sizeof(library_string_empty));
    library_prior_packet_rejection(library_context,library_string64_empty,sizeof(library_string64_empty));
    library_prior_packet_rejection(library_context,library_string_long,sizeof(library_string_long));
    library_prior_packet_rejection(library_context,library_string64_long,sizeof(library_string64_long));
    library_prior_packet_rejection(library_context,library_string_equal,sizeof(library_string_equal));
    library_prior_packet_rejection(library_context,library_string64_equal,sizeof(library_string64_equal));
    library_prior_packet_rejection(library_context,library_string_unused,sizeof(library_string_unused));
    library_prior_packet_rejection(library_context,library_string64_unused,sizeof(library_string64_unused));
    const uint8_t *vectors[]={library_string65_alpha,library_string65_beta,library_string65_nul,
        library_string65_empty,library_string65_long,library_string65_equal,library_string65_unused};
    size_t sizes[]={sizeof(library_string65_alpha),sizeof(library_string65_beta),sizeof(library_string65_nul),
        sizeof(library_string65_empty),sizeof(library_string65_long),sizeof(library_string65_equal),sizeof(library_string65_unused)};
    const char *names[]={"alpha.xr","beta.xr","nul.xr","empty.xr","long.xr","equal.xr","unused.xr"};
    size_t lengths[]={9,9,3,0,703,1,5};
    size_t live=runtime_live,bytes=runtime_bytes;
    XrXirLibraryInput inputs[7];
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
        inputs[i]=string_catalog_input(names[i],vectors[i],sizes[i]);
    }
    XrXirLibraryCatalog *catalog=NULL;size_t count=91;
    CHECK(!xr_xir_compile_library_catalog_resources(NULL,&count)&&count==0);
    CHECK(!xr_xir_compile_library_catalog_resources(NULL,NULL));
    CHECK(xr_xir_compile_library_catalog_new(library_context,inputs,7,&catalog)==XR_XIR_OK);
    const XrModuleResourceBinding *resources=xr_xir_compile_library_catalog_resources(catalog,&count);
    CHECK(resources&&count==7&&!xr_xir_compile_library_catalog_resources(catalog,NULL));
    for(size_t i=0;i<count;++i) CHECK(resources[i].checked && !strcmp(resources[i].logical_path,names[i]));
    CHECK(!memcmp(xr_xir_compile_artifact_module(resources[0].checked)->declarations->literals[0].bytes,"甲-owned",9));
    CHECK(!memcmp(xr_xir_compile_artifact_module(resources[1].checked)->declarations->literals[0].bytes,"乙-owned",9));
    xr_xir_compile_library_catalog_free(catalog);
    XrXirLibraryInput reverse[]={inputs[1],inputs[0]};
    CHECK(xr_xir_compile_library_catalog_new(library_context,reverse,2,&catalog)==XR_XIR_OK);
    resources=xr_xir_compile_library_catalog_resources(catalog,&count);
    CHECK(count==2&&!strcmp(resources[0].logical_path,"beta.xr")&&!strcmp(resources[1].logical_path,"alpha.xr"));
    xr_xir_compile_library_catalog_free(catalog);CHECK(runtime_live==live&&runtime_bytes==bytes);
}
static void string_catalog_bad_inputs(void) {
    XrXirLibraryInput pair[]={string_catalog_input("alpha.xr",library_string65_alpha,sizeof(library_string65_alpha)),
        string_catalog_input("beta.xr",library_string65_beta,sizeof(library_string65_beta))};
    string_catalog_expect(NULL,2,XR_XIR_BAD_STRUCTURE);string_catalog_expect(pair,0,XR_XIR_BAD_STRUCTURE);
    LibraryCompileOwner limited={0};XrCompileResourceLimits limits=library_compile_limits;limits.work=1;
    CHECK(library_compile_owner_new(&limited,&limits)==XR_XIR_OK);XrXirLibraryCatalog *catalog=NULL;
    CHECK(xr_xir_compile_library_catalog_new(&limited.context,(const XrXirLibraryInput *)(uintptr_t)1,1,&catalog)==XR_XIR_BUDGET&&!catalog);
    library_compile_owner_drop(&limited);
    CHECK(xr_xir_compile_library_catalog_new(library_context,(const XrXirLibraryInput *)(uintptr_t)1,SIZE_MAX,&catalog)==XR_XIR_BUDGET&&!catalog);
    XrXirLibraryInput duplicate[]={pair[0],pair[0]};
    string_catalog_expect(duplicate,2,XR_XIR_BAD_STRUCTURE);
    duplicate[1].authority.physical_root="E:/foreign";
    string_catalog_expect(duplicate,2,XR_XIR_BAD_STRUCTURE);
    uint8_t altered[sizeof(library_string65_alpha)];memcpy(altered,library_string65_alpha,sizeof(altered));
    altered[633+3]='X';string_catalog_rehash(altered,sizeof(altered));
    duplicate[1]=string_catalog_input("alpha.xr",altered,sizeof(altered));
    string_catalog_expect(duplicate,2,XR_XIR_BAD_STRUCTURE);
    pair[1]=string_catalog_input("wrong.xr",library_string65_beta,sizeof(library_string65_beta));
    string_catalog_expect(pair,2,XR_XIR_BAD_STRUCTURE);
}
static void string_catalog_poison(void) {
    size_t live=runtime_live,bytes=runtime_bytes;
    uint8_t altered[sizeof(library_string65_alpha)];
    const struct {size_t offset;uint32_t value;} attacks[]={
        {144,2}, /* Original count is2: merged count4 cannot legitimize ID2. */
        {144,UINT32_MAX},{148,UINT32_MAX},{466,UINT32_MAX},{629,UINT32_MAX}
    };
    for(size_t i=0;i<sizeof(attacks)/sizeof(attacks[0]);++i) {
        memcpy(altered,library_string65_alpha,sizeof(altered));
        for(unsigned b=0;b<4;++b)altered[attacks[i].offset+b]=(uint8_t)(attacks[i].value>>(8*b));
        string_catalog_rehash(altered,sizeof(altered));
        XrXirLibraryInput pair[]={string_catalog_input("beta.xr",library_string65_beta,sizeof(library_string65_beta)),
            string_catalog_input("alpha.xr",altered,sizeof(altered))};
        string_catalog_expect(pair,2,XR_XIR_BAD_STRUCTURE);
    }
    memcpy(altered,library_string65_alpha,sizeof(altered));altered[633]=255;
    XrXirLibraryInput input=string_catalog_input("alpha.xr",altered,sizeof(altered));
    string_catalog_expect(&input,1,XR_XIR_BAD_STRUCTURE); /* Correct outerSHA, invalid innerdigest. */
    memcpy(altered,library_string65_alpha,sizeof(altered));input=string_catalog_input("alpha.xr",altered,sizeof(altered));
    input.sha256[0]^=1;string_catalog_expect(&input,1,XR_XIR_BAD_STRUCTURE);
    uint8_t unused[sizeof(library_string65_unused)];memcpy(unused,library_string65_unused,sizeof(unused));
    unused[649]=255;string_catalog_rehash(unused,sizeof(unused));
    input=string_catalog_input("unused.xr",unused,sizeof(unused));string_catalog_expect(&input,1,XR_XIR_BAD_STRUCTURE);
    memcpy(altered,library_string65_alpha,sizeof(altered));size_t length=sizeof(altered)-1;
    uint64_t payload=length-64;for(unsigned b=0;b<8;++b)altered[24+b]=(uint8_t)(payload>>(8*b));
    string_catalog_rehash(altered,length);input=string_catalog_input("alpha.xr",altered,length);
    string_catalog_expect(&input,1,XR_XIR_BAD_STRUCTURE);
    CHECK(runtime_live==live&&runtime_bytes==bytes);
}
static void string_catalog_ownership(void) {
    size_t live=runtime_live,physical=runtime_bytes;
    uint8_t *bytes=malloc(sizeof(library_string65_alpha));CHECK(bytes);memcpy(bytes,library_string65_alpha,sizeof(library_string65_alpha));
    char name[]="alpha.xr",root[]="E:/fixture";
    XrXirLibraryInput input=string_catalog_input(name,bytes,sizeof(library_string65_alpha));input.authority.physical_root=root;
    XrXirLibraryCatalog *catalog=NULL;CHECK(xr_xir_compile_library_catalog_new(library_context,&input,1,&catalog)==XR_XIR_OK);
    memset(bytes,0,sizeof(library_string65_alpha));free(bytes);name[0]='X';root[0]='X';memset(&input,0,sizeof(input));
    size_t count=0;const XrModuleResourceBinding *resources=xr_xir_compile_library_catalog_resources(catalog,&count);
    CHECK(count==1&&!strcmp(resources[0].logical_path,"alpha.xr")&&!strcmp(resources[0].authority.physical_root,"E:/fixture"));
    XrXirArtifact *copy=NULL;CHECK(xr_xir_compile_recheck(library_context,xr_xir_compile_artifact_module(resources[0].checked),&copy,NULL)==XR_XIR_OK);
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
    XrXirLibraryInput inputs[]={string_catalog_input("alpha.xr",library_string65_alpha,sizeof(library_string65_alpha)),
        string_catalog_input("beta.xr",library_string65_beta,sizeof(library_string65_beta))};
    XrCompileResourceStats a=string_catalog_cost(inputs,1),b=string_catalog_cost(inputs+1,1),pair=string_catalog_cost(inputs,2);
    CHECK(pair.work>a.work&&pair.work>b.work&&pair.allocated_bytes>a.allocated_bytes&&pair.allocated_bytes>b.allocated_bytes);
    CHECK(pair.allocation_count>a.allocation_count&&pair.allocation_count>b.allocation_count);
    for(unsigned axis=0;axis<2;++axis){LibraryCompileOwner owner={0};XrCompileResourceLimits limits=library_compile_limits;
        if(axis)limits.allocated_bytes=a.allocated_bytes>b.allocated_bytes?a.allocated_bytes:b.allocated_bytes;
        else limits.work=a.work>b.work?a.work:b.work;
        CHECK(library_compile_owner_new(&owner,&limits)==XR_XIR_OK);LibrarySourceFixture fixture={inputs,2,NULL,{0},XR_XIR_LIBRARY};
        CHECK(library_catalog_operation(&owner.context,&fixture)==XR_XIR_BUDGET);library_compile_owner_drop(&owner);
    }
    LibrarySourceFixture fixture={inputs,1,NULL,{0},XR_XIR_LIBRARY};library_compile_operation_cases("Catalog firstinput",library_catalog_operation,&fixture);
    fixture.count=2;library_compile_operation_cases("Catalog secondprefix",library_catalog_operation,&fixture);
}
static void library_string_catalog_cases(void) {
    string_catalog_golden_roundtrip();string_catalog_bad_inputs();string_catalog_poison();
    string_catalog_ownership();string_catalog_budgets_and_faults();
}
#endif // XIR_LIBRARY_STRING_CATALOG_CASES_H
