/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_native_admission.c - Canonical native metadata and forged declaration rejection
 */
#include "shared/xnative_declaration.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(test) do { if (!(test)) { fprintf(stderr,"check failed at %d: %s\n",__LINE__,#test); abort(); } } while (0)
static const char *const native_names[]={"Array","string","Atomic","Ordering","Task","Channel","Recv","SendResult","Iterator","Map","Set","i64","f64","bool","rune"};
static const uint32_t native_ids[]={1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};
static const uint32_t native_members[]={32,19,10,5,3,10,4,4,5,13,15,29,12,1,6};
static const uint32_t native_parameters[]={1,0,1,0,2,1,1,0,2,2,1,0,0,0,0};
static void complete_declarations(void) {
    for (uint32_t id=1;id<=sizeof(native_names)/sizeof(*native_names);++id) {
        const XrNativeTypeDeclaration *declaration=xr_native_declaration_by_id(native_ids[id-1]);
        CHECK(declaration && xr_native_declaration_validate(declaration));
        CHECK(!strcmp(declaration->name,native_names[id-1]));
        CHECK(declaration->member_count==native_members[id-1]);
        CHECK(declaration->parameter_count==native_parameters[id-1]);
        if (declaration->id==5) {
            CHECK(!strcmp(declaration->members[0].name,"cancel"));
            CHECK(!strcmp(declaration->members[1].name,"poll"));
            CHECK(!strcmp(declaration->members[2].name,"awaitResult"));
            CHECK(!strcmp(declaration->parameter_names[0],"T") && !strcmp(declaration->parameter_names[1],"E"));
        }
        if (declaration->id==9)
            CHECK(!strcmp(declaration->parameter_names[0],"T") && !strcmp(declaration->parameter_names[1],"E"));
        XrNativeTypeDeclaration mutation=*declaration;mutation.name="wrong";
        CHECK(!xr_native_declaration_validate(&mutation));
        mutation=*declaration;
        ((unsigned char *)&mutation.source_fingerprint)[0]^=1;
        CHECK(!xr_native_declaration_validate(&mutation));
        mutation=*declaration; mutation.parameter_count=native_parameters[id-1]+1;
        CHECK(!xr_native_declaration_validate(&mutation));
        if (declaration->parameter_count) {
            const char *wrong[]={"U","V"}; mutation=*declaration; mutation.parameter_names=wrong;
            CHECK(!xr_native_declaration_validate(&mutation));
            if (declaration->id==10) {
                const char *second[]={"K","K"}; mutation=*declaration; mutation.parameter_names=second;
                CHECK(!xr_native_declaration_validate(&mutation));
                second[1]=NULL;
                CHECK(!xr_native_declaration_validate(&mutation));
                second[0]="V";second[1]="K";
                CHECK(!xr_native_declaration_validate(&mutation));
            }
            mutation=*declaration; mutation.parameter_names=NULL;
            CHECK(!xr_native_declaration_validate(&mutation));
        }
        printf("Native %s: canonical identity, members and parameter forgery checks PASS\n",declaration->name);
    }
}
static void lookup_boundaries(void) {
    const XrNativeTypeDeclaration *array=xr_native_declaration_by_id(1);
    const XrNativeMemberDeclaration *member=xr_native_declaration_member(array,"get");
    CHECK(member && !strcmp(member->name,"get") && member->operation==XR_NATIVE_OPERATION_ARRAY_GET);
    CHECK(!xr_native_declaration_by_name("?") && !xr_native_declaration_member(array,"?"));
    CHECK(!xr_native_declaration_by_name(NULL) && !xr_native_declaration_member(NULL,"get"));
    CHECK(!xr_native_declaration_member(array,NULL) && !xr_native_declaration_validate(NULL));
    XrNativeTypeDeclaration mutation=*array;mutation.members=NULL;
    CHECK(!xr_native_declaration_member(&mutation,"get") && !xr_native_declaration_validate(&mutation));
    mutation=*array;mutation.member_count=65;
    CHECK(!xr_native_declaration_member(&mutation,"get") && !xr_native_declaration_validate(&mutation));
}
static void complete_lookup_boundaries(void) {
    for (uint32_t selected=0;selected<sizeof(native_names)/sizeof(*native_names);++selected) {
        const XrNativeTypeDeclaration *declaration=xr_native_declaration_by_id(native_ids[selected]);
        CHECK(xr_native_declaration_by_name(native_names[selected])==declaration);
        for (uint32_t i=0;i<declaration->member_count;++i)
            CHECK(xr_native_declaration_member(declaration,declaration->members[i].name)==&declaration->members[i]);
    }
}
int main(void) { complete_declarations();lookup_boundaries();complete_lookup_boundaries();return 0; }
