/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_specialize_index.c - Exact request-local lookup and atomic growth
 *
 * KEY CONCEPT:
 *   A simple complete-field oracle supplies identities independently of the
 *   production hash. Component faults preserve the published prior table.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %d %s\n",__LINE__,#x); exit(1); } } while (0)
#include "xir/xxir.h"
#include "xir_library_compile_owner.h"
#include "xir/xxir_specialize.c"

enum { INDEX_KEYS = 12, INDEX_WIDTH = 128 };
typedef struct IndexFixture {
    SpecInstance keys[INDEX_KEYS];
    XrXirType types[INDEX_KEYS][INDEX_WIDTH];
    XrXirEffectArgument effects[INDEX_KEYS][2];
} IndexFixture;
static void index_fixture(IndexFixture *f) {
    memset(f,0,sizeof(*f));
    for (uint32_t k=0;k<INDEX_KEYS;++k) {
        for (uint32_t a=0;a<INDEX_WIDTH;++a) f->types[k][a]=XR_XIR_I64;
        f->effects[k][0]=(XrXirEffectArgument){0,XR_XIR_I64};
        f->effects[k][1]=(XrXirEffectArgument){1,XR_XIR_F64};
        f->keys[k]=(SpecInstance){0,f->types[k],INDEX_WIDTH,NULL,f->effects[k],2,UINT32_MAX};
    }
    f->types[1][INDEX_WIDTH-1]=XR_XIR_I32;
    f->effects[2][1].type=XR_XIR_I32;
    f->effects[3][1].parameter=2;
    f->keys[4].owner=0;f->keys[5].owner=1;
    f->keys[6].declaration=1;f->keys[7].count=INDEX_WIDTH-1;
    f->keys[8].effect_count=1;f->types[9][0]=XR_XIR_I32;
    /* Key 10 is equal in independent storage; key 11 reverses effect order. */
    f->effects[11][0]=(XrXirEffectArgument){1,XR_XIR_F64};
    f->effects[11][1]=(XrXirEffectArgument){0,XR_XIR_I64};
}
static uint32_t index_reference(const SpecContext *c,const SpecInstance *key) {
    for (uint32_t i=0;i<c->count;++i) {
        const SpecInstance *old=&c->instances[i];
        if (key->declaration!=old->declaration || key->owner!=old->owner ||
            key->count!=old->count || key->effect_count!=old->effect_count) continue;
        bool same=true;
        for (uint32_t a=0;a<key->count;++a) if (key->arguments[a]!=old->arguments[a]) same=false;
        for (uint32_t a=0;a<key->effect_count;++a)
            if (key->effects[a].parameter!=old->effects[a].parameter || key->effects[a].type!=old->effects[a].type) same=false;
        if (same) return i;
    }
    return UINT32_MAX;
}
static uint32_t index_add(SpecContext *c,const SpecInstance *key) {
    uint32_t hash=0;
    if (!spec_instance_hash(c,key,&hash)) return UINT32_MAX;
    uint32_t found=spec_index_find(c,key,hash);
    if (found!=UINT32_MAX || c->diagnostic.status!=XR_XIR_OK) return found;
    if (!spec_index_reserve(c)) return UINT32_MAX;
    uint32_t id=c->count;
    if (!spec_index_place(c,c->instance_index,c->index_capacity,(SpecIndexEntry){id+1,hash})) return UINT32_MAX;
    c->instances[id]=*key;++c->count;return id;
}
static void index_drop(SpecContext *c) {
    while (c->memory) { SpecMemory *next=c->memory->next;xr_compile_resources_free(c->memory);c->memory=next; }
}
static XrXirStatus index_operation(const XrXirCompileContext *context,void *opaque) {
    (void)opaque;IndexFixture fixture;index_fixture(&fixture);SpecInstance instances[INDEX_KEYS];
    SpecContext c={0};c.remaining=*context;c.instances=instances;
    uint32_t unique=0;
    for (uint32_t repeat=0;repeat<2;++repeat) for (uint32_t k=0;k<INDEX_KEYS;++k) {
        uint32_t expected=index_reference(&c,&fixture.keys[k]);
        if (expected==UINT32_MAX) expected=unique++;
        uint32_t actual=index_add(&c,&fixture.keys[k]);
        if (c.diagnostic.status!=XR_XIR_OK) { index_drop(&c);return c.diagnostic.status; }
        CHECK(actual==expected);
    }
    CHECK(c.count==11 && c.index_capacity==32);index_drop(&c);return XR_XIR_OK;
}
static void index_collisions(void) {
    LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
    IndexFixture f;index_fixture(&f);SpecInstance instances[32];SpecContext c={0};c.remaining=owner.context;c.instances=instances;
    const uint32_t bits[]={8408,18786};uint32_t hashes[2]={0};
    for (uint32_t k=0;k<2;++k) {
        for (uint32_t a=0;a<INDEX_WIDTH;++a) f.types[k][a]=XR_XIR_I64;
        for (uint32_t a=0;a<32;++a) f.types[k][a]=(bits[k]&(UINT32_C(1)<<a))?XR_XIR_I32:XR_XIR_I64;
        CHECK(spec_instance_hash(&c,&f.keys[k],&hashes[k]));
    }
    CHECK(hashes[0]==hashes[1] && index_reference(&c,&f.keys[0])==UINT32_MAX);
    CHECK(index_add(&c,&f.keys[0])==0 && index_add(&c,&f.keys[1])==1);
    CHECK(index_add(&c,&f.keys[0])==0 && index_add(&c,&f.keys[1])==1);
    /* All keys below occupy the same initial bucket, then cross a resize. */
    uint32_t found=0;
    for (uint32_t declaration=0;declaration<10000 && found<12;++declaration) {
        SpecInstance key=f.keys[2];key.declaration=declaration;uint32_t hash=0;
        CHECK(spec_instance_hash(&c,&key,&hash));
        if ((hash&31)!=7) continue;
        CHECK(index_reference(&c,&key)==UINT32_MAX);
        CHECK(index_add(&c,&key)==found+2);++found;
    }
    CHECK(found==12 && c.count==14 && c.index_capacity==32);
    for (uint32_t i=0;i<c.count;++i) {
        uint32_t hash=0;CHECK(spec_instance_hash(&c,&instances[i],&hash));
        CHECK(spec_index_find(&c,&instances[i],hash)==i);
    }
    index_drop(&c);library_compile_owner_drop(&owner);
    puts("specialization complete 32-bit hash collision, bucket chains and stable ordinals across growth PASS");
}
static void index_prepare_growth(SpecContext *c,const XrXirCompileContext *context,SpecInstance *instances) {
    *c=(SpecContext){0};c->remaining=*context;c->instances=instances;
    for (uint32_t i=0;i<4;++i) {
        SpecInstance key={i,NULL,0,NULL,NULL,0,UINT32_MAX};CHECK(index_add(c,&key)==i);
    }
    CHECK(c->index_capacity==8 && c->count==4);
}
static void index_growth_failures(void) {
    LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
    SpecInstance instances[4];SpecContext c;index_prepare_growth(&c,&owner.context,instances);
    uint64_t prefix=library_compile_stats(&owner.context).work;
    CHECK(spec_index_reserve(&c));uint64_t total=library_compile_stats(&owner.context).work;
    CHECK(c.index_capacity==16 && total>prefix);index_drop(&c);library_compile_owner_drop(&owner);
    for (uint64_t cut=0;cut<total-prefix;++cut) {
        XrCompileResourceLimits caps=library_compile_limits;caps.work=prefix+cut;
        CHECK(library_compile_owner_new(&owner,&caps)==XR_XIR_OK);
        index_prepare_growth(&c,&owner.context,instances);
        SpecIndexEntry *prior=c.instance_index,saved[8];memcpy(saved,prior,sizeof(saved));
        CHECK(!spec_index_reserve(&c) && c.diagnostic.status==XR_XIR_BUDGET);
        CHECK(c.instance_index==prior && c.index_capacity==8 && c.count==4 && !memcmp(prior,saved,sizeof(saved)));
        index_drop(&c);library_compile_owner_drop(&owner);
    }
    CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
    index_prepare_growth(&c,&owner.context,instances);
    SpecIndexEntry *prior=c.instance_index,saved[8];memcpy(saved,prior,sizeof(saved));
    source_program_compile_fail_at=source_program_compile_attempts;source_program_compile_injected=false;
    CHECK(!spec_index_reserve(&c) && c.diagnostic.status==XR_XIR_OUT_OF_MEMORY && source_program_compile_injected);
    source_program_compile_fail_at=SIZE_MAX;
    CHECK(c.instance_index==prior && c.index_capacity==8 && !memcmp(prior,saved,sizeof(saved)));
    XrCompileResourceStats failed=library_compile_stats(&owner.context);
    c.diagnostic.status=XR_XIR_OK;CHECK(spec_index_reserve(&c) && c.index_capacity==16);
    CHECK(library_compile_stats(&owner.context).work>failed.work);
    index_drop(&c);library_compile_owner_drop(&owner);
    printf("specialization growth atomicity: %llu complete work cuts, allocation failure and same-ledger retry PASS\n",
        (unsigned long long)(total-prefix));
}
int main(void) {
    LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
    CHECK(index_operation(&owner.context,NULL)==XR_XIR_OK);library_compile_owner_drop(&owner);
    index_collisions();index_growth_failures();
    library_compile_operation_cases("specialization index complete keys",index_operation,NULL);
    library_compile_observer_free();return 0;
}
