/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_native_admission.c - Independent native metadata traversal accounting
 */
#include "shared/xnative_declaration.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(test) do { if (!(test)) { fprintf(stderr,"check failed at %d: %s\n",__LINE__,#test); abort(); } } while (0)
typedef struct Work { uint64_t limit, used, calls; } Work;
static bool charge(void *context, uint64_t units) {
    Work *work=context; ++work->calls;
    if (units>work->limit-work->used) return false;
    work->used+=units; return true;
}
static uint64_t expected_text(const char *text) { return 2*(strlen(text)+1); }
static uint64_t expected_admission(const XrNativeTypeDeclaration *declaration) {
    uint64_t units=1+64;
    units+=expected_text(declaration->identity)+expected_text(declaration->source_path)+
        expected_text(declaration->name)+expected_text(declaration->parameter_name);
    for (uint32_t i=0;i<declaration->member_count;++i) {
        const XrNativeMemberDeclaration *member=&declaration->members[i];
        units+=1+expected_text(member->name)+expected_text(member->signature)+
            expected_text(member->result_text)+expected_text(member->failures);
        for (uint32_t p=0;p<member->parameter_count;++p)
            units+=1+expected_text(member->parameters[p].name)+expected_text(member->parameters[p].type_text);
    }
    return units;
}
static void complete_declarations(void) {
    for (uint32_t id=1;id<=2;++id) {
        const XrNativeTypeDeclaration *declaration=xr_native_declaration_by_id(id);
        CHECK(declaration && xr_native_declaration_validate(declaration));
        CHECK(!strcmp(declaration->name,id==1?"Array":"string"));
        CHECK(declaration->member_count==(id==1?32u:19u));
        uint64_t needed=expected_admission(declaration);
        for (uint64_t limit=0;limit<=needed;++limit) {
            Work ledger={limit,0,0};XrNativeDeclarationWork work={&ledger,charge};
            CHECK(xr_native_declaration_admit(&work,declaration)==
                (limit==needed?XR_NATIVE_DECLARATION_OK:XR_NATIVE_DECLARATION_WORK_LIMIT));
            CHECK(ledger.used<=limit && (limit!=needed || ledger.used==needed));
        }
        XrNativeTypeDeclaration mutation=*declaration;mutation.name="wrong";
        Work ledger={UINT64_MAX,0,0};XrNativeDeclarationWork work={&ledger,charge};
        CHECK(xr_native_declaration_admit(&work,&mutation)==XR_NATIVE_DECLARATION_INVALID);
        CHECK(!xr_native_declaration_validate(&mutation));
        mutation=*declaration;
        ((unsigned char *)&mutation.source_fingerprint)[0]^=1;
        ledger=(Work){UINT64_MAX,0,0};
        CHECK(xr_native_declaration_admit(&work,&mutation)==XR_NATIVE_DECLARATION_INVALID);
        CHECK(ledger.used==3+expected_text(declaration->identity)+expected_text(declaration->source_path)+
            expected_text(declaration->name)+expected_text(declaration->parameter_name));
        printf("Native %s independently expected work %llu, every lower limit rejected\n",declaration->name,(unsigned long long)needed);
    }
}
static void lookup_boundaries(void) {
    for (uint64_t limit=0;limit<=12;++limit) {
        Work ledger={limit,0,0};XrNativeDeclarationWork work={&ledger,charge};
        const XrNativeTypeDeclaration *out=NULL;
        CHECK(xr_native_declaration_find(&work,"Array",&out)==
            (limit==12?XR_NATIVE_DECLARATION_OK:XR_NATIVE_DECLARATION_WORK_LIMIT));
        CHECK(limit==12 ? out==xr_native_declaration_by_id(1) : !out);
    }
    Work ledger={4,0,0};XrNativeDeclarationWork work={&ledger,charge};
    const XrNativeTypeDeclaration *out=NULL;
    CHECK(xr_native_declaration_find(&work,"?",&out)==XR_NATIVE_DECLARATION_NOT_FOUND);
    CHECK(!out && ledger.used==4 && ledger.calls==2);
    ledger=(Work){0,0,0};
    CHECK(xr_native_declaration_find(&work,(const char *)(uintptr_t)1,&out)==XR_NATIVE_DECLARATION_WORK_LIMIT);
    CHECK(!out && !ledger.used && ledger.calls==1);
    CHECK(xr_native_declaration_admit(&work,(const XrNativeTypeDeclaration *)(uintptr_t)1)==XR_NATIVE_DECLARATION_WORK_LIMIT);
    const XrNativeMemberDeclaration *member=NULL;
    CHECK(xr_native_declaration_find_member(&work,(const XrNativeTypeDeclaration *)(uintptr_t)1,
        (const char *)(uintptr_t)1,&member)==XR_NATIVE_DECLARATION_WORK_LIMIT && !member);
    out=(const XrNativeTypeDeclaration *)(uintptr_t)1;
    CHECK(xr_native_declaration_find(&work,"Array",&out)==XR_NATIVE_DECLARATION_INVALID);
    CHECK(out==(const XrNativeTypeDeclaration *)(uintptr_t)1);
    ledger=(Work){UINT64_MAX,0,0};
    const XrNativeTypeDeclaration *array=xr_native_declaration_by_id(1);
    CHECK(xr_native_declaration_find_member(&work,array,"get",&member)==XR_NATIVE_DECLARATION_OK);
    CHECK(member && !strcmp(member->name,"get") && member->operation==XR_NATIVE_OPERATION_ARRAY_GET);
    CHECK(xr_native_declaration_member(array,"get")==member);
    CHECK(!xr_native_declaration_by_name("?") && !xr_native_declaration_member(array,"?"));
    uint64_t needed=ledger.used;
    for (uint64_t limit=0;limit<needed;++limit) {
        ledger=(Work){limit,0,0};member=NULL;
        CHECK(xr_native_declaration_find_member(&work,array,"get",&member)==XR_NATIVE_DECLARATION_WORK_LIMIT);
        CHECK(!member);
    }
}
int main(void) { complete_declarations();lookup_boundaries();return 0; }
