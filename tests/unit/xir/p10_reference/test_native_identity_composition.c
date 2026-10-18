/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_native_identity_composition.c - Unique governed declarations across native families
 *
 * KEY CONCEPT:
 *   A competing ordinal cannot substitute another declaration's full contract.
 */
#include "shared/xnative_declaration.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if (!(c)) {fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}} while (0)
static void identity_cases(void) {
    static const struct {uint32_t id;const char *name;} expected[]={
        {1,"Array"},{2,"string"},{3,"Atomic"},{4,"Ordering"},{5,"Task"},{6,"i64"},
        {7,"f64"},{8,"bool"},{9,"rune"},{10,"Map"},{11,"Set"},{12,"Iterator"}};
    for (size_t i=0;i<sizeof(expected)/sizeof(*expected);++i) {
        const XrNativeTypeDeclaration *type=xr_native_declaration_by_id(expected[i].id);
        CHECK(type && type->id==expected[i].id && !strcmp(type->name,expected[i].name));
        CHECK(xr_native_declaration_by_name(expected[i].name)==type);
        CHECK(xr_native_declaration_validate(type));
    }
    CHECK(!xr_native_declaration_by_id(0) && !xr_native_declaration_by_id(13));
    XrNativeTypeDeclaration bad=*xr_native_declaration_by_name("f64");
    bad.id=6;CHECK(!xr_native_declaration_validate(&bad));
    bad=*xr_native_declaration_by_name("Iterator");bad.id=8;
    CHECK(!xr_native_declaration_validate(&bad));
}
static void member_cases(void) {
    const XrNativeTypeDeclaration *type=xr_native_declaration_by_name("f64");
    const XrNativeMemberDeclaration *parse=xr_native_declaration_member(type,"tryParse");
    CHECK(parse && parse->operation==XR_NATIVE_OPERATION_F64_TRY_PARSE && parse->is_static && parse->lowered);
    CHECK(parse->receiver==XR_NATIVE_RECEIVER_READ && parse->parameter_count==1 &&
        parse->parameters[0].type==XR_NATIVE_TERM_STRING && parse->result==XR_NATIVE_TERM_NULLABLE_F64);
    const XrNativeMemberDeclaration *abs=xr_native_declaration_member(type,"abs");
    CHECK(abs && abs->operation==XR_NATIVE_OPERATION_FLOAT_ABS && !abs->is_static && !abs->lowered);
    const XrNativeMemberDeclaration *copy=xr_native_declaration_member(xr_native_declaration_by_name("string"),"copyBytes");
    CHECK(copy && copy->operation==XR_NATIVE_OPERATION_STRING_COPY_BYTES && copy->result==XR_NATIVE_TERM_ARRAY_U8 &&
        copy->receiver==XR_NATIVE_RECEIVER_READ && !copy->is_static && !copy->parameter_count);
    const XrNativeTypeDeclaration *iterator=xr_native_declaration_by_name("Iterator");
    CHECK(iterator->kind==XR_NATIVE_DECLARATION_IDENTITY && iterator->parameter_count==1);
    const XrNativeMemberDeclaration *has=xr_native_declaration_member(iterator,"hasNext");
    CHECK(has && has->operation==XR_NATIVE_OPERATION_ITERATOR_HAS_NEXT && has->result==XR_NATIVE_TERM_BOOL);
    XrNativeMemberDeclaration members[64];CHECK(type->member_count<=64);
    memcpy(members,type->members,type->member_count*sizeof(*members));
    XrNativeTypeDeclaration bad=*type;bad.members=members;
    size_t index=(size_t)(parse-type->members);members[index].is_static=false;
    CHECK(!xr_native_declaration_validate(&bad));members[index]=*parse;members[index].result=XR_NATIVE_TERM_F64;
    CHECK(!xr_native_declaration_validate(&bad));
}
static bool limited_work(void *context,uint64_t units) {
    uint64_t *remaining=context;if (units>*remaining) return false;*remaining-=units;return true;
}
static void work_cases(void) {
    uint64_t remaining=0;const XrNativeDeclarationWork work={&remaining,limited_work};
    const XrNativeTypeDeclaration *sentinel=xr_native_declaration_by_name("f64"),*result=sentinel;
    CHECK(xr_native_declaration_find(&work,"Iterator",&result)==XR_NATIVE_DECLARATION_WORK_LIMIT && result==sentinel);
    remaining=UINT64_C(1000000);result=NULL;
    CHECK(xr_native_declaration_find(&work,"Iterator",&result)==XR_NATIVE_DECLARATION_OK && result->id==12);
    CHECK(remaining<UINT64_C(1000000));
}
int main(void) {identity_cases();member_cases();work_cases();puts("native identities and complete member contracts passed");return 0;}
