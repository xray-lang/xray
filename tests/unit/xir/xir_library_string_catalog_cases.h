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
    XrXirLibraryCatalog *catalog=(XrXirLibraryCatalog *)(uintptr_t)1;
    XrXirStatus status=xr_xir_library_catalog_new(inputs,count,NULL,&catalog);
    if(status!=expected) fprintf(stderr,"catalog expected%u actual%u\n",expected,status);
    CHECK(status==expected && !catalog);
    CHECK(runtime_live==live && runtime_bytes==bytes);
}
static void string_catalog_golden_roundtrip(void) {
    const uint8_t *vectors[]={library_string_alpha,library_string_beta,library_string_nul,
        library_string_empty,library_string_long,library_string_equal,library_string_unused};
    size_t sizes[]={sizeof(library_string_alpha),sizeof(library_string_beta),sizeof(library_string_nul),
        sizeof(library_string_empty),sizeof(library_string_long),sizeof(library_string_equal),sizeof(library_string_unused)};
    const char *names[]={"alpha.xr","beta.xr","nul.xr","empty.xr","long.xr","equal.xr","unused.xr"};
    size_t lengths[]={9,9,3,0,703,1,5};
    size_t live=runtime_live,bytes=runtime_bytes;
    XrXirLibraryInput inputs[7];
    for(unsigned i=0;i<7;++i) {
        XrXirArtifact *artifact=NULL; XrXirCheckedPacket packet={0};
        CHECK(xr_xir_checked_read(vectors[i],sizes[i],NULL,&artifact,NULL)==XR_XIR_OK);
        const XrXirDeclarations *d=xr_xir_artifact_module(artifact)->declarations;
        CHECK(d->modules[0].initializer==1 && d->literals[0].length==lengths[i]);
        if(i==2) CHECK(!memcmp(d->literals[0].bytes,"a\0b",3));
        if(i==5) CHECK(d->literal_count==2 && d->literals[0].length==1 && d->literals[1].length==1 &&
            d->literals[0].bytes[0]=='=' && d->literals[1].bytes[0]=='=');
        CHECK(xr_xir_checked_write(artifact,NULL,&packet,NULL)==XR_XIR_OK);
        CHECK(packet.length==sizes[i]&&!memcmp(packet.bytes,vectors[i],packet.length));
        xr_xir_checked_packet_free(&packet);xr_xir_artifact_free(artifact);
        inputs[i]=string_catalog_input(names[i],vectors[i],sizes[i]);
    }
    XrXirLibraryCatalog *catalog=NULL;size_t count=91;
    CHECK(!xr_xir_library_catalog_resources(NULL,&count)&&count==0);
    CHECK(!xr_xir_library_catalog_resources(NULL,NULL));
    CHECK(xr_xir_library_catalog_new(inputs,7,NULL,&catalog)==XR_XIR_OK);
    const XrModuleResourceBinding *resources=xr_xir_library_catalog_resources(catalog,&count);
    CHECK(resources&&count==7&&!xr_xir_library_catalog_resources(catalog,NULL));
    for(size_t i=0;i<count;++i) CHECK(resources[i].checked && !strcmp(resources[i].logical_path,names[i]));
    CHECK(!memcmp(xr_xir_artifact_module(resources[0].checked)->declarations->literals[0].bytes,"甲-owned",9));
    CHECK(!memcmp(xr_xir_artifact_module(resources[1].checked)->declarations->literals[0].bytes,"乙-owned",9));
    xr_xir_library_catalog_free(catalog);
    XrXirLibraryInput reverse[]={inputs[1],inputs[0]};
    CHECK(xr_xir_library_catalog_new(reverse,2,NULL,&catalog)==XR_XIR_OK);
    resources=xr_xir_library_catalog_resources(catalog,&count);
    CHECK(count==2&&!strcmp(resources[0].logical_path,"beta.xr")&&!strcmp(resources[1].logical_path,"alpha.xr"));
    xr_xir_library_catalog_free(catalog);CHECK(runtime_live==live&&runtime_bytes==bytes);
}
static void string_catalog_bad_inputs(void) {
    XrXirLibraryInput pair[]={string_catalog_input("alpha.xr",library_string_alpha,sizeof(library_string_alpha)),
        string_catalog_input("beta.xr",library_string_beta,sizeof(library_string_beta))};
    string_catalog_expect(NULL,2,XR_XIR_BAD_STRUCTURE);string_catalog_expect(pair,0,XR_XIR_BAD_STRUCTURE);
    XrXirLibraryCatalog *catalog=(XrXirLibraryCatalog *)(uintptr_t)1;
    XrXirBudget budget=xr_xir_default_budget(); budget.work=0;
    CHECK(xr_xir_library_catalog_new((const XrXirLibraryInput *)(uintptr_t)1,1,&budget,&catalog)==XR_XIR_BUDGET&&!catalog);
    CHECK(xr_xir_library_catalog_new((const XrXirLibraryInput *)(uintptr_t)1,SIZE_MAX,NULL,&catalog)==XR_XIR_BUDGET&&!catalog);
    XrXirLibraryInput duplicate[]={pair[0],pair[0]};
    string_catalog_expect(duplicate,2,XR_XIR_BAD_STRUCTURE);
    duplicate[1].authority.physical_root="E:/foreign";
    string_catalog_expect(duplicate,2,XR_XIR_BAD_STRUCTURE);
    uint8_t altered[sizeof(library_string_alpha)];memcpy(altered,library_string_alpha,sizeof(altered));
    altered[633+3]='X';string_catalog_rehash(altered,sizeof(altered));
    duplicate[1]=string_catalog_input("alpha.xr",altered,sizeof(altered));
    string_catalog_expect(duplicate,2,XR_XIR_BAD_STRUCTURE);
    pair[1]=string_catalog_input("wrong.xr",library_string_beta,sizeof(library_string_beta));
    string_catalog_expect(pair,2,XR_XIR_BAD_STRUCTURE);
}
static void string_catalog_poison(void) {
    size_t live=runtime_live,bytes=runtime_bytes;
    uint8_t altered[sizeof(library_string_alpha)];
    const struct {size_t offset;uint32_t value;} attacks[]={
        {144,2}, /* Original count is2: merged count4 cannot legitimize ID2. */
        {144,UINT32_MAX},{148,UINT32_MAX},{466,UINT32_MAX},{629,UINT32_MAX}
    };
    for(size_t i=0;i<sizeof(attacks)/sizeof(attacks[0]);++i) {
        memcpy(altered,library_string_alpha,sizeof(altered));
        for(unsigned b=0;b<4;++b)altered[attacks[i].offset+b]=(uint8_t)(attacks[i].value>>(8*b));
        string_catalog_rehash(altered,sizeof(altered));
        XrXirLibraryInput pair[]={string_catalog_input("beta.xr",library_string_beta,sizeof(library_string_beta)),
            string_catalog_input("alpha.xr",altered,sizeof(altered))};
        string_catalog_expect(pair,2,XR_XIR_BAD_STRUCTURE);
    }
    memcpy(altered,library_string_alpha,sizeof(altered));altered[633]=255;
    XrXirLibraryInput input=string_catalog_input("alpha.xr",altered,sizeof(altered));
    string_catalog_expect(&input,1,XR_XIR_BAD_STRUCTURE); /* Correct outerSHA, invalid innerdigest. */
    memcpy(altered,library_string_alpha,sizeof(altered));input=string_catalog_input("alpha.xr",altered,sizeof(altered));
    input.sha256[0]^=1;string_catalog_expect(&input,1,XR_XIR_BAD_STRUCTURE);
    uint8_t unused[sizeof(library_string_unused)];memcpy(unused,library_string_unused,sizeof(unused));
    unused[649]=255;string_catalog_rehash(unused,sizeof(unused));
    input=string_catalog_input("unused.xr",unused,sizeof(unused));string_catalog_expect(&input,1,XR_XIR_BAD_STRUCTURE);
    memcpy(altered,library_string_alpha,sizeof(altered));size_t length=sizeof(altered)-1;
    uint64_t payload=length-64;for(unsigned b=0;b<8;++b)altered[24+b]=(uint8_t)(payload>>(8*b));
    string_catalog_rehash(altered,length);input=string_catalog_input("alpha.xr",altered,length);
    string_catalog_expect(&input,1,XR_XIR_BAD_STRUCTURE);
    CHECK(runtime_live==live&&runtime_bytes==bytes);
}
static void string_catalog_ownership(void) {
    size_t live=runtime_live,physical=runtime_bytes;
    uint8_t *bytes=malloc(sizeof(library_string_alpha));CHECK(bytes);memcpy(bytes,library_string_alpha,sizeof(library_string_alpha));
    char name[]="alpha.xr",root[]="E:/fixture";
    XrXirLibraryInput input=string_catalog_input(name,bytes,sizeof(library_string_alpha));input.authority.physical_root=root;
    XrXirLibraryCatalog *catalog=NULL;CHECK(xr_xir_library_catalog_new(&input,1,NULL,&catalog)==XR_XIR_OK);
    memset(bytes,0,sizeof(library_string_alpha));free(bytes);name[0]='X';root[0]='X';memset(&input,0,sizeof(input));
    size_t count=0;const XrModuleResourceBinding *resources=xr_xir_library_catalog_resources(catalog,&count);
    CHECK(count==1&&!strcmp(resources[0].logical_path,"alpha.xr")&&!strcmp(resources[0].authority.physical_root,"E:/fixture"));
    XrXirArtifact *copy=NULL;CHECK(xr_xir_recheck(xr_xir_artifact_module(resources[0].checked),NULL,&copy,NULL)==XR_XIR_OK);
    xr_xir_library_catalog_free(catalog);CHECK(xr_xir_artifact_verify(copy,NULL,NULL)==XR_XIR_OK);
    const XrXirDeclarations *d=xr_xir_artifact_module(copy)->declarations;
    CHECK(d->literals[0].length==9&&!memcmp(d->literals[0].bytes,"甲-owned",9));
    xr_xir_artifact_free(copy);CHECK(runtime_live==live&&runtime_bytes==physical);
}
static uint64_t string_catalog_threshold(const XrXirLibraryInput *inputs,size_t count,unsigned dimension) {
    XrXirBudget full=xr_xir_default_budget();uint64_t lo=0,hi=dimension==0?full.work:(dimension==1?full.metadata_bytes:full.scratch_bytes);
    unsigned rounds=0;size_t live=runtime_live,bytes=runtime_bytes;
    while(lo<hi) {
        CHECK(++rounds<=64);uint64_t middle=lo+(hi-lo)/2;XrXirBudget budget=full;
        if(!dimension)budget.work=middle;else if(dimension==1)budget.metadata_bytes=middle;else budget.scratch_bytes=middle;
        XrXirLibraryCatalog *catalog=NULL;XrXirStatus status=xr_xir_library_catalog_new(inputs,count,&budget,&catalog);
        CHECK(status==XR_XIR_OK||status==XR_XIR_BUDGET);
        if(status==XR_XIR_OK){xr_xir_library_catalog_free(catalog);hi=middle;}else{CHECK(!catalog);lo=middle+1;}
        CHECK(runtime_live==live&&runtime_bytes==bytes);
    }
    CHECK(lo);for(unsigned less=0;less<2;++less) {
        XrXirBudget budget=full;if(!dimension)budget.work=lo-less;else if(dimension==1)budget.metadata_bytes=lo-less;else budget.scratch_bytes=lo-less;
        XrXirBudget before=budget;XrXirLibraryCatalog *catalog=NULL;
        CHECK(xr_xir_library_catalog_new(inputs,count,&budget,&catalog)==(less?XR_XIR_BUDGET:XR_XIR_OK));
        CHECK(!memcmp(&before,&budget,sizeof(budget)));xr_xir_library_catalog_free(catalog);
        CHECK(runtime_live==live&&runtime_bytes==bytes);
    }
    return lo;
}
static void string_catalog_budgets_and_faults(void) {
    XrXirLibraryInput inputs[]={string_catalog_input("alpha.xr",library_string_alpha,sizeof(library_string_alpha)),
        string_catalog_input("beta.xr",library_string_beta,sizeof(library_string_beta))};
    size_t live=runtime_live,bytes=runtime_bytes;XrXirLibraryCatalog *catalog=NULL;
    runtime_attempts=0;CHECK(xr_xir_library_catalog_new(inputs,1,NULL,&catalog)==XR_XIR_OK);size_t first=runtime_attempts;
    xr_xir_library_catalog_free(catalog);runtime_attempts=0;
    CHECK(xr_xir_library_catalog_new(inputs,2,NULL,&catalog)==XR_XIR_OK);size_t sites=runtime_attempts;
    CHECK(sites>first);xr_xir_library_catalog_free(catalog);
    for(size_t i=0;i<sites;++i) {
        runtime_attempts=0;runtime_fail_at=i;catalog=NULL;
        XrXirStatus status=xr_xir_library_catalog_new(inputs,2,NULL,&catalog);runtime_fail_at=SIZE_MAX;
        if(status!=XR_XIR_OUT_OF_MEMORY)fprintf(stderr,"catalog fault %zu/%zu status%u\n",i,sites,status);
        CHECK(status==XR_XIR_OUT_OF_MEMORY&&!catalog);CHECK(runtime_live==live&&runtime_bytes==bytes);
    }
    for(unsigned dimension=0;dimension<3;++dimension) {
        uint64_t pair=string_catalog_threshold(inputs,2,dimension);
        if(dimension<2) {
            uint64_t a=string_catalog_threshold(inputs,1,dimension),b=string_catalog_threshold(inputs+1,1,dimension);
            CHECK(pair>a&&pair>b);XrXirBudget budget=xr_xir_default_budget();
            if(!dimension)budget.work=a>b?a:b;else budget.metadata_bytes=a>b?a:b;
            CHECK(xr_xir_library_catalog_new(inputs,2,&budget,&catalog)==XR_XIR_BUDGET&&!catalog);
        }
        printf("string Catalog dimension%u exact%llu/minus1 PASS\n",dimension,(unsigned long long)pair);
    }
    CHECK(runtime_live==live&&runtime_bytes==bytes);
    printf("string Catalog full hooked OOM=%zu firstinput=%zu; secondprefix physicalzero\n",sites,first);
}
static void library_string_catalog_cases(void) {
    string_catalog_golden_roundtrip();string_catalog_bad_inputs();string_catalog_poison();
    string_catalog_ownership();string_catalog_budgets_and_faults();
}
#endif // XIR_LIBRARY_STRING_CATALOG_CASES_H
